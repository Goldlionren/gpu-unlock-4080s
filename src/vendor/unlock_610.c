/* Modified 2026-10-23: AD102-shaped write/marker/cleanup candidate; see PORT-NOTES.txt. */
// SPDX-License-Identifier: GPL-2.0-only
/* Modified 2026-10-18 for RTX 3080 UEFI: target selection, checked IO, execution/quiescence hooks and diagnostics. See THIRD_PARTY_NOTICES.md.
 * Pre-driver V67 unlock, run before the stock driver binds: FWSEC-FRTS from
 * the board ROM, Booter with the V67 signature, the SS0/SS1 selector writes, then
 * FLR so the stock driver starts from a clean, halted GSP.
 */
#include <linux/aperture.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include "diagnostic.h"
#include "gfw_eval.h"
#include "prep_610.h"
#include "unlock_610.h"
#include "vbios_610.h"
#include "../prom_profile.h"

#ifndef GU_TARGET_PCI_ID
#define GU_TARGET_PCI_ID 0x220410deU
#endif
#ifdef GU_UEFI_PORT
#include "../port.h"
#endif

#define SEC2_BASE 0x00840000u
#define SEC2_FBIF 0x00840600u
#define SEC2_FALCON2 0x00841000u
/* SS0 holds eight 4-bit fields; SS1 stores only its low 4 bits (0x2 when
 * cold). Both get the same per-field value 8. */
#define GU_SS0_UNLOCK 0x88888888u
#define GU_SS1_UNLOCK 0x8u
#define BOOTER_MAGIC 0x314c5442u
#define BOOTER_FILE "gpu-unlock/610.43.03/booter_load_ad102.bin"
#define V67_FILE "gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin"
#define GSP_FILE "gpu-unlock/610.43.03/gsp_ga10x.bin"
#define BOOT_FILE "gpu-unlock/610.43.03/gsp_boot_ad102.bin"
#define BOOT_DESC_FILE "gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin"

static struct gpu_unlock_610_outcome last_outcome;
const struct gpu_unlock_610_outcome *gpu_unlock_610_get_outcome(void)
{
	return &last_outcome;
}

/* User-requested CMP sequence, experimental on AD103. This table changes
 * PLM permissions only; no CFG1/LMR/memory-geometry stores are performed. */
static const struct { u32 addr, value; const char *name; } plm_targets[] = {
	{ 0x001fa7ccu, 0xfffff0ffu, "WPR_CFG" },
	{ 0x009a0148u, 0xffffffffu, "FBPA" },
	{ 0x001fa7c4u, 0xffffffffu, "WPR" },
	{ GU_FEAT_PLM_OFFSET, 0xffffffffu, "FEAT" },
};

struct unlock_transaction_page {
	void *cpu;
	dma_addr_t dma;
};

struct unlock_transaction_work {
	struct pci_dev *pdev;
	void __iomem *bar;
	resource_size_t bar_len;
	u16 original_cmd;
	bool cmd_valid;
	bool enabled;
	bool region;
	bool mapped;
	bool master;
	const struct firmware *booter_fw;
	const struct firmware *v67_fw;
	const struct firmware *gsp_fw;	/* borrowed from gpu_unlock_610_shared */
	const struct firmware *boot_fw;
	struct gpu_unlock_span image_span, sig_span;
	struct gpu_unlock_layout_in layout_in;
	struct gpu_unlock_layout_out layout;
	struct gpu_unlock_boot_desc boot_desc;
	struct gpu_unlock_radix_plan radix;
	struct unlock_transaction_page *pages;
	u32 page_count, pages_allocated;
	void *boot_cpu;
	dma_addr_t boot_dma;
	void *booter_cpu;
	dma_addr_t booter_dma;
	u32 booter_image_size;
	struct gpu_unlock_booter_hdr booter_hdr;
	void *v67_cpu;
	dma_addr_t v67_dma;
	void *stock_cpu;
	dma_addr_t stock_dma;
	u64 stock_size;
	void *meta_cpu;
	dma_addr_t meta_dma;
	u64 *streaming_ptr;
	u64 streaming_before;
	u64 coherent_before;
	bool streaming_set;
	bool coherent_set;
	unsigned int signature_calls;
	bool published;
	bool masks_saved;
	bool firmware_started;
	bool sec2_launched;
	bool sec2_halt_verified;
	bool quiesced;
	bool retained;
	/* Eligibility comes from a readable, halted SEC2 after a closed-target
	 * observation. It never means that DMA buffers may be freed. */
	bool sec2_retryable;
	u32 plm_entry;
	u32 plm_target, plm_expected;
	u32 bars[6];
	void *fwsec_cpu;
	dma_addr_t fwsec_dma;
	u32 fwsec_size;
	u32 last_offset, last_value;
	bool last_valid;
	u32 last_pci_offset, last_pci_value;
	int last_pci_error;
	bool last_pci_cap;
};

static int unlock_transaction_result(struct unlock_transaction_work *work, int error,
			  const char *function, unsigned int line)
{
	if (error)
	{
		dev_err(&work->pdev->dev,
			"failure unlock_610.c:%u %s error=%d last_mmio=%#x value=%#x valid=%u mapped=%u master=%u firmware_started=%u quiesced=%u pages=%u/%u\n",
			line, function, error, work->last_offset, work->last_value,
			work->last_valid, work->mapped, work->master, work->firmware_started,
			work->quiesced, work->pages_allocated, work->page_count);
		dev_err(&work->pdev->dev, "last PCI sample: capability=%u offset=%#x value=%#x result=%d\n",
			work->last_pci_cap, work->last_pci_offset, work->last_pci_value, work->last_pci_error);
	}
	return error;
}
#define GU_RESULT(result) unlock_transaction_result(work, (result), __func__, __LINE__)

static int diag_pci_word(struct unlock_transaction_work *work, bool cap, int offset, u16 *value)
{
	int ret;
	*value = 0xffff;
	ret = cap ? pcie_capability_read_word(work->pdev, offset, value) :
		pci_read_config_word(work->pdev, offset, value);
	work->last_pci_cap = cap;
	work->last_pci_offset = offset;
	work->last_pci_value = *value;
	work->last_pci_error = ret;
	return ret;
}

static int diag_pci_dword(struct unlock_transaction_work *work, bool cap, int offset, u32 *value)
{
	int ret;
	*value = ~0u;
	ret = cap ? pcie_capability_read_dword(work->pdev, offset, value) :
		pci_read_config_dword(work->pdev, offset, value);
	work->last_pci_cap = cap;
	work->last_pci_offset = offset;
	work->last_pci_value = *value;
	work->last_pci_error = ret;
	return ret;
}

static void *diag_dma_alloc(struct unlock_transaction_work *work, const char *name,
			   size_t size, dma_addr_t *dma, unsigned int flags)
{
	void *memory = dma_alloc_coherent(&work->pdev->dev, size, dma, flags);
	if (!memory)
		dev_err(&work->pdev->dev, "DMA allocation failed: asset=%s bytes=%zu pages_allocated=%u requested=%u\n",
			name, size, work->pages_allocated, work->page_count);
	return memory;
}

/* CONFIG_FW_CACHE attaches name records to devres even though this is not
 * devm_request_firmware(). release_firmware() does not remove them, and
 * really_probe() refuses a device with any existing devres. Scope each
 * synchronous request so only its cache records are released; the returned
 * firmware remains owned by the caller.
 */
static int request_scoped(struct device *dev, const struct firmware **fw, const char *name)
{
	void *group;
	int ret, released;

	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	dev_info(dev, "asset request: %s\n", name);
	ret = request_firmware(fw, name, dev);
	released = devres_release_group(dev, group);
	dev_info(dev, "asset result: %s error=%d bytes=%zu\n",
		name, ret, ret ? (size_t)0 : (*fw)->size);
	dev_info(dev, "asset cache cleanup: %s devres_released=%d\n", name, released);
	return ret;
}

static int request_asset(struct unlock_transaction_work *work, const struct firmware **fw,
			 const char *name)
{
	return GU_RESULT(request_scoped(&work->pdev->dev, fw, name));
}

static int check_devres_empty(struct unlock_transaction_work *work, const char *phase)
{
	struct device *dev = &work->pdev->dev;
	unsigned long flags;
	bool empty;

	/* The caller also holds the device lock across the whole transaction. */
	spin_lock_irqsave(&dev->devres_lock, flags);
	empty = list_empty(&dev->devres_head);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	dev_info(dev, "driver probe resources: phase=%s empty=%u\n", phase, empty);
	return GU_RESULT(empty ? 0 : -EBUSY);
}

static int read_bar(struct unlock_transaction_work *work, u32 offset, u32 *value)
{
#ifdef GU_UEFI_PORT
	if (Gu.fault)
		return GU_RESULT(-EIO);
#endif
	work->last_offset = offset;
	work->last_valid = false;
	if (!work->mapped || offset > work->bar_len || work->bar_len - offset < 4)
		return GU_RESULT(-EINVAL);
	*value = readl((u8 __iomem *)work->bar + offset);
#ifdef GU_UEFI_PORT
	if (Gu.fault)
		return GU_RESULT(-EIO);
#endif
	work->last_value = *value;
	work->last_valid = true;
	return 0;
}

static void write_bar(struct unlock_transaction_work *work, u32 offset, u32 value)
{
	writel(value, (u8 __iomem *)work->bar + offset);
}

static int poll_us(struct unlock_transaction_work *work, u32 offset, u32 mask, u32 expect,
		   unsigned int budget_us, bool timeout_is_failure)
{
	ktime_t deadline = ktime_add_us(ktime_get(), budget_us);
	u32 value = 0;

	for (;;) {
		if (read_bar(work, offset, &value) || value == ~0u ||
		    (value & 0xffff0000u) == 0xbadf0000u) {
			dev_err(&work->pdev->dev, "poll inaccessible: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
				offset, value, mask, expect, budget_us);
			return GU_RESULT(-EIO);
		}
		if ((value & mask) == expect)
			return 0;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			/* Console loglevel hides info. A tolerated timeout must not
			 * look like the failure that stopped the boot. */
			if (!timeout_is_failure) {
				dev_info(&work->pdev->dev, "poll timeout tolerated: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
					offset, value, mask, expect, budget_us);
				return -ETIMEDOUT;
			}
			dev_err(&work->pdev->dev, "poll timeout: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
				offset, value, mask, expect, budget_us);
			return GU_RESULT(-ETIMEDOUT);
		}
		usleep_range(50, 100);
	}
}

struct falcon_eng {
	u32 base;
	u32 fbif;
	u32 falcon2;
};

static bool inaccessible(u32 value)
{
	return value == ~0u || (value & 0xffff0000u) == 0xbadf0000u;
}

static int falcon_reset(struct unlock_transaction_work *work, const struct falcon_eng *eng)
{
	u32 engine, hwcfg, bcr, chip;
	unsigned int i;
	int ret;

	/* 610 kflcnPreResetWait_GA102 permits this timeout (HW bug 3419321). */
	dev_info(&work->pdev->dev, "Falcon reset begin: base=%#x\n", eng->base);
	ret = poll_us(work, eng->base + 0xf4, 1u << 31, 1u << 31, 1000, false);
	if (ret && ret != -ETIMEDOUT)
		return GU_RESULT(ret);
	if (ret)
		dev_info(&work->pdev->dev, "Falcon pre-reset timeout tolerated per HW bug 3419321\n");
	if (read_bar(work, eng->base + 0x3c0, &engine) || inaccessible(engine))
		return GU_RESULT(-ENODEV);
	write_bar(work, eng->base + 0x3c0, engine | 1);
	for (i = 0; i < 10; i++)
		readl((u8 __iomem *)work->bar + eng->base + 0x3c0);
	write_bar(work, eng->base + 0x3c0, engine & ~1u);
	for (i = 0; i < 10; i++)
		readl((u8 __iomem *)work->bar + eng->base + 0x3c0);
	ret = poll_us(work, eng->base + 0xf4, 1u << 12, 0, 1000000, true);
	if (ret)
		return GU_RESULT(ret);
	if (read_bar(work, eng->base + 0xf4, &hwcfg) || inaccessible(hwcfg))
		return GU_RESULT(-EIO);
	if (hwcfg & (1u << 10)) {
		if (read_bar(work, eng->falcon2 + 0x668, &bcr) || inaccessible(bcr))
			return GU_RESULT(-ENODEV);
		if (bcr & (1u << 4)) {
			write_bar(work, eng->falcon2 + 0x668, 0);
			ret = poll_us(work, eng->falcon2 + 0x668, 0x11, 1, 1000000, true);
			if (ret)
				return GU_RESULT(ret);
		}
	}
	if (read_bar(work, 0, &chip) || inaccessible(chip))
		return GU_RESULT(-ENODEV);
	write_bar(work, eng->base + 0x84, chip);
	dev_info(&work->pdev->dev, "Falcon reset done: base=%#x hwcfg=%#x chip=%#x\n", eng->base, hwcfg, chip);
	return 0;
}

/* DMATRFMOFFS is 24 bits, FBOFFS is 32 bits, and BASE is 49 bits.
 * Reject truncation/wrap before submitting any part of a transfer. */
static bool falcon_dma_valid(u32 dest, u32 mem_off, dma_addr_t src, u32 size)
{
	return size && !((size | dest | mem_off | src) & 255) &&
		(u64)dest + size <= (1ull << 24) &&
		(u64)mem_off + size <= (1ull << 32) &&
		src < (1ull << 49) &&
		(u64)mem_off + size <= (1ull << 49) - src;
}

static int falcon_dma(struct unlock_transaction_work *work, u32 base, u32 dest, u32 mem_off,
		      dma_addr_t src, u32 size, u32 cmd)
{
	u32 done = 0;
	int ret;

	dev_info(&work->pdev->dev, "Falcon DMA: base=%#x dest=%#x offset=%#x src=%#llx size=%#x cmd=%#x\n",
		base, dest, mem_off, (unsigned long long)src, size, cmd);
	if (!falcon_dma_valid(dest, mem_off, src, size))
		return GU_RESULT(-EINVAL);
	ret = poll_us(work, base + 0x118, 1, 0, 1000000, true);
	if (ret)
		return GU_RESULT(ret);
	write_bar(work, base + 0x110, lower_32_bits(src >> 8));
	write_bar(work, base + 0x128, upper_32_bits(src >> 8) & 0x1ff);
	while (done < size) {
		ret = poll_us(work, base + 0x118, 1, 0, 1000000, true);
		if (ret) {
			dev_err(&work->pdev->dev, "Falcon DMA progress: done=%u total=%u\n", done, size);
			return GU_RESULT(ret);
		}
		write_bar(work, base + 0x114, dest);
		write_bar(work, base + 0x11c, mem_off);
		write_bar(work, base + 0x118, cmd);
		done += 256;
		dest += 256;
		mem_off += 256;
	}
	return GU_RESULT(poll_us(work, base + 0x118, 2, 2, 1000000, true));
}

#ifdef GU_UEFI_PORT
#include "sec2_observe.inc"
/* GA102 registers published in NVIDIA 610.43.03. This reads status, not
 * authentication internals. Protected register values are logged verbatim;
 * neither a readable snapshot nor HALTED proves successful FWSEC execution. */
static int fwsec_snapshot(struct unlock_transaction_work *work, const char *phase)
{
	static const u32 offsets[] = {
		0x110100, 0x110008, 0x110040, 0x110044, 0x110094, 0x1100f4,
		0x110104, 0x11010c, 0x110118, 0x110600, 0x110624, 0x111668,
		0x111180, 0x111198, 0x11119c, 0x111210,
		0x110108, 0x110110, 0x110114, 0x11011c, 0x110128,
	};
	u32 values[ARRAY_SIZE(offsets)], count = 0, i;
	int ret = 0;

	/* Read first, then log so USB/console latency does not split the sample. */
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		ret = read_bar(work, offsets[i], &values[i]);
		if (ret)
			break;
		count++;
	}
	dev_info(&work->pdev->dev, "FWSEC snapshot: phase=%s samples=%u/%u error=%d\n",
		 phase, count, (unsigned int)ARRAY_SIZE(offsets), ret);
	for (i = 0; i < count; i++)
		dev_info(&work->pdev->dev, "FWSEC register: reg=%#x value=%#x sentinel_like=%u\n",
			 offsets[i], values[i], inaccessible(values[i]));
	return ret ? ret : (Gu.fault ? -EIO : 0);
}

static int fwsec_wait_halt(struct unlock_transaction_work *work)
{
	ktime_t deadline = ktime_add_us(ktime_get(), 5000000);
	u32 value = 0, first = 0, samples = 0, requested_wait_us = 0;
	bool observed_nonhalt = false;
	int ret;

	for (;;) {
		ret = read_bar(work, 0x110100, &value);
		if (ret)
			break;
		if (!samples)
			first = value;
		samples++;
		if (inaccessible(value)) { ret = -EIO; break; }
		if (value & 0x10)
			break;
		observed_nonhalt = true;
		if (ktime_compare(ktime_get(), deadline) >= 0) { ret = -ETIMEDOUT; break; }
		usleep_range(50, 100);
		requested_wait_us += 50;
	}
	/* Gu.ticks counts requested Stall intervals, NOT elapsed wall-clock time. */
	dev_info(&work->pdev->dev, "FWSEC halt observation: first=%#x last=%#x samples=%u observed_nonhalt=%u requested_wait_us=%u error=%d\n",
		first, value, samples, observed_nonhalt, requested_wait_us, ret);
	return GU_RESULT(ret ? ret : (Gu.fault ? -EIO : 0));
}
#endif

static int falcon_hs(struct unlock_transaction_work *work, const struct falcon_eng *eng,
		     u32 code_off, u32 code_size, u32 code_va, u32 code_pa,
		     u32 data_off, u32 data_size, u32 data_pa,
		     u32 sig_dmem, u32 engine_mask, u32 ucode_id,
		     dma_addr_t image, u32 mailbox0, u32 mailbox1,
		     bool wait_plm, bool reset_prepared)
{
	u32 trans, cpuctl;
	dma_addr_t code_src;
	int ret;

	if (image > ~0ull - code_off || image + code_off < code_va ||
	    image > ~0ull - data_off)
		return GU_RESULT(-ERANGE);
	code_src = image + code_off - code_va;
	if (!falcon_dma_valid(code_pa, code_va, code_src, code_size) ||
	    (data_size && !falcon_dma_valid(data_pa, 0, image + data_off, data_size)))
		return GU_RESULT(-EINVAL);
	/* The caller can fully prepare SEC2, including reset and Falcon access.
	 * Do not reset again after its ordered preparation/readback experiment. */
	if (!reset_prepared) {
		ret = falcon_reset(work, eng);
		if (ret)
			return GU_RESULT(ret);
	} else if (!wait_plm || eng->base != SEC2_BASE) {
		return GU_RESULT(-EINVAL);
	}
	if (read_bar(work, eng->fbif + 0x24, &trans) || inaccessible(trans))
		return GU_RESULT(-EIO);
	trans |= 1u << 7;
	write_bar(work, eng->fbif + 0x24, trans);
	write_bar(work, eng->base + 0x10c, 0);
	if (read_bar(work, eng->fbif, &trans) || inaccessible(trans))
		return GU_RESULT(-ENODEV);
	trans = (trans & ~7u) | 5u;
	write_bar(work, eng->fbif, trans);
	dma_wmb();
	ret = falcon_dma(work, eng->base, code_pa, code_va, code_src,
			 code_size, gpu_unlock_dma_cmd(true));
	if (ret)
		return GU_RESULT(ret);
	if (data_size) {
		ret = falcon_dma(work, eng->base, data_pa, 0, image + data_off, data_size,
				 gpu_unlock_dma_cmd(false));
		if (ret)
			return GU_RESULT(ret);
	}
	/* Follow the reference DMA -> PKC -> START path. DMEM PIO visibility
	 * before START is not established on this board; do not touch its PIO
	 * window or treat it as proof of staging. Scratch/WPR checks follow. */
	write_bar(work, eng->falcon2 + 0x210, sig_dmem);
	write_bar(work, eng->falcon2 + 0x19c, engine_mask);
	write_bar(work, eng->falcon2 + 0x198, ucode_id & 0xff);
	write_bar(work, eng->falcon2 + 0x180, 1);
	write_bar(work, eng->base + 0x104, code_va);
	write_bar(work, eng->base + 0x40, mailbox0);
	write_bar(work, eng->base + 0x44, mailbox1);
#ifdef GU_UEFI_PORT
	if (!wait_plm && eng->base == 0x110000) {
		ret = fwsec_snapshot(work, "pre-start");
		if (ret)
			return GU_RESULT(ret);
	}
	if (wait_plm && eng->base == SEC2_BASE) {
		ret = sec2_snapshot(work,"pre-start");
		if (ret) return GU_RESULT(ret);
	}
#endif
	if (read_bar(work, eng->base + 0x100, &cpuctl) || inaccessible(cpuctl))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev, "Falcon start: base=%#x cpuctl=%#x code_va=%#x sig_dmem=%#x engine_mask=%#x ucode=%u mailbox=%#x:%#x\n",
		eng->base, cpuctl, code_va, sig_dmem, engine_mask, ucode_id, mailbox1, mailbox0);
	if (wait_plm && eng->base == SEC2_BASE) {
		work->sec2_launched = true;
		work->sec2_halt_verified = false;
	}
	if (cpuctl & (1u << 6))
		write_bar(work, eng->base + 0x130, 1u << 1);
	else
		write_bar(work, eng->base + 0x100, 1u << 1);
	if (wait_plm) {
#ifdef GU_UEFI_PORT
		return GU_RESULT(sec2_observe_plm(work));
#else
		ktime_t deadline = ktime_add_us(ktime_get(), 5000000);

		for (;;) {
			u32 plm, engine, identity;

			if (diag_pci_dword(work, false, PCI_VENDOR_ID, &identity) ||
			    identity != GU_TARGET_PCI_ID ||
			    read_bar(work, eng->base + 0x100, &engine) || engine == ~0u ||
			    (engine & 0xffff0000u) == 0xbadf0000u ||
			    read_bar(work, work->plm_target, &plm))
				return GU_RESULT(-ENODEV);
			/* Cold entry was closed: this transition proves payload execution. */
			if (plm == work->plm_expected)
				return 0;
			if ((engine & (1u << 4)) || ktime_compare(ktime_get(), deadline) >= 0)
				dev_err(&work->pdev->dev, "early PLM failed: pci=%#x cpuctl=%#x plm=%#x entry=%#x budget_us=5000000\n",
					identity, engine, plm, work->plm_entry);
			if (engine & (1u << 4))
				return GU_RESULT(-EIO);
			if (ktime_compare(ktime_get(), deadline) >= 0)
				return GU_RESULT(-ETIMEDOUT);
			usleep_range(50, 100);
		}
#endif
	}
#ifdef GU_UEFI_PORT
	if (eng->base == 0x110000) {
		int snapshot_ret;
		ret = fwsec_wait_halt(work);
		snapshot_ret = fwsec_snapshot(work, ret ? "halt-error" : "post-halt");
		if (!ret)
			ret = snapshot_ret;
	} else
#endif
		ret = poll_us(work, eng->base + 0x100, 1u << 4, 1u << 4, 5000000, true);
	if (ret)
		return GU_RESULT(ret);
	return 0;
}

static int load_booter(struct unlock_transaction_work *work)
{
	const u8 *data;
	u32 magic, version, image_size, sig_bytes;
	struct gpu_unlock_booter_hdr hdr;
	int ret;

	ret = request_asset(work, &work->booter_fw, BOOTER_FILE);
	if (ret)
		return GU_RESULT(ret);
	data = work->booter_fw->data;
	if (work->booter_fw->size < 72)
		return GU_RESULT(-EINVAL);
	memcpy(&magic, data, sizeof(magic));
	memcpy(&version, data + 4, sizeof(version));
	if (magic != BOOTER_MAGIC || version != 1)
		return GU_RESULT(-EINVAL);
	memcpy(&hdr, data + 8, sizeof(hdr));
	memcpy(&image_size, data + 8 + sizeof(hdr), sizeof(image_size));
	memcpy(&sig_bytes, data + 12 + sizeof(hdr), sizeof(sig_bytes));
	if (!hdr.ucode_id || hdr.num_apps != 1 || sig_bytes != hdr.sig_bytes ||
	    image_size != GU_BOOTER_IMAGE_SIZE ||
	    work->booter_fw->size != 72u + image_size + sig_bytes)
		return GU_RESULT(-EINVAL);
	/* Only this pinned production BOOT_FROM_HS geometry is implemented. */
	if (hdr.ucode_id != 3 || hdr.engine_id != 1 || hdr.num_sigs != 2 ||
	    sig_bytes != GU_BOOTER_SIG_TOTAL || hdr.os_code_off != 0 ||
	    hdr.os_code_size != 256 || hdr.os_data_off != 32256 ||
	    hdr.os_data_size != 24576 || hdr.app_code_off != 256 ||
	    hdr.app_code_size != 32000 || hdr.app_data_off != 256 ||
	    hdr.app_data_size != 0 || hdr.patch_loc != 32272)
		return GU_RESULT(-EINVAL);
	work->booter_hdr = hdr;
	work->booter_image_size = image_size;
	/* Allocation follows the ownership and DMA-mask snapshot, not asset lookup. */
	return 0;
}

static int patch_booter(struct unlock_transaction_work *work)
{
	const u8 *data = work->booter_fw->data;
	u32 fuse_raw, fuse_ver;
	u32 index = work->booter_hdr.ucode_id - 1;

	if (read_bar(work, GU_FUSE_SEC2_UCODE1 + index * 4, &fuse_raw))
		return GU_RESULT(-EIO);
	fuse_ver = gpu_unlock_fuse_version(fuse_raw);
	dev_info(&work->pdev->dev, "Booter signature: fuse_raw=%#x version=%u signatures=%u size=%u\n",
		fuse_raw, fuse_ver, work->booter_hdr.num_sigs, work->booter_hdr.sig_bytes);
	return GU_RESULT(gpu_unlock_booter_patch(work->booter_cpu, work->booter_image_size,
				       &work->booter_hdr,
				       data + 72 + work->booter_image_size,
				       work->booter_hdr.sig_bytes, fuse_ver));
}

static void free_coherent(struct unlock_transaction_work *work)
{
	u32 i;

	for (i = 0; i < work->pages_allocated; i++)
		dma_free_coherent(&work->pdev->dev, GU_RADIX_PAGE,
				  work->pages[i].cpu, work->pages[i].dma);
	kvfree(work->pages);
	work->pages = NULL;
	work->pages_allocated = 0;
	if (work->boot_cpu)
		dma_free_coherent(&work->pdev->dev, work->boot_desc.image_size,
				  work->boot_cpu, work->boot_dma);
	work->boot_cpu = NULL;
	if (work->fwsec_cpu) {
		dma_free_coherent(&work->pdev->dev, work->fwsec_size,
				  work->fwsec_cpu, work->fwsec_dma);
		work->fwsec_cpu = NULL;
	}
	if (work->booter_cpu)
		dma_free_coherent(&work->pdev->dev, GU_BOOTER_IMAGE_SIZE,
				  work->booter_cpu, work->booter_dma);
	if (work->v67_cpu)
		dma_free_coherent(&work->pdev->dev, GU_V67_SIZE, work->v67_cpu,
				  work->v67_dma);
	if (work->stock_cpu)
		dma_free_coherent(&work->pdev->dev, work->stock_size,
				  work->stock_cpu, work->stock_dma);
	if (work->meta_cpu)
		dma_free_coherent(&work->pdev->dev, GU_WPR_META_SIZE,
				  work->meta_cpu, work->meta_dma);
	work->booter_cpu = NULL;
	work->v67_cpu = NULL;
	work->stock_cpu = NULL;
	work->meta_cpu = NULL;
}

static int unlock_transaction_unwind(struct unlock_transaction_work *work)
{
	u16 now, want;
	int ret = 0;

	/* A timeout never authorizes freeing firmware-visible memory. */
	if (work->firmware_started && !work->quiesced) {
		work->retained = true;
		return GU_RESULT(-EBUSY);
	}
	if (work->master) {
		pci_clear_master(work->pdev);
		work->master = false;
	}
	if (work->mapped) {
		pci_iounmap(work->pdev, work->bar);
		work->bar = NULL;
		work->mapped = false;
	}
	if (work->region) {
		pci_release_region(work->pdev, 0);
		work->region = false;
	}
	if (work->enabled) {
		pci_disable_device(work->pdev);
		work->enabled = false;
	}
	free_coherent(work);
	if (work->booter_fw) {
		release_firmware(work->booter_fw);
		work->booter_fw = NULL;
	}
	if (work->v67_fw) {
		release_firmware(work->v67_fw);
		work->v67_fw = NULL;
	}
	if (work->boot_fw) {
		release_firmware(work->boot_fw);
		work->boot_fw = NULL;
	}
	/* Match the driver core's entry condition, not just BAR/mask cleanup.
	 * Never release unrelated resources to make this check pass.
	 */
	if (check_devres_empty(work, "handoff"))
		ret = -EBUSY;
	if (work->streaming_set && work->streaming_ptr) {
		if (work->pdev->dev.dma_mask != work->streaming_ptr)
			return GU_RESULT(-EIO);
		if (dma_set_mask(&work->pdev->dev, work->streaming_before) ||
		    *work->streaming_ptr != work->streaming_before)
			ret = -EIO;
		dev_info(&work->pdev->dev, "restore streaming mask: actual=%#llx expected=%#llx\n",
			(unsigned long long)*work->streaming_ptr, (unsigned long long)work->streaming_before);
	}
	if (work->coherent_set) {
		if (dma_set_coherent_mask(&work->pdev->dev, work->coherent_before) ||
		    work->pdev->dev.coherent_dma_mask != work->coherent_before)
			ret = -EIO;
		dev_info(&work->pdev->dev, "restore coherent mask: actual=%#llx expected=%#llx\n",
			(unsigned long long)work->pdev->dev.coherent_dma_mask,
			(unsigned long long)work->coherent_before);
	}
	if (!work->cmd_valid)
		return GU_RESULT(ret);
	if (diag_pci_word(work, false, PCI_COMMAND, &now) || now == 0xffff)
		return GU_RESULT(-ENODEV);
	want = gpu_unlock_command_restore(work->original_cmd, now);
	if (want != now && pci_write_config_word(work->pdev, PCI_COMMAND, want))
		return GU_RESULT(-EIO);
	if (diag_pci_word(work, false, PCI_COMMAND, &now))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev, "restore PCI command: actual=%#x original=%#x mask=%#x\n",
		now, work->original_cmd, GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER);
	if ((now & (GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER)) !=
	    (work->original_cmd & (GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER)))
		return GU_RESULT(-EIO);
	return GU_RESULT(ret);
}

static int unlock_transaction_assets(struct unlock_transaction_work *work)
{
	const struct firmware *desc;
	int ret;

	ret = check_devres_empty(work, "entry");
	if (ret)
		return ret;
	ret = request_asset(work, &work->v67_fw, V67_FILE);
	if (ret)
		return GU_RESULT(ret);
	if (work->v67_fw->size != GU_V67_SIZE)
		return GU_RESULT(-EBADMSG);
	ret = gpu_unlock_elf_section(work->gsp_fw->data, work->gsp_fw->size,
				    ".fwimage", &work->image_span);
	if (ret)
		return GU_RESULT(ret);
	ret = gpu_unlock_elf_section(work->gsp_fw->data, work->gsp_fw->size,
				    ".fwsignature_ad10x", &work->sig_span);
	if (ret)
		return GU_RESULT(ret);
	if (!work->image_span.size || work->image_span.size > 128ull << 20 ||
	    work->sig_span.size != 4096)
		return GU_RESULT(-EINVAL);
	ret = gpu_unlock_radix_plan(work->image_span.size, &work->radix);
	if (ret)
		return GU_RESULT(ret);
	ret = request_asset(work, &work->boot_fw, BOOT_FILE);
	if (ret)
		return GU_RESULT(ret);
	if (work->boot_fw->size != 36864)
		return GU_RESULT(-EINVAL);
	ret = request_asset(work, &desc, BOOT_DESC_FILE);
	if (ret)
		return GU_RESULT(ret);
	ret = gpu_unlock_boot_desc_parse(desc->data, desc->size,
					(u32)work->boot_fw->size, &work->boot_desc);
	dev_info(&work->pdev->dev, "asset geometry: image=%llu signature=%llu boot=%u code=%#x data=%#x manifest=%#x parse=%d\n",
		(unsigned long long)work->image_span.size, (unsigned long long)work->sig_span.size,
		work->boot_desc.image_size, work->boot_desc.code_offset, work->boot_desc.data_offset,
		work->boot_desc.manifest_offset, ret);
	release_firmware(desc);
	return GU_RESULT(ret ? ret : load_booter(work));
}

static int unlock_transaction_layout(struct unlock_transaction_work *work)
{
	struct gpu_unlock_layout_in *in = &work->layout_in;
	u32 mb, vga, lo, hi, plm;
	int ret;

	if (read_bar(work, 0x001183a4, &mb) || !mb || mb == ~0u ||
	    read_bar(work, 0x00625f04, &vga) || vga == ~0u ||
	    read_bar(work, 0x001fa7c8, &plm) || plm == ~0u || !(plm & 1) ||
	    read_bar(work, 0x001fa82c, &lo) || lo == ~0u ||
	    read_bar(work, 0x001fa830, &hi) || hi == ~0u)
		return GU_RESULT(-ENODEV);
	/* Keep the cold board inputs in the USB kernel log even on layout failure.
	 * Register values after NVIDIA binds cannot reconstruct this state. */
	dev_info(&work->pdev->dev,
		 "layout inputs: fb_mb=%#x vga=%#x mmu_plm=%#x mmu_lo=%#x mmu_hi=%#x image=%llu boot=%u\n",
		 mb, vga, plm, lo, hi, (unsigned long long)work->image_span.size,
		 work->boot_desc.image_size);
	in->usable_fb = (u64)mb << 20;
	if (in->usable_fb < 256ull << 20) {
		dev_err(&work->pdev->dev, "layout rejected: usable FB below 256 MiB\n");
		return GU_RESULT(-ERANGE);
	}
	/* kgspPopulateWprMeta_TU102: PRAMIN is the top 1 MiB fallback. */
	in->vga_base = in->usable_fb - (1ull << 20);
	if (vga & (1u << 3)) {
		in->vga_base = (u64)(vga >> 8) << 16;
		if (in->vga_base < in->usable_fb - (1ull << 20))
			in->vga_base = in->usable_fb - 0x20000;
	}
	if (in->vga_base >= in->usable_fb) {
		dev_err(&work->pdev->dev, "layout rejected: VGA base at or above usable FB\n");
		return GU_RESULT(-ERANGE);
	}
	in->mmu_lock_low = (u64)(lo >> 4) << 12;
	{
		/* VAL is bits 31:4; the address is that field shifted by 12.
		 * memmgrBlockMemLockedMemory_GA100 only treats the lock as the
		 * VBIOS end-of-FB reservation when its exclusive high address is
		 * the FB top. A lock elsewhere is not that reservation. GA102
		 * has no scrubber, so WPR must stay in the pre-scrubbed top. */
		u64 mmu_hi = (u64)(hi >> 4) << 12;
		u64 mmu_top = mmu_hi > ~0ull - 0xffffull ? ~0ull :
			(mmu_hi + 0xffffull) & ~0xffffull;
		bool top_lock;

		in->mmu_lock_valid = mmu_hi >= in->mmu_lock_low;
		top_lock = in->mmu_lock_valid && (mmu_top >> 20) == (in->usable_fb >> 20);
		dev_info(&work->pdev->dev,
			 "layout mmu: lo=%#llx hi=%#llx hi_exclusive=%#llx constrain=%u\n",
			 (unsigned long long)in->mmu_lock_low, (unsigned long long)mmu_hi,
			 (unsigned long long)mmu_top, top_lock);
		if (!top_lock) {
			dev_info(&work->pdev->dev,
				 "mmu lock ignored: high address is not the FB top\n");
			in->mmu_lock_valid = false;
		}
	}
	in->frts_size = 1ull << 20;
	in->boot_size = work->boot_desc.image_size;
	in->image_size = work->image_span.size;
	in->nonwpr_size = 1ull << 20;
	/* GA102's supported minimum heap; no GSP-RM clients are started here. */
	in->heap_request = 88ull << 20;
	ret = gpu_unlock_layout(in, &work->layout);
	dev_info(&work->pdev->dev,
		 "layout decoded: fb=%#llx vga=%#llx mmu_valid=%u mmu_low=%#llx frts=%#llx heap=%#llx result=%d\n",
		 (unsigned long long)in->usable_fb, (unsigned long long)in->vga_base,
		 in->mmu_lock_valid, (unsigned long long)in->mmu_lock_low,
		 (unsigned long long)in->frts_size, (unsigned long long)in->heap_request, ret);
	if (ret) {
		dev_err(&work->pdev->dev, "layout rejected: reservation calculation (%d)\n", ret);
		return GU_RESULT(ret);
	}
	dev_info(&work->pdev->dev,
		 "layout ranges: nonwpr=%#llx wpr=%#llx heap=%#llx gsp=%#llx boot=%#llx frts=%#llx end=%#llx scrub_floor=%#llx\n",
		 (unsigned long long)work->layout.nonwpr_start,
		 (unsigned long long)work->layout.wpr_start,
		 (unsigned long long)work->layout.heap_offset,
		 (unsigned long long)work->layout.gsp_offset,
		 (unsigned long long)work->layout.boot_offset,
		 (unsigned long long)work->layout.frts_offset,
		 (unsigned long long)work->layout.wpr_end,
		 (unsigned long long)(in->usable_fb - (256ull << 20)));
	/* GA102 has no scrubber: all reservations must fit the pre-scrubbed top. */
	if (work->layout.nonwpr_start < in->usable_fb - (256ull << 20)) {
		dev_err(&work->pdev->dev, "layout rejected: reservations below pre-scrubbed top 256 MiB\n");
		return GU_RESULT(-ERANGE);
	}
	return 0;
}

static int unlock_transaction_radix(struct unlock_transaction_work *work)
{
	const struct gpu_unlock_radix_plan *r = &work->radix;
	u64 *addresses;
	u32 i, level, first = 0;
	int ret = 0;

	/* The plan comes from the bounded <=128 MiB image above. Keep explicit
	 * range checks at the narrowing boundary for the UEFI/MSVC build. */
	if (r->table_pages > 0xffffffffULL ||
	    r->pages[3] > 0xffffffffULL - r->table_pages)
		return GU_RESULT(-EOVERFLOW);
	work->page_count = (u32)(r->table_pages + r->pages[3]);
	work->pages = kvcalloc(work->page_count, sizeof(*work->pages), GFP_KERNEL);
	addresses = kvcalloc(work->page_count, sizeof(*addresses), GFP_KERNEL);
	if (!work->pages || !addresses) {
		kvfree(addresses);
		return GU_RESULT(-ENOMEM);
	}
	for (i = 0; i < work->page_count; i++) {
		struct unlock_transaction_page *page = &work->pages[i];

		page->cpu = diag_dma_alloc(work, "radix-page", GU_RADIX_PAGE,
					       &page->dma, GFP_KERNEL);
		if (!page->cpu) {
			ret = -ENOMEM;
			goto out;
		}
		work->pages_allocated++;
		addresses[i] = page->dma;
		memset(page->cpu, 0, GU_RADIX_PAGE);
		if (i >= r->table_pages) {
			u64 offset = (i - r->table_pages) * GU_RADIX_PAGE;
			size_t n = min_t(u64, GU_RADIX_PAGE, work->image_span.size - offset);

			memcpy(page->cpu, work->gsp_fw->data + work->image_span.offset + offset, n);
		}
	}
	for (level = 0; level < 3; level++) {
		u32 child = first + (u32)r->pages[level];

		for (i = 0; i < r->pages[level]; i++) {
			u32 at = i * (GU_RADIX_PAGE / 8);
			u32 n = (u32)min_t(u64, GU_RADIX_PAGE / 8, r->pages[level + 1] - at);

			ret = gpu_unlock_radix_page(work->pages[first + i].cpu,
						    addresses + child + at, n);
			if (ret)
				goto out;
		}
		first = child;
	}
out:
	kvfree(addresses);
	return GU_RESULT(ret);
}

static int unlock_transaction_claim(struct unlock_transaction_work *work)
{
	u16 command, pmcsr;
	u32 cap;
	unsigned int i;
	int ret;

	dev_info(&work->pdev->dev, "claim: power=%d pm_cap=%#x flags=%#x\n",
		work->pdev->current_state, work->pdev->pm_cap, work->pdev->dev_flags);
	if (work->pdev->current_state != PCI_D0 ||
	    (work->pdev->pm_cap &&
	     (diag_pci_word(work, false, work->pdev->pm_cap + PCI_PM_CTRL, &pmcsr) ||
	      (pmcsr & PCI_PM_CTRL_STATE_MASK) != PCI_D0)))
		return GU_RESULT(-EOPNOTSUPP);
	if ((work->pdev->dev_flags & PCI_DEV_FLAGS_NO_FLR_RESET) ||
	    !pci_is_pcie(work->pdev) ||
	    diag_pci_dword(work, true, PCI_EXP_DEVCAP, &cap) ||
	    !(cap & PCI_EXP_DEVCAP_FLR))
		return GU_RESULT(-EOPNOTSUPP);
	if (diag_pci_word(work, false, PCI_COMMAND, &command) || command == 0xffff)
		return GU_RESULT(-ENODEV);
	work->original_cmd = command;
	work->cmd_valid = true;
	if (command & PCI_COMMAND_MASTER)
		return GU_RESULT(-EBUSY);
	dev_info(&work->pdev->dev, "claim: command=%#x capability=%#x BAR_mask=%#x\n",
		command, cap, pci_select_bars(work->pdev, IORESOURCE_MEM));
	/* Only BAR0 is mapped. On the display card the firmware framebuffer keeps
	 * BAR1 until just before the FLR, as it does while the stock driver boots.
	 */
	ret = pci_request_region(work->pdev, 0, "gpu_unlock");
	if (ret)
		return GU_RESULT(ret);
	work->region = true;
	if (!(command & PCI_COMMAND_MEMORY)) {
		ret = pci_enable_device_mem(work->pdev);
		if (ret)
			return GU_RESULT(ret);
		work->enabled = true;
	}
	work->bar_len = pci_resource_len(work->pdev, 0);
	if (work->bar_len < SEC2_FALCON2 + 0x1000)
		return GU_RESULT(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(work->bars); i++)
		if (diag_pci_dword(work, false, PCI_BASE_ADDRESS_0 + i * 4, &work->bars[i]))
			return GU_RESULT(-EIO);
	ret = pci_save_state(work->pdev);
	if (ret)
		return GU_RESULT(ret);
	work->bar = pci_iomap(work->pdev, 0, 0);
	if (!work->bar)
		return GU_RESULT(-ENOMEM);
	work->mapped = true;
	return 0;
}

static int unlock_transaction_dma(struct unlock_transaction_work *work)
{
	struct device *dev = &work->pdev->dev;

	if (!dev->dma_mask || work->masks_saved)
		return GU_RESULT(-EINVAL);
	work->masks_saved = true;
	work->streaming_ptr = dev->dma_mask;
	work->streaming_before = *dev->dma_mask;
	work->coherent_before = dev->coherent_dma_mask;
	dev_info(dev, "DMA masks: streaming=%#llx coherent=%#llx requested=%#llx\n",
		(unsigned long long)work->streaming_before, (unsigned long long)work->coherent_before,
		(unsigned long long)DMA_BIT_MASK(32));
	work->streaming_set = true;
	if (GU_DIAG(dma_set_mask(dev, DMA_BIT_MASK(32))))
		return GU_RESULT(-EIO);
	work->coherent_set = true;
	if (GU_DIAG(dma_set_coherent_mask(dev, DMA_BIT_MASK(32))))
		return GU_RESULT(-EIO);
	work->booter_cpu = diag_dma_alloc(work, "booter", work->booter_image_size,
					      &work->booter_dma, GFP_KERNEL);
	if (!work->booter_cpu)
		return GU_RESULT(-ENOMEM);
	memcpy(work->booter_cpu, work->booter_fw->data + 72, work->booter_image_size);
	work->v67_cpu = diag_dma_alloc(work, "v67", GU_V67_SIZE, &work->v67_dma, GFP_KERNEL);
	work->meta_cpu = diag_dma_alloc(work, "metadata", GU_WPR_META_SIZE, &work->meta_dma, GFP_KERNEL);
	if (!work->v67_cpu || !work->meta_cpu)
		return GU_RESULT(-ENOMEM);
	memcpy(work->v67_cpu, work->v67_fw->data, GU_V67_SIZE);
	work->stock_size = work->sig_span.size;
	work->stock_cpu = diag_dma_alloc(work, "stock-signature", work->stock_size, &work->stock_dma, GFP_KERNEL);
	work->boot_cpu = diag_dma_alloc(work, "boot-image", work->boot_desc.image_size, &work->boot_dma, GFP_KERNEL);
	if (!work->stock_cpu || !work->boot_cpu)
		return GU_RESULT(-ENOMEM);
	memcpy(work->stock_cpu, work->gsp_fw->data + work->sig_span.offset, work->stock_size);
	memcpy(work->boot_cpu, work->boot_fw->data, work->boot_desc.image_size);
	{
		int ret = patch_booter(work);

		return GU_RESULT(ret ? ret : unlock_transaction_radix(work));
	}
}

static int unlock_transaction_fwsec(struct unlock_transaction_work *work)
{
	static const struct falcon_eng gsp = {
		.base = 0x00110000, .fbif = 0x00110600, .falcon2 = 0x00111000,
	};
	size_t mapped = 0;
	u8 *rom = NULL;
	struct gpu_unlock_fwsec fw;
	u32 fuse_raw, version, scratch, wpr_lo, wpr_hi, dmem_transfer;
	u16 command;
	u64 frts = work->layout.frts_offset;
	void *image = NULL;
	dma_addr_t image_dma = 0;
	int ret;

	/* kgspExtractVbiosFromRom_TU102 reads NV_PROM_DATA, up to 1 MiB.
	 * pci_map_rom stops at the PCI last-image flag and hides the VN extension. */
	dev_info(&work->pdev->dev, "FWSEC: reading pinned AD103 PROM view at %#x\n", GU_PROM_BAR_OFFSET);
	if (work->bar_len < GU_PROM_BAR_OFFSET + GU_PROM_VIEW_BYTES)
		return GU_RESULT(-ENODEV);
	mapped = GU_PROM_VIEW_BYTES; /* Hardware PROM already skips the full-file prefix. */
	rom = kvmalloc(mapped, GFP_KERNEL);
	if (!rom)
		return GU_RESULT(-ENOMEM);
	memcpy_fromio(rom, (u8 __iomem *)work->bar + GU_PROM_BAR_OFFSET, mapped);
	dev_info(&work->pdev->dev, "FWSEC: PROM bytes=%zu signature=%#x\n",
		 mapped, (unsigned int)(rom[0] | (rom[1] << 8)));
	ret = gpu_unlock_vbios_fwsec(rom, mapped, &fw);
	if (ret) {
		dev_err(&work->pdev->dev, "FWSEC-FRTS not found in board ROM (%d)\n", ret);
		goto out;
	}
	if (fw.engine_mask != 0x400) {
		dev_err(&work->pdev->dev, "FWSEC: unsupported GSP engine mask=%#x\n", fw.engine_mask);
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = gpu_unlock_fwsec_dmem_transfer(&fw, &dmem_transfer);
	if (ret)goto out;
	dev_info(&work->pdev->dev, "FWSEC DMEM declared=%#x transfer=%#x image=%#x\n",fw.dmem_size,dmem_transfer,fw.image_size);
	image = diag_dma_alloc(work, "fwsec", fw.image_size, &image_dma, GFP_KERNEL);
	if (!image) {
		ret = -ENOMEM;
		goto out;
	}
	work->fwsec_cpu = image;
	work->fwsec_dma = image_dma;
	work->fwsec_size = fw.image_size;
	memcpy(image, rom + fw.image_off, fw.image_size);
	ret = gpu_unlock_fwsec_patch_frts(image, &fw, frts);
	if (ret)
		goto out;
	if (!fw.ucode_id ||
	    read_bar(work, 0x008241c0 + (fw.ucode_id - 1) * 4, &fuse_raw)) {
		ret = -EIO;
		goto out;
	}
	version = gpu_unlock_fuse_version(fuse_raw);
	dev_info(&work->pdev->dev, "FWSEC fuse: raw=%#x version=%u\n", fuse_raw, version);
	{
		u32 sig_at = 0;

		ret = gpu_unlock_fwsec_sig_offset(fw.sig_versions, version, 384, &sig_at);
		if (ret)
			goto out;
		memcpy((u8 *)image + fw.data_off + fw.pkc_off, rom + fw.sig_off + sig_at, 384);
	}
	pci_set_master(work->pdev);
	work->master = true;
	if (diag_pci_word(work, false, PCI_COMMAND, &command) ||
	    command == 0xffff || !(command & PCI_COMMAND_MASTER)) {
		ret = -EIO;
		goto out;
	}
	work->firmware_started = true;
#ifdef GU_UEFI_PORT
	GuStarted();
#endif
	ret = falcon_hs(work, &gsp, 0, fw.imem_size, fw.imem_va, fw.imem_pa,
			fw.data_off, dmem_transfer, fw.dmem_pa, fw.pkc_off,
			fw.engine_mask, fw.ucode_id,
			image_dma, 0, 0, false, false);
	if (ret)
		goto out;
	if (read_bar(work, 0x00001400 + 0x0e * 4, &scratch)) {
		ret = -EIO;
		goto out;
	}
	dev_info(&work->pdev->dev, "FWSEC result: scratch=%#x expected_hi16=0\n", scratch);
	if (scratch >> 16) {
		ret = -EIO;
		goto out;
	}
	if (read_bar(work, 0x001fa824, &wpr_lo) || wpr_lo == ~0u ||
	    read_bar(work, 0x001fa828, &wpr_hi) || wpr_hi == ~0u) {
		ret = -EIO;
		goto out;
	}
	dev_info(&work->pdev->dev, "FWSEC WPR2: low=%#x high=%#x expected_page=%#llx end=%#llx\n",
		wpr_lo, wpr_hi, (unsigned long long)(frts >> 12),
		(unsigned long long)work->layout.wpr_end);
	/* Match NVIDIA's initialized-WPR2 check as well as the expected low
	 * address. Never publish Booter metadata for disabled/alien WPR2. */
	if (!(wpr_hi >> 4) || (wpr_hi >> 4) < (wpr_lo >> 4) ||
	    ((u64)(wpr_hi >> 4) << 12) >= work->layout.wpr_end ||
	    (wpr_lo >> 4) != (u32)(frts >> 12))
		ret = -EIO;
out:
	/* Ownership transfers to work before publication, including failure paths. */
	kvfree(rom);
	return GU_RESULT(ret);
}

static int unlock_transaction_gfw(struct unlock_transaction_work *work)
{
	ktime_t deadline = ktime_add_us(ktime_get(), GU_GFW_TIMEOUT_US);
	int assessed = -EAGAIN;

	for (;;) {
		struct gpu_unlock_gfw_sample sample = { };
		u16 vendor = 0xffff;

		if (read_bar(work, GU_GFW_PLM_OFFSET, &sample.plm) ||
		    read_bar(work, GU_GFW_BOOT_OFFSET, &sample.boot) ||
		    read_bar(work, GU_GFW_CPUCTL_OFFSET, &sample.cpuctl))
			return GU_RESULT(-EIO);
		if (!diag_pci_word(work, false, PCI_VENDOR_ID, &vendor) &&
		    vendor != 0xffff)
			sample.config_ok = true;
		assessed = gpu_unlock_gfw_assess(&sample);
		if (!assessed) {
			u32 ss0, ss1, wpr_hi;

			if (read_bar(work, GU_FEAT_PLM_OFFSET, &work->plm_entry) ||
			    read_bar(work, GU_FEAT_SS0_OFFSET, &ss0) || inaccessible(ss0) ||
			    read_bar(work, GU_FEAT_SS1_OFFSET, &ss1) || ss1 == ~0u ||
			    read_bar(work, 0x001fa828, &wpr_hi) || inaccessible(wpr_hi))
				return GU_RESULT(-ENODEV);
			dev_info(&work->pdev->dev, "entry: plm=%#x ss0=%#x ss1=%#x wpr2_hi=%#x\n",
				 work->plm_entry, ss0, ss1, wpr_hi);
			/* An open PLM means the card was already unlocked or not cold. */
			if (work->plm_entry == ~0u)
				return GU_RESULT(-EALREADY);
			/* WPR2 is down from power-on until FWSEC-FRTS runs. An enabled
			 * WPR2 means a driver has already used the card this power
			 * cycle; starting FWSEC and Booter again from here is untested.
			 */
			if (wpr_hi >> 4) {
				dev_err(&work->pdev->dev, "card already initialised since power on (WPR2 enabled); power off first\n");
				return GU_RESULT(-EBUSY);
			}
			return 0;
		}
		if (assessed != -EAGAIN)
			return GU_RESULT(assessed);
		if (ktime_compare(ktime_get(), deadline) >= 0)
			return GU_RESULT(-ETIMEDOUT);
		usleep_range(1000, 2000);
	}
}

static int hold_sec2_reset(struct unlock_transaction_work *work, const char *phase);

/* NVIDIA kflcnReset_TU102 switches back to Falcon after reset completes.
 * kflcnSwitchToFalcon_GA102 explicitly releases priv lockdown. RUN08's
 * reset-only retry read DMATRFCMD before this step and saw badf5620. */
static int sec2_restore_falcon_access(struct unlock_transaction_work *work)
{
	u32 hwcfg, bcr = 0, reset, dmactl, chip, rm;
	int ret;

	if (read_bar(work, SEC2_BASE + 0x3c0, &reset) || inaccessible(reset) ||
	    (reset & 1) || read_bar(work, SEC2_BASE + 0xf4, &hwcfg) || inaccessible(hwcfg))
		return GU_RESULT(-EIO);
	if (hwcfg & (1u << 10)) {
		if (read_bar(work, SEC2_FALCON2 + 0x668, &bcr) || inaccessible(bcr))
			return GU_RESULT(-EIO);
		dev_info(&work->pdev->dev,
			"PLM retry Falcon switch: BCR_CTRL=%#x HWCFG2=%#x reset=%#x\n", bcr, hwcfg, reset);
		/* CORE_SELECT bit4=0 requests Falcon. VALID bit0 is hardware's
		 * acknowledgement. Protected values are never decoded as fields. */
		if ((bcr & 0x11) != 1)
			write_bar(work, SEC2_FALCON2 + 0x668, 0);
		ret = poll_us(work, SEC2_FALCON2 + 0x668, 0x11, 1, 1000000, true);
		if (ret)
			return GU_RESULT(ret);
	}
	if (read_bar(work, 0, &chip) || chip != 0x193000a1u)
		return GU_RESULT(-ENODEV);
	write_bar(work, SEC2_BASE + 0x84, chip);
	if (read_bar(work, SEC2_BASE + 0x84, &rm) || rm != chip ||
	    read_bar(work, SEC2_BASE + 0x10c, &dmactl) || inaccessible(dmactl) ||
	    read_bar(work, SEC2_BASE + 0xf4, &hwcfg) || inaccessible(hwcfg))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev,
		"PLM retry access restored: RM=%#x DMACTL=%#x HWCFG2=%#x\n", rm, dmactl, hwcfg);
	return 0;
}

/* The single-fire payload is immutable and contains all four fixed pairs. */
static int payload_copy_check(struct unlock_transaction_work *work, const char *phase)
{
 bool match=!memcmp(work->v67_cpu,work->v67_fw->data,GU_V67_SIZE);
 dev_info(&work->pdev->dev,"PLM payload: phase=%s copy_match=%u bytes=%u fixed_four_targets=1 cpu_view_only=1\n",phase,match,GU_V67_SIZE);
 return match ? 0 : GU_RESULT(-EIO);
}

/* Comparison variant: restore WPR2 and refill the host signature COPY
 * before SEC2 reset. Capture both sides, and never silently repair a change
 * caused by reset. SEC2 engine reset is distinct from PCIe FLR. */
static int sec2_prepare_fire(struct unlock_transaction_work *work,
                            const struct falcon_eng *sec2,
                            u32 saved_lo, u32 saved_hi, unsigned int prior_fires)
{
	u32 before_lo, before_hi, lo, hi, post_lo, post_hi;
	int ret;

	dev_info(&work->pdev->dev,
		"PLM retry prepare: fire=%u WPR2 -> payload -> reset -> Falcon access -> DMA idle\n", prior_fires + 1);
	/* Unlike reset-first preparation, the published input will be changed
	 * before reset. Do not refill while the previous CPU is still running,
	 * or when HALT cannot be read. This is not global DMA quiescence. */
	if (prior_fires) {
		ret = poll_us(work, SEC2_BASE + 0x100, 0x10, 0x10, 5000000, true);
		if (ret) return GU_RESULT(ret);
	}
	if (read_bar(work, 0x1fa824, &before_lo) || inaccessible(before_lo) ||
	    read_bar(work, 0x1fa828, &before_hi) || inaccessible(before_hi))
		return GU_RESULT(-EIO);
	write_bar(work, 0x1fa824, saved_lo);
	write_bar(work, 0x1fa828, saved_hi);
	if (read_bar(work, 0x1fa824, &lo) ||
	    read_bar(work, 0x1fa828, &hi)) return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev,
		"PLM WPR2: phase=before-reset fire=%u before=%08x:%08x restored=%08x:%08x expected=%08x:%08x restore_changed=%u matched=%u\n",
		prior_fires + 1, before_hi, before_lo, hi, lo, saved_hi, saved_lo,
		before_hi != hi || before_lo != lo, hi == saved_hi && lo == saved_lo);
	dev_info(&work->pdev->dev,
		"PLM WPR2 host restore: fire=%u change_needed=%u host_write_accepted=%s\n",
		prior_fires + 1, before_lo != saved_lo || before_hi != saved_hi,
		(before_lo == saved_lo && before_hi == saved_hi) ? "unproven-same-value" :
		(lo == saved_lo && hi == saved_hi) ? "1" : "0");
	if (lo != saved_lo || hi != saved_hi) return GU_RESULT(-EIO);
	dma_wmb();
#ifdef GU_UEFI_PORT
	if (Gu.fault) return GU_RESULT(-EIO);
#endif
	ret = payload_copy_check(work, "before-reset");
	if (ret) return GU_RESULT(ret);
	/* Preserve the v0.4.9 first-reset implementation and subsequent strict
	 * reset helper, so only preparation ordering and readback checks change. */
	ret = prior_fires ? hold_sec2_reset(work, "plm-next-fire") : falcon_reset(work, sec2);
	if (ret) return GU_RESULT(ret);
	if (read_bar(work, 0x1fa824, &post_lo) || read_bar(work, 0x1fa828, &post_hi))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev,
		"PLM WPR2: phase=after-reset fire=%u before=%08x:%08x after=%08x:%08x reset_changed=%u matched=%u\n",
		prior_fires + 1, hi, lo, post_hi, post_lo,
		post_hi != hi || post_lo != lo, post_hi == saved_hi && post_lo == saved_lo);
	ret = payload_copy_check(work, "after-reset");
	if (ret) return GU_RESULT(ret);
	if (post_hi != saved_hi || post_lo != saved_lo) return GU_RESULT(-EIO);
	if (prior_fires) {
		ret = sec2_restore_falcon_access(work);
		if (ret) return GU_RESULT(ret);
	}
	ret = poll_us(work, SEC2_BASE + 0x118, 1, 0, 1000000, true);
	if (ret) return GU_RESULT(ret);
	dev_info(&work->pdev->dev, "PLM retry DMA idle: access verified after Falcon switch\n");
	dev_info(&work->pdev->dev,
		"PLM retry ready: fire=%u WPR2=%08x:%08x; no post-reset WPR2/payload rewrite; same-value readback is not write-permission proof\n",
		prior_fires + 1, post_hi, post_lo);
	return 0;
}

static int read_target_plm(struct unlock_transaction_work *work, u32 addr, u32 *value)
{
	u32 identity, chip;
	/* All ones can be a real PLM value. Check independent PCI/BAR identity
	 * and reject protected-read sentinels instead of rejecting all ones. */
	if (diag_pci_dword(work, false, PCI_VENDOR_ID, &identity) ||
	    identity != GU_TARGET_PCI_ID || read_bar(work, 0, &chip) ||
	    chip != 0x193000a1u || read_bar(work, addr, value) ||
	    (*value & 0xffff0000u) == 0xbadf0000u)
		return GU_RESULT(-ENODEV);
	return 0;
}

/* Exactly one SEC2 START. Each PLM must match after completion. */
static int unlock_transaction_boot(struct unlock_transaction_work *work)
{
 static const struct falcon_eng sec2={.base=SEC2_BASE,.fbif=SEC2_FBIF,.falcon2=SEC2_FALCON2};
 const struct gpu_unlock_booter_hdr *h=&work->booter_hdr;
 u32 saved_lo,saved_hi,marker,cpuctl,debug,plm,post_lo,post_hi;
 unsigned int index;
 int ret,halt_ret;
 bool all=true, required=true;
 if (!work->published || !work->master || !work->firmware_started || !work->v67_cpu)
  return GU_RESULT(-EINVAL);
 if (read_bar(work,0x1fa824,&saved_lo) || inaccessible(saved_lo) ||
     read_bar(work,0x1fa828,&saved_hi) || inaccessible(saved_hi)) return GU_RESULT(-EIO);
 for(index=0;index<ARRAY_SIZE(plm_targets);index++) {
  if(read_target_plm(work,plm_targets[index].addr,&plm))return GU_RESULT(-ENODEV);
  dev_info(&work->pdev->dev,"AD102 PLM initial: index=%u target=%s value=%#x expected=%#x\n",index,plm_targets[index].name,plm,plm_targets[index].value);
 }
 dev_info(&work->pdev->dev,"AD102 SINGLE FIRE: WPR_CFG->FBPA->WPR->FEAT inside one SEC2 execution; max_starts=1\n");
 ret=sec2_prepare_fire(work,&sec2,saved_lo,saved_hi,0);
 if(ret)return GU_RESULT(ret);
 /* Observer watches the final target; full read-back below remains decisive. */
 work->plm_target=plm_targets[3].addr;
 work->plm_expected=plm_targets[3].value;
 ret=falcon_hs(work,&sec2,h->app_code_off,h->app_code_size,h->app_code_off,0,
  h->os_data_off,h->os_data_size,0,h->patch_loc-h->os_data_off,h->engine_id,
  h->ucode_id,work->booter_dma,lower_32_bits(work->meta_dma),upper_32_bits(work->meta_dma),true,true);
 if(Gu.fault)return GU_RESULT(ret ? ret : -EIO);
 halt_ret=ret ? ret : poll_us(work,SEC2_BASE+0x100,0x10,0x10,5000000,true);
 if(Gu.fault)return GU_RESULT(halt_ret ? halt_ret : -EIO);
 if(read_bar(work,SEC2_BASE+0x40,&marker) || read_bar(work,SEC2_BASE+0x100,&cpuctl) ||
    read_bar(work,SEC2_BASE+0x94,&debug))return GU_RESULT(-ENODEV);
 work->sec2_halt_verified=!inaccessible(cpuctl) && !!(cpuctl&0x10);
 for(index=0;index<ARRAY_SIZE(plm_targets);index++) {
  if(read_target_plm(work,plm_targets[index].addr,&plm))return GU_RESULT(-ENODEV);
  dev_info(&work->pdev->dev,"AD102 PLM final: index=%u target=%s value=%#x expected=%#x matched=%u\n",index,plm_targets[index].name,plm,plm_targets[index].value,plm==plm_targets[index].value);
  if(plm!=plm_targets[index].value) { all=false; if(index!=1)required=false; }
 }
 if(read_bar(work,0x1fa824,&post_lo) || read_bar(work,0x1fa828,&post_hi))return GU_RESULT(-ENODEV);
 dev_info(&work->pdev->dev,"AD102 SINGLE RESULT: booter_ret=%d halt_ret=%d all_expected=%u marker=%#x marker_ok=%u cpuctl=%#x debug=%#x WPR2=%08x:%08x\n",ret,halt_ret,all,marker,marker==0x67b00001u,cpuctl,debug,post_hi,post_lo);
 if(ret || halt_ret || !required || marker!=0x67b00001u || !work->sec2_halt_verified || debug!=0xda550000u)
  return GU_RESULT(ret ? ret : (halt_ret ? halt_ret : -EIO));
 last_outcome.required_plm_verified=1;
 last_outcome.all_plm_verified=all;
 dev_info(&work->pdev->dev,"AD102 PLM chain: fires=1/1 required_expected=1; FBPA mismatch is nonfatal; restoring stock signature before SS0/SS1 and Windows handoff\n");
 return 0;
}

static void put_le64(u8 *dest, u64 value)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		dest[i] = (u8)(value >> (8 * i));
}

static int unlock_transaction_signature(struct unlock_transaction_work *work)
{
	u8 *meta = work->meta_cpu;

	if (!meta || !work->stock_cpu || !work->pages_allocated ||
	    work->signature_calls >= 2)
		return GU_RESULT(-EINVAL);
	if (work->signature_calls == 0) {
		struct gpu_unlock_wpr_args args = {
			.fb_size = work->layout_in.usable_fb,
			.vga_offset = work->layout_in.vga_base,
			.frts_size = work->layout_in.frts_size,
			.signature_dma = work->v67_dma, .signature_size = GU_V67_SIZE,
			.radix_dma = work->pages[0].dma,
			.image_size = work->image_span.size,
			.boot_dma = work->boot_dma, .boot = work->boot_desc,
		};
		int ret = gpu_unlock_wpr_meta_pack(meta, &work->layout, &args);

		if (ret)
			return GU_RESULT(ret);
		dma_wmb();
		work->published = true;
	} else {
		put_le64(meta + 72, work->stock_dma);
		put_le64(meta + 80, work->stock_size);
		dma_wmb();
	}
	work->signature_calls++;
	return 0;
}

/* Write the unlock value to one speed selector and read it back. */
static int write_selector(struct unlock_transaction_work *work, const char *name,
                          u32 offset, u32 unlock)
{
 u32 before=~0u, after=~0u;
 int ret=0;
 if(read_bar(work,offset,&before) || inaccessible(before) || Gu.fault)ret=-ENODEV;
 else {
  if(before!=unlock)write_bar(work,offset,unlock);
  if(read_bar(work,offset,&after) || inaccessible(after) || Gu.fault)ret=-ENODEV;
  else if(after!=unlock)ret=-EIO;
 }
 dev_info(&work->pdev->dev,"SELECTOR RESULT: %s before=%#x after=%#x expected=%#x matched=%u status=%d\n",
          name,before,after,unlock,!ret,ret);
 return ret;
}

/* A readable rejected value is a failed unlock, but still permits the caller
 * to run recovery. I/O errors leave engine state uncertain and require retain. */
static int unlock_transaction_selector(struct unlock_transaction_work *work)
{
 int ss0=write_selector(work,"SS0",GU_FEAT_SS0_OFFSET,GU_SS0_UNLOCK);
 if(ss0 && ss0!=-EIO)return GU_RESULT(ss0);
 int ss1=write_selector(work,"SS1",GU_FEAT_SS1_OFFSET,GU_SS1_UNLOCK);
 dev_info(&work->pdev->dev,"SELECTOR SUMMARY: ss0_status=%d ss1_status=%d unlock_matched=%u; recovering for Windows\n",ss0,ss1,!ss0&&!ss1);
 return ss1 ? GU_RESULT(ss1) : ss0 ? GU_RESULT(ss0) : 0;
}

/* Reset SEC2 after FLR and leave it halted with reset released. Do not reset
 * GSP here: stock RM first requires completed GFW and a halted GSP Falcon.
 * ksec2ResetHw_TU102 ends with RESET_FALSE. Leaving RESET_TRUE makes later
 * host reads return FECS_PRI_RESET (0xbadf12 in bits 31:8). RM's scrub wait
 * treats that value as scrubbing-pending and does not finish.
 */
static int hold_sec2_reset(struct unlock_transaction_work *work, const char *phase)
{
	unsigned int j;
	u32 engine, before, first = 0, hwcfg;
	int ret;

	if (read_bar(work, SEC2_BASE + 0x3c0, &engine) || inaccessible(engine))
		return GU_RESULT(-ENODEV);
	before = engine;
	dev_info(&work->pdev->dev, "engine reset assert: phase=%s base=%#x before=%#x write=%#x\n",
		 phase, SEC2_BASE, before, before | 1);
	write_bar(work, SEC2_BASE + 0x3c0, engine | 1);
	/* 610 ksec2ResetHw_TU102 requires ten register cycles for signal
	 * propagation. A stale zero during those cycles is not evidence that
	 * the assertion failed. No CPU start and no reset retry.
	 */
	for (j = 0; j < 10; j++) {
		if (read_bar(work, SEC2_BASE + 0x3c0, &engine) ||
		    inaccessible(engine)) {
			dev_err(&work->pdev->dev, "engine reset inaccessible: phase=%s base=%#x cycle=%u value=%#x\n",
				phase, SEC2_BASE, j + 1, engine);
			return GU_RESULT(-EIO);
		}
		if (!j)
			first = engine;
	}
	dev_info(&work->pdev->dev, "engine reset propagation: phase=%s base=%#x first=%#x tenth=%#x\n",
		 phase, SEC2_BASE, first, engine);
	ret = poll_us(work, SEC2_BASE + 0x3c0, 1, 1, 1000, true);
	dev_info(&work->pdev->dev, "engine reset asserted: phase=%s base=%#x error=%d last=%#x\n",
		 phase, SEC2_BASE, ret, work->last_value);
	if (ret)
		return GU_RESULT(ret);
	if (read_bar(work, SEC2_BASE + 0x3c0, &engine) || inaccessible(engine))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev, "engine reset release: phase=%s base=%#x before=%#x write=%#x\n",
		 phase, SEC2_BASE, engine, engine & ~1u);
	write_bar(work, SEC2_BASE + 0x3c0, engine & ~1u);
	for (j = 0; j < 10; j++) {
		if (read_bar(work, SEC2_BASE + 0x3c0, &engine) ||
		    inaccessible(engine)) {
			dev_err(&work->pdev->dev, "engine reset release inaccessible: phase=%s base=%#x cycle=%u value=%#x\n",
				phase, SEC2_BASE, j + 1, engine);
			return GU_RESULT(-EIO);
		}
	}
	ret = poll_us(work, SEC2_BASE + 0x3c0, 1, 0, 1000, true);
	if (ret)
		return GU_RESULT(ret);
	/* NV_PFALCON_FALCON_HWCFG2 bit 12 clears when memory scrubbing finishes.
	 * A PRI error fails this wait instead of being handed to RM.
	 */
	ret = poll_us(work, SEC2_BASE + 0xf4, 1u << 12, 0, 1000000, true);
	if (ret)
		return GU_RESULT(ret);
	if (read_bar(work, SEC2_BASE + 0xf4, &hwcfg) || inaccessible(hwcfg))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev, "engine reset released: phase=%s base=%#x engine=%#x hwcfg2=%#x\n",
		 phase, SEC2_BASE, engine, hwcfg);
	return 0;
}

/* PCI configuration can be ready before BAR0. The 610 BIF restore path waits
 * for PMC_BOOT_0 to match the saved chip ID before touching engine registers.
 * Transient badf/all-ones reads are expected only inside this bounded wait.
 */
static int wait_post_flr_bar(struct unlock_transaction_work *work, u32 chip)
{
	ktime_t deadline = ktime_add_us(ktime_get(), 1000000);
	u32 value = 0;

	for (;;) {
		if (read_bar(work, 0, &value))
			return GU_RESULT(-EIO);
		if (value == chip) {
			dev_info(&work->pdev->dev, "post-FLR BAR ready: chip=%#x\n", value);
			return 0;
		}
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			dev_err(&work->pdev->dev, "post-FLR BAR timeout: chip=%#x expected=%#x budget_us=1000000\n",
				value, chip);
			return GU_RESULT(-ETIMEDOUT);
		}
		usleep_range(1000, 2000);
	}
}

/* Match gpuWaitForGfwBootComplete_TU102, without launching any firmware or
 * overwriting the cold-entry evidence. The post-FLR ROM boot may still be busy.
 */
static int wait_post_flr_gfw(struct unlock_transaction_work *work)
{
	ktime_t deadline = ktime_add_us(ktime_get(), GU_GFW_TIMEOUT_US);
	struct gpu_unlock_gfw_sample sample = { };
	u32 identity = 0;
	int ret;

	for (;;) {
		sample = (struct gpu_unlock_gfw_sample){ };
		ret = diag_pci_dword(work, false, PCI_VENDOR_ID, &identity);
		sample.config_ok = !ret && identity == GU_TARGET_PCI_ID;
		if (!sample.config_ok) {
			ret = -ENODEV;
			break;
		}
		if (read_bar(work, GU_GFW_CPUCTL_OFFSET, &sample.cpuctl) ||
		    read_bar(work, GU_GFW_PLM_OFFSET, &sample.plm)) {
			ret = -EIO;
			break;
		}
		/* Avoid the protected scratch read until PL0 can read it. */
		if (!inaccessible(sample.plm) && (sample.plm & GU_GFW_PLM_LEVEL0) &&
		    read_bar(work, GU_GFW_BOOT_OFFSET, &sample.boot)) {
			ret = -EIO;
			break;
		}
		ret = inaccessible(sample.plm) || inaccessible(sample.boot) ||
		      inaccessible(sample.cpuctl) ? -EAGAIN : gpu_unlock_gfw_assess(&sample);
		if (!ret)
			break;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			ret = -ETIMEDOUT;
			break;
		}
		usleep_range(1000, 2000);
	}
	dev_info(&work->pdev->dev, "post-FLR GFW result: error=%d pci=%#x plm=%#x boot=%#x cpuctl=%#x budget_us=%u\n",
		 ret, identity, sample.plm, sample.boot, sample.cpuctl, GU_GFW_TIMEOUT_US);
	return GU_RESULT(ret);
}

static int unlock_transaction_reset(struct unlock_transaction_work *work)
{
	u32 identity, bar, lo, hi, ss0, ss1, plm, chip, cpuctl;
	u16 command, pending;
	unsigned int i;
	int ret;

	if (work->signature_calls != 2)
		return GU_RESULT(-EPROTO);
	if (read_bar(work, GU_FEAT_SS0_OFFSET, &ss0) || inaccessible(ss0) ||
	    read_bar(work, GU_FEAT_SS1_OFFSET, &ss1) || inaccessible(ss1))
		return GU_RESULT(-ENODEV);
	dev_info(&work->pdev->dev, "pre-FLR selectors: ss0=%#x expected=%#x ss1=%#x expected=%#x\n",
		ss0, GU_SS0_UNLOCK, ss1, GU_SS1_UNLOCK);
	last_outcome.selectors_before_flr = ss0 == GU_SS0_UNLOCK && ss1 == GU_SS1_UNLOCK;
	dev_info(&work->pdev->dev, "selector pre-FLR readback: unlock_matched=%u; recovery continues on mismatch, unlock success does not\n",
		 last_outcome.selectors_before_flr);
	pci_clear_master(work->pdev);
	work->master = false;
	if (diag_pci_word(work, false, PCI_COMMAND, &command) ||
	    command == 0xffff || (command & PCI_COMMAND_MASTER))
		return GU_RESULT(-EIO);
	if (read_bar(work, 0, &chip) || !chip || inaccessible(chip))
		return GU_RESULT(-ENODEV);
	/* Ampere PF-FLR requires no engine-reset preconditioning in this cold,
	 * unbound path (no initialized NVLink/P2P). The pinned GA102 rejoin14
	 * sequence also goes from the early PLM result to FLR. A pre-FLR engine
	 * hold is not a prerequisite: secure firmware can reject that write.
	 * Keep EVERY allocation alive, BME off, until post-FLR containment.
	 */
	/* FLR resets the display engine, so a firmware framebuffer (simpledrm,
	 * efifb) on this card must go first; nvidia-drm replaces it later. This
	 * does nothing on a card without one.
	 */
	ret = aperture_remove_conflicting_pci_devices(work->pdev, "gpu_unlock");
	if (ret)
		return GU_RESULT(ret);
	dev_info(&work->pdev->dev, "PCIe FLR begin: BME=0 DMA retained, chip=%#x\n", chip);
	ret = pcie_flr(work->pdev);
	dev_info(&work->pdev->dev, "PCIe FLR returned: result=%d\n", ret);
	if (ret)
		return GU_RESULT(ret);
	/* The saved image predates BME. No transient restore with BME set. */
	pci_restore_state(work->pdev);
	if (diag_pci_dword(work, false, PCI_VENDOR_ID, &identity) ||
	    identity != GU_TARGET_PCI_ID ||
	    diag_pci_word(work, false, PCI_COMMAND, &command) ||
	    command == 0xffff || (command & PCI_COMMAND_MASTER) ||
	    !(command & PCI_COMMAND_MEMORY))
		return GU_RESULT(-ENODEV);
	for (i = 0; i < ARRAY_SIZE(work->bars); i++) {
		if (diag_pci_dword(work, false, PCI_BASE_ADDRESS_0 + i * 4, &bar) ||
		    bar != work->bars[i]) {
			dev_err(&work->pdev->dev, "post-FLR BAR%u: actual=%#x expected=%#x\n", i, bar, work->bars[i]);
			return GU_RESULT(-EIO);
		}
	}
	ret = wait_post_flr_bar(work, chip);
	if (ret)
		return GU_RESULT(ret);
	/* ROM GFW completion/halt plus FLR disposes of the old GSP execution.
	 * Stock RM checks this halt before resetting GSP with its own buffers.
	 */
	ret = wait_post_flr_gfw(work);
	if (ret)
		return GU_RESULT(ret);
	ret = hold_sec2_reset(work, "post-FLR");
	if (ret)
		return GU_RESULT(ret);
	if (diag_pci_word(work, true, PCI_EXP_DEVSTA, &pending) ||
	    pending == 0xffff || (pending & PCI_EXP_DEVSTA_TRPND))
		return GU_RESULT(-EBUSY);
	if (read_bar(work, GU_GFW_CPUCTL_OFFSET, &cpuctl) || inaccessible(cpuctl) ||
	    !(cpuctl & GU_GFW_CPUCTL_HALTED) ||
	    read_bar(work, 0x001fa824, &lo) || inaccessible(lo) ||
	    read_bar(work, 0x001fa828, &hi) || inaccessible(hi) ||
	    read_bar(work, GU_FEAT_SS0_OFFSET, &ss0) || inaccessible(ss0) ||
	    read_bar(work, GU_FEAT_SS1_OFFSET, &ss1) || inaccessible(ss1) ||
	    read_bar(work, GU_FEAT_PLM_OFFSET, &plm) ||
	    (plm & 0xffff0000u) == 0xbadf0000u)
		return GU_RESULT(-ENODEV);
	dev_info(&work->pdev->dev,
		 "post-FLR SEC2 reset released, GSP halted, BME=0 pending=0 WPR2=%08x:%08x SS0=%08x SS1=%08x\n",
		 lo, hi, ss0, ss1);
	/* Both 610 and installed 615 _kgspBootGspRm reject preexisting WPR2
	 * on this cold dGPU path, even our old in-bounds FRTS reservation.
	 * kgspIsWpr2Up_TU102 uses HI.VAL != 0, not a LO/HI range comparison.
	 */
	if (hi >> 4) {
		dev_err(&work->pdev->dev, "post-FLR WPR2 remains enabled: lo=%#x hi=%#x; stock driver requires HI.VAL=0\n", lo, hi);
		return GU_RESULT(-EBUSY);
	}
	last_outcome.selectors_after_flr = ss0 == GU_SS0_UNLOCK && ss1 == GU_SS1_UNLOCK;
	dev_info(&work->pdev->dev, "selector post-FLR readback: unlock_matched=%u; recovery and unlock result are separate\n",
		 last_outcome.selectors_after_flr);
	work->quiesced = true;
#ifdef GU_UEFI_PORT
	GuQuiesced();
#endif
	return 0;
}

int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared)
{
	return request_scoped(&pdev->dev, &shared->gsp, GSP_FILE);
}

void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared)
{
	release_firmware(shared->gsp);
	shared->gsp = NULL;
}

int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared)
{
	struct unlock_transaction_work *work;
	int ret, cleanup, selector_result = 0;

	memset(&last_outcome, 0, sizeof(last_outcome));
	last_outcome.result = -ENOMEM;
	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;
	work->pdev = pci_dev_get(pdev);
	work->gsp_fw = shared->gsp;
	ret = unlock_transaction_assets(work);
	if (!ret)
		ret = unlock_transaction_claim(work);
	if (!ret)
		ret = unlock_transaction_layout(work);
	if (!ret)
		ret = unlock_transaction_dma(work);
	if (!ret)
		ret = unlock_transaction_gfw(work);
	if (!ret)
		ret = unlock_transaction_fwsec(work);
	if (!ret)
		ret = unlock_transaction_signature(work);	/* publish V67 */
	if (!ret)
		ret = unlock_transaction_boot(work);
	if (!ret)
		ret = unlock_transaction_signature(work);	/* restore stock */
	if (!ret) {
		selector_result = unlock_transaction_selector(work);
		/* -EIO here is a known readback mismatch, not an I/O transport
		 * failure (write_selector uses -ENODEV for the latter). Keep the
		 * failure, but do not skip FLR cleanup solely because of it. */
		if (selector_result != -EIO)
			ret = selector_result;
	}
	if (!ret)
		ret = unlock_transaction_reset(work);
	if (!ret && (selector_result || !last_outcome.selectors_before_flr ||
			!last_outcome.selectors_after_flr))
		ret = selector_result ? selector_result : -EIO;
	/* Restore stock only when the SEC2 consumer is proven halted. If its
	 * state is unknown, retain immutable inputs and require power off. */
	if (ret && work->signature_calls == 1) {
		if (!work->sec2_launched || work->sec2_halt_verified)
			unlock_transaction_signature(work);
		else
			dev_err(&pdev->dev, "Stock signature restore deferred: SEC2 HALT not verified; retain inputs until power off\n");
	}
	cleanup = unlock_transaction_unwind(work);
	if (!ret)
		ret = cleanup;
	last_outcome.firmware_started = work->firmware_started;
	last_outcome.recovered = work->quiesced && !work->retained && !cleanup;
#ifdef GU_UEFI_PORT
	/* Unmap/FreeBuffer can fail after engine quiescence. The compatibility
	 * layer latches retention; it must never leak through as result==0. */
	if (!GuSafeToReturn() || Gu.allocations || Gu.fault || Gu.retained) {
		last_outcome.recovered = 0;
		if (!ret)
			ret = -EIO;
	}
#endif
	last_outcome.result = ret;
	dev_info(&pdev->dev, "TRANSACTION OUTCOME: result=%d firmware_started=%u required_plm=%u all_plm=%u selectors_before_flr=%u selectors_after_flr=%u recovered=%u\n",
		ret, last_outcome.firmware_started, last_outcome.required_plm_verified,
		last_outcome.all_plm_verified, last_outcome.selectors_before_flr,
		last_outcome.selectors_after_flr, last_outcome.recovered);
	if (work->retained) {
		/* Firmware may still DMA into these buffers: keep them, the device
		 * reference and this module until the next power off. */
		dev_err(&pdev->dev, "firmware quiescence not proven; keeping DMA buffers until power off\n");
		__module_get(THIS_MODULE);
		return ret;
	}
	pci_dev_put(work->pdev);
	kfree(work);
	return ret;
}

MODULE_FIRMWARE(BOOTER_FILE);
MODULE_FIRMWARE(V67_FILE);
MODULE_FIRMWARE(GSP_FILE);
MODULE_FIRMWARE(BOOT_FILE);
MODULE_FIRMWARE(BOOT_DESC_FILE);

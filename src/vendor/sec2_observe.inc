/* SEC2 diagnostics, v0.4.6. Register offsets/names from NVIDIA 610.43.03
 * published/ampere/ga102/{dev_falcon_v4,dev_sec_pri,dev_fbif_v4,
 * dev_falcon_second_pri,dev_riscv_pri}.h. Raw values are not an AD103
 * authentication-error decoder. No IRQ clearing or TCM/PIO accesses.
 * v0.4.6 adds raw candidates requested by user: 0x823b00 has no confirmed
 * AD103 field definition; 0x8403c4 follows the standard Falcon reset-PLM
 * offset. Do not decode these candidates or use their values as gates. */
static int sec2_snapshot(struct unlock_transaction_work *work, const char *phase)
{
	static const struct { u32 offset; const char *name; } regs[] = {
		{0x840100,"CPUCTL"}, {0x840008,"IRQSTAT"},
		{0x840040,"FALCON_MAILBOX0"}, {0x840044,"FALCON_MAILBOX1"},
		{0x840080,"OS"}, {0x840084,"RM"}, {0x840094,"DEBUGINFO"},
		{0x840104,"BOOTVEC"}, {0x840108,"HWCFG"}, {0x8400f4,"HWCFG2"},
		{0x84010c,"DMACTL"}, {0x840118,"DMATRFCMD"},
		{0x840110,"DMATRFBASE"}, {0x840114,"DMATRFMOFFS"},
		{0x84011c,"DMATRFFBOFFS"}, {0x840128,"DMATRFBASE1"},
		{0x840600,"FBIF_TRANSCFG0"}, {0x840624,"FBIF_CTL"},
		{0x840804,"PSEC_MAILBOX0"}, {0x840808,"PSEC_MAILBOX1"},
		{0x84080c,"PSEC_MAILBOX2"}, {0x840810,"PSEC_MAILBOX3"},
		{0x841668,"BCR_CTRL"}, {0x841180,"MOD_SEL"},
		{0x841198,"BROM_CURR_UCODE_ID"}, {0x84119c,"BROM_ENGIDMASK"},
		{0x841210,"BROM_PARAADDR0"}, {0x823804,"FEAT_PLM"},
		{0x1fa7cc,"WPR_CFG_PLM"}, {0x9a0148,"FBPA_PLM"},
		{0x1fa7c4,"WPR_PLM"}, {0x1fa824,"WPR2_LOW"}, {0x1fa828,"WPR2_HIGH"},
        {0x823b00,"SOURCE_ENABLE_CANDIDATE_RAW"},
        {0x8403c4,"RESET_PLM_CANDIDATE_RAW"},
	};
	u32 values[ARRAY_SIZE(regs)], count = 0, i;
	int ret = 0;
	/* Capture before slow USB logging. Preserve protected-read sentinels. */
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = read_bar(work, regs[i].offset, &values[i]);
		if (ret) break;
		count++;
	}
	dev_info(&work->pdev->dev,"SEC2 snapshot: phase=%s samples=%u/%u error=%d\n",
		phase,count,(unsigned int)ARRAY_SIZE(regs),ret);
	for (i = 0; i < count; i++)
		dev_info(&work->pdev->dev,"SEC2 register: name=%s reg=%#x value=%#x sentinel_like=%u\n",
			regs[i].name,regs[i].offset,values[i],inaccessible(values[i]));
	return ret ? ret : (Gu.fault ? -EIO : 0);
}

static int sec2_observe_plm(struct unlock_transaction_work *work)
{
	struct { u32 wait_us, cpuctl, plm; } trace[32];
	u32 identity = 0, engine = 0, plm = 0, first = 0, samples = 0;
	u32 requested_wait_us = 0, used = 0, dropped = 0, transitions = 0;
	u32 previous_engine = 0, previous_plm = 0, i;
	bool observed_nonhalt = false, identity_ok = false;
	const char *reason = "io-error";
	int ret = -EIO, snapshot_ret;
	/* Gu.ticks measures requested Stall time, not wall-clock time. */
	ktime_t deadline = ktime_add_us(ktime_get(),5000000);
	for (;;) {
		identity_ok = false;
		if (diag_pci_dword(work,false,PCI_VENDOR_ID,&identity) || identity != GU_TARGET_PCI_ID) {
			ret = -ENODEV; reason = "pci-unavailable"; break;
		}
		identity_ok = true;
		if (read_bar(work,SEC2_BASE+0x100,&engine) || read_bar(work,work->plm_target,&plm))
			break;
		if (!samples) first = engine;
		if (samples && (engine != previous_engine || plm != previous_plm)) transitions++;
		samples++;
		if (samples <= 8 || engine != previous_engine || plm != previous_plm ||
		    requested_wait_us == 1000 || requested_wait_us == 10000 || requested_wait_us == 100000 ||
		    requested_wait_us == 1000000 || requested_wait_us == 5000000) {
			if (used < ARRAY_SIZE(trace)) {
				trace[used].wait_us=requested_wait_us; trace[used].cpuctl=engine; trace[used].plm=plm; used++;
			} else dropped++;
		}
		previous_engine=engine; previous_plm=plm;
		if (inaccessible(engine) || (plm & 0xffff0000u) == 0xbadf0000u) {
			ret=-ENODEV; reason="register-unavailable"; break;
		}
		if (!(engine & 0x10)) observed_nonhalt=true;
		if (plm == work->plm_expected) {
			ret=0; reason="plm-open"; break;
		}
		/* A first HALT sample does not distinguish delayed START from a
                 * completed failure. Observe once, without restarting, up to the
                 * existing budget. PLM remains the gate for selector writes. */
		if (ktime_compare(ktime_get(),deadline) >= 0) {
			ret=(engine & 0x10) ? -EIO : -ETIMEDOUT;
			reason=(engine & 0x10) ? "halted-without-plm" : "running-without-plm";
			break;
		}
		usleep_range(50,100); requested_wait_us+=50;
	}
	/* Sample failure state before emitting buffered polling history. Do not
         * read MMIO after device identity is lost. Actual transport errors keep
         * the existing fault latch and prevent further reads. */
	snapshot_ret=identity_ok ? sec2_snapshot(work,ret ? "observe-failure" : "plm-open") : 0;
	dev_info(&work->pdev->dev,
		"SEC2 observation: reason=%s first=%#x last=%#x plm=%#x samples=%u observed_nonhalt=%u transitions=%u requested_wait_us=%u trace_dropped=%u error=%d\n",
		reason,first,engine,plm,samples,observed_nonhalt,transitions,requested_wait_us,dropped,ret);
	for (i=0; i<used; i++)
		dev_info(&work->pdev->dev,"SEC2 trace: requested_wait_us=%u cpuctl=%#x plm=%#x\n",
			trace[i].wait_us,trace[i].cpuctl,trace[i].plm);
	/* Only a completed closed-target observation may request a bounded
	 * reset/reload. Running engines and transport failures never refire. */
	work->sec2_retryable = ret == -EIO && identity_ok && !snapshot_ret &&
		!Gu.fault && (engine & 0x10) && !inaccessible(engine) &&
		(plm & 0xffff0000u) != 0xbadf0000u && plm != work->plm_expected &&
		requested_wait_us >= 5000000;
	return ret ? ret : snapshot_ret;
}

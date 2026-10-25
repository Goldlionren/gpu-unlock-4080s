/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_UNLOCK_610_H
#define GPU_UNLOCK_UNLOCK_610_H

struct firmware;
struct pci_dev;

/* Single-device UEFI transaction evidence, independent of cleanup success.
 * This is not a performance result. Valid after gpu_unlock_610_unlock returns.
 * The local UEFI application permits only one transaction; not concurrent. */
struct gpu_unlock_610_outcome {
	int firmware_started;
	int required_plm_verified;
	int all_plm_verified;
	int selectors_before_flr;
	int selectors_after_flr;
	int recovered;
	int result;
};
const struct gpu_unlock_610_outcome *gpu_unlock_610_get_outcome(void);

/* Read-only firmware every unlock uses: loaded once, shared by concurrent unlocks. */
struct gpu_unlock_610_shared {
	const struct firmware *gsp;
};

/*
 * Load the shared firmware. pdev is any target card; the request leaves no
 * device resources behind on it. Returns 0 or a negative errno.
 */
int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared);

/* Release the shared firmware once every unlock using it has returned. */
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared);

/*
 * Attempt one cold, unbound board selected by the local UEFI profile. Returns
 * 0 only after selector readback before and after FLR and verified recovery;
 * otherwise a negative errno. An error does not imply that all earlier writes
 * were reverted or that the card remains locked; inspect the outcome and
 * platform containment state. Performance still needs Windows verification.
 */
int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared);

#endif

/* SPDX-License-Identifier: MIT */
#ifndef STAGE2_ADMISSION_H
#define STAGE2_ADMISSION_H
#include "core.h"
#include <Protocol/PciIo.h>
EFI_STATUS Stage2Reserve(TEXT *,EFI_HANDLE,EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_PCI_IO_PROTOCOL *);
EFI_STATUS Stage2Release(TEXT *,EFI_HANDLE,EFI_SYSTEM_TABLE *);
BOOLEAN Stage2ReservationHeld(void);
EFI_STATUS Stage2Recheck(EFI_HANDLE,EFI_SYSTEM_TABLE *);
#endif

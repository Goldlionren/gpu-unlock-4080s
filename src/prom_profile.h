#ifndef GU_PROM_PROFILE_H
#define GU_PROM_PROFILE_H
/* CAP_STAGE2_0002: full four-image chain [0,0x9a200) matches both original
 * ROM views at 0x9400 and 0xe9400. Hashes pin this board's actual bytes.
 * The captured 1 MiB prefix is not claimed to be the entire VBIOS storage. */
#define GU_PROM_BAR_OFFSET 0x300000U
#define GU_PROM_VIEW_BYTES 0x9a200U
#define GU_PROM_VIEW_SHA256 "efe79bd0be7cde01fb565f1667ad807f2a38d82736f42b429d4ceeb96fb6e9a5"
#define GU_PROM_CAPTURE_SHA256 "dd5a21fc1434583d3660de35f60799c2cce3b72cd5330dd1d7e5b042a0785f6f"
#endif

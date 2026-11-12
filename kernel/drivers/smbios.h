#ifndef ARCHOS_DRIVERS_SMBIOS_H
#define ARCHOS_DRIVERS_SMBIOS_H

#include <stdint.h>

/* SMBIOS: which machine this is, for the boot report (M1-04). Reads the
 * firmware's SMBIOS 3.x (preferred) or 2.x table through the HHDM. */

#define SMBIOS_STR_MAX 48   /* each string, NUL included; longer ones are cut */

typedef struct {
    char bios_vendor[SMBIOS_STR_MAX];    /* type 0 */
    char bios_version[SMBIOS_STR_MAX];
    char bios_date[SMBIOS_STR_MAX];
    char maker[SMBIOS_STR_MAX];          /* type 1: manufacturer */
    char model[SMBIOS_STR_MAX];          /* type 1: product name */
    char model_version[SMBIOS_STR_MAX];  /* type 1: version (ThinkPads put the model name here) */
    uint8_t major, minor;                /* SMBIOS version */
} SmbiosInfo;

/* Fill out from the entry points (physical addresses, 0 = absent). Missing
 * strings stay empty. Returns 0, or -1 when no valid table was found. */
int smbios_read(uint64_t entry32_phys, uint64_t entry64_phys, SmbiosInfo *out);

#endif /* ARCHOS_DRIVERS_SMBIOS_H */

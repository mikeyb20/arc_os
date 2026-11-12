/* arc_os — SMBIOS reader (M1-04)
 *
 * Owns: finding the SMBIOS structure table from the 3.x ("_SM3_") or 2.x
 * ("_SM_") entry point and copying the BIOS (type 0) and system (type 1)
 * strings. Every read stays inside the table length the entry point gives,
 * and strings are cut to printable ASCII, so a broken table yields empty
 * fields, not a fault. Locks: none (boot only). */

#include "drivers/smbios.h"
#include "mm/vmm.h"
#include "lib/mem.h"

#define SMBIOS_TABLE_MAX   (1u << 20)   /* a sane table is a few KiB */
#define SMBIOS_TYPE_BIOS   0
#define SMBIOS_TYPE_SYSTEM 1
#define SMBIOS_TYPE_END    127

static int checksum_ok(const uint8_t *p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    return sum == 0;
}

/* Copy string number idx (1-based) of the structure whose strings start at
 * strs and must end before end. */
static void copy_string(const uint8_t *strs, const uint8_t *end, uint8_t idx, char *out) {
    out[0] = '\0';
    if (idx == 0) return;
    const uint8_t *p = strs;
    for (uint8_t n = 1; n < idx; n++) {
        while (p < end && *p != 0) p++;
        if (p >= end) return;
        p++;
    }
    uint32_t len = 0;
    while (p < end && *p != 0 && len + 1 < SMBIOS_STR_MAX) {
        out[len++] = (*p >= 0x20 && *p < 0x7F) ? (char)*p : '?';
        p++;
    }
    out[len] = '\0';
    /* Firmware often pads with spaces */
    while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
}

static void walk(const uint8_t *table, uint32_t len, SmbiosInfo *out) {
    const uint8_t *p = table, *end = table + len;
    while (p + 4 <= end) {
        uint8_t type = p[0], flen = p[1];
        if (flen < 4 || p + flen > end) return;
        const uint8_t *strs = p + flen;
        /* Strings end at a double NUL */
        const uint8_t *next = strs;
        while (next + 1 < end && !(next[0] == 0 && next[1] == 0)) next++;
        if (next + 1 >= end) return;
        next += 2;

        if (type == SMBIOS_TYPE_BIOS && flen >= 9) {
            copy_string(strs, next, p[4], out->bios_vendor);
            copy_string(strs, next, p[5], out->bios_version);
            copy_string(strs, next, p[8], out->bios_date);
        } else if (type == SMBIOS_TYPE_SYSTEM && flen >= 7) {
            copy_string(strs, next, p[4], out->maker);
            copy_string(strs, next, p[5], out->model);
            copy_string(strs, next, p[6], out->model_version);
        } else if (type == SMBIOS_TYPE_END) {
            return;
        }
        p = next;
    }
}

int smbios_read(uint64_t entry32_phys, uint64_t entry64_phys, SmbiosInfo *out) {
    memset(out, 0, sizeof(*out));
    uint64_t hhdm = vmm_get_hhdm_offset();   /* phys + offset = virt */
    if (entry64_phys != 0) {
        const uint8_t *e = (const uint8_t *)(uintptr_t)(hhdm + entry64_phys);
        if (memcmp(e, "_SM3_", 5) == 0 && e[6] >= 24 && checksum_ok(e, e[6])) {
            uint32_t len;
            uint64_t addr;
            memcpy(&len, e + 0x0C, 4);
            memcpy(&addr, e + 0x10, 8);
            if (len > 0 && len <= SMBIOS_TABLE_MAX) {
                out->major = e[7];
                out->minor = e[8];
                walk((const uint8_t *)(uintptr_t)(hhdm + addr), len, out);
                return 0;
            }
        }
    }
    if (entry32_phys != 0) {
        const uint8_t *e = (const uint8_t *)(uintptr_t)(hhdm + entry32_phys);
        if (memcmp(e, "_SM_", 4) == 0 && e[5] >= 31 && checksum_ok(e, e[5]) && memcmp(e + 0x10, "_DMI_", 5) == 0) {
            uint16_t len;
            uint32_t addr;
            memcpy(&len, e + 0x16, 2);
            memcpy(&addr, e + 0x18, 4);
            if (len > 0) {
                out->major = e[6];
                out->minor = e[7];
                walk((const uint8_t *)(uintptr_t)(hhdm + addr), len, out);
                return 0;
            }
        }
    }
    return -1;
}

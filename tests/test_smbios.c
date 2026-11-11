/* arc_os — Host-side tests for kernel/drivers/smbios.c (M1-04) */

#define _GNU_SOURCE   /* MAP_32BIT: the 2.x entry point holds a 32-bit table address */
#include "test_framework.h"
#include <stdint.h>
#include <sys/mman.h>

/* Physical address = host pointer: the HHDM offset is 0 */
#define ARCHOS_MM_VMM_H
static uint64_t vmm_get_hhdm_offset(void) {
    return 0;
}
#define ARCHOS_LIB_MEM_H

#include "../kernel/drivers/smbios.c"

static uint8_t table[512];
static uint8_t eps64[24];
static uint8_t eps32[31];

static void fix_checksum(uint8_t *p, uint32_t len, uint32_t at) {
    uint8_t sum = 0;
    p[at] = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    p[at] = (uint8_t)(0 - sum);
}

/* Type 0 and type 1 structures, then the end marker; returns the length */
static uint32_t build_table(void) {
    uint8_t *p = table;
    memset(table, 0, sizeof(table));
    /* Type 0, 0x12 bytes: vendor = 1, version = 2, date = 3 */
    p[0] = 0;
    p[1] = 0x12;
    p[4] = 1;
    p[5] = 2;
    p[8] = 3;
    p += 0x12;
    const char s0[] = "LENOVO\0N2HET77W (1.60 )   \0"
                      "05/23/2024\0";
    memcpy(p, s0, sizeof(s0));   /* includes the final NUL: double NUL */
    p += sizeof(s0);
    /* Type 1, 0x1B bytes: maker = 1, product = 2, version = 3 */
    p[0] = 1;
    p[1] = 0x1B;
    p[4] = 1;
    p[5] = 2;
    p[6] = 3;
    p += 0x1B;
    const char s1[] = "LENOVO\0"
                      "20XW\x01\0ThinkPad X1 Carbon\0";
    memcpy(p, s1, sizeof(s1));
    p += sizeof(s1);
    /* Type 127 end, no strings */
    p[0] = 127;
    p[1] = 4;
    p += 6;
    return (uint32_t)(p - table);
}

static void build_eps64(uint32_t len) {
    memset(eps64, 0, sizeof(eps64));
    memcpy(eps64, "_SM3_", 5);
    eps64[6] = 24;
    eps64[7] = 3;
    eps64[8] = 4;
    memcpy(eps64 + 0x0C, &len, 4);
    uint64_t addr = (uint64_t)(uintptr_t)table;
    memcpy(eps64 + 0x10, &addr, 8);
    fix_checksum(eps64, 24, 5);
}

TEST(reads_type0_and_type1) {
    build_eps64(build_table());
    SmbiosInfo info;
    ASSERT_EQ(smbios_read(0, (uint64_t)(uintptr_t)eps64, &info), 0);
    ASSERT_EQ(info.major, 3);
    ASSERT_EQ(info.minor, 4);
    ASSERT_STR_EQ(info.bios_vendor, "LENOVO");
    ASSERT_STR_EQ(info.bios_version, "N2HET77W (1.60 )");   /* trailing spaces cut */
    ASSERT_STR_EQ(info.bios_date, "05/23/2024");
    ASSERT_STR_EQ(info.maker, "LENOVO");
    ASSERT_STR_EQ(info.model, "20XW?");                     /* control byte replaced */
    ASSERT_STR_EQ(info.model_version, "ThinkPad X1 Carbon");
    return 0;
}

TEST(bad_checksum_falls_back_to_32bit) {
    uint32_t len = build_table();
    build_eps64(len);
    eps64[5]++;   /* break the 64-bit checksum */
    memset(eps32, 0, sizeof(eps32));
    memcpy(eps32, "_SM_", 4);
    eps32[5] = 31;
    eps32[6] = 2;
    eps32[7] = 8;
    memcpy(eps32 + 0x10, "_DMI_", 5);
    uint16_t len16 = (uint16_t)len;
    memcpy(eps32 + 0x16, &len16, 2);
    /* A copy of the table below 4 GiB, where a 2.x entry point can reach it */
    uint8_t *low = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    ASSERT_TRUE(low != MAP_FAILED);
    memcpy(low, table, len);
    uint32_t addr = (uint32_t)(uintptr_t)low;
    memcpy(eps32 + 0x18, &addr, 4);
    fix_checksum(eps32, 31, 4);
    SmbiosInfo info;
    int rc = smbios_read((uint64_t)(uintptr_t)eps32, (uint64_t)(uintptr_t)eps64, &info);
    munmap(low, 4096);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(info.major, 2);
    ASSERT_STR_EQ(info.model, "20XW?");
    return 0;
}

TEST(no_entry_point) {
    SmbiosInfo info;
    memset(&info, 'x', sizeof(info));
    ASSERT_EQ(smbios_read(0, 0, &info), -1);
    ASSERT_STR_EQ(info.maker, "");
    eps64[0] = 'X';   /* wrong anchor */
    ASSERT_EQ(smbios_read(0, (uint64_t)(uintptr_t)eps64, &info), -1);
    return 0;
}

/* A table cut short (string set without its double NUL, a structure longer
 * than the table) leaves fields empty and reads nothing past the length */
TEST(truncated_table_is_safe) {
    uint32_t len = build_table();
    (void)len;
    build_eps64(0x12 + 10);   /* ends inside the type 0 strings */
    SmbiosInfo info;
    ASSERT_EQ(smbios_read(0, (uint64_t)(uintptr_t)eps64, &info), 0);
    ASSERT_STR_EQ(info.bios_vendor, "");
    ASSERT_STR_EQ(info.maker, "");
    table[1] = 0xFF;   /* formatted length past the end */
    build_eps64(0x12);
    ASSERT_EQ(smbios_read(0, (uint64_t)(uintptr_t)eps64, &info), 0);
    ASSERT_STR_EQ(info.bios_vendor, "");
    return 0;
}

TEST(string_index_past_the_set) {
    build_table();
    table[5] = 9;   /* BIOS version = string 9; there are 3 */
    build_eps64(sizeof(table));
    SmbiosInfo info;
    ASSERT_EQ(smbios_read(0, (uint64_t)(uintptr_t)eps64, &info), 0);
    ASSERT_STR_EQ(info.bios_vendor, "LENOVO");
    ASSERT_STR_EQ(info.bios_version, "");
    return 0;
}

TestCase smbios_tests[] = {
    TEST_ENTRY(reads_type0_and_type1),   TEST_ENTRY(bad_checksum_falls_back_to_32bit), TEST_ENTRY(no_entry_point),
    TEST_ENTRY(truncated_table_is_safe), TEST_ENTRY(string_index_past_the_set),
};
int smbios_test_count = sizeof(smbios_tests) / sizeof(smbios_tests[0]);

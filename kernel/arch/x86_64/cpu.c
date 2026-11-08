/* arc_os — CPU features, the x86-64-v3 floor and core types (see cpu.h). */

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/cpuid.h"
#include "lib/mem.h"
#include "lib/string.h"

static CpuFeatures features;

void cpu_features_init(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    features.max_leaf = a;
    memcpy(features.vendor + 0, &b, 4);
    memcpy(features.vendor + 4, &d, 4);
    memcpy(features.vendor + 8, &c, 4);
    features.vendor[12] = '\0';

    cpuid(1, 0, &a, &b, &features.l1_ecx, &features.l1_edx);
    if (features.max_leaf >= 7) cpuid(7, 0, &a, &features.l7_ebx, &features.l7_ecx, &features.l7_edx);

    cpuid(0x80000000, 0, &features.max_ext_leaf, &b, &c, &d);
    if (features.max_ext_leaf >= 0x80000001) cpuid(0x80000001, 0, &a, &b, &features.ext1_ecx, &features.ext1_edx);
    if (features.max_ext_leaf >= 0x80000007) cpuid(0x80000007, 0, &a, &b, &c, &features.ext7_edx);
}

const CpuFeatures *cpu_features(void) {
    return &features;
}

/* x86-64-v3 = v2 (CMPXCHG16B, LAHF-SAHF, POPCNT, SSE3, SSE4.1, SSE4.2, SSSE3)
 * plus AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE, XSAVE. Baseline SSE2 is
 * implied by long mode. */
enum { W_L1_ECX, W_L7_EBX, W_EXT1_ECX };

/* clang-format off */
static const struct {
    const char *name;
    uint8_t     word;
    uint8_t     bit;
} floor_features[] = {
    { "SSE3",   W_L1_ECX,   0 },
    { "SSSE3",  W_L1_ECX,   9 },
    { "FMA",    W_L1_ECX,   12 },
    { "CX16",   W_L1_ECX,   13 },
    { "SSE4.1", W_L1_ECX,   19 },
    { "SSE4.2", W_L1_ECX,   20 },
    { "MOVBE",  W_L1_ECX,   22 },
    { "POPCNT", W_L1_ECX,   23 },
    { "XSAVE",  W_L1_ECX,   26 },
    { "AVX",    W_L1_ECX,   28 },
    { "F16C",   W_L1_ECX,   29 },
    { "BMI1",   W_L7_EBX,   3 },
    { "AVX2",   W_L7_EBX,   5 },
    { "BMI2",   W_L7_EBX,   8 },
    { "LAHF",   W_EXT1_ECX, 0 },
    { "LZCNT",  W_EXT1_ECX, 5 },
};
/* clang-format on */

int cpu_floor_missing(char *buf, size_t size) {
    int missing = 0;
    size_t pos = 0;
    if (size > 0) buf[0] = '\0';
    for (size_t i = 0; i < sizeof(floor_features) / sizeof(floor_features[0]); i++) {
        uint32_t word = floor_features[i].word == W_L1_ECX   ? features.l1_ecx
                        : floor_features[i].word == W_L7_EBX ? features.l7_ebx
                                                             : features.ext1_ecx;
        if (word & (1u << floor_features[i].bit)) continue;
        missing++;
        const char *n = floor_features[i].name;
        size_t len = strlen(n);
        if (pos + len + 2 > size) continue;    /* counted, but no room to name it */
        if (pos > 0) buf[pos++] = ' ';
        memcpy(buf + pos, n, len);
        pos += len;
        buf[pos] = '\0';
    }
    return missing;
}

#define CPUID7_EDX_HYBRID (1u << 15)

uint8_t cpu_core_type_detect(void) {
    if (features.max_leaf < 0x1A || !(features.l7_edx & CPUID7_EDX_HYBRID)) return CPU_CORE_UNKNOWN;
    uint32_t a, b, c, d;
    cpuid(0x1A, 0, &a, &b, &c, &d);
    return (uint8_t)(a >> 24);
}

const char *cpu_core_type_name(uint8_t type) {
    switch (type) {
    case CPU_CORE_CORE:
        return "P";
    case CPU_CORE_ATOM:
        return "E";
    default:
        return "-";
    }
}

uint64_t cpu_hwcap2(void) {
    return 0;
}

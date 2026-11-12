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
    if (features.max_leaf >= 6) cpuid(6, 0, &features.l6_eax, &b, &c, &d);
    if (features.max_leaf >= 7) {
        cpuid(7, 0, &a, &features.l7_ebx, &features.l7_ecx, &features.l7_edx);
        if (a >= 1) cpuid(7, 1, &features.l7s1_eax, &b, &c, &d);
    }

    cpuid(0x80000000, 0, &features.max_ext_leaf, &b, &c, &d);
    if (features.max_ext_leaf >= 0x80000001) cpuid(0x80000001, 0, &a, &b, &features.ext1_ecx, &features.ext1_edx);
    if (features.max_ext_leaf >= 0x80000007) cpuid(0x80000007, 0, &a, &b, &c, &features.ext7_edx);
}

const CpuFeatures *cpu_features(void) {
    return &features;
}

void cpu_brand(char *buf, size_t size) {
    char raw[49];
    if (size == 0) return;
    buf[0] = '\0';
    if (features.max_ext_leaf < 0x80000004) return;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t r[4];
        cpuid(0x80000002 + i, 0, &r[0], &r[1], &r[2], &r[3]);
        memcpy(raw + 16 * i, r, 16);
    }
    raw[48] = '\0';
    const char *p = raw;
    while (*p == ' ') p++;   /* Intel right-aligns the brand string */
    strncpy(buf, p, size - 1);
    buf[size - 1] = '\0';
}

void cpu_signature(uint32_t *family, uint32_t *model, uint32_t *stepping) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    uint32_t fam = (a >> 8) & 0xF, mod = (a >> 4) & 0xF;
    if (fam == 0xF) fam += (a >> 20) & 0xFF;
    if (fam == 0x6 || fam >= 0xF) mod |= ((a >> 16) & 0xF) << 4;
    *family = fam;
    *model = mod;
    *stepping = a & 0xF;
}

/* x86-64-v3 = v2 (CMPXCHG16B, LAHF-SAHF, POPCNT, SSE3, SSE4.1, SSE4.2, SSSE3)
 * plus AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE, XSAVE. Baseline SSE2 is
 * implied by long mode. */
enum { W_L1_ECX, W_L7_EBX, W_EXT1_ECX, W_L7_EDX, W_EXT1_EDX, W_EXT7_EDX, W_L6_EAX, W_L7S1_EAX };

typedef struct {
    const char *name;
    uint8_t     word;
    uint8_t     bit;
} FeatureBit;

/* clang-format off */
static const FeatureBit floor_features[] = {
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

/* Features the boot report lists when present (M1-04) */
static const FeatureBit report_features[] = {
    { "AVX2",         W_L7_EBX,   5 },
    { "FMA",          W_L1_ECX,   12 },
    { "BMI2",         W_L7_EBX,   8 },
    { "XSAVE",        W_L1_ECX,   26 },
    { "AVX512F",      W_L7_EBX,   16 },
    { "AVX-VNNI",     W_L7S1_EAX, 4 },
    { "x2APIC",       W_L1_ECX,   21 },
    { "TSC-DEADLINE", W_L1_ECX,   24 },
    { "INVTSC",       W_EXT7_EDX, 8 },
    { "MWAIT",        W_L1_ECX,   3 },
    { "HWP",          W_L6_EAX,   7 },
    { "HYBRID",       W_L7_EDX,   15 },
    { "SMEP",         W_L7_EBX,   7 },
    { "SMAP",         W_L7_EBX,   20 },
    { "NX",           W_EXT1_EDX, 20 },
    { "1G-PAGES",     W_EXT1_EDX, 26 },
    { "RDRAND",       W_L1_ECX,   30 },
};
/* clang-format on */

static uint32_t feature_word(uint8_t w) {
    switch (w) {
    case W_L1_ECX:
        return features.l1_ecx;
    case W_L7_EBX:
        return features.l7_ebx;
    case W_EXT1_ECX:
        return features.ext1_ecx;
    case W_L7_EDX:
        return features.l7_edx;
    case W_EXT1_EDX:
        return features.ext1_edx;
    case W_EXT7_EDX:
        return features.ext7_edx;
    case W_L6_EAX:
        return features.l6_eax;
    default:
        return features.l7s1_eax;
    }
}

/* Write the names of the features in table whose bit equals `present`,
 * space separated, into buf. Returns how many matched. */
static int list_features(const FeatureBit *table, size_t count, int present, char *buf, size_t size) {
    int matched = 0;
    size_t pos = 0;
    if (size > 0) buf[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        int has = (feature_word(table[i].word) >> table[i].bit) & 1;
        if (has != present) continue;
        matched++;
        const char *n = table[i].name;
        size_t len = strlen(n);
        if (pos + len + 2 > size) continue;    /* counted, but no room to name it */
        if (pos > 0) buf[pos++] = ' ';
        memcpy(buf + pos, n, len);
        pos += len;
        buf[pos] = '\0';
    }
    return matched;
}

int cpu_floor_missing(char *buf, size_t size) {
    return list_features(floor_features, sizeof(floor_features) / sizeof(floor_features[0]), 0, buf, size);
}

void cpu_feature_names(char *buf, size_t size) {
    list_features(report_features, sizeof(report_features) / sizeof(report_features[0]), 1, buf, size);
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

/* arc_os — System V initial process stack (see exec_stack.h). */

#include "proc/exec_stack.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "uapi/auxvec.h"
#include "uapi/errno.h"

#define EXEC_PLATFORM "x86_64"
#define EXEC_PAGE_SIZE 4096
#define AUX_PAIRS      19     /* entries written below, AT_NULL included */

void exec_strings_init(ExecStrings *s) {
    s->argc = 0;
    s->envc = 0;
    s->used = 0;
}

size_t exec_strings_cost(const ExecStrings *s) {
    return (size_t)s->used + 8u * ((size_t)s->argc + s->envc + 2);
}

size_t exec_strings_room(const ExecStrings *s) {
    size_t cost = exec_strings_cost(s) + 8;   /* the new entry's pointer */
    if (s->argc + s->envc >= EXEC_MAX_PTRS || cost >= EXEC_ARG_MAX) return 0;
    return EXEC_ARG_MAX - cost;
}

void exec_strings_commit(ExecStrings *s, size_t len, int is_env) {
    s->off[s->argc + s->envc] = s->used;
    s->used += (uint32_t)len;
    if (is_env) s->envc++;
    else s->argc++;
}

int exec_strings_add(ExecStrings *s, const char *str, int is_env) {
    size_t len = strlen(str) + 1;
    if (len > exec_strings_room(s)) return -E2BIG;
    memcpy(s->data + s->used, str, len);
    exec_strings_commit(s, len, is_env);
    return 0;
}

uint64_t exec_build_stack(uint8_t *buf, size_t buf_size, uint64_t stack_top,
                          const ExecStrings *s, const char *execfn,
                          const ExecAux *aux) {
    uint64_t base = stack_top - buf_size;    /* user address of buf[0] */
    uint64_t pos = stack_top;
#define AT(uaddr) (buf + ((uaddr) - base))

    size_t fn_len = strlen(execfn) + 1;
    size_t plat_len = sizeof(EXEC_PLATFORM);
    size_t words = 1 + (s->argc + 1) + (s->envc + 1) + 2 * AUX_PAIRS;
    size_t need = 8 + fn_len + s->used + plat_len + 16 + 16 + 8 + 8 * words;
    if (need > buf_size) return 0;

    /* Top word stays zero (as on Linux), then the strings */
    pos -= 8;
    memset(AT(pos), 0, 8);
    pos -= fn_len;
    uint64_t execfn_addr = pos;
    memcpy(AT(pos), execfn, fn_len);
    pos -= s->used;
    uint64_t strings_addr = pos;
    memcpy(AT(pos), s->data, s->used);
    pos -= plat_len;
    uint64_t platform_addr = pos;
    memcpy(AT(pos), EXEC_PLATFORM, plat_len);
    pos = (pos - 16) & ~15ULL;
    uint64_t random_addr = pos;
    memcpy(AT(pos), aux->random, 16);

    /* Pointer block: argc ... AT_NULL, with RSP (its start) 16-byte aligned */
    pos -= 8 * words;
    pos &= ~15ULL;
    uint64_t rsp = pos;
    uint64_t *w = (uint64_t *)AT(pos);
#undef AT

    *w++ = s->argc;
    for (uint32_t i = 0; i < s->argc; i++) *w++ = strings_addr + s->off[i];
    *w++ = 0;
    for (uint32_t i = 0; i < s->envc; i++) *w++ = strings_addr + s->off[s->argc + i];
    *w++ = 0;

    const uint64_t auxv[AUX_PAIRS][2] = {
        {AT_PHDR, aux->phdr},
        {AT_PHENT, aux->phent},
        {AT_PHNUM, aux->phnum},
        {AT_PAGESZ, EXEC_PAGE_SIZE},
        {AT_BASE, 0},               /* static programs have no interpreter */
        {AT_FLAGS, 0},
        {AT_ENTRY, aux->entry},
        {AT_UID, aux->uid},
        {AT_EUID, aux->euid},
        {AT_GID, aux->gid},
        {AT_EGID, aux->egid},
        {AT_SECURE, aux->secure},
        {AT_RANDOM, random_addr},
        {AT_HWCAP, aux->hwcap},
        {AT_HWCAP2, aux->hwcap2},
        {AT_MINSIGSTKSZ, aux->minsigstksz},
        {AT_EXECFN, execfn_addr},
        {AT_PLATFORM, platform_addr},
        {AT_NULL, 0},
    };
    for (int i = 0; i < AUX_PAIRS; i++) {
        *w++ = auxv[i][0];
        *w++ = auxv[i][1];
    }
    return rsp;
}

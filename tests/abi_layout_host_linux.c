/* arc_os — the host's side of the ABI layout test (M0-05), from the build
 * host's Linux uapi headers only (no glibc header here: they clash). Items the
 * uapi headers lack (the _G ones) come from glibc in test_abi_layout.c. */

#include <stddef.h>
#include <asm/unistd_64.h>
#include <asm/errno.h>
#include <asm/fcntl.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <asm/stat.h>
#include <linux/time_types.h>
#include <asm/signal.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>
#include <asm/siginfo.h>
#include <linux/signal.h>
#include <linux/uio.h>
#include <asm/termbits.h>
#include <asm/ioctls.h>
#include <asm-generic/termios.h>
#include <linux/poll.h>
#include <linux/in.h>
#include <linux/un.h>
#include <linux/wait.h>
#include <linux/auxvec.h>
#include <asm/prctl.h>
#include <asm/mman.h>
#include <linux/mman.h>
#include "abi_layout.h"

typedef struct stat               abi_stat;       /* asm/stat.h: the kernel's */
typedef struct __kernel_timespec  abi_timespec;
typedef struct sigaction          abi_sigaction;  /* asm/signal.h: the kernel's */
typedef struct _fpx_sw_bytes abi_fpx_sw_bytes;
typedef struct _fpstate abi_fpstate;    /* _fpstate_64 */
typedef struct sigcontext abi_sigcontext;
typedef stack_t abi_stack;
typedef struct ucontext abi_ucontext;
typedef siginfo_t abi_siginfo;
typedef struct iovec              abi_iovec;
typedef struct winsize            abi_winsize;
typedef struct termios            abi_termios;    /* asm/termbits.h: the kernel's */
typedef struct pollfd             abi_pollfd;
typedef struct sockaddr_in        abi_sockaddr_in;
typedef struct sockaddr_un        abi_sockaddr_un;

#define ABI_NR(name)       { "__NR_" #name, __NR_##name },
#define ABI_VAL(name)      { #name, (long)(name) },
#define ABI_SIZE(type)     { "sizeof " #type, (long)sizeof(type) },
#define ABI_OFF(type, f)   { #type "." #f, (long)offsetof(type, f) },
#define ABI_VAL_G(name)
#define ABI_OFF_G(type, f)

const AbiItem abi_host_linux[] = {
#include "abi_layout_list.h"
    { NULL, 0 }
};

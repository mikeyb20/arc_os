/* arc_os — our side of the ABI layout test (M0-05): sizes, offsets and values
 * from kernel/include/uapi, compiled with the host compiler. Only uapi headers
 * are included here, so their struct names cannot clash with the host's. */

#include <stddef.h>
#include "abi_layout.h"
#include "uapi/unistd.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"
#include "uapi/stat.h"
#include "uapi/dirent.h"
#include "uapi/time.h"
#include "uapi/signal.h"
#include "uapi/sigcontext.h"
#include "uapi/uio.h"
#include "uapi/termios.h"
#include "uapi/poll.h"
#include "uapi/socket.h"
#include "uapi/wait.h"
#include "uapi/auxvec.h"
#include "uapi/prctl.h"

typedef struct stat               abi_stat;
typedef struct linux_dirent64     abi_dirent64;
typedef struct timespec           abi_timespec;
typedef struct kernel_sigaction   abi_sigaction;
typedef struct _fpx_sw_bytes abi_fpx_sw_bytes;
typedef struct _fpstate abi_fpstate;
typedef struct sigcontext abi_sigcontext;
typedef stack_t abi_stack;
typedef struct ucontext abi_ucontext;
typedef siginfo_t abi_siginfo;
typedef struct iovec              abi_iovec;
typedef struct winsize            abi_winsize;
typedef struct termios            abi_termios;
typedef struct pollfd             abi_pollfd;
typedef struct sockaddr_in        abi_sockaddr_in;
typedef struct sockaddr_un        abi_sockaddr_un;

#define ABI_NR(name)       { "__NR_" #name, __NR_##name },
#define ABI_VAL(name)      { #name, (long)(name) },
#define ABI_SIZE(type)     { "sizeof " #type, (long)sizeof(type) },
#define ABI_OFF(type, f)   { #type "." #f, (long)offsetof(type, f) },
#define ABI_VAL_G(name)    { #name, (long)(name) },
#define ABI_OFF_G(type, f) { #type "." #f, (long)offsetof(type, f) },

const AbiItem abi_ours[] = {
#include "abi_layout_list.h"
    { NULL, 0 }
};

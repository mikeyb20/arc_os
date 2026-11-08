#ifndef ARCHOS_UAPI_SOCKET_H
#define ARCHOS_UAPI_SOCKET_H

#include <stdint.h>

/* Socket address structs and constants, Linux x86_64 values (ADR-0001). */

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct in_addr {
    uint32_t s_addr;           /* network byte order */
};

struct sockaddr_in {
    uint16_t       sin_family; /* AF_INET */
    uint16_t       sin_port;   /* network byte order */
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

#define ARC_UNIX_PATH_MAX 108

struct sockaddr_un {
    uint16_t sun_family;       /* AF_UNIX */
    char     sun_path[ARC_UNIX_PATH_MAX];
};

_Static_assert(sizeof(struct sockaddr) == 16, "struct sockaddr must be 16 bytes");
_Static_assert(sizeof(struct sockaddr_in) == 16, "struct sockaddr_in must be 16 bytes");
_Static_assert(__builtin_offsetof(struct sockaddr_in, sin_port) == 2, "sin_port offset");
_Static_assert(__builtin_offsetof(struct sockaddr_in, sin_addr) == 4, "sin_addr offset");
_Static_assert(sizeof(struct sockaddr_un) == 110, "struct sockaddr_un must be 110 bytes");
_Static_assert(__builtin_offsetof(struct sockaddr_un, sun_path) == 2, "sun_path offset");

#endif /* ARCHOS_UAPI_SOCKET_H */

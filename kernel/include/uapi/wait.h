#ifndef ARCHOS_UAPI_WAIT_H
#define ARCHOS_UAPI_WAIT_H

/* wait4 options, Linux values (ADR-0001).
 *
 * Status word written by wait4 (Linux encoding):
 *   exited:   (code & 0xff) << 8
 *   signaled: signal number in bits 0-6
 *   stopped:  (signal << 8) | 0x7f            (reported only with WUNTRACED) */

#define WNOHANG    0x00000001
#define WUNTRACED  0x00000002
#define WCONTINUED 0x00000008

#endif /* ARCHOS_UAPI_WAIT_H */

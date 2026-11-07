# M1-18 — AF_UNIX sockets and poll/select

| Size | Tier | Lane | Needs | Unblocks |
|---|---|---|---|---|
| M | R | K | M1-06, M1-11 | M1-20, M2-04 |

**Goal.** Local stream sockets for the `arc-llm` service (ADR-0012), and one poll mechanism that every waitable
file uses.

**Read first.**
- `docs/decisions/0012-model-service-api.md`
- `kernel/net/socket.c`, `socket.h`, `kernel/fs/pipe.c`, `kernel/drivers/tty.c`, `kernel/proc/waitqueue.c`,
  Notes of M1-11

**Build.**
1. File op `poll(file, table)`: returns the ready mask and registers wait queues. Implemented for pipes, tty,
   TCP/UDP/UNIX sockets and regular files (always ready).
2. `poll`, `ppoll`, `select`, `pselect6` (with signal mask), timeouts from M1-06. `epoll` only if the inventory
   needs it; otherwise later.
3. `AF_UNIX` `SOCK_STREAM`: socket, bind (a filesystem path creates a socket inode; abstract names start with a
   NUL byte), listen, accept/accept4, connect, `sendto`/`recvfrom`/`sendmsg`/`recvmsg` (no fd passing yet),
   shutdown, getsockname/getpeername, socketpair, non-blocking mode, `MSG_NOSIGNAL`, `SIGPIPE` on a closed peer.
4. Tests: an echo client and server move 1 MiB in chunks; poll on mixed fds with a timeout; select on tty input;
   EOF, `EPIPE` and `SIGPIPE` behave as Linux.

**Done when.**
- [ ] Tests pass.

**Watch out.**
- One wait-queue registration per (file, poll table) entry; free them all when poll returns, or wakeups touch freed
  memory.

**Notes.**
-

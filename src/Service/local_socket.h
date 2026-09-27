#ifndef OFFS_LOCAL_SOCKET_H
#define OFFS_LOCAL_SOCKET_H

/*
 * Well-known local RPC endpoint shared by the daemon and the CLI.
 *
 * Single compile-time definition of the local socket path both binaries
 * agree on. liboffs' platform_local layer maps the path to the platform's
 * local IPC transport: on POSIX it is used directly as a UNIX-domain
 * socket path; on Windows the basename is encoded into a named pipe
 * ("\\.\pipe\liboffs-<basename>"), so the literal value stays the same
 * across platforms while the transport differs. Should the two platforms
 * ever need different well-known names, this is the divergence point.
 */

#ifdef _WIN32
#define OFFS_LOCAL_SOCKET_PATH "/var/run/offs.sock"
#else
#define OFFS_LOCAL_SOCKET_PATH "/var/run/offs.sock"
#endif

#endif /* OFFS_LOCAL_SOCKET_H */
/* libkaillera - UDP sockets and time, Windows and POSIX. LGPL-2.1-or-later, see LICENSE. */
#ifndef KAILLERA_KC_NET_H
#define KAILLERA_KC_NET_H

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET kc_sock;
#define KC_BAD_SOCK INVALID_SOCKET
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
typedef int kc_sock;
#define KC_BAD_SOCK (-1)
#endif

int kc_net_init(void);	/* WSAStartup once on Windows */
kc_sock kc_net_open(void);	/* non-blocking UDP socket, bound to any port */
void kc_net_close(kc_sock s);
int kc_net_resolve(const char *host, int port, struct sockaddr_in *out);
int kc_net_send(kc_sock s, const struct sockaddr_in *to, const void *data, int len);
/* Returns the length received, 0 if nothing is waiting, -1 on error. */
int kc_net_recv(kc_sock s, void *buf, int size, struct sockaddr_in *from);
/* Waits up to timeout_ms for the socket to become readable: 1 readable, 0 timeout. */
int kc_net_wait(kc_sock s, int timeout_ms);
uint32_t kc_net_ms(void);	/* monotonic milliseconds */

#endif

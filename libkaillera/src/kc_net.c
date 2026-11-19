/* libkaillera - UDP sockets and time, Windows and POSIX. LGPL-2.1-or-later, see LICENSE. */
#include "kc_net.h"

#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

int kc_net_init(void)
{
	static int done;
	WSADATA wsa;
	if (done)
		return 0;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -1;
	done = 1;
	return 0;
}

void kc_net_close(kc_sock s)
{
	if (s != KC_BAD_SOCK)
		closesocket(s);
}

static int set_nonblocking(kc_sock s)
{
	u_long on = 1;
	return ioctlsocket(s, FIONBIO, &on) == 0 ? 0 : -1;
}

uint32_t kc_net_ms(void)
{
	return (uint32_t) GetTickCount();
}

#else
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <time.h>

int kc_net_init(void)
{
	return 0;
}

void kc_net_close(kc_sock s)
{
	if (s != KC_BAD_SOCK)
		close(s);
}

static int set_nonblocking(kc_sock s)
{
	int fl = fcntl(s, F_GETFL, 0);
	return (fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0) ? 0 : -1;
}

uint32_t kc_net_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif

kc_sock kc_net_open(void)
{
	kc_sock s;
	struct sockaddr_in a;
	if (kc_net_init() != 0)
		return KC_BAD_SOCK;
	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == KC_BAD_SOCK)
		return KC_BAD_SOCK;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = 0;
	if (bind(s, (struct sockaddr *) &a, sizeof(a)) != 0 || set_nonblocking(s) != 0)
	{
		kc_net_close(s);
		return KC_BAD_SOCK;
	}
	return s;
}

int kc_net_resolve(const char *host, int port, struct sockaddr_in *out)
{
	struct addrinfo hints, *res = NULL;
	char sport[16];
	if (kc_net_init() != 0)
		return -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	snprintf(sport, sizeof(sport), "%d", port);
	if (getaddrinfo(host, sport, &hints, &res) != 0 || !res)
		return -1;
	memcpy(out, res->ai_addr, sizeof(*out));
	freeaddrinfo(res);
	return 0;
}

int kc_net_send(kc_sock s, const struct sockaddr_in *to, const void *data, int len)
{
	return (int) sendto(s, (const char *) data, len, 0, (const struct sockaddr *) to, sizeof(*to)) == len ? 0 : -1;
}

int kc_net_recv(kc_sock s, void *buf, int size, struct sockaddr_in *from)
{
#ifdef _WIN32
	int fl = sizeof(*from);
	int n = recvfrom(s, (char *) buf, size, 0, (struct sockaddr *) from, &fl);
	if (n == SOCKET_ERROR)
	{
		int e = WSAGetLastError();
		/* WSAECONNRESET: an ICMP "port unreachable" from an earlier send; not fatal for UDP */
		return (e == WSAEWOULDBLOCK || e == WSAECONNRESET || e == WSAEMSGSIZE) ? 0 : -1;
	}
	return n;
#else
	socklen_t fl = sizeof(*from);
	ssize_t n = recvfrom(s, buf, size, 0, (struct sockaddr *) from, &fl);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED) ? 0 : -1;
	return (int) n;
#endif
}

int kc_net_wait(kc_sock s, int timeout_ms)
{
	fd_set rd;
	struct timeval tv;
	if (s == KC_BAD_SOCK)
		return 0;
	FD_ZERO(&rd);
	FD_SET(s, &rd);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	return select((int) s + 1, &rd, NULL, NULL, &tv) > 0 ? 1 : 0;
}

/* plat_posix - libc implementation of plat.h for host builds and tests */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "plat.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  /* macOS: SO_NOSIGPIPE + SIG_IGN instead */
#endif

const char *plat_default_watchdog = 0;

int mb_main(int argc, char **argv);

int main(int argc, char **argv)
{
	return mb_main(argc, argv);
}

void plat_init(void)
{
	signal(SIGPIPE, SIG_IGN);
}

void plat_exit(int code)
{
	exit(code);
}

u32 plat_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u32)ts.tv_sec * 1000u + (u32)(ts.tv_nsec / 1000000);
}

int plat_serial_open(const char *dev, u32 baud, char parity)
{
	struct termios t;
	speed_t sp;
	int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	switch (baud) {
	case 9600: sp = B9600; break;
	case 19200: sp = B19200; break;
	case 38400: sp = B38400; break;
	default: close(fd); return -1;
	}
	if (tcgetattr(fd, &t) < 0) {
		close(fd);
		return -1;
	}
	cfmakeraw(&t);
	t.c_iflag |= IGNBRK;
	cfsetispeed(&t, sp);
	cfsetospeed(&t, sp);
	t.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | PARODD | CRTSCTS);
	t.c_cflag |= CS8 | CREAD | CLOCAL;
	if (parity == 'e')
		t.c_cflag |= PARENB;
	else if (parity == 'o')
		t.c_cflag |= PARENB | PARODD;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &t) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

void plat_serial_flush(int fd)
{
	tcflush(fd, TCIFLUSH);
}

int plat_serial_describe(const char *dev, struct sbuf *s)
{
	struct termios t;
	int fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK), r;
	if (fd < 0)
		return -1;
	r = tcgetattr(fd, &t);
	close(fd);
	if (r < 0)
		return -1;
	sb_str(s, "iflag=");
	sb_u32(s, (u32)t.c_iflag);
	sb_str(s, " oflag=");
	sb_u32(s, (u32)t.c_oflag);
	sb_str(s, " cflag=");
	sb_u32(s, (u32)t.c_cflag);
	sb_str(s, " lflag=");
	sb_u32(s, (u32)t.c_lflag);
	sb_str(s, " ispeed=");
	sb_u32(s, (u32)cfgetispeed(&t));
	return 0;
}

int plat_read(int fd, void *b, int n)
{
	ssize_t r = read(fd, b, (size_t)n);
	if (r < 0 && (errno == EAGAIN || errno == EINTR))
		return 0;
	return (int)r;
}

int plat_write(int fd, const void *b, int n)
{
	const char *p = b;
	int done = 0;
	while (done < n) {
		ssize_t r = write(fd, p + done, (size_t)(n - done));
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (r <= 0)
			return -1;
		done += (int)r;
	}
	return done;
}

void plat_close(int fd)
{
	close(fd);
}

int plat_open_write(const char *path, int append)
{
	return open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
}

int plat_listen(u32 port)
{
	struct sockaddr_in a;
	int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((u16)port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int plat_accept(int lfd)
{
	int one = 1, fd = accept(lfd, 0, 0);
	if (fd < 0)
		return -1;
#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	return fd;
}

int plat_send(int fd, const void *b, int n)
{
	return (int)send(fd, b, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL);
}

int plat_recv(int fd, void *b, int n)
{
	ssize_t r = recv(fd, b, (size_t)n, MSG_DONTWAIT);
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return PLAT_AGAIN;
	return (int)r;
}

int plat_wait(const int *fds, int nfds, u8 *ready, int timeout_ms)
{
	fd_set rd;
	struct timeval tv;
	int i, max = -1, r;
	FD_ZERO(&rd);
	for (i = 0; i < nfds; i++) {
		ready[i] = 0;
		if (fds[i] >= 0 && fds[i] < PLAT_MAX_FDS) {
			FD_SET(fds[i], &rd);
			if (fds[i] > max)
				max = fds[i];
		}
	}
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	r = select(max + 1, &rd, 0, 0, &tv);
	if (r <= 0)
		return r < 0 && errno == EINTR ? 0 : r;
	for (i = 0; i < nfds; i++)
		ready[i] = fds[i] >= 0 && fds[i] < PLAT_MAX_FDS && FD_ISSET(fds[i], &rd);
	return r;
}

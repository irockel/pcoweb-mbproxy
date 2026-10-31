/*
 * plat_pcoweb - freestanding implementation of plat.h for the Carel pCOWeb card
 * (ARM720T, Linux 2.4.21, old ARM ABI, glibc 2.2 not used).
 *
 * System calls use the old ABI ("swi 0x900000 + nr"), networking goes through socketcall.
 * Kernel structures used here (termios, sockaddr_in, timeval, fd_set) have no 64-bit
 * members, so their layout is the same for OABI and the EABI compiler we build with.
 */
#include "plat.h"

#define NR_exit         1
#define NR_read         3
#define NR_write        4
#define NR_open         5
#define NR_close        6
#define NR_times        43
#define NR_signal       48
#define NR_ioctl        54
#define NR_socketcall   102
#define NR_newselect    142

#define SYSCALL5(nr, a, b, c, d, e) ({                                              \
	register long r0 asm("r0") = (long)(a);                                     \
	register long r1 asm("r1") = (long)(b);                                     \
	register long r2 asm("r2") = (long)(c);                                     \
	register long r3 asm("r3") = (long)(d);                                     \
	register long r4 asm("r4") = (long)(e);                                     \
	asm volatile("swi %5" : "+r"(r0)                                            \
		     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "i"(0x900000 + (nr))     \
		     : "memory");                                                   \
	r0; })
#define SYSCALL3(nr, a, b, c) SYSCALL5(nr, a, b, c, 0, 0)

#define O_WRONLY   01
#define O_RDWR     02
#define O_CREAT    0100
#define O_NOCTTY   0400
#define O_TRUNC    01000
#define O_APPEND   02000
#define O_NONBLOCK 04000

#define EINTR  4
#define EAGAIN 11

#define TCGETS   0x5401
#define TCSETS   0x5402
#define TCFLSH   0x540B
#define TIOCMGET 0x5415
#define TCIFLUSH 0

#define IGNBRK  0000001
#define B9600   0000015
#define B19200  0000016
#define B38400  0000017
#define CS8     0000060
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define CLOCAL  0004000
#define VTIME   5
#define VMIN    6

#define SYS_SOCKET     1
#define SYS_BIND       2
#define SYS_LISTEN     4
#define SYS_ACCEPT     5
#define SYS_SEND       9
#define SYS_RECV       10
#define SYS_SETSOCKOPT 14
#define AF_INET        2
#define SOCK_STREAM    1
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_KEEPALIVE   9
#define IPPROTO_TCP    6
#define TCP_NODELAY    1
#define MSG_DONTWAIT   0x40
#define MSG_NOSIGNAL   0x4000

#define SIGPIPE 13
#define SIG_IGN 1

#define HZ 100                  /* jiffies per second (times() return value) */

struct ktermios {
	u32 c_iflag, c_oflag, c_cflag, c_lflag;
	u8 c_line;
	u8 c_cc[19];
};

struct sockaddr_in {
	u16 sin_family;
	u16 sin_port;           /* network byte order */
	u32 sin_addr;
	u8 sin_zero[8];
};

struct timeval { long sec, usec; };
struct tms { long utime, stime, cutime, cstime; };

const char *plat_default_watchdog = "/proc/hwio";

int mb_main(int argc, char **argv);

/* gcc may emit calls to these for struct copies/initialisation */
void *memset(void *d, int c, unsigned long n)
{
	u8 *p = d;
	while (n--)
		*p++ = (u8)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	u8 *p = d;
	const u8 *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

/* the kernel starts us with sp -> argc, argv[0], ..., 0, envp... */
__asm__(".text\n"
	".align 2\n"
	".global _start\n"
	".type _start, %function\n"
	"_start:\n"
	"\tmov r0, sp\n"
	"\tbl pcoweb_start\n"
	"\tb .\n");

__attribute__((used)) void pcoweb_start(long *sp)
{
	plat_exit(mb_main((int)sp[0], (char **)(sp + 1)));
}

static long socketcall(long call, unsigned long *args)
{
	return SYSCALL3(NR_socketcall, call, args, 0);
}

void plat_init(void)
{
	SYSCALL3(NR_signal, SIGPIPE, SIG_IGN, 0);
}

void plat_exit(int code)
{
	SYSCALL3(NR_exit, code, 0, 0);
	for (;;)
		;
}

u32 plat_ms(void)
{
	struct tms t;
	/* jiffies since boot: monotonic, unlike the wall clock (no RTC, may be set) */
	return (u32)SYSCALL3(NR_times, &t, 0, 0) * (1000 / HZ);
}

int plat_serial_open(const char *dev, u32 baud, char parity)
{
	struct ktermios t;
	u32 sp;
	long fd = SYSCALL3(NR_open, dev, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	switch (baud) {
	case 9600: sp = B9600; break;
	case 19200: sp = B19200; break;
	case 38400: sp = B38400; break;
	default: plat_close((int)fd); return -1;
	}
	if (SYSCALL3(NR_ioctl, fd, TCGETS, &t) < 0) {
		plat_close((int)fd);
		return -1;
	}
	/* same as pcod (checked with -q on the card): IGNBRK, raw, 8N1, CREAD|CLOCAL */
	t.c_iflag = IGNBRK;
	t.c_oflag = 0;
	t.c_lflag = 0;
	t.c_cflag = sp | CS8 | CREAD | CLOCAL;
	if (parity == 'e')
		t.c_cflag |= PARENB;
	else if (parity == 'o')
		t.c_cflag |= PARENB | PARODD;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (SYSCALL3(NR_ioctl, fd, TCSETS, &t) < 0) {
		plat_close((int)fd);
		return -1;
	}
	return (int)fd;
}

void plat_serial_flush(int fd)
{
	SYSCALL3(NR_ioctl, fd, TCFLSH, TCIFLUSH);
}

int plat_serial_describe(const char *dev, struct sbuf *s)
{
	struct ktermios t;
	u32 m = 0;
	int i;
	long r, fd = SYSCALL3(NR_open, dev, O_NOCTTY | O_NONBLOCK, 0);     /* O_RDONLY */
	if (fd < 0)
		return -1;
	r = SYSCALL3(NR_ioctl, fd, TCGETS, &t);
	SYSCALL3(NR_ioctl, fd, TIOCMGET, &m);
	plat_close((int)fd);
	if (r < 0)
		return -1;
	sb_str(s, "iflag=");
	sb_u32(s, t.c_iflag);
	sb_str(s, " oflag=");
	sb_u32(s, t.c_oflag);
	sb_str(s, " cflag=");
	sb_u32(s, t.c_cflag);
	sb_str(s, " lflag=");
	sb_u32(s, t.c_lflag);
	sb_str(s, " line=");
	sb_u32(s, t.c_line);
	sb_str(s, " cc=");
	for (i = 0; i < 19; i++) {
		if (i)
			sb_str(s, ",");
		sb_u32(s, t.c_cc[i]);
	}
	sb_str(s, " modem=");
	sb_u32(s, m);
	return 0;
}

int plat_read(int fd, void *b, int n)
{
	long r = SYSCALL3(NR_read, fd, b, n);
	if (r == -EAGAIN || r == -EINTR)
		return 0;
	return (int)r;
}

int plat_write(int fd, const void *b, int n)
{
	const u8 *p = b;
	int done = 0;
	while (done < n) {
		long r = SYSCALL3(NR_write, fd, p + done, n - done);
		if (r == -EAGAIN || r == -EINTR)
			continue;
		if (r <= 0)
			return -1;
		done += (int)r;
	}
	return done;
}

void plat_close(int fd)
{
	SYSCALL3(NR_close, fd, 0, 0);
}

int plat_open_write(const char *path, int append)
{
	long fd = SYSCALL3(NR_open, path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
	return fd < 0 ? -1 : (int)fd;
}

static int set_opt(long fd, long level, long opt)
{
	int one = 1;
	unsigned long a[5] = { (unsigned long)fd, (unsigned long)level, (unsigned long)opt,
			       (unsigned long)&one, sizeof(one) };
	return (int)socketcall(SYS_SETSOCKOPT, a);
}

int plat_listen(u32 port)
{
	struct sockaddr_in addr;
	unsigned long a[3] = { AF_INET, SOCK_STREAM, 0 };
	long fd = socketcall(SYS_SOCKET, a);
	if (fd < 0)
		return -1;
	set_opt(fd, SOL_SOCKET, SO_REUSEADDR);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = (u16)(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
	a[0] = (unsigned long)fd;
	a[1] = (unsigned long)&addr;
	a[2] = sizeof(addr);
	if (socketcall(SYS_BIND, a) < 0)
		goto fail;
	a[1] = 4;
	if (socketcall(SYS_LISTEN, a) < 0)
		goto fail;
	return (int)fd;
fail:
	plat_close((int)fd);
	return -1;
}

int plat_accept(int lfd)
{
	unsigned long a[3] = { (unsigned long)lfd, 0, 0 };
	long fd = socketcall(SYS_ACCEPT, a);
	if (fd < 0)
		return -1;
	set_opt(fd, IPPROTO_TCP, TCP_NODELAY);
	set_opt(fd, SOL_SOCKET, SO_KEEPALIVE);
	return (int)fd;
}

int plat_send(int fd, const void *b, int n)
{
	unsigned long a[4] = { (unsigned long)fd, (unsigned long)b, (unsigned long)n,
			       MSG_DONTWAIT | MSG_NOSIGNAL };
	return (int)socketcall(SYS_SEND, a);
}

int plat_recv(int fd, void *b, int n)
{
	unsigned long a[4] = { (unsigned long)fd, (unsigned long)b, (unsigned long)n, MSG_DONTWAIT };
	long r = socketcall(SYS_RECV, a);
	if (r == -EAGAIN || r == -EINTR)
		return PLAT_AGAIN;
	return (int)r;
}

int plat_wait(const int *fds, int nfds, u8 *ready, int timeout_ms)
{
	u32 set[PLAT_MAX_FDS / 32];
	struct timeval tv;
	int i, max = -1;
	long r;
	memset(set, 0, sizeof(set));
	for (i = 0; i < nfds; i++) {
		ready[i] = 0;
		if (fds[i] >= 0 && fds[i] < PLAT_MAX_FDS) {
			set[fds[i] >> 5] |= 1u << (fds[i] & 31);
			if (fds[i] > max)
				max = fds[i];
		}
	}
	tv.sec = timeout_ms / 1000;
	tv.usec = (timeout_ms % 1000) * 1000;
	r = SYSCALL5(NR_newselect, max + 1, set, 0, 0, &tv);
	if (r <= 0)
		return r == -EINTR ? 0 : (int)r;
	for (i = 0; i < nfds; i++)
		ready[i] = fds[i] >= 0 && fds[i] < PLAT_MAX_FDS && (set[fds[i] >> 5] >> (fds[i] & 31)) & 1;
	return (int)r;
}

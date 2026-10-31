/*
 * Platform layer for mbproxy. Two implementations:
 *   plat_pcoweb.c  freestanding, raw OABI system calls (Carel pCOWeb, Linux 2.4, ARM720T)
 *   plat_posix.c   libc, for building and testing on the development machine
 */
#ifndef PLAT_H
#define PLAT_H

#include "mb_core.h"

#define PLAT_MAX_FDS 64         /* plat_wait handles descriptors 0..63 */

/* set by the platform: path the watchdog is fed through by default (0 = none) */
extern const char *plat_default_watchdog;

void plat_init(void);                           /* ignore SIGPIPE etc. */
void plat_exit(int code);
u32 plat_ms(void);                              /* monotonic milliseconds, wraps around */

int plat_serial_open(const char *dev, u32 baud, char parity);   /* fd or < 0 */
void plat_serial_flush(int fd);                 /* drop unread input */
int plat_serial_describe(const char *dev, struct sbuf *s);      /* line settings as text, changes nothing */

int plat_read(int fd, void *b, int n);          /* < 0 on error, 0 = nothing / EOF */
int plat_write(int fd, const void *b, int n);
void plat_close(int fd);
int plat_open_write(const char *path, int append);      /* create, truncate unless append */

int plat_listen(u32 port);
int plat_accept(int lfd);
int plat_send(int fd, const void *b, int n);    /* never blocks, never raises SIGPIPE */
int plat_recv(int fd, void *b, int n);          /* 0 = peer closed, < 0 = error, PLAT_AGAIN */
#define PLAT_AGAIN -11

/* wait until one of fds is readable or timeout_ms passed; ready[i] = 1 for readable fds[i] */
int plat_wait(const int *fds, int nfds, u8 *ready, int timeout_ms);

#endif

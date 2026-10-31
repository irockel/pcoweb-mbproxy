/* Minimal freestanding test for the Carel pCOWeb (ARM720T, Linux 2.4, OABI).
 * No libc: system calls use the old ABI, i.e. "swi 0x900000 + number". */
#define SYS_exit  1
#define SYS_write 4

static long sys_write(long fd, const char *buf, long len)
{
	register long r0 asm("r0") = fd;
	register long r1 asm("r1") = (long)buf;
	register long r2 asm("r2") = len;
	asm volatile("swi %3" : "+r"(r0) : "r"(r1), "r"(r2), "i"(0x900000 + SYS_write) : "memory");
	return r0;
}

static void sys_exit(long code)
{
	register long r0 asm("r0") = code;
	asm volatile("swi %1" : : "r"(r0), "i"(0x900000 + SYS_exit) : "memory");
	for (;;) ;
}

void _start(void)
{
	static const char msg[] = "hello from the pCOWeb - own code runs\n";
	sys_write(1, msg, sizeof(msg) - 1);
	sys_exit(0);
}

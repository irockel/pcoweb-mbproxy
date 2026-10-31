/*
 * mbproxy - transparent Modbus TCP -> Modbus RTU gateway for the Carel pCOWeb card,
 * replacing pcod + tcpmodbus when the pCO controller is a Modbus RTU slave.
 *
 * Every TCP request is forwarded 1:1 to the controller and the answer is returned: no cache,
 * no background polling, no "U" phases. Requests of all clients go through one FIFO, so the
 * serial line only ever carries one transaction. A slave that does not answer yields
 * exception 0x0B instead of zeros. Writes are refused unless the address is on an allowlist.
 *
 * Single process, single thread, select() loop, static memory only. On the card the kernel
 * watchdog (WDOG_PCO) is fed by writing "pwdog" to /proc/hwio from the main loop, as pcod
 * does: if the loop hangs, the card reboots.
 *
 * Platform code lives in plat_pcoweb.c (card) and plat_posix.c (host tests).
 */
#include "plat.h"

#define MAX_CLIENTS     8
#define RXBUF           (2 * MB_TCP_MAX)
#define QLEN            16
#define TICK_MS         200
#define WDOG_EVERY_MS   2000
#define STAT_EVERY_MS   10000

struct client {
	int fd;
	u32 gen;                /* changes on every connection in this slot */
	u32 last_ms;
	int rxlen;
	u8 rx[RXBUF];
};

struct req {
	int client;
	u32 gen;
	u16 tid;
	u8 unit;
	u8 ex;                  /* != 0: answer with this exception, no bus traffic */
	int pdu_len;
	u8 pdu[MB_PDU_MAX];
};

static struct {
	u32 port, baud, slave, timeout_ms, retries, gap_ms, ratelimit_s, duration_s, max_clients, log_max, bind_wait_s;
	char parity;
	int verbose, query;
	const char *dev, *wdog, *logpath, *statpath;
	struct mb_cfg mb;
} opt;

static struct {
	u32 requests, ok, slave_exc, rejected, busy, timeouts, bad_frames, retries, failed, writes;
	u32 lat_min, lat_max, lat_sum, lat_n, q_max, connects, evicted;
} st;

static struct client clients[MAX_CLIENTS];
static struct req queue[QLEN];
static int q_head, q_count;
static u32 gen_counter, start_ms, now;
static int ser_fd = -1, listen_fd = -1;

/* serial transaction */
enum { S_IDLE, S_GAP, S_WAIT };
static struct {
	int state;
	int active;             /* r is still to be (re)sent after the gap */
	u32 attempt;
	struct req r;
	u8 tx[MB_RTU_MAX];
	int txlen;
	u8 rx[MB_RTU_MAX];
	int rxlen;
	u32 t_start, t_deadline, t_gap_end;
} ser;

/* rate limit per address: time of the last confirmed write + 1 (0 = never) */
static u32 last_write_hr[MB_BITMAP_ADDRS], last_write_coil[MB_BITMAP_ADDRS];

static int after(u32 a, u32 b) { return (int)(a - b) >= 0; }    /* a at or after b, wrap-safe */

/* ------------------------------------------------------------------ logging */

static int log_fd = 2;
static u32 log_size;
static char line_buf[512];
static struct sbuf line;

static struct sbuf *L(void)
{
	u32 t = now - start_ms, ms = t % 1000;
	line.p = line_buf;
	line.len = 0;
	line.cap = sizeof(line_buf) - 1;
	sb_u32(&line, t / 1000);
	sb_str(&line, ms < 10 ? ".00" : ms < 100 ? ".0" : ".");
	sb_u32(&line, ms);
	sb_str(&line, " ");
	return &line;
}

static void L_end(void)
{
	line.p[line.len++] = '\n';
	if (opt.logpath && log_size + (u32)line.len > opt.log_max) {
		plat_close(log_fd);
		log_fd = plat_open_write(opt.logpath, 0);
		log_size = 0;
	}
	if (log_fd >= 0 && plat_write(log_fd, line.p, line.len) > 0)
		log_size += (u32)line.len;
}

static void log_req(struct sbuf *s, const u8 *pdu)
{
	sb_str(s, "fc");
	sb_u32(s, pdu[0]);
	sb_str(s, " a=");
	sb_u32(s, mb_get16(pdu + 1));
	sb_str(s, pdu[0] == 5 || pdu[0] == 6 ? " v=" : " n=");
	sb_u32(s, mb_get16(pdu + 3));
	if (pdu[0] == 16) {
		u32 i, n = mb_get16(pdu + 3);
		sb_str(s, " [");
		for (i = 0; i < n && i < 8; i++) {
			if (i)
				sb_str(s, ",");
			sb_u32(s, mb_get16(pdu + 6 + 2 * i));
		}
		sb_str(s, n > 8 ? ",...]" : "]");
	}
}

/* ------------------------------------------------------------------ stats + watchdog */

static void write_stats(void)
{
	static char buf[600];
	struct sbuf s = { buf, 0, sizeof(buf) };
	int i, fd, n = 0;
	for (i = 0; i < MAX_CLIENTS; i++)
		n += clients[i].fd >= 0;
#define STAT(name, v) do { sb_str(&s, name "="); sb_u32(&s, v); sb_str(&s, "\n"); } while (0)
	STAT("uptime_s", (now - start_ms) / 1000);
	STAT("clients", (u32)n);
	STAT("connects", st.connects);
	STAT("evicted", st.evicted);
	STAT("requests", st.requests);
	STAT("ok", st.ok);
	STAT("slave_exceptions", st.slave_exc);
	STAT("rejected", st.rejected);
	STAT("busy", st.busy);
	STAT("timeouts", st.timeouts);
	STAT("bad_frames", st.bad_frames);
	STAT("retries", st.retries);
	STAT("failed", st.failed);
	STAT("writes", st.writes);
	STAT("lat_min_ms", st.lat_n ? st.lat_min : 0);
	STAT("lat_avg_ms", st.lat_n ? st.lat_sum / st.lat_n : 0);
	STAT("lat_max_ms", st.lat_max);
	STAT("queue_max", st.q_max);
#undef STAT
	fd = plat_open_write(opt.statpath, 0);
	if (fd >= 0) {
		plat_write(fd, buf, s.len);
		plat_close(fd);
	}
}

static void feed_watchdog(void)
{
	static int failed;
	int fd = plat_open_write(opt.wdog, 0);
	if (fd >= 0 && plat_write(fd, "pwdog\n", 6) == 6) {
		failed = 0;
	} else if (!failed++) {
		sb_str(L(), "watchdog: cannot write ");
		sb_str(&line, opt.wdog);
		L_end();
	}
	if (fd >= 0)
		plat_close(fd);
}

/* ------------------------------------------------------------------ clients */

static void close_client(struct client *c, const char *why)
{
	sb_str(L(), "client ");
	sb_u32(&line, (u32)(c - clients));
	sb_str(&line, " closed: ");
	sb_str(&line, why);
	L_end();
	plat_close(c->fd);
	c->fd = -1;
	c->rxlen = 0;
}

static void respond(const struct req *r, const u8 *pdu, int len)
{
	u8 out[MB_TCP_MAX];
	struct client *c = &clients[r->client];
	int n;
	if (c->fd < 0 || c->gen != r->gen)
		return;         /* client went away meanwhile */
	n = mb_tcp_build(r->tid, r->unit, pdu, len, out);
	if (plat_send(c->fd, out, n) != n)
		close_client(c, "send failed");
}

static void respond_ex(const struct req *r, u8 code)
{
	u8 pdu[2];
	respond(r, pdu, mb_exception_pdu(r->pdu[0], code, pdu));
}

static void accept_client(void)
{
	struct client *c = 0;
	u32 i;
	int fd = plat_accept(listen_fd);
	if (fd < 0)
		return;
	for (i = 0; i < opt.max_clients; i++)
		if (clients[i].fd < 0) {
			c = &clients[i];
			break;
		}
	if (!c) {
		/* full: drop the client that was quiet longest (half-open client connections after a restart) */
		c = &clients[0];
		for (i = 1; i < opt.max_clients; i++)
			if (after(c->last_ms, clients[i].last_ms) && c->last_ms != clients[i].last_ms)
				c = &clients[i];
		st.evicted++;
		close_client(c, "evicted for new connection");
	}
	c->fd = fd;
	c->gen = ++gen_counter;
	c->last_ms = now;
	c->rxlen = 0;
	st.connects++;
	sb_str(L(), "client ");
	sb_u32(&line, (u32)(c - clients));
	sb_str(&line, " connected");
	L_end();
}

static void enqueue(struct client *c, const struct mb_adu *a)
{
	struct req *r = &queue[(q_head + q_count) % QLEN];
	int i;
	r->client = (int)(c - clients);
	r->gen = c->gen;
	r->tid = a->tid;
	r->unit = a->unit;
	r->pdu_len = a->pdu_len;
	for (i = 0; i < a->pdu_len; i++)
		r->pdu[i] = a->pdu[i];
	st.requests++;
	r->ex = (u8)mb_validate(&opt.mb, r->pdu, r->pdu_len);
	if (r->ex) {
		st.rejected++;
		if (opt.verbose || mb_is_write(r->pdu[0])) {
			struct sbuf *s = L();
			sb_str(s, "rejected ");
			log_req(s, r->pdu);
			sb_str(s, " ex=");
			sb_u32(s, r->ex);
			L_end();
		}
	}
	if (++q_count > (int)st.q_max)
		st.q_max = (u32)q_count;
}

/* take complete ADUs out of the client buffer while the queue has room */
static void process_client(struct client *c)
{
	struct mb_adu a;
	int used = 0, n, i;
	while (q_count < QLEN) {
		n = mb_tcp_parse(c->rx + used, c->rxlen - used, &a);
		if (n < 0) {
			close_client(c, "invalid Modbus TCP header");
			return;
		}
		if (!n)
			break;
		enqueue(c, &a);
		used += n;
	}
	for (i = used; i < c->rxlen; i++)
		c->rx[i - used] = c->rx[i];
	c->rxlen -= used;
}

static void read_client(struct client *c)
{
	int n = plat_recv(c->fd, c->rx + c->rxlen, RXBUF - c->rxlen);
	if (n == PLAT_AGAIN)
		return;
	if (n <= 0) {
		close_client(c, n ? "receive error" : "peer closed");
		return;
	}
	c->rxlen += n;
	c->last_ms = now;
	process_client(c);
}

/* ------------------------------------------------------------------ serial line */

static void send_frame(void)
{
	plat_serial_flush(ser_fd);
	ser.rxlen = 0;
	ser.t_start = now;
	ser.t_deadline = now + opt.timeout_ms;
	ser.state = S_WAIT;
	if (opt.verbose) {
		sb_str(L(), "tx ");
		sb_hex(&line, ser.tx, ser.txlen);
		L_end();
	}
	if (plat_write(ser_fd, ser.tx, ser.txlen) != ser.txlen)
		ser.t_deadline = now;   /* handled as timeout */
}

static void finish(void)
{
	ser.active = 0;
	ser.state = S_GAP;
	ser.t_gap_end = now + opt.gap_ms;
}

static void fail_attempt(const char *why)
{
	struct sbuf *s = L();
	sb_str(s, why);
	sb_str(s, " ");
	log_req(s, ser.r.pdu);
	sb_str(s, " attempt ");
	sb_u32(s, ser.attempt + 1);
	if (ser.rxlen) {
		sb_str(s, " rx ");
		sb_hex(s, ser.rx, ser.rxlen);
	}
	L_end();
	if (ser.attempt < opt.retries) {
		ser.attempt++;
		st.retries++;
		ser.state = S_GAP;
		ser.t_gap_end = now + opt.gap_ms;
		return;
	}
	st.failed++;
	respond_ex(&ser.r, EX_GW_NO_RESPONSE);
	finish();
}

static void succeed(const u8 *pdu, int len)
{
	u32 lat = now - ser.t_start, i;
	u16 addr, count;
	if (!st.lat_n || lat < st.lat_min)
		st.lat_min = lat;
	if (lat > st.lat_max)
		st.lat_max = lat;
	st.lat_sum += lat;
	st.lat_n++;
	if (pdu[0] & 0x80) {
		st.slave_exc++;
	} else {
		st.ok++;
		if (mb_write_span(ser.r.pdu, &addr, &count)) {
			u32 *tab = ser.r.pdu[0] == 5 || ser.r.pdu[0] == 15 ? last_write_coil : last_write_hr;
			for (i = 0; i < count && addr + i < MB_BITMAP_ADDRS; i++)
				tab[addr + i] = now + 1;
			st.writes++;
		}
	}
	if (opt.verbose || mb_is_write(ser.r.pdu[0]) || (pdu[0] & 0x80)) {
		struct sbuf *s = L();
		log_req(s, ser.r.pdu);
		if (pdu[0] & 0x80) {
			sb_str(s, " slave exception ");
			sb_u32(s, pdu[1]);
		} else {
			sb_str(s, " ok");
		}
		sb_str(s, " ");
		sb_u32(s, lat);
		sb_str(s, "ms");
		L_end();
	}
	respond(&ser.r, pdu, len);
	finish();
}

static void serial_rx(void)
{
	int n = plat_read(ser_fd, ser.rx + ser.rxlen, MB_RTU_MAX - ser.rxlen), exp;
	if (n < 0) {
		st.bad_frames++;
		fail_attempt("serial read error");
		return;
	}
	ser.rxlen += n;
	exp = mb_rtu_expected((u8)opt.slave, ser.r.pdu, ser.rx, ser.rxlen);
	if (exp < 0 || (exp == 0 && ser.rxlen == MB_RTU_MAX)) {
		st.bad_frames++;
		fail_attempt("bad frame");
	} else if (exp > 0 && ser.rxlen >= exp) {
		int r = mb_rtu_check((u8)opt.slave, ser.r.pdu, ser.r.pdu_len, ser.rx, exp);
		if (r < 0) {
			st.bad_frames++;
			fail_attempt(r == RTU_BAD_CRC ? "bad crc" : r == RTU_BAD_ECHO ? "bad echo" : "bad frame");
		} else {
			succeed(ser.rx + 1, r);
		}
	}
}

static int rate_limited(const struct req *r)
{
	u16 addr, count, i;
	u32 *tab, limit = opt.ratelimit_s * 1000;
	if (!limit || !mb_write_span(r->pdu, &addr, &count))
		return 0;
	tab = r->pdu[0] == 5 || r->pdu[0] == 15 ? last_write_coil : last_write_hr;
	for (i = 0; i < count && addr + i < MB_BITMAP_ADDRS; i++)
		if (tab[addr + i] && !after(now + 1, tab[addr + i] + limit))
			return 1;
	return 0;
}

static void serial_step(void)
{
	if (ser.state == S_WAIT && after(now, ser.t_deadline)) {
		st.timeouts++;
		fail_attempt("timeout");
	}
	if (ser.state == S_GAP && after(now, ser.t_gap_end)) {
		if (ser.active)
			send_frame();
		else
			ser.state = S_IDLE;
	}
	while (ser.state == S_IDLE && q_count) {
		struct req *r = &queue[q_head];
		q_head = (q_head + 1) % QLEN;
		q_count--;
		if (clients[r->client].fd < 0 || clients[r->client].gen != r->gen)
			continue;
		if (r->ex) {
			respond_ex(r, r->ex);
			continue;
		}
		if (rate_limited(r)) {
			st.busy++;
			sb_str(L(), "rate limited ");
			log_req(&line, r->pdu);
			L_end();
			respond_ex(r, EX_SERVER_BUSY);
			continue;
		}
		ser.r = *r;
		ser.attempt = 0;
		ser.active = 1;
		ser.txlen = mb_rtu_build((u8)opt.slave, r->pdu, r->pdu_len, ser.tx);
		send_frame();
	}
}

/* ------------------------------------------------------------------ options */

static void usage(void)
{
	static const char u[] =
		"usage: mbproxy [options]\n"
		"  -p port      Modbus TCP port (502)\n"
		"  -d device    serial device (/dev/ttyAM0)\n"
		"  -b baud      9600 | 19200 | 38400 (19200)\n"
		"  -P parity    n | e | o (n)\n"
		"  -s slave     Modbus RTU slave address of the pCO (1)\n"
		"  -T ms        response timeout per attempt (1500)\n"
		"  -r n         retries after timeout / bad frame (1)\n"
		"  -g ms        silence between transactions (100)\n"
		"  -a addr      highest holding register; requests beyond it are refused without bus\n"
		"               traffic (65535 = no check)\n"
		"  -k addr      highest coil, as -a (65535)\n"
		"  -w list      holding registers that may be written, e.g. 38,92,94 (none)\n"
		"  -c list      coils that may be written (none)\n"
		"  -z list      holding registers never written with 0 (none)\n"
		"  -V ranges    allowed raw values per holding register, e.g. 38=350..600,159=0..1\n"
		"               (signed; registers without a range accept any value)\n"
		"  -R s         minimum seconds between writes to one address (0 = off)\n"
		"  -W path      watchdog file, 'none' to disable (card: /proc/hwio)\n"
		"  -l path      log file (stderr)\n"
		"  -L bytes     truncate the log file beyond this size (65536)\n"
		"  -S path      statistics file, rewritten every 10 s\n"
		"  -t s         exit after this many seconds (0 = run forever)\n"
		"  -B s         keep trying to bind the port this long, e.g. while tcpmodbus' connections\n"
		"               are in TIME_WAIT (120)\n"
		"  -m n         maximum TCP clients, 1-8 (4)\n"
		"  -q           print the current settings of the serial device and exit\n"
		"  -v           log every request and frame\n";
	plat_write(2, u, sizeof(u) - 1);
}

static int parse_args(int argc, char **argv)
{
	int i;
	opt.port = 502;
	opt.dev = "/dev/ttyAM0";
	opt.baud = 19200;
	opt.parity = 'n';
	opt.slave = 1;
	opt.timeout_ms = 1500;
	opt.retries = 1;
	opt.gap_ms = 100;
	opt.mb.max_hr = 65535;
	opt.mb.max_coil = 65535;
	opt.wdog = plat_default_watchdog;
	opt.log_max = 65536;
	opt.max_clients = 4;
	opt.bind_wait_s = 120;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
		u32 n = 0;
		int bad = 0;
		if (a[0] != '-' || !a[1] || a[2])
			return -1;
		if (a[1] == 'v') { opt.verbose = 1; continue; }
		if (a[1] == 'q') { opt.query = 1; continue; }
		if (!v)
			return -1;
		i++;
		switch (a[1]) {
		case 'd': opt.dev = v; break;
		case 'W': opt.wdog = str_eq(v, "none") ? 0 : v; break;
		case 'l': opt.logpath = v; break;
		case 'S': opt.statpath = v; break;
		case 'P': opt.parity = v[0]; bad = v[1] || (v[0] != 'n' && v[0] != 'e' && v[0] != 'o'); break;
		case 'w': bad = parse_addr_list(v, opt.mb.hr_write); break;
		case 'c': bad = parse_addr_list(v, opt.mb.coil_write); break;
		case 'V': bad = parse_ranges(v, &opt.mb); break;
		case 'z':
			for (n = 0; n < sizeof(opt.mb.hr_nonzero); n++)
				opt.mb.hr_nonzero[n] = 0;
			bad = parse_addr_list(v, opt.mb.hr_nonzero);
			break;
		default:
			bad = parse_u32(v, &n);
			switch (a[1]) {
			case 'p': opt.port = n; bad |= !n || n > 65535; break;
			case 'b': opt.baud = n; break;
			case 's': opt.slave = n; bad |= !n || n > 247; break;
			case 'T': opt.timeout_ms = n; bad |= n < 10 || n > 60000; break;
			case 'r': opt.retries = n; bad |= n > 5; break;
			case 'g': opt.gap_ms = n; bad |= n > 10000; break;
			case 'a': opt.mb.max_hr = (u16)n; bad |= n > 65535; break;
			case 'k': opt.mb.max_coil = (u16)n; bad |= n > 65535; break;
			case 'R': opt.ratelimit_s = n; bad |= n > 86400; break;
			case 'L': opt.log_max = n; bad |= n < 1024; break;
			case 't': opt.duration_s = n; break;
			case 'B': opt.bind_wait_s = n; bad |= n > 3600; break;
			case 'm': opt.max_clients = n; bad |= !n || n > MAX_CLIENTS; break;
			default: bad = 1;
			}
		}
		if (bad)
			return -1;
	}
	return 0;
}

static void log_list(struct sbuf *s, const char *name, const u8 *map)
{
	u32 a = 0, b, first = 1;
	sb_str(s, name);
	while (a < MB_BITMAP_ADDRS) {
		if (!mb_bit(map, a)) {
			a++;
			continue;
		}
		for (b = a; b + 1 < MB_BITMAP_ADDRS && mb_bit(map, b + 1); b++)
			;
		sb_str(s, first ? "" : ",");
		sb_u32(s, a);
		if (b > a) {
			sb_str(s, "-");
			sb_u32(s, b);
		}
		first = 0;
		a = b + 1;
	}
	if (first)
		sb_str(s, "none");
}

/* ------------------------------------------------------------------ main loop */

int mb_main(int argc, char **argv)
{
	int fds[2 + MAX_CLIENTS], who[2 + MAX_CLIENTS], i, n, waited = 0;
	u8 ready[2 + MAX_CLIENTS];
	u32 last_wdog, last_stat;

	if (parse_args(argc, argv) < 0) {
		usage();
		return 2;
	}
	now = start_ms = plat_ms();
	if (opt.query) {
		static char buf[300];
		struct sbuf s = { buf, 0, sizeof(buf) - 1 };
		if (plat_serial_describe(opt.dev, &s) < 0)
			return 1;
		buf[s.len++] = '\n';
		plat_write(1, buf, s.len);
		return 0;
	}
	plat_init();
	if (opt.logpath)
		log_fd = plat_open_write(opt.logpath, 0);
	for (i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;

	/* feed the watchdog first: when started from the plugin script, pcod has just been killed */
	if (opt.wdog)
		feed_watchdog();
	last_wdog = last_stat = now;

	ser_fd = plat_serial_open(opt.dev, opt.baud, opt.parity);
	{
		struct sbuf *s = L();
		sb_str(s, "mbproxy start dev=");
		sb_str(s, opt.dev);
		sb_str(s, " baud=");
		sb_u32(s, opt.baud);
		sb_str(s, " slave=");
		sb_u32(s, opt.slave);
		sb_str(s, " port=");
		sb_u32(s, opt.port);
		sb_str(s, " timeout=");
		sb_u32(s, opt.timeout_ms);
		sb_str(s, " retries=");
		sb_u32(s, opt.retries);
		sb_str(s, " gap=");
		sb_u32(s, opt.gap_ms);
		log_list(s, " write_hr=", opt.mb.hr_write);
		log_list(s, " write_coil=", opt.mb.coil_write);
		log_list(s, " nonzero=", opt.mb.hr_nonzero);
		sb_str(s, " ranges=");
		if (!opt.mb.n_ranges)
			sb_str(s, "none");
		for (i = 0; i < opt.mb.n_ranges; i++) {
			sb_str(s, i ? "," : "");
			sb_u32(s, opt.mb.range[i].addr);
			sb_str(s, "=");
			sb_i32(s, opt.mb.range[i].lo);
			sb_str(s, "..");
			sb_i32(s, opt.mb.range[i].hi);
		}
		L_end();
	}
	if (ser_fd < 0) {
		sb_str(L(), "cannot open serial device");
		L_end();
		return 1;
	}
	/*
	 * Right after tcpmodbus is killed its connections sit in TIME_WAIT for 60 s, and Linux only lets us
	 * bind over them if they had SO_REUSEADDR too, which they don't. So wait for the port, feeding the
	 * watchdog meanwhile.
	 */
	for (;;) {
		listen_fd = plat_listen(opt.port);
		now = plat_ms();
		if (listen_fd >= 0 || after(now, start_ms + opt.bind_wait_s * 1000))
			break;
		if (opt.wdog && after(now, last_wdog + WDOG_EVERY_MS)) {
			feed_watchdog();
			last_wdog = now;
		}
		if (!waited++) {
			sb_str(L(), "port busy, retrying");
			L_end();
		}
		plat_wait(fds, 0, ready, 1000);
	}
	if (listen_fd < 0) {
		sb_str(L(), "cannot listen on port");
		L_end();
		return 1;
	}
	if (waited) {
		sb_str(L(), "listening");
		L_end();
	}

	for (;;) {
		now = plat_ms();
		if (opt.wdog && after(now, last_wdog + WDOG_EVERY_MS)) {
			feed_watchdog();
			last_wdog = now;
		}
		if (opt.statpath && after(now, last_stat + STAT_EVERY_MS)) {
			write_stats();
			last_stat = now;
		}
		if (opt.duration_s && after(now, start_ms + opt.duration_s * 1000))
			break;

		serial_step();
		/* the queue may have room again for requests still waiting in client buffers */
		for (i = 0; i < MAX_CLIENTS && q_count < QLEN; i++)
			if (clients[i].fd >= 0 && clients[i].rxlen)
				process_client(&clients[i]);
		serial_step();

		n = 0;
		fds[n] = listen_fd;
		who[n++] = -1;
		for (i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0 && clients[i].rxlen < RXBUF) {
				fds[n] = clients[i].fd;
				who[n++] = i;
			}
		if (ser.state == S_WAIT) {
			fds[n] = ser_fd;
			who[n++] = -2;
		}
		{
			int t = TICK_MS, rem;
			if (ser.state == S_WAIT || ser.state == S_GAP) {
				rem = (int)((ser.state == S_WAIT ? ser.t_deadline : ser.t_gap_end) - now);
				if (rem < t)
					t = rem < 0 ? 0 : rem;
			}
			if (plat_wait(fds, n, ready, t) < 0)
				continue;
		}
		now = plat_ms();
		for (i = 0; i < n; i++) {
			if (!ready[i])
				continue;
			if (who[i] == -1)
				accept_client();
			else if (who[i] == -2)
				serial_rx();
			else if (clients[who[i]].fd == fds[i])
				read_client(&clients[who[i]]);
		}
	}

	sb_str(L(), "mbproxy exit after ");
	sb_u32(&line, opt.duration_s);
	sb_str(&line, " s");
	L_end();
	if (opt.statpath)
		write_stats();
	return 0;
}

/* mb_core - see mb_core.h */
#include "mb_core.h"

u16 mb_crc16(const u8 *b, int n)
{
	u16 c = 0xFFFF;
	int i;
	while (n--) {
		c ^= *b++;
		for (i = 0; i < 8; i++)
			c = (c & 1) ? (u16)((c >> 1) ^ 0xA001) : (u16)(c >> 1);
	}
	return c;
}

int mb_bit(const u8 *map, u32 addr)
{
	return addr < MB_BITMAP_ADDRS && (map[addr >> 3] >> (addr & 7)) & 1;
}

void mb_set_bit(u8 *map, u32 addr)
{
	if (addr < MB_BITMAP_ADDRS)
		map[addr >> 3] |= (u8)(1 << (addr & 7));
}

static int any_bit(const u8 *map)
{
	int i;
	for (i = 0; i < MB_BITMAP_ADDRS / 8; i++)
		if (map[i])
			return 1;
	return 0;
}

/* value v (raw, signed 16 bit) allowed for holding register addr? */
static int in_range(const struct mb_cfg *cfg, u32 addr, u16 v)
{
	int i;
	for (i = 0; i < cfg->n_ranges; i++)
		if (cfg->range[i].addr == addr)
			return (short)v >= cfg->range[i].lo && (short)v <= cfg->range[i].hi;
	return 1;
}

static int all_bits(const u8 *map, u32 addr, u32 count)
{
	while (count--)
		if (!mb_bit(map, addr++))
			return 0;
	return 1;
}

/* ------------------------------------------------------------------ Modbus TCP */

int mb_tcp_parse(const u8 *buf, int len, struct mb_adu *out)
{
	int mlen;
	if (len < 7)
		return 0;
	mlen = mb_get16(buf + 4);
	if (mb_get16(buf + 2) != 0 || mlen < 2 || mlen > 1 + MB_PDU_MAX)
		return -1;
	if (len < 6 + mlen)
		return 0;
	out->tid = mb_get16(buf);
	out->unit = buf[6];
	out->pdu = buf + 7;
	out->pdu_len = mlen - 1;
	return 6 + mlen;
}

int mb_tcp_build(u16 tid, u8 unit, const u8 *pdu, int pdu_len, u8 *out)
{
	int i;
	mb_put16(out, tid);
	mb_put16(out + 2, 0);
	mb_put16(out + 4, (u16)(pdu_len + 1));
	out[6] = unit;
	for (i = 0; i < pdu_len; i++)
		out[7 + i] = pdu[i];
	return 7 + pdu_len;
}

int mb_exception_pdu(u8 fc, u8 code, u8 *out)
{
	out[0] = (u8)(fc | 0x80);
	out[1] = code;
	return 2;
}

/* ------------------------------------------------------------------ request validation */

int mb_is_write(u8 fc)
{
	return fc == 5 || fc == 6 || fc == 15 || fc == 16;
}

int mb_write_span(const u8 *pdu, u16 *addr, u16 *count)
{
	*addr = mb_get16(pdu + 1);
	*count = (pdu[0] == 5 || pdu[0] == 6) ? 1 : mb_is_write(pdu[0]) ? mb_get16(pdu + 3) : 0;
	return *count;
}

int mb_validate(const struct mb_cfg *cfg, const u8 *pdu, int len)
{
	u8 fc = pdu[0];
	u32 addr, qty, i;

	if (len < 5)
		return (fc >= 1 && fc <= 6) || fc == 15 || fc == 16 ? EX_ILLEGAL_VALUE : EX_ILLEGAL_FUNCTION;
	addr = mb_get16(pdu + 1);
	qty = mb_get16(pdu + 3);

	switch (fc) {
	case 1:
	case 2:
		if (len != 5 || qty < 1 || qty > 2000)
			return EX_ILLEGAL_VALUE;
		return addr + qty - 1 > cfg->max_coil ? EX_ILLEGAL_ADDRESS : 0;
	case 3:
	case 4:
		if (len != 5 || qty < 1 || qty > 125)
			return EX_ILLEGAL_VALUE;
		return addr + qty - 1 > cfg->max_hr ? EX_ILLEGAL_ADDRESS : 0;
	case 5:
		if (!any_bit(cfg->coil_write))
			return EX_ILLEGAL_FUNCTION;
		if (len != 5 || (qty != 0 && qty != 0xFF00))
			return EX_ILLEGAL_VALUE;
		if (addr > cfg->max_coil || !mb_bit(cfg->coil_write, addr))
			return EX_ILLEGAL_ADDRESS;
		return 0;
	case 6:
		if (!any_bit(cfg->hr_write))
			return EX_ILLEGAL_FUNCTION;
		if (len != 5)
			return EX_ILLEGAL_VALUE;
		if (addr > cfg->max_hr || !mb_bit(cfg->hr_write, addr))
			return EX_ILLEGAL_ADDRESS;
		if ((qty == 0 && mb_bit(cfg->hr_nonzero, addr)) || !in_range(cfg, addr, (u16)qty))
			return EX_ILLEGAL_VALUE;
		return 0;
	case 15:
		if (!any_bit(cfg->coil_write))
			return EX_ILLEGAL_FUNCTION;
		if (len < 6 || qty < 1 || qty > 1968 || pdu[5] != (qty + 7) / 8 || len != 6 + pdu[5])
			return EX_ILLEGAL_VALUE;
		if (addr + qty - 1 > cfg->max_coil || !all_bits(cfg->coil_write, addr, qty))
			return EX_ILLEGAL_ADDRESS;
		return 0;
	case 16:
		if (!any_bit(cfg->hr_write))
			return EX_ILLEGAL_FUNCTION;
		if (len < 6 || qty < 1 || qty > 123 || pdu[5] != qty * 2 || len != 6 + pdu[5])
			return EX_ILLEGAL_VALUE;
		if (addr + qty - 1 > cfg->max_hr || !all_bits(cfg->hr_write, addr, qty))
			return EX_ILLEGAL_ADDRESS;
		for (i = 0; i < qty; i++) {
			u16 v = mb_get16(pdu + 6 + 2 * i);
			if ((v == 0 && mb_bit(cfg->hr_nonzero, addr + i)) || !in_range(cfg, addr + i, v))
				return EX_ILLEGAL_VALUE;
		}
		return 0;
	}
	return EX_ILLEGAL_FUNCTION;
}

/* ------------------------------------------------------------------ Modbus RTU */

int mb_rtu_build(u8 slave, const u8 *pdu, int pdu_len, u8 *out)
{
	int i;
	u16 c;
	out[0] = slave;
	for (i = 0; i < pdu_len; i++)
		out[1 + i] = pdu[i];
	c = mb_crc16(out, 1 + pdu_len);
	out[1 + pdu_len] = (u8)c;
	out[2 + pdu_len] = (u8)(c >> 8);
	return 3 + pdu_len;
}

int mb_rtu_expected(u8 slave, const u8 *req, const u8 *rx, int rxlen)
{
	u32 bc;
	if (rxlen < 1)
		return 0;
	if (rx[0] != slave)
		return RTU_BAD_FRAME;
	if (rxlen < 2)
		return 0;
	if (rx[1] == (req[0] | 0x80))
		return 5;
	if (rx[1] != req[0])
		return RTU_BAD_FRAME;
	switch (req[0]) {
	case 1:
	case 2:
	case 3:
	case 4:
		if (rxlen < 3)
			return 0;
		bc = mb_get16(req + 3);
		bc = req[0] <= 2 ? (bc + 7) / 8 : bc * 2;
		return rx[2] == bc ? 5 + (int)bc : RTU_BAD_FRAME;
	default:
		return 8;
	}
}

int mb_rtu_check(u8 slave, const u8 *req, int req_len, const u8 *rx, int rxlen)
{
	int exp = mb_rtu_expected(slave, req, rx, rxlen), i;
	(void)req_len;
	if (exp <= 0 || rxlen != exp)
		return RTU_BAD_FRAME;
	if (mb_crc16(rx, rxlen - 2) != (u16)(rx[rxlen - 2] | (rx[rxlen - 1] << 8)))
		return RTU_BAD_CRC;
	if (rx[1] & 0x80)
		return 2;
	if (req[0] == 5 || req[0] == 6) {
		for (i = 0; i < 5; i++)
			if (rx[1 + i] != req[i])
				return RTU_BAD_ECHO;
	} else if (req[0] == 15 || req[0] == 16) {
		for (i = 1; i < 5; i++)
			if (rx[1 + i] != req[i])
				return RTU_BAD_ECHO;
	}
	return rxlen - 3;
}

/* ------------------------------------------------------------------ text helpers */

void sb_str(struct sbuf *s, const char *str)
{
	while (*str && s->len < s->cap - 1)
		s->p[s->len++] = *str++;
	s->p[s->len] = 0;
}

void sb_u32(struct sbuf *s, u32 v)
{
	char tmp[11];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n && s->len < s->cap - 1)
		s->p[s->len++] = tmp[--n];
	s->p[s->len] = 0;
}

void sb_i32(struct sbuf *s, int v)
{
	if (v < 0) {
		sb_str(s, "-");
		sb_u32(s, (u32)-v);
	} else {
		sb_u32(s, (u32)v);
	}
}

void sb_hex(struct sbuf *s, const u8 *b, int n)
{
	static const char hx[] = "0123456789abcdef";
	int i;
	for (i = 0; i < n && s->len < s->cap - 3; i++) {
		if (i)
			s->p[s->len++] = ' ';
		s->p[s->len++] = hx[b[i] >> 4];
		s->p[s->len++] = hx[b[i] & 15];
	}
	s->p[s->len] = 0;
}

int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

int str_len(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

/* parses digits up to the first non-digit, returns the pointer behind them or 0 */
static const char *scan_u32(const char *s, u32 *out)
{
	u32 v = 0;
	if (*s < '0' || *s > '9')
		return 0;
	while (*s >= '0' && *s <= '9') {
		if (v > 429496728)
			return 0;
		v = v * 10 + (u32)(*s++ - '0');
	}
	*out = v;
	return s;
}

/* like scan_u32 with an optional leading '-' */
static const char *scan_i32(const char *s, int *out)
{
	u32 v;
	int neg = *s == '-';
	s = scan_u32(s + neg, &v);
	if (!s || v > 32768)
		return 0;
	*out = neg ? -(int)v : (int)v;
	return s;
}

int parse_ranges(const char *s, struct mb_cfg *cfg)
{
	u32 a;
	int lo, hi;
	while (*s) {
		s = scan_u32(s, &a);
		if (!s || *s != '=' || a > 65535 || cfg->n_ranges >= MB_MAX_RANGES)
			return -1;
		s = scan_i32(s + 1, &lo);
		if (!s || s[0] != '.' || s[1] != '.')
			return -1;
		s = scan_i32(s + 2, &hi);
		if (!s || lo > hi || lo < -32768 || hi > 32767)
			return -1;
		cfg->range[cfg->n_ranges].addr = (u16)a;
		cfg->range[cfg->n_ranges].lo = (short)lo;
		cfg->range[cfg->n_ranges].hi = (short)hi;
		cfg->n_ranges++;
		if (*s == ',')
			s++;
		else if (*s)
			return -1;
	}
	return 0;
}

int parse_u32(const char *s, u32 *out)
{
	s = scan_u32(s, out);
	return s && !*s ? 0 : -1;
}

int parse_addr_list(const char *s, u8 *map)
{
	u32 a, b;
	while (*s) {
		s = scan_u32(s, &a);
		if (!s)
			return -1;
		b = a;
		if (*s == '-') {
			s = scan_u32(s + 1, &b);
			if (!s || b < a)
				return -1;
		}
		if (b >= MB_BITMAP_ADDRS)
			return -1;
		while (a <= b)
			mb_set_bit(map, a++);
		if (*s == ',')
			s++;
		else if (*s)
			return -1;
	}
	return 0;
}

/* Unit tests for mb_core (host only) */
#include <stdio.h>
#include <string.h>

#include "mb_core.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static struct mb_cfg cfg;

static int v(const u8 *pdu, int len) { return mb_validate(&cfg, pdu, len); }

static void test_crc(void)
{
	/* read 32: HR 37 n=58 as sent by tools/direct_write_test.py */
	u8 f[8] = { 32, 3, 0, 37, 0, 58 }, out[8];
	u16 c = mb_crc16(f, 6);
	CHECK(mb_rtu_build(32, f + 1, 5, out) == 8);
	CHECK(out[6] == (u8)c && out[7] == (u8)(c >> 8));
	/* well-known vector: 01 03 00 00 00 0A -> CRC C5 CD */
	{
		u8 g[] = { 1, 3, 0, 0, 0, 10 };
		CHECK(mb_crc16(g, 6) == 0xCDC5);
	}
}

static void test_tcp(void)
{
	u8 buf[32] = { 0x12, 0x34, 0, 0, 0, 6, 0xFF, 3, 0, 1, 0, 2, 0xAA };
	u8 out[32], pdu[] = { 3, 4, 0, 1, 0, 2 };
	struct mb_adu a;
	CHECK(mb_tcp_parse(buf, 6, &a) == 0);
	CHECK(mb_tcp_parse(buf, 11, &a) == 0);
	CHECK(mb_tcp_parse(buf, 13, &a) == 12);
	CHECK(a.tid == 0x1234 && a.unit == 0xFF && a.pdu_len == 5 && a.pdu[0] == 3);
	buf[2] = 1;
	CHECK(mb_tcp_parse(buf, 13, &a) == -1);         /* protocol id != 0 */
	buf[2] = 0;
	buf[5] = 1;
	CHECK(mb_tcp_parse(buf, 13, &a) == -1);         /* length too small */
	buf[4] = 1;
	buf[5] = 0;
	CHECK(mb_tcp_parse(buf, 13, &a) == -1);         /* length too large (256) */
	CHECK(mb_tcp_build(7, 1, pdu, 6, out) == 13);
	CHECK(out[0] == 0 && out[1] == 7 && out[5] == 7 && out[6] == 1 && out[7] == 3);
}

static void test_validate(void)
{
	u8 r3[] = { 3, 0, 1, 0, 125 };
	u8 r3_big[] = { 3, 0, 1, 0, 126 };
	u8 r3_edge[] = { 3, 0, 249, 0, 1 };
	u8 r3_over[] = { 3, 0, 240, 0, 11 };
	u8 r1[] = { 1, 0, 190, 0, 10 };
	u8 r1_over[] = { 1, 0, 190, 0, 11 };
	u8 w6[] = { 6, 0, 38, 1, 229 };
	u8 w6_other[] = { 6, 0, 39, 0, 1 };
	u8 w6_zero[] = { 6, 0, 158, 0, 0 };
	u8 w6_one[] = { 6, 0, 158, 0, 1 };
	u8 w16[] = { 16, 0, 38, 0, 2, 4, 1, 229, 0, 5 };
	u8 w16_bc[] = { 16, 0, 38, 0, 2, 3, 1, 229, 0 };
	u8 w16_zero[] = { 16, 0, 157, 0, 2, 4, 0, 1, 0, 0 };
	u8 w5[] = { 5, 0, 10, 0xFF, 0 };
	u8 w5_bad[] = { 5, 0, 10, 0x12, 0 };
	u8 w15[] = { 15, 0, 10, 0, 3, 1, 5 };
	u8 w15_bc[] = { 15, 0, 10, 0, 9, 1, 5 };
	u8 f8[] = { 8, 0, 0, 0, 0 };
	u8 r3_short[] = { 3, 0, 1 };

	memset(&cfg, 0, sizeof(cfg));
	cfg.max_hr = 249;
	cfg.max_coil = 199;
	CHECK(v(r3, 5) == 0);
	CHECK(v(r3_big, 5) == EX_ILLEGAL_VALUE);
	CHECK(v(r3_edge, 5) == 0);
	CHECK(v(r3_over, 5) == EX_ILLEGAL_ADDRESS);
	CHECK(v(r3_short, 3) == EX_ILLEGAL_VALUE);
	CHECK(v(r1, 5) == 0);
	CHECK(v(r1_over, 5) == EX_ILLEGAL_ADDRESS);
	CHECK(v(f8, 5) == EX_ILLEGAL_FUNCTION);
	/* writes disabled entirely */
	CHECK(v(w6, 5) == EX_ILLEGAL_FUNCTION);
	CHECK(v(w5, 5) == EX_ILLEGAL_FUNCTION);

	CHECK(parse_addr_list("38,92,94,155-158", cfg.hr_write) == 0);
	CHECK(parse_addr_list("158-161", cfg.hr_nonzero) == 0);
	CHECK(parse_addr_list("10-12", cfg.coil_write) == 0);
	CHECK(v(w6, 5) == 0);
	CHECK(v(w6_other, 5) == EX_ILLEGAL_ADDRESS);
	CHECK(v(w6_zero, 5) == EX_ILLEGAL_VALUE);
	CHECK(v(w6_one, 5) == 0);
	CHECK(v(w16, 10) == EX_ILLEGAL_ADDRESS);        /* 39 not allowed */
	w16[2] = 92;
	CHECK(v(w16, 10) == EX_ILLEGAL_ADDRESS);        /* 93 not allowed */
	w16[2] = 155;
	CHECK(v(w16, 10) == 0);
	CHECK(v(w16_bc, 9) == EX_ILLEGAL_VALUE);
	CHECK(v(w16_zero, 10) == EX_ILLEGAL_VALUE);     /* 158 = 0 */
	CHECK(v(w5, 5) == 0);
	CHECK(v(w5_bad, 5) == EX_ILLEGAL_VALUE);
	CHECK(v(w15, 7) == 0);
	CHECK(v(w15_bc, 7) == EX_ILLEGAL_VALUE);
	w15[4] = 4;
	CHECK(v(w15, 7) == EX_ILLEGAL_ADDRESS);         /* coil 13 not allowed */
}

static void test_ranges(void)
{
	u8 w6_ok[] = { 6, 0, 38, 0x01, 0xE0 };           /* 480 */
	u8 w6_high[] = { 6, 0, 38, 0x02, 0xBC };         /* 700 */
	u8 w6_low[] = { 6, 0, 38, 0x01, 0x5D };          /* 349 */
	u8 w6_edge[] = { 6, 0, 38, 0x02, 0x58 };         /* 600 */
	u8 w6_neg_ok[] = { 6, 0, 91, 0xFF, 0x88 };       /* -120 */
	u8 w6_neg_low[] = { 6, 0, 91, 0xFF, 0x38 };      /* -200 */
	u8 w6_bms0[] = { 6, 0, 159, 0, 0 };
	u8 w6_bms2[] = { 6, 0, 159, 0, 2 };
	u8 w6_free[] = { 6, 0, 94, 0x7F, 0xFF };         /* no range for 94 */
	u8 w16[] = { 16, 0, 38, 0, 2, 4, 0x01, 0xE0, 0, 7 };    /* 38=480 ok, 39 has no range */
	u8 w16_bad[] = { 16, 0, 158, 0, 2, 4, 0, 1, 0, 3 };     /* 159=3 out of range */

	memset(&cfg, 0, sizeof(cfg));
	cfg.max_hr = 249;
	cfg.max_coil = 199;
	CHECK(parse_addr_list("38,39,91,94,158,159", cfg.hr_write) == 0);
	CHECK(parse_ranges("38=350..600,91=-150..-50,159=0..1", &cfg) == 0);
	CHECK(cfg.n_ranges == 3 && cfg.range[1].lo == -150 && cfg.range[1].hi == -50);
	CHECK(v(w6_ok, 5) == 0);
	CHECK(v(w6_edge, 5) == 0);
	CHECK(v(w6_high, 5) == EX_ILLEGAL_VALUE);
	CHECK(v(w6_low, 5) == EX_ILLEGAL_VALUE);
	CHECK(v(w6_neg_ok, 5) == 0);
	CHECK(v(w6_neg_low, 5) == EX_ILLEGAL_VALUE);
	CHECK(v(w6_bms0, 5) == 0);                      /* 159 not in hr_nonzero here */
	CHECK(v(w6_bms2, 5) == EX_ILLEGAL_VALUE);
	CHECK(v(w6_free, 5) == 0);
	CHECK(v(w16, 10) == 0);
	CHECK(v(w16_bad, 10) == EX_ILLEGAL_VALUE);

	CHECK(parse_ranges("", &cfg) == 0);
	CHECK(parse_ranges("38", &cfg) == -1);
	CHECK(parse_ranges("38=1", &cfg) == -1);
	CHECK(parse_ranges("38=5..1", &cfg) == -1);
	CHECK(parse_ranges("38=1..2;", &cfg) == -1);
	CHECK(parse_ranges("38=-40000..2", &cfg) == -1);
}

static void test_rtu(void)
{
	u8 req3[] = { 3, 0, 37, 0, 2 };
	u8 req6[] = { 6, 0, 38, 1, 229 };
	u8 req16[] = { 16, 0, 38, 0, 1, 2, 1, 229 };
	u8 req1[] = { 1, 0, 0, 0, 10 };
	u8 rx[16];
	int n;

	n = mb_rtu_build(32, (u8[]){ 3, 4, 1, 224, 1, 229 }, 6, rx);
	CHECK(n == 9);
	CHECK(mb_rtu_expected(32, req3, rx, 0) == 0);
	CHECK(mb_rtu_expected(32, req3, rx, 2) == 0);
	CHECK(mb_rtu_expected(32, req3, rx, 3) == 9);
	CHECK(mb_rtu_check(32, req3, 5, rx, 9) == 6);
	rx[4] ^= 1;
	CHECK(mb_rtu_check(32, req3, 5, rx, 9) == RTU_BAD_CRC);
	rx[0] = 1;
	CHECK(mb_rtu_expected(32, req3, rx, 1) == RTU_BAD_FRAME);
	rx[0] = 32;
	rx[2] = 6;
	CHECK(mb_rtu_expected(32, req3, rx, 3) == RTU_BAD_FRAME);      /* wrong byte count */
	rx[1] = 4;
	CHECK(mb_rtu_expected(32, req3, rx, 2) == RTU_BAD_FRAME);      /* wrong function */

	n = mb_rtu_build(32, (u8[]){ 0x83, 2 }, 2, rx);
	CHECK(mb_rtu_expected(32, req3, rx, 2) == 5);
	CHECK(mb_rtu_check(32, req3, 5, rx, n) == 2);

	n = mb_rtu_build(32, req6, 5, rx);
	CHECK(mb_rtu_expected(32, req6, rx, 2) == 8);
	CHECK(mb_rtu_check(32, req6, 5, rx, n) == 5);
	n = mb_rtu_build(32, (u8[]){ 6, 0, 38, 1, 228 }, 5, rx);
	CHECK(mb_rtu_check(32, req6, 5, rx, n) == RTU_BAD_ECHO);

	n = mb_rtu_build(32, req16, 5, rx);
	CHECK(mb_rtu_check(32, req16, 8, rx, n) == 5);

	n = mb_rtu_build(32, (u8[]){ 1, 2, 0xFF, 3 }, 4, rx);
	CHECK(mb_rtu_expected(32, req1, rx, 3) == 7);
	CHECK(mb_rtu_check(32, req1, 5, rx, n) == 4);
}

static void test_text(void)
{
	char b[64];
	struct sbuf s = { b, 0, sizeof(b) };
	u8 bytes[] = { 0x20, 0x03, 0xAB };
	u8 map[MB_BITMAP_ADDRS / 8];
	u32 n;
	sb_u32(&s, 0);
	sb_str(&s, " ");
	sb_u32(&s, 4294967295u);
	sb_str(&s, " ");
	sb_i32(&s, -42);
	sb_str(&s, " ");
	sb_hex(&s, bytes, 3);
	CHECK(strcmp(b, "0 4294967295 -42 20 03 ab") == 0);
	s.len = 0;
	s.cap = 4;
	sb_str(&s, "abcdef");
	CHECK(strcmp(b, "abc") == 0);

	CHECK(parse_u32("1500", &n) == 0 && n == 1500);
	CHECK(parse_u32("", &n) == -1);
	CHECK(parse_u32("12x", &n) == -1);
	CHECK(parse_u32("4294967296", &n) == -1);
	memset(map, 0, sizeof(map));
	CHECK(parse_addr_list("", map) == 0 && !mb_bit(map, 0));
	CHECK(parse_addr_list("1,3-5", map) == 0);
	CHECK(mb_bit(map, 1) && !mb_bit(map, 2) && mb_bit(map, 3) && mb_bit(map, 5) && !mb_bit(map, 6));
	CHECK(parse_addr_list("5-3", map) == -1);
	CHECK(parse_addr_list("1;2", map) == -1);
	CHECK(parse_addr_list("1024", map) == -1);
	CHECK(str_eq("none", "none") && !str_eq("none", "non") && str_len("abc") == 3);
}

int main(void)
{
	test_crc();
	test_tcp();
	test_validate();
	test_ranges();
	test_rtu();
	test_text();
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}

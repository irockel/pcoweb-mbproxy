/*
 * mb_core - platform independent Modbus helpers for mbproxy (no libc, no allocation).
 *
 * Everything here is a pure function on byte buffers, so it builds unchanged for the
 * freestanding pCOWeb binary and for the host test build.
 */
#ifndef MB_CORE_H
#define MB_CORE_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define MB_PDU_MAX      253     /* function code + data */
#define MB_TCP_MAX      (7 + MB_PDU_MAX)
#define MB_RTU_MAX      (1 + MB_PDU_MAX + 2)
#define MB_BITMAP_ADDRS 1024    /* write allowlists cover addresses 0..1023 */
#define MB_MAX_RANGES   32      /* holding registers with a value range */

/* exception codes */
#define EX_ILLEGAL_FUNCTION 0x01
#define EX_ILLEGAL_ADDRESS  0x02
#define EX_ILLEGAL_VALUE    0x03
#define EX_SERVER_BUSY      0x06
#define EX_GW_NO_RESPONSE   0x0B

struct mb_cfg {
	u16 max_hr;                             /* highest readable holding/input register */
	u16 max_coil;                           /* highest readable coil/discrete input */
	u8 hr_write[MB_BITMAP_ADDRS / 8];       /* holding registers that may be written */
	u8 coil_write[MB_BITMAP_ADDRS / 8];     /* coils that may be written */
	u8 hr_nonzero[MB_BITMAP_ADDRS / 8];     /* holding registers that must never be written with 0 */
	int n_ranges;                           /* holding registers whose written value must lie in [lo, hi] */
	struct { u16 addr; short lo, hi; } range[MB_MAX_RANGES];
};

/* one request taken from a Modbus TCP stream */
struct mb_adu {
	u16 tid;
	u8 unit;
	int pdu_len;
	const u8 *pdu;          /* points into the parsed buffer */
};

u16 mb_crc16(const u8 *b, int n);
static inline u16 mb_get16(const u8 *b) { return (u16)((b[0] << 8) | b[1]); }
static inline void mb_put16(u8 *b, u16 v) { b[0] = (u8)(v >> 8); b[1] = (u8)v; }

int mb_bit(const u8 *map, u32 addr);
void mb_set_bit(u8 *map, u32 addr);

/* Modbus TCP: returns bytes consumed (> 0), 0 if more data is needed, -1 if the stream is invalid */
int mb_tcp_parse(const u8 *buf, int len, struct mb_adu *out);
int mb_tcp_build(u16 tid, u8 unit, const u8 *pdu, int pdu_len, u8 *out);
int mb_exception_pdu(u8 fc, u8 code, u8 *out);

/* returns 0 if the request may go to the slave, otherwise the exception code to answer with */
int mb_validate(const struct mb_cfg *cfg, const u8 *pdu, int len);
int mb_is_write(u8 fc);
/* first address and number of addresses a write touches (0 for reads) */
int mb_write_span(const u8 *pdu, u16 *addr, u16 *count);

/* Modbus RTU */
int mb_rtu_build(u8 slave, const u8 *pdu, int pdu_len, u8 *out);
/* expected length of the reply to req: > 0 total frame length, 0 = need more bytes, < 0 = bad frame */
int mb_rtu_expected(u8 slave, const u8 *req, const u8 *rx, int rxlen);
#define RTU_BAD_FRAME  -1
#define RTU_BAD_CRC    -2
#define RTU_BAD_ECHO   -3
/* complete reply: returns the length of its PDU (rx + 1) or one of the RTU_BAD_* codes */
int mb_rtu_check(u8 slave, const u8 *req, int req_len, const u8 *rx, int rxlen);

/* text helpers (no printf) */
struct sbuf { char *p; int len, cap; };
void sb_str(struct sbuf *s, const char *str);
void sb_u32(struct sbuf *s, u32 v);
void sb_i32(struct sbuf *s, int v);
void sb_hex(struct sbuf *s, const u8 *b, int n);
int str_eq(const char *a, const char *b);
int str_len(const char *s);
/* parse a decimal number; returns 0 on success */
int parse_u32(const char *s, u32 *out);
/* "38,92,94" or "100-110,160"; addresses >= MB_BITMAP_ADDRS are an error. returns 0 on success */
int parse_addr_list(const char *s, u8 *map);
/* "38=350..600,159=0..1" (signed raw values); adds to cfg->range. returns 0 on success */
int parse_ranges(const char *s, struct mb_cfg *cfg);

#endif

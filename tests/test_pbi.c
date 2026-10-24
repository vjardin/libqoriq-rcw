/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "rcw_internal.h"

static rcw_ctx_t *
setup_ctx(void) {
  rcw_ctx_t *ctx = rcw_ctx_new();
  ctx->vars.pbiformat = 2;
  ctx->vars.littleendian = true;

  return ctx;
}

/* Helper: read a LE32 word from PBI buffer at offset */
static uint32_t
pbi_word(const rcw_ctx_t *ctx, size_t offset) {
  const uint8_t *p = ctx->pbi.data + offset;

  return (uint32_t)p[0]
      | ((uint32_t)p[1] << 8)
      | ((uint32_t)p[2] << 16)
      | ((uint32_t)p[3] << 24)
      ;
}

/* Test expression evaluator */
static void
test_eval_simple(void **state) {
  (void)state;
  uint32_t result;

  assert_int_equal(rcw_eval_expr("0x1234", &result), RCW_OK);
  assert_int_equal(result, 0x1234);

  assert_int_equal(rcw_eval_expr("42", &result), RCW_OK);
  assert_int_equal(result, 42);

  assert_int_equal(rcw_eval_expr("0x100 + 0x200", &result), RCW_OK);
  assert_int_equal(result, 0x300);
}

static void
test_eval_complex(void **state) {
  (void)state;
  uint32_t result;

  /* Typical macro expansion from serdes_28g.rcw */
  assert_int_equal(rcw_eval_expr("(0x1ea0000 + (0x100 * (0)) + 0x0800)", &result), RCW_OK);
  assert_int_equal(result, 0x1ea0800);

  assert_int_equal(rcw_eval_expr("(0x1ea0000 + (0x40 * (2)) + 0x1200)", &result), RCW_OK);
  assert_int_equal(result, 0x1ea1280);

  /* Bitwise operations */
  assert_int_equal(rcw_eval_expr("((0x10) << 3) & 0x000000F8", &result), RCW_OK);
  assert_int_equal(result, 0x80);
}

/* Operands are Python literals: 0b and 0o exist, C octal does not. */
static void
test_eval_python_literals(void **state) {
  (void)state;
  uint32_t r;

  assert_int_equal(rcw_eval_expr("0b1010", &r), RCW_OK);
  assert_int_equal(r, 10);
  assert_int_equal(rcw_eval_expr("0o17", &r), RCW_OK);
  assert_int_equal(r, 15);
  assert_int_equal(rcw_eval_expr("0x10 + 0B1", &r), RCW_OK);
  assert_int_equal(r, 17);
  assert_int_equal(rcw_eval_expr("1_000", &r), RCW_OK);
  assert_int_equal(r, 1000);
  assert_int_equal(rcw_eval_expr("0", &r), RCW_OK);
  assert_int_equal(r, 0);
  assert_int_equal(rcw_eval_expr("010", &r), RCW_ERR_PBI_SYNTAX);
}

/* Test write command */
static void
test_pbi_write(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "write 0x01e00200,0x12345678");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 8);

  /* write: (3 << 28) | addr */
  assert_int_equal(pbi_word(ctx, 0), (3u << 28) | 0x01e00200);
  assert_int_equal(pbi_word(ctx, 4), 0x12345678);

  rcw_ctx_free(ctx);
}

/* Test write.b1 command */
static void
test_pbi_write_b1(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "write.b1 0x1e60060,0xff");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 8);

  /* write.b1: (1 << 28) | addr */
  assert_int_equal(pbi_word(ctx, 0), (1u << 28) | 0x01e60060);
  assert_int_equal(pbi_word(ctx, 4), 0xff);

  rcw_ctx_free(ctx);
}

/* Test awrite command */
static void
test_pbi_awrite(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "awrite 0x400098,0x00000000");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 8);

  /* awrite: 0x80000000 | (3 << 26) | addr */
  assert_int_equal(pbi_word(ctx, 0), 0x80000000 | (3u << 26) | 0x400098);
  assert_int_equal(pbi_word(ctx, 4), 0x00000000);

  rcw_ctx_free(ctx);
}

/* Test awrite.b4 command */
static void
test_pbi_awrite_b4(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "awrite.b4 0x100000,0xAAAAAAAA,0xBBBBBBBB");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 12);

  assert_int_equal(pbi_word(ctx, 0), 0x80000000 | (4u << 26) | 0x100000);
  assert_int_equal(pbi_word(ctx, 4), 0xAAAAAAAA);
  assert_int_equal(pbi_word(ctx, 8), 0xBBBBBBBB);

  rcw_ctx_free(ctx);
}

/* Test awrite.b5 command */
static void
test_pbi_awrite_b5(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "awrite.b5 0x2508000,0x11111111,0x22222222,0x33333333,0x44444444");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 20);

  assert_int_equal(pbi_word(ctx, 0), 0x80000000 | (5u << 26) | 0x2508000);
  assert_int_equal(pbi_word(ctx, 4), 0x11111111);
  assert_int_equal(pbi_word(ctx, 8), 0x22222222);
  assert_int_equal(pbi_word(ctx, 12), 0x33333333);
  assert_int_equal(pbi_word(ctx, 16), 0x44444444);

  rcw_ctx_free(ctx);
}

/* Test wait command */
static void
test_pbi_wait(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "wait 2500");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 4);

  assert_int_equal(pbi_word(ctx, 0), 0x80820000 | 2500);

  rcw_ctx_free(ctx);
}

/* Test wait command at the field maximum: 0xFFFF must encode cleanly. */
static void
test_pbi_wait_max(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "wait 0xFFFF");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 4);
  assert_int_equal(pbi_word(ctx, 0), 0x80820000 | 0xFFFF);

  rcw_ctx_free(ctx);
}

/*
 * Test wait command overflow: anything past 0xFFFF would silently
 * corrupt the CMD field. The encoder must reject it instead.
 */
static void
test_pbi_wait_overflow(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  /* 0x10000 is the first value that overflows. */
  rcw_error_t err = rcw_pbi_encode_line(ctx, "wait 0x10000");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);

  /* The historical nbxv3 footgun: wait 1000000 (0xF4240). */
  err = rcw_pbi_encode_line(ctx, "wait 1000000");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);

  rcw_ctx_free(ctx);
}

/* Test blockcopy command */
static void
test_pbi_blockcopy(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "blockcopy 0x0,0x01e00210,0x18000000,0x4");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 16);

  assert_int_equal(pbi_word(ctx, 0), 0x80000000);
  assert_int_equal(pbi_word(ctx, 4), 0x01e00210);
  assert_int_equal(pbi_word(ctx, 8), 0x18000000);
  assert_int_equal(pbi_word(ctx, 12), 0x4);

  rcw_ctx_free(ctx);
}

/* Test loadacwindow command */
static void
test_pbi_loadacwindow(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "loadacwindow 0x1C0");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 4);

  assert_int_equal(pbi_word(ctx, 0), 0x80120000 | 0x1C0);

  rcw_ctx_free(ctx);
}

/* Test poll command */
static void
test_pbi_poll(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "poll 0x01e00404,0x00000001,0x00000001");
  assert_int_equal(err, RCW_OK);

  /*
   * Four words per LX2160ARM Section 8.3.11 Table 44: the address has
   * a word of its own and word0's low half is reserved-zero. Note the
   * address here is exactly the case that used to break: ORed into
   * word0 it produced 0x81e00404, whose command byte 0x81 is
   * poll.long, a short poll silently became a long one.
   */
  assert_int_equal(ctx->pbi.len, 16);

  assert_int_equal(pbi_word(ctx, 0), 0x80800000);
  assert_int_equal(pbi_word(ctx, 4), 0x01e00404);
  assert_int_equal(pbi_word(ctx, 8), 0x00000001);
  assert_int_equal(pbi_word(ctx, 12), 0x00000001);

  rcw_ctx_free(ctx);
}

/* poll.long differs only in the command byte; the layout is the same. */
static void
test_pbi_poll_long(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx,
      "poll.long 0x01e00404,0x0000000f,0x00000002");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 16);

  assert_int_equal(pbi_word(ctx, 0), 0x80810000);
  assert_int_equal(pbi_word(ctx, 4), 0x01e00404);
  assert_int_equal(pbi_word(ctx, 8), 0x0000000f);
  assert_int_equal(pbi_word(ctx, 12), 0x00000002);

  rcw_ctx_free(ctx);
}

/*
 * A full 28-bit address must survive intact -- it no longer shares a
 * word with the command byte.
 */
static void
test_pbi_poll_wide_address(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx,
      "poll 0x0fffffff,0xffffffff,0xffffffff");
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 16);

  assert_int_equal(pbi_word(ctx, 0), 0x80800000);  /* command intact */
  assert_int_equal(pbi_word(ctx, 4), 0x0fffffff);  /* address intact */

  rcw_ctx_free(ctx);
}

/*
 * A4 - the parens-only "((((...))))" pattern would blow the C stack
 * via unbounded recursion in eval_primary. The evaluator must hit
 * the depth cap and return RCW_ERR_PBI_SYNTAX cleanly.
 */
static void
test_eval_recursion_capped(void **state) {
  (void)state;
  /* 200 nested '(' - well past the 64-deep cap. */
  char expr[512];
  for (int i = 0; i < 200; i++)
    expr[i] = '(';
  expr[200] = '1';
  for (int i = 201; i < 401; i++)
    expr[i] = ')';
  expr[401] = '\0';

  uint32_t r;
  rcw_error_t err = rcw_eval_expr(expr, &r);
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
}

/*
 * Shifts are Python's: "1 << 32" is 4294967296, which struct.pack('L')
 * refuses. The intermediate is wider
 * than the word, so a shift back brings it into range again.
 */
static void
test_eval_shift_count_overflow(void **state) {
  (void)state;
  uint32_t r;

  assert_int_equal(rcw_eval_expr("1 << 32", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("(1 << 32) >> 1", &r), RCW_OK);
  assert_int_equal(r, 0x80000000u);
  assert_int_equal(rcw_eval_expr("0xFFFFFFFF >> 32", &r), RCW_OK);
  assert_int_equal(r, 0);
  assert_int_equal(rcw_eval_expr("1 << -1", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("1 << 63", &r), RCW_ERR_PBI_SYNTAX);

  /* Sanity: legal shifts still work */
  assert_int_equal(rcw_eval_expr("1 << 31", &r), RCW_OK);
  assert_int_equal(r, 0x80000000u);
  assert_int_equal(rcw_eval_expr("0xFFFFFFFF >> 31", &r), RCW_OK);
  assert_int_equal(r, 1);
}

/*
 * The whole string must be one Python expression.
 * Text the grammar does not consume used to be
 * ignored here, so "0x5 junk" compiled as 5 and "0x10%3" as 0x10.
 */
static void
test_eval_rejects_trailing_text(void **state) {
  (void)state;
  uint32_t r;

  assert_int_equal(rcw_eval_expr("0x5 junk", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("0x10 0x20", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("0x10 %", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("(1", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("1)", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("1 ? 2 : 3", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("1 == 1", &r), RCW_ERR_PBI_SYNTAX);
}

/* Python precedence and the operators eval() offers on integers. */
static void
test_eval_python_operators(void **state) {
  (void)state;
  uint32_t r;

  assert_int_equal(rcw_eval_expr("7 % 3", &r), RCW_OK);
  assert_int_equal(r, 1);
  assert_int_equal(rcw_eval_expr("0x10 % 3", &r), RCW_OK);
  assert_int_equal(r, 1);
  /* floor division: -7 // 2 is -4 in Python, not -3 */
  assert_int_equal(rcw_eval_expr("-7 // 2 + 10", &r), RCW_OK);
  assert_int_equal(r, 6);
  assert_int_equal(rcw_eval_expr("-7 % 2", &r), RCW_OK);
  assert_int_equal(r, 1);
  assert_int_equal(rcw_eval_expr("2 ** 10", &r), RCW_OK);
  assert_int_equal(r, 1024);
  assert_int_equal(rcw_eval_expr("2 ** 31", &r), RCW_OK);
  assert_int_equal(r, 0x80000000u);
  /* ** binds tighter than *, shifts bind looser than + */
  assert_int_equal(rcw_eval_expr("2 * 3 ** 2", &r), RCW_OK);
  assert_int_equal(r, 18);
  assert_int_equal(rcw_eval_expr("4 - 2 ** 2", &r), RCW_OK);
  assert_int_equal(r, 0);
  assert_int_equal(rcw_eval_expr("1 + 2 << 3", &r), RCW_OK);
  assert_int_equal(r, 24);
  /* | ^ & from loosest to tightest */
  assert_int_equal(rcw_eval_expr("1 | 2 ^ 3 & 4", &r), RCW_OK);
  assert_int_equal(r, 3);
  assert_int_equal(rcw_eval_expr("+5", &r), RCW_OK);
  assert_int_equal(r, 5);
  assert_int_equal(rcw_eval_expr("-1 + 2", &r), RCW_OK);
  assert_int_equal(r, 1);
  assert_int_equal(rcw_eval_expr("~0 & 0xff", &r), RCW_OK);
  assert_int_equal(r, 0xff);
  assert_int_equal(rcw_eval_expr("(0xf << 23)", &r), RCW_OK);
  assert_int_equal(r, 0x07800000u);
}

/*
 * struct.pack('L') takes 0..0xFFFFFFFF and nothing else, and Python has
 * no 32-bit wrap.
 */
static void
test_eval_rejects_what_struct_pack_rejects(void **state) {
  (void)state;
  uint32_t r;

  assert_int_equal(rcw_eval_expr("0xffffffff + 1", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("2 ** 32", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("~0", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("-1", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("0x10 / 4", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("2 ** -1", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("1 % 0", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("1 // 0", &r), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_eval_expr("0xffffffff * 0xffffffff * 0xffffffff", &r),
                   RCW_ERR_PBI_SYNTAX);

  /* a 64-bit detour is fine as long as the result fits */
  assert_int_equal(rcw_eval_expr("0xffffffff + 1 - 1", &r), RCW_OK);
  assert_int_equal(r, 0xffffffffu);
  assert_int_equal(rcw_eval_expr("0xffffffff * 0x10 // 0x10", &r), RCW_OK);
  assert_int_equal(r, 0xffffffffu);
}

/* The encoder names the operand and the reason. */
static void
test_pbi_operand_error_is_explained(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();
  char why[RCW_EVAL_WHY_MAX];
  uint32_t r;

  assert_int_equal(rcw_pbi_encode_line(ctx, "write 0x1e00400,0x5 junk"),
                   RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);
  assert_non_null(strstr(rcw_ctx_last_error_detail(ctx), "'0x5 junk'"));
  assert_non_null(strstr(rcw_ctx_last_error_detail(ctx), "unexpected 'junk'"));

  assert_int_equal(rcw_eval_expr_why("0xffffffff + 1", &r, why, sizeof(why)),
                   RCW_ERR_PBI_SYNTAX);
  assert_non_null(strstr(why, "4294967296 does not fit"));
  assert_int_equal(rcw_eval_expr_why("1 // 0", &r, why, sizeof(why)),
                   RCW_ERR_PBI_SYNTAX);
  assert_string_equal(why, "division by zero");

  rcw_ctx_free(ctx);
}

/*
 * B2 - integer literal that does not fit in uint32_t (e.g.
 * 0x100000000) must be rejected, not silently truncated.
 */
static void
test_eval_numeric_literal_overflow(void **state) {
  (void)state;
  uint32_t r;

  rcw_error_t err = rcw_eval_expr("0x100000000", &r);
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);

  /* UINT32_MAX still works */
  assert_int_equal(rcw_eval_expr("0xFFFFFFFF", &r), RCW_OK);
  assert_int_equal(r, 0xFFFFFFFFu);
}

/*
 * B1 - addresses that exceed the per-command field width OR-overflow
 * into the command-type bits and silently corrupt the encoded
 * command. Each opcode must reject upfront.
 */
static void
test_pbi_write_addr_overflow(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  /* CCSR address field is [27:0]; 0x10000000 sets bit 28 and
   * silently changes the byte-count field from B=3 to B=2 (RESERVED). */
  rcw_error_t err = rcw_pbi_encode_line(ctx, "write 0x10000000,0xCAFE");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);

  /* And the upper-bound address is still accepted. */
  err = rcw_pbi_encode_line(ctx, "write 0x0FFFFFFF,0xCAFE");
  assert_int_equal(err, RCW_OK);

  rcw_ctx_free(ctx);
}

static void
test_pbi_write_b1_addr_overflow(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_pbi_encode_line(ctx, "write.b1 0x10000000,0xff");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);

  rcw_ctx_free(ctx);
}

static void
test_pbi_awrite_addr_overflow(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  /* AltConfig offset field is [25:0]; 0x4000000 corrupts B-field. */
  rcw_error_t err = rcw_pbi_encode_line(ctx, "awrite 0x4000000,0");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);

  err = rcw_pbi_encode_line(ctx, "awrite.b4 0x4000000,0,0");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);

  err = rcw_pbi_encode_line(ctx, "awrite.b5 0x4000000,0,0,0,0");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);

  /* Upper-bound offsets still encode. */
  err = rcw_pbi_encode_line(ctx, "awrite 0x3FFFFFC,0");
  assert_int_equal(err, RCW_OK);
  err = rcw_pbi_encode_line(ctx, "awrite.b1 0x3FFFFFF,0");
  assert_int_equal(err, RCW_OK);

  rcw_ctx_free(ctx);
}

static void
test_pbi_loadacwindow_overflow(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  /* 14-bit field; 0x4000 sets bit 14 and corrupts the CMD byte. */
  rcw_error_t err = rcw_pbi_encode_line(ctx, "loadacwindow 0x4000");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);

  err = rcw_pbi_encode_line(ctx, "loadacwindow 0x3FFF");
  assert_int_equal(err, RCW_OK);

  rcw_ctx_free(ctx);
}

static void
test_pbi_blockcopy_src_overflow(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  /* SRC interface byte is 8 bits; 0x100 spills into the CMD byte. */
  rcw_error_t err = rcw_pbi_encode_line(ctx,
      "blockcopy 0x100,0x01e00210,0x18000000,0x4");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);

  rcw_ctx_free(ctx);
}

/*
 * B2 - extra parameters past PBI_MAX_PARAMS used to be silently
 * dropped; the encoder must now report it.
 */
static void
test_pbi_too_many_params(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  /* awrite.b5 takes 5 params; supply 6 */
  rcw_error_t err = rcw_pbi_encode_line(ctx,
      "awrite.b5 0x100,0x1,0x2,0x3,0x4,0x5");
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, 0);

  rcw_ctx_free(ctx);
}

/*
 * AltConfig Write widths: B in 1..10 selects N = 2^(B-1) bytes
 * in ceil(N/4) data words
 */
static void
test_pbi_awrite_every_width(void **state) {
  (void)state;
  static const struct { unsigned b; int nwords; } widths[] = {
    { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 2 }, { 5, 4 }, { 6, 8 },
    { 7, 16 }, { 8, 32 }, { 9, 64 }, { 10, 128 },
  };
  char line[2048];

  for (size_t t = 0; t < sizeof(widths) / sizeof(widths[0]); t++) {
    rcw_ctx_t *ctx = setup_ctx();
    int n = snprintf(line, sizeof(line), "awrite.b%u 0x100", widths[t].b);
    for (int k = 0; k < widths[t].nwords; k++)
      n += snprintf(line + n, sizeof(line) - (size_t)n, ",0x%08x", 0x11110000u + (unsigned)k);

    assert_int_equal(rcw_pbi_encode_line(ctx, line), RCW_OK);
    assert_int_equal(ctx->pbi.len, 4u * (1u + (unsigned)widths[t].nwords));
    assert_int_equal(pbi_word(ctx, 0), 0x80000000u | (widths[t].b << 26) | 0x100u);
    for (int k = 0; k < widths[t].nwords; k++)
      assert_int_equal(pbi_word(ctx, 4u * (1u + (unsigned)k)), 0x11110000u + (unsigned)k);
    rcw_ctx_free(ctx);
  }

  rcw_ctx_t *ctx = setup_ctx();
  assert_int_equal(rcw_pbi_encode_line(ctx, "awrite.b1 0x100,0x5a"), RCW_OK);
  assert_int_equal(pbi_word(ctx, 0), 0x84000100u);
  assert_int_equal(pbi_word(ctx, 4), 0x0000005au);
  rcw_ctx_free(ctx);
}

/* The suffix is the B field: nothing outside .b1 to .b10 encodes. */
static void
test_pbi_awrite_bad_width(void **state) {
  (void)state;
  static const char *lines[] = {
    "awrite.b0 0x100,0x1",
    "awrite.b11 0x100,0x1",
    "awrite.b01 0x100,0x1",
    "awrite.bx 0x100,0x1",
    "awrite.short 0x100,0x1",
    "awrite.b 0x100,0x1",
  };

  for (size_t t = 0; t < sizeof(lines) / sizeof(lines[0]); t++) {
    rcw_ctx_t *ctx = setup_ctx();
    assert_int_equal(rcw_pbi_encode_line(ctx, lines[t]), RCW_ERR_PBI_SYNTAX);
    assert_int_equal(ctx->pbi.len, 0);
    rcw_ctx_free(ctx);
  }
}

/* The operand count follows the width: one too few or too many is an error. */
static void
test_pbi_awrite_operand_count(void **state) {
  (void)state;
  static const char *lines[] = {
    "awrite 0x100",
    "awrite 0x100,0x1,0x2",
    "awrite.b1 0x100,0x1,0x2",
    "awrite.b4 0x100,0x1",
    "awrite.b4 0x100,0x1,0x2,0x3",
    "awrite.b6 0x100,0x1,0x2,0x3,0x4,0x5,0x6,0x7",
  };

  for (size_t t = 0; t < sizeof(lines) / sizeof(lines[0]); t++) {
    rcw_ctx_t *ctx = setup_ctx();
    assert_int_equal(rcw_pbi_encode_line(ctx, lines[t]), RCW_ERR_PBI_SYNTAX);
    assert_int_equal(ctx->pbi.len, 0);
    rcw_ctx_free(ctx);
  }
}

/* For B above 2, ALTCFG_OFFSET must be word aligned. */
static void
test_pbi_awrite_alignment(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  assert_int_equal(rcw_pbi_encode_line(ctx, "awrite.b1 0x101,0x5a"), RCW_OK);
  assert_int_equal(rcw_pbi_encode_line(ctx, "awrite.b2 0x102,0x5a5a"), RCW_OK);
  assert_int_equal(rcw_pbi_encode_line(ctx, "awrite 0x104,0x1"), RCW_OK);
  size_t len = ctx->pbi.len;

  assert_int_equal(rcw_pbi_encode_line(ctx, "awrite 0x102,0x1"), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_pbi_encode_line(ctx, "awrite.b4 0x101,0x1,0x2"), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(rcw_pbi_encode_line(ctx, "awrite.b5 0x103,0x1,0x2,0x3,0x4"), RCW_ERR_PBI_SYNTAX);
  assert_int_equal(ctx->pbi.len, len);
  assert_non_null(strstr(rcw_ctx_last_error_detail(ctx), "word aligned"));

  rcw_ctx_free(ctx);
}

/*
 * It is a legacy pbiformat=1 construct: an 8-byte CCSR write into the PBL
 * address space that forces a read-back of the previous write, encoded as
 * PBL_CMD_FLUSH | (pbladdr & RCW_PBLADDR_MASK) followed by a zero word.
 *
 * pbiformat=2 does not use it, but the encoder still accepts it for
 * compatibility, so it is exactly the kind of path that can rot unnoticed.
 */
static void
test_pbi_flush_encoding(void **state) {
  (void)state;

  static const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n"
    "SYS_PLL_RAT=7\n"
    ".pbi\n"
    "flush\n"
    ".end\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);
  uint8_t *bin = NULL;
  size_t len = 0;
  assert_int_equal(rcw_compile_buffer(ctx, src, strlen(src), &bin, &len),
                   RCW_OK);
  assert_non_null(bin);

  /* Two words: the flush header, then a zero. */
  int found = 0;
  for (size_t i = 0; i + 8 <= len; i += 4) {
    uint32_t w = (uint32_t)bin[i] | ((uint32_t)bin[i + 1] << 8) |
                 ((uint32_t)bin[i + 2] << 16) | ((uint32_t)bin[i + 3] << 24);
    uint32_t n = (uint32_t)bin[i + 4] | ((uint32_t)bin[i + 5] << 8) |
                 ((uint32_t)bin[i + 6] << 16) | ((uint32_t)bin[i + 7] << 24);
    if ((w & 0xFF000000u) == PBL_CMD_FLUSH && n == 0u) {
      found = 1;
      break;
    }
  }
  assert_int_equal(found, 1);

  rcw_free(bin);
  rcw_ctx_free(ctx);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_pbi_flush_encoding),
    cmocka_unit_test(test_eval_simple),
    cmocka_unit_test(test_eval_complex),
    cmocka_unit_test(test_eval_python_literals),
    cmocka_unit_test(test_pbi_write),
    cmocka_unit_test(test_pbi_write_b1),
    cmocka_unit_test(test_pbi_awrite),
    cmocka_unit_test(test_pbi_awrite_b4),
    cmocka_unit_test(test_pbi_awrite_b5),
    cmocka_unit_test(test_pbi_wait),
    cmocka_unit_test(test_pbi_wait_max),
    cmocka_unit_test(test_pbi_wait_overflow),
    cmocka_unit_test(test_pbi_blockcopy),
    cmocka_unit_test(test_pbi_loadacwindow),
    cmocka_unit_test(test_pbi_poll),
    cmocka_unit_test(test_pbi_poll_long),
    cmocka_unit_test(test_pbi_poll_wide_address),
    cmocka_unit_test(test_eval_recursion_capped),
    cmocka_unit_test(test_eval_shift_count_overflow),
    cmocka_unit_test(test_eval_rejects_trailing_text),
    cmocka_unit_test(test_eval_python_operators),
    cmocka_unit_test(test_eval_rejects_what_struct_pack_rejects),
    cmocka_unit_test(test_pbi_operand_error_is_explained),
    cmocka_unit_test(test_eval_numeric_literal_overflow),
    cmocka_unit_test(test_pbi_write_addr_overflow),
    cmocka_unit_test(test_pbi_write_b1_addr_overflow),
    cmocka_unit_test(test_pbi_awrite_addr_overflow),
    cmocka_unit_test(test_pbi_loadacwindow_overflow),
    cmocka_unit_test(test_pbi_blockcopy_src_overflow),
    cmocka_unit_test(test_pbi_too_many_params),
    cmocka_unit_test(test_pbi_awrite_every_width),
    cmocka_unit_test(test_pbi_awrite_bad_width),
    cmocka_unit_test(test_pbi_awrite_operand_count),
    cmocka_unit_test(test_pbi_awrite_alignment),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

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
  assert_int_equal(ctx->pbi.len, 12);

  assert_int_equal(pbi_word(ctx, 0), 0x80800000 | 0x01e00404);
  assert_int_equal(pbi_word(ctx, 4), 0x00000001);
  assert_int_equal(pbi_word(ctx, 8), 0x00000001);

  rcw_ctx_free(ctx);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_eval_simple),
    cmocka_unit_test(test_eval_complex),
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
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

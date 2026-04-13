/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * Tests for the .uboot subsection encoder (rcw_uboot.c).
 *
 * The .uboot input is an xxd hex dump (8 16-bit words per line, 16 bytes
 * per line). Grouping rules (mirroring rcw.py build_pbi_uboot):
 *   - The last 4 lines are emitted as 2 pairs (32 bytes each), prefixed
 *     by header word 0xC1F80000 + (line2_addr - 0x10).
 *   - Every preceding 4-line group is emitted as a quad (64 bytes),
 *     prefixed by header word 0x81F80000 + (line4_addr - 0x30).
 * All output is big-endian regardless of %littleendian.
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
  ctx->vars.littleendian = true; /* irrelevant for .uboot - output is BE */

  return ctx;
}

/* Read a 32-bit big-endian word from the PBI buffer. */
static uint32_t
pbi_be32(const rcw_ctx_t *ctx, size_t offset) {
  const uint8_t *p = ctx->pbi.data + offset;

  return ((uint32_t)p[0] << 24)
       | ((uint32_t)p[1] << 16)
       | ((uint32_t)p[2] << 8)
       |  (uint32_t)p[3]
       ;
}

/* Read a 16-bit big-endian word from the PBI buffer. */
static uint16_t
pbi_be16(const rcw_ctx_t *ctx, size_t offset) {
  const uint8_t *p = ctx->pbi.data + offset;

  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/*
 * Test 1: a 4-line input. All 4 lines fall in the "last 4 lines"
 * tail, so the encoder emits 2 pairs (no quad).
 *
 * Layout: addresses 0x00, 0x10, 0x20, 0x30; data words are i*0x100+j.
 * Expected:
 *   pair 1: header=0xC1F80000+(0x10-0x10)=0xC1F80000, then 16 BE16 words
 *           from lines 1+2.
 *   pair 2: header=0xC1F80000+(0x30-0x10)=0xC1F80020, then 16 BE16 words
 *           from lines 3+4.
 * Total bytes: 4+32 + 4+32 = 72.
 */
static void
test_uboot_four_lines_two_pairs(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  static const char body[] =
    "00000000: 0001 0203 0405 0607 0809 0a0b 0c0d 0e0f\n"
    "00000010: 1011 1213 1415 1617 1819 1a1b 1c1d 1e1f\n"
    "00000020: 2021 2223 2425 2627 2829 2a2b 2c2d 2e2f\n"
    "00000030: 3031 3233 3435 3637 3839 3a3b 3c3d 3e3f\n";

  rcw_error_t err = rcw_uboot_encode(ctx, body, strlen(body));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 72);

  /* Pair 1 header */
  assert_int_equal(pbi_be32(ctx, 0), 0xC1F80000u);
  /* First few words of pair 1 (line 1 starts at offset 4) */
  assert_int_equal(pbi_be16(ctx, 4),  0x0001);
  assert_int_equal(pbi_be16(ctx, 6),  0x0203);
  assert_int_equal(pbi_be16(ctx, 8),  0x0405);
  /* Last word of line 1 (offset 4 + 7*2 = 18) */
  assert_int_equal(pbi_be16(ctx, 18), 0x0e0f);
  /* First word of line 2 */
  assert_int_equal(pbi_be16(ctx, 20), 0x1011);
  /* Last word of pair 1 = last word of line 2 (offset 4 + 15*2 = 34) */
  assert_int_equal(pbi_be16(ctx, 34), 0x1e1f);

  /* Pair 2 header at offset 36 */
  assert_int_equal(pbi_be32(ctx, 36), 0xC1F80020u);
  /* First word of line 3 */
  assert_int_equal(pbi_be16(ctx, 40), 0x2021);
  /* Last word of line 4 (offset 36 + 4 + 15*2 = 70) */
  assert_int_equal(pbi_be16(ctx, 70), 0x3e3f);

  rcw_ctx_free(ctx);
}

/*
 * Test 2: an 8-line input. Lines 1-4 form a quad (the first 4 are not
 * in the last-4 tail). Lines 5-8 are the tail and emit 2 pairs.
 *
 * Expected:
 *   quad: header=0x81F80000+(0x30-0x30)=0x81F80000, 32 BE16 words.
 *   pair 1: header=0xC1F80000+(0x50-0x10)=0xC1F80040, 16 BE16 words.
 *   pair 2: header=0xC1F80000+(0x70-0x10)=0xC1F80060, 16 BE16 words.
 * Total bytes: 4+64 + 4+32 + 4+32 = 140.
 */
static void
test_uboot_eight_lines_quad_plus_pairs(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  static const char body[] =
    "00000000: 0001 0203 0405 0607 0809 0a0b 0c0d 0e0f\n"
    "00000010: 1011 1213 1415 1617 1819 1a1b 1c1d 1e1f\n"
    "00000020: 2021 2223 2425 2627 2829 2a2b 2c2d 2e2f\n"
    "00000030: 3031 3233 3435 3637 3839 3a3b 3c3d 3e3f\n"
    "00000040: 4041 4243 4445 4647 4849 4a4b 4c4d 4e4f\n"
    "00000050: 5051 5253 5455 5657 5859 5a5b 5c5d 5e5f\n"
    "00000060: 6061 6263 6465 6667 6869 6a6b 6c6d 6e6f\n"
    "00000070: 7071 7273 7475 7677 7879 7a7b 7c7d 7e7f\n";

  rcw_error_t err = rcw_uboot_encode(ctx, body, strlen(body));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 140);

  /* --- quad block --- */
  assert_int_equal(pbi_be32(ctx, 0), 0x81F80000u);
  /* First word of line 1 (offset 4) */
  assert_int_equal(pbi_be16(ctx, 4), 0x0001);
  /* First word of line 2 (offset 4 + 8*2) */
  assert_int_equal(pbi_be16(ctx, 20), 0x1011);
  /* First word of line 3 (offset 4 + 16*2) */
  assert_int_equal(pbi_be16(ctx, 36), 0x2021);
  /* Last word of quad = last word of line 4 (offset 4 + 31*2 = 66) */
  assert_int_equal(pbi_be16(ctx, 66), 0x3e3f);

  /* --- pair 1 (lines 5-6) starts at offset 68 --- */
  assert_int_equal(pbi_be32(ctx, 68), 0xC1F80040u);
  assert_int_equal(pbi_be16(ctx, 72), 0x4041);
  /* Last word of pair 1 = last word of line 6 (offset 68 + 4 + 15*2 = 102) */
  assert_int_equal(pbi_be16(ctx, 102), 0x5e5f);

  /* --- pair 2 (lines 7-8) starts at offset 104 --- */
  assert_int_equal(pbi_be32(ctx, 104), 0xC1F80060u);
  assert_int_equal(pbi_be16(ctx, 108), 0x6061);
  /* Last word = last word of line 8 (offset 104 + 4 + 15*2 = 138) */
  assert_int_equal(pbi_be16(ctx, 138), 0x7e7f);

  rcw_ctx_free(ctx);
}

/*
 * Test 3: blank lines inside the .uboot block must be skipped (matches
 * rcw.py which silently ignores them). Same expected output as test 1.
 */
static void
test_uboot_skips_blank_lines(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  static const char body[] =
    "\n"
    "00000000: 0001 0203 0405 0607 0809 0a0b 0c0d 0e0f\n"
    "   \n"
    "00000010: 1011 1213 1415 1617 1819 1a1b 1c1d 1e1f\n"
    "00000020: 2021 2223 2425 2627 2829 2a2b 2c2d 2e2f\n"
    "00000030: 3031 3233 3435 3637 3839 3a3b 3c3d 3e3f\n";

  rcw_error_t err = rcw_uboot_encode(ctx, body, strlen(body));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 72);
  assert_int_equal(pbi_be32(ctx, 0),  0xC1F80000u);
  assert_int_equal(pbi_be32(ctx, 36), 0xC1F80020u);

  rcw_ctx_free(ctx);
}

/*
 * Test 4: end-to-end via the parser. Verifies that the parser
 * recognizes the .uboot/.end markers and dispatches to the encoder.
 */
static void
test_uboot_via_parser(void **state) {
  (void)state;

  static const char source[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "PBI_LENGTH[287:276]\n"
    ".uboot\n"
    "00000000: 0001 0203 0405 0607 0809 0a0b 0c0d 0e0f\n"
    "00000010: 1011 1213 1415 1617 1819 1a1b 1c1d 1e1f\n"
    "00000020: 2021 2223 2425 2627 2829 2a2b 2c2d 2e2f\n"
    "00000030: 3031 3233 3435 3637 3839 3a3b 3c3d 3e3f\n"
    ".end\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_error_t err = rcw_parse(ctx, source, strlen(source));
  assert_int_equal(err, RCW_OK);

  /* The parser should have routed the .uboot body through the encoder */
  assert_int_equal(ctx->pbi.len, 72);
  assert_int_equal(pbi_be32(ctx, 0),  0xC1F80000u);
  assert_int_equal(pbi_be32(ctx, 36), 0xC1F80020u);

  rcw_ctx_free(ctx);
}

/*
 * Test 5: empty .uboot block produces no PBI output and no error.
 */
static void
test_uboot_empty(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  rcw_error_t err = rcw_uboot_encode(ctx, "", 0);
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 0);

  static const char blanks[] = "\n\n   \n";
  err = rcw_uboot_encode(ctx, blanks, strlen(blanks));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->pbi.len, 0);

  rcw_ctx_free(ctx);
}

/*
 * Test 6: malformed line (missing ':' after address) -> RCW_ERR_PBI_SYNTAX.
 */
static void
test_uboot_bad_address(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  static const char body[] = "notavalidline\n";
  rcw_error_t err = rcw_uboot_encode(ctx, body, strlen(body));
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);

  rcw_ctx_free(ctx);
}

/*
 * Test 7: malformed line (truncated - fewer than 8 hex words) ->
 * RCW_ERR_PBI_SYNTAX.
 */
static void
test_uboot_truncated_line(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  static const char body[] = "00000000: 0001 0203\n";
  rcw_error_t err = rcw_uboot_encode(ctx, body, strlen(body));
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);

  rcw_ctx_free(ctx);
}

/*
 * Test 8: line longer than RCW_UBOOT_LINE_MAX -> RCW_ERR_PBI_SYNTAX.
 */
static void
test_uboot_line_too_long(void **state) {
  (void)state;
  rcw_ctx_t *ctx = setup_ctx();

  /* Build a line with > RCW_UBOOT_LINE_MAX chars */
  char body[RCW_UBOOT_LINE_MAX + 64];
  memset(body, 'X', sizeof(body) - 1);
  body[sizeof(body) - 1] = '\n';

  rcw_error_t err = rcw_uboot_encode(ctx, body, sizeof(body));
  assert_int_equal(err, RCW_ERR_PBI_SYNTAX);

  rcw_ctx_free(ctx);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_uboot_four_lines_two_pairs),
    cmocka_unit_test(test_uboot_eight_lines_quad_plus_pairs),
    cmocka_unit_test(test_uboot_skips_blank_lines),
    cmocka_unit_test(test_uboot_via_parser),
    cmocka_unit_test(test_uboot_empty),
    cmocka_unit_test(test_uboot_bad_address),
    cmocka_unit_test(test_uboot_truncated_line),
    cmocka_unit_test(test_uboot_line_too_long),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBI density commands: CCSR Write from Address (0x02) and CCSR
 * Read-Modify-Write (0x42).
 *
 * Both exist to express in one command what would otherwise be a run of
 * blind writes
 *  - 0x02 loads N consecutive CCSR registers from memory,
 *  - 0x42 sets/clears/replaces bits under a mask without the caller having to
 *   know the register's other contents.
 *
 *   Table 37  CCSR Write from Address  16 B  HDR=0x80 CMD=0x02 |wtype
 *                                            SRC | DEST | COPY LENGTH
 *   Table 43  CCSR Read-Modify-Write   16 B  HDR=0x80 CMD=0x42 |ops|mtype
 *                                            ADDR | MASK | DATA
 *
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rcw_internal.h"

#define HDR                        \
  "%size=1024\n"                   \
  "%pbiformat=2\n"                 \
  "%classicbitnumbers=1\n"         \
  "%littleendian=1\n"              \
  "%nocrc=1\n"                     \
  "SYS_PLL_RAT[6:2]\n"             \
  "PBI_LENGTH[287:276]\n"          \
  "SYS_PLL_RAT=7\n"

static uint8_t *compile_ok(const char *src, size_t *len) {
  rcw_ctx_t *c = rcw_ctx_new();
  assert_non_null(c);
  uint8_t *bin = NULL;
  assert_int_equal(rcw_compile_buffer(c, src, strlen(src), &bin, len), RCW_OK);
  assert_non_null(bin);
  rcw_ctx_free(c);
  return bin;
}

static void compile_fails(const char *src) {
  rcw_ctx_t *c = rcw_ctx_new();
  assert_non_null(c);
  uint8_t *bin = NULL;
  size_t len = 0;
  assert_int_not_equal(rcw_compile_buffer(c, src, strlen(src), &bin, &len),
                       RCW_OK);
  free(bin);
  rcw_ctx_free(c);
}

static uint32_t word_at(const uint8_t *b, size_t off) {
  return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
         ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static long find_word(const uint8_t *b, size_t len, uint32_t want) {
  for (size_t i = 0; i + 4 <= len; i += 4)
    if (word_at(b, i) == want)
      return (long)i;
  return -1;
}

static void test_ccsrcopy_encoding(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(
      HDR ".pbi\nccsrcopy.b4 0x18000000,0x01E00300,0x00000010\n.end\n", &len);

  /* CMD 0x02, word type 3 (32-bit destination registers). */
  long at = find_word(bin, len, 0x80020003u);
  assert_true(at >= 0);
  assert_int_equal(word_at(bin, (size_t)at + 4),  0x18000000u); /* SRC   */
  assert_int_equal(word_at(bin, (size_t)at + 8),  0x01E00300u); /* DEST  */
  assert_int_equal(word_at(bin, (size_t)at + 12), 0x00000010u); /* WORDS */
  free(bin);
}

/* Word type is 1/2/3 for 8/16/32-bit destination registers; 0 is invalid. */
static void test_ccsrcopy_word_types(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *b1 = compile_ok(
      HDR ".pbi\nccsrcopy.b1 0x1,0x01E00300,0x1\n.end\n", &len);
  assert_true(find_word(b1, len, 0x80020001u) >= 0);
  uint8_t *b2 = compile_ok(
      HDR ".pbi\nccsrcopy.b2 0x1,0x01E00300,0x1\n.end\n", &len);
  assert_true(find_word(b2, len, 0x80020002u) >= 0);
  free(b1); free(b2);
}

/*
 * ops occupies bits [3:2] and mask type bits [1:0], so the header low byte
 * is (ops << 2) | mtype: set=0x03, clear=0x07, replace=0x0B at .b4.
 */
static void test_rmw_ops_encoding(void **state) {
  (void)state;
  size_t len = 0;

  uint8_t *bs = compile_ok(
      HDR ".pbi\nccsrset.b4 0x01E00400,0x0000000F\n.end\n", &len);
  long at = find_word(bs, len, 0x80420003u);
  assert_true(at >= 0);
  assert_int_equal(word_at(bs, (size_t)at + 4), 0x01E00400u);
  assert_int_equal(word_at(bs, (size_t)at + 8), 0x0000000Fu);
  /* Data word is emitted but ignored by the SP for set/clear. */
  assert_int_equal(word_at(bs, (size_t)at + 12), 0u);

  uint8_t *bc = compile_ok(
      HDR ".pbi\nccsrclear.b4 0x01E00404,0x000000F0\n.end\n", &len);
  assert_true(find_word(bc, len, 0x80420007u) >= 0);

  uint8_t *br = compile_ok(
      HDR ".pbi\nccsrreplace.b4 0x01E00408,0x0000FF00,0x00005500\n.end\n",
      &len);
  at = find_word(br, len, 0x8042000Bu);
  assert_true(at >= 0);
  assert_int_equal(word_at(br, (size_t)at + 12), 0x00005500u); /* DATA */

  free(bs); free(bc); free(br);
}

static void test_rmw_mask_types(void **state) {
  (void)state;
  size_t len = 0;
  /* .b1 needs no alignment, .b2 needs 2-byte, .b4 needs 4-byte. */
  uint8_t *b1 = compile_ok(
      HDR ".pbi\nccsrset.b1 0x01E00401,0x0F\n.end\n", &len);
  assert_true(find_word(b1, len, 0x80420001u) >= 0);
  uint8_t *b2 = compile_ok(
      HDR ".pbi\nccsrset.b2 0x01E00402,0x0FFF\n.end\n", &len);
  assert_true(find_word(b2, len, 0x80420002u) >= 0);
  free(b1); free(b2);
}

static void test_sizes(void **state) {
  (void)state;
  size_t base = 0, l_copy = 0, l_rmw = 0;
  uint8_t *b0 = compile_ok(HDR ".pbi\n.end\n", &base);
  uint8_t *b1 = compile_ok(
      HDR ".pbi\nccsrcopy.b4 0x1,0x01E00300,0x1\n.end\n", &l_copy);
  uint8_t *b2 = compile_ok(
      HDR ".pbi\nccsrset.b4 0x01E00400,0x1\n.end\n", &l_rmw);
  assert_int_equal(l_copy - base, 16);   /* RM Table 37 */
  assert_int_equal(l_rmw  - base, 16);   /* RM Table 43 */
  free(b0); free(b1); free(b2);
}

/* Every error check the RM lists for Table 43. */
static void test_rmw_guards(void **state) {
  (void)state;
  /* mask type 0 is invalid -> a width suffix is mandatory */
  compile_fails(HDR ".pbi\nccsrset 0x01E00400,0x1\n.end\n");
  /* destination must be above 0x100_0000 */
  compile_fails(HDR ".pbi\nccsrset.b4 0x00FF0000,0x1\n.end\n");
  /* 4-byte mask needs a 4-byte aligned address */
  compile_fails(HDR ".pbi\nccsrset.b4 0x01E00402,0x1\n.end\n");
  /* 2-byte mask needs a 2-byte aligned address */
  compile_fails(HDR ".pbi\nccsrset.b2 0x01E00401,0x1\n.end\n");
  /* replace needs its data operand */
  compile_fails(HDR ".pbi\nccsrreplace.b4 0x01E00400,0xF\n.end\n");
}

static void test_ccsrcopy_guards(void **state) {
  (void)state;
  /* word type 0 is invalid -> suffix mandatory */
  compile_fails(HDR ".pbi\nccsrcopy 0x1,0x01E00300,0x4\n.end\n");
  compile_fails(HDR ".pbi\nccsrcopy.b4 0x1,0x01E00300\n.end\n");
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_ccsrcopy_encoding),
    cmocka_unit_test(test_ccsrcopy_word_types),
    cmocka_unit_test(test_rmw_ops_encoding),
    cmocka_unit_test(test_rmw_mask_types),
    cmocka_unit_test(test_sizes),
    cmocka_unit_test(test_rmw_guards),
    cmocka_unit_test(test_ccsrcopy_guards),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

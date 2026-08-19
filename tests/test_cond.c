/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBI conditional-execution commands: Load Condition (0x14), Jump (0x84)
 * and Conditional Jump (0x85).
 *
 * Both jumps are FORWARD ONLY. The RM specifies two error checks for each
 * (4-byte alignment, and OFFSET strictly greater than 8) and both are
 * asserted below, because a misaligned offset lands mid-command and an
 * offset of 8 targets the word immediately after the command.
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

/* Minimal preamble: enough field definitions for a valid image. */
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

/* Little-endian word fetch, matching %littleendian=1 above. */
static uint32_t word_at(const uint8_t *b, size_t off) {
  return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
         ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

/* Find a 32-bit word in the image; -1 if absent. */
static long find_word(const uint8_t *b, size_t len, uint32_t want) {
  for (size_t i = 0; i + 4 <= len; i += 4)
    if (word_at(b, i) == want)
      return (long)i;
  return -1;
}

static void test_loadcondition_encoding(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(
      HDR ".pbi\nloadcondition 0x01E00100,0x000000FF\n.end\n", &len);

  long at = find_word(bin, len, 0x80140000u);   /* RM Table 40 header */
  assert_true(at >= 0);
  assert_int_equal(word_at(bin, (size_t)at + 4), 0x01E00100u);  /* ADDR */
  assert_int_equal(word_at(bin, (size_t)at + 8), 0x000000FFu);  /* MASK */
  free(bin);
}

static void test_jump_encoding(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR ".pbi\njump 0x0000000C\n.end\n", &len);

  long at = find_word(bin, len, 0x80840000u);   /* RM Table 46 header */
  assert_true(at >= 0);
  assert_int_equal(word_at(bin, (size_t)at + 4), 0x0000000Cu);  /* OFFSET */
  free(bin);
}

static void test_jumpif_encoding(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR ".pbi\njumpif 0x00000010,0x00000005\n.end\n",
                            &len);

  long at = find_word(bin, len, 0x80850000u);   /* RM Table 47 header */
  assert_true(at >= 0);
  assert_int_equal(word_at(bin, (size_t)at + 4), 0x00000010u);  /* OFFSET */
  assert_int_equal(word_at(bin, (size_t)at + 8), 0x00000005u);  /* CONDITION */
  free(bin);
}

/*
 * Sizes are load-bearing: the SP walks the PBI linearly, so a command that
 * consumes the wrong number of words desynchronises everything after it.
 * Compile one of each in isolation and check the image grows by exactly the
 * RM-specified amount over an empty PBI.
 */
static void test_command_sizes(void **state) {
  (void)state;
  size_t base = 0, l_cond = 0, l_jump = 0, l_jumpif = 0;
  uint8_t *b0 = compile_ok(HDR ".pbi\n.end\n", &base);
  uint8_t *b1 = compile_ok(HDR ".pbi\nloadcondition 0x1,0x2\n.end\n", &l_cond);
  uint8_t *b2 = compile_ok(HDR ".pbi\njump 0x0C\n.end\n", &l_jump);
  uint8_t *b3 = compile_ok(HDR ".pbi\njumpif 0x0C,0x1\n.end\n", &l_jumpif);

  assert_int_equal(l_cond   - base, 12);   /* RM Table 40 */
  assert_int_equal(l_jump   - base,  8);   /* RM Table 46 */
  assert_int_equal(l_jumpif - base, 12);   /* RM Table 47 */

  free(b0); free(b1); free(b2); free(b3);
}

/* RM Table 46/47 error check: OFFSET must be 4-byte aligned. */
static void test_offset_must_be_aligned(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\njump 0x0000000A\n.end\n");
  compile_fails(HDR ".pbi\njumpif 0x0000000A,0x1\n.end\n");
}

/*
 * RM Table 46/47 error check: OFFSET must be greater than 8. Exactly 8 is
 * the boundary case and is rejected: it would target the word right after
 * the command, which the SP treats as an error rather than a no-op.
 */
static void test_offset_must_exceed_eight(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\njump 0x00000008\n.end\n");
  compile_fails(HDR ".pbi\njump 0x00000004\n.end\n");
  compile_fails(HDR ".pbi\njumpif 0x00000008,0x1\n.end\n");
  compile_fails(HDR ".pbi\njumpif 0x00000004,0x1\n.end\n");
}

static void test_missing_operands_rejected(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\nloadcondition 0x01E00100\n.end\n");
  compile_fails(HDR ".pbi\njump\n.end\n");
  compile_fails(HDR ".pbi\njumpif 0x0C\n.end\n");
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_loadcondition_encoding),
    cmocka_unit_test(test_jump_encoding),
    cmocka_unit_test(test_jumpif_encoding),
    cmocka_unit_test(test_command_sizes),
    cmocka_unit_test(test_offset_must_be_aligned),
    cmocka_unit_test(test_offset_must_exceed_eight),
    cmocka_unit_test(test_missing_operands_rejected),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

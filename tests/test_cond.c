/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBI flow control:
 *   - Load Condition (0x14)
 *   - Jump (0x84)
 *   - Conditional Jump (0x85)
 * aliased as loadc / jump / jumpc too
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

static void compile_fails(const char *src, const char *why) {
  rcw_ctx_t *c = rcw_ctx_new();
  assert_non_null(c);
  uint8_t *bin = NULL;
  size_t len = 0;
  assert_int_equal(rcw_compile_buffer(c, src, strlen(src), &bin, &len),
                   RCW_ERR_PBI_SYNTAX);
  if (why)
    assert_non_null(strstr(rcw_ctx_last_error_detail(c), why));
  free(bin);
  rcw_ctx_free(c);
}

static void compile_same(const char *a, const char *b) {
  size_t la = 0, lb = 0;
  uint8_t *ba = compile_ok(a, &la);
  uint8_t *bb = compile_ok(b, &lb);
  assert_int_equal(la, lb);
  assert_memory_equal(ba, bb, la);
  free(ba);
  free(bb);
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

static void test_loadc_encoding(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(
      HDR ".pbi\nloadc 0x01E00100,0x000000FF\n.end\n", &len);

  long at = find_word(bin, len, 0x80140000u);   /* RM Table 40 header */
  assert_true(at >= 0);
  assert_int_equal(word_at(bin, (size_t)at + 4), 0x01E00100u);  /* ADDR */
  assert_int_equal(word_at(bin, (size_t)at + 8), 0x000000FFu);  /* MASK */
  free(bin);

  compile_same(HDR ".pbi\nloadc 0x01E00100,0x000000FF\n.end\n",
               HDR ".pbi\nloadcondition 0x01E00100,0x000000FF\n.end\n");
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

static void test_jumpc_encoding(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR ".pbi\njumpc 0x00000010,0x00000005\n.end\n",
                            &len);

  long at = find_word(bin, len, 0x80850000u);   /* RM Table 47 header */
  assert_true(at >= 0);
  assert_int_equal(word_at(bin, (size_t)at + 4), 0x00000010u);  /* OFFSET */
  assert_int_equal(word_at(bin, (size_t)at + 8), 0x00000005u);  /* CONDITION */
  free(bin);

  compile_same(HDR ".pbi\njumpc 0x10,0x5\n.end\n",
               HDR ".pbi\njumpif 0x10,0x5\n.end\n");
}

/*
 * Sizes are load-bearing: the SP walks the PBI linearly, so a command that
 * consumes the wrong number of words desynchronises everything after it.
 * Compile one of each in isolation and check the image grows by exactly the
 * RM-specified amount over an empty PBI.
 */
static void test_command_sizes(void **state) {
  (void)state;
  size_t base = 0, l_cond = 0, l_jump = 0, l_jumpc = 0;
  uint8_t *b0 = compile_ok(HDR ".pbi\n.end\n", &base);
  uint8_t *b1 = compile_ok(HDR ".pbi\nloadc 0x1,0x2\n.end\n", &l_cond);
  uint8_t *b2 = compile_ok(HDR ".pbi\njump 0x0C\n.end\n", &l_jump);
  uint8_t *b3 = compile_ok(HDR ".pbi\njumpc 0x0C,0x1\n.end\n", &l_jumpc);

  assert_int_equal(l_cond  - base, 12);   /* RM Table 40 */
  assert_int_equal(l_jump  - base,  8);   /* RM Table 46 */
  assert_int_equal(l_jumpc - base, 12);   /* RM Table 47 */

  free(b0); free(b1); free(b2); free(b3);
}

/* RM Table 46/47 error check: OFFSET must be 4-byte aligned. */
static void test_offset_must_be_aligned(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\njump 0x0000000A\n.end\n", "aligned");
  compile_fails(HDR ".pbi\njumpc 0x0000000A,0x1\n.end\n", "aligned");
  compile_fails(HDR ".pbi\njumpif 0x0000000A,0x1\n.end\n", "aligned");
}

/*
 * RM Table 46/47 error check: OFFSET must be greater than 8. Exactly 8 is
 * the boundary case and is rejected: it would target the word right after
 * the command, which the SP treats as an error rather than a no-op.
 */
static void test_offset_must_exceed_eight(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\njump 0x00000008\n.end\n", "greater than 8");
  compile_fails(HDR ".pbi\njump 0x00000004\n.end\n", "greater than 8");
  compile_fails(HDR ".pbi\njumpc 0x00000008,0x1\n.end\n", "greater than 8");
  compile_fails(HDR ".pbi\njumpc 0x00000004,0x1\n.end\n", "greater than 8");
}

static void test_missing_operands_rejected(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\nloadc 0x01E00100\n.end\n", "requires 2");
  compile_fails(HDR ".pbi\njump\n.end\n", "requires 1");
  compile_fails(HDR ".pbi\njumpc 0x0C\n.end\n", "requires 2");
  compile_fails(HDR ".pbi\njumpc done\n.end\n", "requires 2");
}

/*
 * lx2160asi/bootlocptr_auto.rcw in the upstream rcw tree, SolidRun's
 * auto-boot: loadc PORSR1[RCW_SRC], then per boot source a jumpc over a
 * jump that skips its block copy.
 */
#define AUTO_BOOT \
  ".pbi\n" \
  "loadc 0x01e00000, (0xf << 23)\n" \
  "jumpc sdhc1_boot, (0x8 << 23)\n" \
  "jump sdhc1_done\n" \
  "sdhc1_boot:\n" \
  "blockcopy 0x08, 0x0000a000, 0x1800d000, 0x00020000\n" \
  "write 0x01e00400, 0x1800d000\n" \
  "sdhc1_done:\n" \
  "jumpc sdhc2_boot, (0x9 << 23)\n" \
  "jump sdhc2_done\n" \
  "sdhc2_boot:\n" \
  "blockcopy 0x09, 0x0000a000, 0x1800d000, 0x00020000\n" \
  "write 0x01e00400, 0x1800d000\n" \
  "sdhc2_done:\n" \
  "jumpc xspi_boot, (0xf << 23)\n" \
  "jump xspi_done\n" \
  "xspi_boot:\n" \
  "blockcopy 0x0f, 0x20009000, 0x1800d000, 0x00020000\n" \
  "write 0x01e00400, 0x1800d000\n" \
  "xspi_done:\n" \
  ".end\n"

static const uint32_t auto_boot_words[] = {
  0x80140000, 0x01e00000, 0x07800000,
  0x80850000, 0x00000014, 0x04000000,
  0x80840000, 0x00000020,
  0x80000008, 0x0000a000, 0x1800d000, 0x00020000,
  0x31e00400, 0x1800d000,
  0x80850000, 0x00000014, 0x04800000,
  0x80840000, 0x00000020,
  0x80000009, 0x0000a000, 0x1800d000, 0x00020000,
  0x31e00400, 0x1800d000,
  0x80850000, 0x00000014, 0x07800000,
  0x80840000, 0x00000020,
  0x8000000f, 0x20009000, 0x1800d000, 0x00020000,
  0x31e00400, 0x1800d000,
  0x80ff0000, 0x00000000, /* Stop */
};

static void test_labels_resolve_as_rcwpy(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR AUTO_BOOT, &len);
  size_t n = sizeof(auto_boot_words) / sizeof(auto_boot_words[0]);

  assert_int_equal(len, RCW_PBL_PBI_OFFSET + 4 * n);
  for (size_t i = 0; i < n; i++)
    assert_int_equal(word_at(bin, RCW_PBL_PBI_OFFSET + 4 * i), auto_boot_words[i]);
  free(bin);

  /* the same block with the offsets written out */
  compile_same(HDR AUTO_BOOT,
      HDR ".pbi\n"
      "loadc 0x01e00000,0x07800000\n"
      "jumpc 0x14,0x04000000\n"
      "jump 0x20\n"
      "blockcopy 0x08,0x0000a000,0x1800d000,0x00020000\n"
      "write 0x01e00400,0x1800d000\n"
      "jumpc 0x14,0x04800000\n"
      "jump 0x20\n"
      "blockcopy 0x09,0x0000a000,0x1800d000,0x00020000\n"
      "write 0x01e00400,0x1800d000\n"
      "jumpc 0x14,0x07800000\n"
      "jump 0x20\n"
      "blockcopy 0x0f,0x20009000,0x1800d000,0x00020000\n"
      "write 0x01e00400,0x1800d000\n"
      ".end\n");
}

/* A label right after an 8-byte jump is the offset 8 we refuse. */
static void test_label_too_close(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\njump next\nnext:\nwait 1\n.end\n", "more than 8");
  /* the conditional jump is 12 bytes, so the same layout is fine */
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR ".pbi\njumpc next,1\nnext:\nwait 1\n.end\n", &len);
  long at = find_word(bin, len, 0x80850000u);
  assert_true(at >= 0);
  assert_int_equal(word_at(bin, (size_t)at + 4), 12);
  free(bin);
}

static void test_label_errors(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\njump out\nwait 1\nout:\nwait 1\nout:\n.end\n",
                "Duplicate label \"out\"");
  compile_fails(HDR ".pbi\njump nowhere\nwait 1\n.end\n",
                "\"jump nowhere\": label \"nowhere\" is not defined");
  compile_fails(HDR ".pbi\njumpc nowhere,1\nwait 1\n.end\n",
                "\"jumpc nowhere\": label \"nowhere\" is not defined");
  /* jump is forward only */
  compile_fails(HDR ".pbi\nback:\nwait 1\njump back\n.end\n",
                "behind the jump");
  compile_fails(HDR ".pbi\njump nowhere\nwait 1\n", "is not defined");

  char name[RCW_LABEL_NAME_MAX + 8];
  memset(name, 'a', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  char src[1024];
  snprintf(src, sizeof(src), "%s.pbi\njump %s\nwait 1\n%s:\n.end\n", HDR, name, name);
  compile_fails(src, "too long");
}

static void test_labels_are_per_block(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(
      HDR ".pbi\njump out\nwait 1\nout:\n.end\n"
          ".pbi\njump out\nwait 2\nout:\n.end\n", &len);
  /* each jump skips its own wait: 12 bytes */
  assert_int_equal(word_at(bin, RCW_PBL_PBI_OFFSET + 4), 12);
  assert_int_equal(word_at(bin, RCW_PBL_PBI_OFFSET + 12 + 4), 12);
  free(bin);

  compile_fails(HDR ".pbi\nwait 1\n.end\n.pbi\njump later\n.end\n.pbi\nlater:\n.end\n",
                "is not defined in this .pbi block");
}

static void test_label_names_and_block_end(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(
      HDR ".pbi\njump write\nwait 1\nwait 1\nwrite:\nwrite 0x01e00400,0x1\n.end\n", &len);
  assert_int_equal(word_at(bin, RCW_PBL_PBI_OFFSET + 4), 16);
  free(bin);

  bin = compile_ok(HDR ".pbi\njump done\nwait 1\ndone:\n.end\n", &len);
  assert_int_equal(word_at(bin, RCW_PBL_PBI_OFFSET + 4), 12);
  /* the jump lands on the Stop command */
  assert_int_equal(word_at(bin, RCW_PBL_PBI_OFFSET + 12), 0x80ff0000u);
  free(bin);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_loadc_encoding),
    cmocka_unit_test(test_jump_encoding),
    cmocka_unit_test(test_jumpc_encoding),
    cmocka_unit_test(test_command_sizes),
    cmocka_unit_test(test_offset_must_be_aligned),
    cmocka_unit_test(test_offset_must_exceed_eight),
    cmocka_unit_test(test_missing_operands_rejected),
    cmocka_unit_test(test_labels_resolve_as_rcwpy),
    cmocka_unit_test(test_label_too_close),
    cmocka_unit_test(test_label_errors),
    cmocka_unit_test(test_labels_are_per_block),
    cmocka_unit_test(test_label_names_and_block_end),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

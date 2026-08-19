/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBI secure-boot commands: Load Security Header (0x20) and Load Boot 1
 * CSF Header Pointer (0x22).
 *
 *   Table 41  Load Security Header       84 B  HDR=0x80 CMD=0x20
 *                                              | CSF_WORD_0 .. CSF_WORD_19
 *   Table 42  Load Boot 1 CSF Hdr Ptr     8 B  HDR=0x80 CMD=0x22 | CSF_PTR
 *
 * NEITHER HAS EVER RUN ON THIS BOARD. nbxv3 boots non-secure -- ITS is
 * unburned and DBG_LVL is 000 -- so unlike every other PBI command there is
 * no bench evidence behind these, only the RM tables. Treat a first secure
 * boot as the real validation; these tests pin the encoding so it cannot
 * drift in the meantime.
 *
 * The RM specifies exactly two checks between them (4-byte alignment on
 * CSF_PTR, and on the Load Security Header address) and defines nothing
 * about the CSF word contents -- that layout lives in the QorIQ Trust
 * Architecture UG, which is not in doc/. So the 20 words are carried
 * verbatim and deliberately NOT interpreted; a structural check invented
 * here would risk rejecting valid CST output.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Build "loadsecurityheader 0x...,0x..." with 20 predictable words. */
static void inline_src(char *buf, size_t sz, uint32_t base) {
  /* HDR holds %size=, %pbiformat= ... -- must NOT be a format string. */
  size_t n = (size_t)snprintf(buf, sz, "%s", HDR ".pbi\nloadsecurityheader ");
  for (unsigned i = 0; i < PBI_CSF_HEADER_WORDS; i++)
    n += (size_t)snprintf(buf + n, sz - n, "%s0x%08x",
                          i ? "," : "", base + i);
  snprintf(buf + n, sz - n, "\n.end\n");
}

static void test_loadsecurityheader_inline(void **state) {
  (void)state;
  char src[2048];
  inline_src(src, sizeof(src), 0x11110000u);

  size_t len = 0;
  uint8_t *bin = compile_ok(src, &len);

  long at = find_word(bin, len, 0x80200000u);   /* RM Table 41 header */
  assert_true(at >= 0);
  for (unsigned i = 0; i < PBI_CSF_HEADER_WORDS; i++)
    assert_int_equal(word_at(bin, (size_t)at + 4 + 4 * i), 0x11110000u + i);
  free(bin);
}

static void test_loadboot1csfptr_encoding(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(
      HDR ".pbi\nloadboot1csfptr 0x18000000\n.end\n", &len);

  long at = find_word(bin, len, 0x80220000u);   /* RM Table 42 header */
  assert_true(at >= 0);
  assert_int_equal(word_at(bin, (size_t)at + 4), 0x18000000u);
  free(bin);
}

/* 84 B and 8 B exactly -- a wrong length desynchronises the SP's walk. */
static void test_sizes(void **state) {
  (void)state;
  char src[2048];
  inline_src(src, sizeof(src), 0x1u);

  size_t base = 0, l_hdr = 0, l_ptr = 0;
  uint8_t *b0 = compile_ok(HDR ".pbi\n.end\n", &base);
  uint8_t *b1 = compile_ok(src, &l_hdr);
  uint8_t *b2 = compile_ok(
      HDR ".pbi\nloadboot1csfptr 0x18000000\n.end\n", &l_ptr);

  assert_int_equal(l_hdr - base, 84);   /* RM Table 41 */
  assert_int_equal(l_ptr - base,  8);   /* RM Table 42 */
  free(b0); free(b1); free(b2);
}

/* RM Table 42's only stated check. */
static void test_csf_ptr_alignment(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\nloadboot1csfptr 0x18000001\n.end\n");
  compile_fails(HDR ".pbi\nloadboot1csfptr 0x18000002\n.end\n");
  compile_fails(HDR ".pbi\nloadboot1csfptr\n.end\n");
}

/* Exactly 20 words, or a quoted path -- nothing else. */
static void test_word_count_enforced(void **state) {
  (void)state;
  compile_fails(HDR ".pbi\nloadsecurityheader 0x1,0x2\n.end\n");
  compile_fails(HDR ".pbi\nloadsecurityheader\n.end\n");
}

/*
 * File form. Written as hex TEXT rather than raw binary so a CSF header
 * stays diffable in git -- it authenticates the boot chain, so being able
 * to review a change to it matters. Tokens may be separated by whitespace
 * or commas, "0x" is optional, and # / // comments are allowed.
 */
static void write_file(const char *path, const char *body) {
  FILE *f = fopen(path, "w");
  assert_non_null(f);
  fputs(body, f);
  fclose(f);
}

static void test_loadsecurityheader_from_file(void **state) {
  (void)state;
  const char *path = "test_csf_hdr.txt";
  char body[1024];
  size_t n = (size_t)snprintf(body, sizeof(body),
      "# CSF header, hex text\n");
  for (unsigned i = 0; i < PBI_CSF_HEADER_WORDS; i++)
    n += (size_t)snprintf(body + n, sizeof(body) - n,
                          "0x%08x%s", 0x33330000u + i,
                          ((i % 4) == 3) ? "   // row\n" : ", ");
  write_file(path, body);

  size_t len = 0;
  uint8_t *bin = compile_ok(
      HDR ".pbi\nloadsecurityheader \"test_csf_hdr.txt\"\n.end\n", &len);

  long at = find_word(bin, len, 0x80200000u);
  assert_true(at >= 0);
  for (unsigned i = 0; i < PBI_CSF_HEADER_WORDS; i++)
    assert_int_equal(word_at(bin, (size_t)at + 4 + 4 * i), 0x33330000u + i);

  free(bin);
  unlink(path);
}

/* The file form must be strict in both directions and about content. */
static void test_file_errors(void **state) {
  (void)state;
  char body[1024];
  size_t n;

  compile_fails(HDR ".pbi\nloadsecurityheader \"no_such_csf.txt\"\n.end\n");

  n = 0;
  for (unsigned i = 0; i < PBI_CSF_HEADER_WORDS - 1; i++)      /* 19 */
    n += (size_t)snprintf(body + n, sizeof(body) - n, "0x%x ", i);
  write_file("test_csf_short.txt", body);
  compile_fails(HDR ".pbi\nloadsecurityheader \"test_csf_short.txt\"\n.end\n");
  unlink("test_csf_short.txt");

  n = 0;
  for (unsigned i = 0; i < PBI_CSF_HEADER_WORDS + 1; i++)      /* 21 */
    n += (size_t)snprintf(body + n, sizeof(body) - n, "0x%x ", i);
  write_file("test_csf_long.txt", body);
  compile_fails(HDR ".pbi\nloadsecurityheader \"test_csf_long.txt\"\n.end\n");
  unlink("test_csf_long.txt");

  write_file("test_csf_bad.txt", "0x1 0x2 zz 0x4\n");
  compile_fails(HDR ".pbi\nloadsecurityheader \"test_csf_bad.txt\"\n.end\n");
  unlink("test_csf_bad.txt");
}

/* Both forms must produce identical bytes for identical words. */
static void test_file_and_inline_agree(void **state) {
  (void)state;
  char body[1024], src[2048];
  size_t n = 0;
  for (unsigned i = 0; i < PBI_CSF_HEADER_WORDS; i++)
    n += (size_t)snprintf(body + n, sizeof(body) - n,
                          "0x%08x\n", 0x44440000u + i);
  write_file("test_csf_same.txt", body);

  inline_src(src, sizeof(src), 0x44440000u);
  size_t l1 = 0, l2 = 0;
  uint8_t *b1 = compile_ok(src, &l1);
  uint8_t *b2 = compile_ok(
      HDR ".pbi\nloadsecurityheader \"test_csf_same.txt\"\n.end\n", &l2);

  assert_int_equal(l1, l2);
  assert_memory_equal(b1, b2, l1);

  free(b1); free(b2);
  unlink("test_csf_same.txt");
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_loadsecurityheader_inline),
    cmocka_unit_test(test_loadboot1csfptr_encoding),
    cmocka_unit_test(test_sizes),
    cmocka_unit_test(test_csf_ptr_alignment),
    cmocka_unit_test(test_word_count_enforced),
    cmocka_unit_test(test_loadsecurityheader_from_file),
    cmocka_unit_test(test_file_errors),
    cmocka_unit_test(test_file_and_inline_agree),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * Each source line is compiled, decoded one command after the other, the
 * jumps not followed so that every line is handled.
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
  "SYS_PLL_RAT[6:2]\n"             \
  "SB_EN[266]\n"                   \
  "PBI_LENGTH[287:276]\n"          \
  "SYS_PLL_RAT=7\n"

#define MAX_LINES 16

struct lines {
  size_t n;
  char text[MAX_LINES][RCW_PBI_SOURCE_MAX];
};

static void collect(const rcw_pbi_cmd_t *cmd, struct lines *l) {
  assert_true(l->n < MAX_LINES);
  int n = rcw_pbi_to_source(cmd, l->text[l->n], sizeof(l->text[l->n]));
  assert_true(n > 0 && (size_t)n < sizeof(l->text[l->n]));
  assert_int_equal((size_t)n, strlen(l->text[l->n]));
  l->n++;
}

/* Compile pbi (the lines between .pbi and .end) and print every command. */
static void run(const char *pbi, int crc, struct lines *l) {
  size_t srclen = strlen(HDR) + strlen(pbi) + 32;
  char *src = malloc(srclen);
  rcw_ctx_t *c = rcw_ctx_new();
  uint8_t *bin = NULL;
  size_t len = 0;
  rcw_pbl_t pbl;
  rcw_pbi_cmd_t cmd;
  size_t off = 0;

  assert_non_null(src);
  assert_non_null(c);
  snprintf(src, srclen, "%s%s.pbi\n%s.end\n", HDR, crc ? "" : "%nocrc=1\n", pbi);
  assert_int_equal(rcw_compile_buffer(c, src, strlen(src), &bin, &len), RCW_OK);
  memset(l, 0, sizeof(*l));
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  while (off < PBI_WORD_BYTES * (size_t)pbl.pbi_length) {
    assert_int_equal(rcw_pbi_decode(pbl.pbi + off, pbl.pbi_avail - off, 1, &cmd), RCW_OK);
    collect(&cmd, l);
    off += cmd.size;
  }
  assert_int_equal(off, PBI_WORD_BYTES * (size_t)pbl.pbi_length);
  free(bin);
  free(src);
  rcw_ctx_free(c);
}

static void test_general_commands(void **state) {
  (void)state;
  struct lines l;

  run("write 0x01e00620,0x80000000\n"
      "write.b1 0x01e00621,0x5a\n"
      "blockcopy 0x0f,0x20009000,0x1800d000,0x1000\n"
      "wait 0x64\n"
      "poll 0x01e00100,0xff,0x1\n"
      "poll.long 0x01e00100,0xff,0x1\n"
      "loadacwindow 0x100\n", 0, &l);
  assert_int_equal(l.n, 8);
  assert_string_equal(l.text[0], "write 0x01e00620,0x80000000");
  assert_string_equal(l.text[1], "write.b1 0x01e00621,0x0000005a");
  assert_string_equal(l.text[2], "blockcopy 0x0f,0x20009000,0x1800d000,0x00001000");
  assert_string_equal(l.text[3], "wait 0x00000064");
  assert_string_equal(l.text[4], "poll.short 0x01e00100,0x000000ff,0x00000001");
  assert_string_equal(l.text[5], "poll.long 0x01e00100,0x000000ff,0x00000001");
  assert_string_equal(l.text[6], "loadacwindow 0x00000100");
  assert_string_equal(l.text[7], "/* Stop command */");
}

static void test_awrite_and_flow(void **state) {
  (void)state;
  struct lines l;

  run("awrite 0x100,0x1\n"
      "awrite.b4 0x100000,0xaaaaaaaa,0xbbbbbbbb\n"
      "jump skip\n"
      "wait 1\n"
      "skip:\n"
      "loadc 0x01e00000,0x07800000\n"
      "jumpc end,0x04000000\n"
      "wait 2\n"
      "end:\n"
      "wait 3\n", 0, &l);
  assert_int_equal(l.n, 9);
  assert_string_equal(l.text[0], "awrite 0x00000100,0x00000001");
  assert_string_equal(l.text[1], "awrite.b4 0x00100000,0xaaaaaaaa,0xbbbbbbbb");
  /* The jump is 8 bytes, the wait after it 4: skip is 12 bytes on. */
  assert_string_equal(l.text[2], "jump 0x0000000c");
  assert_string_equal(l.text[3], "wait 0x00000001");
  assert_string_equal(l.text[4], "loadc 0x01e00000,0x07800000");
  /* jumpc is 12 bytes, the wait 4: end is 16 bytes on. */
  assert_string_equal(l.text[5], "jumpc 0x00000010,0x04000000");
  assert_string_equal(l.text[6], "wait 0x00000002");
  assert_string_equal(l.text[7], "wait 0x00000003");
  assert_string_equal(l.text[8], "/* Stop command */");
}

static void test_density_and_secure(void **state) {
  (void)state;
  struct lines l;

  run("ccsrcopy.b4 0x18000000,0x01E00300,0x00000010\n"
      "ccsrset.b4 0x01E00400,0x0000000F\n"
      "ccsrclear.b1 0x01E00401,0x0F\n"
      "ccsrreplace.b4 0x01E00408,0x0000FF00,0x00005500\n"
      "loadboot1csfptr 0x20000000\n", 1, &l);
  assert_int_equal(l.n, 6);
  assert_string_equal(l.text[0], "ccsrcopy.b4 0x18000000,0x01e00300,0x00000010");
  assert_string_equal(l.text[1], "ccsrset.b4 0x01e00400,0x0000000f");
  assert_string_equal(l.text[2], "ccsrclear.b1 0x01e00401,0x0000000f");
  assert_string_equal(l.text[3], "ccsrreplace.b4 0x01e00408,0x0000ff00,0x00005500");
  assert_string_equal(l.text[4], "loadboot1csfptr 0x20000000");
  assert_memory_equal(l.text[5], "/* CRC and Stop command (CRC 0x", 31);
}

static void test_truncation(void **state) {
  (void)state;
  rcw_pbi_cmd_t cmd;
  static const uint8_t write[8] = { 0x20, 0x06, 0xe0, 0x31, 0x00, 0x00, 0x00, 0x80 }; /* write 0x01e00620,0x80000000 */
  char buf[12];

  assert_int_equal(rcw_pbi_decode(write, sizeof(write), 1, &cmd), RCW_OK);
  assert_int_equal(cmd.op, RCW_PBI_CCSR_WRITE);
  assert_int_equal(rcw_pbi_to_source(&cmd, NULL, 0), (int)strlen("write 0x01e00620,0x80000000"));
  assert_int_equal(rcw_pbi_to_source(&cmd, buf, sizeof(buf)), (int)strlen("write 0x01e00620,0x80000000"));
  assert_string_equal(buf, "write 0x01e");
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_general_commands),
    cmocka_unit_test(test_awrite_and_flow),
    cmocka_unit_test(test_density_and_secure),
    cmocka_unit_test(test_truncation),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

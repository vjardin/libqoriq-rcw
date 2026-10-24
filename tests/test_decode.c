/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBI command decoder, rcw_pbi_decode().
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

static void put_le32(uint8_t *p, uint32_t w) {
  p[0] = (uint8_t)w; p[1] = (uint8_t)(w >> 8);
  p[2] = (uint8_t)(w >> 16); p[3] = (uint8_t)(w >> 24);
}

static void put_be32(uint8_t *p, uint32_t w) {
  p[0] = (uint8_t)(w >> 24); p[1] = (uint8_t)(w >> 16);
  p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w;
}

struct layout {
  const char *name;
  uint32_t word;
  rcw_pbi_op_t op;
  size_t nargs;
  uint32_t param;
  uint32_t width;
};

static const struct layout layouts[] = {
  { "write",              0x31e00400, RCW_PBI_CCSR_WRITE,    1,   0x01e00400, 4   },
  { "write.b1",           0x121c0500, RCW_PBI_CCSR_WRITE,    1,   0x021c0500, 1   },
  { "awrite",             0x8c117e60, RCW_PBI_ALTCFG_WRITE,  1,   0x00117e60, 4   },
  { "awrite.b4",          0x92508000, RCW_PBI_ALTCFG_WRITE,  2,   0x02508000, 8   },
  { "awrite.b5",          0x96518000, RCW_PBI_ALTCFG_WRITE,  4,   0x02518000, 16  },
  { "awrite, 1 byte",     0x84000010, RCW_PBI_ALTCFG_WRITE,  1,   0x00000010, 1   },
  { "awrite, 512 bytes",  0xa8000000, RCW_PBI_ALTCFG_WRITE,  128, 0x00000000, 512 },
  { "blockcopy",          0x8000000f, RCW_PBI_BLOCKCOPY,     3,   0x0f,       0   },
  { "ccsrcopy.b4",        0x80020003, RCW_PBI_CCSR_COPY,     3,   0x03,       0   },
  { "loadrcw",            0x80100000, RCW_PBI_LOAD_RCW,      33,  0,          0   },
  { "loadrcw.nochecksum", 0x80110000, RCW_PBI_LOAD_RCW_NOCS, 33,  0,          0   },
  { "loadacwindow",       0x801201c0, RCW_PBI_LOADACWINDOW,  0,   0x1c0,      0   },
  { "loadcondition",      0x80140000, RCW_PBI_LOADCONDITION, 2,   0,          0   },
  { "loadsecurityheader", 0x80200000, RCW_PBI_LOAD_SEC_HDR,  20,  0,          0   },
  { "loadboot1csfptr",    0x80220000, RCW_PBI_LOAD_CSF_PTR,  1,   0,          0   },
  { "ccsrreplace.b4",     0x8042000b, RCW_PBI_CCSR_RMW,      3,   0x0b,       0   },
  { "poll.short",         0x80800000, RCW_PBI_POLL_SHORT,    3,   0,          0   },
  { "poll.long",          0x80810000, RCW_PBI_POLL_LONG,     3,   0,          0   },
  { "wait",               0x80820064, RCW_PBI_WAIT,          0,   0x64,       0   },
  { "jump",               0x80840000, RCW_PBI_JUMP,          1,   0,          0   },
  { "jumpif",             0x80850000, RCW_PBI_JUMP_COND,     2,   0,          0   },
  { "crcstop",            0x808f0000, RCW_PBI_CRC_STOP,      1,   0,          0   },
  { "stop",               0x80ff0000, RCW_PBI_STOP,          1,   0,          0   },
};

static void test_layouts(void **state) {
  (void)state;
  uint8_t buf[4 + 4 * RCW_PBI_MAX_ARGS + 8];
  rcw_pbi_cmd_t cmd;

  for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
    const struct layout *l = &layouts[i];
    size_t size = 4 + 4 * l->nargs;

    print_message("  %s\n", l->name);
    memset(buf, 0xee, sizeof(buf));
    put_le32(buf, l->word);
    for (size_t k = 0; k < l->nargs; k++)
      put_le32(buf + 4 + 4 * k, 0x1000u + (uint32_t)k);

    assert_int_equal(rcw_pbi_decode(buf, size, 1, &cmd), RCW_OK);
    assert_int_equal(cmd.op, l->op);
    assert_int_equal(cmd.word, l->word);
    assert_int_equal(cmd.size, size);
    assert_int_equal(cmd.nargs, l->nargs);
    assert_int_equal(cmd.param, l->param);
    assert_int_equal(cmd.width, l->width);
    for (size_t k = 0; k < l->nargs; k++)
      assert_int_equal(cmd.args[k], 0x1000u + (uint32_t)k);

    assert_int_equal(rcw_pbi_decode(buf, size + 8, 1, &cmd), RCW_OK);
    assert_int_equal(cmd.size, size);

    if (l->nargs > 0) {
      assert_int_equal(rcw_pbi_decode(buf, size - 1, 1, &cmd), RCW_ERR_TRUNCATED);
      assert_int_equal(cmd.op, l->op);
      assert_int_equal(cmd.word, l->word);
      assert_int_equal(cmd.size, size);
      assert_int_equal(cmd.nargs, l->nargs);
      assert_int_equal(rcw_pbi_decode(buf, 4, 1, &cmd), RCW_ERR_TRUNCATED);
      assert_int_equal(cmd.size, size);
    }
  }
}

static void test_rejects(void **state) {
  (void)state;
  static const uint32_t words[] = {
    0x01e00200, /* CCSR Write, B=0: reserved */
    0x21e00200, /* CCSR Write, B=2: 2-byte writes are not legal */
    0x41e00200, /* CCSR Write with RSVD bit 6 set */
    0x71e00200,
    0xac000000, /* AltConfig Write, B=11: above 512 bytes */
    0xbc000000, /* AltConfig Write, B=15 */
    0x81000000, /* ACS=1, B=0, RSVD bits [1:0] set */
    0x83ff0000,
    0x80010000, /* undefined CMD bytes */
    0x80030000,
    0x80300000,
    0x80430000,
    0x80830000,
    0x80860000,
    0x80900000,
    0x80fe0000,
    0xc0000000, /* ACS=1 with RSVD bit 30 set */
    0xffffffff,
  };
  uint8_t buf[16];
  rcw_pbi_cmd_t cmd;

  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    print_message("  0x%08x\n", words[i]);
    put_le32(buf, words[i]);
    memset(buf + 4, 0, sizeof(buf) - 4);
    assert_int_equal(rcw_pbi_decode(buf, sizeof(buf), 1, &cmd), RCW_ERR_BAD_BINARY);
    assert_int_equal(cmd.op, RCW_PBI_UNKNOWN);
    assert_int_equal(cmd.word, words[i]);
    assert_int_equal(cmd.size, 0);
  }
}

static void test_too_short(void **state) {
  (void)state;
  uint8_t buf[4] = { 0x00, 0x00, 0xff, 0x80 };
  rcw_pbi_cmd_t cmd;

  for (size_t len = 0; len < 4; len++) {
    assert_int_equal(rcw_pbi_decode(buf, len, 1, &cmd), RCW_ERR_TRUNCATED);
    assert_int_equal(cmd.op, RCW_PBI_UNKNOWN);
    assert_int_equal(cmd.size, 0);
    assert_int_equal(cmd.nargs, 0);
  }
}

static void test_big_endian(void **state) {
  (void)state;
  uint8_t buf[16];
  rcw_pbi_cmd_t cmd;

  put_be32(buf, 0x8000000f);
  put_be32(buf + 4, 0x20009000);
  put_be32(buf + 8, 0x1800d000);
  put_be32(buf + 12, 0x00001000);
  assert_int_equal(rcw_pbi_decode(buf, sizeof(buf), 0, &cmd), RCW_OK);
  assert_int_equal(cmd.op, RCW_PBI_BLOCKCOPY);
  assert_int_equal(cmd.param, 0x0f);
  assert_int_equal(cmd.args[0], 0x20009000);
  assert_int_equal(cmd.args[1], 0x1800d000);
  assert_int_equal(cmd.args[2], 0x00001000);

  assert_int_equal(rcw_pbi_decode(buf, sizeof(buf), 1, &cmd), RCW_ERR_BAD_BINARY);
}

static void test_op_names(void **state) {
  (void)state;

  assert_string_equal(rcw_pbi_op_name(RCW_PBI_CCSR_WRITE), "write");
  assert_string_equal(rcw_pbi_op_name(RCW_PBI_ALTCFG_WRITE), "awrite");
  assert_string_equal(rcw_pbi_op_name(RCW_PBI_BLOCKCOPY), "blockcopy");
  assert_string_equal(rcw_pbi_op_name(RCW_PBI_LOAD_SEC_HDR), "loadsecurityheader");
  assert_string_equal(rcw_pbi_op_name(RCW_PBI_LOAD_CSF_PTR), "loadboot1csfptr");
  assert_string_equal(rcw_pbi_op_name(RCW_PBI_CRC_STOP), "crcstop");
  assert_string_equal(rcw_pbi_op_name(RCW_PBI_STOP), "stop");
  assert_string_equal(rcw_pbi_op_name(RCW_PBI_UNKNOWN), "unknown");
  assert_string_equal(rcw_pbi_op_name((rcw_pbi_op_t)0x33), "unknown");
  for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++)
    assert_string_not_equal(rcw_pbi_op_name(layouts[i].op), "unknown");
}

#define HDR                        \
  "%size=1024\n"                   \
  "%pbiformat=2\n"                 \
  "%classicbitnumbers=1\n"         \
  "%littleendian=1\n"              \
  "SYS_PLL_RAT[6:2]\n"             \
  "PBI_LENGTH[287:276]\n"          \
  "SYS_PLL_RAT=7\n"

#define EVERY_COMMAND \
  ".pbi\n" \
  "write 0x01e00620,0x80000000\n" \
  "write.b1 0x021c0500,0x49\n" \
  "awrite 0x117e60,0x80000001\n" \
  "awrite.b4 0x2508000,0x64a8150e,0xcfc4885c\n" \
  "awrite.b5 0x2518000,0x1,0x2,0x3,0x4\n" \
  "blockcopy 0x0f,0x20009000,0x1800d000,0x1000\n" \
  "ccsrcopy.b4 0x18000000,0x01e00210,0x4\n" \
  "ccsrset.b4 0x01e00620,0x1\n" \
  "ccsrreplace.b2 0x01e00622,0xff,0x12\n" \
  "loadacwindow 0x1c0\n" \
  "loadcondition 0x01e60130,0xff\n" \
  "jump 0x10\n" \
  "jumpif 0x14,0x1\n" \
  "loadsecurityheader 0x12192001,0x200,0x101,0,0x800,0x100,0x600,0x1," \
      "0x1800d000,0,0,0,0,0,0,0,0,0,0,0\n" \
  "loadboot1csfptr 0x1800a000\n" \
  "poll 0x01e00404,0x1,0x1\n" \
  "poll.long 0x01e00404,0x1,0x1\n" \
  "wait 0x64\n" \
  ".end\n"

static const rcw_pbi_op_t every_op[] = {
  RCW_PBI_CCSR_WRITE, RCW_PBI_CCSR_WRITE,
  RCW_PBI_ALTCFG_WRITE, RCW_PBI_ALTCFG_WRITE, RCW_PBI_ALTCFG_WRITE,
  RCW_PBI_BLOCKCOPY, RCW_PBI_CCSR_COPY, RCW_PBI_CCSR_RMW, RCW_PBI_CCSR_RMW,
  RCW_PBI_LOADACWINDOW, RCW_PBI_LOADCONDITION, RCW_PBI_JUMP, RCW_PBI_JUMP_COND,
  RCW_PBI_LOAD_SEC_HDR, RCW_PBI_LOAD_CSF_PTR,
  RCW_PBI_POLL_SHORT, RCW_PBI_POLL_LONG, RCW_PBI_WAIT,
};

static uint8_t *compile_ok(const char *src, size_t *len) {
  rcw_ctx_t *c = rcw_ctx_new();
  assert_non_null(c);
  uint8_t *bin = NULL;
  assert_int_equal(rcw_compile_buffer(c, src, strlen(src), &bin, len), RCW_OK);
  assert_non_null(bin);
  rcw_ctx_free(c);
  return bin;
}

static void walk(const char *src, rcw_pbi_op_t terminator) {
  size_t len = 0;
  uint8_t *bin = compile_ok(src, &len);
  size_t off = RCW_PBL_PBI_OFFSET;
  rcw_pbi_cmd_t cmd;

  for (size_t i = 0; i < sizeof(every_op) / sizeof(every_op[0]); i++) {
    print_message("  byte %zu: %s\n", off, rcw_pbi_op_name(every_op[i]));
    assert_int_equal(rcw_pbi_decode(bin + off, len - off, 1, &cmd), RCW_OK);
    assert_int_equal(cmd.op, every_op[i]);
    switch (i) {
    case 0:
      assert_int_equal(cmd.param, 0x01e00620);
      assert_int_equal(cmd.width, 4);
      assert_int_equal(cmd.args[0], 0x80000000);
      break;
    case 1:
      assert_int_equal(cmd.width, 1);
      assert_int_equal(cmd.args[0], 0x49);
      break;
    case 4:
      assert_int_equal(cmd.param, 0x2518000);
      assert_int_equal(cmd.width, 16);
      assert_int_equal(cmd.args[3], 4);
      break;
    case 5:
      assert_int_equal(cmd.param, 0x0f);
      assert_int_equal(cmd.args[0], 0x20009000);
      assert_int_equal(cmd.args[1], 0x1800d000);
      assert_int_equal(cmd.args[2], 0x1000);
      break;
    case 13:
      assert_int_equal(cmd.nargs, PBI_CSF_HEADER_WORDS);
      assert_int_equal(cmd.args[0], 0x12192001);
      assert_int_equal(cmd.args[8], 0x1800d000);
      break;
    case 14:
      assert_int_equal(cmd.args[0], 0x1800a000);
      break;
    case 17:
      assert_int_equal(cmd.param, 0x64);
      break;
    default:
      break;
    }
    off += cmd.size;
  }

  assert_int_equal(rcw_pbi_decode(bin + off, len - off, 1, &cmd), RCW_OK);
  assert_int_equal(cmd.op, terminator);
  assert_int_equal(off + cmd.size, len);
  free(bin);
}

static void test_walk_stop(void **state) {
  (void)state;
  walk(HDR "%nocrc=1\n" EVERY_COMMAND, RCW_PBI_STOP);
}

static void test_walk_crc_stop(void **state) {
  (void)state;
  walk(HDR EVERY_COMMAND, RCW_PBI_CRC_STOP);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_layouts),
    cmocka_unit_test(test_rejects),
    cmocka_unit_test(test_too_short),
    cmocka_unit_test(test_big_endian),
    cmocka_unit_test(test_op_names),
    cmocka_unit_test(test_walk_stop),
    cmocka_unit_test(test_walk_crc_stop),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBL image envelope and PBI check
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

#define PBI                                   \
  ".pbi\n"                                    \
  "write 0x01e00620,0x80000000\n"             \
  "blockcopy 0x0f,0x20009000,0x1800d000,0x1000\n" \
  "wait 0x64\n"                               \
  ".end\n"

#define OFF_WRITE     0
#define OFF_BLOCKCOPY 8
#define OFF_WAIT      24
#define OFF_STOP      28
#define PBI_BYTES     36
#define PBI_WORDS     (PBI_BYTES / 4)

#define RCW_WORD8_AT     (RCW_PBL_RCW_OFFSET + 8 * PBI_WORD_BYTES)
#define PBI_LENGTH_SHIFT 20
#define PBI_LENGTH_MASK  (0xFFFu << PBI_LENGTH_SHIFT)
#define SB_EN_BIT        (1u << 10)

#define LOAD_RCW_CMD_BYTE (PBI_WORD_BYTES + 2)

static uint8_t *compile_ok(const char *src, size_t *len) {
  rcw_ctx_t *c = rcw_ctx_new();
  assert_non_null(c);
  uint8_t *bin = NULL;
  assert_int_equal(rcw_compile_buffer(c, src, strlen(src), &bin, len), RCW_OK);
  assert_non_null(bin);
  rcw_ctx_free(c);
  return bin;
}

static uint32_t word_at(const uint8_t *b, size_t off) {
  return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
         ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static void put_word(uint8_t *b, size_t off, uint32_t w) {
  b[off] = (uint8_t)w;
  b[off + 1] = (uint8_t)(w >> 8);
  b[off + 2] = (uint8_t)(w >> 16);
  b[off + 3] = (uint8_t)(w >> 24);
}

static void set_pbi_length(uint8_t *bin, uint32_t words) {
  put_word(bin, RCW_WORD8_AT, (word_at(bin, RCW_WORD8_AT) & ~PBI_LENGTH_MASK) | (words << PBI_LENGTH_SHIFT));
}

static rcw_pbi_check_t parse_check_ok(const uint8_t *bin, size_t len, rcw_pbl_t *pbl) {
  rcw_pbi_check_t chk;

  assert_int_equal(rcw_pbl_parse(bin, len, 1, pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(pbl, 1, NULL, NULL, &chk), RCW_OK);
  assert_int_equal(chk.len, PBI_WORD_BYTES * (size_t)pbl->pbi_length);
  assert_int_equal(chk.end, chk.len);
  assert_ptr_equal(pbl->pbi + chk.end, bin + len);
  return chk;
}

struct visit_log {
  size_t n;
  rcw_pbi_op_t op[8];
  size_t off[8];
};

static void log_visit(const rcw_pbi_cmd_t *cmd, size_t off, void *arg) {
  struct visit_log *l = arg;

  assert_true(l->n < 8);
  l->op[l->n] = cmd->op;
  l->off[l->n] = off;
  l->n++;
}

static void test_parse_stop(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "%nocrc=1\n" "SB_EN=1\n" PBI, &len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk = parse_check_ok(bin, len, &pbl);

  assert_int_equal(pbl.load_rcw_cmd, PBL_CMD_LOAD_RCW);
  assert_ptr_equal(pbl.rcw, bin + RCW_PBL_RCW_OFFSET);
  assert_int_equal(pbl.checksum, pbl.checksum_computed);
  assert_int_equal(pbl.checksum, word_at(bin, RCW_PBL_CHECKSUM_OFFSET));
  assert_ptr_equal(pbl.pbi, bin + RCW_PBL_PBI_OFFSET);
  assert_int_equal(pbl.pbi_avail, len - RCW_PBL_PBI_OFFSET);
  assert_int_equal(pbl.pbi_length, PBI_WORDS);
  assert_int_equal(pbl.sb_en, 1);

  assert_int_equal(word_at(bin, RCW_WORD8_AT) >> PBI_LENGTH_SHIFT, pbl.pbi_length);
  assert_int_equal(word_at(bin, RCW_WORD8_AT) & SB_EN_BIT, SB_EN_BIT);

  assert_int_equal(chk.stop_op, RCW_PBI_STOP);
  assert_int_equal(chk.cmd.op, RCW_PBI_STOP);
  assert_int_equal(chk.ncmd, 4);
  assert_int_equal(chk.crc, 0);
  assert_int_equal(chk.crc_computed, 0);
  free(bin);
}

static void test_parse_crc_stop(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR PBI, &len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk = parse_check_ok(bin, len, &pbl);

  assert_int_equal(pbl.sb_en, 0);
  assert_int_equal(word_at(bin, RCW_WORD8_AT) & SB_EN_BIT, 0);

  assert_int_equal(chk.stop_op, RCW_PBI_CRC_STOP);
  assert_int_equal(chk.cmd.op, RCW_PBI_CRC_STOP);
  assert_int_equal(chk.crc, word_at(bin, len - PBI_WORD_BYTES));
  assert_int_equal(chk.crc_computed, chk.crc);
  assert_int_equal(rcw_pbi_crc(pbl.pbi, PBI_WORD_BYTES * ((size_t)pbl.pbi_length - 1)), chk.crc);
  assert_int_not_equal(rcw_pbi_crc(pbl.pbi, PBI_WORD_BYTES * ((size_t)pbl.pbi_length - 2)), chk.crc);
  free(bin);
}

static void test_parse_without_checksum(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "%loadwochecksum=1\n" PBI, &len);
  rcw_pbl_t pbl;

  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(pbl.load_rcw_cmd, PBL_CMD_LOAD_RCW_NOCS);
  assert_int_equal(pbl.checksum, 0);
  assert_int_not_equal(pbl.checksum_computed, 0);
  free(bin);
}

static void test_pbi_length_field(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "PBI_LENGTH=0xabc\n" PBI, &len);
  rcw_pbl_t pbl;

  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(pbl.pbi_length, 0xabc);
  assert_int_equal(word_at(bin, RCW_WORD8_AT) >> PBI_LENGTH_SHIFT, 0xabc);
  free(bin);
}

static void test_checksum_mismatch(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR PBI, &len);
  rcw_pbl_t pbl;

  bin[RCW_PBL_RCW_OFFSET] ^= 0x01;
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_not_equal(pbl.checksum, pbl.checksum_computed);
  assert_int_equal(pbl.checksum_computed - pbl.checksum, (bin[RCW_PBL_RCW_OFFSET] & 1) ? 1 : (uint32_t)-1);
  free(bin);
}

static void test_errors(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR PBI, &len);
  rcw_pbl_t pbl;

  for (size_t cut = 0; cut < RCW_PBL_PBI_OFFSET; cut += 7)
    assert_int_equal(rcw_pbl_parse(bin, cut, 1, &pbl), RCW_ERR_TRUNCATED);
  assert_int_equal(rcw_pbl_parse(bin, RCW_PBL_PBI_OFFSET - 1, 1, &pbl), RCW_ERR_TRUNCATED);
  assert_int_equal(rcw_pbl_parse(bin, RCW_PBL_PBI_OFFSET, 1, &pbl), RCW_OK);
  assert_int_equal(pbl.pbi_avail, 0);

  assert_int_not_equal(rcw_pbl_parse(bin + RCW_PBL_RCW_OFFSET, RCW_SIZE_BYTES, 1, &pbl), RCW_OK);

  bin[0] ^= 0x01;
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_ERR_BAD_BINARY);
  bin[0] ^= 0x01;

  bin[LOAD_RCW_CMD_BYTE] = PBI_GENCMD_STOP;
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_ERR_BAD_BINARY);
  bin[LOAD_RCW_CMD_BYTE] = PBI_GENCMD_LOAD_RCW_CS;
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  free(bin);
}

static void test_crc_check_value(void **state) {
  (void)state;
  static const uint8_t check[] = "123456789";

  assert_int_equal(rcw_pbi_crc(check, 9), 0xCBF43926);
  assert_int_equal(rcw_pbi_crc(check, 0), 0);
}

/* Every executed command is visited in order, at the offset its layout gives it. */
static void test_check_visit(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "%nocrc=1\n" PBI, &len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk;
  struct visit_log l = { 0 };

  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, log_visit, &l, &chk), RCW_OK);
  assert_int_equal(chk.ncmd, 4);
  assert_int_equal(l.n, 4);
  assert_int_equal(l.op[0], RCW_PBI_CCSR_WRITE);
  assert_int_equal(l.off[0], OFF_WRITE);
  assert_int_equal(l.op[1], RCW_PBI_BLOCKCOPY);
  assert_int_equal(l.off[1], OFF_BLOCKCOPY);
  assert_int_equal(l.op[2], RCW_PBI_WAIT);
  assert_int_equal(l.off[2], OFF_WAIT);
  assert_int_equal(l.op[3], RCW_PBI_STOP);
  assert_int_equal(l.off[3], OFF_STOP);
  assert_int_equal(chk.len, PBI_BYTES);
  assert_int_equal(chk.end, PBI_BYTES);
  free(bin);
}

static void test_check_no_pbi(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "PBI_LENGTH=0\n" PBI, &len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk;
  struct visit_log l = { 0 };

  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(pbl.pbi_length, 0);
  assert_int_equal(pbl.pbi_avail, PBI_BYTES);
  assert_int_equal(rcw_pbl_check(&pbl, 1, log_visit, &l, &chk), RCW_OK);
  assert_int_equal(chk.len, 0);
  assert_int_equal(chk.end, 0);
  assert_int_equal(chk.ncmd, 0);
  assert_int_equal(chk.stop_op, RCW_PBI_UNKNOWN);
  assert_int_equal(l.n, 0);
  free(bin);
}

static void test_check_crc_mismatch(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR PBI, &len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk;

  bin[RCW_PBL_PBI_OFFSET + OFF_WRITE + PBI_WORD_BYTES] ^= 0x01; /* the data word of the write */
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_OK);
  assert_int_equal(chk.stop_op, RCW_PBI_CRC_STOP);
  assert_int_equal(chk.crc, word_at(bin, len - PBI_WORD_BYTES));
  assert_int_not_equal(chk.crc, chk.crc_computed);

  bin[RCW_PBL_PBI_OFFSET + OFF_WRITE + PBI_WORD_BYTES] ^= 0x01;
  bin[len - 1] ^= 0x80; /* the CRC word itself */
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_OK);
  assert_int_equal(chk.crc, word_at(bin, len - PBI_WORD_BYTES));
  assert_int_equal(chk.crc_computed, rcw_pbi_crc(pbl.pbi, PBI_BYTES - PBI_WORD_BYTES));
  assert_int_not_equal(chk.crc, chk.crc_computed);
  free(bin);
}

static void test_check_pbi_length(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "%nocrc=1\n" PBI, &len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk;

  set_pbi_length(bin, 1);
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_ERR_PBI_LENGTH);
  assert_string_not_equal(rcw_strerror(RCW_ERR_PBI_LENGTH), "Unknown error");
  assert_int_equal(chk.len, PBI_WORD_BYTES);
  assert_int_equal(chk.end, OFF_WRITE);
  assert_int_equal(chk.ncmd, 0);
  assert_int_equal(chk.cmd.op, RCW_PBI_CCSR_WRITE);
  assert_int_equal(chk.cmd.size, 8);
  assert_int_equal(chk.stop_op, RCW_PBI_UNKNOWN);

  set_pbi_length(bin, 5);
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_ERR_PBI_LENGTH);
  assert_int_equal(chk.end, OFF_BLOCKCOPY);
  assert_int_equal(chk.ncmd, 1);
  assert_int_equal(chk.cmd.op, RCW_PBI_BLOCKCOPY);

  set_pbi_length(bin, 7);
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_ERR_PBI_LENGTH);
  assert_int_equal(chk.end, OFF_STOP);
  assert_int_equal(chk.ncmd, 3);
  assert_int_equal(chk.cmd.op, RCW_PBI_WAIT);
  assert_int_equal(chk.stop_op, RCW_PBI_UNKNOWN);

  set_pbi_length(bin, PBI_WORDS);
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_OK);
  free(bin);
}

static void test_check_development_buffer(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "%nocrc=1\n" PBI, &len);
  size_t img_len = len + PBI_WORD_BYTES + PBL_TERMINATOR_BYTES; /* a wait word, then the final terminator */
  uint8_t *img = malloc(img_len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk;
  struct visit_log l = { 0 };

  assert_non_null(img);
  memcpy(img, bin, len);
  free(bin);
  put_word(img, len, PBL_CMD_WAIT | 1);  /* wait 1: never executed */
  put_word(img, len + PBI_WORD_BYTES, PBL_CMD_STOP); /* the final Stop */
  put_word(img, len + 2 * PBI_WORD_BYTES, 0);
  set_pbi_length(img, PBI_WORDS + 1 + PBL_TERMINATOR_WORDS);

  assert_int_equal(rcw_pbl_parse(img, img_len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, log_visit, &l, &chk), RCW_OK);
  assert_int_equal(chk.len, img_len - RCW_PBL_PBI_OFFSET);
  assert_int_equal(chk.end, PBI_BYTES);
  assert_int_equal(chk.ncmd, 4);
  assert_int_equal(l.n, 4);
  assert_int_equal(l.op[3], RCW_PBI_STOP);
  assert_int_equal(chk.cmd.op, RCW_PBI_STOP);
  assert_int_equal(chk.stop_op, RCW_PBI_STOP);

  put_word(img, len + PBI_WORD_BYTES, PBL_CMD_CRC_STOP);
  put_word(img, len + 2 * PBI_WORD_BYTES,
           rcw_pbi_crc(img + RCW_PBL_PBI_OFFSET, PBI_BYTES + 2 * PBI_WORD_BYTES));
  assert_int_equal(rcw_pbl_parse(img, img_len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_OK);
  assert_int_equal(chk.end, PBI_BYTES);
  assert_int_equal(chk.stop_op, RCW_PBI_CRC_STOP);
  assert_int_equal(chk.crc, chk.crc_computed);
  assert_int_equal(chk.crc, word_at(img, len + 2 * PBI_WORD_BYTES));

  put_word(img, len + PBI_WORD_BYTES, PBL_CMD_WAIT | 2);
  put_word(img, len + 2 * PBI_WORD_BYTES, PBL_CMD_WAIT | 3);
  assert_int_equal(rcw_pbl_parse(img, img_len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_ERR_PBI_LENGTH);
  assert_int_equal(chk.end, PBI_BYTES);
  assert_int_equal(chk.cmd.op, RCW_PBI_STOP);
  assert_int_equal(chk.stop_op, RCW_PBI_UNKNOWN);
  free(img);
}

static void test_check_unknown_word(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "%nocrc=1\n" PBI, &len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk;
  struct visit_log l = { 0 };

  put_word(bin, RCW_PBL_PBI_OFFSET + OFF_WAIT,
           PBL_CMD_GENERAL | (0x30u << PBI_CMD_SHIFT)); /* CMD 0x30: not in Tables */
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, log_visit, &l, &chk), RCW_ERR_BAD_BINARY);
  assert_int_equal(chk.end, OFF_WAIT);
  assert_int_equal(chk.ncmd, 2);
  assert_int_equal(l.n, 2);
  assert_int_equal(chk.cmd.op, RCW_PBI_UNKNOWN);
  assert_int_equal(chk.cmd.word, 0x80300000);
  assert_int_equal(chk.stop_op, RCW_PBI_UNKNOWN);
  free(bin);
}

static void test_check_truncated(void **state) {
  (void)state;
  size_t len = 0;
  uint8_t *bin = compile_ok(HDR "%nocrc=1\n" PBI, &len);
  rcw_pbl_t pbl;
  rcw_pbi_check_t chk;

  assert_int_equal(rcw_pbl_parse(bin, len - 4, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_ERR_TRUNCATED);
  assert_int_equal(chk.len, PBI_BYTES);
  assert_int_equal(chk.end, OFF_STOP);
  assert_int_equal(chk.ncmd, 3);
  assert_int_equal(chk.cmd.op, RCW_PBI_STOP);
  assert_int_equal(chk.cmd.size, 8);
  assert_int_equal(chk.stop_op, RCW_PBI_UNKNOWN);

  /* Nothing after the RCW. */
  assert_int_equal(rcw_pbl_parse(bin, RCW_PBL_PBI_OFFSET, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_ERR_TRUNCATED);
  assert_int_equal(chk.end, 0);
  assert_int_equal(chk.ncmd, 0);

  set_pbi_length(bin, PBI_WORDS + 2);
  assert_int_equal(rcw_pbl_parse(bin, len, 1, &pbl), RCW_OK);
  assert_int_equal(rcw_pbl_check(&pbl, 1, NULL, NULL, &chk), RCW_ERR_TRUNCATED);
  assert_int_equal(chk.len, PBI_BYTES + 2 * PBI_WORD_BYTES);
  assert_int_equal(chk.end, PBI_BYTES);
  assert_int_equal(chk.ncmd, 4);
  assert_int_equal(chk.cmd.op, RCW_PBI_STOP);
  assert_int_equal(chk.stop_op, RCW_PBI_UNKNOWN);
  free(bin);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_parse_stop),
    cmocka_unit_test(test_parse_crc_stop),
    cmocka_unit_test(test_parse_without_checksum),
    cmocka_unit_test(test_pbi_length_field),
    cmocka_unit_test(test_checksum_mismatch),
    cmocka_unit_test(test_errors),
    cmocka_unit_test(test_crc_check_value),
    cmocka_unit_test(test_check_visit),
    cmocka_unit_test(test_check_no_pbi),
    cmocka_unit_test(test_check_crc_mismatch),
    cmocka_unit_test(test_check_pbi_length),
    cmocka_unit_test(test_check_development_buffer),
    cmocka_unit_test(test_check_unknown_word),
    cmocka_unit_test(test_check_truncated),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

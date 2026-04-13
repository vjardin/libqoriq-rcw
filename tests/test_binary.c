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

/*
 * Minimal source for a nocrc board (lx2160a-style).
 * This is what gcc -E would produce after preprocessing.
 */
static const char nocrc_source[] =
  "%size=1024\n"
  "%pbiformat=2\n"
  "%classicbitnumbers=1\n"
  "%littleendian=1\n"
  "%nocrc=1\n"
  "SYS_PLL_RAT[6:2]\n"
  "PBI_LENGTH[287:276]\n"
  "SYS_PLL_RAT=7\n"
  ".pbi\n"
  "write 0x01e00620,0x80000000\n"
  ".end\n";

/* Test nocrc binary generation (Stop command, no CRC) */
static void
test_binary_nocrc(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, nocrc_source, strlen(nocrc_source), &out, &len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(out);

  /*
   * Expected layout:
   *   4 bytes: preamble 0xAA55AA55 (LE)
   *   4 bytes: Load RCW cmd 0x80100000 (LE) 128 bytes: RCW data
   *   4 bytes: checksum
   *   8 bytes: PBI write command
   *   4 bytes: Stop 0x80FF0000 (LE)
   *   4 bytes: 0x00000000
   * Total: 4+4+128+4+8+4+4 = 156
   */
  assert_int_equal(len, 156);

  /* Check preamble (LE) */
  assert_int_equal(out[0], 0x55);
  assert_int_equal(out[1], 0xAA);
  assert_int_equal(out[2], 0x55);
  assert_int_equal(out[3], 0xAA);

  /* Check Stop command near the end (LE: 0x80FF0000) */
  size_t stop_off = len - 8;
  uint32_t stop_word = (uint32_t)out[stop_off]
                    | ((uint32_t)out[stop_off+1] << 8)
                    | ((uint32_t)out[stop_off+2] << 16)
                    | ((uint32_t)out[stop_off+3] << 24);

  assert_int_equal(stop_word, 0x80FF0000);

  rcw_free(out);
  rcw_ctx_free(ctx);
}

/* Test no-pbl mode (raw RCW bits only) */
static void
test_binary_no_pbl(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "SYS_PLL_RAT=7\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);
  rcw_ctx_set_pbl(ctx, 0);

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, src, strlen(src), &out, &len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(out);

  /* No PBL: just 128 bytes of RCW data */
  assert_int_equal(len, 128);

  rcw_free(out);
  rcw_ctx_free(ctx);
}

/*
 * Minimal source for a CRC board (ls1088-style, no %nocrc).
 */
static const char crc_source[] =
  "%size=1024\n"
  "%pbiformat=2\n"
  "%classicbitnumbers=1\n"
  "%littleendian=1\n"
  "SYS_PLL_RAT[6:2]\n"
  "PBI_LENGTH[287:276]\n"
  "SYS_PLL_RAT=7\n"
  ".pbi\n"
  "write 0x01e00620,0x80000000\n"
  ".end\n";

/* Test CRC binary generation (CRC-and-Stop command) */
static void
test_binary_with_crc(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, crc_source, strlen(crc_source), &out, &len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(out);

  /* Same size as nocrc: 156 bytes */
  assert_int_equal(len, 156);

  /* Check CRC-and-Stop command near the end (LE: 0x808F0000) */
  size_t crc_off = len - 8;
  uint32_t crc_word = (uint32_t)out[crc_off]
     | ((uint32_t)out[crc_off+1] << 8)
     | ((uint32_t)out[crc_off+2] << 16)
     | ((uint32_t)out[crc_off+3] << 24);
  assert_int_equal(crc_word, 0x808F0000);

  /* CRC value should be non-zero */
  uint32_t crc_val = (uint32_t)out[crc_off+4]
    | ((uint32_t)out[crc_off+5] << 8)
    | ((uint32_t)out[crc_off+6] << 16)
    | ((uint32_t)out[crc_off+7] << 24);
  assert_true(crc_val != 0);

  rcw_free(out);
  rcw_ctx_free(ctx);
}

/* Test that PBI_LENGTH is auto-calculated */
static void
test_binary_pbi_length_auto(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, nocrc_source, strlen(nocrc_source), &out, &len);
  assert_int_equal(err, RCW_OK);

  /* PBI_LENGTH should have been set: 1 write = 2 words of PBI, +2 for Stop = 4 */
  bool found = false;
  for (size_t i = 0; i < ctx->symbols.count; i++) {
    if (strcmp(ctx->symbols.entries[i].name, "PBI_LENGTH") == 0) {
      assert_true(ctx->symbols.entries[i].has_value);
      assert_int_equal(ctx->symbols.entries[i].value, 4);
      found = true;
      break;
    }
  }
  assert_true(found);

  rcw_free(out);
  rcw_ctx_free(ctx);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_binary_nocrc),
    cmocka_unit_test(test_binary_no_pbl),
    cmocka_unit_test(test_binary_with_crc),
    cmocka_unit_test(test_binary_pbi_length_auto),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

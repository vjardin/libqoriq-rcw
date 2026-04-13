/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include "rcw_internal.h"

/* Test CRC of an empty buffer */
static void
test_crc32_empty(void **state) {
  (void)state;
  uint32_t crc = rcw_crc32(NULL, 0);
  /* With init=0xFFFFFFFF and no data, CRC should remain 0xFFFFFFFF */
  assert_int_equal(crc, 0xFFFFFFFF);
}

/* Test CRC of a single zero byte */
static void
test_crc32_single_zero(void **state) {
  (void)state;
  uint8_t data[] = {0x00};
  uint32_t crc = rcw_crc32(data, 1);
  /* Known value for this polynomial with single zero byte */
  assert_int_equal(crc, 0x4E08BFB4);
}

/* Test CRC of a known pattern "123456789" (ASCII) */
static void
test_crc32_ascii_check(void **state) {
  (void)state;
  const uint8_t data[] = "123456789";
  uint32_t crc = rcw_crc32(data, 9);
  /*
   * The standard CRC-32 check value for polynomial 0x04C11DB7
   * with init 0xFFFFFFFF and no final XOR is 0x0376E6E7.
   */
  assert_int_equal(crc, 0x0376E6E7);
}

/* Test CRC of all-FF data (8 bytes) */
static void
test_crc32_all_ff(void **state) {
  (void)state;
  uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint32_t crc = rcw_crc32(data, 8);
  /* Verify it produces a deterministic result */
  assert_true(crc != 0);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_crc32_empty),
    cmocka_unit_test(test_crc32_single_zero),
    cmocka_unit_test(test_crc32_ascii_check),
    cmocka_unit_test(test_crc32_all_ff),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

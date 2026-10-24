/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * rcw_ctx_add_define(): macros handed to mcpp
 *
 * libmcpp keeps static state between mcpp_lib_main() calls, so one
 * preprocessor run per test binary: a single source exercises a valued
 * define, a bare one and an absent one at once.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>
#include <cmocka.h>

#include "rcw_internal.h"

static void
test_defines_reach_mcpp(void **state) {
  (void)state;
  static const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "MEM_PLL_RAT[15:10]\n"
    "PBI_LENGTH[287:276]\n"
    "#if SRDS_PRTCL_S1_VAL == 19\n"
    "SYS_PLL_RAT=19\n"
    "#else\n"
    "SYS_PLL_RAT=1\n"
    "#endif\n"
    "#ifdef RCW_ERRATA_NO_TRIM\n"
    "MEM_PLL_RAT=RCW_ERRATA_NO_TRIM\n"
    "#endif\n"
    "#ifdef NOT_DEFINED_ANYWHERE\n"
    "MEM_PLL_RAT=63\n"
    "#endif\n";

  char tmpl[] = "/tmp/test-define-XXXXXX";
  int fd = mkstemp(tmpl);
  assert_true(fd >= 0);
  assert_int_equal(write(fd, src, sizeof(src) - 1), (ssize_t)(sizeof(src) - 1));
  close(fd);

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);
  assert_int_equal(rcw_ctx_add_define(ctx, "SRDS_PRTCL_S1_VAL", "19"), RCW_OK);
  assert_int_equal(rcw_ctx_add_define(ctx, "RCW_ERRATA_NO_TRIM", NULL), RCW_OK);
  assert_int_equal(rcw_ctx_add_define(ctx, "", "1"), RCW_ERR_PARSE);
  assert_string_equal(ctx->defines[0], "-DSRDS_PRTCL_S1_VAL=19");
  assert_string_equal(ctx->defines[1], "-DRCW_ERRATA_NO_TRIM");

  char *pp = NULL;
  size_t pp_len = 0;
  assert_int_equal(rcw_preprocess_file(ctx, tmpl, &pp, &pp_len), RCW_OK);
  unlink(tmpl);
  assert_non_null(pp);

  char *bare = malloc(pp_len + 1);
  assert_non_null(bare);
  size_t n = 0;
  for (size_t i = 0; i < pp_len; i++)
    if (pp[i] != ' ' && pp[i] != '\t')
      bare[n++] = pp[i];
  bare[n] = '\0';
  assert_non_null(strstr(bare, "\nSYS_PLL_RAT=19\n"));
  assert_null(strstr(bare, "\nSYS_PLL_RAT=1\n"));
  assert_non_null(strstr(bare, "\nMEM_PLL_RAT=1\n"));
  assert_null(strstr(bare, "MEM_PLL_RAT=63"));
  assert_null(strstr(bare, "#if"));
  free(bare);

  uint8_t *bin = NULL;
  size_t bin_len = 0;
  assert_int_equal(rcw_compile_buffer(ctx, pp, pp_len, &bin, &bin_len), RCW_OK);
  assert_int_equal(rcw_bits_extract(bin + RCW_PBL_RCW_OFFSET, 6, 2, true), 19);
  assert_int_equal(rcw_bits_extract(bin + RCW_PBL_RCW_OFFSET, 15, 10, true), 1);

  rcw_free(bin);
  rcw_free(pp);
  rcw_ctx_free(ctx);
}

static void
test_define_table_is_bounded(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  char name[16];

  for (int i = 0; i < RCW_MAX_DEFINES; i++) {
    snprintf(name, sizeof(name), "D%d", i);
    assert_int_equal(rcw_ctx_add_define(ctx, name, NULL), RCW_OK);
  }
  assert_int_equal(rcw_ctx_add_define(ctx, "ONE_TOO_MANY", NULL), RCW_ERR_NOMEM);
  assert_int_equal(ctx->define_count, RCW_MAX_DEFINES);
  rcw_ctx_free(ctx);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_defines_reach_mcpp),
    cmocka_unit_test(test_define_table_is_bounded),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * The command-line forms of the context configuration:
 * rcw_ctx_set_bitfield_text() ("NAME=VALUE" à la rcw.py -D) and
 * rcw_ctx_add_define() with "NAME=VALUE" as a C compiler's -D
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdint.h>
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

static void test_set_bitfield_text(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();

  assert_non_null(ctx);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "SYS_PLL_RAT=0b1110"), RCW_OK);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "MEM_PLL_RAT=0x1F"), RCW_OK);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "A_9=0o17"), RCW_OK);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "B=10"), RCW_OK);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "C=0"), RCW_OK);
  assert_int_equal(ctx->override_count, 5);
  assert_string_equal(ctx->overrides[0].name, "SYS_PLL_RAT");
  assert_int_equal(ctx->overrides[0].value, 14);
  assert_string_equal(ctx->overrides[1].name, "MEM_PLL_RAT");
  assert_int_equal(ctx->overrides[1].value, 31);
  assert_int_equal(ctx->overrides[2].value, 15);
  assert_int_equal(ctx->overrides[3].value, 10);
  assert_int_equal(ctx->overrides[4].value, 0);

  /* Python refuses a leading zero; rcw.py's regex passes no sign or space. */
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "B=010"), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "B=-1"), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "B= 1"), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "B=1x"), RCW_ERR_PARSE);
  assert_non_null(strstr(rcw_ctx_last_error_detail(ctx), "1x"));
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "B="), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "=1"), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "FOO"), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "FO O=1"), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, ""), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, NULL), RCW_ERR_PARSE);
  assert_int_equal(ctx->override_count, 5);
  rcw_ctx_free(ctx);

  /* The override reaches the image: SYS_PLL_RAT[6:2]=14 is 0x38 in RCW byte 0. */
  ctx = rcw_ctx_new();
  assert_non_null(ctx);
  assert_int_equal(rcw_ctx_set_bitfield_text(ctx, "SYS_PLL_RAT=0b1110"), RCW_OK);
  uint8_t *bin = NULL;
  size_t len = 0;
  assert_int_equal(rcw_compile_buffer(ctx, HDR, strlen(HDR), &bin, &len), RCW_OK);
  assert_int_equal(bin[RCW_PBL_RCW_OFFSET], 0x38);
  free(bin);
  rcw_ctx_free(ctx);
}

static void test_add_define_text(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();

  assert_non_null(ctx);
  assert_int_equal(rcw_ctx_add_define(ctx, "FOO=1", NULL), RCW_OK);
  assert_int_equal(rcw_ctx_add_define(ctx, "BAR", NULL), RCW_OK);
  assert_int_equal(rcw_ctx_add_define(ctx, "_X9", "2"), RCW_OK);
  assert_int_equal(rcw_ctx_add_define(ctx, "EMPTY=", NULL), RCW_OK);
  assert_int_equal(ctx->define_count, 4);
  assert_string_equal(ctx->defines[0], "-DFOO=1");
  assert_string_equal(ctx->defines[1], "-DBAR");
  assert_string_equal(ctx->defines[2], "-D_X9=2");
  assert_string_equal(ctx->defines[3], "-DEMPTY=");

  assert_int_equal(rcw_ctx_add_define(ctx, "=3", NULL), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_add_define(ctx, "9X", NULL), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_add_define(ctx, "FO-O", NULL), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_add_define(ctx, "FOO=1", "2"), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_add_define(ctx, "", NULL), RCW_ERR_PARSE);
  assert_int_equal(rcw_ctx_add_define(ctx, NULL, NULL), RCW_ERR_PARSE);
  assert_int_equal(ctx->define_count, 4);
  rcw_ctx_free(ctx);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_set_bitfield_text),
    cmocka_unit_test(test_add_define_text),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

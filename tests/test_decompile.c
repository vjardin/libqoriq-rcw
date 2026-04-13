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
 * Round-trip test: compile, then decompile, then recompile.
 * The two binaries must be identical.
 */
static const char roundtrip_source[] =
  "%size=1024\n"
  "%pbiformat=2\n"
  "%classicbitnumbers=1\n"
  "%littleendian=1\n"
  "%nocrc=1\n"
  "SYS_PLL_RAT[6:2]\n"
  "MEM_PLL_RAT[15:10]\n"
  "CGA_PLL1_RAT[31:26]\n"
  "BOOT_LOC[264:260]\n"
  "PBI_LENGTH[287:276]\n"
  "SYSCLK_FREQ[301:292]\n"
  "SYS_PLL_RAT=7\n"
  "MEM_PLL_RAT=21\n"
  "CGA_PLL1_RAT=16\n"
  "BOOT_LOC=26\n"
  "SYSCLK_FREQ=0x258\n"
  ".pbi\n"
  "write 0x01e00620,0x80000000\n"
  ".end\n";

static void
test_decompile_roundtrip(void **state) {
  (void)state;

  /* Step 1: Compile */
  rcw_ctx_t *ctx1 = rcw_ctx_new();
  assert_non_null(ctx1);

  uint8_t *bin1 = NULL;
  size_t bin1_len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx1, roundtrip_source, strlen(roundtrip_source), &bin1, &bin1_len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(bin1);

  /* Step 2: Decompile */
  /* Re-parse the symbol definitions only (simulate .rcwi) */
  const char rcwi_source[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "MEM_PLL_RAT[15:10]\n"
    "CGA_PLL1_RAT[31:26]\n"
    "BOOT_LOC[264:260]\n"
    "PBI_LENGTH[287:276]\n"
    "SYSCLK_FREQ[301:292]\n";

  rcw_ctx_t *ctx2 = rcw_ctx_new();
  assert_non_null(ctx2);

  err = rcw_parse(ctx2, rcwi_source, strlen(rcwi_source));
  assert_int_equal(err, RCW_OK);

  char *source_out = NULL;
  size_t source_len = 0;
  err = rcw_decompile(ctx2, bin1, bin1_len, &source_out, &source_len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(source_out);

  /* Step 3: Recompile the decompiled source */
  rcw_ctx_t *ctx3 = rcw_ctx_new();
  assert_non_null(ctx3);

  /*
   * The decompiled output doesn't include %variable definitions or
   * bitfield definitions (those come from the .rcwi). We need to
   * prepend the rcwi definitions.
   */
  size_t combined_len = strlen(rcwi_source) + source_len;
  char *combined = malloc(combined_len + 1);
  assert_non_null(combined);
  memcpy(combined, rcwi_source, strlen(rcwi_source));
  memcpy(combined + strlen(rcwi_source), source_out, source_len);
  combined[combined_len] = '\0';

  uint8_t *bin2 = NULL;
  size_t bin2_len = 0;
  err = rcw_compile_buffer(ctx3, combined, combined_len, &bin2, &bin2_len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(bin2);

  /* Step 4: Compare binaries */
  assert_int_equal(bin1_len, bin2_len);
  assert_memory_equal(bin1, bin2, bin1_len);

  free(combined);
  rcw_free(source_out);
  rcw_free(bin1);
  rcw_free(bin2);
  rcw_ctx_free(ctx1);
  rcw_ctx_free(ctx2);
  rcw_ctx_free(ctx3);
}

/* Test decompile produces expected field output */
static void
test_decompile_fields(void **state) {
  (void)state;

  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n"
    "SYS_PLL_RAT=7\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);
  rcw_ctx_set_pbl(ctx, 0); /* no PBL for simplicity */

  uint8_t *bin = NULL;
  size_t bin_len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, src, strlen(src), &bin, &bin_len);
  assert_int_equal(err, RCW_OK);

  /* Decompile */
  rcw_ctx_t *ctx2 = rcw_ctx_new();
  const char rcwi[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n";

  err = rcw_parse(ctx2, rcwi, strlen(rcwi));
  assert_int_equal(err, RCW_OK);

  char *source = NULL;
  size_t slen = 0;
  err = rcw_decompile(ctx2, bin, bin_len, &source, &slen);
  assert_int_equal(err, RCW_OK);

  /* Should contain "SYS_PLL_RAT=7" */
  assert_non_null(strstr(source, "SYS_PLL_RAT=7"));

  rcw_free(source);
  rcw_free(bin);
  rcw_ctx_free(ctx);
  rcw_ctx_free(ctx2);
}

/*
 * Test rcw_decompile_buffer():
 *   - With rcwi_name -> output starts with "#include <name>\n\n"
 *   - With rcwi_name=NULL -> output omits the header
 *   - Field values are extracted correctly in both cases
 */
static void
test_decompile_buffer_with_header(void **state) {
  (void)state;

  static const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n"
    "SYS_PLL_RAT=7\n";

  /* Step 1: compile to get a binary */
  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_ctx_set_pbl(ctx, 0);
  uint8_t *bin = NULL;
  size_t bin_len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, src, strlen(src),
                                       &bin, &bin_len);
  assert_int_equal(err, RCW_OK);
  rcw_ctx_free(ctx);

  /* Step 2: decompile via the buffer API with rcwi_name="my.rcwi" */
  static const char rcwi_pp[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n";

  rcw_ctx_t *ctx2 = rcw_ctx_new();
  char *source = NULL;
  size_t source_len = 0;
  err = rcw_decompile_buffer(ctx2,
                             rcwi_pp, strlen(rcwi_pp),
                             bin, bin_len,
                             "my.rcwi",
                             &source, &source_len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(source);

  /* Header must be the very first thing in the output */
  assert_int_equal(strncmp(source, "#include <my.rcwi>\n\n",
                           strlen("#include <my.rcwi>\n\n")), 0);
  /* Body must contain the extracted field */
  assert_non_null(strstr(source, "SYS_PLL_RAT=7"));

  rcw_free(source);
  rcw_free(bin);
  rcw_ctx_free(ctx2);
}

static void
test_decompile_buffer_no_header(void **state) {
  (void)state;

  static const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n"
    "SYS_PLL_RAT=7\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_ctx_set_pbl(ctx, 0);
  uint8_t *bin = NULL;
  size_t bin_len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, src, strlen(src),
                                       &bin, &bin_len);
  assert_int_equal(err, RCW_OK);
  rcw_ctx_free(ctx);

  static const char rcwi_pp[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n";

  rcw_ctx_t *ctx2 = rcw_ctx_new();
  char *source = NULL;
  size_t source_len = 0;
  err = rcw_decompile_buffer(ctx2,
                             rcwi_pp, strlen(rcwi_pp),
                             bin, bin_len,
                             NULL, /* no header */
                             &source, &source_len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(source);

  /* No #include header */
  assert_null(strstr(source, "#include"));
  /* Body still contains the field */
  assert_non_null(strstr(source, "SYS_PLL_RAT=7"));

  rcw_free(source);
  rcw_free(bin);
  rcw_ctx_free(ctx2);
}

/*
 * Test that rcw_compile_buffer + rcw_decompile_buffer round-trip
 * produces the same binary (no file I/O, no gcc dependency).
 */
static void
test_decompile_buffer_roundtrip(void **state) {
  (void)state;

  static const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "MEM_PLL_RAT[15:10]\n"
    "BOOT_LOC[264:260]\n"
    "PBI_LENGTH[287:276]\n"
    "SYS_PLL_RAT=7\n"
    "MEM_PLL_RAT=21\n"
    "BOOT_LOC=26\n";

  static const char rcwi_pp[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "MEM_PLL_RAT[15:10]\n"
    "BOOT_LOC[264:260]\n"
    "PBI_LENGTH[287:276]\n";

  /* Step 1: compile original source */
  rcw_ctx_t *ctx1 = rcw_ctx_new();
  uint8_t *bin1 = NULL;
  size_t bin1_len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx1, src, strlen(src),
                                       &bin1, &bin1_len);
  assert_int_equal(err, RCW_OK);
  rcw_ctx_free(ctx1);

  /* Step 2: decompile via buffer API (header included) */
  rcw_ctx_t *ctx2 = rcw_ctx_new();
  char *decompiled = NULL;
  size_t decompiled_len = 0;
  err = rcw_decompile_buffer(ctx2,
                             rcwi_pp, strlen(rcwi_pp),
                             bin1, bin1_len,
                             "test.rcwi",
                             &decompiled, &decompiled_len);
  assert_int_equal(err, RCW_OK);
  rcw_ctx_free(ctx2);

  /*
   * Step 3: recompile the decompiled output.  We must drop the
   * "#include <test.rcwi>" line since rcw_compile_buffer expects
   * already-preprocessed text and there is no file to include from.
   * Concatenate the rcwi definitions with the body after the header.
   */
  const char *body = strstr(decompiled, "\n\n");
  assert_non_null(body);
  body += 2; /* skip the two newlines after the header */

  size_t combined_len = strlen(rcwi_pp) + strlen(body);
  char *combined = malloc(combined_len + 1);
  assert_non_null(combined);
  memcpy(combined, rcwi_pp, strlen(rcwi_pp));
  memcpy(combined + strlen(rcwi_pp), body, strlen(body));
  combined[combined_len] = '\0';

  rcw_ctx_t *ctx3 = rcw_ctx_new();
  uint8_t *bin2 = NULL;
  size_t bin2_len = 0;
  err = rcw_compile_buffer(ctx3, combined, combined_len,
                           &bin2, &bin2_len);
  assert_int_equal(err, RCW_OK);

  /* Step 4: compare binaries */
  assert_int_equal(bin1_len, bin2_len);
  assert_memory_equal(bin1, bin2, bin1_len);

  free(combined);
  rcw_free(decompiled);
  rcw_free(bin1);
  rcw_free(bin2);
  rcw_ctx_free(ctx3);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_decompile_roundtrip),
    cmocka_unit_test(test_decompile_fields),
    cmocka_unit_test(test_decompile_buffer_with_header),
    cmocka_unit_test(test_decompile_buffer_no_header),
    cmocka_unit_test(test_decompile_buffer_roundtrip),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

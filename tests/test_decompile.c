/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
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

/*
 * Exercise rcw_preprocess_file(): write a small .rcwi to a tempfile
 * (with a #define that mcpp must expand), preprocess it, feed the
 * result to rcw_compile_buffer(). End-to-end this proves the new
 * public preprocess API correctly invokes mcpp and produces text
 * the rest of the library can consume.
 */
static void
test_preprocess_file_roundtrip(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "#define RAT 14\n"
    "SYS_PLL_RAT[6:2]\n"
    "SYS_PLL_RAT=RAT\n"
    "PBI_LENGTH[287:276]\n";

  char tmpl[] = "/tmp/test-pp-XXXXXX";
  int fd = mkstemp(tmpl);
  assert_true(fd >= 0);
  ssize_t n = write(fd, src, sizeof(src) - 1);
  assert_int_equal(n, (ssize_t)(sizeof(src) - 1));
  close(fd);

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  char *pp = NULL;
  size_t pp_len = 0;
  rcw_error_t err = rcw_preprocess_file(ctx, tmpl, &pp, &pp_len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(pp);
  assert_true(pp_len > 0);
  /* mcpp expanded RAT -> 14 (it preserves a token boundary, so the
   * actual line reads "SYS_PLL_RAT= 14"; whitespace is irrelevant
   * since the parser strips it on non-PBI lines).
   */
  assert_non_null(strstr(pp, "14"));
  assert_null(strstr(pp, "=RAT"));
  assert_null(strstr(pp, "= RAT"));
  /* The #define line itself is consumed by the preprocessor. */
  assert_null(strstr(pp, "#define"));

  /* The preprocessed text round-trips through compile_buffer. */
  uint8_t *bin = NULL;
  size_t bin_len = 0;
  err = rcw_compile_buffer(ctx, pp, pp_len, &bin, &bin_len);
  assert_int_equal(err, RCW_OK);
  assert_true(bin_len > 0);

  rcw_free(bin);
  rcw_free(pp);
  rcw_ctx_free(ctx);
  unlink(tmpl);
}

/*
 * NOTE on mcpp re-entrancy:
 *
 * libmcpp keeps a substantial amount of static state and does not
 * fully reset between mcpp_lib_main() calls in the same process.
 * The CLI is unaffected (one invocation per fork+exec), but cmocka
 * test suites can only safely exercise rcw_preprocess_file() /
 * rcw_compile_file() / rcw_decompile_file() once per test binary.
 * That is why this file has a single round-trip preprocess test and
 * routes everything else through the buffer APIs.
 */

/*
 * A2 - the disassembler used to round pbi_len up to a multiple of 4
 * and then read 4 bytes per word, over-reading up to 3 bytes past
 * the heap allocation when pbi_len was not aligned. We exercise the
 * unaligned path under the address sanitizer (it traps any OOB read).
 */
static void
test_decompile_unaligned_pbi_no_oob(void **state) {
  (void)state;

  /*
   * Construct a binary with a valid 128-byte RCW preceded by an 8-byte
   * PBL header and 4-byte checksum, then a PBI section that ends in
   * a 1-byte tail (so total PBI region is 5 bytes, not a multiple of 4).
   *
   *   binary layout:
   *     [0..3]   preamble  (LE)
   *     [4..7]   load RCW cmd (LE)
   *     [8..135] RCW (zeroed)
   *     [136..139] checksum (zero)
   *     [140..144] PBI: 1 valid 4-byte word + 1 dangling byte
   */
  uint8_t bin[145] = {0};
  /* preamble */
  bin[0] = 0x55; bin[1] = 0xAA; bin[2] = 0x55; bin[3] = 0xAA;
  /* load-RCW cmd 0x80100000 LE */
  bin[4] = 0x00; bin[5] = 0x00; bin[6] = 0x10; bin[7] = 0x80;
  /* PBI word 0 = a valid Stop command 0x80FF0000 LE */
  bin[140] = 0x00; bin[141] = 0x00; bin[142] = 0xFF; bin[143] = 0x80;
  /* trailing dangling byte at [144] */
  bin[144] = 0xAA;

  /* Minimal rcwi - no fields needed, just the meta vars. */
  static const char rcwi[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%littleendian=1\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  char *out = NULL;
  size_t out_len = 0;
  /*
   * Pre-fix: this would over-read 3 bytes past the buffer end. ASan
   * would trap; without ASan the call would return RCW_OK with
   * garbage trailing bytes from the heap.
   * Post-fix: stops at the last full word, returns OK cleanly.
   */
  rcw_error_t err = rcw_decompile_buffer(ctx,
                                         rcwi, strlen(rcwi),
                                         bin, sizeof(bin),
                                         NULL,
                                         &out, &out_len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(out);
  /* We should have disassembled the Stop command and stopped there. */
  assert_non_null(strstr(out, "Stop command"));

  rcw_free(out);
  rcw_ctx_free(ctx);
}

/*
 * A3 - a buffer that starts with the PBL preamble but is shorter
 * than the minimum 140 bytes (preamble+cmd+RCW+checksum) used to
 * trigger an OOB read of up to 7 bytes past the buffer when
 * rcw_bits_extract() pulled 128 bytes from binary+8.
 *
 * Now: detected as no-preamble, falls into the short-buffer branch
 * and returns RCW_ERR_BAD_BINARY (no field extraction attempted).
 */
static void
test_decompile_short_preamble_no_oob(void **state) {
  (void)state;

  /* 130 bytes: preamble + load-cmd + only 122 bytes of "RCW" -> too short. */
  uint8_t bin[130] = {0};
  bin[0] = 0x55; bin[1] = 0xAA; bin[2] = 0x55; bin[3] = 0xAA;
  bin[4] = 0x00; bin[5] = 0x00; bin[6] = 0x10; bin[7] = 0x80;

  static const char rcwi[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%littleendian=1\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  char *out = NULL;
  size_t out_len = 0;
  rcw_error_t err = rcw_decompile_buffer(ctx,
                                         rcwi, strlen(rcwi),
                                         bin, sizeof(bin),
                                         NULL,
                                         &out, &out_len);
  /*
   * 130 bytes is below the 140-byte PBL minimum, so we ignore the
   * preamble bytes and go down the no-preamble branch. That branch
   * needs >= 128 bytes of RCW; 130 is fine, so decompile succeeds
   * but treats the buffer as raw RCW.
   */
  assert_int_equal(err, RCW_OK);
  rcw_free(out);

  /* And a buffer truly shorter than RCW_SIZE_BYTES is rejected. */
  out = NULL;
  err = rcw_decompile_buffer(ctx,
                             rcwi, strlen(rcwi),
                             bin, 64, /* well under 128 */
                             NULL,
                             &out, &out_len);
  assert_int_equal(err, RCW_ERR_BAD_BINARY);

  rcw_ctx_free(ctx);
}

/*
 * PBI mnemonic seam: decompiler emitter vs rcw_pbi.c parser.
 *
 * test_pbi.c already covers ENCODING of every awrite width, and the
 * round-trip test above covers decompile+recompile but only for a
 * single plain "write". Nothing crossed the two, so a decompiler that
 * emitted a bare "awrite" for a B=4/B=5 command went unnoticed: the
 * source still listed every data word, but recompiling parsed it as
 * B=3 and silently dropped all but the first.
 *
 * These tests assert exact byte counts, not just equality, so a
 * truncation cannot hide behind a coincidentally-matching prefix.
 */

/* Minimal .rcwi shared by the seam tests below. */
static const char seam_rcwi[] =
  "%size=1024\n"
  "%pbiformat=2\n"
  "%classicbitnumbers=1\n"
  "%littleendian=1\n"
  "%nocrc=1\n"
  "SYS_PLL_RAT[6:2]\n"
  "PBI_LENGTH[287:276]\n";

/*
 * Compile `pbi_body` (a .pbi block), decompile it, recompile the
 * result, and hand both binaries back to the caller. Any step failing
 * is a test failure.
 */
static void
seam_roundtrip(const char *pbi_body,
               uint8_t **bin1, size_t *bin1_len,
               uint8_t **bin2, size_t *bin2_len,
               char **decompiled) {
  char src[4096];
  int n = snprintf(src, sizeof(src),
                   "%sSYS_PLL_RAT=7\n.pbi\n%s.end\n", seam_rcwi, pbi_body);
  assert_true(n > 0 && (size_t)n < sizeof(src));

  rcw_ctx_t *c1 = rcw_ctx_new();
  assert_non_null(c1);
  assert_int_equal(rcw_compile_buffer(c1, src, strlen(src), bin1, bin1_len),
                   RCW_OK);

  rcw_ctx_t *c2 = rcw_ctx_new();
  assert_non_null(c2);
  assert_int_equal(rcw_parse(c2, seam_rcwi, strlen(seam_rcwi)), RCW_OK);

  char *out = NULL;
  size_t out_len = 0;
  assert_int_equal(rcw_decompile(c2, *bin1, *bin1_len, &out, &out_len),
                   RCW_OK);
  assert_non_null(out);

  size_t comb_len = strlen(seam_rcwi) + out_len;
  char *comb = malloc(comb_len + 1);
  assert_non_null(comb);
  memcpy(comb, seam_rcwi, strlen(seam_rcwi));
  memcpy(comb + strlen(seam_rcwi), out, out_len);
  comb[comb_len] = '\0';

  rcw_ctx_t *c3 = rcw_ctx_new();
  assert_non_null(c3);
  assert_int_equal(rcw_compile_buffer(c3, comb, comb_len, bin2, bin2_len),
                   RCW_OK);

  free(comb);
  if (decompiled)
    *decompiled = out;
  else
    rcw_free(out);
  rcw_ctx_free(c1);
  rcw_ctx_free(c2);
  rcw_ctx_free(c3);
}

/*
 * The decompiler must spell the width into the mnemonic, and must
 * still emit every data word.
 */
static void
test_decompile_awrite_emits_width_suffix(void **state) {
  (void)state;

  uint8_t *b1 = NULL, *b2 = NULL;
  size_t l1 = 0, l2 = 0;
  char *out = NULL;

  seam_roundtrip("awrite 0x0400098,0x11111111\n"
                 "awrite.b4 0x0100000,0x22222222,0x33333333\n"
                 "awrite.b5 0x2508000,0x44444444,0x55555555,"
                            "0x66666666,0x77777777\n",
                 &b1, &l1, &b2, &l2, &out);

  /* Mnemonics, each at the start of its own line. */
  assert_non_null(strstr(out, "\nawrite 0x00400098,0x11111111\n"));
  assert_non_null(strstr(out,
      "\nawrite.b4 0x00100000,0x22222222,0x33333333\n"));
  assert_non_null(strstr(out,
      "\nawrite.b5 0x02508000,0x44444444,0x55555555,"
      "0x66666666,0x77777777\n"));

  /* A bare "awrite " must not be used for the wide forms. */
  assert_null(strstr(out, "\nawrite 0x00100000"));
  assert_null(strstr(out, "\nawrite 0x02508000"));

  rcw_free(out);
  rcw_free(b1);
  rcw_free(b2);
}

/*
 * Byte-exact regression for the 992-byte truncation. PBI payload:
 *   awrite    (B=3)  4 + 4      =  8
 *   awrite.b4 (B=4)  4 + 8      = 12
 *   awrite.b5 (B=5)  4 + 16     = 20
 *                              --- 40
 * plus 140 header (preamble + Load RCW + RCW + checksum) and an
 * 8-byte STOP terminator (%nocrc=1) = 188.
 */
static void
test_decompile_roundtrip_awrite_widths(void **state) {
  (void)state;

  uint8_t *b1 = NULL, *b2 = NULL;
  size_t l1 = 0, l2 = 0;

  seam_roundtrip("awrite 0x0400098,0x11111111\n"
                 "awrite.b4 0x0100000,0x22222222,0x33333333\n"
                 "awrite.b5 0x2508000,0x44444444,0x55555555,"
                            "0x66666666,0x77777777\n",
                 &b1, &l1, &b2, &l2, NULL);

  assert_int_equal(l1, 140 + 40 + 8);
  assert_int_equal(l2, l1);              /* no silent shrink */
  assert_memory_equal(b1, b2, l1);

  rcw_free(b1);
  rcw_free(b2);
}

/*
 * Every other mnemonic the decompiler can emit, in one image, so the
 * emitter/parser seam is covered as a whole rather than per-command.
 *
 * Sizes: loadacwindow 4, write 8, write.b1 8, wait 4,
 * blockcopy 16 = 40.
 * poll is excluded, see the dedicated test below.
 */
static void
test_decompile_roundtrip_all_mnemonics(void **state) {
  (void)state;

  uint8_t *b1 = NULL, *b2 = NULL;
  size_t l1 = 0, l2 = 0;
  char *out = NULL;

  seam_roundtrip("loadacwindow 0x00000005\n"
                 "write 0x01e00620,0x80000000\n"
                 "write.b1 0x021c0000,0x00000049\n"
                 "wait 0x00000800\n"
                 "blockcopy 0x0f,0x00009000,0x1800d000,0x00020000\n",
                 &b1, &l1, &b2, &l2, &out);

  assert_int_equal(l1, 140 + 40 + 8);
  assert_int_equal(l2, l1);
  assert_memory_equal(b1, b2, l1);

  assert_non_null(strstr(out, "\nloadacwindow 0x00000005\n"));
  assert_non_null(strstr(out, "\nwrite 0x01e00620,0x80000000\n"));
  assert_non_null(strstr(out, "\nwrite.b1 0x021c0000,0x00000049\n"));
  assert_non_null(strstr(out, "\nwait 0x00000800\n"));
  assert_non_null(strstr(out,
      "\nblockcopy 0x0f,0x00009000,0x1800d000,0x00020000\n"));

  rcw_free(out);
  rcw_free(b1);
  rcw_free(b2);
}

/*
 * poll round-trips now that rcw_pbi.c emits the four words of
 * LX2160ARM Section 8.3.11 Table 44 (word0 = HDR|CMD|0x0000, then
 * ADDR, MASK, CONDITION) and agrees with the decompiler, which
 * already followed the RM.
 *
 * Before that fix the encoder wrote three words with the address
 * ORed into word0: the decompiler then read one word too many and
 * printed MASK, CONDITION and the NEXT command's header as the three
 * arguments, desynchronising the rest of the stream. A wide address
 * also corrupted the command byte, turning a short poll into a long
 * one. Both are covered here and in test_pbi.c.
 *
 * Sizes: poll 16 + poll.long 16 = 32.
 */
static void
test_decompile_roundtrip_poll(void **state) {
  (void)state;

  uint8_t *b1 = NULL, *b2 = NULL;
  size_t l1 = 0, l2 = 0;
  char *out = NULL;

  seam_roundtrip("poll 0x01e00620,0xffffffff,0x80000000\n"
                 "poll.long 0x01e00624,0x0000000f,0x00000001\n",
                 &b1, &l1, &b2, &l2, &out);

  assert_int_equal(l1, 140 + 32 + 8);
  assert_int_equal(l2, l1);
  assert_memory_equal(b1, b2, l1);

  /* Address preserved, and the two forms stay distinct. */
  assert_non_null(strstr(out,
      "\npoll.short 0x01e00620,0xffffffff,0x80000000\n"));
  assert_non_null(strstr(out,
      "\npoll.long 0x01e00624,0x0000000f,0x00000001\n"));
  assert_null(strstr(out, "Unknown word"));

  rcw_free(out);
  rcw_free(b1);
  rcw_free(b2);
}

/*
 * Known gap, pinned deliberately: rcw_decompile.c emits
 * "loadcondition" for PBI command 0x14, but rcw_pbi.c has no such
 * mnemonic, so the decompiled source does NOT recompile: it fails
 * with "Unknown PBI command". Unlike the awrite case this is a loud
 * failure rather than silent corruption, which is why it is recorded
 * rather than worked around.
 *
 * If parser support for loadcondition is added, this test will start
 * failing: replace it with a byte-identity round-trip like the ones
 * above.
 */
static void
test_decompile_loadcondition_does_not_recompile(void **state) {
  (void)state;

  /* Hand-build a PBI: Load Condition (0x14) is 12 bytes. */
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n"
    "SYS_PLL_RAT=7\n";

  rcw_ctx_t *c1 = rcw_ctx_new();
  assert_non_null(c1);
  uint8_t *bin = NULL;
  size_t bin_len = 0;
  assert_int_equal(rcw_compile_buffer(c1, src, strlen(src), &bin, &bin_len),
                   RCW_OK);

  /* Splice a Load Condition command in front of the terminator. */
  static const uint8_t loadcond[12] = {
    0x00, 0x00, 0x14, 0x80,   /* 0x80140000, little-endian */
    0x78, 0x56, 0x34, 0x12,
    0x21, 0x43, 0x65, 0x87,
  };
  size_t spliced_len = bin_len + sizeof(loadcond);
  uint8_t *spliced = malloc(spliced_len);
  assert_non_null(spliced);
  memcpy(spliced, bin, bin_len - 8);
  memcpy(spliced + bin_len - 8, loadcond, sizeof(loadcond));
  memcpy(spliced + bin_len - 8 + sizeof(loadcond), bin + bin_len - 8, 8);

  rcw_ctx_t *c2 = rcw_ctx_new();
  assert_non_null(c2);
  assert_int_equal(rcw_parse(c2, seam_rcwi, strlen(seam_rcwi)), RCW_OK);

  char *out = NULL;
  size_t out_len = 0;
  assert_int_equal(rcw_decompile(c2, spliced, spliced_len, &out, &out_len),
                   RCW_OK);
  assert_non_null(strstr(out, "loadcondition 0x12345678,0x87654321"));

  /* ... and that spelling is not accepted back. */
  size_t comb_len = strlen(seam_rcwi) + out_len;
  char *comb = malloc(comb_len + 1);
  assert_non_null(comb);
  memcpy(comb, seam_rcwi, strlen(seam_rcwi));
  memcpy(comb + strlen(seam_rcwi), out, out_len);
  comb[comb_len] = '\0';

  rcw_ctx_t *c3 = rcw_ctx_new();
  assert_non_null(c3);
  uint8_t *bin2 = NULL;
  size_t bin2_len = 0;
  assert_int_equal(rcw_compile_buffer(c3, comb, comb_len, &bin2, &bin2_len),
                   RCW_ERR_PBI_SYNTAX);

  free(comb);
  free(spliced);
  rcw_free(out);
  rcw_free(bin);
  rcw_ctx_free(c1);
  rcw_ctx_free(c2);
  rcw_ctx_free(c3);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_decompile_roundtrip),
    cmocka_unit_test(test_decompile_awrite_emits_width_suffix),
    cmocka_unit_test(test_decompile_roundtrip_awrite_widths),
    cmocka_unit_test(test_decompile_roundtrip_all_mnemonics),
    cmocka_unit_test(test_decompile_roundtrip_poll),
    cmocka_unit_test(test_decompile_loadcondition_does_not_recompile),
    cmocka_unit_test(test_decompile_fields),
    cmocka_unit_test(test_decompile_buffer_with_header),
    cmocka_unit_test(test_decompile_buffer_no_header),
    cmocka_unit_test(test_decompile_buffer_roundtrip),
    cmocka_unit_test(test_preprocess_file_roundtrip),
    cmocka_unit_test(test_decompile_unaligned_pbi_no_oob),
    cmocka_unit_test(test_decompile_short_preamble_no_oob),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

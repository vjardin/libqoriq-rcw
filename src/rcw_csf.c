/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * Helper for the Load Security Header PBI command (RM Table 41), which
 * carries 20 CSF words. Those words are produced by NXP's Code Signing
 * Tool, so they are normally kept in a file rather than pasted into the
 * .rcw source but let's be feature complete.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rcw_internal.h"

/* Open path, honouring the ctx include search list. Caller fcloses. */
static FILE *
csf_fopen(rcw_ctx_t *ctx, const char *path) {
  FILE *f = fopen(path, "r");
  if (f)
    return f;

  for (size_t i = 0; i < ctx->include_path_count; i++) {
    char buf[RCW_PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%s/%s", ctx->include_paths[i], path);
    if (n <= 0 || (size_t)n >= sizeof(buf))
      continue;
    f = fopen(buf, "r");
    if (f)
      return f;
  }
  return NULL;
}

/*
 * Read exactly PBI_CSF_HEADER_WORDS hex words from `path` into `out`.
 *
 * Strict on count in both directions: too few means a truncated CSF header
 * and too many means the wrong file entirely, and either would otherwise be
 * encoded into a PBL that the Service Processor rejects at boot with
 * nothing to explain why.
 */
rcw_error_t
rcw_csf_read_words(rcw_ctx_t *ctx, const char *path,
                   uint32_t *out, size_t want) {
  FILE *f = csf_fopen(ctx, path);
  if (!f) {
    rcw_set_error(ctx, "cannot open CSF header file \"%s\" "
                       "(searched the working directory and %zu include path(s))",
                  path, ctx->include_path_count);
    return RCW_ERR_PBI_SYNTAX;
  }

  size_t n = 0;
  int c;
  char tok[32];

  while ((c = fgetc(f)) != EOF) {
    /* Line comments: # ... and // ... */
    if (c == '#') {
      while ((c = fgetc(f)) != EOF && c != '\n')
        ;
      continue;
    }
    if (c == '/') {
      int c2 = fgetc(f);
      if (c2 == '/') {
        while ((c = fgetc(f)) != EOF && c != '\n')
          ;
        continue;
      }
      if (c2 != EOF)
        ungetc(c2, f);
      rcw_set_error(ctx, "stray '/' in CSF header file \"%s\"", path);
      fclose(f);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (isspace(c) || c == ',')
      continue;

    /* Collect one token. */
    size_t len = 0;
    do {
      if (len + 1 >= sizeof(tok)) {
        rcw_set_error(ctx, "over-long token in CSF header file \"%s\"", path);
        fclose(f);
        return RCW_ERR_PBI_SYNTAX;
      }
      tok[len++] = (char)c;
      c = fgetc(f);
    } while (c != EOF && !isspace(c) && c != ',');
    tok[len] = '\0';

    if (n >= want) {
      rcw_set_error(ctx,
          "CSF header file \"%s\" holds more than %zu words", path, want);
      fclose(f);
      return RCW_ERR_PBI_SYNTAX;
    }

    char *end = NULL;
    unsigned long v = strtoul(tok, &end, 16);   /* always hex, 0x optional */
    if (!end || *end != '\0') {
      rcw_set_error(ctx,
          "\"%s\" in CSF header file \"%s\" is not a hex word", tok, path);
      fclose(f);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (v > 0xFFFFFFFFul) {
      rcw_set_error(ctx,
          "\"%s\" in CSF header file \"%s\" exceeds 32 bits", tok, path);
      fclose(f);
      return RCW_ERR_PBI_SYNTAX;
    }
    out[n++] = (uint32_t)v;
  }
  fclose(f);

  if (n != want) {
    rcw_set_error(ctx,
        "CSF header file \"%s\" holds %zu words, expected exactly %zu",
        path, n, want);
    return RCW_ERR_PBI_SYNTAX;
  }
  return RCW_OK;
}

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 */

#define _POSIX_C_SOURCE 200809L

#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "rcw_internal.h"

rcw_ctx_t *
rcw_ctx_new(void) {
  rcw_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->pbl = true;
  ctx->vars.pbladdr = RCW_DEFAULT_PBLADDR; /* rcw.py default */

  return ctx;
}

void
rcw_ctx_free(rcw_ctx_t *ctx) {
  if (!ctx)
    return;

  for (size_t i = 0; i < ctx->include_path_count; i++)
    free(ctx->include_paths[i]);

  free(ctx->pbi.data);
  free(ctx);
}

rcw_error_t
rcw_ctx_add_include_path(rcw_ctx_t *ctx, const char *path) {
  if (ctx->include_path_count >= RCW_MAX_INCLUDE_PATHS)
    return RCW_ERR_NOMEM;

  ctx->include_paths[ctx->include_path_count] = strdup(path);
  if (!ctx->include_paths[ctx->include_path_count])
    return RCW_ERR_NOMEM;

  ctx->include_path_count++;

  return RCW_OK;
}

rcw_error_t
rcw_ctx_set_pbl(rcw_ctx_t *ctx, int enable) {
  ctx->pbl = enable != 0;

  return RCW_OK;
}

rcw_error_t
rcw_ctx_set_warnings(rcw_ctx_t *ctx, int enable) {
  ctx->warnings = enable != 0;

  return RCW_OK;
}

rcw_error_t
rcw_ctx_set_bitfield(rcw_ctx_t *ctx, const char *name, uint64_t value) {
  if (ctx->override_count >= RCW_MAX_OVERRIDES)
    return RCW_ERR_NOMEM;

  snprintf(ctx->overrides[ctx->override_count].name, RCW_FIELD_NAME_MAX, "%s", name);

  ctx->overrides[ctx->override_count].value = value;
  ctx->override_count++;

  return RCW_OK;
}

static void
ctx_reset_state(rcw_ctx_t *ctx) {

  memset(&ctx->symbols, 0, sizeof(ctx->symbols));
  memset(&ctx->vars, 0, sizeof(ctx->vars));
  ctx->vars.pbladdr = RCW_DEFAULT_PBLADDR;
  free(ctx->pbi.data);
  memset(&ctx->pbi, 0, sizeof(ctx->pbi));
  ctx->error_detail[0] = '\0';
}

static rcw_error_t
ctx_apply_overrides(rcw_ctx_t *ctx) {
  for (size_t i = 0; i < ctx->override_count; i++) {
    const char *name = ctx->overrides[i].name;
    uint64_t value = ctx->overrides[i].value;
    bool found = false;

    for (size_t j = 0; j < ctx->symbols.count; j++) {
      if (strcmp(ctx->symbols.entries[j].name, name) == 0) {
        ctx->symbols.entries[j].has_value = true;
        ctx->symbols.entries[j].value = value;
        found = true;

        break;
      }
    }

    if (!found) {
      rcw_set_error(ctx, "Unknown bitfield: %s", name);
      return RCW_ERR_UNKNOWN_FIELD;
    }
  }

  return RCW_OK;
}

rcw_error_t
rcw_compile_buffer(rcw_ctx_t *ctx, const char *preprocessed, size_t len, uint8_t **out_data, size_t *out_len) {
  ctx_reset_state(ctx);

  rcw_error_t err = rcw_parse(ctx, preprocessed, len);
  if (err != RCW_OK)
    return err;

  err = ctx_apply_overrides(ctx);
  if (err != RCW_OK)
    return err;

  if (ctx->vars.pbiformat != 2) {
    rcw_set_error(ctx, "Only pbiformat=2 is supported (got %d)", ctx->vars.pbiformat);
    return RCW_ERR_UNSUPPORTED;
  }
  if (ctx->vars.size != RCW_SIZE_BITS) {
    rcw_set_error(ctx, "%%size must be %d (got %d)", RCW_SIZE_BITS, ctx->vars.size);
    return RCW_ERR_MISSING_VAR;
  }

  return rcw_binary_generate(ctx, out_data, out_len);
}

rcw_error_t
rcw_compile_file(rcw_ctx_t *ctx, const char *input_path, uint8_t **out_data, size_t *out_len) {
  char *preprocessed = NULL;
  size_t pp_len = 0;

  rcw_error_t err = rcw_preprocess(ctx, input_path, &preprocessed, &pp_len);

  if (err != RCW_OK)
    return err;

  err = rcw_compile_buffer(ctx, preprocessed, pp_len, out_data, out_len);
  free(preprocessed);

  return err;
}

/*
 * Prepend "#include <name>\n\n" to body, matching rcw.py line 814:
 *     source = '#include <%s>\n\n' % os.path.basename(options.rcwi)
 * Frees body unconditionally and returns the new buffer in *out / *out_len.
 */
static rcw_error_t
prepend_include(char *body, size_t body_len, const char *name,
                char **out, size_t *out_len) {
  size_t header_len = strlen("#include <") + strlen(name) + strlen(">\n\n");
  size_t total = header_len + body_len;
  char *buf = malloc(total + 1);
  if (!buf) {
    free(body);
    return RCW_ERR_NOMEM;
  }

  int n = snprintf(buf, header_len + 1, "#include <%s>\n\n", name);
  if (n < 0) {
    free(buf);
    free(body);
    return RCW_ERR_IO;
  }
  memcpy(buf + (size_t)n, body, body_len);
  buf[(size_t)n + body_len] = '\0';
  free(body);

  *out = buf;
  *out_len = (size_t)n + body_len;

  return RCW_OK;
}

rcw_error_t
rcw_decompile_buffer(rcw_ctx_t *ctx,
                     const char *rcwi_preprocessed, size_t rcwi_len,
                     const uint8_t *binary, size_t binary_len,
                     const char *rcwi_name,
                     char **out_source, size_t *out_len) {
  ctx_reset_state(ctx);

  rcw_error_t err = rcw_parse(ctx, rcwi_preprocessed, rcwi_len);
  if (err != RCW_OK)
    return err;

  char *body = NULL;
  size_t body_len = 0;
  err = rcw_decompile(ctx, binary, binary_len, &body, &body_len);
  if (err != RCW_OK)
    return err;

  if (rcwi_name == NULL) {
    *out_source = body;
    *out_len = body_len;

    return RCW_OK;
  }

  return prepend_include(body, body_len, rcwi_name, out_source, out_len);
}

rcw_error_t
rcw_decompile_file(rcw_ctx_t *ctx, const char *bin_path, const char *rcwi_path, char **out_source, size_t *out_len) {
  /* Run gcc -E on the .rcwi to expand its macros */
  char *rcwi_pp = NULL;
  size_t rcwi_len = 0;

  rcw_error_t err = rcw_preprocess(ctx, rcwi_path, &rcwi_pp, &rcwi_len);
  if (err != RCW_OK)
    return err;

  /* Read the binary file */
  FILE *f = fopen(bin_path, "rb");
  if (!f) {
    free(rcwi_pp);
    rcw_set_error(ctx, "Cannot open %s", bin_path);

    return RCW_ERR_IO;
  }
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);

  uint8_t *binary = malloc((size_t)fsize);
  if (!binary) {
    fclose(f);
    free(rcwi_pp);

    return RCW_ERR_NOMEM;
  }

  if (fread(binary, 1, (size_t)fsize, f) != (size_t)fsize) {
    fclose(f);
    free(binary);
    free(rcwi_pp);
    rcw_set_error(ctx, "Failed to read %s", bin_path);

    return RCW_ERR_IO;
  }

  fclose(f);

  /*
   * Compute basename(rcwi_path) for the #include header.
   * basename(3) may modify its argument, so work on a copy.
   */
  char *rcwi_copy = strdup(rcwi_path);
  if (!rcwi_copy) {
    free(binary);
    free(rcwi_pp);
    return RCW_ERR_NOMEM;
  }
  const char *base = basename(rcwi_copy);

  err = rcw_decompile_buffer(ctx, rcwi_pp, rcwi_len,
                             binary, (size_t)fsize, base,
                             out_source, out_len);
  free(rcwi_copy);
  free(binary);
  free(rcwi_pp);

  return err;
}

const char *
rcw_strerror(rcw_error_t err) {
  switch (err) {
    case RCW_OK:
      return "Success";
    case RCW_ERR_NOMEM:
      return "Out of memory";
    case RCW_ERR_IO:
      return "I/O error";
    case RCW_ERR_PREPROCESS:
      return "Preprocessing failed";
    case RCW_ERR_PARSE:
      return "Parse error";
    case RCW_ERR_OVERLAP:
      return "Bitfield overlap";
    case RCW_ERR_UNKNOWN_FIELD:
      return "Unknown bitfield";
    case RCW_ERR_VALUE_OVERFLOW:
      return "Value too large for field";
    case RCW_ERR_PBI_SYNTAX:
      return "PBI syntax error";
    case RCW_ERR_MISSING_VAR:
      return "Required variable not set";
    case RCW_ERR_BAD_BINARY:
      return "Invalid binary format";
    case RCW_ERR_UNSUPPORTED:
      return "Unsupported feature";
  }

  return "Unknown error";
}

const char *
rcw_ctx_last_error_detail(const rcw_ctx_t *ctx) {
  return ctx->error_detail;
}

void
rcw_free(void *ptr) {
  free(ptr);
}

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * QorIQ Reset Configuration Word (RCW) compiler library.
 * Compiles .rcw source files into PBL/RCW binary images and
 * decompiles binaries back to source (pbiformat=2 platforms).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QORIQ_RCW_VERSION_MAJOR 0
#define QORIQ_RCW_VERSION_MINOR 1
#define QORIQ_RCW_VERSION_PATCH 0

/* Error codes */
typedef enum {
  RCW_OK = 0,
  RCW_ERR_NOMEM,
  RCW_ERR_IO,
  RCW_ERR_PREPROCESS,
  RCW_ERR_PARSE,
  RCW_ERR_OVERLAP,
  RCW_ERR_UNKNOWN_FIELD,
  RCW_ERR_VALUE_OVERFLOW,
  RCW_ERR_PBI_SYNTAX,
  RCW_ERR_MISSING_VAR,
  RCW_ERR_BAD_BINARY,
  RCW_ERR_UNSUPPORTED,
} rcw_error_t;

/* Opaque context - all state lives here, no globals. */
typedef struct rcw_ctx rcw_ctx_t;

/*
 * Lifecycle
 */
rcw_ctx_t *rcw_ctx_new(void);
void rcw_ctx_free(rcw_ctx_t *ctx);

/*
 * Configuration (call before compilation)
 */
rcw_error_t rcw_ctx_add_include_path(rcw_ctx_t *ctx, const char *path);
rcw_error_t rcw_ctx_set_pbl(rcw_ctx_t *ctx, int enable);
rcw_error_t rcw_ctx_set_warnings(rcw_ctx_t *ctx, int enable);

/* Override a bitfield value (equivalent to rcw.py -D FIELD=VALUE).
 * Can be called multiple times. Overrides apply after source parsing. */
rcw_error_t rcw_ctx_set_bitfield(rcw_ctx_t *ctx, const char *name, uint64_t value);

/*
 * Forward compilation: .rcw -> .bin
 *
 * rcw_compile_file() runs the C preprocessor (gcc -E) on input_path,
 * then parses and compiles the result.
 *
 * rcw_compile_buffer() takes already-preprocessed text (for embedding
 * or testing without gcc).
 *
 * Both allocate *out_data; caller must free it with rcw_free().
 */
rcw_error_t rcw_compile_file(rcw_ctx_t *ctx, const char *input_path, uint8_t **out_data, size_t *out_len);
rcw_error_t rcw_compile_buffer(rcw_ctx_t *ctx, const char *preprocessed, size_t len, uint8_t **out_data, size_t *out_len);

/*
 * Reverse decompilation: .bin -> .rcw text
 *
 * rcw_decompile_file() preprocesses rcwi_path with the C preprocessor
 * (gcc -E) to obtain symbol definitions, then reads bin_path and
 * produces RCW source text. The output is prefixed with
 *     #include <basename(rcwi_path)>
 *
 * rcw_decompile_buffer() takes already-preprocessed .rcwi text and an
 * in-memory binary buffer. No preprocessor is invoked and no file
 * I/O is performed - useful for embedding the library in environments
 * where gcc is not available, or for unit testing. If rcwi_name is
 * non-NULL, the output is prefixed with "#include <rcwi_name>\n\n";
 * pass NULL to omit the header.
 *
 * Both allocate *out_source; caller must free it with rcw_free().
 */
rcw_error_t rcw_decompile_file(rcw_ctx_t *ctx, const char *bin_path, const char *rcwi_path, char **out_source, size_t *out_len);
rcw_error_t rcw_decompile_buffer(rcw_ctx_t *ctx, const char *rcwi_preprocessed, size_t rcwi_len, const uint8_t *binary, size_t binary_len, const char *rcwi_name, char **out_source, size_t *out_len);

/*
 * Error information
 */
const char *rcw_strerror(rcw_error_t err);
const char *rcw_ctx_last_error_detail(const rcw_ctx_t *ctx);

/*
 * Memory management - free any buffer allocated by the library.
 */
void rcw_free(void *ptr);

#ifdef __cplusplus
}
#endif

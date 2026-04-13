/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * C preprocessor wrapper - embeds mcpp (Matsui C Preprocessor) to
 * expand #include / #define / #ifdef / #if in .rcw/.rcwi files.
 *
 * No fork/exec, no gcc at runtime: the whole preprocessor runs
 * in-process via libmcpp, so the library works on targets that do
 * not ship a toolchain.
 *
 * See http://mcpp.sourceforge.net/ - mcpp is BSD-2-Clause licensed
 * and validated against the ISO C99/C11 preprocessor conformance tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <mcpp_lib.h>
#include <mcpp_out.h>

#include "rcw_internal.h"

/*
 * Build an argv for mcpp_lib_main():
 *
 *   mcpp -P -I . [-I path]... <input_path>
 *
 * -P  Suppress #line marker output.
 * -I  Add a directory to the include search path.
 *
 * argv[0] is conventionally the program name. mcpp itself ignores it.
 * Max argv slots: "mcpp" + "-P" + "-I" + "." + 2*N includes + input + NULL
 *               =  1    +  1   +  1   +  1  + 2*N           +   1   +  1
 *               =  6 + 2*N
 */
#define MCPP_ARGV_FIXED  6

rcw_error_t
rcw_preprocess(const rcw_ctx_t *ctx, const char *input_path,
               char **out, size_t *out_len) {
  size_t max_argv = MCPP_ARGV_FIXED + 2 * ctx->include_path_count;
  char **argv = calloc(max_argv, sizeof(char *));
  if (!argv)
    return RCW_ERR_NOMEM;

  int argc = 0;
  argv[argc++] = (char *)"mcpp";
  argv[argc++] = (char *)"-P";
  argv[argc++] = (char *)"-I";
  argv[argc++] = (char *)".";

  for (size_t i = 0; i < ctx->include_path_count; i++) {
    argv[argc++] = (char *)"-I";
    argv[argc++] = ctx->include_paths[i];
  }
  argv[argc++] = (char *)input_path;
  argv[argc]   = NULL;

  /*
   * Ask mcpp to buffer output in memory instead of writing to fp_out /
   * fp_err. The buffers are owned by mcpp; we strdup() what we need
   * before returning since the caller will call mcpp_lib_main() again
   * on the next preprocess invocation and that resets the buffers.
   */
  mcpp_use_mem_buffers(1);

  int rc = mcpp_lib_main(argc, argv);

  char *mcpp_out = mcpp_get_mem_buffer(OUT);
  char *mcpp_err = mcpp_get_mem_buffer(ERR);

  /* Forward mcpp diagnostics to stderr (mirrors the gcc -E behavior) */
  if (mcpp_err && *mcpp_err)
    fprintf(stderr, "%s", mcpp_err);

  free(argv);

  if (rc != 0) {
    /*
     * mcpp_get_mem_buffer(OUT) is still owned by mcpp after a failed
     * run; we do not free it here. The next mcpp_lib_main() call
     * (if any) will reset the buffers.
     */
    return RCW_ERR_PREPROCESS;
  }

  if (!mcpp_out) {
    /* No error but also no output - treat as empty input */
    char *empty = malloc(1);
    if (!empty)
      return RCW_ERR_NOMEM;
    empty[0] = '\0';
    *out = empty;
    *out_len = 0;
    return RCW_OK;
  }

  /*
   * Copy the mcpp output buffer to a caller-owned allocation so the
   * caller can free it with free() / rcw_free() and so subsequent
   * mcpp invocations on the same thread do not clobber it.
   */
  size_t len = strlen(mcpp_out);
  char *buf = malloc(len + 1);
  if (!buf)
    return RCW_ERR_NOMEM;
  memcpy(buf, mcpp_out, len + 1);

  *out = buf;
  *out_len = len;

  return RCW_OK;
}

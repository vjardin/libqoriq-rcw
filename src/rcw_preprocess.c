/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * C preprocessor wrapper - runs gcc -E on .rcw/.rcwi files.
 * TODO: stop using gcc -E since it cannot be used on a target without gcc
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rcw_internal.h"

rcw_error_t
rcw_preprocess(const rcw_ctx_t *ctx, const char *input_path, char **out, size_t *out_len) {
  /*
   * Build the command: gcc -E -x c -P -I . [-I path]... input_path
   * Max args: "gcc" "-E" "-x" "c" "-P" "-I" "." + 2*N includes +
   *           input_path + NULL
   */
  size_t max_args = 7 + 2 * ctx->include_path_count + 1 + 1;
  const char **argv = calloc(max_args, sizeof(char *));
  if (!argv)
    return RCW_ERR_NOMEM;

  size_t a = 0;
  argv[a++] = "gcc";
  argv[a++] = "-E";
  argv[a++] = "-x";
  argv[a++] = "c";
  argv[a++] = "-P";
  argv[a++] = "-I";
  argv[a++] = ".";

  for (size_t i = 0; i < ctx->include_path_count; i++) {
    argv[a++] = "-I";
    argv[a++] = ctx->include_paths[i];
  }
  argv[a++] = input_path;
  argv[a] = NULL;

  /* Create pipes for stdout and stderr */
  int stdout_pipe[2];
  int stderr_pipe[2];

  if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
    free(argv);
    return RCW_ERR_IO;
  }

  pid_t pid = fork();
  if (pid < 0) {
    free(argv);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);

    return RCW_ERR_IO;
  }

  if (pid == 0) {
    /* Child */
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    execvp("gcc", (char *const *)argv);
    _exit(127);
  }

  /* Parent */
  free(argv);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  /* Read stdout */
  size_t cap = 64 * 1024;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    return RCW_ERR_NOMEM;
  }

  for (;;) {
    if (len + 4096 > cap) {
      cap *= 2;
      char *p = realloc(buf, cap);
      if (!p) {
        free(buf);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        return RCW_ERR_NOMEM;
      }
      buf = p;
    }
    ssize_t n = read(stdout_pipe[0], buf + len, cap - len);
    if (n <= 0)
      break;
    len += (size_t)n;
  }
  close(stdout_pipe[0]);

  /* Read stderr for error messages */
  char errbuf[4096];
  size_t errlen = 0;
  for (;;) {
    ssize_t n = read(stderr_pipe[0], errbuf + errlen, sizeof(errbuf) - errlen - 1);
    if (n <= 0)
      break;
    errlen += (size_t)n;
  }
  close(stderr_pipe[0]);
  errbuf[errlen] = '\0';

  /* Wait for child */
  int status;
  waitpid(pid, &status, 0);

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    free(buf);
    /* Print gcc errors to stderr like rcw.py does */
    if (errlen > 0)
      fprintf(stderr, "%s", errbuf);
    return RCW_ERR_PREPROCESS;
  }

  *out = buf;
  *out_len = len;

  return RCW_OK;
}

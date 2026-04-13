/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * Parser for preprocessed .rcw/.rcwi source files.
 * Mirrors rcw.py parse_source_file() (lines 445-520).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "rcw_internal.h"

/* Find a symbol by name, return index or -1. */
static int
symtab_find(const rcw_symtab_t *tab, const char *name) {
  for (size_t i = 0; i < tab->count; i++) {
    if (strcmp(tab->entries[i].name, name) == 0)
      return (int)i;
  }

  return -1;
}

/* Check for bitfield overlap. */
static rcw_error_t
check_overlap(rcw_ctx_t *ctx, const char *name, int begin, int end) {
  int b = (begin < end) ? begin : end;
  int e = (begin < end) ? end : begin;

  if (symtab_find(&ctx->symbols, name) >= 0) {
    rcw_set_error(ctx, "Duplicate bitfield definition for %s", name);

    return RCW_ERR_OVERLAP;
  }

  for (size_t i = 0; i < ctx->symbols.count; i++) {
    const rcw_symbol_t *s = &ctx->symbols.entries[i];
    int sb = (s->begin < s->end) ? s->begin : s->end;
    int se = (s->begin < s->end) ? s->end : s->begin;

    if ((sb <= b && b <= se) || (sb <= e && e <= se) ||
        (b <= sb && sb <= e) || (b <= se && se <= e)) {
      rcw_set_error(ctx, "Bitfield %s overlaps with %s", name, s->name);

      return RCW_ERR_OVERLAP;
    }
  }

  return RCW_OK;
}

/* Parse a %variable assignment. */
static rcw_error_t
parse_variable(rcw_ctx_t *ctx, const char *line) {
  /* Skip '%' */
  line++;

  const char *eq = strchr(line, '=');
  if (!eq)
    return RCW_ERR_PARSE;

  char name[64];
  size_t nlen = (size_t)(eq - line);
  if (nlen >= sizeof(name))
    return RCW_ERR_PARSE;
  memcpy(name, line, nlen);
  name[nlen] = '\0';

  const char *valstr = eq + 1;
  unsigned long val = strtoul(valstr, NULL, 0);

  if (strcmp(name, "size") == 0)
    ctx->vars.size = (int)val;
  else if (strcmp(name, "pbiformat") == 0)
    ctx->vars.pbiformat = (int)val;
  else if (strcmp(name, "classicbitnumbers") == 0)
    ctx->vars.classicbitnumbers = (val != 0);
  else if (strcmp(name, "littleendian") == 0)
    ctx->vars.littleendian = (val != 0);
  else if (strcmp(name, "nocrc") == 0)
    ctx->vars.nocrc = (val != 0);
  else if (strcmp(name, "loadwochecksum") == 0)
    ctx->vars.loadwochecksum = (val != 0);
  else if (strcmp(name, "pbladdr") == 0)
    ctx->vars.pbladdr = (uint32_t)strtoul(valstr, NULL, 16);
  /* Silently ignore unknown variables (like rcw.py) */

  return RCW_OK;
}

/*
 * Strip all whitespace from a string (for non-PBI lines).
 * Returns dynamically allocated string.
 */
static char *
strip_all_whitespace(const char *s, size_t len) {
  char *out = malloc(len + 1);
  if (!out)
    return NULL;

  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    if (!isspace((unsigned char)s[i]))
      out[j++] = s[i];
  }
  out[j] = '\0';

  return out;
}

/*
 * Strip leading and trailing whitespace (for PBI lines).
 * Returns dynamically allocated string.
 */
static char *
strip_trim(const char *s, size_t len) {
  while (len > 0 && isspace((unsigned char)s[0])) {
    s++;
    len--;
  }
  while (len > 0 && isspace((unsigned char)s[len - 1]))
    len--;

  char *out = malloc(len + 1);
  if (!out)
    return NULL;

  memcpy(out, s, len);
  out[len] = '\0';

  return out;
}

/*
 * Append a line (with newline) to a dynamic char buffer.
 * Used to accumulate .uboot subsection bodies.
 */
static rcw_error_t
ubuf_append(char **buf, size_t *len, size_t *cap,
            const char *line, size_t line_len) {
  size_t need = *len + line_len + 1; /* +1 for newline */
  if (need > *cap) {
    size_t newcap = (*cap == 0) ? 4096 : *cap;
    while (newcap < need)
      newcap *= 2;
    char *p = realloc(*buf, newcap);
    if (!p)
      return RCW_ERR_NOMEM;
    *buf = p;
    *cap = newcap;
  }

  memcpy(*buf + *len, line, line_len);
  (*buf)[*len + line_len] = '\n';
  *len += line_len + 1;

  return RCW_OK;
}

/* Subsection types we recognize. */
enum sub_type {
  SUB_NONE = 0,
  SUB_PBI,   /* .pbi   ... .end */
  SUB_UBOOT, /* .uboot ... .end */
};

rcw_error_t
rcw_parse(rcw_ctx_t *ctx, const char *source, size_t len) {
  enum sub_type subsection = SUB_NONE;
  char  *uboot_buf = NULL;
  size_t uboot_len = 0;
  size_t uboot_cap = 0;
  const char *p = source;
  const char *end = source + len;

  while (p < end) {
    /* Find end of current line */
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol)
      eol = end;

    size_t line_len = (size_t)(eol - p);

    if (subsection != SUB_NONE) {
      /* Subsection lines: strip leading/trailing whitespace only */
      char *trimmed = strip_trim(p, line_len);
      if (!trimmed) {
        free(uboot_buf);
        return RCW_ERR_NOMEM;
      }

      /* Check for .end */
      if (trimmed[0] == '.') {
        /* Strip all whitespace to check subsection marker */
        char *stripped = strip_all_whitespace(p, line_len);
        if (!stripped) {
          free(trimmed);
          free(uboot_buf);
          return RCW_ERR_NOMEM;
        }

        if (strcmp(stripped, ".end") == 0) {
          enum sub_type closing = subsection;
          subsection = SUB_NONE;
          free(stripped);
          free(trimmed);

          /* Flush a .uboot block at .end */
          if (closing == SUB_UBOOT) {
            rcw_error_t err = rcw_uboot_encode(ctx,
                uboot_buf, uboot_len);
            free(uboot_buf);
            uboot_buf = NULL;
            uboot_len = 0;
            uboot_cap = 0;
            if (err != RCW_OK)
              return err;
          }

          p = eol + 1;
          continue;
        }

        free(stripped);
      }

      /* Skip empty lines and comment-only lines in subsections */
      if (trimmed[0] != '\0' &&
          !(trimmed[0] == '/' && trimmed[1] == '*')) {
        rcw_error_t err;
        if (subsection == SUB_PBI) {
          err = rcw_pbi_encode_line(ctx, trimmed);
        } else { /* SUB_UBOOT */
          err = ubuf_append(&uboot_buf, &uboot_len, &uboot_cap,
              trimmed, strlen(trimmed));
        }
        if (err != RCW_OK) {
          free(trimmed);
          free(uboot_buf);
          return err;
        }
      }
      free(trimmed);
      p = eol + 1;

      continue;
    }

    /* Non-PBI: strip ALL whitespace */
    char *line = strip_all_whitespace(p, line_len);
    if (!line)
      return RCW_ERR_NOMEM;

    /* Skip blank lines */
    if (line[0] == '\0') {
      free(line);
      p = eol + 1;

      continue;
    }

    /* Subsection marker: .pbi or .uboot */
    if (line[0] == '.') {
      if (strcmp(line + 1, "pbi") == 0)
        subsection = SUB_PBI;
      else if (strcmp(line + 1, "uboot") == 0)
        subsection = SUB_UBOOT;
      /* else: unknown subsection, ignore */
      free(line);
      p = eol + 1;

      continue;
    }

    /* Variable: %var=value */
    if (line[0] == '%') {
      rcw_error_t err = parse_variable(ctx, line);
      free(line);
      if (err != RCW_OK)
        return err;
      p = eol + 1;

      continue;
    }

    /*
     * Try bitfield definition: NAME[begin:end] or NAME[pos]
     * and assignment: NAME=value
     *
     * We look for '[' first (definition), then '=' (assignment).
     */
    char *bracket = strchr(line, '[');
    char *eq = strchr(line, '=');

    if (bracket && (!eq || bracket < eq)) {
      /* Bitfield definition */
      *bracket = '\0';
      const char *name = line;
      char *range = bracket + 1;
      char *endbr = strchr(range, ']');
      if (!endbr) {
        free(line);
        return RCW_ERR_PARSE;
      }
      *endbr = '\0';

      char *colon = strchr(range, ':');
      if (colon) {
        /* NAME[begin:end] */
        *colon = '\0';
        int begin = (int)strtoul(range, NULL, 0);
        int endpos = (int)strtoul(colon + 1, NULL, 0);

        rcw_error_t err = check_overlap(ctx, name, begin, endpos);
        if (err != RCW_OK) {
          free(line);
          /* Warn but continue, like rcw.py */
          if (ctx->warnings)
            fprintf(stderr, "%s\n", ctx->error_detail);
          p = eol + 1;

          continue;
        }

        if (ctx->symbols.count >= RCW_MAX_SYMBOLS) {
          free(line);

          return RCW_ERR_NOMEM;
        }

        rcw_symbol_t *sym = &ctx->symbols.entries[ctx->symbols.count++];
        snprintf(sym->name, RCW_FIELD_NAME_MAX, "%s", name);
        sym->begin = begin;
        sym->end = endpos;
        sym->has_value = false;

      } else {
        /* NAME[pos] (single bit) */
        int pos = (int)strtoul(range, NULL, 0);

        rcw_error_t err = check_overlap(ctx, name, pos, pos);
        if (err != RCW_OK) {
          free(line);
          if (ctx->warnings)
            fprintf(stderr, "%s\n", ctx->error_detail);
          p = eol + 1;

          continue;
        }

        if (ctx->symbols.count >= RCW_MAX_SYMBOLS) {
          free(line);
          return RCW_ERR_NOMEM;
        }

        rcw_symbol_t *sym = &ctx->symbols.entries[ctx->symbols.count++];
        snprintf(sym->name, RCW_FIELD_NAME_MAX, "%s", name);
        sym->begin = pos;
        sym->end = pos;
        sym->has_value = false;
      }
      free(line);
      p = eol + 1;

      continue;
    }

    if (eq) {
      /* Assignment: NAME=value */
      *eq = '\0';
      const char *name = line;
      const char *valstr = eq + 1;

      int idx = symtab_find(&ctx->symbols, name);
      if (idx < 0) {
        /* rcw.py prints error but continues */
        fprintf(stderr, "Error: Unknown bitfield %s\n", name);
        rcw_set_error(ctx, "Unknown bitfield: %s", name);
        free(line);
        p = eol + 1;

        continue;
      }

      if (ctx->warnings && ctx->symbols.entries[idx].has_value)
        fprintf(stderr, "Warning: Duplicate assignment for " "bitfield %s\n", name);

      ctx->symbols.entries[idx].has_value = true;
      ctx->symbols.entries[idx].value = strtoull(valstr, NULL, 0);

      free(line);
      p = eol + 1;
      continue;
    }

    /* Unknown line - print error like rcw.py but continue */
    fprintf(stderr, "Error: unknown command %s\n", line);
    free(line);
    p = eol + 1;
  }

  /*
   * Source ended without closing a subsection.  For .uboot, flush
   * what we have anyway (matches rcw.py: build_pbi_uboot is called
   * once at .end; if the file is truncated, the remainder is lost).
   * For .pbi we have nothing to flush since lines are encoded as
   * they arrive.
   */
  if (subsection == SUB_UBOOT && uboot_buf) {
    rcw_error_t err = rcw_uboot_encode(ctx, uboot_buf, uboot_len);
    free(uboot_buf);
    if (err != RCW_OK)
      return err;
  } else {
    free(uboot_buf);
  }

  return RCW_OK;
}

/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Cell inventory v1: strict parser + poll-based file watcher. See nr_cell_inventory.h and docs/superpowers/specs/mpci-cell-inventory-v1.md.
 * No JSON library exists in the tree, so this is a small recursive-descent parser: whole-document validation (RFC 8259 grammar, depth <= 8,
 * no trailing data), known keys type-checked, unknown keys skipped. */
#include "nr_cell_inventory.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DEPTH 8

typedef struct {
  const char *p, *end;
  char *err;
  size_t errn;
  int failed, toolong;
} ps_t;

static int fail(ps_t *s, const char *what)
{
  if (!s->failed && s->err && s->errn)
    snprintf(s->err, s->errn, "%s", what);
  s->failed = 1;
  return -1;
}
static void ws(ps_t *s)
{
  while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r'))
    s->p++;
}
static int peek(ps_t *s)
{
  ws(s);
  return s->p < s->end ? (unsigned char)*s->p : -1;
}
static int expect(ps_t *s, char c)
{
  if (peek(s) != c)
    return fail(s, "syntax error (unexpected character or truncated document)");
  s->p++;
  return 0;
}
/* string -> buf (n bytes, NUL terminated); too long -> failure when `must_fit`, else truncated silently (skip mode passes buf = NULL) */
static int parse_string(ps_t *s, char *buf, size_t n)
{
  if (expect(s, '"'))
    return -1;
  size_t o = 0;
  for (;;) {
    if (s->p >= s->end)
      return fail(s, "unterminated string");
    unsigned char c = (unsigned char)*s->p++;
    if (c == '"')
      break;
    if (c < 0x20)
      return fail(s, "control character in string");
    int ch = c;
    if (c == '\\') {
      if (s->p >= s->end)
        return fail(s, "unterminated string");
      char e = *s->p++;
      switch (e) {
        case '"': case '\\': case '/': ch = e; break;
        case 'b': ch = '\b'; break;
        case 'f': ch = '\f'; break;
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'u':
          if (s->end - s->p < 4)
            return fail(s, "bad \\u escape");
          for (int i = 0; i < 4; i++)
            if (!((s->p[i] >= '0' && s->p[i] <= '9') || (s->p[i] >= 'a' && s->p[i] <= 'f') || (s->p[i] >= 'A' && s->p[i] <= 'F')))
              return fail(s, "bad \\u escape");
          s->p += 4;
          ch = '?';
          break;
        default: return fail(s, "bad escape");
      }
    }
    if (buf) {
      if (o + 1 >= n) {
        s->toolong = 1;
        return fail(s, "string too long");
      }
      buf[o++] = (char)ch;
    }
  }
  if (buf)
    buf[o] = 0;
  return 0;
}
/* object key: a key longer than the buffer cannot be a known key, so it is skipped (key = "") instead of rejecting the file */
static int parse_key(ps_t *s, char *buf, size_t n)
{
  const char *save = s->p;
  if (parse_string(s, buf, n) == 0)
    return 0;
  if (s->failed && s->toolong) {
    s->failed = s->toolong = 0;
    if (s->err && s->errn)
      s->err[0] = 0;
    s->p = save;
    buf[0] = 0;
    return parse_string(s, NULL, 0);
  }
  return -1;
}
static int parse_number(ps_t *s, double *v)
{
  ws(s);
  const char *q = s->p;
  if (q < s->end && *q == '-')
    q++;
  if (q >= s->end || *q < '0' || *q > '9')
    return fail(s, "bad number");
  if (*q == '0')
    q++;
  else
    while (q < s->end && *q >= '0' && *q <= '9')
      q++;
  if (q < s->end && *q == '.') {
    q++;
    if (q >= s->end || *q < '0' || *q > '9')
      return fail(s, "bad number");
    while (q < s->end && *q >= '0' && *q <= '9')
      q++;
  }
  if (q < s->end && (*q == 'e' || *q == 'E')) {
    q++;
    if (q < s->end && (*q == '+' || *q == '-'))
      q++;
    if (q >= s->end || *q < '0' || *q > '9')
      return fail(s, "bad number");
    while (q < s->end && *q >= '0' && *q <= '9')
      q++;
  }
  char tmp[64];
  size_t l = (size_t)(q - s->p);
  if (l >= sizeof tmp)
    return fail(s, "number too long");
  memcpy(tmp, s->p, l);
  tmp[l] = 0;
  double d = strtod(tmp, NULL);
  if (!isfinite(d))
    return fail(s, "number out of range");
  s->p = q;
  if (v)
    *v = d;
  return 0;
}
static int literal(ps_t *s, const char *w)
{
  size_t l = strlen(w);
  if ((size_t)(s->end - s->p) < l || memcmp(s->p, w, l) != 0)
    return fail(s, "bad literal");
  s->p += l;
  return 0;
}
static int skip_value(ps_t *s, int depth)
{
  if (depth > MAX_DEPTH)
    return fail(s, "nesting too deep");
  int c = peek(s);
  if (c == '"')
    return parse_string(s, NULL, 0);
  if (c == '{') {
    s->p++;
    if (peek(s) == '}') {
      s->p++;
      return 0;
    }
    for (;;) {
      if (parse_string(s, NULL, 0) || expect(s, ':') || skip_value(s, depth + 1))
        return -1;
      c = peek(s);
      if (c == ',') {
        s->p++;
        continue;
      }
      return expect(s, '}');
    }
  }
  if (c == '[') {
    s->p++;
    if (peek(s) == ']') {
      s->p++;
      return 0;
    }
    for (;;) {
      if (skip_value(s, depth + 1))
        return -1;
      c = peek(s);
      if (c == ',') {
        s->p++;
        continue;
      }
      return expect(s, ']');
    }
  }
  if (c == 't')
    return literal(s, "true");
  if (c == 'f')
    return literal(s, "false");
  if (c == 'n')
    return literal(s, "null");
  return parse_number(s, NULL);
}

/* field presence bits */
enum { F_ID = 1, F_CENTRE = 2, F_GSCN = 4, F_SSBHZ = 8, F_SSBIDX = 16, F_PCI = 32, F_CFO = 64, F_NRB = 128, F_SCS = 256, F_SSS = 512, F_DMRS = 1024,
       F_SEEN = 2048, F_STATE = 4096, F_ALL = 8191 };
enum { W_CENTRE = 1, W_RATE = 2, W_NRX = 4, W_ALL = 7 };

static int integral(ps_t *s, double d, double lo, double hi, const char *what)
{
  if (d != floor(d) || d < lo || d > hi)
    return fail(s, what);
  return 0;
}
static int parse_cell(ps_t *s, nr_inv_cell_t *c)
{
  nr_cell_desc_init(&c->desc);
  c->id[0] = 0;
  c->sss_gap = c->dmrs_ratio = 0;
  c->first_seen_ns = 0;
  unsigned seen = 0;
  if (expect(s, '{'))
    return -1;
  if (peek(s) == '}')
    return fail(s, "empty cell object");
  for (;;) {
    char key[40];
    double d;
    if (parse_key(s, key, sizeof key) || expect(s, ':'))
      return -1;
#define NUM(flag, dst, lo, hi, what, isint) \
  do { \
    if (parse_number(s, &d)) return -1; \
    if ((isint) && integral(s, d, lo, hi, what)) return -1; \
    if (!(isint) && (d < (lo) || d > (hi))) return fail(s, what); \
    dst; seen |= flag; \
  } while (0)
    if (!strcmp(key, "id")) {
      if (peek(s) != '"')
        return fail(s, "cell.id must be a string");
      if (parse_string(s, c->id, sizeof c->id))
        return -1;
      if (!c->id[0])
        return fail(s, "cell.id is empty");
      seen |= F_ID;
    } else if (!strcmp(key, "centre_hz")) NUM(F_CENTRE, c->desc.centre_hz = d, 1e6, 1e11, "cell.centre_hz out of range", 0);
    else if (!strcmp(key, "gscn")) NUM(F_GSCN, c->desc.gscn = (int)d, 0, 100000, "cell.gscn must be an integer 0..100000", 1);
    else if (!strcmp(key, "ssb_hz")) NUM(F_SSBHZ, c->desc.ssb_hz = d, 0, 1e11, "cell.ssb_hz out of range", 0);
    else if (!strcmp(key, "ssb_index")) NUM(F_SSBIDX, c->desc.ssb_index = (int)d, -1, 63, "cell.ssb_index must be an integer -1..63", 1);
    else if (!strcmp(key, "pci")) NUM(F_PCI, c->desc.pci = (int)d, -1, 1007, "cell.pci must be an integer -1..1007", 1);
    else if (!strcmp(key, "cfo_hz")) NUM(F_CFO, c->desc.cfo_hz = d, -1e7, 1e7, "cell.cfo_hz out of range", 0);
    else if (!strcmp(key, "n_rb_hint")) {
      if (peek(s) == 'n') {
        if (literal(s, "null"))
          return -1;
        c->desc.n_rb = 0;
        seen |= F_NRB;
      } else
        NUM(F_NRB, c->desc.n_rb = (int)d, 1, 275, "cell.n_rb_hint must be null or an integer 1..275", 1);
    } else if (!strcmp(key, "scs_khz")) {
      NUM(F_SCS, c->desc.scs_khz = (int)d, 0, 240, "cell.scs_khz must be 0, 15, 30, 60 or 120", 1);
      int v = c->desc.scs_khz;
      if (v != 0 && v != 15 && v != 30 && v != 60 && v != 120)
        return fail(s, "cell.scs_khz must be 0, 15, 30, 60 or 120");
    } else if (!strcmp(key, "sss_gap")) NUM(F_SSS, c->sss_gap = d, -1e6, 1e6, "cell.sss_gap out of range", 0);
    else if (!strcmp(key, "dmrs_ratio")) NUM(F_DMRS, c->dmrs_ratio = d, -1e6, 1e6, "cell.dmrs_ratio out of range", 0);
    else if (!strcmp(key, "first_seen_ns")) NUM(F_SEEN, c->first_seen_ns = (int64_t)d, 0, 9.0e18, "cell.first_seen_ns must be a non-negative integer", 1);
    else if (!strcmp(key, "state")) {
      char st[16];
      if (peek(s) != '"')
        return fail(s, "cell.state must be a string");
      if (parse_string(s, st, sizeof st))
        return -1;
      int v = nr_cell_inventory_state_from_name(st);
      if (v < 0)
        return fail(s, "cell.state must be new, acquire, maintain or lost");
      c->desc.state = v;
      seen |= F_STATE;
    } else if (skip_value(s, 2))
      return -1;
#undef NUM
    int ch = peek(s);
    if (ch == ',') {
      s->p++;
      continue;
    }
    if (expect(s, '}'))
      return -1;
    break;
  }
  if (seen != F_ALL)
    return fail(s, "cell is missing a required key (id centre_hz gscn ssb_hz ssb_index pci cfo_hz n_rb_hint scs_khz sss_gap dmrs_ratio first_seen_ns state)");
  return 0;
}
static int parse_window(ps_t *s, nr_cell_inventory_t *o)
{
  unsigned seen = 0;
  if (expect(s, '{'))
    return -1;
  if (peek(s) == '}')
    return fail(s, "empty window object");
  for (;;) {
    char key[32];
    double d;
    if (parse_key(s, key, sizeof key) || expect(s, ':'))
      return -1;
    if (!strcmp(key, "centre_hz")) {
      if (parse_number(s, &d)) return -1;
      if (d < 1e6 || d > 1e11) return fail(s, "window.centre_hz out of range");
      o->window_centre_hz = d; seen |= W_CENTRE;
    } else if (!strcmp(key, "rate_msps")) {
      if (parse_number(s, &d)) return -1;
      if (d <= 0 || d > 1e5) return fail(s, "window.rate_msps out of range");
      o->window_rate_msps = d; seen |= W_RATE;
    } else if (!strcmp(key, "n_rx")) {
      if (parse_number(s, &d)) return -1;
      if (integral(s, d, 1, 64, "window.n_rx must be an integer 1..64")) return -1;
      o->window_n_rx = (int)d; seen |= W_NRX;
    } else if (skip_value(s, 2))
      return -1;
    int ch = peek(s);
    if (ch == ',') {
      s->p++;
      continue;
    }
    if (expect(s, '}'))
      return -1;
    break;
  }
  if (seen != W_ALL)
    return fail(s, "window is missing a required key (centre_hz rate_msps n_rx)");
  return 0;
}

int nr_cell_inventory_parse(const char *text, size_t len, nr_cell_inventory_t *out, char *err, size_t errn)
{
  if (err && errn)
    err[0] = 0;
  if (!text || !out)
    return -1;
  /* parse into a scratch copy so that a rejected file never touches *out */
  nr_cell_inventory_t *t = (nr_cell_inventory_t *)calloc(1, sizeof *t);
  if (!t) {
    if (err && errn)
      snprintf(err, errn, "out of memory");
    return -1;
  }
  ps_t sv = {.p = text, .end = text + len, .err = err, .errn = errn}, *s = &sv;
  int have_schema = 0, have_window = 0, have_cells = 0;
  if (expect(s, '{'))
    goto bad;
  if (peek(s) == '}') {
    fail(s, "empty document");
    goto bad;
  }
  for (;;) {
    char key[32];
    if (parse_key(s, key, sizeof key) || expect(s, ':'))
      goto bad;
    if (!strcmp(key, "schema")) {
      double d;
      if (parse_number(s, &d))
        goto bad;
      if (d != 1) {
        fail(s, "unsupported schema (this reader handles schema 1 only)");
        goto bad;
      }
      have_schema = 1;
    } else if (!strcmp(key, "window")) {
      if (parse_window(s, t))
        goto bad;
      have_window = 1;
    } else if (!strcmp(key, "cells")) {
      if (expect(s, '['))
        goto bad;
      if (peek(s) == ']')
        s->p++;
      else
        for (;;) {
          if (t->n >= NR_ADMIT_PLAN_MAX) {
            fail(s, "too many cells");
            goto bad;
          }
          if (parse_cell(s, &t->cell[t->n]))
            goto bad;
          for (int i = 0; i < t->n; i++)
            if (!strcmp(t->cell[i].id, t->cell[t->n].id)) {
              fail(s, "duplicate cell id");
              goto bad;
            }
          t->n++;
          int ch = peek(s);
          if (ch == ',') {
            s->p++;
            continue;
          }
          if (expect(s, ']'))
            goto bad;
          break;
        }
      have_cells = 1;
    } else if (skip_value(s, 1))
      goto bad;
    int ch = peek(s);
    if (ch == ',') {
      s->p++;
      continue;
    }
    if (expect(s, '}'))
      goto bad;
    break;
  }
  ws(s);
  if (s->p != s->end) {
    fail(s, "trailing data after the document");
    goto bad;
  }
  if (!have_schema || !have_window || !have_cells) {
    fail(s, "missing top-level key (schema window cells)");
    goto bad;
  }
  for (int i = 0; i < t->n; i++) {
    nr_cell_desc_t *d = &t->cell[i].desc;
    d->n_rx = t->window_n_rx;
    d->window_centre_hz = t->window_centre_hz;
    d->window = 0;
    d->weight = 1;
  }
  *out = *t;
  free(t);
  return 0;
bad:
  free(t);
  return -1;
}

int nr_cell_inventory_state_from_name(const char *s)
{
  if (!s) return -1;
  if (!strcmp(s, "new")) return NR_CELLD_NEW;
  if (!strcmp(s, "acquire")) return NR_CELLD_ACQUIRE;
  if (!strcmp(s, "maintain")) return NR_CELLD_MAINTAIN;
  if (!strcmp(s, "lost")) return NR_CELLD_LOST;
  return -1;
}
const char *nr_cell_inventory_state_name(int st)
{
  switch (st) {
    case NR_CELLD_NEW: return "new";
    case NR_CELLD_ACQUIRE: return "acquire";
    case NR_CELLD_MAINTAIN: return "maintain";
    case NR_CELLD_LOST: return "lost";
  }
  return "?";
}

int nr_cell_inventory_default_path(const char *run_dir, char *out, size_t n)
{
  if (!run_dir || !out || !n)
    return -1;
  size_t l = strlen(run_dir);
  while (l > 1 && run_dir[l - 1] == '/')
    l--;
  int r = snprintf(out, n, "%.*s/cells.json", (int)l, run_dir);
  return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

int nr_cell_inventory_watch_init(nr_cell_inventory_watch_t *w, const char *path)
{
  memset(w, 0, sizeof *w);
  if (!path || strlen(path) >= sizeof w->path)
    return -1;
  strcpy(w->path, path);
  return 0;
}

static void set_err(char *err, size_t errn, const char *a, const char *b)
{
  if (err && errn)
    snprintf(err, errn, "%s%s%s", a, b ? ": " : "", b ? b : "");
}

int nr_cell_inventory_poll(nr_cell_inventory_watch_t *w, char *err, size_t errn)
{
  if (err && errn)
    err[0] = 0;
  int fd = open(w->path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0; /* not there (yet): nothing to do, previous inventory (if any) stays */
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return 0;
  }
  int64_t ms = (int64_t)st.st_mtim.tv_sec, mn = (int64_t)st.st_mtim.tv_nsec, sz = (int64_t)st.st_size, ino = (int64_t)st.st_ino;
  if (w->have_stat && ms == w->mtime_sec && mn == w->mtime_nsec && sz == w->size && ino == w->ino) {
    close(fd);
    return 0;
  }
  w->have_stat = 1;
  w->mtime_sec = ms; w->mtime_nsec = mn; w->size = sz; w->ino = ino;
  if (sz <= 0 || sz > NR_INV_FILE_MAX) {
    close(fd);
    set_err(err, errn, sz <= 0 ? "empty file" : "file too large", NULL);
    return -1;
  }
  char *buf = (char *)malloc((size_t)sz);
  if (!buf) {
    close(fd);
    set_err(err, errn, "out of memory", NULL);
    return -1;
  }
  size_t got = 0;
  while (got < (size_t)sz) {
    ssize_t r = read(fd, buf + got, (size_t)sz - got);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      break;
    got += (size_t)r;
  }
  close(fd);
  if (got != (size_t)sz) {
    free(buf);
    set_err(err, errn, "short read", NULL);
    return -1;
  }
  char e[160] = "";
  nr_cell_inventory_t *tmp = (nr_cell_inventory_t *)malloc(sizeof *tmp);
  int rc = tmp ? nr_cell_inventory_parse(buf, got, tmp, e, sizeof e) : -1;
  free(buf);
  if (rc != 0) {
    free(tmp);
    set_err(err, errn, "rejected, previous inventory kept", tmp == NULL && e[0] == 0 ? "out of memory" : e);
    return -1;
  }
  w->inv = *tmp;
  free(tmp);
  w->have_inv = 1;
  w->generation++;
  return 1;
}

/*
 * a2600srcmap -- see a2600srcmap.h.
 *
 * Reads dasm listings of batari Basic programs. Rules (checked against the
 * real listings in core/tests/data/bbasic):
 *
 *  - A row is "<line> <hex offset> [bytes] [-] <source>". A '-' flag token
 *    before the source means dasm skipped the row: ignored. Offsets that are
 *    not 4-5 hex digits ("U0000", symbol values) are not code rows.
 *  - The first assembled ORG decides the cart: below $F000 (e.g. $1000) the
 *    cart is banked, bank = (offset >> 12) - 1; otherwise bank 0. The run-time
 *    address is always offset & 0xFFF.
 *  - A marker row (";;line N;;", or ".Lnn ; text" in old listings) starts a
 *    source line's code. Its extent ends at the next marker in the same bank.
 *    A marker not followed by one ends at the first assembled ORG/RORG or
 *    "bB.asm file is split here" row, but no later than the first assembler
 *    directive row (if/echo/include/...) after it: that is where bB's own
 *    kernel and runtime begin, and they must not be attributed to the last
 *    user line.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "a2600srcmap.h"

#define MAX_FILES  4000
#define MAX_DEPTH  8
#define PATH_BUF   4096
#define NBANKS     16
#define BANKSZ     4096
#define NSLOTS     (NBANKS * BANKSZ)
#define NORM_MAX   200

typedef struct {
  char* key;       /* canonical path, '/' separators */
  char* name;      /* what the UI shows */
  char* text;
  char** lines;
  int nlines;
  int32_t* first;  /* [0..first_n): (bank << 16) | low 12 bits, or -1 */
  int first_n;
} sfile;

struct a2600srcmap
{
  pthread_mutex_t lock;
  sfile* files;
  int nfiles, cap;
  char root[PATH_BUF];
  unsigned gen;
  uint16_t* file_of;   /* [NSLOTS] file index + 1, 0 = not BASIC */
  uint16_t* line_of;
  uint8_t* start;
};

/* ---- helpers ------------------------------------------------------------ */

static char* dupstr(const char* s)
{
  size_t n = strlen(s) + 1;
  char* p = (char*)malloc(n);
  if(p) memcpy(p, s, n);
  return p;
}

static void norm_slashes(char* s)
{
  for(; *s; s++)
    if(*s == '\\') *s = '/';
}

static int ci_eq_n(const char* a, const char* b, size_t n)
{
  while(n--)
  {
    if(tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    if(!*a) return 1;
    a++; b++;
  }
  return 1;
}

static int has_ext(const char* name, const char* ext)
{
  size_t n = strlen(name), e = strlen(ext);
  return n >= e && ci_eq_n(name + n - e, ext, e);
}

static const char* base_of(const char* path)
{
  const char* s = strrchr(path, '/');
  const char* b = strrchr(path, '\\');
  if(b > s) s = b;
  return s ? s + 1 : path;
}

static void dir_of(const char* path, char* dst, size_t n)
{
  const char* b = base_of(path);
  size_t len = (size_t)(b - path);
  if(len > 0) len--;
  if(len >= n) len = n - 1;
  memcpy(dst, path, len);
  dst[len] = '\0';
}

static void canon_key(const char* p, char* out, size_t n)
{
  char tmp[PATH_BUF];
  size_t len;
#ifdef _WIN32
  if(!_fullpath(tmp, p, sizeof(tmp))) snprintf(tmp, sizeof(tmp), "%s", p);
#else
  if(!realpath(p, tmp)) snprintf(tmp, sizeof(tmp), "%s", p);
#endif
  norm_slashes(tmp);
  len = strlen(tmp);
  while(len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
  snprintf(out, n, "%s", tmp);
}

static int is_dir(const char* p)
{
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char* p)
{
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int read_lines(const char* path, char** text, char*** lines, int* n)
{
  FILE* f = fopen(path, "rb");
  long size;
  char *buf, **ls, *p;
  int count = 0, i;
  size_t got;

  if(!f) return 0;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if(size < 0 || size > 64L * 1024 * 1024) { fclose(f); return 0; }
  buf = (char*)malloc((size_t)size + 1);
  if(!buf) { fclose(f); return 0; }
  got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  buf[got] = '\0';

  for(p = buf; *p; p++)
    if(*p == '\n') count++;
  if(got > 0 && buf[got - 1] != '\n') count++;
  ls = (char**)malloc((size_t)(count ? count : 1) * sizeof(char*));
  if(!ls) { free(buf); return 0; }
  p = buf;
  for(i = 0; i < count; i++)
  {
    char* e = strchr(p, '\n');
    ls[i] = p;
    if(e)
    {
      *e = '\0';
      if(e > p && e[-1] == '\r') e[-1] = '\0';
      p = e + 1;
    }
    else
    {
      size_t l = strlen(p);
      if(l && p[l - 1] == '\r') p[l - 1] = '\0';
    }
  }
  *text = buf; *lines = ls; *n = count;
  return 1;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void free_files(a2600srcmap* m)
{
  int i;
  for(i = 0; i < m->nfiles; i++)
  {
    free(m->files[i].key);
    free(m->files[i].name);
    free(m->files[i].text);
    free(m->files[i].lines);
    free(m->files[i].first);
  }
  free(m->files);
  m->files = NULL;
  m->nfiles = m->cap = 0;
}

static void reset_tables(a2600srcmap* m)
{
  memset(m->file_of, 0, NSLOTS * sizeof(uint16_t));
  memset(m->line_of, 0, NSLOTS * sizeof(uint16_t));
  memset(m->start, 0, NSLOTS);
}

a2600srcmap* a2600srcmap_create(void)
{
  a2600srcmap* m = (a2600srcmap*)calloc(1, sizeof(*m));
  if(!m) return NULL;
  m->file_of = (uint16_t*)calloc(NSLOTS, sizeof(uint16_t));
  m->line_of = (uint16_t*)calloc(NSLOTS, sizeof(uint16_t));
  m->start = (uint8_t*)calloc(NSLOTS, 1);
  if(!m->file_of || !m->line_of || !m->start)
  {
    free(m->file_of); free(m->line_of); free(m->start); free(m);
    return NULL;
  }
  pthread_mutex_init(&m->lock, NULL);
  return m;
}

void a2600srcmap_destroy(a2600srcmap* m)
{
  if(!m) return;
  free_files(m);
  free(m->file_of); free(m->line_of); free(m->start);
  pthread_mutex_destroy(&m->lock);
  free(m);
}

a2600srcmap* a2600srcmap_shared(void)
{
  static a2600srcmap* shared;
  static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&once);
  if(!shared) shared = a2600srcmap_create();
  pthread_mutex_unlock(&once);
  return shared;
}

void a2600srcmap_clear(a2600srcmap* m)
{
  if(!m) return;
  pthread_mutex_lock(&m->lock);
  free_files(m);
  reset_tables(m);
  m->root[0] = '\0';
  m->gen++;
  pthread_mutex_unlock(&m->lock);
}

unsigned a2600srcmap_generation(const a2600srcmap* m)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  unsigned g;
  pthread_mutex_lock(&mm->lock);
  g = mm->gen;
  pthread_mutex_unlock(&mm->lock);
  return g;
}

/* ---- files (callers hold the lock or own the map) ------------------------ */

static int push_file(a2600srcmap* m, const char* key, const char* name)
{
  sfile* f;
  if(m->nfiles >= MAX_FILES) return -1;
  if(m->nfiles == m->cap)
  {
    int ncap = m->cap ? m->cap * 2 : 16;
    sfile* nf = (sfile*)realloc(m->files, (size_t)ncap * sizeof(sfile));
    if(!nf) return -1;
    m->files = nf;
    m->cap = ncap;
  }
  f = &m->files[m->nfiles];
  memset(f, 0, sizeof(*f));
  f->key = dupstr(key);
  f->name = dupstr(name);
  if(!f->key || !f->name) { free(f->key); free(f->name); return -1; }
  return m->nfiles++;
}

static int find_key(const a2600srcmap* m, const char* key)
{
  int i;
  for(i = 0; i < m->nfiles; i++)
    if(strcmp(m->files[i].key, key) == 0) return i;
  return -1;
}

static void display_name(const a2600srcmap* m, const char* key, char* out, size_t n)
{
  size_t rl = strlen(m->root);
  if(rl && strncmp(key, m->root, rl) == 0 && key[rl] == '/')
    snprintf(out, n, "%s", key + rl + 1);
  else
    snprintf(out, n, "%s", base_of(key));
}

static int add_file(a2600srcmap* m, const char* path)
{
  char key[PATH_BUF], name[PATH_BUF];
  char *text, **lines;
  int n, idx;

  canon_key(path, key, sizeof(key));
  idx = find_key(m, key);
  if(idx >= 0 && m->files[idx].text) return idx;
  if(!read_lines(key, &text, &lines, &n)) return -1;
  if(idx < 0)
  {
    display_name(m, key, name, sizeof(name));
    idx = push_file(m, key, name);
    if(idx < 0) { free(text); free(lines); return -1; }
  }
  m->files[idx].text = text;
  m->files[idx].lines = lines;
  m->files[idx].nlines = n;
  return idx;
}

static int cmp_names(const void* a, const void* b)
{
  return strcmp(*(char* const*)a, *(char* const*)b);
}

static void walk(a2600srcmap* m, const char* dir, int depth)
{
  DIR* d = opendir(dir);
  struct dirent* e;
  char** names = NULL;
  int n = 0, cap = 0, i;

  if(!d) return;
  while((e = readdir(d)) != NULL)
  {
    if(e->d_name[0] == '.') continue;
    if(n == cap)
    {
      char** nn = (char**)realloc(names, (size_t)(cap ? cap * 2 : 32) * sizeof(char*));
      if(!nn) break;
      names = nn;
      cap = cap ? cap * 2 : 32;
    }
    names[n] = dupstr(e->d_name);
    if(names[n]) n++;
  }
  closedir(d);
  if(n) qsort(names, (size_t)n, sizeof(char*), cmp_names);
  for(i = 0; i < n; i++)
  {
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    if(is_dir(path))
    {
      if(depth < MAX_DEPTH) walk(m, path, depth + 1);
    }
    else if(has_ext(names[i], ".bas"))
      add_file(m, path);
    free(names[i]);
  }
  free(names);
}

/* ---- text normalisation (old-format matching, scoring) ------------------- */

static int is_ws(char c) { return c == ' ' || c == '\t'; }

/* Whitespace removed; a trailing ';' or ' rem ...' comment dropped, except a
 * line that is itself a rem (kept as text). */
static void norm_text(const char* s, char* out, size_t n)
{
  size_t o = 0;
  const char* p = s;
  int prev_ws = 1;

  while(is_ws(*p)) p++;
  if(ci_eq_n(p, "rem", 3) && (is_ws(p[3]) || !p[3]))
  {
    for(; *p && o + 1 < n; p++)
      if(!is_ws(*p)) out[o++] = *p;
    out[o] = '\0';
    return;
  }
  for(; *p && o + 1 < n; p++)
  {
    if(*p == ';') break;
    if(prev_ws && ci_eq_n(p, "rem", 3) && (is_ws(p[3]) || !p[3]) && o > 0) break;
    prev_ws = is_ws(*p);
    if(!prev_ws) out[o++] = *p;
  }
  out[o] = '\0';
}

/* ---- listing parser ------------------------------------------------------- */

typedef struct {
  int line;            /* source line (0: not resolved yet) */
  int file;            /* file index, -1 none */
  int bank;
  unsigned start, end; /* run-time (offset & 0xFFF), [start, end) */
  int dir_set;
  unsigned dir_addr;
  int srcline;         /* new format: the N of ;;line N;; */
  char ntext[NORM_MAX];
} mark;

typedef struct {
  mark* marks;
  int n, cap;
  int old_format;
} parsed;

static void parsed_free(parsed* p) { free(p->marks); memset(p, 0, sizeof(*p)); }

typedef struct {
  int valid;
  unsigned off;
  int skipped;
  const char* tok;   /* first token after the address column bytes */
  const char* rest;  /* the whole remainder after the address */
} row;

static int hexval(char c)
{
  return isxdigit((unsigned char)c) != 0;
}

static void parse_row(const char* l, row* r)
{
  const char* p = l;
  const char* q;
  size_t len;

  memset(r, 0, sizeof(*r));
  while(*p == ' ') p++;
  if(!isdigit((unsigned char)*p)) return;
  while(isdigit((unsigned char)*p)) p++;
  if(!is_ws(*p)) return;
  while(is_ws(*p)) p++;
  q = p;
  while(hexval(*q)) q++;
  len = (size_t)(q - p);
  if(len < 4 || len > 5 || !(is_ws(*q) || !*q)) return;
  r->off = (unsigned)strtoul(p, NULL, 16);
  r->rest = q;
  p = q;
  for(;;)
  {
    while(is_ws(*p)) p++;
    if(!*p) { r->tok = p; break; }
    q = p;
    while(*q && !is_ws(*q)) q++;
    len = (size_t)(q - p);
    if(len == 1 && *p == '-') { r->skipped = 1; r->tok = p; break; }
    if((len == 4 && !memcmp(p, "????", 4)) ||
       (len == 2 && hexval(p[0]) && hexval(p[1])))
    {
      p = q;
      continue;
    }
    r->tok = p;
    break;
  }
  r->valid = 1;
}

static int tok_is(const char* t, const char* word)
{
  size_t n = strlen(word);
  return ci_eq_n(t, word, n) && (is_ws(t[n]) || !t[n]);
}

static int is_directive(const char* t)
{
  static const char* const words[] = { "if", "ifconst", "ifnconst", "echo", "include",
    "seg", "align", "endif", "else", "eif", "end", NULL };
  int i;
  for(i = 0; words[i]; i++)
    if(tok_is(t, words[i])) return 1;
  return 0;
}

static int push_mark(parsed* ps, const mark* mk)
{
  if(ps->n == ps->cap)
  {
    int nc = ps->cap ? ps->cap * 2 : 128;
    mark* nm = (mark*)realloc(ps->marks, (size_t)nc * sizeof(mark));
    if(!nm) return 0;
    ps->marks = nm;
    ps->cap = nc;
  }
  ps->marks[ps->n++] = *mk;
  return 1;
}

/* Old-format marker: first token ".L<digits>" followed by ';' and text. */
static const char* old_marker_text(const char* tok)
{
  const char* p = tok;
  if(p[0] != '.' || (p[1] != 'L' && p[1] != 'l') || !isdigit((unsigned char)p[2])) return NULL;
  p += 2;
  while(isdigit((unsigned char)*p)) p++;
  while(is_ws(*p)) p++;
  if(*p != ';' || p[1] == ';') return NULL;
  return p + 1;
}

/* Read a listing; returns 0 on an unreadable file. */
static int parse_listing(const char* path, parsed* ps)
{
  char *text, **lines;
  int n, i, cur = -1, org_seen = 0, multibank = 0, any_new = 0;
  unsigned last_off = 0;
  int have_last = 0, last_bank = 0;
  mark mk;

  memset(ps, 0, sizeof(*ps));
  if(!read_lines(path, &text, &lines, &n)) return 0;

  for(i = 0; i < n; i++)
  {
    const char* l = lines[i];
    row r;
    int bank;
    unsigned a12;
    const char* mt;

    parse_row(l, &r);
    if(!r.valid || r.skipped) continue;
    bank = multibank ? (int)(r.off >> 12) - 1 : 0;
    if(bank < 0) bank = 0;
    if(bank >= NBANKS) bank = NBANKS - 1;
    a12 = r.off & 0xFFF;

    /* boundaries: assembled ORG / RORG, the bB file split */
    if(tok_is(r.tok, "org") || tok_is(r.tok, "rorg") || strstr(l, "file is split here"))
    {
      if(tok_is(r.tok, "org") && !org_seen)
      {
        org_seen = 1;
        multibank = r.off < 0xF000;
        bank = multibank ? (int)(r.off >> 12) - 1 : 0;
        if(bank < 0) bank = 0;
        if(bank >= NBANKS) bank = NBANKS - 1;
      }
      if(cur >= 0)
      {
        mark* c = &ps->marks[cur];
        c->end = c->dir_set ? c->dir_addr : (bank == c->bank && a12 >= c->start ? a12 : c->start);
        cur = -1;
      }
      last_off = r.off; have_last = 1; last_bank = bank;
      continue;
    }

    memset(&mk, 0, sizeof(mk));
    mk.file = -1;
    mk.bank = bank;
    mk.start = a12;

    if((mt = strstr(r.tok, ";;line ")) != NULL)
    {
      char* e;
      long ln = strtol(mt + 7, &e, 10);
      if(e != mt + 7 && e[0] == ';' && e[1] == ';')
      {
        mk.srcline = (int)ln;
        mk.line = (int)ln;
        norm_text(e + 2, mk.ntext, sizeof(mk.ntext));
        any_new = 1;
        goto new_marker;
      }
    }
    if((mt = old_marker_text(r.tok)) != NULL)
    {
      norm_text(mt, mk.ntext, sizeof(mk.ntext));
      goto new_marker;
    }

    /* an ordinary row */
    if(cur >= 0)
    {
      mark* c = &ps->marks[cur];
      if(!c->dir_set && bank == c->bank && a12 >= c->start && is_directive(r.tok))
      {
        c->dir_set = 1;
        c->dir_addr = a12;
      }
    }
    last_off = r.off; have_last = 1; last_bank = bank;
    continue;

new_marker:
    if(cur >= 0)
    {
      mark* c = &ps->marks[cur];
      if(bank == c->bank && a12 >= c->start)
        c->end = a12;
      else
        c->end = c->dir_set ? c->dir_addr : c->start;
    }
    if(!push_mark(ps, &mk)) { cur = -1; break; }
    cur = ps->n - 1;
    ps->marks[cur].end = ps->marks[cur].start;
    last_off = r.off; have_last = 1; last_bank = bank;
  }
  if(cur >= 0)
  {
    mark* c = &ps->marks[cur];
    unsigned a12 = last_off & 0xFFF;
    c->end = c->dir_set ? c->dir_addr
                        : (have_last && last_bank == c->bank && a12 >= c->start ? a12 : c->start);
  }
  /* all clamped to the bank */
  for(i = 0; i < ps->n; i++)
    if(ps->marks[i].end > BANKSZ) ps->marks[i].end = BANKSZ;
  ps->old_format = !any_new && ps->n > 0;
  free(text);
  free(lines);
  return 1;
}

/* ---- matching marks to files ----------------------------------------------- */

/* Resolve marks against one file; returns how many marks match its text. */
static int match_file(parsed* ps, const sfile* f, int file, int assign)
{
  int matched = 0, i, pos = 0;
  char buf[NORM_MAX];

  for(i = 0; i < ps->n; i++)
  {
    mark* mk = &ps->marks[i];
    if(!ps->old_format)
    {
      if(mk->srcline >= 1 && mk->srcline <= f->nlines)
      {
        norm_text(f->lines[mk->srcline - 1], buf, sizeof(buf));
        if(strcmp(buf, mk->ntext) == 0) matched++;
        if(assign) { mk->file = file; mk->line = mk->srcline; }
      }
      else if(assign)
      {
        mk->file = -1;
        mk->line = 0;
      }
    }
    else
    {
      int k, hit = 0;
      for(k = pos; k < f->nlines; k++)
      {
        norm_text(f->lines[k], buf, sizeof(buf));
        if(strcmp(buf, mk->ntext) == 0) { hit = k + 1; break; }
      }
      if(hit)
      {
        matched++;
        pos = hit;
      }
      if(assign)
      {
        mk->file = hit ? file : -1;
        mk->line = hit;
      }
    }
  }
  return matched;
}

/* Picks the best file for the listing; assigns marks; returns its score. */
static int resolve_marks(parsed* ps, const a2600srcmap* m, const char* lst_path)
{
  int best = -1, best_score = -1, i;
  char stem[PATH_BUF];
  char* dot;

  snprintf(stem, sizeof(stem), "%s", base_of(lst_path));
  dot = strrchr(stem, '.');
  if(dot) *dot = '\0';   /* "main.bas.lst" -> "main.bas" */
  for(i = 0; i < m->nfiles; i++)
  {
    int sc;
    if(!m->files[i].text) continue;
    sc = match_file(ps, &m->files[i], i, 0) * 2 + (strcmp(base_of(m->files[i].key), stem) == 0);
    if(sc > best_score) { best_score = sc; best = i; }
  }
  if(best < 0) return 0;
  return match_file(ps, &m->files[best], best, 1);
}

/* ---- placing ranges ------------------------------------------------------------ */

static void ensure_first(a2600srcmap* m)
{
  int i, k;
  for(i = 0; i < m->nfiles; i++)
  {
    sfile* f = &m->files[i];
    free(f->first);
    f->first_n = f->nlines + 2;
    f->first = (int32_t*)malloc((size_t)f->first_n * sizeof(int32_t));
    if(!f->first) { f->first_n = 0; continue; }
    for(k = 0; k < f->first_n; k++) f->first[k] = -1;
  }
}

/* Fills tables from the resolved marks; returns lines that have code. */
static int place_marks(a2600srcmap* m, const parsed* ps)
{
  int i, mapped = 0;

  reset_tables(m);
  ensure_first(m);
  for(i = 0; i < ps->n; i++)
  {
    const mark* mk = &ps->marks[i];
    sfile* f;
    unsigned a;
    if(mk->file < 0 || mk->line < 1 || mk->line > 65535 || mk->end <= mk->start) continue;
    f = &m->files[mk->file];
    for(a = mk->start; a < mk->end && a < BANKSZ; a++)
    {
      unsigned slot = ((unsigned)mk->bank << 12) | a;
      m->file_of[slot] = (uint16_t)(mk->file + 1);
      m->line_of[slot] = (uint16_t)mk->line;
    }
    m->start[((unsigned)mk->bank << 12) | mk->start] = 1;
    if(f->first && mk->line < f->first_n)
    {
      int32_t v = (int32_t)(((unsigned)mk->bank << 16) | mk->start);
      if(f->first[mk->line] < 0)
      {
        f->first[mk->line] = v;
        mapped++;
      }
      else if(v < f->first[mk->line])
        f->first[mk->line] = v;
    }
  }
  return mapped;
}

/* Parses and applies one listing to m (no locking). -1 = unreadable. */
static int apply_listing(a2600srcmap* m, const char* lst, int* old_format)
{
  parsed ps;
  int mapped;
  if(!parse_listing(lst, &ps)) return -1;
  resolve_marks(&ps, m, lst);
  mapped = place_marks(m, &ps);
  if(old_format) *old_format = ps.old_format;
  parsed_free(&ps);
  return mapped;
}

int a2600srcmap_load_map(a2600srcmap* m, const char* lst_path)
{
  int mapped;
  if(!m || !lst_path) return -1;
  pthread_mutex_lock(&m->lock);
  mapped = apply_listing(m, lst_path, NULL);
  if(mapped >= 0) m->gen++;
  pthread_mutex_unlock(&m->lock);
  return mapped;
}

/* ---- import ----------------------------------------------------------------------- */

static int score_map(a2600srcmap* t, const char* path)
{
  parsed ps;
  int sc;
  if(!t->nfiles || !parse_listing(path, &ps)) return 0;
  sc = ps.n ? resolve_marks(&ps, t, path) : 0;
  parsed_free(&ps);
  return sc;
}

static void sym_for(const char* map_path, char* out, size_t n)
{
  char* dot;
  out[0] = '\0';
  if(!map_path[0]) return;
  snprintf(out, n, "%s", map_path);
  dot = strrchr(out, '.');
  if(!dot || dot < base_of(out) || (size_t)(dot - out) + 5 > n) { out[0] = '\0'; return; }
  strcpy(dot, ".sym");
  if(!is_file(out)) out[0] = '\0';
}

static int find_map(a2600srcmap* t, const char* path_key, const char* dir,
                    char* out, size_t outsz)
{
  char cand[PATH_BUF];
  int best_score = 0, have = 0, i;
  DIR* d;
  struct dirent* e;

  out[0] = '\0';
  for(i = 0; i < 3; i++)
  {
    char stem[PATH_BUF], *dot;
    snprintf(stem, sizeof(stem), "%s", path_key);
    dot = strrchr(stem, '.');
    if(i == 0) snprintf(cand, sizeof(cand), "%s.lst", path_key);
    else if(i == 1)
    {
      if(dot && dot > base_of(stem)) *dot = '\0';
      snprintf(cand, sizeof(cand), "%s.lst", stem);
    }
    else snprintf(cand, sizeof(cand), "%s.bas.lst", path_key);
    if(is_file(cand))
    {
      snprintf(out, outsz, "%s", cand);
      return 1;
    }
  }
  d = opendir(dir);
  if(!d) return 0;
  {
    /* sorted so ties are stable */
    char** names = NULL;
    int n = 0, cap = 0;
    while((e = readdir(d)) != NULL)
    {
      if(e->d_name[0] == '.' || !has_ext(e->d_name, ".lst")) continue;
      if(n == cap)
      {
        char** nn = (char**)realloc(names, (size_t)(cap ? cap * 2 : 16) * sizeof(char*));
        if(!nn) break;
        names = nn;
        cap = cap ? cap * 2 : 16;
      }
      names[n] = dupstr(e->d_name);
      if(names[n]) n++;
    }
    closedir(d);
    if(n) qsort(names, (size_t)n, sizeof(char*), cmp_names);
    for(i = 0; i < n; i++)
    {
      int sc;
      snprintf(cand, sizeof(cand), "%s/%s", dir, names[i]);
      sc = score_map(t, cand);
      if(sc > best_score)
      {
        best_score = sc;
        have = 1;
        snprintf(out, outsz, "%s", cand);
      }
      free(names[i]);
    }
    free(names);
  }
  return have;
}

int a2600srcmap_import(a2600srcmap* m, const char* path, a2600srcmap_import_result* out)
{
  a2600srcmap* t;
  char key[PATH_BUF], dir[PATH_BUF], mapfile[PATH_BUF];
  int isdir, mapped = 0, old_format = 0, files;

  if(out) memset(out, 0, sizeof(*out));
  if(!m || !path) return -1;
  isdir = is_dir(path);
  if(!isdir && !is_file(path)) return -1;
  t = a2600srcmap_create();
  if(!t) return -1;

  canon_key(path, key, sizeof(key));
  if(isdir)
  {
    snprintf(t->root, sizeof(t->root), "%s", key);
    snprintf(dir, sizeof(dir), "%s", key);
    walk(t, key, 0);
  }
  else
  {
    dir_of(key, t->root, sizeof(t->root));
    snprintf(dir, sizeof(dir), "%s", t->root);
    add_file(t, key);
  }
  if(t->nfiles == 0) { a2600srcmap_destroy(t); return -1; }

  ensure_first(t);
  mapfile[0] = '\0';
  if(find_map(t, key, dir, mapfile, sizeof(mapfile)))
  {
    mapped = apply_listing(t, mapfile, &old_format);
    if(mapped < 0) { mapped = 0; mapfile[0] = '\0'; }
  }
  files = t->nfiles;

  pthread_mutex_lock(&m->lock);
  free_files(m);
  m->files = t->files; m->nfiles = t->nfiles; m->cap = t->cap;
  {
    uint16_t* f = m->file_of; uint16_t* l = m->line_of; uint8_t* s = m->start;
    m->file_of = t->file_of; m->line_of = t->line_of; m->start = t->start;
    t->file_of = f; t->line_of = l; t->start = s;
  }
  snprintf(m->root, sizeof(m->root), "%s", t->root);
  m->gen++;
  pthread_mutex_unlock(&m->lock);
  t->files = NULL;
  t->nfiles = t->cap = 0;
  a2600srcmap_destroy(t);

  if(out)
  {
    out->files = files;
    out->mapped = mapped;
    out->old_format = old_format;
    snprintf(out->map_path, sizeof(out->map_path), "%s", mapfile);
    sym_for(mapfile, out->sym_path, sizeof(out->sym_path));
  }
  return 0;
}

/* ---- queries --------------------------------------------------------------------------- */

int a2600srcmap_file_count(const a2600srcmap* m)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  int n;
  pthread_mutex_lock(&mm->lock);
  n = mm->nfiles;
  pthread_mutex_unlock(&mm->lock);
  return n;
}

const char* a2600srcmap_file_name(const a2600srcmap* m, int file)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  const char* s = NULL;
  pthread_mutex_lock(&mm->lock);
  if(file >= 0 && file < mm->nfiles) s = mm->files[file].name;
  pthread_mutex_unlock(&mm->lock);
  return s;
}

int a2600srcmap_line_count(const a2600srcmap* m, int file)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  int n = 0;
  pthread_mutex_lock(&mm->lock);
  if(file >= 0 && file < mm->nfiles) n = mm->files[file].nlines;
  pthread_mutex_unlock(&mm->lock);
  return n;
}

const char* a2600srcmap_line_text(const a2600srcmap* m, int file, int line)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  const char* s = "";
  pthread_mutex_lock(&mm->lock);
  if(file >= 0 && file < mm->nfiles && line >= 1 && line <= mm->files[file].nlines)
    s = mm->files[file].lines[line - 1];
  pthread_mutex_unlock(&mm->lock);
  return s;
}

int a2600srcmap_line_has_code(const a2600srcmap* m, int file, int line)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  int r = 0;
  pthread_mutex_lock(&mm->lock);
  if(file >= 0 && file < mm->nfiles && line >= 1 && line < mm->files[file].first_n)
    r = mm->files[file].first[line] >= 0;
  pthread_mutex_unlock(&mm->lock);
  return r;
}

int a2600srcmap_lookup(const a2600srcmap* m, int bank, unsigned addr, int* file, int* line)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  unsigned slot;
  int hit;
  if(bank < 0 || bank >= NBANKS) return 0;
  slot = ((unsigned)bank << 12) | (addr & 0xFFF);
  pthread_mutex_lock(&mm->lock);
  hit = mm->file_of[slot] != 0;
  if(hit)
  {
    if(file) *file = mm->file_of[slot] - 1;
    if(line) *line = mm->line_of[slot];
  }
  pthread_mutex_unlock(&mm->lock);
  return hit;
}

int a2600srcmap_is_line_start(const a2600srcmap* m, int bank, unsigned addr)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  unsigned slot;
  int hit;
  if(bank < 0 || bank >= NBANKS) return 0;
  slot = ((unsigned)bank << 12) | (addr & 0xFFF);
  pthread_mutex_lock(&mm->lock);
  hit = mm->start[slot];
  pthread_mutex_unlock(&mm->lock);
  return hit;
}

int a2600srcmap_first_addr(const a2600srcmap* m, int file, int line, int* bank)
{
  a2600srcmap* mm = (a2600srcmap*)m;
  int r = -1;
  pthread_mutex_lock(&mm->lock);
  if(file >= 0 && file < mm->nfiles && line >= 1 && line < mm->files[file].first_n &&
     mm->files[file].first[line] >= 0)
  {
    int32_t v = mm->files[file].first[line];
    if(bank) *bank = v >> 16;
    r = 0xF000 | (v & 0xFFF);
  }
  pthread_mutex_unlock(&mm->lock);
  return r;
}

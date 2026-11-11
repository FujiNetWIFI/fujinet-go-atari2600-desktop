/*
 * srcmap_test -- the batari Basic source map: dasm listings (new ";;line N;;"
 * and old ".Lnn ; text" formats) against the real fixtures in
 * core/tests/data/bbasic. Pure: no machine, no SDL.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "a2600srcmap.h"

#ifndef BBASIC_FIXTURE_DIR
#error "BBASIC_FIXTURE_DIR must name core/tests/data/bbasic"
#endif

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static void fixture(char* dst, size_t n, const char* rel)
{
  snprintf(dst, n, "%s/%s", BBASIC_FIXTURE_DIR, rel);
}

static int copy_file(const char* from, const char* to)
{
  FILE* a = fopen(from, "rb");
  FILE* b = fopen(to, "wb");
  char buf[4096];
  size_t k;
  if (!a || !b) { if (a) fclose(a); if (b) fclose(b); return 0; }
  while ((k = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, k, b);
  fclose(a);
  fclose(b);
  return 1;
}

/* line -> (bank, run-time address); -1 if no code */
static int addr_of(a2600srcmap* m, int line, int* bank)
{
  return a2600srcmap_first_addr(m, 0, line, bank);
}

/* Checks main.bas (or old/main.bas): every line's address from the listing. */
static void check_main(a2600srcmap* m, const char* tag)
{
  static const struct { int line; int addr; } code[] = {
    { 3, 0xF45D }, { 6, 0xF461 }, { 7, 0xF465 }, { 8, 0xF467 }, { 9, 0xF46A },
    { 10, 0xF46D }, { 13, 0xF470 }, { 14, 0xF474 } };
  static const int none[] = { 1, 2, 4, 5, 11, 12 };   /* rem, dim, blank, labels */
  char msg[160];
  size_t i;
  int bank, file, line;

  for (i = 0; i < sizeof(code) / sizeof(code[0]); i++)
  {
    bank = -1;
    snprintf(msg, sizeof(msg), "%s: line %d first_addr", tag, code[i].line);
    CHECK(addr_of(m, code[i].line, &bank) == code[i].addr && bank == 0, msg);
    snprintf(msg, sizeof(msg), "%s: line %d has code", tag, code[i].line);
    CHECK(a2600srcmap_line_has_code(m, 0, code[i].line) == 1, msg);
    snprintf(msg, sizeof(msg), "%s: line %d is a line start", tag, code[i].line);
    CHECK(a2600srcmap_is_line_start(m, 0, code[i].addr) == 1, msg);
    file = line = -1;
    snprintf(msg, sizeof(msg), "%s: lookup of line %d", tag, code[i].line);
    CHECK(a2600srcmap_lookup(m, 0, code[i].addr, &file, &line) == 1 && file == 0 && line == code[i].line, msg);
  }
  for (i = 0; i < sizeof(none) / sizeof(none[0]); i++)
  {
    snprintf(msg, sizeof(msg), "%s: line %d has no code", tag, none[i]);
    CHECK(addr_of(m, none[i], &bank) == -1 && a2600srcmap_line_has_code(m, 0, none[i]) == 0, msg);
  }
  /* mid-instruction: still line 6, not a start (LDA #$86 is 2 bytes + STA 2) */
  line = 0;
  CHECK(a2600srcmap_lookup(m, 0, 0xF463, NULL, &line) == 1 && line == 6, "main: f463 is line 6");
  CHECK(a2600srcmap_is_line_start(m, 0, 0xF463) == 0, "main: f463 is not a start");
  /* the RTS is the last user byte; the runtime after it is not line 14 */
  CHECK(a2600srcmap_lookup(m, 0, 0xF474, NULL, &line) == 1 && line == 14, "main: f474 is line 14");
  CHECK(a2600srcmap_lookup(m, 0, 0xF475, NULL, NULL) == 0, "main: f475 (after the last line) unmapped");
  CHECK(a2600srcmap_lookup(m, 0, 0xF3BD, NULL, NULL) == 0, "main: drawscreen kernel unmapped");
  CHECK(a2600srcmap_lookup(m, 0, 0xF000, NULL, NULL) == 0, "main: f000 unmapped");
  CHECK(a2600srcmap_lookup(m, 1, 0xF461, NULL, NULL) == 0, "main: bank 1 unmapped");
  CHECK(a2600srcmap_lookup(m, -1, 0xF461, NULL, NULL) == 0 && a2600srcmap_lookup(m, 16, 0xF461, NULL, NULL) == 0,
        "main: bad banks unmapped");
}

int main(void)
{
  char p[600], d[600];
  a2600srcmap_import_result r;
  a2600srcmap* m = a2600srcmap_create();
  unsigned g;

  CHECK(m != NULL, "create");
  CHECK(a2600srcmap_shared() == a2600srcmap_shared() && a2600srcmap_shared() != NULL, "shared is stable");

  /* ---- import a .bas file ---- */
  fixture(p, sizeof(p), "main.bas");
  g = a2600srcmap_generation(m);
  CHECK(a2600srcmap_import(m, p, &r) == 0, "import main.bas");
  CHECK(a2600srcmap_generation(m) > g, "generation bumps on import");
  CHECK(r.files == 1 && r.old_format == 0, "one file, new format");
  CHECK(r.mapped == 8, "eight lines have code");
  CHECK(strstr(r.map_path, "main.bas.lst") != NULL, "map is main.bas.lst");
  CHECK(strstr(r.sym_path, "main.bas.sym") != NULL, "sym is main.bas.sym");
  CHECK(strcmp(a2600srcmap_file_name(m, 0), "main.bas") == 0, "display name");
  CHECK(a2600srcmap_line_count(m, 0) == 14, "14 lines");
  CHECK(strcmp(a2600srcmap_line_text(m, 0, 6), " COLUBK = $86") == 0, "line text");
  CHECK(a2600srcmap_line_text(m, 0, 99)[0] == '\0', "past the end is empty");
  check_main(m, "new");

  /* ---- the old format gives the same addresses ---- */
  fixture(p, sizeof(p), "old/main.bas");
  CHECK(a2600srcmap_import(m, p, &r) == 0, "import old/main.bas");
  CHECK(r.old_format == 1, "old format detected");
  CHECK(r.mapped == 8, "old: eight lines have code");
  CHECK(strstr(r.map_path, "old/main.bas.lst") != NULL, "old: map chosen");
  check_main(m, "old");

  /* ---- banked ---- */
  fixture(p, sizeof(p), "banked.bas");
  CHECK(a2600srcmap_import(m, p, &r) == 0 && r.old_format == 0, "import banked.bas");
  {
    int bank = -1, line = 0, file = -1;
    CHECK(r.mapped == 8, "banked: eight lines have code");   /* 3,6,7,8,9,13,14,15 */
    CHECK(addr_of(m, 3, &bank) == 0xF000 && bank == 0, "banked: line 3 at bank 0 $F000");
    CHECK(addr_of(m, 6, &bank) == 0xF004 && bank == 0, "banked: line 6 (main loop)");
    CHECK(addr_of(m, 8, &bank) == 0xF020 && bank == 0, "banked: line 8");
    CHECK(addr_of(m, 9, &bank) == 0xF038 && bank == 0, "banked: line 9");
    CHECK(addr_of(m, 13, &bank) == 0xF4D6 && bank == 1, "banked: bump line 13 is bank 1 $4D6");
    CHECK(addr_of(m, 14, &bank) == 0xF4D8 && bank == 1, "banked: line 14");
    CHECK(addr_of(m, 15, &bank) == 0xF4DC && bank == 1, "banked: line 15");
    CHECK(addr_of(m, 1, &bank) == -1 && addr_of(m, 2, &bank) == -1, "banked: set/dim have no code");
    CHECK(addr_of(m, 10, &bank) == -1 && addr_of(m, 11, &bank) == -1, "banked: blank and `bank 2` have no code");
    CHECK(addr_of(m, 12, &bank) == -1, "banked: the `bump` label has no code");

    CHECK(a2600srcmap_lookup(m, 1, 0x4D6, &file, &line) == 1 && line == 13, "bank 1 $4D6 -> line 13");
    CHECK(a2600srcmap_lookup(m, 0, 0x4D6, NULL, NULL) == 0, "bank 0 $4D6 does not map");
    CHECK(a2600srcmap_lookup(m, 1, 0x004, NULL, NULL) == 0, "bank 1 $004 does not map");
    CHECK(a2600srcmap_lookup(m, 0, 0x004, NULL, &line) == 1 && line == 6, "bank 0 $004 -> line 6");
    CHECK(a2600srcmap_lookup(m, 0, 0xF004, NULL, &line) == 1 && line == 6, "pc with the $F000 bits works");
    CHECK(a2600srcmap_is_line_start(m, 1, 0x4D6) == 1 && a2600srcmap_is_line_start(m, 0, 0x4D6) == 0,
          "line starts are per bank");
    CHECK(a2600srcmap_lookup(m, 1, 0x4DE, NULL, &line) == 1 && line == 15, "the last byte of `return otherbank` is line 15");
    CHECK(a2600srcmap_lookup(m, 1, 0x4DF, NULL, NULL) == 0, "bank 1 after the last line (kernel) unmapped");
    CHECK(a2600srcmap_lookup(m, 0, 0x03B, NULL, NULL) == 0, "bank 0 after the last jmp (bank 2 / echo) unmapped");
    CHECK(a2600srcmap_lookup(m, 0, 0x03A, NULL, &line) == 1 && line == 9, "last jmp byte is line 9");
    CHECK(a2600srcmap_lookup(m, 0, 0xFD4, NULL, NULL) == 0, "bank 0 kernel unmapped");
    CHECK(a2600srcmap_lookup(m, 1, 0x436, NULL, NULL) == 0, "bank 1 drawscreen routine unmapped");
  }

  /* ---- folder import: all .bas under it, a map is chosen ---- */
  CHECK(a2600srcmap_import(m, BBASIC_FIXTURE_DIR, &r) == 0, "folder import");
  CHECK(r.files == 3, "folder: banked.bas, main.bas, old/main.bas");
  CHECK(r.mapped > 0 && r.map_path[0] != '\0', "folder: a map is chosen");
  CHECK(strstr(a2600srcmap_file_name(m, 2), "old/main.bas") != NULL, "folder: relative display name");

  /* a temp folder holding one program + its listing: picked by scan */
  {
    char tmp[] = "/tmp/a2600srcmap_testXXXXXX";
    char a[700], b[700];
    CHECK(mkdtemp(tmp) != NULL, "mkdtemp");
    snprintf(a, sizeof(a), "%s/game.bas", tmp);
    fixture(p, sizeof(p), "main.bas");
    CHECK(copy_file(p, a), "copy game.bas");
    snprintf(b, sizeof(b), "%s/odd name.lst", tmp);
    fixture(d, sizeof(d), "main.bas.lst");
    CHECK(copy_file(d, b), "copy listing");
    CHECK(a2600srcmap_import(m, tmp, &r) == 0 && r.files == 1, "temp folder import");
    CHECK(r.mapped == 8 && strstr(r.map_path, "odd name.lst") != NULL, "listing found by content");
    CHECK(r.sym_path[0] == '\0', "no sym beside it");
    CHECK(addr_of(m, 6, &(int){0}) == 0xF461, "temp: addresses");
    CHECK(a2600srcmap_import(m, a, &r) == 0 && r.mapped == 8, "single file import falls back to a scan");
    unlink(b);

    /* a missing listing: the text still shows, nothing is mapped */
    g = a2600srcmap_generation(m);
    CHECK(a2600srcmap_import(m, a, &r) == 0, "import without a listing");
    CHECK(r.files == 1 && r.mapped == 0 && r.map_path[0] == '\0' && r.sym_path[0] == '\0', "unmapped result");
    CHECK(a2600srcmap_generation(m) > g, "generation bumps");
    CHECK(strcmp(a2600srcmap_line_text(m, 0, 6), " COLUBK = $86") == 0, "text still shows");
    CHECK(a2600srcmap_lookup(m, 0, 0xF461, NULL, NULL) == 0, "nothing mapped");
    CHECK(addr_of(m, 6, &(int){0}) == -1 && a2600srcmap_line_has_code(m, 0, 6) == 0, "no addresses");

    /* load_map after the fact */
    fixture(d, sizeof(d), "main.bas.lst");
    CHECK(a2600srcmap_load_map(m, d) == 8, "load_map maps eight lines");
    CHECK(a2600srcmap_lookup(m, 0, 0xF461, NULL, &(int){0}) == 1, "load_map: lookup");
    CHECK(a2600srcmap_load_map(m, "/nonexistent/x.lst") == -1, "load_map of a missing file");
    CHECK(a2600srcmap_lookup(m, 0, 0xF461, NULL, NULL) == 1, "a failed load keeps the map");

    snprintf(b, sizeof(b), "%s/game.bas", tmp);
    unlink(b);
    rmdir(tmp);
  }

  /* ---- errors, clear ---- */
  g = a2600srcmap_generation(m);
  CHECK(a2600srcmap_import(m, "/nonexistent/none.bas", &r) == -1, "unreadable path");
  CHECK(a2600srcmap_generation(m) == g && a2600srcmap_file_count(m) == 1, "a failed import keeps the old map");
  a2600srcmap_clear(m);
  CHECK(a2600srcmap_generation(m) > g, "clear bumps the generation");
  CHECK(a2600srcmap_file_count(m) == 0, "clear empties the files");
  CHECK(a2600srcmap_lookup(m, 0, 0xF461, NULL, NULL) == 0 && a2600srcmap_is_line_start(m, 0, 0xF461) == 0,
        "clear empties the tables");
  CHECK(a2600srcmap_first_addr(m, 0, 6, NULL) == -1, "clear: no first_addr");
  CHECK(a2600srcmap_file_name(m, 0) == NULL && a2600srcmap_line_count(m, 0) == 0, "clear: no names");

  a2600srcmap_destroy(m);
  printf(fails ? "srcmap_test: %d FAILED\n" : "srcmap_test: all passed\n", fails);
  return fails ? 1 : 0;
}

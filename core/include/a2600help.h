/*
 * a2600help -- the app's Help: data/help/help.md, built in by
 * tools/help/make-help.py as topics of blocks of styled spans, which every
 * frontend lays out with its own toolkit.
 *
 * A frontend shows the blocks whose platforms include its own bit. Ids are
 * GitHub's heading slugs ("controllers-and-keys"); a Help button names a
 * context instead ("prefs/Machine", "debugger/cpu"), which stays put when a
 * heading is renamed. Everything here is constant data and reentrant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A2600HELP_H
#define A2600HELP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A2600HELP_GNOME = 1, A2600HELP_KDE = 2, A2600HELP_WINDOWS = 4, A2600HELP_MACOS = 8
};

typedef enum {
    A2600HELP_H2 = 0, A2600HELP_H3,
    A2600HELP_PARA,
    A2600HELP_BULLET, A2600HELP_BULLET2,    /* "- " and "  - " */
    A2600HELP_NUMBERED,                     /* a step; number is its own */
    A2600HELP_NOTE,
    A2600HELP_PRE,                          /* one TEXT span, lines as they are */
    A2600HELP_TABLE_HEAD,                   /* starts a table; its rows follow */
    A2600HELP_TABLE_ROW
} a2600help_block_kind;

typedef enum {
    A2600HELP_TEXT = 0, A2600HELP_BOLD, A2600HELP_ITALIC, A2600HELP_CODE,
    A2600HELP_KBD,                          /* keys; mac_text is the Mac's */
    A2600HELP_LINK,                         /* target "#slug" or a URL */
    A2600HELP_CELL                          /* tables: the next column starts */
} a2600help_span_kind;

typedef struct {
    int kind;
    const char *text;                       /* UTF-8 */
    const char *mac_text;                   /* KBD on macOS, or NULL: text */
    const char *target;
} a2600help_span;

typedef struct {
    int kind;
    unsigned platforms;                     /* A2600HELP_* bits */
    int number;
    const char *id;                         /* headings: the slug */
    const char *context;                    /* headings: or NULL */
    const a2600help_span *spans;            /* headings: one TEXT span */
    int nspans;
    /* Tables: the width of each column but the last, in ems, for frontends
     * that set tab stops (the last column wraps under itself). */
    const unsigned char *col_em;
    int ncol_em;
} a2600help_block;

typedef struct {
    const char *id;
    const char *title;
    const char *context;                    /* or NULL */
    const a2600help_block *blocks;
    int nblocks;
} a2600help_topic;

int a2600help_topic_count(void);
const a2600help_topic *a2600help_topic_at(int t);

/* "slug", "#slug" or a context: its topic, and the heading's block (-1 for
 * the topic itself). 0, or -1 if there is none. */
int a2600help_find(const char *id, int *topic, int *block);

/* The span's text as platform shows it. */
const char *a2600help_span_text(const a2600help_span *sp, unsigned platform);

/* A heading block's text. */
const char *a2600help_heading(const a2600help_block *b);

/* The block after the section headed by block (the next heading at its
 * level or above, or the topic's end); -1: the whole topic. */
int a2600help_section_end(int topic, int block);

/* The section headed by block (-1: from the topic's title) as text on
 * platform: a line per block, cells separated by " | ". Returns its length
 * (truncated to dstsz - 1). */
int a2600help_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz);

/* Sections that have every word of query (case-insensitive), best first:
 * where it is in a title, then how often. block -1 is a topic's start, up
 * to its first heading. Returns how many were written. */
typedef struct {
    int topic, block, score;
} a2600help_hit;
int a2600help_search(unsigned platform, const char *query, a2600help_hit *out, int max);

/* The first match of a word of query in text at byte from or after: where
 * it starts and its length, for highlighting. 1, or 0 if there is none. */
int a2600help_match(const char *text, const char *query, int from, int *start, int *len);

#ifdef __cplusplus
}
#endif

#endif /* A2600HELP_H */

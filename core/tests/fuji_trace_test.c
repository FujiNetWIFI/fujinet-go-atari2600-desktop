/*
 * fuji_trace_test -- the debugger's FujiNet trace decoder on synthetic
 * bytes: SLIP frames both ways, a reply split mid-escape, repeats folded,
 * a dangling reply dropped by the next request, the ring's cap, and Clear.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fuji_trace.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

/* A FujiBus packet (device, command, LE length, checksum, descriptor,
 * payload), SLIP-framed. Returns the framed length. */
static size_t frame(uint8_t *out, uint8_t dev, uint8_t cmd, const uint8_t *payload, size_t plen)
{
    uint8_t pkt[256];
    size_t n = 0, o = 0, i;
    pkt[n++] = dev;
    pkt[n++] = cmd;
    pkt[n++] = (uint8_t)((6 + plen) & 0xff);
    pkt[n++] = (uint8_t)((6 + plen) >> 8);
    pkt[n++] = 0;
    pkt[n++] = 0;
    memcpy(pkt + n, payload, plen);
    n += plen;
    out[o++] = 0xC0;
    for (i = 0; i < n; i++) {
        if (pkt[i] == 0xC0) { out[o++] = 0xDB; out[o++] = 0xDC; }
        else if (pkt[i] == 0xDB) { out[o++] = 0xDB; out[o++] = 0xDD; }
        else out[o++] = pkt[i];
    }
    out[o++] = 0xC0;
    return o;
}

static a2600_fuji_packet p[FUJI_TRACE_MAX + 8];

int main(void)
{
    uint8_t req[600], rep[600], other[600];
    static const uint8_t reply_payload[] = { 0xC0, 0xDB, 'S', 'D', 0 };
    static const uint8_t other_payload[] = { 1 };
    uint64_t packets[2], bytes[2], last;
    size_t nreq, nrep, nother, cut, i;
    int n;
    fuji_trace *t = fuji_trace_new();

    check(fuji_trace_copy(t, p, 16) == 0, "a new trace is empty");

    /* a request, whole */
    nreq = frame(req, 0x70, 0xF4, NULL, 0);
    fuji_trace_feed(t, 1, req, nreq);
    n = fuji_trace_copy(t, p, 16);
    check(n == 1, "a request is one packet");
    check(p[0].to_host == 0 && p[0].device == 0x70 && p[0].command == 0xF4 && p[0].length == 6
          && p[0].nbytes == 6 && p[0].repeat == 1, "its FujiBus header");
    check(strstr(p[0].text, "2600 \xe2\x86\x92 FujiNet") != NULL && strstr(p[0].text, "READ HOST SLOTS") != NULL,
          "its text names the way and the command");
    printf("     %s\n", p[0].text);

    /* the reply, split inside the escape of its 0xC0 byte */
    nrep = frame(rep, 0x70, 0x01, reply_payload, sizeof reply_payload);
    for (cut = 0; cut + 1 < nrep && !(rep[cut] == 0xDB && rep[cut + 1] == 0xDC); cut++) {}
    fuji_trace_feed(t, 0, rep, cut + 1);
    check(fuji_trace_copy(t, p, 16) == 1, "half a reply is not a packet yet");
    fuji_trace_feed(t, 0, rep + cut + 1, 2);
    fuji_trace_feed(t, 0, rep + cut + 3, nrep - cut - 3);
    n = fuji_trace_copy(t, p, 16);
    check(n == 2, "the reply, fed in three pieces, is one packet");
    check(p[1].to_host == 1 && p[1].nbytes == 6 + (int)sizeof reply_payload, "the reply's length");
    check(p[1].data[6] == 0xC0 && p[1].data[7] == 0xDB, "SLIP escapes decoded");
    check(strstr(p[1].text, "FujiNet \xe2\x86\x92 2600") != NULL, "the reply's way");
    printf("     %s\n", p[1].text);
    check(p[1].serial > p[0].serial, "serials rise");

    /* the same exchange twice more folds into the first */
    last = p[1].serial;
    for (i = 0; i < 2; i++) {
        fuji_trace_feed(t, 1, req, nreq);
        fuji_trace_feed(t, 0, rep, nrep);
    }
    n = fuji_trace_copy(t, p, 16);
    check(n == 2, "repeated exchanges fold");
    check(p[0].repeat == 3 && p[1].repeat == 3, "each way counted three times");
    check(strstr(p[0].text, "\xc3\x97" "3") != NULL && strstr(p[1].text, "\xc3\x97" "3") != NULL,
          "the text says x3");
    check(p[1].serial > last, "a fold bumps the last serial");
    printf("     %s\n     %s\n", p[0].text, p[1].text);

    /* a different request breaks the run; a request repeated alone folds */
    nother = frame(other, 0x70, 0xE8, other_payload, sizeof other_payload);
    fuji_trace_feed(t, 1, other, nother);
    fuji_trace_feed(t, 1, other, nother);
    n = fuji_trace_copy(t, p, 16);
    check(n == 3 && p[2].command == 0xE8 && p[2].repeat == 2, "a lone repeat folds too");

    /* half a reply, then a request: the half is thrown away */
    fuji_trace_feed(t, 0, rep, 4);
    fuji_trace_feed(t, 1, req, nreq);
    fuji_trace_feed(t, 0, rep, nrep);
    n = fuji_trace_copy(t, p, 16);
    check(n == 5 && p[4].to_host == 1 && p[4].nbytes == 6 + (int)sizeof reply_payload,
          "a request drops a dangling reply");

    /* a frame too short for a header */
    {
        static const uint8_t junk[] = { 0xC0, 0x01, 0x02, 0xC0 };
        fuji_trace_feed(t, 0, junk, sizeof junk);
        n = fuji_trace_copy(t, p, 16);
        check(n == 6 && p[5].device == -1 && p[5].command == -1 && p[5].nbytes == 2, "a short frame");
        printf("     %s\n", p[5].text);
    }

    /* totals count everything, folded or not */
    fuji_trace_totals(t, packets, bytes);
    check(packets[0] == 6 && packets[1] == 5, "packet totals each way");
    check(bytes[0] == 4 * nreq + 2 * nother && bytes[1] == 4 * nrep + 4 + 4, "byte totals each way");

    /* the ring keeps the newest 512, oldest first */
    for (i = 0; i < 600; i++) {
        uint8_t pl[2] = { (uint8_t)(i & 0xff), (uint8_t)(i >> 8) };
        nother = frame(other, 0x70, 0xF6, pl, sizeof pl);
        fuji_trace_feed(t, 1, other, nother);
    }
    n = fuji_trace_copy(t, p, FUJI_TRACE_MAX + 8);
    check(n == FUJI_TRACE_MAX, "capped at 512");
    check(p[n - 1].data[6] == (599 & 0xff) && p[n - 1].data[7] == (599 >> 8), "the newest is last");
    for (i = 1; i < (size_t)n && p[i].serial > p[i - 1].serial; i++) {}
    check(i == (size_t)n, "oldest first");
    n = fuji_trace_copy(t, p, 4);
    check(n == 4 && p[3].data[6] == (599 & 0xff), "a short copy takes the newest");

    fuji_trace_clear(t);
    check(fuji_trace_copy(t, p, 16) == 0, "Clear empties it");
    fuji_trace_totals(t, packets, bytes);
    check(packets[0] == 606, "Clear leaves the totals");

    fuji_trace_free(t);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

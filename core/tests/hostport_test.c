/*
 * The external-FujiNet address field: "host:port", a bare port, empty text
 * for the default, and everything that must be refused.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "a2600session.h"

static int failures;

static void ok(const char *text, const char *want_host, int want_port)
{
    char host[64] = "";
    int port = 0;
    int rc = a2600session_parse_hostport(text, host, sizeof(host), &port);
    if (rc != 0 || strcmp(host, want_host) || port != want_port) {
        fprintf(stderr, "FAIL \"%s\": rc %d, got %s:%d, want %s:%d\n", text,
                rc, host, port, want_host, want_port);
        failures++;
    }
}

static void bad(const char *text)
{
    char host[64] = "keep";
    int port = 7;
    if (a2600session_parse_hostport(text, host, sizeof(host), &port) == 0 ||
        strcmp(host, "keep") || port != 7) {
        fprintf(stderr, "FAIL \"%s\" should be refused untouched\n", text);
        failures++;
    }
}

int main(void)
{
    ok("", "127.0.0.1", 11504);
    ok("   ", "127.0.0.1", 11504);
    ok(NULL, "127.0.0.1", 11504);
    ok("65300", "127.0.0.1", 65300);
    ok("0.0.0.0:11504", "0.0.0.0", 11504);
    ok("192.168.1.20:9985", "192.168.1.20", 9985);
    ok("  127.0.0.1:1  ", "127.0.0.1", 1);
    ok("localhost:65535", "localhost", 65535);
    ok("fujinet.example.com:11504", "fujinet.example.com", 11504);
    ok("my-box_2:80", "my-box_2", 80);

    bad("0");
    bad("65536");
    bad("-5");
    bad("abc");
    bad("0.0.0.0:");
    bad(":11504");
    bad("0.0.0.0:abc");
    bad("256.1.1.1:11504");
    bad("1.2.3:11504");
    bad("1.2.3.4.5:11504");
    bad("exa mple.com:11504");
    bad("-lead.com:11504");
    bad("1.2.3.4:11504x");
    return failures ? 1 : 0;
}

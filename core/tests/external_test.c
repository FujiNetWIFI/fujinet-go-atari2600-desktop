/*
 * external_test -- "Use external FujiNet": a bad address refuses to start; a
 * good one is dialled by the cartridge (a stand-in TCP listener on an
 * ephemeral port sees the connection) and no built-in runtime is started.
 * POSIX only; the Windows cross-build compiles it empty.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "a2600session.h"
#include "test_tmpdir.h"

#ifdef _WIN32
int main(void) { return 77; }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    char cfg[512], data[512], addr[64];
    a2600session_paths paths;
    a2600session *session;
    a2600session_start_opts opts;
    struct sockaddr_in sin;
    socklen_t sinlen = sizeof sin;
    struct pollfd pfd;
    int listener, conn = -1;

    test_tmpdir(cfg, sizeof cfg, "xcfg");
    test_tmpdir(data, sizeof data, "xdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    session = a2600session_new(&paths);
    if (!session) return 1;

    a2600session_default_opts(session, &opts);
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.external_fujinet = 1;
    opts.boip_connect = "not an address";
    check(a2600session_start(session, &opts) != 0, "a bad external address refuses to start");
    check(!a2600session_is_running(session), "...and nothing runs");

    listener = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, (struct sockaddr *)&sin, sizeof sin) != 0 ||
        listen(listener, 1) != 0 || getsockname(listener, (struct sockaddr *)&sin, &sinlen) != 0) {
        printf("cannot open the stand-in listener\n");
        return 1;
    }
    snprintf(addr, sizeof addr, "127.0.0.1:%d", ntohs(sin.sin_port));
    opts.boip_connect = addr;
    if (a2600session_start(session, &opts) != 0) {
        printf("start failed: %s\n", a2600session_last_error(session));
        return 1;
    }
    check(a2600session_fujinet_external(session), "the session reports external");
    check(!a2600session_fujinet_running(session), "no built-in runtime was started");
    check(strcmp(a2600session_boip_endpoint(session), addr) == 0, "the endpoint is the address");
    check(a2600session_fujinet_webui_url(session)[0] == '\0', "no built-in web UI URL");

    pfd.fd = listener;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 10000) > 0)
        conn = accept(listener, NULL, NULL);
    check(conn >= 0, "the cartridge dialled the external address");
    if (conn >= 0)
        close(conn);

    a2600session_stop(session);
    check(!a2600session_fujinet_external(session), "external is cleared on stop");
    close(listener);
    a2600session_free(session);
    return failures ? 1 : 0;
}
#endif

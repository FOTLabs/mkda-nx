/*
 * xbox_relaunch (POSIX hosts): restart this process for a title that rebooted
 * itself through XLaunchNewImage. The console's quick reboot gives the new
 * image fresh memory and the launch data page; here the page is written to a
 * file, RECOMP_LAUNCH_DATA names it, and the process re-executes itself with
 * its original arguments. kernel_bridge.c loads the page back at startup.
 * (Switch: the game's platform glue provides its own, via envSetNextLoad.)
 */
#if !defined(_WIN32) && !defined(__SWITCH__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void xbox_relaunch(const void *page, size_t len)
{
    static char path[4096], cmd[65536], *argv[256];
    const char *dir = getenv("RECOMP_LAUNCH_DIR");
    FILE *f;
    size_t n, i;
    int argc = 0;

    snprintf(path, sizeof path, "%s/launchdata.bin", dir && *dir ? dir : ".");
    f = fopen(path, "wb");
    if (!f || fwrite(page, 1, len, f) != len) {
        fprintf(stderr, "  [relaunch] cannot write %s\n", path);
        if (f) fclose(f);
        return;
    }
    fclose(f);
    if (path[0] != '/') {
        char cwd[2048];
        if (getcwd(cwd, sizeof cwd)) {
            char tmp[4096];
            snprintf(tmp, sizeof tmp, "%s/%s", cwd, path);
            strcpy(path, tmp);
        }
    }
    setenv("RECOMP_LAUNCH_DATA", path, 1);
    {   /* A test drives the menus with one input script and the relaunched
         * image with another (RECOMP_PAD_SCRIPT_RELAUNCH). */
        const char *next = getenv("RECOMP_PAD_SCRIPT_RELAUNCH");
        if (next && *next)
            setenv("RECOMP_PAD_SCRIPT_FILE", next, 1);
    }
    f = fopen("/proc/self/cmdline", "rb");
    n = f ? fread(cmd, 1, sizeof cmd - 1, f) : 0;
    if (f) fclose(f);
    cmd[n] = 0;
    for (i = 0; i < n && argc < 255; i += strlen(cmd + i) + 1)
        argv[argc++] = cmd + i;
    argv[argc] = NULL;
    fprintf(stderr, "  [relaunch] re-executing with %s\n", path);
    fflush(stderr);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "  [relaunch] execv failed\n");
}
#endif

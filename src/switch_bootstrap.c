/* Buffered SD logging follows the proven Buffy/Conker boot infrastructure.
 * Keep libnx's types separate from the runtime's Win32 compatibility headers. */
#include <switch.h>
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>
#include "switch_bootstrap.h"

extern uint64_t mkda_frame_wait(uint64_t *, uint64_t, uint64_t, int);
void mkda_game_frame_pace(int active)
{
    static uint64_t deadline;
    static int announced;
    if (active && !announced) {
        announced = 1;
        fprintf(stderr, "[MKDA] Combat and arcade endings paced at60Hz\n");
    }
    uint64_t wait = mkda_frame_wait(&deadline, armGetSystemTick(), 19200000, active);
    if (wait) svcSleepThread((int64_t)((wait * 1000000000 + 19199999) / 19200000));
}

#define LOG_CAP (512u * 1024u)
static Mutex log_lock;
static char *log_buffer;
static size_t log_length;
static int log_fd = -1, line_start = 1, log_disabled, synchronous;
static u64 started;
static pthread_t log_thread;
static int thread_started;
static atomic_bool stopping;

static void drain_locked(void)
{
    size_t written = 0;
    while (written < log_length) {
        ssize_t n = write(log_fd, log_buffer + written, log_length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    log_length -= written;
    if (written && log_length) memmove(log_buffer, log_buffer + written, log_length);
}

static void append_locked(const char *data, size_t length)
{
    while (length) {
        if (log_length == LOG_CAP) {
            drain_locked();
            /* A failing/full SD card must not wedge the guest's logger. */
            if (log_length == LOG_CAP) log_length = 0;
        }
        size_t n = length < LOG_CAP - log_length ? length : LOG_CAP - log_length;
        memcpy(log_buffer + log_length, data, n);
        log_length += n;
        data += n;
        length -= n;
    }
}

static ssize_t log_write(struct _reent *r, void *fd, const char *data, size_t length)
{
    (void)r; (void)fd;
    if (log_disabled || log_fd < 0) return (ssize_t)length;
    mutexLock(&log_lock);
    for (size_t offset = 0; offset < length;) {
        if (line_start) {
            char stamp[32];
            u64 ms = armTicksToNs(armGetSystemTick() - started) / 1000000ull;
            /* Nested float formatting corrupts newlib's per-thread dtoa buffer. */
            int n = snprintf(stamp, sizeof stamp, "[%4llu.%03llu] ",
                             (unsigned long long)(ms / 1000),
                             (unsigned long long)(ms % 1000));
            if (n > 0) append_locked(stamp, (size_t)n);
            line_start = 0;
        }
        const char *newline = memchr(data + offset, '\n', length - offset);
        size_t n = newline ? (size_t)(newline - data - offset) + 1 : length - offset;
        append_locked(data + offset, n);
        offset += n;
        if (newline) line_start = 1;
    }
    if (synchronous) drain_locked();
    mutexUnlock(&log_lock);
    return (ssize_t)length;
}

static int log_open(struct _reent *r, void *fd, const char *path, int flags, int mode)
{
    (void)r; (void)fd; (void)path; (void)flags; (void)mode;
    return 0;
}
static int log_close(struct _reent *r, void *fd) { (void)r; (void)fd; return 0; }
static int log_stat(struct _reent *r, void *fd, struct stat *st)
{
    (void)r; (void)fd;
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR;
    return 0;
}
static const devoptab_t log_device = {
    .name = "mkdalog", .structSize = sizeof(int),
    .open_r = log_open, .close_r = log_close, .write_r = log_write, .fstat_r = log_stat,
};

void mkda_switch_flush_log(void)
{
    if (log_fd >= 0 && mutexTryLock(&log_lock)) {
        drain_locked();
        mutexUnlock(&log_lock);
        fsync(log_fd);
    }
}

static void *flush_worker(void *unused)
{
    (void)unused;
    while (!atomic_load(&stopping)) {
        svcSleepThread(50000000ull);
        static unsigned ticks;
        if (++ticks % 10 == 0) {
            mutexLock(&log_lock);
            drain_locked();
            mutexUnlock(&log_lock);
        }
    }
    return NULL;
}

static void shutdown_log(void)
{
    fflush(stdout); fflush(stderr);
    atomic_store(&stopping, true);
    if (thread_started) pthread_join(log_thread, NULL);
    thread_started = 0;
    mkda_switch_flush_log();
    if (log_fd >= 0) close(log_fd);
    log_fd = -1;
    /* libc can still flush its streams after atexit. Keep their device/buffer
     * valid until process teardown; later writes are safely discarded. */
}

const char *xbox_platform_shader_cache_dir(void)
{
    return MKDA_SWITCH_DIR "/";
}

void mkda_switch_shutdown_sdl(void) { SDL_Quit(); }

void xbox_platform_abort(unsigned code)
{
    fprintf(stderr, "[MKDA] Terminating process after unsafe exit (%u)\n", code);
    mkda_switch_flush_log();
    /* A failed worker join must not return to hbloader and unload live code. */
    svcExitProcess();
    __builtin_unreachable();
}

static int directory(const char *path)
{
    if (mkdir(path, 0777) == 0) return 1;
    struct stat st;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void load_settings(void)
{
    FILE *file = fopen(MKDA_SWITCH_DIR "/mkdax_env.txt", "r");
    if (!file) return;
    char line[1024];
    while (fgets(line, sizeof line, file)) {
        char *start = line, *end, *value;
        while (*start == ' ' || *start == '\t') ++start;
        if (*start == '#' || !*start) continue;
        value = strchr(start, '=');
        if (!value) continue;
        end = value++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
        *end = 0;
        while (*value == ' ' || *value == '\t') ++value;
        end = value + strlen(value);
        while (end > value && (end[-1] == '\r' || end[-1] == '\n' ||
                              end[-1] == ' ' || end[-1] == '\t')) --end;
        *end = 0;
        if (*start) setenv(start, value, 1);
    }
    fclose(file);
}

int mkda_switch_bootstrap(void)
{
    if (!directory("sdmc:/switch") || !directory(MKDA_SWITCH_DIR) ||
        !directory(MKDA_SWITCH_SAVES)) return 0;
    started = armGetSystemTick();
    log_buffer = malloc(LOG_CAP);
    if (!log_buffer) return 0;
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (access(MKDA_SWITCH_DIR "/mkdax_log.txt", F_OK) == 0) {
        remove(MKDA_SWITCH_DIR "/mkdax_previous.log");
        if (rename(MKDA_SWITCH_DIR "/mkdax_log.txt",
                   MKDA_SWITCH_DIR "/mkdax_previous.log") != 0)
            flags = O_WRONLY | O_CREAT | O_APPEND;
    }
    log_fd = open(MKDA_SWITCH_DIR "/mkdax_log.txt", flags, 0644);
    if (log_fd < 0 || AddDevice(&log_device) < 0) {
        if (log_fd >= 0) close(log_fd);
        log_fd = -1;
        free(log_buffer); log_buffer = NULL;
        return 0;
    }
    if (!freopen("mkdalog:", "w", stdout)) {
        RemoveDevice("mkdalog:");
        close(log_fd); log_fd = -1;
        free(log_buffer); log_buffer = NULL;
        return 0;
    }
    if (dup2(fileno(stdout), fileno(stderr)) < 0)
        fprintf(stdout, "[MKDA] stderr redirection unavailable\n");
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    load_settings();
    /* The shared GL backend otherwise defaults to Conker's cache directory. */
    setenv("RECOMP_PROG_CACHE", MKDA_SWITCH_DIR "/progcache.bin", 0);
    const char *logging = getenv("MKDA_LOG");
    log_disabled = logging && !strcmp(logging, "0");
    if (atexit(shutdown_log) != 0) synchronous = 1;
    else if (!log_disabled) {
        thread_started = pthread_create(&log_thread, NULL, flush_worker, NULL) == 0;
        synchronous = !thread_started;
    }
    return 1;
}

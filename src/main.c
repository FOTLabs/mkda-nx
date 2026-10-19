#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <SDL.h>
#if !defined(_WIN32)
#include <pthread.h>
#endif
#ifdef __linux__
#include <dirent.h>
#endif
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "nv2a_gl.h"
#ifdef __SWITCH__
#include "switch_bootstrap.h"
#define MKDA_DEFAULT_ASSETS MKDA_SWITCH_ASSETS
#define MKDA_DEFAULT_SAVES MKDA_SWITCH_SAVES
#define MKDA_HOST "Switch"
#else
#define MKDA_DEFAULT_ASSETS "game"
#define MKDA_DEFAULT_SAVES "saves"
#define MKDA_HOST "Linux"
#endif

extern RECOMP_TLS uint32_t g_esp;
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);
extern void xbox_path_init(const char *, const char *);
extern void mkda_install_diagnostics(void);
extern int nv2a_renderer_replay(void);
extern void mkda_task_on_longjmp(uint32_t guest_buffer);
extern int mkda_devices_init(void);
extern void mkda_devices_shutdown(void);
extern int xbox_BridgeShutdown(void);
extern void mkda_start_native_sampler(void);
extern void xbox_gil_enter(void), xbox_gil_leave(void), xbox_gil_mark_main(void);
extern void xbox_guest_pin(int interrupt);
static int memory_started, renderer_started, kernel_started, bridge_started;

static int audit_threads(void)
{
#ifdef __linux__
    if (getenv("MKDA_THREAD_AUDIT")) {
        DIR *directory = opendir("/proc/self/task");
        if (!directory) return 0;
        unsigned count = 0;
        struct dirent *entry;
        while ((entry = readdir(directory)))
            if (entry->d_name[0] != '.') ++count;
        closedir(directory);
        fprintf(stderr, "[MKDA] Live threads after shutdown: %u\n", count);
        return count == 1;
    }
#endif
    return 1;
}

static int shutdown_runtime(void)
{
    if (bridge_started && !xbox_BridgeShutdown()) return 0;
    bridge_started = 0;
    if (memory_started && !xbox_MemoryLayoutStopWorkers(5000)) return 0;
    mkda_devices_shutdown();
    if (renderer_started && !nv2a_gl_shutdown()) return 0;
    renderer_started = 0;
    if (memory_started) xbox_MemoryLayoutShutdown();
    memory_started = 0;
    if (kernel_started) xbox_kernel_shutdown();
    kernel_started = 0;
    return 1;
}

static int game_main(int argc, char **argv)
{
    const char *assets = argc > 1 ? argv[1] : MKDA_DEFAULT_ASSETS;
    const char *saves = argc > 2 ? argv[2] : MKDA_DEFAULT_SAVES;
    setenv("RECOMP_PB_EXEC", "1", 0);
    setenv("RECOMP_VBLANK", "1", 0);
    setenv("RECOMP_AC97_READY", "plain", 0);
    setenv("RECOMP_USB", "1", 0);
    setenv("RECOMP_USB_NDP", "4", 0);
    setenv("RECOMP_USB_PORT", "2", 0);
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/default.xbe", assets) >= sizeof(path))
        return 1;
#ifndef __SWITCH__
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
    mkda_install_diagnostics();
    recomp_set_longjmp_observer(mkda_task_on_longjmp);
    fprintf(stderr, "[MKDA] %s bootstrap; entry 0x0015F39E\n", MKDA_HOST);
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long size = ftell(file);
    rewind(file);
    void *data = size > 0 ? malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data); fclose(file); return 1;
    }
    fclose(file);
    memory_started = 1;
    if (!xbox_MemoryLayoutInit(data, (size_t)size)) { free(data); return 1; }
    free(data);
    if (!mkda_devices_init()) return 1;
    nv2a_gl_install();
    renderer_started = 1;
    kernel_started = 1;
    xbox_kernel_init();
    xbox_path_init(assets, saves);
    bridge_started = 1;
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[MKDA] Using binary-search dispatch\n");
    xbox_WatchdogStart();
    if (getenv("RECOMP_REPLAY")) return nv2a_renderer_replay();
    mkda_start_native_sampler();
    fprintf(stderr, "[MKDA] Entering guest, esp=%08X\n", g_esp);
    xbox_guest_pin(0);
    xbox_gil_mark_main();
    xbox_gil_enter();
    xbe_entry_point();
    xbox_gil_leave();
    fprintf(stderr, "[MKDA] Guest returned\n");
    return 0;
}

#if !defined(_WIN32)
typedef struct { int argc; char **argv; int result; } GameArguments;
static pthread_t game_owner;
static GameArguments *game_arguments;
static _Noreturn void abort_runtime(unsigned code);
extern int xbox_gil_suspend(void);
extern RECOMP_TLS int g_guest_interrupts_enabled;

/* ExitProcess from the game leaves teardown to main's original stack.
 * Other worker failures terminate the process rather than unloading live code. */
void xbox_platform_exit(unsigned code)
{
    if (!game_arguments || !pthread_equal(pthread_self(), game_owner) ||
        !g_guest_interrupts_enabled) abort_runtime(code);
    game_arguments->result = (int)code;
    if (!xbox_WatchdogStop(2000)) abort_runtime(code);
    if (!nv2a_gl_detach_context()) abort_runtime(code);
    xbox_gil_suspend();
    pthread_exit(NULL);
    __builtin_unreachable();
}
static _Noreturn void abort_runtime(unsigned code)
{
#ifdef __SWITCH__
    xbox_platform_abort(code);
#else
    fflush(stderr);
    _Exit((int)code);
#endif
}
static void *game_thread(void *argument)
{
#ifdef __SWITCH__
    extern void xbox_nx_set_el0_tp(void);
    xbox_nx_set_el0_tp();
#endif
    GameArguments *args = argument;
    game_owner = pthread_self();
    game_arguments = args;
    args->result = game_main(args->argc, args->argv);
    if (!xbox_WatchdogStop(2000)) abort_runtime(1);
    if (!nv2a_gl_detach_context()) abort_runtime(1);
    return NULL;
}
static int run_game_thread(int argc, char **argv)
{
    /* Buffy uses this stack size for the lifted CRT and game call chains.
     * Cooperative tasks have their own separate native stacks. */
    pthread_attr_t attributes;
    pthread_t thread;
    GameArguments args = { argc, argv, 1 };
    int error = pthread_attr_init(&attributes);
    if (error) return 1;
    error = pthread_attr_setstacksize(&attributes, 16u * 1024u * 1024u);
    if (!error) error = pthread_create(&thread, &attributes, game_thread, &args);
    pthread_attr_destroy(&attributes);
    if (error) {
        fprintf(stderr, "[MKDA] Cannot start 16 MB game stack: %d\n", error);
        return 1;
    }
    error = pthread_join(thread, NULL);
    if (error) {
        fprintf(stderr, "[MKDA] Cannot join game thread: %d\n", error);
        abort_runtime(1);
    }
    if (!shutdown_runtime()) abort_runtime(1);
    game_arguments = NULL;
#ifdef __SWITCH__
    mkda_switch_shutdown_sdl();
    extern void xbox_nx_release_all(void);
    xbox_nx_release_all();
#else
    SDL_Quit();
#endif
    if (!audit_threads()) return 1;
    return args.result;
}
#endif
#ifdef __SWITCH__
int main(int argc, char **argv)
{
    if (!mkda_switch_bootstrap()) return 1;
    return run_game_thread(argc, argv);
}
#else
int main(int argc, char **argv)
{
#ifndef _WIN32
    const char *threaded = getenv("MKDA_GAME_THREAD");
    if (threaded && threaded[0] == '1') return run_game_thread(argc, argv);
#endif
    int result = game_main(argc, argv);
    if (!shutdown_runtime()) return 1;
    SDL_Quit();
    if (!audit_threads()) return 1;
    return result;
}
#endif

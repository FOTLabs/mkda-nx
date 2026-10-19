#ifndef MKDA_SWITCH_BOOTSTRAP_H
#define MKDA_SWITCH_BOOTSTRAP_H

#define MKDA_SWITCH_DIR "sdmc:/switch/mkdax"
#define MKDA_SWITCH_ASSETS MKDA_SWITCH_DIR "/game"
#define MKDA_SWITCH_SAVES MKDA_SWITCH_DIR "/save"

int mkda_switch_bootstrap(void);
void mkda_switch_flush_log(void);
void mkda_switch_shutdown_sdl(void);
void xbox_platform_abort(unsigned code) __attribute__((noreturn));

#endif

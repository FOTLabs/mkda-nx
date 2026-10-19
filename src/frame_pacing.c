#include <stdint.h>

/* One simulation tick per 60 Hz frame. Never run catch-up ticks after a hitch. */
uint64_t mkda_frame_wait(uint64_t *deadline, uint64_t now,
                         uint64_t frequency, int active)
{
    if (!active) { *deadline = 0; return 0; }
    uint64_t period = frequency / 60;
    if (!*deadline || now >= *deadline) {
        *deadline = now + period;
        return 0;
    }
    uint64_t wait = *deadline - now;
    *deadline += period;
    return wait;
}

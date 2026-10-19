#include <assert.h>
#include <stdint.h>
extern uint64_t mkda_frame_wait(uint64_t *, uint64_t, uint64_t, int);
int main(void)
{
    const uint64_t hz = 19200000, period = hz / 60;
    uint64_t deadline = 0, now = 1000;
    assert(mkda_frame_wait(&deadline, now, hz, 1) == 0);
    for (unsigned i = 0; i < 600; ++i) {
        uint64_t previous = now;
        now += hz / 200; /* renderer finishes in5ms */
        now += mkda_frame_wait(&deadline, now, hz, 1);
        assert(now - previous == period);
    }
    now += hz; /* long loading hitch must not cause a burst of catch-up frames */
    assert(mkda_frame_wait(&deadline, now, hz, 1) == 0);
    assert(deadline == now + period);
    assert(mkda_frame_wait(&deadline, now, hz, 0) == 0 && deadline == 0);
    assert(mkda_frame_wait(&deadline, now + hz, hz, 1) == 0);
    return 0;
}

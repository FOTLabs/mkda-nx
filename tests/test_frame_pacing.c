#include <assert.h>
#include <stdint.h>
extern uint64_t mkda_frame_wait(uint64_t *, uint64_t, uint64_t, int);
extern int mkda_frame_paced_context(uint32_t);
int main(void)
{
    const uint64_t hz = 19200000, period = hz / 60;
    for (uint32_t context = 0; context < 32; ++context)
        assert(mkda_frame_paced_context(context) == (context == 5 || context == 14));
    /* An ending's 180-frame wait must span three seconds even at 200fps.
     * Keep intro/menu contexts out of this simulation clock. */
    uint64_t ending_deadline = 0, ending_now = 1000, ending_start = ending_now;
    mkda_frame_wait(&ending_deadline, ending_now, hz, mkda_frame_paced_context(14));
    for (unsigned frame = 0; frame < 180; ++frame) {
        ending_now += hz / 200;
        ending_now += mkda_frame_wait(&ending_deadline, ending_now, hz,
                                      mkda_frame_paced_context(14));
    }
    assert(ending_now - ending_start == 3 * hz);
    assert(mkda_frame_wait(&ending_deadline, ending_now, hz,
                          mkda_frame_paced_context(2)) == 0 && ending_deadline == 0);
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

/* Xbox motor envelopes adapted to the Switch's paired HD Rumble actuators.
 * Keep event timing, with low body and high detail shaped independently. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static float s_strength = 0.55f;
static pthread_once_t s_rumble_once = PTHREAD_ONCE_INIT;

static float rumble_unit(float value)
{
    if (!isfinite(value) || value < 0.0f) return 0.0f;
    return value > 1.0f ? 1.0f : value;
}

static void rumble_map(float low, float high, float strength, unsigned side,
                       float *out_low, float *out_high,
                       float *low_hz, float *high_hz)
{
    low = rumble_unit(low); high = rumble_unit(high);
    strength = rumble_unit(strength);
    float sum_input = low + high;
    float texture = sum_input > 0.0f ? high / sum_input : 0.0f;
    /* A heavy-motor event has a rounded low carrier; a light-motor event
     * retains the crisp high resonance. Track the original motor balance,
     * not the update rate, so repeated identical reports stay identical. */
    *low_hz = sum_input > 0.0f ? 120.0f + 30.0f * texture : 160.0f;
    *high_hz = sum_input > 0.0f ? 250.0f + 70.0f * texture : 320.0f;
    /* A soft knee reduces peaks more than small cues. The high band feels
     * sharper on Joy-Cons, so give it less weight than the low motor. */
    low = low * (1.0f - 0.25f * low) * strength;
    high = high * (1.0f - 0.25f * high) * strength * 0.65f;
    /* Preserve the Xbox's left-heavy/right-fine character without dropping
     * either band from a grip. A single Joy-Con (side 2) gets both bands. */
    if (side == 0) high *= 0.55f;
    else if (side == 1) low *= 0.75f;
    float sum = low + high;
    if (sum > strength) {
        float scale = strength / sum;
        low *= scale; high *= scale;
    }
    *out_low = low; *out_high = high;
}

static void rumble_load(void)
{
    const char *value = getenv("MKDA_RUMBLE_STRENGTH");
    if (value && *value) {
        char *end;
        float parsed = strtof(value, &end);
        if (end != value && !*end && isfinite(parsed))
            s_strength = rumble_unit(parsed);
    }
    fprintf(stderr, "[rumble] HD profile, strength %.2f\n", s_strength);
}

void xbox_platform_rumble_filter(unsigned port, unsigned side,
                                float *low, float *high,
                                float *low_hz, float *high_hz)
{
    (void)port;
    pthread_once(&s_rumble_once, rumble_load);
    rumble_map(*low, *high, s_strength, side, low, high, low_hz, high_hz);
}

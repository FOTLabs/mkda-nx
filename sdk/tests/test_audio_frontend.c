/* gcc -O2 -ffunction-sections -fdata-sections -I src/apu $(sdl2-config --cflags)
 * tests/test_audio_frontend.c src/apu/apu_xaudio2.c -Wl,--gc-sections -lm -o /tmp/test_audio_frontend */
#include "apu_xaudio2.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    int16_t in, out;
    xa2_set_master_volume(0.5f);
    xa2_set_frontend(0);
    int base = xa2_queue_target();
    for (int i = -32768; i <= 32767; ++i) {
        in = (int16_t)i;
        apu_output_safety(&in, &out, 1);
        assert(out == (int16_t)(16384.0f * tanhf((float)in * 0.5f / 16384.0f)));
    }
    xa2_set_frontend(1);
    assert(xa2_queue_target() == (base < 24 ? 24 : base));
    int previous = -16384;
    for (int i = -32768; i <= 32767; ++i) {
        in = (int16_t)i;
        apu_output_safety(&in, &out, 1);
        assert(out >= -16384 && out <= 16384 && out >= previous);
        assert(out == (int16_t)(16384.0f * tanhf((float)in / 16384.0f)));
        previous = out;
    }
    xa2_set_frontend_boost(1.41421356f);
    in=512;apu_output_safety(&in,&out,1);
    assert(out==(int16_t)(16384.0f*tanhf(512.0f*1.414f/16384.0f)));
    xa2_set_frontend(0);
    /* The extra boost must also disappear for EVERY possible game sample. */
    for(int i=-32768;i<=32767;++i) {
        in=(int16_t)i;apu_output_safety(&in,&out,1);
        assert(out==(int16_t)(16384.0f*tanhf((float)in*0.5f/16384.0f)));
    }
    assert(xa2_queue_target() == base);
    puts("PASS: all 65536 gameplay samples unchanged; frontend gain bounded/monotonic; queue reserve restored");
}

#include <assert.h>
#include <stdint.h>
#include <string.h>
void xbox_platform_pad_filter(unsigned, uint16_t *, uint8_t[8], int16_t[4]);
int main(void)
{
    uint8_t analog[8] = {255, 1, 2, 3, 4, 5, 6, 7}, saved[8];
    memcpy(saved, analog, 8);
    int16_t thumbs[4] = {0, 0, 1111, -2222};
    uint16_t digital = 0x10;
    xbox_platform_pad_filter(0, &digital, analog, thumbs);
    assert(digital == 0x10);
    thumbs[0] = -32768; thumbs[1] = 32767;
    xbox_platform_pad_filter(1, &digital, analog, thumbs);
    assert(digital == 0x15); /* Start + up-left */
    assert(thumbs[0] == -32768 && thumbs[1] == 32767);
    assert(thumbs[2] == 1111 && thumbs[3] == -2222);
    assert(!memcmp(analog, saved, 8));
    digital = 0x40; thumbs[0] = 32767; thumbs[1] = -32768;
    xbox_platform_pad_filter(0, &digital, analog, thumbs);
    assert(digital == 0x4a); /* Stick click + down-right */
    digital = 1; thumbs[0] = 12000; thumbs[1] = -12000;
    xbox_platform_pad_filter(0, &digital, analog, thumbs);
    assert(digital == 1); /* Preserve real D-pad; neutral within dead zone. */
    return 0;
}

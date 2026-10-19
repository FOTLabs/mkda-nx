#include <stdint.h>

/* MKDA reads digital directions for movement. Keep the real D-pad and native
 * stick values, adding digital directions outside a generous stick dead zone.
 * Stateless mapping keeps both controller ports independent. */
void xbox_platform_pad_filter(unsigned port, uint16_t *digital,
                              uint8_t analog[8], int16_t thumbs[4])
{
    (void)port; (void)analog;
    const int threshold = 12000;
    if (thumbs[0] < -threshold) *digital |= 0x0004;
    if (thumbs[0] > threshold)  *digital |= 0x0008;
    if (thumbs[1] < -threshold) *digital |= 0x0002;
    if (thumbs[1] > threshold)  *digital |= 0x0001;
}

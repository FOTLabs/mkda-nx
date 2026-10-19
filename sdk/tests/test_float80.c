/* Standalone: cc -I templates/runtime tests/test_float80.c -lm -o /tmp/test_float80 */
#include "recomp_types.h"
#include <assert.h>
#include <float.h>
#include <string.h>

int main(void)
{
    const uint8_t half_pi[10] = {0x35,0xc2,0x68,0x21,0xa2,0xda,0x0f,0xc9,0xff,0x3f};
    assert(recomp_load_f80(half_pi) == acos(-1.0) / 2);
    const double values[] = {0.0, -0.0, 1.0, -1.0, DBL_MIN, DBL_MAX,
                            0x1p-1074, INFINITY, -INFINITY, NAN};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        uint8_t bytes[12]; memset(bytes, 0x5a, sizeof(bytes));
        recomp_store_f80(bytes + 1, values[i]);
        double result = recomp_load_f80(bytes + 1);
        assert(bytes[0] == 0x5a && bytes[11] == 0x5a);
        if (isnan(values[i])) assert(isnan(result));
        else assert(memcmp(&result, &values[i], sizeof(result)) == 0);
    }
    uint8_t invalid[10] = {1,0,0,0,0,0,0,0,1,0};
    assert(isnan(recomp_load_f80(invalid)));
    return 0;
}

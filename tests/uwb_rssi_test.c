#include "deca_rsl.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    const unsigned powers[] = {1, 8, 100, 1000, 1000000};
    const unsigned counts[] = {32, 128, 1024};
    unsigned cases = 0;
    for (unsigned c = 0; c < sizeof powers / sizeof powers[0]; ++c)
        for (unsigned n = 0; n < sizeof counts / sizeof counts[0]; ++n)
            for (unsigned d = 0; d <= 7; ++d) {
                double expected = 10.0 * log10((double)powers[c] * 2097152.0 /
                    ((double)counts[n] * counts[n])) + 6.0 * d - 121.7;
                if (expected < -120.0 || expected > 0.0) continue;
                double actual = rsl_calculate_signal_power(powers[c], 21, counts[n], d, 9, false) / 256.0;
                if (fabs(actual - expected) > 0.2) {
                    fprintf(stderr, "RSSI mismatch C=%u N=%u D=%u: %.3f vs %.3f\n",
                        powers[c], counts[n], d, actual, expected);
                    return 1;
                }
                cases++;
            }
    assert(rsl_calculate_signal_power(0, 21, 128, 0, 9, false) == SHRT_MIN);
    assert(rsl_calculate_signal_power(100, 21, 0, 0, 9, false) == SHRT_MIN);
    printf("UWB RSSI tests passed (%u comparisons against independent log10 formula).\n", cases);
}

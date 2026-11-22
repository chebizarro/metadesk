#include "rates.h"
#include "md_test.h"
#include <math.h>
#include <stdio.h>

static void check(MdClientRateValues rates, double fps, double mbps) {
    MD_CHECK(fabs(rates.fps - fps) < 0.000001);
    MD_CHECK(fabs(rates.mbps - mbps) < 0.000001);
}

int main(void) {
    MdClientRates s = { 0 };
    check(md_client_rates_update(&s, 100, 1000, 9000000), 0, 0);
    /* Same timestamp: don't fabricate an elapsed interval or discard deltas. */
    check(md_client_rates_update(&s, 100, 1010, 9100000), 0, 0);
    check(md_client_rates_update(&s, 1100, 1060, 10000000), 60, 8);

    /* Cumulative observer counters: 60 fps and 8 Mbps across repeated CLI
     * reporting boundaries (5s, 10s), with multiple renders per sample. */
    s = (MdClientRates){ 0 };
    for (uint32_t ms = 0; ms <= 12000; ms += 10) {
        MdClientRateValues rates = md_client_rates_update(&s, ms,
            1000 + ms * 60 / 1000, 9000000 + (uint64_t)ms * 1000);
        if (ms && ms % 100 == 0)
            check(rates, 60, 8);
        MD_CHECK(s.count <= MD_RATE_SAMPLES);
        check(md_client_rates_update(&s, ms,
            1000 + ms * 60 / 1000, 9000000 + (uint64_t)ms * 1000),
            rates.fps, rates.mbps);
    }
    /* A new rate replaces old traffic, rather than lifetime averaging. */
    for (uint32_t ms = 12100; ms <= 18000; ms += 100)
        md_client_rates_update(&s, ms,
            1720 + (ms - 12000) * 30 / 1000,
            21000000 + (uint64_t)(ms - 12000) * 500);
    check(md_client_rates_update(&s, 18000, 1900, 24000000), 30, 4);
    for (uint32_t ms = 18100; ms <= 23100; ms += 100)
        md_client_rates_update(&s, ms, 1900, 24000000);
    check(md_client_rates_update(&s, 23100, 1900, 24000000), 0, 0);
    /* Long idle gap has no usable sliding-window baseline. */
    check(md_client_rates_update(&s, 40000, 2000, 25000000), 0, 0);

    /* Reconnect: bytes restart but decoded frames remain cumulative. */
    check(md_client_rates_update(&s, 41000, 2000, 0), 0, 0);
    check(md_client_rates_update(&s, 42000, 2060, 1000000), 60, 8);
    /* Frame-counter restart must reset the byte/clock baseline too. */
    check(md_client_rates_update(&s, 43000, 0, 2000000), 0, 0);
    check(md_client_rates_update(&s, 44000, 30, 2500000), 30, 4);
    /* Explicit connection reset even if new counters exceed old counters. */
    s = (MdClientRates){ 0 };
    check(md_client_rates_update(&s, 45000, 3000, 3000000), 0, 0);
    check(md_client_rates_update(&s, 46000, 3060, 4000000), 60, 8);

    s = (MdClientRates){ 0 };
    check(md_client_rates_update(&s, UINT32_MAX - 499, 0, 0), 0, 0);
    check(md_client_rates_update(&s, 500, 60, 1000000), 60, 8);
    /* Difference before conversion preserves precision for large totals. */
    s = (MdClientRates){ 0 };
    check(md_client_rates_update(&s, 0, UINT64_MAX - 1000,
                                 UINT64_MAX - 2000000), 0, 0);
    check(md_client_rates_update(&s, 1000, UINT64_MAX - 940,
                                 UINT64_MAX - 1000000), 60, 8);
    return md_test_report();
}

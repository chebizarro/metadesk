/* Matching cumulative-counter samples over a trailing five-second window. */
#ifndef MD_CLIENT_RATES_H
#define MD_CLIENT_RATES_H

#include <stdbool.h>
#include <stdint.h>

#define MD_RATE_WINDOW_MS 5000u
#define MD_RATE_SAMPLE_MS 100u
#define MD_RATE_SAMPLES (MD_RATE_WINDOW_MS / MD_RATE_SAMPLE_MS + 2u)

typedef struct {
    uint32_t time_ms;
    uint64_t frames, bytes;
} MdRateSample;

typedef struct {
    MdRateSample samples[MD_RATE_SAMPLES];
    unsigned head, count;
    MdRateSample previous;
    bool initialized;
} MdClientRates;

typedef struct {
    double fps, mbps;
} MdClientRateValues;

/* Zero-initialize/reset state for each connection. now_ms is monotonic and
 * may wrap at UINT32_MAX. Counter regressions also establish a new baseline.
 * Rates use the same oldest sample still inside the window and the same
 * elapsed time. No elapsed time (or no recent baseline) yields zero rates. */
MdClientRateValues md_client_rates_update(MdClientRates *state, uint32_t now_ms,
                                         uint64_t frames, uint64_t bytes);
#endif

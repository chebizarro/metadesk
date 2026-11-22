#include "rates.h"

MdClientRateValues md_client_rates_update(MdClientRates *s, uint32_t now_ms,
                                         uint64_t frames, uint64_t bytes) {
    MdRateSample current = { now_ms, frames, bytes };
    MdClientRateValues rates = { 0 };
    if (!s->initialized || frames < s->previous.frames ||
        bytes < s->previous.bytes ||
        now_ms - s->previous.time_ms > INT32_MAX) {
        *s = (MdClientRates){ 0 };
        s->initialized = true;
    }
    s->previous = current;

    while (s->count &&
           now_ms - s->samples[s->head].time_ms > MD_RATE_WINDOW_MS) {
        s->head = (s->head + 1u) % MD_RATE_SAMPLES;
        s->count--;
    }
    if (s->count) {
        MdRateSample oldest = s->samples[s->head];
        uint32_t elapsed = now_ms - oldest.time_ms;
        if (elapsed) {
            rates.fps = (double)(frames - oldest.frames) * 1000.0 / elapsed;
            rates.mbps = (double)(bytes - oldest.bytes) * 0.008 / elapsed;
        }
    }

    unsigned next = (s->head + s->count) % MD_RATE_SAMPLES;
    unsigned last = (next + MD_RATE_SAMPLES - 1u) % MD_RATE_SAMPLES;
    if (!s->count || now_ms - s->samples[last].time_ms >= MD_RATE_SAMPLE_MS) {
        s->samples[next] = current;
        s->count++;
    }
    return rates;
}

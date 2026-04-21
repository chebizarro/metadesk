/*
 * metadesk — bitrate_ctrl.h
 * Adaptive bitrate controller driven by RTT feedback.
 *
 * Uses an AIMD-style algorithm (Additive Increase, Multiplicative
 * Decrease) with hysteresis to avoid oscillation:
 *
 *   avg_rtt > rtt_high  →  multiplicative decrease (×0.7)
 *   avg_rtt < rtt_low   →  additive increase (step toward max)
 *   otherwise           →  hold (dead zone / hysteresis band)
 *
 * The controller is a pure state machine — it has no side effects.
 * Caller feeds RTT samples and reads the recommended bitrate;
 * caller is responsible for actually calling md_encoder_set_bitrate().
 *
 * Typical usage in a streaming loop:
 *
 *   MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);
 *   ...
 *   // every ping/pong or periodic interval:
 *   uint32_t new_br = md_bitrate_ctrl_update(ctrl, avg_rtt_ms);
 *   if (new_br != current_br)
 *       md_encoder_set_bitrate(enc, new_br);
 */
#ifndef MD_BITRATE_CTRL_H
#define MD_BITRATE_CTRL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ───────────────────────────────────────────── */

typedef struct {
    uint32_t max_bitrate;       /* ceiling bitrate (bits/sec)           */
    uint32_t min_bitrate;       /* floor bitrate (bits/sec)             */
    uint32_t initial_bitrate;   /* starting bitrate (0 = max)           */

    uint32_t rtt_high_ms;       /* RTT above this → decrease (default 150) */
    uint32_t rtt_low_ms;        /* RTT below this → increase (default 50)  */

    /* Decrease: multiply current bitrate by (decrease_pct / 100).
     * Default 70 means 0.7× on congestion. */
    uint32_t decrease_pct;

    /* Increase: add this fraction of (max - current) per step.
     * Default 10 means add 10% of headroom each step. */
    uint32_t increase_pct;

    /* Minimum interval between bitrate changes (ms).
     * Prevents rapid oscillation. Default 1000. */
    uint32_t cooldown_ms;

    /* Number of consecutive low-RTT samples needed before increasing.
     * Default 3. Adds stability to the increase path. */
    uint32_t increase_threshold;
} MdBitrateCtrlConfig;

/* ── Default values ──────────────────────────────────────────── */

#define MD_BITRATE_CTRL_DEFAULT_RTT_HIGH     150
#define MD_BITRATE_CTRL_DEFAULT_RTT_LOW       50
#define MD_BITRATE_CTRL_DEFAULT_DECREASE_PCT  70
#define MD_BITRATE_CTRL_DEFAULT_INCREASE_PCT  10
#define MD_BITRATE_CTRL_DEFAULT_COOLDOWN_MS  1000
#define MD_BITRATE_CTRL_DEFAULT_INCREASE_THRESH 3

/* ── Controller state ────────────────────────────────────────── */

typedef enum {
    MD_BITRATE_HOLD,       /* in dead zone, no change                */
    MD_BITRATE_INCREASE,   /* RTT below low threshold, ramping up    */
    MD_BITRATE_DECREASE,   /* RTT above high threshold, backing off  */
} MdBitrateAction;

typedef struct MdBitrateCtrl MdBitrateCtrl;

/* ── Public API ──────────────────────────────────────────────── */

/* Create a bitrate controller. cfg fields that are 0 get defaults.
 * Returns NULL on allocation failure. */
MdBitrateCtrl *md_bitrate_ctrl_create(const MdBitrateCtrlConfig *cfg);

/*
 * Feed an RTT sample and get the recommended bitrate.
 *
 * avg_rtt_ms: current exponential moving average RTT (from MdStreamStats)
 * now_ms:     current timestamp (from md_stream_now_ms())
 *
 * Returns the recommended bitrate in bits/sec.
 * This may be the same as the previous call if no change is needed.
 */
uint32_t md_bitrate_ctrl_update(MdBitrateCtrl *ctrl,
                                uint32_t avg_rtt_ms,
                                uint32_t now_ms);

/* Query the current recommended bitrate without feeding a new sample. */
uint32_t md_bitrate_ctrl_get_bitrate(const MdBitrateCtrl *ctrl);

/* Query what action the controller last took. */
MdBitrateAction md_bitrate_ctrl_get_action(const MdBitrateCtrl *ctrl);

/* Reset controller state (e.g., on reconnection). */
void md_bitrate_ctrl_reset(MdBitrateCtrl *ctrl);

/* Destroy the controller. */
void md_bitrate_ctrl_destroy(MdBitrateCtrl *ctrl);

#ifdef __cplusplus
}
#endif

#endif /* MD_BITRATE_CTRL_H */

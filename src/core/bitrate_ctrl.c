/*
 * metadesk — bitrate_ctrl.c
 * Adaptive bitrate controller — AIMD with hysteresis.
 *
 * The algorithm is intentionally simple and deterministic:
 *
 *   1. If RTT > rtt_high → multiplicative decrease (emergency)
 *      new_br = current * decrease_pct / 100
 *      Decrease is immediate — no waiting for consecutive samples.
 *
 *   2. If RTT < rtt_low for N consecutive samples → additive increase
 *      headroom = max - current
 *      step = headroom * increase_pct / 100
 *      new_br = current + max(step, 1)
 *
 *   3. Otherwise → hold (hysteresis band between low and high)
 *      No change to bitrate.
 *
 *   4. Cooldown: no change allowed within cooldown_ms of last change.
 *      Exception: decrease always fires (safety over smoothness).
 *
 * This produces a sawtooth pattern under congestion: rapid drops
 * followed by slow recovery, which is well-understood and robust.
 */
#include "bitrate_ctrl.h"

#include <stdlib.h>
#include <string.h>

struct MdBitrateCtrl {
    MdBitrateCtrlConfig cfg;
    uint32_t            current_bitrate;
    uint32_t            last_change_ms;     /* timestamp of last bitrate change */
    uint32_t            consecutive_low;    /* consecutive samples below rtt_low */
    MdBitrateAction     last_action;
};

/* ── Helpers ─────────────────────────────────────────────────── */

static void apply_defaults(MdBitrateCtrlConfig *cfg) {
    if (cfg->rtt_high_ms == 0)
        cfg->rtt_high_ms = MD_BITRATE_CTRL_DEFAULT_RTT_HIGH;
    if (cfg->rtt_low_ms == 0)
        cfg->rtt_low_ms = MD_BITRATE_CTRL_DEFAULT_RTT_LOW;
    if (cfg->decrease_pct == 0)
        cfg->decrease_pct = MD_BITRATE_CTRL_DEFAULT_DECREASE_PCT;
    if (cfg->increase_pct == 0)
        cfg->increase_pct = MD_BITRATE_CTRL_DEFAULT_INCREASE_PCT;
    if (cfg->cooldown_ms == 0)
        cfg->cooldown_ms = MD_BITRATE_CTRL_DEFAULT_COOLDOWN_MS;
    if (cfg->increase_threshold == 0)
        cfg->increase_threshold = MD_BITRATE_CTRL_DEFAULT_INCREASE_THRESH;

    /* Ensure sane thresholds */
    if (cfg->rtt_low_ms >= cfg->rtt_high_ms)
        cfg->rtt_low_ms = cfg->rtt_high_ms / 2;

    /* Ensure decrease_pct is < 100 (otherwise it's not a decrease) */
    if (cfg->decrease_pct >= 100)
        cfg->decrease_pct = 70;
}

static uint32_t clamp(uint32_t val, uint32_t lo, uint32_t hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

/* ── Public API ──────────────────────────────────────────────── */

MdBitrateCtrl *md_bitrate_ctrl_create(const MdBitrateCtrlConfig *cfg) {
    if (!cfg) return NULL;

    MdBitrateCtrl *ctrl = calloc(1, sizeof(MdBitrateCtrl));
    if (!ctrl) return NULL;

    ctrl->cfg = *cfg;
    apply_defaults(&ctrl->cfg);

    /* Ensure min < max */
    if (ctrl->cfg.min_bitrate == 0)
        ctrl->cfg.min_bitrate = 100000;  /* 100 Kbps */
    if (ctrl->cfg.max_bitrate == 0)
        ctrl->cfg.max_bitrate = 8000000; /* 8 Mbps */
    if (ctrl->cfg.min_bitrate >= ctrl->cfg.max_bitrate)
        ctrl->cfg.min_bitrate = ctrl->cfg.max_bitrate / 2;

    /* Initial bitrate */
    ctrl->current_bitrate = ctrl->cfg.initial_bitrate
        ? clamp(ctrl->cfg.initial_bitrate,
                ctrl->cfg.min_bitrate, ctrl->cfg.max_bitrate)
        : ctrl->cfg.max_bitrate;

    ctrl->last_action = MD_BITRATE_HOLD;
    return ctrl;
}

uint32_t md_bitrate_ctrl_update(MdBitrateCtrl *ctrl,
                                uint32_t avg_rtt_ms,
                                uint32_t now_ms) {
    if (!ctrl) return 0;

    uint32_t br = ctrl->current_bitrate;
    uint32_t elapsed = now_ms - ctrl->last_change_ms;

    /* ── Decrease path (always fires, ignores cooldown) ──────── */
    if (avg_rtt_ms > ctrl->cfg.rtt_high_ms) {
        ctrl->consecutive_low = 0;

        uint32_t new_br = (uint32_t)((uint64_t)br * ctrl->cfg.decrease_pct / 100);
        new_br = clamp(new_br, ctrl->cfg.min_bitrate, ctrl->cfg.max_bitrate);

        if (new_br < br) {
            ctrl->current_bitrate = new_br;
            ctrl->last_change_ms = now_ms;
            ctrl->last_action = MD_BITRATE_DECREASE;
            return new_br;
        }
        /* Already at min — hold */
        ctrl->last_action = MD_BITRATE_HOLD;
        return br;
    }

    /* ── Increase path (requires cooldown + consecutive samples) ─ */
    if (avg_rtt_ms < ctrl->cfg.rtt_low_ms) {
        ctrl->consecutive_low++;

        if (ctrl->consecutive_low >= ctrl->cfg.increase_threshold
            && elapsed >= ctrl->cfg.cooldown_ms
            && br < ctrl->cfg.max_bitrate) {

            uint32_t headroom = ctrl->cfg.max_bitrate - br;
            uint32_t step = headroom * ctrl->cfg.increase_pct / 100;
            if (step == 0) step = 1;  /* always make progress */

            uint32_t new_br = br + step;
            new_br = clamp(new_br, ctrl->cfg.min_bitrate, ctrl->cfg.max_bitrate);

            ctrl->current_bitrate = new_br;
            ctrl->last_change_ms = now_ms;
            ctrl->consecutive_low = 0;
            ctrl->last_action = MD_BITRATE_INCREASE;
            return new_br;
        }

        /* Not enough consecutive samples or in cooldown — hold */
        ctrl->last_action = MD_BITRATE_HOLD;
        return br;
    }

    /* ── Dead zone (hysteresis band) ─────────────────────────── */
    ctrl->consecutive_low = 0;
    ctrl->last_action = MD_BITRATE_HOLD;
    return br;
}

uint32_t md_bitrate_ctrl_get_bitrate(const MdBitrateCtrl *ctrl) {
    return ctrl ? ctrl->current_bitrate : 0;
}

MdBitrateAction md_bitrate_ctrl_get_action(const MdBitrateCtrl *ctrl) {
    return ctrl ? ctrl->last_action : MD_BITRATE_HOLD;
}

void md_bitrate_ctrl_reset(MdBitrateCtrl *ctrl) {
    if (!ctrl) return;
    ctrl->current_bitrate = ctrl->cfg.initial_bitrate
        ? clamp(ctrl->cfg.initial_bitrate,
                ctrl->cfg.min_bitrate, ctrl->cfg.max_bitrate)
        : ctrl->cfg.max_bitrate;
    ctrl->last_change_ms = 0;
    ctrl->consecutive_low = 0;
    ctrl->last_action = MD_BITRATE_HOLD;
}

void md_bitrate_ctrl_destroy(MdBitrateCtrl *ctrl) {
    free(ctrl);
}

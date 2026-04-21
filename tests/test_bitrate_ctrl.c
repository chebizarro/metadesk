/*
 * test_bitrate_ctrl.c — Unit tests for the adaptive bitrate controller.
 *
 * Tests the AIMD algorithm: decrease on high RTT, increase on sustained
 * low RTT, hold in the hysteresis band, cooldown enforcement, and
 * edge cases like clamping and reset.
 */
#include "bitrate_ctrl.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do {                                       \
    tests_run++;                                                \
    printf("  [%2d] %-50s ", tests_run, #fn);                   \
    fn();                                                       \
    tests_passed++;                                             \
    printf("PASS\n");                                           \
} while (0)

/* Helper: create controller with standard test config */
static MdBitrateCtrl *make_ctrl(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 8000000,      /* 8 Mbps  */
        .min_bitrate = 500000,       /* 500 Kbps */
        .initial_bitrate = 8000000,
        .rtt_high_ms = 150,
        .rtt_low_ms = 50,
        .decrease_pct = 70,
        .increase_pct = 10,
        .cooldown_ms = 1000,
        .increase_threshold = 3,
    };
    return md_bitrate_ctrl_create(&cfg);
}

/* ── Creation and defaults ───────────────────────────────────── */

static void test_create_basic(void) {
    MdBitrateCtrl *ctrl = make_ctrl();
    assert(ctrl != NULL);
    assert(md_bitrate_ctrl_get_bitrate(ctrl) == 8000000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_HOLD);
    md_bitrate_ctrl_destroy(ctrl);
}

static void test_create_null(void) {
    assert(md_bitrate_ctrl_create(NULL) == NULL);
}

static void test_create_defaults(void) {
    /* All zeros → should get sensible defaults */
    MdBitrateCtrlConfig cfg = {0};
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);
    assert(ctrl != NULL);
    assert(md_bitrate_ctrl_get_bitrate(ctrl) > 0);
    md_bitrate_ctrl_destroy(ctrl);
}

static void test_create_initial_bitrate(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 10000000,
        .min_bitrate = 100000,
        .initial_bitrate = 5000000,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);
    assert(ctrl != NULL);
    assert(md_bitrate_ctrl_get_bitrate(ctrl) == 5000000);
    md_bitrate_ctrl_destroy(ctrl);
}

static void test_create_initial_clamped(void) {
    /* Initial above max → clamped to max */
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 5000000,
        .min_bitrate = 100000,
        .initial_bitrate = 99000000,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);
    assert(ctrl != NULL);
    assert(md_bitrate_ctrl_get_bitrate(ctrl) == 5000000);
    md_bitrate_ctrl_destroy(ctrl);
}

/* ── Decrease on high RTT ────────────────────────────────────── */

static void test_decrease_on_high_rtt(void) {
    MdBitrateCtrl *ctrl = make_ctrl();
    /* RTT = 200ms > 150ms threshold */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 200, 1000);
    /* 8M * 0.7 = 5.6M */
    assert(br == 5600000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_DECREASE);
    md_bitrate_ctrl_destroy(ctrl);
}

static void test_decrease_cascading(void) {
    MdBitrateCtrl *ctrl = make_ctrl();
    /* Multiple high-RTT samples → keep decreasing */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 200, 1000);
    assert(br == 5600000);
    br = md_bitrate_ctrl_update(ctrl, 200, 1100);
    assert(br == 3920000);  /* 5.6M * 0.7 */
    br = md_bitrate_ctrl_update(ctrl, 200, 1200);
    assert(br == 2744000);  /* 3.92M * 0.7 */
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_DECREASE);
    md_bitrate_ctrl_destroy(ctrl);
}

static void test_decrease_floor(void) {
    MdBitrateCtrl *ctrl = make_ctrl();
    /* Drive bitrate down to floor */
    uint32_t br = 8000000;
    for (int i = 0; i < 50; i++) {
        br = md_bitrate_ctrl_update(ctrl, 500, (uint32_t)(1000 + i * 100));
    }
    assert(br == 500000);  /* clamped to min */
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_HOLD);
    md_bitrate_ctrl_destroy(ctrl);
}

static void test_decrease_ignores_cooldown(void) {
    MdBitrateCtrl *ctrl = make_ctrl();
    /* Two decreases in quick succession (< cooldown) — both should fire */
    uint32_t br1 = md_bitrate_ctrl_update(ctrl, 200, 1000);
    uint32_t br2 = md_bitrate_ctrl_update(ctrl, 200, 1001); /* 1ms later */
    assert(br2 < br1);
    md_bitrate_ctrl_destroy(ctrl);
}

/* ── Increase on low RTT ─────────────────────────────────────── */

static void test_increase_after_threshold(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 8000000,
        .min_bitrate = 500000,
        .initial_bitrate = 4000000,
        .rtt_high_ms = 150,
        .rtt_low_ms = 50,
        .decrease_pct = 70,
        .increase_pct = 10,
        .cooldown_ms = 1000,
        .increase_threshold = 3,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);
    assert(ctrl != NULL);

    /* First two low-RTT samples → hold (need 3 consecutive) */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 20, 2000);
    assert(br == 4000000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_HOLD);

    br = md_bitrate_ctrl_update(ctrl, 20, 2500);
    assert(br == 4000000);

    /* Third sample + past cooldown → increase */
    br = md_bitrate_ctrl_update(ctrl, 20, 3500);
    /* headroom = 8M - 4M = 4M, step = 4M * 0.1 = 400K */
    assert(br == 4400000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_INCREASE);

    md_bitrate_ctrl_destroy(ctrl);
}

static void test_increase_respects_cooldown(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 8000000,
        .min_bitrate = 500000,
        .initial_bitrate = 4000000,
        .rtt_high_ms = 150,
        .rtt_low_ms = 50,
        .cooldown_ms = 2000,
        .increase_threshold = 1,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);

    /* First update → increase (threshold=1, time=0 so elapsed >= cooldown
     * since last_change_ms starts at 0) */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 20, 5000);
    assert(br > 4000000);  /* increased */

    /* Immediate second update → should hold (cooldown not elapsed) */
    uint32_t br2 = md_bitrate_ctrl_update(ctrl, 20, 5100);
    assert(br2 == br);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_HOLD);

    md_bitrate_ctrl_destroy(ctrl);
}

static void test_increase_to_ceiling(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 8000000,
        .min_bitrate = 500000,
        .initial_bitrate = 7900000,
        .rtt_high_ms = 150,
        .rtt_low_ms = 50,
        .increase_pct = 50,
        .cooldown_ms = 100,
        .increase_threshold = 1,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);

    /* headroom = 100K, step = 50K → 7.95M */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 10, 1000);
    assert(br == 7950000);

    /* Next step → headroom = 50K, step = 25K → 7.975M */
    br = md_bitrate_ctrl_update(ctrl, 10, 2000);
    assert(br == 7975000);

    /* Keep going until we hit max */
    for (int i = 0; i < 20; i++) {
        br = md_bitrate_ctrl_update(ctrl, 10, (uint32_t)(3000 + i * 200));
    }
    assert(br == 8000000);

    /* At max, should hold */
    br = md_bitrate_ctrl_update(ctrl, 10, 20000);
    assert(br == 8000000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_HOLD);

    md_bitrate_ctrl_destroy(ctrl);
}

/* ── Hysteresis band ─────────────────────────────────────────── */

static void test_hold_in_dead_zone(void) {
    MdBitrateCtrl *ctrl = make_ctrl();
    /* RTT = 100ms → between 50 and 150 → hold */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 100, 1000);
    assert(br == 8000000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_HOLD);

    br = md_bitrate_ctrl_update(ctrl, 80, 2000);
    assert(br == 8000000);

    br = md_bitrate_ctrl_update(ctrl, 120, 3000);
    assert(br == 8000000);

    md_bitrate_ctrl_destroy(ctrl);
}

static void test_hold_at_boundary(void) {
    MdBitrateCtrl *ctrl = make_ctrl();
    /* RTT exactly at rtt_high → not above, so hold */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 150, 1000);
    assert(br == 8000000);

    /* RTT exactly at rtt_low → not below, so hold (deadzone includes boundaries) */
    br = md_bitrate_ctrl_update(ctrl, 50, 2000);
    assert(br == 8000000);

    md_bitrate_ctrl_destroy(ctrl);
}

/* ── Consecutive sample tracking ─────────────────────────────── */

static void test_consecutive_reset_on_dead_zone(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 8000000,
        .min_bitrate = 500000,
        .initial_bitrate = 4000000,
        .rtt_high_ms = 150,
        .rtt_low_ms = 50,
        .cooldown_ms = 100,
        .increase_threshold = 3,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);

    /* Two low samples */
    md_bitrate_ctrl_update(ctrl, 20, 1000);
    md_bitrate_ctrl_update(ctrl, 20, 1200);

    /* One dead-zone sample resets consecutive counter */
    md_bitrate_ctrl_update(ctrl, 80, 1400);

    /* Two more low samples → still not 3 consecutive since reset */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 20, 1600);
    assert(br == 4000000);
    br = md_bitrate_ctrl_update(ctrl, 20, 1800);
    assert(br == 4000000);

    /* Third consecutive → now increase */
    br = md_bitrate_ctrl_update(ctrl, 20, 2000);
    assert(br > 4000000);

    md_bitrate_ctrl_destroy(ctrl);
}

static void test_consecutive_reset_on_high_rtt(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 8000000,
        .min_bitrate = 500000,
        .initial_bitrate = 4000000,
        .rtt_high_ms = 150,
        .rtt_low_ms = 50,
        .cooldown_ms = 100,
        .increase_threshold = 3,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);

    /* Two low samples */
    md_bitrate_ctrl_update(ctrl, 20, 1000);
    md_bitrate_ctrl_update(ctrl, 20, 1200);

    /* High RTT resets counter AND decreases */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 200, 1400);
    assert(br < 4000000);

    md_bitrate_ctrl_destroy(ctrl);
}

/* ── AIMD sawtooth pattern ───────────────────────────────────── */

static void test_sawtooth_pattern(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 8000000,
        .min_bitrate = 500000,
        .initial_bitrate = 8000000,
        .rtt_high_ms = 150,
        .rtt_low_ms = 50,
        .decrease_pct = 70,
        .increase_pct = 10,
        .cooldown_ms = 500,
        .increase_threshold = 2,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);

    /* Simulate: congestion spike → decrease → recovery → increase */
    uint32_t t = 1000;
    uint32_t br;

    /* Phase 1: congestion → decrease */
    br = md_bitrate_ctrl_update(ctrl, 300, t);
    assert(br == 5600000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_DECREASE);

    /* Phase 2: congestion clears → dead zone for a while */
    t += 500;
    br = md_bitrate_ctrl_update(ctrl, 80, t);
    assert(br == 5600000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_HOLD);

    /* Phase 3: RTT drops low → recovery ramp */
    t += 600;
    br = md_bitrate_ctrl_update(ctrl, 30, t); /* consecutive_low = 1 */
    assert(br == 5600000);

    t += 600;
    br = md_bitrate_ctrl_update(ctrl, 30, t); /* consecutive_low = 2 → increase */
    assert(br > 5600000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_INCREASE);

    uint32_t recovered = br;

    /* Phase 4: another spike → decrease again */
    t += 100;
    br = md_bitrate_ctrl_update(ctrl, 250, t);
    assert(br < recovered);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_DECREASE);

    md_bitrate_ctrl_destroy(ctrl);
}

/* ── Reset ───────────────────────────────────────────────────── */

static void test_reset(void) {
    MdBitrateCtrl *ctrl = make_ctrl();

    /* Decrease bitrate */
    md_bitrate_ctrl_update(ctrl, 200, 1000);
    assert(md_bitrate_ctrl_get_bitrate(ctrl) < 8000000);

    /* Reset → back to initial */
    md_bitrate_ctrl_reset(ctrl);
    assert(md_bitrate_ctrl_get_bitrate(ctrl) == 8000000);
    assert(md_bitrate_ctrl_get_action(ctrl) == MD_BITRATE_HOLD);

    md_bitrate_ctrl_destroy(ctrl);
}

/* ── Null safety ─────────────────────────────────────────────── */

static void test_null_safety(void) {
    assert(md_bitrate_ctrl_get_bitrate(NULL) == 0);
    assert(md_bitrate_ctrl_get_action(NULL) == MD_BITRATE_HOLD);
    assert(md_bitrate_ctrl_update(NULL, 100, 1000) == 0);
    md_bitrate_ctrl_reset(NULL);   /* should not crash */
    md_bitrate_ctrl_destroy(NULL); /* should not crash */
}

/* ── Zero RTT ────────────────────────────────────────────────── */

static void test_zero_rtt(void) {
    MdBitrateCtrlConfig cfg = {
        .max_bitrate = 8000000,
        .min_bitrate = 500000,
        .initial_bitrate = 4000000,
        .cooldown_ms = 100,
        .increase_threshold = 1,
    };
    MdBitrateCtrl *ctrl = md_bitrate_ctrl_create(&cfg);

    /* RTT = 0 → below rtt_low → should increase */
    uint32_t br = md_bitrate_ctrl_update(ctrl, 0, 5000);
    assert(br > 4000000);

    md_bitrate_ctrl_destroy(ctrl);
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("bitrate controller tests\n");
    printf("==============================\n\n");

    /* Creation */
    RUN_TEST(test_create_basic);
    RUN_TEST(test_create_null);
    RUN_TEST(test_create_defaults);
    RUN_TEST(test_create_initial_bitrate);
    RUN_TEST(test_create_initial_clamped);

    /* Decrease */
    RUN_TEST(test_decrease_on_high_rtt);
    RUN_TEST(test_decrease_cascading);
    RUN_TEST(test_decrease_floor);
    RUN_TEST(test_decrease_ignores_cooldown);

    /* Increase */
    RUN_TEST(test_increase_after_threshold);
    RUN_TEST(test_increase_respects_cooldown);
    RUN_TEST(test_increase_to_ceiling);

    /* Hysteresis */
    RUN_TEST(test_hold_in_dead_zone);
    RUN_TEST(test_hold_at_boundary);

    /* Consecutive tracking */
    RUN_TEST(test_consecutive_reset_on_dead_zone);
    RUN_TEST(test_consecutive_reset_on_high_rtt);

    /* Full pattern */
    RUN_TEST(test_sawtooth_pattern);

    /* Reset */
    RUN_TEST(test_reset);

    /* Edge cases */
    RUN_TEST(test_null_safety);
    RUN_TEST(test_zero_rtt);

    printf("\n==============================\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

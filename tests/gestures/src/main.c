/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "gestures.h"

static struct gestures g;
static enum gesture_action actions[16];
static size_t count;

static void emit(enum gesture_action action)
{
	zassert_true(count < ARRAY_SIZE(actions));
	actions[count++] = action;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	count = 0;
	gestures_init(&g, emit);
	gestures_state(&g, GESTURES_RUNNING);
}

static void tap(enum cwbeat_button button, uint32_t when)
{
	gestures_event(&g, button, true, when);
	gestures_event(&g, button, false, when + 20);
}

ZTEST(gestures, test_volume_release_does_not_repeat)
{
	gestures_event(&g, BUTTON_UP, true, 100);
	zassert_equal(gestures_timeout(&g, 399), 1);
	gestures_poll(&g, 400);
	zassert_equal(count, 1);
	zassert_equal(actions[0], GESTURE_UP);
	zassert_equal(gestures_timeout(&g, 400), 300);
	gestures_event(&g, BUTTON_UP, false, 401);
	zassert_equal(count, 1);
	zassert_equal(gestures_timeout(&g, 401), UINT32_MAX);
	tap(BUTTON_DOWN, 500);
	zassert_equal(count, 2);
	zassert_equal(actions[1], GESTURE_DOWN);
}

ZTEST(gestures, test_volume_release_at_deadline)
{
	gestures_event(&g, BUTTON_DOWN, true, 0);
	gestures_event(&g, BUTTON_DOWN, false, 300);
	zassert_equal(count, 1);
	zassert_equal(actions[0], GESTURE_DOWN);
	gestures_event(&g, BUTTON_UP, true, 500);
	gestures_poll(&g, 800);
	gestures_poll(&g, 1100);
	gestures_event(&g, BUTTON_UP, false, 1200);
	zassert_equal(count, 3);
}

ZTEST(gestures, test_click_deadlines_and_rollover)
{
	tap(BUTTON_PLAY, UINT32_MAX - 30);
	zassert_equal(gestures_timeout(&g, 0), 289);
	gestures_poll(&g, 288);
	zassert_equal(count, 0);
	gestures_poll(&g, 289);
	zassert_equal(count, 1);
	zassert_equal(actions[0], GESTURE_PLAY_PAUSE);
	zassert_equal(gestures_timeout(&g, 289), UINT32_MAX);
	tap(BUTTON_PLAY, 1000);
	tap(BUTTON_PLAY, 1100);
	gestures_poll(&g, 1420);
	zassert_equal(actions[1], GESTURE_NEXT);
	tap(BUTTON_PLAY, 2000);
	tap(BUTTON_PLAY, 2100);
	tap(BUTTON_PLAY, 2200);
	zassert_equal(actions[2], GESTURE_PREVIOUS);
	zassert_equal(count, 3);
}

ZTEST(gestures, test_long_holds_have_no_expired_deadline)
{
	gestures_event(&g, BUTTON_POWER, true, 10);
	zassert_equal(gestures_timeout(&g, 2009), 1);
	gestures_poll(&g, 2010);
	zassert_equal(actions[0], GESTURE_OFF);
	zassert_equal(gestures_timeout(&g, 2010), UINT32_MAX);
	gestures_event(&g, BUTTON_POWER, false, 2011);
	zassert_equal(count, 1);
	gestures_event(&g, BUTTON_BT, true, 3000);
	zassert_equal(gestures_timeout(&g, 5000), 3000);
	gestures_poll(&g, 8000);
	zassert_equal(actions[1], GESTURE_PAIR);
	zassert_equal(gestures_timeout(&g, 8000), UINT32_MAX);
	gestures_event(&g, BUTTON_BT, false, 8001);
	zassert_equal(count, 2);
	gestures_event(&g, BUTTON_PLAY, true, 9000);
	zassert_equal(gestures_timeout(&g, 9000), UINT32_MAX);
	gestures_event(&g, BUTTON_PLAY, false, 11000);
	zassert_equal(count, 2);
	zassert_equal(gestures_timeout(&g, 11000), UINT32_MAX);
}

ZTEST(gestures, test_chord_and_resync_require_release)
{
	gestures_event(&g, BUTTON_PLAY, true, 0);
	gestures_event(&g, BUTTON_BT, true, 20);
	zassert_equal(gestures_timeout(&g, 20), UINT32_MAX);
	gestures_event(&g, BUTTON_PLAY, false, 30);
	gestures_event(&g, BUTTON_BT, false, 40);
	zassert_equal(count, 0);
	tap(BUTTON_PLAY, 100);
	gestures_resync(&g, 1U << BUTTON_UP);
	zassert_equal(gestures_timeout(&g, 200), UINT32_MAX);
	gestures_poll(&g, 1000);
	gestures_event(&g, BUTTON_UP, false, 1100);
	zassert_equal(count, 0);
	tap(BUTTON_UP, 1200);
	zassert_equal(count, 1);
}

ZTEST(gestures, test_mode_change_discards_pending_click)
{
	tap(BUTTON_PLAY, 0);
	gestures_state(&g, GESTURES_OFF);
	zassert_equal(gestures_timeout(&g, 100), UINT32_MAX);
	gestures_poll(&g, 1000);
	zassert_equal(count, 0);
	gestures_event(&g, BUTTON_POWER, true, 1100);
	zassert_equal(actions[0], GESTURE_ON);
	zassert_equal(gestures_timeout(&g, 1100), UINT32_MAX);
	gestures_state(&g, GESTURES_RUNNING);
	gestures_resync(&g, 1U << BUTTON_POWER);
	gestures_event(&g, BUTTON_POWER, false, 1200);
	zassert_equal(count, 1);
	tap(BUTTON_PLAY, 1300);
	gestures_poll(&g, 1620);
	zassert_equal(actions[1], GESTURE_PLAY_PAUSE);
}

ZTEST(gestures, test_disabled_boot_hold_and_power_only)
{
	gestures_state(&g, GESTURES_DISABLED);
	gestures_event(&g, BUTTON_POWER, true, 0);
	gestures_state(&g, GESTURES_RUNNING);
	gestures_resync(&g, 1U << BUTTON_POWER);
	gestures_poll(&g, 5000);
	zassert_equal(count, 0);
	gestures_event(&g, BUTTON_POWER, false, 5010);
	gestures_state(&g, GESTURES_POWER_ONLY);
	tap(BUTTON_UP, 6000);
	zassert_equal(count, 0);
	tap(BUTTON_POWER, 7000);
	zassert_equal(count, 1);
	zassert_equal(actions[0], GESTURE_OFF);
}

ZTEST(gestures, test_invalid_buttons)
{
	gestures_event(&g, (enum cwbeat_button) - 1, true, 0);
	gestures_event(&g, BUTTON_COUNT, true, 0);
	zassert_equal(g.held, 0);
	zassert_equal(count, 0);
	zassert_equal(gestures_timeout(&g, 0), UINT32_MAX);
}

ZTEST_SUITE(gestures, NULL, NULL, before, NULL, NULL);

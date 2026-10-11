/* Product gestures are independent of input drivers and kernel timers. */
#include "gestures.h"
#define CLICK_WINDOW_MS    300u
#define REPEAT_MS          300u
#define HOLD_MS            2000u
#define PAIR_HOLD_MS       5000u
#define DOUBLE_CLICK       2u
#define TRIPLE_CLICK       3u
#define BUTTON_BIT(button) (UINT32_C(1) << (button))
#define BUTTON_MASK        (BUTTON_BIT(BUTTON_COUNT) - UINT32_C(1))

void gestures_init(struct gestures *g, gesture_emit_t emit)
{
	*g = (struct gestures){.blocked = true, .emit = emit};
}
void gestures_cancel(struct gestures *g)
{
	g->blocked = true;
	g->clicks = 0;
	g->suppressed = true;
}
void gestures_state(struct gestures *g, enum gestures_mode mode)
{
	if (g->mode != mode) {
		gestures_cancel(g);
	}
	g->mode = mode;
	if (mode != GESTURES_DISABLED && !g->held) {
		g->blocked = false;
	}
}
void gestures_resync(struct gestures *g, uint32_t held)
{
	gestures_cancel(g);
	g->held = held & BUTTON_MASK;
	if (g->mode != GESTURES_DISABLED && !g->held) {
		g->blocked = false;
	}
}
static void click(struct gestures *g)
{
	g->emit(g->clicks >= TRIPLE_CLICK   ? GESTURE_PREVIOUS
		: g->clicks == DOUBLE_CLICK ? GESTURE_NEXT
					    : GESTURE_PLAY_PAUSE);
	g->clicks = 0;
}
void gestures_poll(struct gestures *g, uint32_t now)
{
	if (g->blocked || g->mode == GESTURES_DISABLED || g->mode == GESTURES_OFF) {
		return;
	}
	if (g->held) {
		uint32_t duration = now - g->pressed_at;
		if (g->held & (BUTTON_BIT(BUTTON_UP) | BUTTON_BIT(BUTTON_DOWN))) {
			if ((uint32_t)(now - g->repeat_at) >= REPEAT_MS) {
				g->repeat_at = now;
				g->repeated = true;
				g->emit(g->held & BUTTON_BIT(BUTTON_UP) ? GESTURE_UP
									: GESTURE_DOWN);
			}
		} else {
			if (duration >= HOLD_MS) {
				g->suppressed = true;
				g->clicks = 0;
			}
			if (!g->long_sent && g->held == BUTTON_BIT(BUTTON_BT) &&
			    duration >= PAIR_HOLD_MS) {
				g->long_sent = true;
				g->emit(GESTURE_PAIR);
			}
			if (!g->long_sent && g->held == BUTTON_BIT(BUTTON_POWER) &&
			    duration >= HOLD_MS) {
				g->long_sent = true;
				g->emit(GESTURE_OFF);
			}
		}
	} else if (g->clicks && (uint32_t)(now - g->released_at) >= CLICK_WINDOW_MS) {
		click(g);
	}
}
void gestures_event(struct gestures *g, enum cwbeat_button button, bool down, uint32_t now)
{
	if (button < BUTTON_PLAY || button >= BUTTON_COUNT) {
		return;
	}
	gestures_poll(g, now);
	uint32_t bit = BUTTON_BIT(button);
	uint32_t before = g->held;
	if (down) {
		g->held |= bit;
	} else {
		g->held &= ~bit;
	}
	if (g->mode == GESTURES_DISABLED || g->blocked || (g->held & (g->held - 1u)) ||
	    (g->mode == GESTURES_POWER_ONLY && button != BUTTON_POWER)) {
		gestures_cancel(g);
		if (g->mode != GESTURES_DISABLED && !g->held) {
			g->blocked = false;
		}
		return;
	}
	if (g->mode == GESTURES_OFF) {
		if (button == BUTTON_POWER && down) {
			g->emit(GESTURE_ON);
			gestures_cancel(g);
		}
		return;
	}
	if (down) {
		if (before & bit) {
			return;
		}
		/* A different key ends any pending Play multi-click gesture. */
		if (button != BUTTON_PLAY) {
			g->clicks = 0;
		}
		g->pressed_at = g->repeat_at = now;
		g->suppressed = g->long_sent = g->repeated = false;
		return;
	}
	if (!(before & bit)) {
		return;
	}
	uint32_t duration = now - g->pressed_at;
	if (button == BUTTON_UP || button == BUTTON_DOWN) {
		if (!g->repeated) {
			g->emit(button == BUTTON_UP ? GESTURE_UP : GESTURE_DOWN);
		}
	} else if (!g->suppressed && duration < HOLD_MS) {
		switch (button) {
		case BUTTON_PLAY:
			++g->clicks;
			g->released_at = now;
			if (g->clicks == TRIPLE_CLICK) {
				click(g);
			}
			break;
		case BUTTON_BT:
			g->emit(GESTURE_DISCOVER);
			break;
		case BUTTON_POWER:
			g->emit(GESTURE_OFF);
			break;
		case BUTTON_LINK:
			g->emit(GESTURE_LINK);
			break;
		default:
			break;
		}
	}
}

static uint32_t remaining(uint32_t now, uint32_t since, uint32_t interval)
{
	uint32_t elapsed = now - since;

	return elapsed >= interval ? 0 : interval - elapsed;
}

uint32_t gestures_timeout(const struct gestures *g, uint32_t now)
{
	if (g->blocked || g->mode == GESTURES_DISABLED || g->mode == GESTURES_OFF) {
		return UINT32_MAX;
	}
	if (!g->held) {
		return g->clicks ? remaining(now, g->released_at, CLICK_WINDOW_MS) : UINT32_MAX;
	}
	if (g->held & (BUTTON_BIT(BUTTON_UP) | BUTTON_BIT(BUTTON_DOWN))) {
		return remaining(now, g->repeat_at, REPEAT_MS);
	}
	if (g->held == BUTTON_BIT(BUTTON_BT) && !g->long_sent) {
		return remaining(now, g->pressed_at, PAIR_HOLD_MS);
	}
	if (g->held == BUTTON_BIT(BUTTON_POWER) && !g->long_sent) {
		return remaining(now, g->pressed_at, HOLD_MS);
	}
	return UINT32_MAX;
}

#ifndef CWBEAT_GESTURES_H
#define CWBEAT_GESTURES_H
#include <stdbool.h>
#include <stdint.h>
enum cwbeat_button {
	BUTTON_PLAY,
	BUTTON_BT,
	BUTTON_UP,
	BUTTON_DOWN,
	BUTTON_POWER,
	BUTTON_LINK,
	BUTTON_COUNT
};
enum gesture_action {
	GESTURE_PLAY_PAUSE,
	GESTURE_NEXT,
	GESTURE_PREVIOUS,
	GESTURE_UP,
	GESTURE_DOWN,
	GESTURE_DISCOVER,
	GESTURE_PAIR,
	GESTURE_OFF,
	GESTURE_ON,
	GESTURE_LINK
};
enum gestures_mode {
	GESTURES_DISABLED,
	GESTURES_RUNNING,
	GESTURES_OFF,
	GESTURES_POWER_ONLY
};
typedef void (*gesture_emit_t)(enum gesture_action action);
struct gestures {
	uint32_t held, pressed_at, repeat_at, released_at;
	uint8_t clicks;
	bool blocked, suppressed, long_sent, repeated;
	enum gestures_mode mode;
	gesture_emit_t emit;
};
void gestures_init(struct gestures *g, gesture_emit_t emit);
void gestures_state(struct gestures *g, enum gestures_mode mode);
void gestures_cancel(struct gestures *g);
/* The caller must discard queued history and capture held together. A sampled
 * release qualifies fresh gestures; any sampled held keys must first release. */
void gestures_resync(struct gestures *g, uint32_t held);
void gestures_event(struct gestures *g, enum cwbeat_button button, bool down, uint32_t now);
/* Milliseconds until the next action; UINT32_MAX means no deadline. */
uint32_t gestures_timeout(const struct gestures *g, uint32_t now);
void gestures_poll(struct gestures *g, uint32_t now);
#endif

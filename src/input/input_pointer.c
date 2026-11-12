#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <time.h>
#include "input.h"
#include "input_internal.h"
#include "shl/eloop.h"
#include "shl/hook.h"

static void pointer_update_inactivity_timer(struct input_dev *dev)
{
	struct itimerspec spec;

	spec.it_interval.tv_nsec = 0;
	spec.it_interval.tv_sec = 0;
	spec.it_value.tv_nsec = 0;
	spec.it_value.tv_sec = 20;
	ev_timer_update(dev->input->hide_pointer, &spec);
}

static void pointer_dev_send_move(struct input_dev *dev)
{
	struct input_pointer_event pev = {0};

	pev.event = POINTER_MOVED;
	pev.pointer_x = dev->input->pointer_x;
	pev.pointer_y = dev->input->pointer_y;

	/* Include button state if a button is pressed during motion (drag) */
	if (dev->pointer.pressed_button != BUTTON_NONE) {
		pev.button = dev->pointer.pressed_button;
		pev.pressed = true;
	} else {
		pev.button = 0;
		pev.pressed = false;
	}

	shl_hook_call(dev->input->pointer_hook, dev->input, &pev);
}

static void pointer_dev_send_wheel(struct input_dev *dev, int32_t value)
{
	struct input_pointer_event pev = {0};

	pev.event = POINTER_WHEEL;
	pev.wheel = value;

	shl_hook_call(dev->input->pointer_hook, dev->input, &pev);
}

static void pointer_dev_send_button(struct input_dev *dev, uint8_t button, bool pressed,
				    bool dbl_click)
{
	struct input_pointer_event pev = {0};

	pev.event = POINTER_BUTTON;
	pev.button = button;
	pev.pressed = pressed;
	pev.double_click = dbl_click;

	shl_hook_call(dev->input->pointer_hook, dev->input, &pev);
}

void pointer_dev_sync(struct input_dev *dev)
{
	struct input_pointer_event pev = {0};

	pev.event = POINTER_SYNC;

	shl_hook_call(dev->input->pointer_hook, dev->input, &pev);
	pointer_update_inactivity_timer(dev);
}

static int32_t clamp_axis(int32_t value, int32_t max)
{
	if (value < 0)
		return 0;
	if (value > max)
		return max;
	return value;
}

void pointer_dev_rel(struct input_dev *dev, uint16_t code, int32_t value)
{
	struct input *input = dev->input;

	switch (code) {
	case REL_X:
		input->pointer_x = clamp_axis(input->pointer_x + value, input->pointer_max_x);
		pointer_dev_send_move(dev);
		break;
	case REL_Y:
		input->pointer_y = clamp_axis(input->pointer_y + value, input->pointer_max_y);
		pointer_dev_send_move(dev);
		break;
	case REL_WHEEL:
		pointer_dev_send_wheel(dev, value);
		break;
	default:
		break;
	}
}

/*
 * Touchpads move the pointer by finger distance, not by raw axis units, so
 * the speed does not depend on the pad's resolution (which may differ
 * between the axes). Without a reported resolution, units map to pixels.
 */
#define TOUCHPAD_PIXELS_PER_MM 10

/*
 * Move by the finger's motion since the last event, keeping the fraction of
 * a pixel for the next one, so the pointer continues from wherever another
 * device left it, even during a touch.
 */
static int32_t touchpad_motion(int32_t value, int32_t *raw, int32_t *rem, int32_t res)
{
	int32_t units = value - *raw;
	int32_t acc, move;

	*raw = value;
	if (res <= 0)
		return units;
	acc = units * TOUCHPAD_PIXELS_PER_MM + *rem;
	move = acc / res;
	*rem = acc - move * res;
	return move;
}

/*
 * Gestures as libinput does them by default: two fingers moving vertically
 * scroll, a short touch without motion taps, with one, two or three fingers
 * for the left, right or middle button. Tap limits are libinput's.
 */
#define TAP_TIMEOUT_MS 180
#define TAP_MOVE_THRESHOLD_TENTHS_MM 13
#define SCROLL_STEP_MM 4

static int32_t abs32(int32_t v)
{
	return v < 0 ? -v : v;
}

/* True if the finger travel since touch down exceeds the tap threshold. */
static bool touchpad_moved(const struct input_pointer *p)
{
	int32_t tx = p->res_x > 0 ? p->res_x * TAP_MOVE_THRESHOLD_TENTHS_MM / 10 : 13;
	int32_t ty = p->res_y > 0 ? p->res_y * TAP_MOVE_THRESHOLD_TENTHS_MM / 10 : 13;

	return p->travel_x > tx || p->travel_y > ty;
}

/* Turn vertical motion with two or more fingers into wheel steps. */
static void touchpad_scroll(struct input_dev *dev, int32_t units)
{
	struct input_pointer *p = &dev->pointer;
	int32_t step = p->res_y > 0 ? p->res_y * SCROLL_STEP_MM : SCROLL_STEP_MM * 10;

	p->scroll_rem += units;
	/* fingers moving down scroll down, like a wheel turned towards the user */
	while (p->scroll_rem >= step) {
		p->scroll_rem -= step;
		pointer_dev_send_wheel(dev, -1);
	}
	while (p->scroll_rem <= -step) {
		p->scroll_rem += step;
		pointer_dev_send_wheel(dev, 1);
	}
}

static void pointer_dev_abs_x(struct input_dev *dev, int32_t value)
{
	struct input *input = dev->input;
	int32_t move;

	switch (dev->pointer.kind) {
	case POINTER_TOUCHPAD:
		if (dev->pointer.touchpad_needs_sync_off_x) {
			dev->pointer.raw_x = value;
			dev->pointer.rem_x = 0;
			dev->pointer.touchpad_needs_sync_off_x = false;
		}
		if (!dev->pointer.touchpaddown)
			return;
		dev->pointer.travel_x += abs32(value - dev->pointer.raw_x);
		if (dev->pointer.fingers >= 2) {
			dev->pointer.raw_x = value;
			return;
		}
		move = touchpad_motion(value, &dev->pointer.raw_x, &dev->pointer.rem_x,
				       dev->pointer.res_x);
		input->pointer_x = clamp_axis(input->pointer_x + move, input->pointer_max_x);
		break;
	case POINTER_TOUCHSCREEN:
	case POINTER_VMOUSE:
		input->pointer_x = ((value - dev->pointer.min_x) * input->pointer_max_x) /
				   (dev->pointer.max_x - dev->pointer.min_x);
		break;
	default:
		return;
	}
	pointer_dev_send_move(dev);
}

static void pointer_dev_abs_y(struct input_dev *dev, int32_t value)
{
	struct input *input = dev->input;
	int32_t move;

	switch (dev->pointer.kind) {
	case POINTER_TOUCHPAD:
		if (dev->pointer.touchpad_needs_sync_off_y) {
			dev->pointer.raw_y = value;
			dev->pointer.rem_y = 0;
			dev->pointer.touchpad_needs_sync_off_y = false;
		}
		if (!dev->pointer.touchpaddown)
			return;
		dev->pointer.travel_y += abs32(value - dev->pointer.raw_y);
		if (dev->pointer.fingers >= 2) {
			touchpad_scroll(dev, value - dev->pointer.raw_y);
			dev->pointer.raw_y = value;
			return;
		}
		move = touchpad_motion(value, &dev->pointer.raw_y, &dev->pointer.rem_y,
				       dev->pointer.res_y);
		input->pointer_y = clamp_axis(input->pointer_y + move, input->pointer_max_y);
		break;
	case POINTER_TOUCHSCREEN:
	case POINTER_VMOUSE:
		input->pointer_y = ((value - dev->pointer.min_y) * input->pointer_max_y) /
				   (dev->pointer.max_y - dev->pointer.min_y);
		break;
	default:
		return;
	}
	pointer_dev_send_move(dev);
}

void pointer_dev_abs(struct input_dev *dev, uint16_t code, int32_t value)
{
	switch (code) {
	case ABS_X:
		pointer_dev_abs_x(dev, value);
		break;
	case ABS_Y:
		pointer_dev_abs_y(dev, value);
		break;
	}
}

static void pointer_dev_left(struct input_dev *dev, bool pressed)
{
	struct timespec tp;
	uint64_t elapsed;
	bool dbl_click = false;

	if (pressed) {
		clock_gettime(CLOCK_MONOTONIC, &tp);
		elapsed = (tp.tv_sec - dev->pointer.last_click.tv_sec) * 1000 +
			  (tp.tv_nsec - dev->pointer.last_click.tv_nsec) / 1000000;
		dbl_click = (elapsed < 500);
		dev->pointer.last_click = tp;
		dev->pointer.pressed_button = 0; /* Button 0 = left */
	} else {
		dev->pointer.pressed_button = BUTTON_NONE; /* No button */
	}
	pointer_dev_send_button(dev, 0, pressed, dbl_click);
}

static void pointer_dev_click(struct input_dev *dev, uint8_t button, bool pressed)
{
	if (button == 0) {
		pointer_dev_left(dev, pressed);
		return;
	}
	dev->pointer.pressed_button = pressed ? button : BUTTON_NONE;
	pointer_dev_send_button(dev, button, pressed, false);
}

/*
 * A finger tool reports how many fingers rest on the pad, not a click. The
 * single-touch position may switch to another finger when the count
 * changes, so take it afresh instead of moving by the jump.
 */
static void touchpad_fingers(struct input_dev *dev, uint8_t n, bool pressed)
{
	struct input_pointer *p = &dev->pointer;

	if (pressed)
		p->fingers = n;
	else if (p->fingers == n)
		p->fingers = 0;
	else
		return;
	if (p->fingers > p->max_fingers)
		p->max_fingers = p->fingers;
	p->touchpad_needs_sync_off_x = true;
	p->touchpad_needs_sync_off_y = true;
	p->scroll_rem = 0;
}

static void touchpad_touch(struct input_dev *dev, bool pressed)
{
	struct input_pointer *p = &dev->pointer;
	struct timespec tp;
	int64_t elapsed;
	/* one, two, three fingers: left, right, middle */
	static const uint8_t tap_button[] = {0, 2, 1};
	uint8_t n;

	p->touchpaddown = pressed;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	if (pressed) {
		p->touchpad_needs_sync_off_x = true;
		p->touchpad_needs_sync_off_y = true;
		p->touch_start = tp;
		p->travel_x = 0;
		p->travel_y = 0;
		p->scroll_rem = 0;
		p->max_fingers = p->fingers;
		p->tap_possible = true;
		return;
	}
	elapsed = (int64_t)(tp.tv_sec - p->touch_start.tv_sec) * 1000 +
		  (tp.tv_nsec - p->touch_start.tv_nsec) / 1000000;
	if (!p->tap_possible || elapsed >= TAP_TIMEOUT_MS || touchpad_moved(p))
		return;
	n = p->max_fingers ? p->max_fingers : 1;
	if (n > 3)
		n = 3;
	pointer_dev_click(dev, tap_button[n - 1], true);
	pointer_dev_click(dev, tap_button[n - 1], false);
}

void pointer_dev_button(struct input_dev *dev, uint16_t code, int32_t value)
{
	bool pressed = (value == 1);
	bool touchpad = (dev->pointer.kind == POINTER_TOUCHPAD);

	/* Only handle press and release events */
	if (value > 1)
		return;

	switch (code) {
	case BTN_LEFT:
		/* a physical click during a touch is not also a tap */
		dev->pointer.tap_possible = false;
		pointer_dev_left(dev, pressed);
		break;
	case BTN_RIGHT:
		dev->pointer.tap_possible = false;
		dev->pointer.pressed_button = pressed ? 2 : BUTTON_NONE; /* Button 2 = right */
		pointer_dev_send_button(dev, 2, pressed, false);
		break;
	case BTN_MIDDLE:
		dev->pointer.tap_possible = false;
		dev->pointer.pressed_button = pressed ? 1 : BUTTON_NONE; /* Button 1 = middle */
		pointer_dev_send_button(dev, 1, pressed, false);
		break;
	case BTN_TOOL_FINGER:
		if (touchpad)
			touchpad_fingers(dev, 1, pressed);
		break;
	case BTN_TOOL_DOUBLETAP:
		if (touchpad)
			touchpad_fingers(dev, 2, pressed);
		break;
	case BTN_TOOL_TRIPLETAP:
	case BTN_TOOL_QUADTAP:
	case BTN_TOOL_QUINTTAP:
		if (touchpad)
			touchpad_fingers(dev, 3, pressed);
		break;
	case BTN_TOUCH:
		if (touchpad)
			touchpad_touch(dev, pressed);
		else
			dev->pointer.touchpaddown = pressed;
		break;
	default:
		break;
	}
}

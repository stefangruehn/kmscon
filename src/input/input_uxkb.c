/*
 * Kmscon - XKB Keyboard Handling
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>
#include "input.h"
#include "input_fallback.xkb.bin.h"
#include "input_internal.h"
#include "shl/hook.h"
#include "shl/log.h"
#include "shl/misc.h"

#define LOG_SUBSYSTEM "uxkb"

static enum log_severity uxkb_log_level(enum xkb_log_level level)
{
	switch (level) {
	case XKB_LOG_LEVEL_CRITICAL:
		return LOG_CRITICAL;
	case XKB_LOG_LEVEL_ERROR:
		return LOG_ERROR;
	case XKB_LOG_LEVEL_WARNING:
		return LOG_WARNING;
	case XKB_LOG_LEVEL_INFO:
		return LOG_INFO;
	case XKB_LOG_LEVEL_DEBUG:
	default:
		return LOG_DEBUG;
	}
}

static void uxkb_log(struct xkb_context *context, enum xkb_log_level level, const char *format,
		     va_list args)
{
	log_submit(uxkb_log_level(level), "xkbcommon", format, args);
}

static struct xkb_keymap *uxkb_layout_from_file(struct input *input, const char *file)
{
	struct xkb_keymap *keymap;
	FILE *keymap_file;

	keymap_file = fopen(file, "r");
	if (!keymap_file) {
		log_error("cannot open keymap file %s", file);
		return NULL;
	}

	keymap = xkb_keymap_new_from_file(input->ctx, keymap_file, XKB_KEYMAP_FORMAT_TEXT_V1,
					  XKB_KEYMAP_COMPILE_NO_FLAGS);
	fclose(keymap_file);
	if (!keymap) {
		log_error("cannot create keymap from file %s", file);
		return NULL;
	}
	return keymap;
}

int uxkb_layout_init(struct input *input, const char *model, const char *layout,
		     const char *variant, const char *options, const char *keymap_file)
{
	int ret = 0;
	struct xkb_rule_names rmlvo = {
		.rules = "evdev",
		.model = model,
		.layout = layout,
		.variant = variant,
		.options = options,
	};
	const char *fallback;

	fallback = _binary_input_fallback_xkb_start;

	input->ctx = xkb_context_new(0);
	if (!input->ctx) {
		log_error("cannot create XKB context");
		return -ENOMEM;
	}

	/* Set logging function. You can use XKB_LOG_VERBOSITY and XKB_LOG_LEVEL
	 * to change the xkbcommon logger. That's why we don't touch the
	 * verbosity and level here. */
	xkb_context_set_log_fn(input->ctx, uxkb_log);

	/* If a complete keymap file was given, first try that. */
	if (keymap_file)
		input->keymap = uxkb_layout_from_file(input, keymap_file);

	if (!input->keymap)
		input->keymap =
			xkb_keymap_new_from_names(input->ctx, &rmlvo, XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (!input->keymap) {
		log_warn("failed to create keymap (%s, %s, %s, %s), "
			 "reverting to default system keymap",
			 model, layout, variant, options);

		rmlvo.model = "";
		rmlvo.layout = "";
		rmlvo.variant = "";
		rmlvo.options = "";

		input->keymap = xkb_keymap_new_from_names(input->ctx, &rmlvo, 0);
		if (!input->keymap) {
			log_warn("failed to create XKB default keymap, "
				 "reverting to built-in fallback");

			input->keymap = xkb_keymap_new_from_string(input->ctx, fallback,
								   XKB_KEYMAP_FORMAT_TEXT_V1, 0);
			if (!input->keymap) {
				log_error("cannot create fallback keymap");
				ret = -EFAULT;
				goto err_ctx;
			}
		}

		log_debug("new fallback keyboard description");
	} else {
		log_debug("new keyboard description (%s, %s, %s, %s)", model, layout, variant,
			  options);
	}
	input->state = xkb_state_new(input->keymap);
	if (!input->state) {
		log_error("cannot create XKB state");
		ret = -ENOMEM;
		goto err_keymap;
	}

	return 0;
err_keymap:
	xkb_keymap_unref(input->keymap);
	input->keymap = NULL;
err_ctx:
	xkb_context_unref(input->ctx);
	input->ctx = NULL;
	return ret;
}

void uxkb_compose_table_init(struct input *input, const char *compose_file, const char *locale)
{
	FILE *compose;

	if (compose_file) {
		compose = fopen(compose_file, "r");
		if (!compose) {
			log_error("cannot open compose file %s", compose_file);
			return;
		}
		input->compose_table = xkb_compose_table_new_from_file(
			input->ctx, compose, locale, XKB_COMPOSE_FORMAT_TEXT_V1,
			XKB_COMPOSE_COMPILE_NO_FLAGS);

		fclose(compose);
		if (input->compose_table) {
			log_debug("new compose table from file %s", compose_file);
		} else {
			log_warn("cannot parse compose table, "
				 "reverting to default");
		}
	}

	if (!input->compose_table) {
		input->compose_table = xkb_compose_table_new_from_locale(
			input->ctx, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
		if (!input->compose_table) {
			log_warn("failed to create XKB default compose "
				 "table, disabling compose support");
		}
	}

	if (input->compose_table) {
		input->compose_state = xkb_compose_state_new(input->compose_table, 0);
		if (!input->compose_state)
			log_warn("cannot create compose state, "
				 "disabling compose support");
	}
}

void uxkb_compose_table_destroy(struct input *input)
{
	xkb_compose_state_unref(input->compose_state);
	input->compose_state = NULL;
	xkb_compose_table_unref(input->compose_table);
	input->compose_table = NULL;
}

void uxkb_layout_destroy(struct input *input)
{
	xkb_state_unref(input->state);
	input->state = NULL;
	xkb_keymap_unref(input->keymap);
	xkb_context_unref(input->ctx);
	input->keymap = NULL;
	input->ctx = NULL;
}

static void timer_event(struct ev_timer *timer, uint64_t num, void *data)
{
	struct input_dev *dev = data;

	dev->repeat_event.handled = false;
	shl_hook_call(dev->input->key_hook, dev->input, &dev->repeat_event);
}

int uxkb_dev_init(struct input_dev *dev)
{
	/* The shared XKB state is created in uxkb_layout_init() */
	return ev_eloop_new_timer(dev->input->eloop, &dev->repeat_timer, NULL, timer_event, dev);
}

void uxkb_dev_destroy(struct input_dev *dev)
{
	ev_eloop_rm_timer(dev->repeat_timer);
	dev->repeat_timer = NULL;
}

#define EVDEV_KEYCODE_OFFSET 8
enum {
	KEY_RELEASED = 0,
	KEY_PRESSED = 1,
	KEY_REPEATED = 2,
};

void uxkb_dev_update_keyboard_leds(struct input_dev *dev)
{
	static const struct {
		int evdev_led;
		const char *xkb_led;
	} leds[] = {
		{LED_NUML, XKB_LED_NAME_NUM},
		{LED_CAPSL, XKB_LED_NAME_CAPS},
		{LED_SCROLLL, XKB_LED_NAME_SCROLL},
	};
	struct input_event events[sizeof(leds) / sizeof(*leds)];
	int i, ret;

	if (!(dev->capabilities & DEVICE_HAS_LEDS))
		return;

	memset(events, 0, sizeof(events));

	for (i = 0; i < sizeof(leds) / sizeof(*leds); i++) {
		events[i].type = EV_LED;
		events[i].code = leds[i].evdev_led;
		if (xkb_state_led_name_is_active(dev->input->state, leds[i].xkb_led) > 0)
			events[i].value = 1;
	}

	ret = write(dev->rfd, events, sizeof(events));
	if (ret != sizeof(events))
		log_warning("cannot update LED state (%d): %m", errno);
}

void uxkb_dev_set_leds(struct input_dev *dev, unsigned int scroll_lock, unsigned int num_lock,
		       unsigned int caps_lock)
{
	struct input_event events[3];
	int ret;

	if (!(dev->capabilities & DEVICE_HAS_LEDS))
		return;

	memset(events, 0, sizeof(events));

	events[0].type = EV_LED;
	events[0].code = LED_SCROLLL;
	events[0].value = !!scroll_lock;

	events[1].type = EV_LED;
	events[1].code = LED_NUML;
	events[1].value = !!num_lock;

	events[2].type = EV_LED;
	events[2].code = LED_CAPSL;
	events[2].value = !!caps_lock;

	ret = write(dev->rfd, events, sizeof(events));
	if (ret != sizeof(events))
		log_warning("cannot set LED state (%d): %m", errno);
}

static inline int uxkb_dev_resize_event(struct input_dev *dev, size_t s)
{
	uint32_t *tmp;

	if (s > dev->num_syms) {
		tmp = realloc(dev->event.keysyms, sizeof(uint32_t) * s);
		if (!tmp) {
			log_warning("cannot reallocate keysym buffer");
			return -ENOKEY;
		}
		dev->event.keysyms = tmp;

		tmp = realloc(dev->event.codepoints, sizeof(uint32_t) * s);
		if (!tmp) {
			log_warning("cannot reallocate codepoints buffer");
			return -ENOKEY;
		}
		dev->event.codepoints = tmp;

		tmp = realloc(dev->repeat_event.keysyms, sizeof(uint32_t) * s);
		if (!tmp) {
			log_warning("cannot reallocate keysym buffer");
			return -ENOKEY;
		}
		dev->repeat_event.keysyms = tmp;

		tmp = realloc(dev->repeat_event.codepoints, sizeof(uint32_t) * s);
		if (!tmp) {
			log_warning("cannot reallocate codepoints buffer");
			return -ENOKEY;
		}
		dev->repeat_event.codepoints = tmp;

		dev->num_syms = s;
	}

	return 0;
}

static int uxkb_dev_fill_event(struct input_dev *dev, struct input_key_event *ev,
			       xkb_keycode_t code, int num_syms, const xkb_keysym_t *syms)
{
	int ret, i;

	ret = uxkb_dev_resize_event(dev, num_syms);
	if (ret)
		return ret;

	ev->keycode = code;
	ev->ascii = shl_get_ascii(dev->input->state, code, syms, num_syms);
	ev->mods = shl_get_xkb_mods(dev->input->state);
	ev->num_syms = num_syms;
	memcpy(ev->keysyms, syms, sizeof(uint32_t) * num_syms);

	for (i = 0; i < num_syms; ++i) {
		ev->codepoints[i] = xkb_keysym_to_utf32(syms[i]);
		if (!ev->codepoints[i])
			ev->codepoints[i] = INPUT_INVALID;
	}

	return 0;
}

static void uxkb_dev_repeat(struct input_dev *dev, unsigned int state)
{
	struct xkb_keymap *keymap = xkb_state_get_keymap(dev->input->state);
	unsigned int i;
	int num_keysyms, ret;
	const uint32_t *keysyms;
	struct itimerspec spec;

	if (dev->repeating && dev->repeat_event.keycode == dev->event.keycode) {
		if (state == KEY_RELEASED) {
			dev->repeating = false;
			ev_timer_update(dev->repeat_timer, NULL);
		}
		return;
	}

	if (state == KEY_PRESSED && xkb_keymap_key_repeats(keymap, dev->event.keycode)) {
		dev->repeat_event.keycode = dev->event.keycode;
		dev->repeat_event.ascii = dev->event.ascii;
		dev->repeat_event.mods = dev->event.mods;
		dev->repeat_event.num_syms = dev->event.num_syms;

		for (i = 0; i < dev->event.num_syms; ++i) {
			dev->repeat_event.keysyms[i] = dev->event.keysyms[i];
			dev->repeat_event.codepoints[i] = dev->event.codepoints[i];
		}
	} else if (dev->repeating && !xkb_keymap_key_repeats(keymap, dev->event.keycode)) {
		num_keysyms = xkb_state_key_get_syms(dev->input->state, dev->repeat_event.keycode,
						     &keysyms);
		if (num_keysyms <= 0)
			return;

		ret = uxkb_dev_fill_event(dev, &dev->repeat_event, dev->repeat_event.keycode,
					  num_keysyms, keysyms);
		if (ret)
			return;

		return;
	} else {
		return;
	}

	dev->repeating = true;
	spec.it_interval.tv_sec = 0;
	spec.it_interval.tv_nsec = dev->input->repeat_rate * 1000000;
	spec.it_value.tv_sec = 0;
	spec.it_value.tv_nsec = dev->input->repeat_delay * 1000000;
	ev_timer_update(dev->repeat_timer, &spec);
}

/* Insert on its own or on the keypad, without Shift, Ctrl, Alt or Super */
static bool uxkb_bare_insert(struct xkb_state *state, xkb_keysym_t sym)
{
	static const char *const mods[] = {XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT,
					   XKB_MOD_NAME_LOGO};
	size_t i;

	if (sym != XKB_KEY_Insert && sym != XKB_KEY_KP_Insert)
		return false;
	for (i = 0; i < sizeof(mods) / sizeof(*mods); i++) {
		if (xkb_state_mod_name_is_active(state, mods[i], XKB_STATE_MODS_EFFECTIVE) > 0)
			return false;
	}
	return true;
}

int uxkb_dev_process(struct input_dev *dev, uint16_t key_state, uint16_t code)
{
	struct xkb_state *state;
	struct xkb_compose_state *compose_state;
	xkb_keycode_t keycode;
	const xkb_keysym_t *keysyms;
	xkb_keysym_t one_sym;
	int num_keysyms, ret;
	enum xkb_compose_status compose_status;
	enum xkb_state_component changed;

	if (key_state == KEY_REPEATED)
		return -ENOKEY;

	state = dev->input->state;
	compose_state = dev->input->compose_state;
	keycode = code + EVDEV_KEYCODE_OFFSET;

	/*
	 * To summarize the following convoluted logic:
	 *
	 * - single key press may produce one or more keysyms
	 * - if num_keysyms == 1,
	 *     + use get_one_sym to handle the maybe present LOCK mod
	 *     + use the resulting one_sym to feed the compose_state
	 *     + if the keysym completes a compose sequence,
	 *         * compose_state will either produce one keysym, which is set
	 *           back to one_sym, or
	 *         * compose_state will produce NoSymbol, which is treated as a
	 *           cancelled compose sequence
	 *     + if the keysym completes or cancels a sequence, reset
	 *       compose_state
	 * - if num_keysyms != 1,
	 *     + LOCK mod translation doesn't make sense, so skip that
	 *     + compose_state is fed NoSymbol per documentation and the result
	 *       is basically ignored
	 * - update xkb state after querying keysyms per documentation
	 * - if in the process of composing or if the composing was cancelled by
	 *   the key press, stop here
	 * - otherwise process events with keysyms and num_keysyms
	 *
	 * Some background on compose handling:
	 * - https://github.com/xkbcommon/libxkbcommon/issues/4
	 */

	num_keysyms = xkb_state_key_get_syms(state, keycode, &keysyms);

	one_sym = XKB_KEY_NoSymbol;
	if (num_keysyms == 1) {
		/* See: https://bugs.freedesktop.org/show_bug.cgi?id=67167 */
		one_sym = xkb_state_key_get_one_sym(state, keycode);
		keysyms = &one_sym;
	}

	compose_status = XKB_COMPOSE_NOTHING;
	if (compose_state && key_state == KEY_PRESSED) {
		/* XKB_KEY_NoSymbol cancels the current compose sequence. */
		xkb_compose_state_feed(compose_state, one_sym);

		compose_status = xkb_compose_state_get_status(compose_state);

		if (compose_status == XKB_COMPOSE_COMPOSED) {
			one_sym = xkb_compose_state_get_one_sym(compose_state);
			if (one_sym == XKB_KEY_NoSymbol) {
				/*
				 * It is possible that the sequence only
				 * specifies an utf8 string and not a keysym.
				 * Treat this is cancelled, as the rest of the
				 * system can not handle it.
				 */
				compose_status = XKB_COMPOSE_CANCELLED;
			}
		}

		/*
		 * Modifiers are legal key presses, but do not change the
		 * compose_state. If the state is not reset after a sequence is
		 * completed, holding down a modifier after composing repeats
		 * the last composed sequence.
		 */
		if (compose_status == XKB_COMPOSE_COMPOSED ||
		    compose_status == XKB_COMPOSE_CANCELLED)
			xkb_compose_state_reset(compose_state);
	}

	changed = 0;
	if (key_state == KEY_PRESSED)
		changed = xkb_state_update_key(state, keycode, XKB_KEY_DOWN);
	else if (key_state == KEY_RELEASED)
		changed = xkb_state_update_key(state, keycode, XKB_KEY_UP);

	if (changed & XKB_STATE_LEDS) {
		uxkb_dev_update_keyboard_leds(dev);
		uxkb_write_locks(dev->input);
	}

	if (num_keysyms <= 0)
		return -ENOKEY;

	if (compose_status == XKB_COMPOSE_COMPOSING || compose_status == XKB_COMPOSE_CANCELLED)
		return -ENOKEY;

	if (key_state == KEY_PRESSED && num_keysyms == 1 &&
	    dev->input->insert_mode != INPUT_INSERT_PASS && uxkb_bare_insert(state, keysyms[0])) {
		dev->input->insert_count++;
		uxkb_write_locks(dev->input);
		if (dev->input->insert_mode == INPUT_INSERT_BLOCK)
			return -ENOKEY;
	}

	ret = uxkb_dev_fill_event(dev, &dev->event, keycode, num_keysyms, keysyms);
	if (ret)
		return -ENOKEY;

	uxkb_dev_repeat(dev, key_state);

	if (key_state == KEY_RELEASED)
		return -ENOKEY;

	dev->event.handled = false;
	shl_hook_call(dev->input->key_hook, dev->input, &dev->event);

	return 0;
}

unsigned int uxkb_dev_get_mods(struct input_dev *dev)
{
	if (!dev || !dev->input->state)
		return 0;

	return xkb_state_serialize_mods(dev->input->state, XKB_STATE_MODS_EFFECTIVE);
}

void uxkb_wake_up(struct input *input)
{
	xkb_mod_mask_t locked_mods;
	xkb_layout_index_t locked_layout;

	if (!input->state)
		return;

	/*
	 * On VT re-entry, there are some race conditions about the state of the key
	 * So to avoid sticky modifiers, save only the "locked mods" (like Ver Num)
	 * and recreate a new clear state.
	 * The drawback is that the keys that are held when you switch to this VT
	 * will need to be pressed again.
	 */
	locked_mods = xkb_state_serialize_mods(input->state, XKB_STATE_MODS_LOCKED);
	locked_layout = xkb_state_serialize_layout(input->state, XKB_STATE_LAYOUT_LOCKED);

	/* There is no way to clear the current state, so destroy and recreate a new one */
	xkb_state_unref(input->state);

	input->state = xkb_state_new(input->keymap);
	if (!input->state) {
		log_error("cannot create XKB state");
		return;
	}

	xkb_state_update_mask(input->state, 0, 0, locked_mods, 0, 0, locked_layout);

	if (input->compose_state)
		xkb_compose_state_reset(input->compose_state);
}

xkb_mod_mask_t uxkb_locked_mods(struct input *input)
{
	if (!input->state)
		return 0;
	return xkb_state_serialize_mods(input->state, XKB_STATE_MODS_LOCKED);
}

void uxkb_lock_mods(struct input *input, xkb_mod_mask_t mods)
{
	if (input->state && mods)
		xkb_state_update_mask(input->state, 0, 0, mods, 0, 0, 0);
}

/* the lock keys of --lock-file and --lock-request-file, in file order */
static const struct {
	const char *name;
	const char *led;
	xkb_keysym_t sym;
} uxkb_locks[] = {
	{"caps", XKB_LED_NAME_CAPS, XKB_KEY_Caps_Lock},
	{"num", XKB_LED_NAME_NUM, XKB_KEY_Num_Lock},
	{"scroll", XKB_LED_NAME_SCROLL, XKB_KEY_Scroll_Lock},
};

struct uxkb_key_search {
	xkb_keysym_t sym;
	xkb_keycode_t key;
};

static void uxkb_search_key(struct xkb_keymap *keymap, xkb_keycode_t key, void *data)
{
	struct uxkb_key_search *search = data;
	const xkb_keysym_t *syms;
	int i, n;

	if (search->key != XKB_KEYCODE_INVALID)
		return;
	n = xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &syms);
	for (i = 0; i < n; i++) {
		if (syms[i] == search->sym) {
			search->key = key;
			return;
		}
	}
}

/* the key that gives sym on the first level of the first layout, if any */
static xkb_keycode_t uxkb_key_for(struct xkb_keymap *keymap, xkb_keysym_t sym)
{
	struct uxkb_key_search search = {sym, XKB_KEYCODE_INVALID};

	xkb_keymap_key_for_each(keymap, uxkb_search_key, &search);
	return search.key;
}

/* whether pressing the key for lock i switches its LED, tried on a fresh state */
static bool uxkb_lock_switchable(struct xkb_keymap *keymap, size_t i)
{
	struct xkb_state *state;
	xkb_keycode_t key;
	int was;
	bool ret;

	key = uxkb_key_for(keymap, uxkb_locks[i].sym);
	if (key == XKB_KEYCODE_INVALID)
		return false;
	state = xkb_state_new(keymap);
	if (!state)
		return false;
	was = xkb_state_led_name_is_active(state, uxkb_locks[i].led) > 0;
	xkb_state_update_key(state, key, XKB_KEY_DOWN);
	xkb_state_update_key(state, key, XKB_KEY_UP);
	ret = (xkb_state_led_name_is_active(state, uxkb_locks[i].led) > 0) != was;
	xkb_state_unref(state);
	return ret;
}

/*
 * Write --lock-file when its content changed: through a new file and a
 * rename, so a reader never sees half of it. A lock is "none" when it is off
 * and no key of the keymap switches it (Scroll Lock has a key in the usual
 * keymaps, but no LED change).
 */
void uxkb_write_locks(struct input *input)
{
	char text[sizeof(input->lock_text)], tmp[PATH_MAX];
	size_t i, len = 0;
	const char *word;
	int fd, ret;
	FILE *f;

	if (!input->lock_file || !input->state)
		return;

	for (i = 0; i < sizeof(uxkb_locks) / sizeof(*uxkb_locks); i++) {
		if (xkb_state_led_name_is_active(input->state, uxkb_locks[i].led) > 0)
			word = "on";
		else if (uxkb_lock_switchable(input->keymap, i))
			word = "off";
		else
			word = "none";
		len += snprintf(text + len, sizeof(text) - len, "%s %s\n", uxkb_locks[i].name,
				word);
	}
	snprintf(text + len, sizeof(text) - len, "insert %u\n", input->insert_count);
	if (!strcmp(text, input->lock_text))
		return;

	ret = snprintf(tmp, sizeof(tmp), "%s.new", input->lock_file);
	if (ret < 0 || (size_t)ret >= sizeof(tmp))
		return;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		log_warning("cannot write lock file %s: %m", tmp);
		return;
	}
	/* readable by the user's status line, whatever the umask */
	fchmod(fd, 0644);
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		return;
	}
	fputs(text, f);
	if (fclose(f) || rename(tmp, input->lock_file)) {
		log_warning("cannot write lock file %s: %m", input->lock_file);
		unlink(tmp);
		return;
	}
	snprintf(input->lock_text, sizeof(input->lock_text), "%s", text);
}

/*
 * Switch one lock key (on 1, off 0, toggle -1) by pressing and releasing the
 * key the keymap has for it, as the keyboard would. Returns true if the lock
 * changed.
 */
bool uxkb_set_lock(struct input *input, const char *name, int on)
{
	xkb_keycode_t key;
	size_t i;
	int was;

	if (!input->state)
		return false;
	for (i = 0; i < sizeof(uxkb_locks) / sizeof(*uxkb_locks); i++) {
		if (!strcmp(name, uxkb_locks[i].name))
			break;
	}
	if (i == sizeof(uxkb_locks) / sizeof(*uxkb_locks)) {
		log_warning("unknown lock %s", name);
		return false;
	}
	was = xkb_state_led_name_is_active(input->state, uxkb_locks[i].led) > 0;
	if (on >= 0 && on == was)
		return false;
	key = uxkb_key_for(input->keymap, uxkb_locks[i].sym);
	if (key == XKB_KEYCODE_INVALID) {
		log_warning("no key for lock %s", name);
		return false;
	}
	xkb_state_update_key(input->state, key, XKB_KEY_DOWN);
	xkb_state_update_key(input->state, key, XKB_KEY_UP);
	if ((xkb_state_led_name_is_active(input->state, uxkb_locks[i].led) > 0) == was) {
		log_warning("lock %s did not switch", name);
		return false;
	}
	return true;
}

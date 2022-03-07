#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/interfaces/wlr_tablet_tool.h>
#include <wlr/interfaces/wlr_tablet_pad.h>
#include <wlr/util/log.h>

#include "interfaces/wlr_input_device.h"
#include "backend/wayland.h"
#include "util/signal.h"
#include "util/time.h"

static const struct wlr_touch_impl touch_impl;

static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	struct wlr_keyboard *keyboard = data;

	uint32_t *keycode_ptr;
	wl_array_for_each(keycode_ptr, keys) {
		struct wlr_event_keyboard_key event = {
			.keycode = *keycode_ptr,
			.state = WL_KEYBOARD_KEY_STATE_PRESSED,
			.time_msec = get_current_time_msec(),
			.update_state = false,
		};
		wlr_keyboard_notify_key(keyboard, &event);
	}
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface) {
	struct wlr_keyboard *keyboard = data;

	size_t num_keycodes = keyboard->num_keycodes;
	uint32_t pressed[num_keycodes + 1];
	memcpy(pressed, keyboard->keycodes,
		num_keycodes * sizeof(uint32_t));

	for (size_t i = 0; i < num_keycodes; ++i) {
		uint32_t keycode = pressed[i];

		struct wlr_event_keyboard_key event = {
			.keycode = keycode,
			.state = WL_KEYBOARD_KEY_STATE_RELEASED,
			.time_msec = get_current_time_msec(),
			.update_state = false,
		};
		wlr_keyboard_notify_key(keyboard, &event);
	}
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	struct wlr_keyboard *keyboard = data;

	struct wlr_event_keyboard_key wlr_event = {
		.keycode = key,
		.state = state,
		.time_msec = time,
		.update_state = false,
	};
	wlr_keyboard_notify_key(keyboard, &wlr_event);
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	struct wlr_keyboard *keyboard = data;
	wlr_keyboard_notify_modifiers(keyboard, mods_depressed, mods_latched,
		mods_locked, group);
}

static void keyboard_handle_repeat_info(void *data,
		struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
	// This space is intentionally left blank
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_handle_keymap,
	.enter = keyboard_handle_enter,
	.leave = keyboard_handle_leave,
	.key = keyboard_handle_key,
	.modifiers = keyboard_handle_modifiers,
	.repeat_info = keyboard_handle_repeat_info
};

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "wl-keyboard",
};

void init_seat_keyboard(struct wlr_wl_seat *seat) {
	assert(seat->wl_keyboard);

	char name[128] = {0};
	snprintf(name, sizeof(name), "wayland-keyboard-%s", seat->name);

	wlr_keyboard_init(&seat->wlr_keyboard, &keyboard_impl, name);
	wl_keyboard_add_listener(seat->wl_keyboard, &keyboard_listener,
		&seat->wlr_keyboard);

	wlr_signal_emit_safe(&seat->backend->backend.events.new_input,
		&seat->wlr_keyboard.base);
}

static void touch_coordinates_to_absolute(struct wlr_wl_input_device *device,
		wl_fixed_t x, wl_fixed_t y, double *sx, double *sy) {
	// TODO: each output needs its own touch
	struct wlr_wl_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &device->backend->outputs, link) {
		*sx = wl_fixed_to_double(x) / output->wlr_output.width;
		*sy = wl_fixed_to_double(y) / output->wlr_output.height;
		return; // Choose the first output in the list
	}

	*sx = *sy = 0;
}

static void touch_handle_down(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, struct wl_surface *surface,
		int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct wlr_wl_input_device *device = data;
	assert(device && device->wlr_input_device.touch);

	double sx, sy;
	touch_coordinates_to_absolute(device, x, y, &sx, &sy);
	struct wlr_event_touch_down event = {
		.device = &device->wlr_input_device,
		.time_msec = time,
		.touch_id = id,
		.x = sx,
		.y = sy
	};
	wlr_signal_emit_safe(&device->wlr_input_device.touch->events.down, &event);
}

static void touch_handle_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id) {
	struct wlr_wl_input_device *device = data;
	assert(device && device->wlr_input_device.touch);

	struct wlr_event_touch_up event = {
		.device = &device->wlr_input_device,
		.time_msec = time,
		.touch_id = id,
	};
	wlr_signal_emit_safe(&device->wlr_input_device.touch->events.up, &event);
}

static void touch_handle_motion(void *data, struct wl_touch *wl_touch,
		uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct wlr_wl_input_device *device = data;
	assert(device && device->wlr_input_device.touch);

	double sx, sy;
	touch_coordinates_to_absolute(device, x, y, &sx, &sy);
	struct wlr_event_touch_motion event = {
		.device = &device->wlr_input_device,
		.time_msec = time,
		.touch_id = id,
		.x = sx,
		.y = sy
	};
	wlr_signal_emit_safe(&device->wlr_input_device.touch->events.motion, &event);
}

static void touch_handle_frame(void *data, struct wl_touch *wl_touch) {
	struct wlr_wl_input_device *device = data;
	assert(device && device->wlr_input_device.touch);

	wlr_signal_emit_safe(&device->wlr_input_device.touch->events.frame, NULL);
}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch) {
	// no-op
}

static void touch_handle_shape(void *data, struct wl_touch *wl_touch,
		int32_t id, wl_fixed_t major, wl_fixed_t minor) {
	// no-op
}

static void touch_handle_orientation(void *data, struct wl_touch *wl_touch,
		int32_t id, wl_fixed_t orientation) {
	// no-op
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_handle_down,
	.up = touch_handle_up,
	.motion = touch_handle_motion,
	.frame = touch_handle_frame,
	.cancel = touch_handle_cancel,
	.shape = touch_handle_shape,
	.orientation = touch_handle_orientation,
};

static struct wlr_wl_input_device *get_wl_input_device_from_input_device(
		struct wlr_input_device *wlr_dev) {
	assert(wlr_input_device_is_wl(wlr_dev));
	return (struct wlr_wl_input_device *)wlr_dev;
}

bool create_wl_seat(struct wl_seat *wl_seat, struct wlr_wl_backend *wl) {
	struct wlr_wl_seat *seat = calloc(1, sizeof(struct wlr_wl_seat));
	if (!seat) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return false;
	}
	seat->wl_seat = wl_seat;
	seat->backend = wl;
	wl_list_insert(&wl->seats, &seat->link);
	wl_seat_add_listener(wl_seat, &seat_listener, seat);
	return true;
}

void destroy_wl_seats(struct wlr_wl_backend *wl) {
	struct wlr_wl_seat *seat, *tmp_seat;
	wl_list_for_each_safe(seat, tmp_seat, &wl->seats, link) {
		if (seat->touch) {
			wl_touch_destroy(seat->touch);
		}
		if (seat->wl_pointer) {
			finish_seat_pointer(seat);
		}
		if (seat->wl_keyboard) {
			wl_keyboard_release(seat->wl_keyboard);
			wlr_keyboard_finish(&seat->wlr_keyboard);
		}

		free(seat->name);
		assert(seat->wl_seat);
		wl_seat_destroy(seat->wl_seat);

		wl_list_remove(&seat->link);
		free(seat);
	}
}

static struct wlr_wl_seat *input_device_get_seat(struct wlr_input_device *wlr_dev) {
	struct wlr_wl_input_device *dev =
		get_wl_input_device_from_input_device(wlr_dev);
	assert(dev->seat);
	return dev->seat;
}

bool wlr_input_device_is_wl(struct wlr_input_device *dev) {
	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		return dev->keyboard->impl == &keyboard_impl;
	case WLR_INPUT_DEVICE_POINTER:
		return dev->pointer->impl == &wl_pointer_impl;
	case WLR_INPUT_DEVICE_TOUCH:
		return dev->touch->impl == &touch_impl;
	case WLR_INPUT_DEVICE_TABLET_TOOL:
		return dev->tablet->impl == &tablet_impl;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		return dev->tablet_pad->impl == &tablet_pad_impl;
	default:
		return false;
	}
}

struct wlr_wl_input_device *create_wl_input_device(
		struct wlr_wl_seat *seat, enum wlr_input_device_type type) {
	struct wlr_wl_input_device *dev =
		calloc(1, sizeof(struct wlr_wl_input_device));
	if (dev == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	dev->backend = seat->backend;
	dev->seat = seat;

	struct wlr_input_device *wlr_dev = &dev->wlr_input_device;

	const char *type_name = "unknown";

	switch (type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		wlr_log(WLR_ERROR, "can't create keyboard wlr_wl_input_device");
		free(dev);
		return NULL;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_log(WLR_ERROR, "can't create pointer wlr_wl_input_device");
		free(dev);
		return NULL;
	case WLR_INPUT_DEVICE_TOUCH:
		type_name = "touch";
		break;
	case WLR_INPUT_DEVICE_TABLET_TOOL:
		type_name = "tablet-tool";
		break;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		type_name = "tablet-pad";
		break;
	default:
		wlr_log(WLR_ERROR, "device not handled");
		free(dev);
		return NULL;
	}

	size_t name_size = 8 + strlen(type_name) + strlen(seat->name) + 1;
	char name[name_size];
	(void) snprintf(name, name_size, "wayland-%s-%s", type_name, seat->name);

	wlr_input_device_init(wlr_dev, type, name);
	wl_list_insert(&seat->backend->devices, &dev->link);
	return dev;
}

void destroy_wl_input_device(struct wlr_wl_input_device *dev) {
	/**
	 * TODO remove the redundant wlr_input_device from wlr_wl_input_device
	 * wlr_wl_input_device::wlr_input_device is not owned by its input device
	 * type, which means we have 2 wlr_input_device to cleanup
	 */
	wlr_input_device_finish(&dev->wlr_input_device);
	if (dev->wlr_input_device._device) {
		struct wlr_input_device *wlr_dev = &dev->wlr_input_device;
		switch (wlr_dev->type) {
		case WLR_INPUT_DEVICE_KEYBOARD:
			wlr_log(WLR_ERROR, "wlr_wl_input_device has no keyboard");
			break;
		case WLR_INPUT_DEVICE_POINTER:
			wlr_log(WLR_ERROR, "wlr_wl_input_device has no pointer");
			break;
		case WLR_INPUT_DEVICE_TABLET_PAD:
			wlr_tablet_pad_finish(wlr_dev->tablet_pad);
			free(wlr_dev->tablet_pad);
			break;
		case WLR_INPUT_DEVICE_TABLET_TOOL:
			wlr_tablet_finish(wlr_dev->tablet);
			free(wlr_dev->tablet);
			break;
		case WLR_INPUT_DEVICE_TOUCH:
			wlr_touch_finish(wlr_dev->touch);
			free(wlr_dev->touch);
			break;
		default:
			break;
		}
	}
	wl_list_remove(&dev->link);
	free(dev);
}

void create_wl_touch(struct wlr_wl_seat *seat) {
	assert(seat->touch);
	struct wl_touch *wl_touch = seat->touch;
	struct wlr_wl_input_device *dev =
		create_wl_input_device(seat, WLR_INPUT_DEVICE_TOUCH);
	if (!dev) {
		return;
	}

	struct wlr_input_device *wlr_dev = &dev->wlr_input_device;

	wlr_dev->touch = calloc(1, sizeof(*wlr_dev->touch));
	if (!wlr_dev->touch) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		destroy_wl_input_device(dev);
		return;
	}
	wlr_touch_init(wlr_dev->touch, &touch_impl, wlr_dev->name);

	wl_touch_add_listener(wl_touch, &touch_listener, dev);
	wlr_signal_emit_safe(&seat->backend->backend.events.new_input, wlr_dev);
}

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		enum wl_seat_capability caps) {
	struct wlr_wl_seat *seat = data;
	struct wlr_wl_backend *backend = seat->backend;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && seat->wl_pointer == NULL) {
		wlr_log(WLR_DEBUG, "seat '%s' offering pointer", seat->name);

		seat->wl_pointer = wl_seat_get_pointer(wl_seat);
		init_seat_pointer(seat);
	}
	if (!(caps & WL_SEAT_CAPABILITY_POINTER) && seat->wl_pointer != NULL) {
		wlr_log(WLR_DEBUG, "seat '%s' dropping pointer", seat->name);
		finish_seat_pointer(seat);
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && seat->wl_keyboard == NULL) {
		wlr_log(WLR_DEBUG, "seat '%s' offering keyboard", seat->name);

		struct wl_keyboard *wl_keyboard = wl_seat_get_keyboard(wl_seat);
		seat->wl_keyboard = wl_keyboard;

		if (backend->started) {
			init_seat_keyboard(seat);
		}
	}
	if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && seat->wl_keyboard != NULL) {
		wlr_log(WLR_DEBUG, "seat '%s' dropping keyboard", seat->name);

		wl_keyboard_release(seat->wl_keyboard);
		wlr_keyboard_finish(&seat->wlr_keyboard);

		seat->wl_keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && seat->touch == NULL) {
		wlr_log(WLR_DEBUG, "seat %p offered touch", (void *)wl_seat);

		seat->touch = wl_seat_get_touch(wl_seat);
		if (backend->started) {
			create_wl_touch(seat);
		}
	}
	if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && seat->touch != NULL) {
		wlr_log(WLR_DEBUG, "seat %p dropped touch", (void *)wl_seat);

		struct wlr_wl_input_device *device, *tmp;
		wl_list_for_each_safe(device, tmp, &backend->devices, link) {
			if (device->wlr_input_device.type == WLR_INPUT_DEVICE_TOUCH) {
				destroy_wl_input_device(device);
			}
		}

		wl_touch_release(seat->touch);
		seat->touch = NULL;
	}
}

static void seat_handle_name(void *data, struct wl_seat *wl_seat,
		const char *name) {
	struct wlr_wl_seat *seat = data;
	free(seat->name);
	seat->name = strdup(name);
}

const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};

struct wl_seat *wlr_wl_input_device_get_seat(struct wlr_input_device *wlr_dev) {
	return input_device_get_seat(wlr_dev)->wl_seat;
}

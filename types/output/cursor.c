#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include "render/allocator/allocator.h"
#include "types/wlr_buffer.h"
#include "types/wlr_output.h"

static bool output_set_hardware_cursor(struct wlr_output *output,
		struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
	if (!output->impl->set_cursor) {
		return false;
	}

	if (!output->impl->set_cursor(output, buffer, hotspot_x, hotspot_y)) {
		return false;
	}

	wlr_buffer_unlock(output->cursor_front_buffer);
	output->cursor_front_buffer = NULL;

	if (buffer != NULL) {
		output->cursor_front_buffer = wlr_buffer_lock(buffer);
	}

	return true;
}

static void output_cursor_damage_whole(struct wlr_output_cursor *cursor);

void wlr_output_lock_software_cursors(struct wlr_output *output, bool lock) {
	if (lock) {
		++output->software_cursor_locks;
	} else {
		assert(output->software_cursor_locks > 0);
		--output->software_cursor_locks;
	}
	wlr_log(WLR_DEBUG, "%s hardware cursors on output '%s' (locks: %d)",
		lock ? "Disabling" : "Enabling", output->name,
		output->software_cursor_locks);

	if (output->software_cursor_locks > 0 && output->hardware_cursor != NULL) {
		output_set_hardware_cursor(output, NULL, 0, 0);
		output_cursor_damage_whole(output->hardware_cursor);
		output->hardware_cursor = NULL;
	}

	// If it's possible to use hardware cursors again, don't switch immediately
	// since a recorder is likely to lock software cursors for the next frame
	// again.
}

static void output_scissor(struct wlr_output *output, pixman_box32_t *rect) {
	struct wlr_renderer *renderer = output->renderer;
	assert(renderer);

	struct wlr_box box = {
		.x = rect->x1,
		.y = rect->y1,
		.width = rect->x2 - rect->x1,
		.height = rect->y2 - rect->y1,
	};

	int ow, oh;
	wlr_output_transformed_resolution(output, &ow, &oh);

	enum wl_output_transform transform =
		wlr_output_transform_invert(output->transform);
	wlr_box_transform(&box, &box, transform, ow, oh);

	wlr_renderer_scissor(renderer, &box);
}

/**
 * Returns the cursor box, scaled for its output.
 */
static void output_cursor_get_box(struct wlr_output_cursor *cursor,
		struct wlr_box *box) {
	box->x = cursor->x - cursor->hotspot_x;
	box->y = cursor->y - cursor->hotspot_y;
	box->width = cursor->width;
	box->height = cursor->height;
}

static void output_cursor_render(struct wlr_output_cursor *cursor,
		pixman_region32_t *damage) {
	struct wlr_renderer *renderer = cursor->output->renderer;
	assert(renderer);

	struct wlr_texture *texture = cursor->texture;
	if (texture == NULL) {
		return;
	}

	struct wlr_box box;
	output_cursor_get_box(cursor, &box);

	pixman_region32_t surface_damage;
	pixman_region32_init(&surface_damage);
	pixman_region32_union_rect(&surface_damage, &surface_damage, box.x, box.y,
		box.width, box.height);
	pixman_region32_intersect(&surface_damage, &surface_damage, damage);
	if (!pixman_region32_not_empty(&surface_damage)) {
		goto surface_damage_finish;
	}

	float matrix[9];
	wlr_matrix_project_box(matrix, &box, WL_OUTPUT_TRANSFORM_NORMAL, 0,
		cursor->output->transform_matrix);

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(&surface_damage, &nrects);
	for (int i = 0; i < nrects; ++i) {
		output_scissor(cursor->output, &rects[i]);
		wlr_render_texture_with_matrix(renderer, texture, matrix, 1.0f);
	}
	wlr_renderer_scissor(renderer, NULL);

surface_damage_finish:
	pixman_region32_fini(&surface_damage);
}

void wlr_output_render_software_cursors(struct wlr_output *output,
		const pixman_region32_t *damage) {
	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);

	pixman_region32_t render_damage;
	pixman_region32_init(&render_damage);
	pixman_region32_union_rect(&render_damage, &render_damage, 0, 0,
		width, height);
	if (damage != NULL) {
		// Damage tracking supported
		pixman_region32_intersect(&render_damage, &render_damage, damage);
	}

	if (pixman_region32_not_empty(&render_damage)) {
		struct wlr_output_cursor *cursor;
		wl_list_for_each(cursor, &output->cursors, link) {
			if (!cursor->enabled || !cursor->visible ||
					output->hardware_cursor == cursor) {
				continue;
			}
			output_cursor_render(cursor, &render_damage);
		}
	}

	pixman_region32_fini(&render_damage);
}

void wlr_output_add_software_cursors_to_render_pass(struct wlr_output *output,
		struct wlr_render_pass *render_pass, const pixman_region32_t *damage) {
	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);

	pixman_region32_t render_damage;
	pixman_region32_init_rect(&render_damage, 0, 0, width, height);
	if (damage != NULL) {
		pixman_region32_intersect(&render_damage, &render_damage, damage);
	}

	struct wlr_output_cursor *cursor;
	wl_list_for_each(cursor, &output->cursors, link) {
		if (!cursor->enabled || !cursor->visible ||
				output->hardware_cursor == cursor) {
			continue;
		}

		struct wlr_texture *texture = cursor->texture;
		if (texture == NULL) {
			continue;
		}

		struct wlr_box box;
		output_cursor_get_box(cursor, &box);

		pixman_region32_t cursor_damage;
		pixman_region32_init_rect(&cursor_damage, box.x, box.y, box.width, box.height);
		pixman_region32_intersect(&cursor_damage, &cursor_damage, &render_damage);
		if (!pixman_region32_not_empty(&cursor_damage)) {
			pixman_region32_fini(&cursor_damage);
			continue;
		}

		enum wl_output_transform transform =
			wlr_output_transform_invert(output->transform);
		wlr_box_transform(&box, &box, transform, width, height);
		wlr_region_transform(&cursor_damage, &cursor_damage, transform, width, height);

		wlr_render_pass_add_texture(render_pass, &(struct wlr_render_texture_options) {
			.texture = texture,
			.dst_box = box,
			.clip = &cursor_damage,
			.transform = output->transform,
		});

		pixman_region32_fini(&cursor_damage);
	}

	pixman_region32_fini(&render_damage);
}

static void output_cursor_damage_whole(struct wlr_output_cursor *cursor) {
	struct wlr_box box;
	output_cursor_get_box(cursor, &box);

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, box.x, box.y, box.width, box.height);

	struct wlr_output_event_damage event = {
		.output = cursor->output,
		.damage = &damage,
	};
	wl_signal_emit_mutable(&cursor->output->events.damage, &event);

	pixman_region32_fini(&damage);
}

static void output_cursor_reset(struct wlr_output_cursor *cursor) {
	if (cursor->output->hardware_cursor != cursor) {
		output_cursor_damage_whole(cursor);
	}
}

static void output_cursor_update_visible(struct wlr_output_cursor *cursor) {
	struct wlr_box output_box;
	output_box.x = output_box.y = 0;
	wlr_output_transformed_resolution(cursor->output, &output_box.width,
		&output_box.height);

	struct wlr_box cursor_box;
	output_cursor_get_box(cursor, &cursor_box);

	struct wlr_box intersection;
	cursor->visible =
		wlr_box_intersection(&intersection, &output_box, &cursor_box);
}

static bool output_pick_cursor_format(struct wlr_output *output,
		struct wlr_drm_format *format) {
	struct wlr_allocator *allocator = output->allocator;
	assert(allocator != NULL);

	const struct wlr_drm_format_set *display_formats = NULL;
	if (output->impl->get_cursor_formats) {
		display_formats =
			output->impl->get_cursor_formats(output, allocator->buffer_caps);
		if (display_formats == NULL) {
			wlr_log(WLR_DEBUG, "Failed to get cursor display formats");
			return false;
		}
	}

	return output_pick_format(output, display_formats, format, DRM_FORMAT_ARGB8888);
}

static struct wlr_buffer *render_cursor_buffer(struct wlr_output_cursor *cursor) {
	struct wlr_output *output = cursor->output;

	float scale = output->scale;
	enum wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
	struct wlr_texture *texture = cursor->texture;
	if (texture == NULL) {
		return NULL;
	}

	struct wlr_allocator *allocator = output->allocator;
	struct wlr_renderer *renderer = output->renderer;
	assert(allocator != NULL && renderer != NULL);

	int width = texture->width * output->scale / scale;
	int height = texture->height * output->scale / scale;
	if (output->impl->get_cursor_size) {
		// Apply hardware limitations on buffer size
		output->impl->get_cursor_size(cursor->output, &width, &height);
		if ((int)texture->width > width || (int)texture->height > height) {
			wlr_log(WLR_DEBUG, "Cursor texture too large (%dx%d), "
				"exceeds hardware limitations (%dx%d)", texture->width,
				texture->height, width, height);
			return NULL;
		}
	}

	if (output->cursor_swapchain == NULL ||
			output->cursor_swapchain->width != width ||
			output->cursor_swapchain->height != height) {
		struct wlr_drm_format format = {0};
		if (!output_pick_cursor_format(output, &format)) {
			wlr_log(WLR_DEBUG, "Failed to pick cursor format");
			return NULL;
		}

		wlr_swapchain_destroy(output->cursor_swapchain);
		output->cursor_swapchain = wlr_swapchain_create(allocator,
			width, height, &format);
		wlr_drm_format_finish(&format);
		if (output->cursor_swapchain == NULL) {
			wlr_log(WLR_ERROR, "Failed to create cursor swapchain");
			return NULL;
		}
	}

	struct wlr_buffer *buffer =
		wlr_swapchain_acquire(output->cursor_swapchain, NULL);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_box cursor_box = {
		.width = texture->width * output->scale / scale,
		.height = texture->height * output->scale / scale,
	};

	float output_matrix[9];
	wlr_matrix_identity(output_matrix);
	if (output->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		struct wlr_box tr_size = {
			.width = buffer->width,
			.height = buffer->height,
		};
		wlr_box_transform(&tr_size, &tr_size, output->transform, 0, 0);

		wlr_matrix_translate(output_matrix, buffer->width / 2.0,
			buffer->height / 2.0);
		wlr_matrix_transform(output_matrix, output->transform);
		wlr_matrix_translate(output_matrix, - tr_size.width / 2.0,
			- tr_size.height / 2.0);
	}

	float matrix[9];
	wlr_matrix_project_box(matrix, &cursor_box, transform, 0, output_matrix);

	if (!wlr_renderer_begin_with_buffer(renderer, buffer)) {
		wlr_buffer_unlock(buffer);
		return NULL;
	}

	wlr_renderer_clear(renderer, (float[]){ 0.0, 0.0, 0.0, 0.0 });
	wlr_render_texture_with_matrix(renderer, texture, matrix, 1.0);

	wlr_renderer_end(renderer);

	return buffer;
}

static bool output_cursor_attempt_hardware(struct wlr_output_cursor *cursor) {
	struct wlr_output *output = cursor->output;

	if (!output->impl->set_cursor ||
			output->software_cursor_locks > 0) {
		return false;
	}

	struct wlr_output_cursor *hwcur = output->hardware_cursor;
	if (hwcur != NULL && hwcur != cursor) {
		return false;
	}

	struct wlr_texture *texture = cursor->texture;

	// If the cursor was hidden or was a software cursor, the hardware
	// cursor position is outdated
	output->impl->move_cursor(cursor->output,
		(int)cursor->x, (int)cursor->y);

	struct wlr_buffer *buffer = NULL;
	if (texture != NULL) {
		buffer = render_cursor_buffer(cursor);
		if (buffer == NULL) {
			wlr_log(WLR_DEBUG, "Failed to render cursor buffer");
			return false;
		}
	}

	struct wlr_box hotspot = {
		.x = cursor->hotspot_x,
		.y = cursor->hotspot_y,
	};
	wlr_box_transform(&hotspot, &hotspot,
		wlr_output_transform_invert(output->transform),
		buffer ? buffer->width : 0, buffer ? buffer->height : 0);

	bool ok = output_set_hardware_cursor(output, buffer, hotspot.x, hotspot.y);
	wlr_buffer_unlock(buffer);
	if (ok) {
		output->hardware_cursor = cursor;
	}
	return ok;
}

bool wlr_output_cursor_set_image(struct wlr_output_cursor *cursor,
		const uint8_t *pixels, int32_t stride, uint32_t width, uint32_t height,
		int32_t hotspot_x, int32_t hotspot_y) {
	struct wlr_buffer *buffer = NULL;

	if (pixels) {
		struct wlr_readonly_data_buffer *ro_buffer = readonly_data_buffer_create(
			DRM_FORMAT_ARGB8888, stride, width, height, pixels);
		if (ro_buffer == NULL) {
			return false;
		}
		buffer = &ro_buffer->base;
	}
	bool ok = wlr_output_cursor_set_buffer(cursor, buffer, hotspot_x, hotspot_y);

	wlr_buffer_drop(buffer);
	return ok;
}

bool wlr_output_cursor_set_buffer(struct wlr_output_cursor *cursor,
		struct wlr_buffer *buffer, int32_t hotspot_x, int32_t hotspot_y) {
	struct wlr_renderer *renderer = cursor->output->renderer;
	if (!renderer) {
		return false;
	}

	struct wlr_texture *texture = NULL;
	if (buffer != NULL) {
		texture = wlr_texture_from_buffer(renderer, buffer);
		if (texture == NULL) {
			return false;
		}
	}

	return output_cursor_set_texture(cursor, texture, true, 1,
		WL_OUTPUT_TRANSFORM_NORMAL, hotspot_x, hotspot_y);
}

bool output_cursor_set_texture(struct wlr_output_cursor *cursor,
		struct wlr_texture *texture, bool own_texture, float scale,
		enum wl_output_transform transform, int32_t hotspot_x, int32_t hotspot_y) {
	output_cursor_reset(cursor);

	cursor->enabled = texture != NULL;
	if (texture != NULL) {
		struct wlr_box box = { .width = texture->width, .height = texture->height };
		wlr_box_transform(&box, &box, wlr_output_transform_invert(transform), 0, 0);
		cursor->width = (int)roundf(box.width * scale);
		cursor->height = (int)roundf(box.height * scale);
	} else {
		cursor->width = 0;
		cursor->height = 0;
	}

	cursor->hotspot_x = (int)roundf(hotspot_x * scale);
	cursor->hotspot_y = (int)roundf(hotspot_y * scale);

	output_cursor_update_visible(cursor);

	if (cursor->own_texture) {
		wlr_texture_destroy(cursor->texture);
	}
	cursor->texture = texture;
	cursor->own_texture = own_texture;

	if (output_cursor_attempt_hardware(cursor)) {
		return true;
	}

	wlr_log(WLR_DEBUG, "Falling back to software cursor on output '%s'",
		cursor->output->name);
	output_cursor_damage_whole(cursor);
	return true;
}

bool wlr_output_cursor_move(struct wlr_output_cursor *cursor,
		double x, double y) {
	// Scale coordinates for the output
	x *= cursor->output->scale;
	y *= cursor->output->scale;

	if (cursor->x == x && cursor->y == y) {
		return true;
	}

	if (cursor->output->hardware_cursor != cursor) {
		output_cursor_damage_whole(cursor);
	}

	cursor->x = x;
	cursor->y = y;
	bool was_visible = cursor->visible;
	output_cursor_update_visible(cursor);

	if (!was_visible && !cursor->visible) {
		// Cursor is still hidden, do nothing
		return true;
	}

	if (cursor->output->hardware_cursor != cursor) {
		output_cursor_damage_whole(cursor);
		return true;
	}

	assert(cursor->output->impl->move_cursor);
	return cursor->output->impl->move_cursor(cursor->output, (int)x, (int)y);
}

struct wlr_output_cursor *wlr_output_cursor_create(struct wlr_output *output) {
	struct wlr_output_cursor *cursor =
		calloc(1, sizeof(struct wlr_output_cursor));
	if (cursor == NULL) {
		return NULL;
	}
	cursor->output = output;
	wl_list_insert(&output->cursors, &cursor->link);
	cursor->visible = true; // default position is at (0, 0)
	return cursor;
}

void wlr_output_cursor_destroy(struct wlr_output_cursor *cursor) {
	if (cursor == NULL) {
		return;
	}
	output_cursor_reset(cursor);
	if (cursor->output->hardware_cursor == cursor) {
		// If this cursor was the hardware cursor, disable it
		output_set_hardware_cursor(cursor->output, NULL, 0, 0);
		cursor->output->hardware_cursor = NULL;
	}
	if (cursor->own_texture) {
		wlr_texture_destroy(cursor->texture);
	}
	wl_list_remove(&cursor->link);
	free(cursor);
}

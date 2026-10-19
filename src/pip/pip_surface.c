#include "mango/pip/pip.h"

#include "mango/animation/common.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/config/parse.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include <scenefx/types/wlr_scene.h>
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>

/* One node per surface of the mirrored client (root plus subsurfaces). The
 * layout is the same trick the overview cards use: a private scene tree that
 * renders the client's surfaces scaled into an arbitrary rectangle, while the
 * client itself keeps its real state (src/overview/overview.c:15-96). */
struct pip_surface {
	Client *c;
	struct wlr_surface *surface;
	struct wlr_scene_surface *scene_surface;
	struct wlr_scene_buffer *buffer;
	int sx, sy; /* coordinates relative to the root surface */
	bool is_root;
	struct wl_list link;
	struct wl_listener commit;
	struct wl_listener destroy;
};

void pip_surface_layout(Client *c);

static void pip_surface_commit(struct wl_listener *listener, void *data) {
	struct pip_surface *entry = wl_container_of(listener, entry, commit);
	if (entry->c && entry->c->pip_tree)
		pip_surface_layout(entry->c);
}

static void pip_surface_destroy(struct wl_listener *listener, void *data) {
	struct pip_surface *entry = wl_container_of(listener, entry, destroy);
	wl_list_remove(&entry->link);
	wl_list_remove(&entry->commit.link);
	wl_list_remove(&entry->destroy.link);
	free(entry);
}

static void pip_surface_add(struct wlr_surface *surface, int sx, int sy,
							void *data) {
	Client *c = data;
	if (!c->pip_tree)
		return;

	struct pip_surface *entry = ecalloc(1, sizeof(*entry));
	entry->c = c;
	entry->surface = surface;
	entry->sx = sx;
	entry->sy = sy;
	entry->is_root = surface == client_surface(c);

	entry->scene_surface = wlr_scene_surface_create(c->pip_tree, surface);
	if (!entry->scene_surface) {
		free(entry);
		return;
	}
	entry->buffer = entry->scene_surface->buffer;
	wlr_scene_buffer_set_filter_mode(entry->buffer, WLR_SCALE_FILTER_BILINEAR);

	entry->commit.notify = pip_surface_commit;
	wl_signal_add(&surface->events.commit, &entry->commit);
	entry->destroy.notify = pip_surface_destroy;
	wl_signal_add(&surface->events.destroy, &entry->destroy);

	wl_list_insert(&c->pip_surfaces, &entry->link);
}

/* Aspect-preserving box in the configured corner of the client's monitor. */
static bool pip_target_box(Client *c, struct wlr_box *out) {
	Monitor *m = c->mon;
	if (!m)
		return false;

	struct wlr_box clip;
	client_get_clip(c, &clip);
	if (clip.width <= 0 || clip.height <= 0)
		return false;

	int32_t width = (int32_t)(config.pip_width_ratio * (float)m->m.width);
	if (width < 64)
		width = 64;
	int32_t height = (int32_t)((double)width * clip.height / clip.width);
	int32_t max_height = m->m.height / 2;
	if (height > max_height) {
		height = max_height;
		width = (int32_t)((double)height * clip.width / clip.height);
	}

	int32_t margin = config.pip_margin;
	if (margin < 0)
		margin = 0;
	int32_t x, y;
	if (c->pip_moved) {
		/* Dragged: keep the position, clamped so the whole frame stays on the
		 * monitor. */
		int32_t bw = (int32_t)config.borderpx;
		x = MANGO_MAX(m->m.x + bw,
					  MANGO_MIN(m->m.x + m->m.width - bw - width, c->pip_x));
		y = MANGO_MAX(m->m.y + bw,
					  MANGO_MIN(m->m.y + m->m.height - bw - height, c->pip_y));
	} else {
		switch (config.pip_corner) {
		case PIP_TOPLEFT:
			x = m->m.x + margin;
			y = m->m.y + margin;
			break;
		case PIP_TOPRIGHT:
			x = m->m.x + m->m.width - margin - width;
			y = m->m.y + margin;
			break;
		case PIP_BOTTOMLEFT:
			x = m->m.x + margin;
			y = m->m.y + m->m.height - margin - height;
			break;
		default:
			x = m->m.x + m->m.width - margin - width;
			y = m->m.y + m->m.height - margin - height;
			break;
		}
	}

	*out = (struct wlr_box){
		.x = x, .y = y, .width = width, .height = height};
	return true;
}

static void pip_content_size(Client *c, float *width, float *height) {
	struct wlr_surface *surface = client_surface(c);
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		*width = surface->current.width;
		*height = surface->current.height;
		return;
	}
#endif
	*width = c->surface.xdg->geometry.width;
	*height = c->surface.xdg->geometry.height;
}

void pip_surface_layout(Client *c) {
	if (!c || !c->pip_tree)
		return;

	struct wlr_box rect;
	if (!pip_target_box(c, &rect))
		return;
	c->pip_rect = rect;

	struct wlr_box clip;
	client_get_clip(c, &clip);

	float content_w, content_h;
	pip_content_size(c, &content_w, &content_h);
	if (content_w <= 0 || content_h <= 0)
		return;

	float scale = MANGO_MIN((float)rect.width / content_w,
							(float)rect.height / content_h);
	int32_t dest_w = (int32_t)(content_w * scale);
	int32_t dest_h = (int32_t)(content_h * scale);
	if (dest_w <= 0 || dest_h <= 0)
		return;

	int32_t ox = rect.x + (rect.width - dest_w) / 2;
	int32_t oy = rect.y + (rect.height - dest_h) / 2;
	wlr_scene_node_set_position(&c->pip_tree->node, ox, oy);

	struct pip_surface *entry;
	wl_list_for_each(entry, &c->pip_surfaces, link) {
		struct wlr_surface *surface = entry->surface;
		float lw = surface->current.width;
		float lh = surface->current.height;

		/* Buffer pixels per unit of surface size (viewporter/buffer scale). */
		struct wlr_fbox bsrc = {0};
		wlr_surface_get_buffer_source_box(surface, &bsrc);
		float ratio_x =
			surface->current.width > 0 ? bsrc.width / surface->current.width
									   : 1.0f;
		float ratio_y =
			surface->current.height > 0 ? bsrc.height / surface->current.height
										: 1.0f;

		if (entry->is_root) {
			wlr_scene_node_set_position(&entry->buffer->node, 0, 0);
			wlr_scene_buffer_set_dest_size(entry->buffer, dest_w, dest_h);
			struct wlr_fbox src = {
				.x = bsrc.x + clip.x * ratio_x,
				.y = bsrc.y + clip.y * ratio_y,
				.width = content_w * ratio_x,
				.height = content_h * ratio_y,
			};
			wlr_scene_buffer_set_source_box(entry->buffer, &src);
			continue;
		}

		int32_t px = (int32_t)((entry->sx - clip.x) * scale);
		int32_t py = (int32_t)((entry->sy - clip.y) * scale);
		int32_t dw = (int32_t)(lw * scale);
		int32_t dh = (int32_t)(lh * scale);

		/* Subsurfaces are cropped to the PiP content box. */
		int32_t cx0 = MANGO_MAX(px, 0);
		int32_t cy0 = MANGO_MAX(py, 0);
		int32_t cx1 = MANGO_MIN(px + dw, dest_w);
		int32_t cy1 = MANGO_MIN(py + dh, dest_h);
		int32_t cw = cx1 - cx0;
		int32_t ch = cy1 - cy0;
		if (cw <= 0 || ch <= 0) {
			wlr_scene_node_set_position(&entry->buffer->node, 0, 0);
			wlr_scene_buffer_set_dest_size(entry->buffer, 0, 0);
			continue;
		}

		float off_x = (float)(cx0 - px) / scale;
		float off_y = (float)(cy0 - py) / scale;
		wlr_scene_node_set_position(&entry->buffer->node, cx0, cy0);
		wlr_scene_buffer_set_dest_size(entry->buffer, cw, ch);
		struct wlr_fbox src = {
			.x = bsrc.x + off_x * ratio_x,
			.y = bsrc.y + off_y * ratio_y,
			.width = ((float)cw / scale) * ratio_x,
			.height = ((float)ch / scale) * ratio_y,
		};
		wlr_scene_buffer_set_source_box(entry->buffer, &src);
	}

	struct fx_corner_radii corners = corner_radii_all(config.border_radius);
	wl_list_for_each(entry, &c->pip_surfaces, link)
		wlr_scene_buffer_set_corner_radii(entry->buffer, corners);

	/* WM-style frame: shadow below, border above it, content above both. Both
	 * nodes are created before the surface nodes, so the tree order is already
	 * correct and no raise/lower is needed per commit. */
	int32_t bw = (int32_t)config.borderpx;
	if (c->pip_shadow) {
		if (config.shadows) {
			int32_t delta = (int32_t)config.shadows_size + bw;
			wlr_scene_node_set_enabled(&c->pip_shadow->node, true);
			wlr_scene_node_set_position(
				&c->pip_shadow->node,
				-delta + config.shadows_position_x,
				-delta + config.shadows_position_y);
			wlr_scene_shadow_set_size(c->pip_shadow, dest_w + 2 * delta,
									  dest_h + 2 * delta);
			wlr_scene_shadow_set_corner_radius(c->pip_shadow,
											   config.border_radius);
			wlr_scene_shadow_set_blur_sigma(c->pip_shadow,
											config.shadows_blur);
			wlr_scene_shadow_set_color(c->pip_shadow, config.shadowscolor);
		} else {
			wlr_scene_node_set_enabled(&c->pip_shadow->node, false);
		}
	}
	if (c->pip_border) {
		if (bw > 0) {
			wlr_scene_node_set_enabled(&c->pip_border->node, true);
			wlr_scene_node_set_position(&c->pip_border->node, -bw, -bw);
			wlr_scene_rect_set_size(c->pip_border, dest_w + 2 * bw,
									dest_h + 2 * bw);
			wlr_scene_rect_set_corner_radius(c->pip_border,
											 config.border_radius);
			wlr_scene_rect_set_color(c->pip_border, config.bordercolor);
		} else {
			wlr_scene_node_set_enabled(&c->pip_border->node, false);
		}
	}
}

void pip_surface_build(Client *c) {
	struct wlr_surface *surface = client_surface(c);
	if (!surface || !surface->mapped)
		return;

	c->pip_shadow = wlr_scene_shadow_create(
		c->pip_tree, 0, 0, config.border_radius, config.shadows_blur,
		config.shadowscolor);
	c->pip_border = wlr_scene_rect_create(c->pip_tree, 0, 0,
										  config.bordercolor);
	wlr_surface_for_each_surface(surface, pip_surface_add, c);
}

void pip_surface_clear(Client *c) {
	struct pip_surface *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &c->pip_surfaces, link) {
		wl_list_remove(&entry->commit.link);
		wl_list_remove(&entry->destroy.link);
		wl_list_remove(&entry->link);
		free(entry);
	}
}

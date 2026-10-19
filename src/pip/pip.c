#include "mango/pip/pip.h"

#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/config/parse.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include <scenefx/types/wlr_scene.h>
#include <string.h>
#include <time.h>

int32_t pip_corner_from_string(const char *value) {
	if (!value)
		return PIP_BOTTOMRIGHT;
	if (strcmp(value, "topleft") == 0)
		return PIP_TOPLEFT;
	if (strcmp(value, "topright") == 0)
		return PIP_TOPRIGHT;
	if (strcmp(value, "bottomleft") == 0)
		return PIP_BOTTOMLEFT;
	return PIP_BOTTOMRIGHT;
}

void pip_init(void) {
	server.pip_tree = wlr_scene_tree_create(&server.scene->tree);
	if (!server.pip_tree)
		return;
	/* Above every window layer, below the layer-shell overlay surfaces
	 * (notifications, OSD, the switcher). */
	wlr_scene_node_place_below(&server.pip_tree->node,
							   &server.layers_wrap[LyrSpecialOverlay]->node);
}

void pip_enter(Client *c) {
	if (!c || !c->mon || c->is_pip || !server.pip_tree)
		return;

	c->pip_tree = wlr_scene_tree_create(server.pip_tree);
	if (!c->pip_tree)
		return;

	c->is_pip = 1;
	pip_surface_build(c);
	pip_surface_layout(c);

	/* Nothing to draw (no mapped surface, or no usable geometry): abort instead
	 * of cutting the window out of the tag with an empty copy. */
	if (wl_list_empty(&c->pip_surfaces) || c->pip_rect.width <= 0) {
		pip_exit(c);
		return;
	}

	/* Kicks the client's frame loop: from here on the PiP nodes drive it. */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	client_send_frame_done(c, &now);
}

void pip_exit(Client *c) {
	if (!c || !c->pip_tree)
		return;

	pip_surface_clear(c);
	wlr_scene_node_destroy(&c->pip_tree->node);
	c->pip_tree = NULL;
	c->pip_border = NULL;
	c->pip_shadow = NULL;
	c->is_pip = 0;
	c->pip_moved = false;
	c->pip_rect = (struct wlr_box){0};
}

void pip_config_changed(void) {
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (!c->is_pip)
			continue;
		if (!config.pip_enable)
			pip_exit(c);
		else
			pip_surface_layout(c);
	}
}

void pip_destroy(Client *c) { pip_exit(c); }

void pip_toggle(Client *c) {
	if (!c)
		return;
	if (c->is_pip) {
		pip_exit(c);
		return;
	}
	if (!c->isfullscreen || !c->mon || !config.pip_enable)
		return;
	/* Never mirror a window that is already on screen. */
	if (VISIBLEON(c, c->mon))
		return;
	pip_enter(c);
}

void pip_apply_view(Monitor *m, uint32_t old_tagset) {
	if (!m)
		return;

	uint32_t new_tagset = m->tagset[m->seltags];
	Client *c;

	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m || !c->is_pip)
			continue;
		/* Back on screen, or no longer fullscreen: the copy is not needed. */
		if ((c->tags & new_tagset) || !c->isfullscreen)
			pip_exit(c);
	}

	if (!config.pip_enable || m->isoverview)
		return;

	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m || c->is_pip || !c->isfullscreen || c->isminimized)
			continue;
		if (!(c->tags & old_tagset) || (c->tags & new_tagset))
			continue;
		pip_enter(c);
		break; /* one PiP per monitor */
	}
}

void pip_set_overview(Monitor *m, bool enter) {
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m || !c->pip_tree)
			continue;
		wlr_scene_node_set_enabled(&c->pip_tree->node, !enter);
	}
}

Client *pip_client_at(double x, double y) {
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (!c->is_pip || !c->pip_tree || !c->pip_tree->node.enabled)
			continue;
		struct wlr_box *r = &c->pip_rect;
		if (x >= r->x && x < r->x + r->width && y >= r->y &&
			y < r->y + r->height)
			return c;
	}
	return NULL;
}

bool pip_pointer_hit(double x, double y) { return pip_client_at(x, y) != NULL; }

void pip_activate(Client *c) {
	if (!c || !c->mon)
		return;
	client_switch_view(&(Arg){.ui = c->tags}, true);
	client_focus(c, 1);
}

/* ---- dragging the copy ---------------------------------------------------- */

static Client *drag_client;
static double drag_grab_x, drag_grab_y; /* cursor offset inside the box */
static int32_t drag_start_x, drag_start_y;
static bool drag_moved;

bool pip_drag_active(void) { return drag_client != NULL; }

bool pip_drag_begin(Client *c, double x, double y) {
	if (!c || !c->is_pip || c->pip_rect.width <= 0)
		return false;
	drag_client = c;
	drag_grab_x = x - c->pip_rect.x;
	drag_grab_y = y - c->pip_rect.y;
	drag_start_x = c->pip_rect.x;
	drag_start_y = c->pip_rect.y;
	drag_moved = false;
	return true;
}

bool pip_drag_update(double x, double y) {
	Client *c = drag_client;
	if (!c)
		return false;

	if (!drag_moved) {
		double dx = x - (drag_start_x + drag_grab_x);
		double dy = y - (drag_start_y + drag_grab_y);
		/* Same 5 px threshold the group bar uses before it starts a move. */
		if (dx * dx + dy * dy < 25.0)
			return true;
		drag_moved = true;
	}

	Monitor *m = c->mon;
	if (!m)
		return true;
	int32_t bw = (int32_t)config.borderpx;
	int32_t min_x = m->m.x + bw;
	int32_t min_y = m->m.y + bw;
	int32_t max_x = m->m.x + m->m.width - bw - c->pip_rect.width;
	int32_t max_y = m->m.y + m->m.height - bw - c->pip_rect.height;
	int32_t nx = (int32_t)(x - drag_grab_x);
	int32_t ny = (int32_t)(y - drag_grab_y);
	c->pip_x = MANGO_MAX(min_x, MANGO_MIN(max_x, nx));
	c->pip_y = MANGO_MAX(min_y, MANGO_MIN(max_y, ny));
	c->pip_moved = true;
	pip_surface_layout(c);
	return true;
}

void pip_drag_end(bool activate) {
	Client *c = drag_client;
	bool moved = drag_moved;
	drag_client = NULL;
	drag_moved = false;
	if (!c)
		return;
	/* A press that never moved is the click that brings the window back. */
	if (activate && !moved)
		pip_activate(c);
}

/* ---- state for `mmsg get pip` --------------------------------------------- */

bool pip_enabled_value(void) { return config.pip_enable != 0; }

int32_t pip_active_count(void) {
	int32_t count = 0;
	Client *c;
	wl_list_for_each(c, &server.clients, link)
		if (c->is_pip)
			count++;
	return count;
}

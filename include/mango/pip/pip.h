#ifndef __PIP_PIP_H__
#define __PIP_PIP_H__ 1

#include "mango/common/types.h"
#include <stdbool.h>
#include <stdint.h>

/* Corner of the monitor where the picture-in-picture window is placed. */
enum { PIP_TOPLEFT, PIP_TOPRIGHT, PIP_BOTTOMLEFT, PIP_BOTTOMRIGHT };

/* Parses pip_corner ("topleft", "topright", "bottomleft", "bottomright"). */
int32_t pip_corner_from_string(const char *value);

/* Creates the scene layer that hosts every PiP tree (called once at startup). */
void pip_init(void);

/* Enters/exits PiP for a client. Entering keeps every client-side state (tag,
 * fullscreen, geometry) untouched: only a scaled copy of its surface tree is
 * rendered in the corner. */
void pip_enter(Client *c);
void pip_exit(Client *c);
void pip_toggle(Client *c);

/* Called by client_view_on_monitor before arrange: enters PiP for the fullscreen
 * window that the new view no longer shows and exits the ones that came back. */
void pip_apply_view(Monitor *m, uint32_t old_tagset);

/* Hides/restores the PiP trees of a monitor while its overview is open. */
void pip_set_overview(Monitor *m, bool enter);

/* Pointer helpers: the PiP is drawn outside the layer hit-test trees on
 * purpose, so input has to resolve it through these. */
Client *pip_client_at(double x, double y);
bool pip_pointer_hit(double x, double y);
void pip_activate(Client *c);

/* Dragging the copy: press on the PiP starts a potential drag, motion past the
 * click threshold moves it (clamped to its monitor), release without motion is
 * a click that brings the window back. */
bool pip_drag_begin(Client *c, double x, double y);
bool pip_drag_update(double x, double y);
void pip_drag_end(bool activate);
bool pip_drag_active(void);

/* After a config reload: drop the copies when pip_enable went off, otherwise
 * re-lay them out with the new size/margin/corner. */
void pip_config_changed(void);

/* State for `mmsg get pip` (the shell uses it as a feature probe). */
bool pip_enabled_value(void);
int32_t pip_active_count(void);

/* Tears down the PiP tree of a client (client teardown / property reset). */
void pip_destroy(Client *c);

/* Internal to the module: the scaled surface tree (src/pip/pip_surface.c). */
void pip_surface_build(Client *c);
void pip_surface_layout(Client *c);
void pip_surface_clear(Client *c);

#endif

---
title: Picture-in-Picture
description: Keep a fullscreen window visible in the corner while you work on another tag.
---

A fullscreen window belongs to one tag. When you switch away from that tag, mangowm
can keep it on screen as a small copy in the corner of the tag you just switched
to — and of every other tag you visit until you come back.

The window itself is never touched: it stays fullscreen, keeps its geometry, its
tag and its client-side state. Only a scaled copy of its surface tree is drawn,
so applications are not asked to resize and cannot "re-enter fullscreen" behind
your back.

## Configuration

```ini
pip_enable=1
pip_width_ratio=0.25
pip_margin=24
pip_corner=bottomright
```

| Option | Values | Description |
| :--- | :--- | :--- |
| `pip_enable` | `0` / `1` | Enables the automatic PiP on tag switches. Default `0`. |
| `pip_width_ratio` | `0.05` – `0.5` | PiP width as a fraction of the monitor width. Default `0.25`. |
| `pip_margin` | `0` – `200` | Distance in pixels from the monitor edges. Default `24`. |
| `pip_corner` | `topleft` / `topright` / `bottomleft` / `bottomright` | Corner of the monitor. Default `bottomright`. |

The height follows the window's aspect ratio and is capped at half the monitor
height, so a portrait window does not fill the screen again.

## Behavior

- **Enter:** switching the active tag of a monitor away from the tag of a
  *fullscreen* window (and only a fullscreen one — maximized and fake-fullscreen
  windows are ignored) mirrors it in the configured corner. The tag-out animation
  of that window is cut: the copy appears where the window was, instead of the
  window sliding away behind it.
- **Exit:** switching back to the window's tag destroys the copy and the real
  window is shown again, with the normal tag-in animation. The copy also
  disappears if the client leaves fullscreen by itself, closes, or is moved to
  the view that is currently on screen.
- **One per monitor.** Two fullscreen windows on different tags of the same
  monitor do not stack copies in the same corner; the first matching one wins.
- **Click brings the window back.** Left-clicking the copy switches to its tag
  and focuses it. Hovering the copy does not send pointer events to the hidden
  window or to whatever is drawn behind the copy.
- **Overview** hides the copies; the overview cards already show every window.
- **Manual toggle:** `mmsg dispatch toggle_pip client,<id>` (or a bind) mirrors
  the focused client when it is fullscreen and not currently visible, and drops
  its copy.

## How it works

The copy reuses the machinery the overview cards use to draw a window into an
arbitrary rectangle: a private `wlr_scene_tree` holding one scaled
`wlr_scene_surface` per surface of the client, root first and then every
subsurface, with bilinear filtering and the theme's corner radius
(`src/overview/overview.c:68-241` is the original; `src/pip/pip.c` is the PiP
variant).

- The tree lives in `server.pip_tree`, a layer placed above the window layers and
  below the layer-shell overlay surfaces (notifications, OSD, the switcher), so
  the copy draws over tiled/floating/fullscreen windows but never over shell
  panels.
- Every surface commit re-applies the scale (`wlr_scene_surface` resets dest size
  and source box on commit), which is what keeps a video moving.
- The copy is not part of the layer hit-test trees. Pointer input resolves it by
  the box stored in `Client.pip_rect` instead (`src/input/pointer.c`), which is
  also why hovering it cannot reach the hidden client.
- On enter, one `client_send_frame_done` is sent to kick the client's frame loop;
  after that the copy's scene nodes keep it running while the window's own scene
  tree is hidden.

## Limitations

- The copy is a render of the window, not an input surface: only a left click is
  understood (bring the window back). Dragging or scrolling on the copy does not
  reach the application.
- The copy is not captured by screencopy/recording tools unless they capture the
  output.
- Changing the monitor layout or resolution while a copy is on screen re-lays it
  out on the next surface commit only.

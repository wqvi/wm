#ifndef WM_H
#define WM_H

#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

typedef union {
	int i;
	uint32_t ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	const Arg arg;
} Button;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct Monitor Monitor;

typedef struct {
	unsigned int type; // Never X11
	struct wlr_box geom; // layout-relative, includes border
	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border[4]; // top, bottom, left, right
	struct wlr_scene_tree *scene_surface;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_xdg_surface *surface;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wlr_box prev; // layout-relative, includes border
	unsigned int bw;
	uint32_t tags;
	int is_floating;
	int is_urgent;
	int is_fullscreen;
	uint32_t resize; // configure serial of a pending size
} Client;

#endif // WM_H

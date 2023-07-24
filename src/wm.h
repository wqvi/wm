#ifndef WM_H
#define WM_H

#include <stdint.h>
#include <unistd.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

union Arg {
	int i;
	uint32_t ui;
	float f;
	const void *v;
};

struct Client {
	unsigned int type; // Never X11
	struct wlr_box geom; // layout-relative, includes border
	struct Monitor *mon;
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
};

struct Keyboard {
	struct wl_list link;
	struct wlr_keyboard *wlr_keyboard;

	int nsyms;
	const xkb_keysym_t *keysyms; // invalid if nsyms == 0
	uint32_t mods; // invalid if nsyms == 0
	struct wl_event_source *key_repeat_source;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct LayerSurface {
	// Must keep these three elements in this order
	unsigned int type; // layer shell
	struct wlr_box geom;
	struct Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
};

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg; // See createmon() for info
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m; // monitor area, layout-relative
	struct wlr_box w; // window area, layout-relative
	struct wl_list layers[4]; // LayerSurface::link
	unsigned int seltags;
	unsigned int sellt;
	uint32_t tagset[2];
	double mfact;
	int nmaster;
};

struct MonitorRule {
	const char *name;
	float mfact;
	int nmaster;
	float scale;
	enum wl_output_transform rr;
	int x, y;
};

struct Rule {
	const char *id;
	const char *title;
	uint32_t tags;
	int is_floating;
	int monitor;
};

struct SessionLock {
	struct wlr_scene_tree *scene;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
};

enum {
	CurNormal,
	CurPressed,
	CurMove,
	CurResize
}; // cursor

enum { 
	XDGShell, 
	LayerShell 
}; // client types

enum {
	LyrBg,
	LyrBottom,
	LyrTile,
	LyrFloat,
	LyrFS,
	LyrTop,
	LyrOverlay,
	LyrBlock,
	NUM_LAYERS 
}; // scene layers

struct process {
	struct wlr_xdg_activation_token_v1 *token;
	struct wl_listener token_destroy;
	struct wl_list link;
};

int run_daemon(const char *cmd, struct wl_list *processes, struct wlr_xdg_activation_v1 *activation, struct wlr_seat *seat);

int run_child(const char *cmd, struct wl_list *processes, struct wlr_xdg_activation_v1 *activation, struct wlr_seat *seat);

void run(void);

void setup(void);

void cleanup(void);

#endif // WM_H

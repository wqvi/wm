#ifndef WM_H
#define WM_H

#include <linux/input-event-codes.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#define IDLE_NOTIFY_ACTIVITY wlr_idle_notify_activity(server->idle, server->seat), wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat)
#define MODKEY WLR_MODIFIER_LOGO

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

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_scene *scene;
	struct wlr_scene_tree *layers[NUM_LAYERS];
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_box sgeom;
	struct wlr_seat *seat;
	struct Monitor *selmon;
	
	struct wlr_idle *idle;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wlr_input_inhibit_manager *input_inhibit_mgr;
	struct wlr_session_lock_manager_v1 *session_lock_mgr;
	struct wlr_scene_rect *locked_bg;
	struct wlr_session_lock_v1 *cur_lock;
	int locked;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_activation_v1 *activation;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;

	struct wlr_output_layout *output_layout;
	struct wlr_layer_shell_v1 *layer_shell;

	struct wlr_output_manager_v1 *output_mgr;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	unsigned int cursor_mode;
	int grabcx, grabcy;
	struct Client *grabc;
	const char *cursor_image;

	struct wl_list keyboards;
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
	
	struct wl_listener request_activate;
	struct wl_listener layout_change;
	struct wl_listener new_output;
	struct wl_listener idle_inhibitor_create;
	struct wl_listener idle_inhibitor_destroy;
	struct wl_listener new_layer_shell_surface;
	struct wl_listener new_xdg_surface;
	struct wl_listener new_xdg_decoration;
	struct wl_listener session_lock_create_lock;
	struct wl_listener session_lock_mgr_destroy;
	struct wl_listener request_cursor;
	struct wl_listener request_set_psel;
	struct wl_listener request_set_sel;
	struct wl_listener output_mgr_apply;
	struct wl_listener output_mgr_test;

	struct wl_listener cursor_axis; 
	struct wl_listener cursor_button;
	struct wl_listener cursor_frame;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	
	struct wl_listener new_input;
	struct wl_listener new_virtual_keyboard;

	struct wl_list processes;
	struct wl_list monitors;
	struct wl_list clients;
	struct wl_list focus_stack;
};

extern struct server *server;

int run_daemon(const char *cmd, struct wl_list *processes, struct wlr_xdg_activation_v1 *activation, struct wlr_seat *seat);

int run_child(const char *cmd, struct wl_list *processes, struct wlr_xdg_activation_v1 *activation, struct wlr_seat *seat);

struct Monitor *xytomon(struct wlr_output_layout *output_layout, double x, double y);

void die(const char *fmt, ...);

void *ecalloc(size_t nmemb, size_t size);

void setup(void);

void run(void);

void cleanup(void);

// Listeners

void axisnotify(struct wl_listener *listener, void *data);

void buttonpress(struct wl_listener *listener, void *data);

void cursorframe(struct wl_listener *listener, void *data);

void motionnotify(uint32_t time);

void motionrelative(struct wl_listener *listener, void *data);

void checkidleinhibitor(struct wlr_surface *exclude);

void createidleinhibitor(struct wl_listener *listener, void *data);

void destroyidleinhibitor(struct wl_listener *listener, void *data);

void destroylayersurfacenotify(struct wl_listener *listener, void *data);

void destroylock(struct SessionLock *lock, int unlocked);

void destroylocksurface(struct wl_listener *listener, void *data);

void destroynotify(struct wl_listener *listener, void *data);

void destroysessionlock(struct wl_listener *listener, void *data);

void destroysessionmgr(struct wl_listener *listener, void *data);

void focusclient(struct Client *c, int lift);

struct Client *focustop(struct Monitor *m);

void client_notify_enter(struct wlr_surface *s, struct wlr_keyboard *kb);

struct wlr_scene_node *xytonode(double x, double y, struct wlr_surface **psurface, struct Client **pc, struct LayerSurface **pl, double *nx, double *ny);

void pointerfocus(struct Client *c, struct wlr_surface *surface, double sx, double sy, uint32_t time);

void setmon(struct Client *c, struct Monitor *m, uint32_t newtags);

void resize(struct Client *c, struct wlr_box geo, int interact);

void motionabsolute(struct wl_listener *listener, void *data);

void virtualkeyboard(struct wl_listener *listener, void *data);

void urgent(struct wl_listener *listener, void *data);

void setpsel(struct wl_listener *listener, void *data);

void setsel(struct wl_listener *listener, void *data);

void setcursor(struct wl_listener *listener, void *data);

void outputmgrtest(struct wl_listener *listener, void *data);

void outputmgrapply(struct wl_listener *listener, void *data);

void locksession(struct wl_listener *listener, void *data);

void inputdevice(struct wl_listener *listener, void *data);

void createnotify(struct wl_listener *listener, void *data);

void createmon(struct wl_listener *listener, void *data);

void createlayersurface(struct wl_listener *listener, void *data);

void createdecoration(struct wl_listener *listener, void *data);

void unmaplayersurfacenotify(struct wl_listener *listener, void *data);

void unmapnotify(struct wl_listener *listener, void *data);

void updatemons(struct wl_listener *listener, void *data);

void updatetitle(struct wl_listener *listener, void *data);

#include "client.h"

#endif // WM_H

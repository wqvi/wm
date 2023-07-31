#ifndef WM_H
#define WM_H

#include <GLES2/gl2.h>
#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
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
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
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
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define END(A)                  ((A) + LENGTH(A))
#define TAGMASK                 ((1u << 9) - 1)
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define IDLE_NOTIFY_ACTIVITY wlr_idle_notify_activity(server->idle, server->seat), wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat)
#define MODKEY WLR_MODIFIER_LOGO

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

	void *exclusive_focus;

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

struct Client *monitor_get_top_client(struct Monitor *m);

void client_notify_enter(struct wlr_surface *s, struct wlr_keyboard *kb);

struct wlr_scene_node *xytonode(double x, double y, struct wlr_surface **psurface, struct Client **pc, struct LayerSurface **pl, double *nx, double *ny);

void pointerfocus(struct Client *c, struct wlr_surface *surface, double sx, double sy, uint32_t time);

void monitor_set(struct Client *c, struct Monitor *m, uint32_t newtags);

void applybounds(struct Client *c, struct wlr_box *bbox);

void applyrules(struct Client *c);

void monitor_arrange(struct Monitor *m);

void monitor_arrange_layer(struct Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive);

void monitor_arrange_layers(struct Monitor *m);

void monitor_close(struct Monitor *m);

void createkeyboard(struct wlr_keyboard *keyboard);

void createpointer(struct wlr_pointer *pointer);

struct Monitor *monitor_get_by_direction(enum wlr_direction dir);

int keyrepeat(void *data);

void maximizenotify(struct wl_listener *listener, void *data);

void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);

void printstatus(void);

void setfullscreen(struct Client *c, int fullscreen);

#include "listeners.h"

#include "client.h"

#endif // WM_H

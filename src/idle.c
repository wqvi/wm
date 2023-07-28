#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include "wm.h"

void checkidleinhibitor(struct wlr_surface *exclude) {
	int inhibited = 0, unused_lx, unused_ly;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &server->idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;
		if (exclude != surface && (!tree
				|| wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly))) {
			inhibited = 1;
			break;
		}
	}

	wlr_idle_set_enabled(server->idle, NULL, !inhibited);
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, inhibited);
}

void destroyidleinhibitor(struct wl_listener *listener, void *data) {
	/* `data` is the wlr_surface of the idle inhibitor being destroyed,
	 * at this point the idle inhibitor is still in the list of the manager */
	checkidleinhibitor(wlr_surface_get_root_surface(data));
}

void destroylayersurfacenotify(struct wl_listener *listener, void *data) {
	struct LayerSurface *layersurface = wl_container_of(listener, layersurface, destroy);

	wl_list_remove(&layersurface->link);
	wl_list_remove(&layersurface->destroy.link);
	wl_list_remove(&layersurface->map.link);
	wl_list_remove(&layersurface->unmap.link);
	wl_list_remove(&layersurface->surface_commit.link);
	wlr_scene_node_destroy(&layersurface->scene->node);
	free(layersurface);
}

void destroylock(struct SessionLock *lock, int unlock) {
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	if ((server->locked = !unlock))
		goto destroy;

	wlr_scene_node_set_enabled(&server->locked_bg->node, 0);

	focusclient(focustop(server->selmon), 0);
	motionnotify(0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	wlr_scene_node_destroy(&lock->scene->node);
	server->cur_lock = NULL;
	free(lock);
}

void destroylocksurface(struct wl_listener *listener, void *data) {
	struct Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);

	if (lock_surface->surface == server->seat->keyboard_state.focused_surface) {
		if (server->locked && server->cur_lock && !wl_list_empty(&server->cur_lock->surfaces)) {
			surface = wl_container_of(server->cur_lock->surfaces.next, surface, link);
			client_notify_enter(surface->surface, wlr_seat_get_keyboard(server->seat));
		} else if (!server->locked) {
			focusclient(focustop(server->selmon), 1);
		} else {
			wlr_seat_keyboard_clear_focus(server->seat);
		}
	}
}

void destroynotify(struct wl_listener *listener, void *data) {
	/* Called when the surface is destroyed and should never be shown again. */
	struct Client *c = wl_container_of(listener, c, destroy);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
	free(c);
}

void destroysessionlock(struct wl_listener *listener, void *data) {
	struct SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

void destroysessionmgr(struct wl_listener *listener, void *data) {
	wl_list_remove(&server->session_lock_create_lock.link);
	wl_list_remove(&server->session_lock_mgr_destroy.link);
}

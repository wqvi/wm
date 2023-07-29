/*
 * See LICENSE file for copyright and license details.
 */

#include "wm.h"
#include <GLES2/gl2.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>

static const char broken[] = "broken";
// Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };

// configuration, allows nested code to access above variables
#include "config.h"

void applybounds(struct Client *c, struct wlr_box *bbox) {
	if (!c->is_fullscreen) {
		struct wlr_box min = {0}, max = {0};
		client_get_size_hints(c, &max, &min);
		// try to set size hints
		c->geom.width = MAX(min.width + (2 * (int)c->bw), c->geom.width);
		c->geom.height = MAX(min.height + (2 * (int)c->bw), c->geom.height);
		// Some clients set their max size to INT_MAX, which does not violate the
		// protocol but it's unnecesary, as they can set their max size to zero.
		if (max.width > 0 && !(2 * c->bw > INT_MAX - max.width)) /* Checks for overflow */
			c->geom.width = MIN(max.width + (2 * c->bw), c->geom.width);
		if (max.height > 0 && !(2 * c->bw > INT_MAX - max.height)) /* Checks for overflow */
			c->geom.height = MIN(max.height + (2 * c->bw), c->geom.height);
	}

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width + 2 * c->bw <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height + 2 * c->bw <= bbox->y)
		c->geom.y = bbox->y;
}

void applyrules(struct Client *c) {
	// rule matching
	const char *appid, *title;
	uint32_t newtags = 0;
	struct Monitor *mon = server->selmon;

	c->is_floating = client_is_float_type(c);
	if (!(appid = client_get_appid(c)))
		appid = broken;
	if (!(title = client_get_title(c)))
		title = broken;
	
	wlr_scene_node_reparent(&c->scene->node, server->layers[c->is_floating ? LyrFloat : LyrTile]);
	setmon(c, mon, newtags);
}

void arrange(struct Monitor *m) {
	struct Client *c;
	wl_list_for_each(c, &server->clients, link)
		if (c->mon == m)
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
			(c = get_top(m)) && c->is_fullscreen);

	tile(m);
	motionnotify(0);
	checkidleinhibitor(NULL);
}

void arrangelayer(struct Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive) {
	struct LayerSurface *layersurface;
	struct wlr_box full_area = m->m;

	wl_list_for_each(layersurface, list, link) {
		struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
		struct wlr_layer_surface_v1_state *state = &wlr_layer_surface->current;

		if (exclusive != (state->exclusive_zone > 0))
			continue;

		wlr_scene_layer_surface_v1_configure(layersurface->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&layersurface->popups->node,
				layersurface->scene->node.x, layersurface->scene->node.y);
		layersurface->geom.x = layersurface->scene->node.x;
		layersurface->geom.y = layersurface->scene->node.y;
	}
}

void arrangelayers(struct Monitor *m) {
	int i;
	struct wlr_box usable_area = m->m;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	struct LayerSurface *layersurface;

	if (!m->wlr_output->enabled)
		return;

	// Arrange exclusive surfaces from top->bottom
	for (i = 3; i >= 0; i--) {
		arrangelayer(m, &m->layers[i], &usable_area, 1);
	}

	if (memcmp(&usable_area, &m->w, sizeof(struct wlr_box))) {
		m->w = usable_area;
		arrange(m);
	}

	// Arrange non-exlusive surfaces from top->bottom
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	// Find topmost keyboard interactive layer, if such a layer exists
	for (i = 0; i < LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(layersurface,
				&m->layers[layers_above_shell[i]], link) {
			if (!server->locked && layersurface->layer_surface->current.keyboard_interactive
					&& layersurface->mapped) {
				// Deactivate the focused client.
				focusclient(NULL, 0);
				server->exclusive_focus = layersurface;
				client_notify_enter(layersurface->layer_surface->surface, wlr_seat_get_keyboard(server->seat));
				return;
			}
		}
	}
}

void cleanupmon(struct wl_listener *listener, void *data) {
	struct Monitor *m = wl_container_of(listener, m, destroy);
	struct LayerSurface *l, *tmp;
	int i;

	for (i = 0; i <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link) {
			wlr_layer_surface_v1_destroy(l->layer_surface);
		}
	}

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->link);
	m->wlr_output->data = NULL;
	wlr_output_layout_remove(server->output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);

	closemon(m);
	free(m);
}

void closemon(struct Monitor *m) {
	// update selmon if needed and
	// move closed monitor's clients to the focused one
	struct Client *c;
	if (wl_list_empty(&server->monitors)) {
		server->selmon = NULL;
	} else if (m == server->selmon) {
		int nmons = wl_list_length(&server->monitors), i = 0;
		do // don't switch to disabled mons
			server->selmon = wl_container_of(server->monitors.next, server->selmon, link);
		while (!server->selmon->wlr_output->enabled && i++ < nmons);
	}

	wl_list_for_each(c, &server->clients, link) {
		if (c->is_floating && c->geom.x > m->m.width)
			resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
				.width = c->geom.width, .height = c->geom.height}, 0);
		if (c->mon == m)
			setmon(c, server->selmon, c->tags);
	}
	focusclient(get_top(server->selmon), 1);
	printstatus();
}

void commitlayersurfacenotify(struct wl_listener *listener, void *data) {
	struct LayerSurface *layersurface = wl_container_of(listener, layersurface, surface_commit);
	struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
	struct wlr_output *wlr_output = wlr_layer_surface->output;
	struct wlr_scene_tree *layer = server->layers[layermap[wlr_layer_surface->current.layer]];

	// For some reason this layersurface have no monitor, this can be because
	// its monitor has just been destroyed
	if (!wlr_output || !(layersurface->mon = wlr_output->data))
		return;

	if (layer != layersurface->scene->node.parent) {
		wlr_scene_node_reparent(&layersurface->scene->node, layer);
		wlr_scene_node_reparent(&layersurface->popups->node, layer);
		wl_list_remove(&layersurface->link);
		wl_list_insert(&layersurface->mon->layers[wlr_layer_surface->current.layer],
				&layersurface->link);
	}
	if (wlr_layer_surface->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		wlr_scene_node_reparent(&layersurface->popups->node, server->layers[LyrTop]);

	if (wlr_layer_surface->current.committed == 0
			&& layersurface->mapped == wlr_layer_surface->mapped)
		return;
	layersurface->mapped = wlr_layer_surface->mapped;

	arrangelayers(layersurface->mon);
}

void commitnotify(struct wl_listener *listener, void *data) {
	struct Client *c = wl_container_of(listener, c, commit);
	struct wlr_box box = {0};
	client_get_geometry(c, &box);

	if (c->mon && !wlr_box_empty(&box) && (box.width != c->geom.width - 2 * c->bw
			|| box.height != c->geom.height - 2 * c->bw))
		c->is_floating ? resize(c, c->geom, 1) : arrange(c->mon);

	// mark a pending resize as completed
	if (c->resize && c->resize <= c->surface->current.configure_serial)
		c->resize = 0;
}

void createdecoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *dec = data;
	wlr_xdg_toplevel_decoration_v1_set_mode(dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void createlayersurface(struct wl_listener *listener, void *data) {
	struct wlr_layer_surface_v1 *wlr_layer_surface = data;
	struct LayerSurface *layersurface;
	struct wlr_layer_surface_v1_state old_state;
	struct wlr_scene_tree *l = server->layers[layermap[wlr_layer_surface->pending.layer]];

	if (!wlr_layer_surface->output)
		wlr_layer_surface->output = server->selmon ? server->selmon->wlr_output : NULL;

	if (!wlr_layer_surface->output) {
		wlr_layer_surface_v1_destroy(wlr_layer_surface);
		return;
	}

	layersurface = ecalloc(1, sizeof(struct LayerSurface));
	layersurface->type = LayerShell;
	LISTEN(&wlr_layer_surface->surface->events.commit,
			&layersurface->surface_commit, commitlayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.destroy, &layersurface->destroy,
			destroylayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.map, &layersurface->map,
			maplayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.unmap, &layersurface->unmap,
			unmaplayersurfacenotify);

	layersurface->layer_surface = wlr_layer_surface;
	layersurface->mon = wlr_layer_surface->output->data;
	wlr_layer_surface->data = layersurface;

	layersurface->scene_layer = wlr_scene_layer_surface_v1_create(l, wlr_layer_surface);
	layersurface->scene = layersurface->scene_layer->tree;
	layersurface->popups = wlr_layer_surface->surface->data = wlr_scene_tree_create(l);

	layersurface->scene->node.data = layersurface;

	wl_list_insert(&layersurface->mon->layers[wlr_layer_surface->pending.layer],
			&layersurface->link);

	// Temporarily set the layer's current state to pending
	// so that we can easily arrange it
	old_state = wlr_layer_surface->current;
	wlr_layer_surface->current = wlr_layer_surface->pending;
	layersurface->mapped = 1;
	arrangelayers(layersurface->mon);
	wlr_layer_surface->current = old_state;
}

void createmon(struct wl_listener *listener, void *data) {
	// This event is raised by the backend when a new output (aka a display or
	// monitor) becomes available.
	struct wlr_output *wlr_output = data;
	const struct MonitorRule *r;
	size_t i;
	struct Monitor *m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	if (!eglMakeCurrent(wlr_egl_get_display(egl), EGL_NO_SURFACE, EGL_NO_SURFACE, wlr_egl_get_context(egl))) {
		wlr_log(WLR_ERROR, "yea uhhhh something went wrong with the whole EGL thing here??? you're kinda on your own :)");
		free(m);
		return;
	}

	wlr_log(WLR_INFO, "Binding new stuffes to the monitor!");

	if (!eglMakeCurrent(wlr_egl_get_display(egl), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
		wlr_log(WLR_ERROR, "Failed to do the freeing of the egl context! This is definitely not supposed to happen!!! :)");
		free(m);
		return;
	}

	// Initialize monitor state using configured rules 
	for (i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);
	m->tagset[0] = m->tagset[1] = 1;
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(wlr_output->name, r->name)) {
			m->mfact = r->mfact;
			m->nmaster = r->nmaster;
			wlr_output_set_scale(wlr_output, r->scale);
			wlr_xcursor_manager_load(server->cursor_mgr, r->scale);
			wlr_output_set_transform(wlr_output, r->rr);
			m->m.x = r->x;
			m->m.y = r->y;
			break;
		}
	}

	// The mode is a tuple of (width, height, refresh rate), and each
	// monitor supports only a specific set of modes. We just pick the
	// monitor's preferred mode; a more sophisticated compositor would let
	// the user configure it.
	wlr_output_set_mode(wlr_output, wlr_output_preferred_mode(wlr_output));

	// Set up event listeners 
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);

	wlr_output_enable(wlr_output, 1);
	if (!wlr_output_commit(wlr_output))
		return;

	// Try to enable adaptive sync, note that not all monitors support it.
	// wlr_output_commit() will deactivate it in case it cannot be enabled
	wlr_output_enable_adaptive_sync(wlr_output, 1);
	wlr_output_commit(wlr_output);

	wl_list_insert(&server->monitors, &m->link);
	printstatus();

	// The xdg-protocol specifies:
	//
	// If the fullscreened surface is not opaque, the compositor must make
	// sure that other screen content not part of the same surface tree (made
	// up of subsurfaces, popups or similarly coupled surfaces) are not
	// visible below the fullscreened surface.
	//
	// updatemons() will resize and set correct position 
	m->fullscreen_bg = wlr_scene_rect_create(server->layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	// Adds this to the output layout in the order it was configured in.
	//
	// The output layout utility automatically adds a wl_output global to the
	// display, which Wayland clients can see to find out information about the
	// output (such as DPI, scale factor, manufacturer, etc).
	m->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (m->m.x < 0 || m->m.y < 0)
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	else
		wlr_output_layout_add(server->output_layout, wlr_output, m->m.x, m->m.y);
}

void createnotify(struct wl_listener *listener, void *data) {
	// This event is raised when wlr_xdg_shell receives a new xdg surface from a
	// client, either a toplevel (application window) or popup,
	// or when wlr_layer_shell receives a new popup from a layer.
	// If you want to do something tricky with popups you should check if
	// its parent is wlr_xdg_shell or wlr_layer_shell
	struct wlr_xdg_surface *xdg_surface = data;
	struct Client *c = NULL;
	struct LayerSurface *l = NULL;

	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_box box;
		int type = toplevel_from_wlr_surface(xdg_surface->surface, &c, &l);
		if (!xdg_surface->popup->parent || type < 0)
			return;
		xdg_surface->surface->data = wlr_scene_xdg_surface_create(
				xdg_surface->popup->parent->data, xdg_surface);
		if ((l && !l->mon) || (c && !c->mon))
			return;
		box = type == LayerShell ? l->mon->m : c->mon->w;
		box.x -= (type == LayerShell ? l->geom.x : c->geom.x);
		box.y -= (type == LayerShell ? l->geom.y : c->geom.y);
		wlr_xdg_popup_unconstrain_from_box(xdg_surface->popup, &box);
		return;
	} else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_NONE)
		return;

	// Allocate a Client for this surface
	c = xdg_surface->data = ecalloc(1, sizeof(*c));
	c->surface = xdg_surface;
	c->bw = borderpx;

	LISTEN(&xdg_surface->events.map, &c->map, mapnotify);
	LISTEN(&xdg_surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&xdg_surface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xdg_surface->toplevel->events.set_title, &c->set_title, updatetitle);
	LISTEN(&xdg_surface->toplevel->events.request_fullscreen, &c->fullscreen,
			fullscreennotify);
	LISTEN(&xdg_surface->toplevel->events.request_maximize, &c->maximize,
			maximizenotify);
}

struct Monitor *dirtomon(enum wlr_direction dir) {
	struct wlr_output *next;
	if (!wlr_output_layout_get(server->output_layout, server->selmon->wlr_output))
		return server->selmon;
	if ((next = wlr_output_layout_adjacent_output(server->output_layout,
			dir, server->selmon->wlr_output, server->selmon->m.x, server->selmon->m.y)))
		return next->data;
	if ((next = wlr_output_layout_farthest_output(server->output_layout,
			dir ^ (WLR_DIRECTION_LEFT|WLR_DIRECTION_RIGHT),
			server->selmon->wlr_output, server->selmon->m.x, server->selmon->m.y)))
		return next->data;
	return server->selmon;
}

void focusclient(struct Client *c, int lift) {
	struct wlr_surface *old = server->seat->keyboard_state.focused_surface;
	int i, unused_lx, unused_ly, old_client_type;
	struct Client *old_c = NULL;
	struct LayerSurface *old_l = NULL;

	if (server->locked)
		return;

	// Raise client in stacking order if requested
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && client_surface(c) == old)
		return;

	if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	// Put the new client atop the focus stack and select its monitor
	if (c) {
		wl_list_remove(&c->flink);
		wl_list_insert(&server->focus_stack, &c->flink);
		server->selmon = c->mon;
		c->is_urgent = 0;
		client_restack_surface(c);

		// Don't change border color if there is an exclusive focus or we are
		// handling a drag operation 
		if (!server->exclusive_focus && !server->seat->drag) {
			for (i = 0; i < 4; i++) {
				wlr_scene_rect_set_color(c->border[i], focuscolor);
			}
		}
	}

	// Deactivate old client if focus is changing
	if (old && (!c || client_surface(c) != old)) {
		// If an overlay is focused, don't focus or activate the client,
		// but only update its position in fstack to render its border with focuscolor
		// and focus it after the overlay is closed.
		if (old_client_type == LayerShell && wlr_scene_node_coords(
					&old_l->scene->node, &unused_lx, &unused_ly)
				&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			return;
		} else if (old_c && old_c == server->exclusive_focus && client_wants_focus(old_c)) {
			return;
		// Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
		// and probably other clients 
		} else if (old_c && (!c || !client_wants_focus(c))) {
			for (i = 0; i < 4; i++)
				wlr_scene_rect_set_color(old_c->border[i], bordercolor);

			client_activate_surface(old, 0);
		}
	}
	printstatus();

	if (!c) {
		// With no client, all we have left is to clear focus 
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}

	// Change cursor surface
	motionnotify(0);

	// Have a client, so focus its top-level wlr_surface
	client_notify_enter(client_surface(c), wlr_seat_get_keyboard(server->seat));

	// Activate the new client
	client_activate_surface(client_surface(c), 1);
}

void focusmon(int dir) {
	int i = 0, nmons = wl_list_length(&server->monitors);
	if (nmons)
		do // don't switch to disabled mons
			server->selmon = dirtomon(dir);
		while (!server->selmon->wlr_output->enabled && i++ < nmons);
	focusclient(get_top(server->selmon), 1);
}

struct Client *get_top(struct Monitor *m) {
	struct Client *c;

	wl_list_for_each(c, &server->focus_stack, flink) {
		if (VISIBLEON(c, m)) return c;
	}

	return NULL;
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	struct Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

void incnmaster(int i) {
	if (!server->selmon)
		return;
	// this needs to be improved
	// there needs to be some comparison to prevent
	// nmaster from becoming to astronomically high up
	server->selmon->nmaster = MAX(server->selmon->nmaster + i, 0);
	arrange(server->selmon);
}

void killclient(void) {
	struct Client *sel = get_top(server->selmon);
	if (sel)
		client_send_close(sel);
}

void
locksession(struct wl_listener *listener, void *data)
{
	struct wlr_session_lock_v1 *session_lock = data;
	struct SessionLock *lock;
	wlr_scene_node_set_enabled(&server->locked_bg->node, 1);
	if (server->cur_lock) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = ecalloc(1, sizeof(*lock));
	focusclient(NULL, 0);

	lock->scene = wlr_scene_tree_create(server->layers[LyrBlock]);
	server->cur_lock = lock->lock = session_lock;
	server->locked = 1;
	session_lock->data = lock;

	LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	wlr_session_lock_v1_send_locked(session_lock);
}

void
maplayersurfacenotify(struct wl_listener *listener, void *data)
{
	struct LayerSurface *l = wl_container_of(listener, l, map);
	wlr_surface_send_enter(l->layer_surface->surface, l->mon->wlr_output);
	motionnotify(0);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct Client *p, *c = wl_container_of(listener, c, map);
	int i;

	/* Create scene tree for this client and its border */
	c->scene = wlr_scene_tree_create(server->layers[LyrTile]);
	wlr_scene_node_set_enabled(&c->scene->node, c->type != XDGShell);
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	if (client_surface(c)) {
		client_surface(c)->data = c->scene;
		/* Ideally we should do this in createnotify{,x11} but at that moment
		* wlr_xwayland_surface doesn't have wlr_surface yet. */
		LISTEN(&client_surface(c)->events.commit, &c->commit, commitnotify);
	}
	c->scene->node.data = c->scene_surface->node.data = c;

	for (i = 0; i < 4; i++) {
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
		c->border[i]->node.data = c;
	}

	/* Initialize client geometry with room for border */
	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	client_get_geometry(c, &c->geom);
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;

	/* Insert this client into client lists. */
	wl_list_insert(&server->clients, &c->link);
	wl_list_insert(&server->focus_stack, &c->flink);

	/* Set initial monitor, tags, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same tags and monitor than its parent, if not
	 * try to apply rules for them */
	 /* TODO: https://github.com/djpohly/dwl/pull/334#issuecomment-1330166324 */
	if (c->type == XDGShell && (p = client_get_parent(c))) {
		c->is_floating = 1;
		wlr_scene_node_reparent(&c->scene->node, server->layers[LyrFloat]);
		setmon(c, p->mon, p->tags);
	} else {
		applyrules(c);
	}
	printstatus();
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations. dwl doesn't support maximization, but
	 * to conform to xdg-shell protocol we still must send a configure.
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
	struct Client *c = wl_container_of(listener, c, maximize);
	wlr_xdg_surface_schedule_configure(c->surface);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	motionnotify(event->time_msec);
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration. This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	int ok = 1;

	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		struct Monitor *m = wlr_output->data;

		wlr_output_enable(wlr_output, config_head->state.enabled);
		if (!config_head->state.enabled)
			goto apply_or_test;
		if (config_head->state.mode)
			wlr_output_set_mode(wlr_output, config_head->state.mode);
		else
			wlr_output_set_custom_mode(wlr_output,
					config_head->state.custom_mode.width,
					config_head->state.custom_mode.height,
					config_head->state.custom_mode.refresh);

		/* Don't move monitors if position wouldn't change, this to avoid
		 * wlroots marking the output as manually configured */
		if (m->m.x != config_head->state.x || m->m.y != config_head->state.y)
			wlr_output_layout_move(server->output_layout, wlr_output,
					config_head->state.x, config_head->state.y);
		wlr_output_set_transform(wlr_output, config_head->state.transform);
		wlr_output_set_scale(wlr_output, config_head->state.scale);
		wlr_output_enable_adaptive_sync(wlr_output,
				config_head->state.adaptive_sync_enabled);

apply_or_test:
		if (test) {
			ok &= wlr_output_test(wlr_output);
			wlr_output_rollback(wlr_output);
		} else {
			ok &= wlr_output_commit(wlr_output);
		}
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	/* TODO: use a wrapper function? */
	updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void
printstatus(void)
{
	struct Monitor *m = NULL;
	struct Client *c;
	uint32_t occ, urg, sel;
	const char *appid, *title;

	wl_list_for_each(m, &server->monitors, link) {
		occ = urg = 0;
		wl_list_for_each(c, &server->clients, link) {
			if (c->mon != m)
				continue;
			occ |= c->tags;
			if (c->is_urgent)
				urg |= c->tags;
		}
		if ((c = get_top(m))) {
			title = client_get_title(c);
			appid = client_get_appid(c);
			printf("%s title %s\n", m->wlr_output->name, title ? title : broken);
			printf("%s appid %s\n", m->wlr_output->name, appid ? appid : broken);
			printf("%s fullscreen %u\n", m->wlr_output->name, c->is_fullscreen);
			printf("%s floating %u\n", m->wlr_output->name, c->is_floating);
			sel = c->tags;
		} else {
			printf("%s title \n", m->wlr_output->name);
			printf("%s appid \n", m->wlr_output->name);
			printf("%s fullscreen \n", m->wlr_output->name);
			printf("%s floating \n", m->wlr_output->name);
			sel = 0;
		}

		printf("%s selmon %u\n", m->wlr_output->name, m == server->selmon);
		printf("%s tags %u %u %u %u\n", m->wlr_output->name, occ, m->tagset[m->seltags],
				sel, urg);
		printf("%s layout []=\n", m->wlr_output->name);
	}
	fflush(stdout);
}

void
rendermon(struct wl_listener *listener, void *data)
{
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct Monitor *m = wl_container_of(listener, m, frame);
	struct Client *c;
	struct timespec now;

	/* Render if no XDG clients have an outstanding resize and are visible on
	 * this monitor. */
	wl_list_for_each(c, &server->clients, link)
		if (c->resize && !c->is_floating && client_is_rendered_on_mon(c, m) && !client_is_stopped(c))
			goto skip;
	wlr_scene_output_commit(m->scene_output);

skip:
	/* Let clients know a frame has been rendered */
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

void
resize(struct Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox = interact ? &server->sgeom : &c->mon->w;
	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	/* Update scene-graph, including borders */
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
	wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
	wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);

	/* this is a no-op if size hasn't changed */
	c->resize = client_set_size(c, c->geom.width - 2 * c->bw,
			c->geom.height - 2 * c->bw);
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (server->cursor_mode != CurNormal && server->cursor_mode != CurPressed)
		return;
	server->cursor_image = NULL;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == server->seat->pointer_state.focused_client)
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setfullscreen(struct Client *c, int fullscreen)
{
	c->is_fullscreen = fullscreen;
	if (!c->mon)
		return;
	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, server->layers[fullscreen
			? LyrFS : c->is_floating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
	} else {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		resize(c, c->prev, 0);
	}
	arrange(c->mon);
	printstatus();
}

void
setmon(struct Client *c, struct Monitor *m, uint32_t newtags)
{
	struct Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	c->mon = m;
	c->prev = c->geom;

	/* TODO leave/enter is not optimal but works */
	if (oldmon) {
		wlr_surface_send_leave(client_surface(c), oldmon->wlr_output);
		arrange(oldmon);
	}
	if (m) {
		/* Make sure window actually overlaps with the monitor */
		resize(c, c->geom, 0);
		wlr_surface_send_enter(client_surface(c), m->wlr_output);
		c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
		setfullscreen(c, c->is_fullscreen); /* This will call arrange(c->mon) */
	}
	focusclient(get_top(server->selmon), 1);
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void tag(uint32_t ui) {
	struct Client *sel = get_top(server->selmon);
	if (sel && ui & TAGMASK) {
		sel->tags = ui & TAGMASK;
		focusclient(get_top(server->selmon), 1);
		arrange(server->selmon);
	}
	printstatus();
}

void tagmon(int dir) {
	struct Client *sel = get_top(server->selmon);
	if (sel)
		setmon(sel, dirtomon(dir), 0);
}

void tile(struct Monitor *m) {
	unsigned int i, n = 0, mw, my, ty;
	struct Client *c;
	const int pixel_gap = 8;

	wl_list_for_each(c, &server->clients, link)
		if (VISIBLEON(c, m) && !c->is_floating && !c->is_fullscreen)
			n++;
	if (n == 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? m->w.width * m->mfact : 0;
	else
		mw = m->w.width;
	i = my = ty = 0;
	wl_list_for_each(c, &server->clients, link) {
		if (!VISIBLEON(c, m) || c->is_floating || c->is_fullscreen)
			continue;
		if (i < m->nmaster) {
			struct wlr_box box = {
				.x = m->w.x + (pixel_gap / 2),
				.y = m->w.y + my + (pixel_gap / 2),
				.width = mw - pixel_gap / 2,
				.height = ((m->w.height - my) / (MIN(n, m->nmaster) - i)) - pixel_gap
			};
			resize(c, box, 0);
			my += c->geom.height + pixel_gap;
		} else {
			struct wlr_box box = {
				.x = (m->w.x + mw) + (pixel_gap / 2),
				.y = (m->w.y + ty) + (pixel_gap / 2),
				.width = (m->w.width - mw) - pixel_gap,
				.height = ((m->w.height - ty) / (n - i)) - pixel_gap
			};
			resize(c, box, 0);
			ty += c->geom.height + (pixel_gap / 2);
		}
		i++;
	}
}

void togglefullscreen(void) {
	struct Client *sel = get_top(server->selmon);
	if (sel)
		setfullscreen(sel, !sel->is_fullscreen);
}

void
unlocksession(struct wl_listener *listener, void *data)
{
	struct SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
	struct LayerSurface *layersurface = wl_container_of(listener, layersurface, unmap);

	layersurface->mapped = 0;
	wlr_scene_node_set_enabled(&layersurface->scene->node, 0);
	if (layersurface == server->exclusive_focus)
		server->exclusive_focus = NULL;
	if (layersurface->layer_surface->output
			&& (layersurface->mon = layersurface->layer_surface->output->data))
		arrangelayers(layersurface->mon);
	if (layersurface->layer_surface->surface ==
			server->seat->keyboard_state.focused_surface)
		focusclient(get_top(server->selmon), 1);
	motionnotify(0);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct Client *c = wl_container_of(listener, c, unmap);
	if (c == server->grabc) {
		server->cursor_mode = CurNormal;
		server->grabc = NULL;
	}

	wl_list_remove(&c->link);
	setmon(c, NULL, 0);
	wl_list_remove(&c->flink);
	
	wl_list_remove(&c->commit.link);
	wlr_scene_node_destroy(&c->scene->node);
	printstatus();
	motionnotify(0);
}

void
updatemons(struct wl_listener *listener, void *data)
{
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc. This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();
	struct Client *c;
	struct wlr_output_configuration_head_v1 *config_head;
	struct Monitor *m;

	/* First remove from the layout the disabled monitors */
	wl_list_for_each(m, &server->monitors, link) {
		if (m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.enabled = 0;
		/* Remove this output from the layout to avoid cursor enter inside it */
		wlr_output_layout_remove(server->output_layout, m->wlr_output);
		closemon(m);
		memset(&m->m, 0, sizeof(m->m));
		memset(&m->w, 0, sizeof(m->w));
	}
	/* Insert outputs that need to */
	wl_list_for_each(m, &server->monitors, link)
		if (m->wlr_output->enabled
				&& !wlr_output_layout_get(server->output_layout, m->wlr_output))
			wlr_output_layout_add_auto(server->output_layout, m->wlr_output);

	/* Now that we update the output layout we can get its box */
	wlr_output_layout_get_box(server->output_layout, NULL, &server->sgeom);

	/* Make sure the clients are hidden when dwl is locked */
	wlr_scene_node_set_position(&server->locked_bg->node, server->sgeom.x, server->sgeom.y);
	wlr_scene_rect_set_size(server->locked_bg, server->sgeom.width, server->sgeom.height);

	wl_list_for_each(m, &server->monitors, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* Get the effective monitor geometry to use for surfaces */
		wlr_output_layout_get_box(server->output_layout, m->wlr_output, &(m->m));
		wlr_output_layout_get_box(server->output_layout, m->wlr_output, &(m->w));
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
		wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width,
					m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);

		config_head->state.enabled = 1;
		config_head->state.mode = m->wlr_output->current_mode;
		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;
	}

	if (server->selmon && server->selmon->wlr_output->enabled) {
		wl_list_for_each(c, &server->clients, link)
			if (!c->mon && client_is_mapped(c))
				setmon(c, server->selmon, c->tags);
		focusclient(get_top(server->selmon), 1);
		if (server->selmon->lock_surface) {
			client_notify_enter(server->selmon->lock_surface->surface,
					wlr_seat_get_keyboard(server->seat));
			client_activate_surface(server->selmon->lock_surface->surface, 1);
		}
	}

	wlr_output_manager_v1_set_configuration(server->output_mgr, config);
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	struct Client *c = wl_container_of(listener, c, set_title);
	if (c == get_top(c->mon))
		printstatus();
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (c && c != get_top(server->selmon)) {
		c->is_urgent = 1;
		printstatus();
	}
}

void view(uint32_t ui) {
	if (!server->selmon || (ui & TAGMASK) == server->selmon->tagset[server->selmon->seltags])
		return;
	server->selmon->seltags ^= 1; /* toggle sel tagset */
	if (ui & TAGMASK)
		server->selmon->tagset[server->selmon->seltags] = ui & TAGMASK;
	focusclient(get_top(server->selmon), 1);
	arrange(server->selmon);
	printstatus();
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	createkeyboard(&keyboard->keyboard);
}

struct wlr_scene_node *
xytonode(double x, double y, struct wlr_surface **psurface,
		struct Client **pc, struct LayerSurface **pl, double *nx, double *ny)
{
	struct wlr_scene_node *node, *pnode;
	struct wlr_surface *surface = NULL;
	struct Client *c = NULL;
	struct LayerSurface *l = NULL;
	int layer;

	for (layer = NUM_LAYERS - 1; !surface && layer >= 0; layer--) {
		if (!(node = wlr_scene_node_at(&server->layers[layer]->node, x, y, nx, ny)))
			continue;

		if (node->type == WLR_SCENE_NODE_BUFFER)
			surface = wlr_scene_surface_from_buffer(
					wlr_scene_buffer_from_node(node))->surface;
		/* Walk the tree to find a node that knows the client */
		for (pnode = node; pnode && !c; pnode = &pnode->parent->node)
			c = pnode->data;
		if (c && c->type == LayerShell) {
			c = NULL;
			l = pnode->data;
		}
	}

	if (psurface) *psurface = surface;
	if (pc) *pc = c;
	if (pl) *pl = l;
	return node;
}

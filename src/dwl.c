/*
 * See LICENSE file for copyright and license details.
 */

#include "wm.h"

static const char broken[] = "broken";
// Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };

void setfullscreen(struct Client *c, int fullscreen) {
	c->is_fullscreen = fullscreen;
	if (!c->mon)
		return;
	c->bw = fullscreen ? 0 : 2;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, server->layers[fullscreen
			? LyrFS : c->is_floating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		client_resize(c, c->mon->m, 0);
	} else {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		client_resize(c, c->prev, 0);
	}
	monitor_arrange(c->mon);
	printstatus();
}

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
	monitor_set(c, mon, newtags);
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

	monitor_arrange_layers(layersurface->mon);
}

void commitnotify(struct wl_listener *listener, void *data) {
	struct Client *c = wl_container_of(listener, c, commit);
	struct wlr_box box = {0};
	client_get_geometry(c, &box);

	if (c->mon && !wlr_box_empty(&box) && (box.width != c->geom.width - 2 * c->bw
			|| box.height != c->geom.height - 2 * c->bw))
		c->is_floating ? client_resize(c, c->geom, 1) : monitor_arrange(c->mon);

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
	monitor_arrange_layers(layersurface->mon);
	wlr_layer_surface->current = old_state;
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
	c->bw = 2;

	LISTEN(&xdg_surface->events.map, &c->map, mapnotify);
	LISTEN(&xdg_surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&xdg_surface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xdg_surface->toplevel->events.set_title, &c->set_title, updatetitle);
	LISTEN(&xdg_surface->toplevel->events.request_fullscreen, &c->fullscreen,
			fullscreennotify);
	LISTEN(&xdg_surface->toplevel->events.request_maximize, &c->maximize,
			maximizenotify);
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	struct Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
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
	client_focus(NULL, 0);

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
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0, (float[]){0.5f, 0.5f, 0.5f, 1.0f});
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
		monitor_set(c, p->mon, p->tags);
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
		if ((c = monitor_get_top_client(m))) {
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
	if (layersurface == server->exclusive_focus) {
		server->exclusive_focus = NULL;
	}

	if (layersurface->layer_surface->output
			&& (layersurface->mon = layersurface->layer_surface->output->data)) {
		monitor_arrange_layers(layersurface->mon);
	}

	if (layersurface->layer_surface->surface ==
			server->seat->keyboard_state.focused_surface) {
		client_focus(monitor_get_top_client(server->selmon), 1);
	}

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
	monitor_set(c, NULL, 0);
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
		monitor_close(m);
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
		monitor_arrange_layers(m);
		/* Don't move clients to the left output when plugging monitors */
		monitor_arrange(m);

		config_head->state.enabled = 1;
		config_head->state.mode = m->wlr_output->current_mode;
		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;
	}

	if (server->selmon && server->selmon->wlr_output->enabled) {
		wl_list_for_each(c, &server->clients, link)
			if (!c->mon && client_is_mapped(c)) {
				monitor_set(c, server->selmon, c->tags);
			}
		client_focus(monitor_get_top_client(server->selmon), 1);
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
	if (c == monitor_get_top_client(c->mon))
		printstatus();
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (c && c != monitor_get_top_client(server->selmon)) {
		c->is_urgent = 1;
		printstatus();
	}
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

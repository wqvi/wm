#include "wm.h"

static void monitor_tile_clients(struct Monitor *m) {
	unsigned int i, n = 0, mw, my, ty;
	struct Client *c;
	const int pixel_gap = 8;

	wl_list_for_each(c, &server->clients, link) {
		if (VISIBLEON(c, m) && !c->is_fullscreen) {
			n++;
		}
	}

	if (n == 0) {
		return;
	}

	if (n > m->nmaster) {
		mw = m->nmaster ? m->w.width * m->mfact : 0;
	} else {
		mw = m->w.width;
	}

	i = my = ty = 0;
	wl_list_for_each(c, &server->clients, link) {
		if (!VISIBLEON(c, m) || c->is_fullscreen)
			continue;
		if (i < m->nmaster) {
			struct wlr_box box = {
				.x = m->w.x + (pixel_gap / 2),
				.y = m->w.y + my + (pixel_gap / 2),
				.width = mw - pixel_gap / 2,
				.height = ((m->w.height - my) / (MIN(n, m->nmaster) - i)) - pixel_gap
			};
			client_resize(c, box, 0);
			my += c->geom.height + pixel_gap;
		} else {
			struct wlr_box box = {
				.x = (m->w.x + mw) + (pixel_gap / 2),
				.y = (m->w.y + ty) + (pixel_gap / 2),
				.width = (m->w.width - mw) - pixel_gap,
				.height = ((m->w.height - ty) / (n - i)) - pixel_gap
			};
			client_resize(c, box, 0);
			ty += c->geom.height + (pixel_gap / 2);
		}
		i++;
	}
}

void monitor_arrange(struct Monitor *m) {
	struct Client *c;
	wl_list_for_each(c, &server->clients, link)
		if (c->mon == m)
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
			(c = monitor_get_top_client(m)) && c->is_fullscreen);

	monitor_tile_clients(m);
	motionnotify(0);
	checkidleinhibitor(NULL);
}

void cleanupmon(struct wl_listener *listener, void *data) {
	struct Monitor *m = wl_container_of(listener, m, destroy);
	struct LayerSurface *l, *tmp;

	for (int i = 0; i <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; i++) {
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

	monitor_close(m);
	free(m);
}

void monitor_close(struct Monitor *m) {
	// update selmon if needed and
	// move closed monitor's clients to the focused one
	struct Client *c;
	if (wl_list_empty(&server->monitors)) {
		server->selmon = NULL;
	} else if (m == server->selmon) {
		int i = 0;
		int nmons = wl_list_length(&server->monitors);
		do { // don't switch to disabled mons
			server->selmon = wl_container_of(server->monitors.next, server->selmon, link);
		} while (!server->selmon->wlr_output->enabled && i++ < nmons);
	}

	wl_list_for_each(c, &server->clients, link) {
		if (c->is_floating && c->geom.x > m->m.width) {
			client_resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
				.width = c->geom.width, .height = c->geom.height}, 0);
		}
		if (c->mon == m) {
			monitor_set(c, server->selmon, c->tags);
		}
	}

	client_focus(monitor_get_top_client(server->selmon), 1);
	printstatus();
}

void new_monitor_available(struct wl_listener *listener, void *data) {
	struct wlr_output *wlr_output = data;
	size_t i;
	struct wlr_egl *egl;
	struct Monitor *m = wlr_output->data = ecalloc(1, sizeof(*m));

	m->wlr_output = wlr_output;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);
	egl = wlr_gles2_renderer_get_egl(server->renderer);
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

	m->mfact = 0.5f;
	m->nmaster = 1;
	wlr_output_set_scale(wlr_output, 1.0f);
	wlr_xcursor_manager_load(server->cursor_mgr, 1.0f);
	wlr_output_set_transform(wlr_output, WL_OUTPUT_TRANSFORM_NORMAL);
	m->m.x = 1;
	m->m.y = -1;

	// The mode is a tuple of (width, height, refresh rate), and each
	// monitor supports only a specific set of modes. We just pick the
	// monitor's preferred mode; a more sophisticated compositor would let
	// the user configure it.
	wlr_output_set_mode(wlr_output, wlr_output_preferred_mode(wlr_output));

	// Set up eventlisteners 
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
	m->fullscreen_bg = wlr_scene_rect_create(server->layers[LyrFS], 0, 0, (float[]){0.1f, 0.1f, 0.1f, 1.0f});
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

void monitor_arrange_layer(struct Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive) {
	struct LayerSurface *layersurface;
	struct wlr_box full_area = m->m;

	wl_list_for_each(layersurface, list, link) {
		struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
		struct wlr_layer_surface_v1_state *state = &wlr_layer_surface->current;

		if (exclusive != (state->exclusive_zone > 0)) {
			continue;
		}

		wlr_scene_layer_surface_v1_configure(layersurface->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&layersurface->popups->node, layersurface->scene->node.x, layersurface->scene->node.y);
		layersurface->geom.x = layersurface->scene->node.x;
		layersurface->geom.y = layersurface->scene->node.y;
	}
}

void monitor_arrange_layers(struct Monitor *m) {
	int i;
	struct wlr_box usable_area = m->m;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	struct LayerSurface *layersurface;

	if (!m->wlr_output->enabled)
		return;

	// Arrange exclusive surfaces from top to bottom
	for (i = 3; i >= 0; i--) {
		monitor_arrange_layer(m, &m->layers[i], &usable_area, 1);
	}

	if (memcmp(&usable_area, &m->w, sizeof(struct wlr_box))) {
		m->w = usable_area;
		monitor_arrange(m);
	}

	// Arrange non-exlusive surfaces from top to bottom
	for (i = 3; i >= 0; i--) {
		monitor_arrange_layer(m, &m->layers[i], &usable_area, 0);
	}

	// Find topmost keyboard interactive layer, if such a layer exists
	for (i = 0; i < LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(layersurface,
				&m->layers[layers_above_shell[i]], link) {
			if (!server->locked && layersurface->layer_surface->current.keyboard_interactive
					&& layersurface->mapped) {
				// Deactivate the focused client.
				client_focus(NULL, 0);
				server->exclusive_focus = layersurface;
				client_notify_enter(layersurface->layer_surface->surface, wlr_seat_get_keyboard(server->seat));
				return;
			}
		}
	}
}

struct Client *monitor_get_top_client(struct Monitor *m) {
	struct Client *c;

	wl_list_for_each(c, &server->focus_stack, flink) {
		if (VISIBLEON(c, m)) return c;
	}

	return NULL;
}

void rendermon(struct wl_listener *listener, void *data) {
	// This function is called every time an output is ready to display a frame,
	// generally at the output's refresh rate (e.g. 60Hz).
	struct Monitor *m = wl_container_of(listener, m, frame);
	struct Client *c;
	struct timespec now;

	// Render if no XDG clients have an outstanding resize and are visible on
	// this monitor.
	wl_list_for_each(c, &server->clients, link) {
		if (c->resize && !c->is_floating && client_is_rendered_on_mon(c, m) && !client_is_stopped(c)) {
			goto skip;
		}
	}

	wlr_scene_output_commit(m->scene_output);

skip:
	// Let clients know a frame has been rendered
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

struct Monitor *monitor_get_by_direction(enum wlr_direction dir) {
	struct wlr_output *next;
	if (!wlr_output_layout_get(server->output_layout, server->selmon->wlr_output)) {
		return server->selmon;
	}
	
	if ((next = wlr_output_layout_adjacent_output(server->output_layout,
			dir, server->selmon->wlr_output, server->selmon->m.x, server->selmon->m.y))) {
		return next->data;
	}
	
	if ((next = wlr_output_layout_farthest_output(server->output_layout,
			dir ^ (WLR_DIRECTION_LEFT|WLR_DIRECTION_RIGHT),
			server->selmon->wlr_output, server->selmon->m.x, server->selmon->m.y))) {
		return next->data;
	}

	return server->selmon;
}

void monitor_set(struct Client *c, struct Monitor *m, uint32_t newtags) {
	struct Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	c->mon = m;
	c->prev = c->geom;

	// TODO leave/enter is not optimal but works
	if (oldmon) {
		wlr_surface_send_leave(client_surface(c), oldmon->wlr_output);
		monitor_arrange(oldmon);
	}
	if (m) {
		// Make sure window actually overlaps with the monitor
		client_resize(c, c->geom, 0);
		wlr_surface_send_enter(client_surface(c), m->wlr_output);
		c->tags = newtags ? newtags : m->tagset[m->seltags]; // assign tags of target monitor
		setfullscreen(c, c->is_fullscreen); // This will call arrange(c->mon)
	}
	client_focus(monitor_get_top_client(server->selmon), 1);
}


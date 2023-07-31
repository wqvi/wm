#include <sys/wait.h>
#include <errno.h>
#include "wm.h"
#include "client.h"

void client_focus(struct Client *c, int lift) {
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
				wlr_scene_rect_set_color(c->border[i], (float[]){1.0f, 1.0f, 0.0f, 1.0f});
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
				wlr_scene_rect_set_color(old_c->border[i], (float[]){0.5f, 0.5f, 0.5f, 1.0f});

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

void client_resize(struct Client *c, struct wlr_box geo, int interact) {
	struct wlr_box *bbox = interact ? &server->sgeom : &c->mon->w;
	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	// Update scene-graph, including borders
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
	wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
	wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);

	// this is a no-op if size hasn't changed
	c->resize = client_set_size(c, c->geom.width - 2 * c->bw,
			c->geom.height - 2 * c->bw);
}

void client_get_size_hints(struct Client *c, struct wlr_box *max, struct wlr_box *min) {
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_toplevel_state *state;
	toplevel = c->surface->toplevel;
	state = &toplevel->current;
	max->width = state->max_width;
	max->height = state->max_height;
	min->width = state->min_width;
	min->height = state->min_height;
}

struct wlr_surface *client_surface(struct Client *c) {
	return c->surface->surface;
}

int toplevel_from_wlr_surface(struct wlr_surface *s, struct Client **pc, struct LayerSurface **pl) {
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_surface *root_surface;
	struct wlr_layer_surface_v1 *layer_surface;
	struct Client *c = NULL;
	struct LayerSurface *l = NULL;
	int type = -1;

	if (!s)
		return type;
	root_surface = wlr_surface_get_root_surface(s);

	if (wlr_surface_is_layer_surface(root_surface)
			&& (layer_surface = wlr_layer_surface_v1_from_wlr_surface(root_surface))) {
		l = layer_surface->data;
		type = LayerShell;
		goto end;
	}

	if (wlr_surface_is_xdg_surface(root_surface)
			&& (xdg_surface = wlr_xdg_surface_from_wlr_surface(root_surface))) {
		while (1) {
			switch (xdg_surface->role) {
			case WLR_XDG_SURFACE_ROLE_POPUP:
				if (!xdg_surface->popup->parent)
					return -1;
				else if (!wlr_surface_is_xdg_surface(xdg_surface->popup->parent))
					return toplevel_from_wlr_surface(xdg_surface->popup->parent, pc, pl);

				xdg_surface = wlr_xdg_surface_from_wlr_surface(xdg_surface->popup->parent);
				break;
			case WLR_XDG_SURFACE_ROLE_TOPLEVEL:
				c = xdg_surface->data;
				type = c->type;
				goto end;
			case WLR_XDG_SURFACE_ROLE_NONE:
				return -1;
			}
		}
	}

end:
	if (pl)
		*pl = l;
	if (pc)
		*pc = c;
	return type;
}

/* The others */
void client_activate_surface(struct wlr_surface *s, int activated) {
	struct wlr_xdg_surface *surface;

	if (wlr_surface_is_xdg_surface(s)
			&& (surface = wlr_xdg_surface_from_wlr_surface(s))
			&& surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		wlr_xdg_toplevel_set_activated(surface->toplevel, activated);
}

uint32_t client_set_bounds(struct Client *c, int32_t width, int32_t height) {
	if (c->surface->client->shell->version >=
			XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION && width >= 0 && height >= 0)
		return wlr_xdg_toplevel_set_bounds(c->surface->toplevel, width, height);
	return 0;
}

void client_for_each_surface(struct Client *c, wlr_surface_iterator_func_t fn, void *data) {
	wlr_surface_for_each_surface(client_surface(c), fn, data);
	wlr_xdg_surface_for_each_popup_surface(c->surface, fn, data);
}

const char *client_get_appid(struct Client *c) {
	return c->surface->toplevel->app_id;
}

void client_get_geometry(struct Client *c, struct wlr_box *geom) {
	wlr_xdg_surface_get_geometry(c->surface, geom);
}

struct Client *client_get_parent(struct Client *c) {
	struct Client *p = NULL;
	if (c->surface->toplevel->parent)
		toplevel_from_wlr_surface(c->surface->toplevel->parent->base->surface, &p, NULL);

	return p;
}

const char *client_get_title(struct Client *c)
{
	return c->surface->toplevel->title;
}

int client_is_float_type(struct Client *c) {
	struct wlr_box min = {0}, max = {0};
	client_get_size_hints(c, &max, &min);
	return ((min.width > 0 || min.height > 0 || max.width > 0 || max.height > 0)
		&& (min.width == max.width || min.height == max.height));
}

int client_is_mapped(struct Client *c) {
	return c->surface->mapped;
}

int client_is_rendered_on_mon(struct Client *c, struct Monitor *m) {
	/* This is needed for when you don't want to check formal assignment,
	 * but rather actual displaying of the pixels.
	 * Usually VISIBLEON suffices and is also faster. */
	struct wlr_surface_output *s;
	if (!c->scene->node.enabled)
		return 0;
	wl_list_for_each(s, &client_surface(c)->current_outputs, link)
		if (s->output == m->wlr_output)
			return 1;
	return 0;
}

int client_is_stopped(struct Client *c) {
	int pid;
	siginfo_t in = {0};

	wl_client_get_credentials(c->surface->client->client, &pid, NULL, NULL);
	if (waitid(P_PID, pid, &in, WNOHANG|WCONTINUED|WSTOPPED|WNOWAIT) < 0) {
		/* This process is not our child process, while is very unluckely that
		 * it is stopped, in order to do not skip frames assume that it is. */
		if (errno == ECHILD)
			return 1;
	} else if (in.si_pid) {
		if (in.si_code == CLD_STOPPED || in.si_code == CLD_TRAPPED)
			return 1;
		if (in.si_code == CLD_CONTINUED)
			return 0;
	}

	return 0;
}

void client_notify_enter(struct wlr_surface *s, struct wlr_keyboard *kb) {
	if (kb)
		wlr_seat_keyboard_notify_enter(server->seat, s, kb->keycodes,
				kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(server->seat, s, NULL, 0, NULL);
}

void client_restack_surface(struct Client *c) {
	return;
}

void client_send_close(struct Client *c) {
	wlr_xdg_toplevel_send_close(c->surface->toplevel);
}

void client_set_fullscreen(struct Client *c, int fullscreen) {
	wlr_xdg_toplevel_set_fullscreen(c->surface->toplevel, fullscreen);
}

uint32_t client_set_size(struct Client *c, uint32_t width, uint32_t height) {
	if (width == c->surface->toplevel->current.width
			&& height ==c->surface->toplevel->current.height)
		return 0;
	return wlr_xdg_toplevel_set_size(c->surface->toplevel, width, height);
}

void client_set_tiled(struct Client *c, uint32_t edges) {
	wlr_xdg_toplevel_set_tiled(c->surface->toplevel, edges);
}

struct wlr_surface *client_surface_at(struct Client *c, double cx, double cy, double *sx, double *sy) {
	return wlr_xdg_surface_surface_at(c->surface, cx, cy, sx, sy);
}

int client_wants_focus(struct Client *c) {
	return 0;
}

int client_wants_fullscreen(struct Client *c) {
	return c->surface->toplevel->requested.fullscreen;
}

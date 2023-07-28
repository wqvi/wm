/*
 * See LICENSE file for copyright and license details.
 */

#include "wm.h"

/* macros */
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define END(A)                  ((A) + LENGTH(A))
#define TAGMASK                 ((1u << tagcount) - 1)
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
//#define IDLE_NOTIFY_ACTIVITY    wlr_idle_notify_activity(idle, seat), wlr_idle_notifier_v1_notify_activity(idle_notifier, seat)

/* function declarations */
static void applybounds(struct Client *c, struct wlr_box *bbox);

static void applyrules(struct Client *c);

static void arrange(struct Monitor *m);

static void arrangelayer(struct Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive);

static void arrangelayers(struct Monitor *m);

static void closemon(struct Monitor *m);

static void createkeyboard(struct wlr_keyboard *keyboard);

static void createpointer(struct wlr_pointer *pointer);

static struct Monitor *dirtomon(enum wlr_direction dir);

static void focusmon(int dir);

static void focusstack(const union Arg *arg);

static void incnmaster(const union Arg *arg);

static int keybinding(uint32_t mods, xkb_keysym_t sym);

static int keyrepeat(void *data);

static void killclient(const union Arg *arg);

static void maximizenotify(struct wl_listener *listener, void *data);

static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);

static void printstatus(void);

static void setfullscreen(struct Client *c, int fullscreen);

static void setmfact(const union Arg *arg);

static void tag(const union Arg *arg);

static void tagmon(const union Arg *arg);

static void tile(struct Monitor *m);

static void togglefullscreen(const union Arg *arg);

static void view(const union Arg *arg);

/* variables */
static const char broken[] = "broken";
static void *exclusive_focus;
/* Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };

/* global event handlers */


/* configuration, allows nested code to access above variables */
#include "config.h"

/* attempt to encapsulate suck into one file */

/* function implementations */
void
applybounds(struct Client *c, struct wlr_box *bbox)
{
	if (!c->is_fullscreen) {
		struct wlr_box min = {0}, max = {0};
		client_get_size_hints(c, &max, &min);
		/* try to set size hints */
		c->geom.width = MAX(min.width + (2 * (int)c->bw), c->geom.width);
		c->geom.height = MAX(min.height + (2 * (int)c->bw), c->geom.height);
		/* Some clients set their max size to INT_MAX, which does not violate the
		 * protocol but it's unnecesary, as they can set their max size to zero. */
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

void
applyrules(struct Client *c)
{
	/* rule matching */
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

void
arrange(struct Monitor *m)
{
	struct Client *c;
	wl_list_for_each(c, &server->clients, link)
		if (c->mon == m)
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
			(c = focustop(m)) && c->is_fullscreen);

	tile(m);
	motionnotify(0);
	checkidleinhibitor(NULL);
}

void
arrangelayer(struct Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
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

void
arrangelayers(struct Monitor *m)
{
	int i;
	struct wlr_box usable_area = m->m;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	struct LayerSurface *layersurface;
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	if (memcmp(&usable_area, &m->w, sizeof(struct wlr_box))) {
		m->w = usable_area;
		arrange(m);
	}

	/* Arrange non-exlusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	/* Find topmost keyboard interactive layer, if such a layer exists */
	for (i = 0; i < LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(layersurface,
				&m->layers[layers_above_shell[i]], link) {
			if (!server->locked && layersurface->layer_surface->current.keyboard_interactive
					&& layersurface->mapped) {
				/* Deactivate the focused client. */
				focusclient(NULL, 0);
				exclusive_focus = layersurface;
				client_notify_enter(layersurface->layer_surface->surface, wlr_seat_get_keyboard(server->seat));
				return;
			}
		}
	}
}

void
cleanupkeyboard(struct wl_listener *listener, void *data)
{
	struct Keyboard *kb = wl_container_of(listener, kb, destroy);

	wl_event_source_remove(kb->key_repeat_source);
	wl_list_remove(&kb->link);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	free(kb);
}

void
cleanupmon(struct wl_listener *listener, void *data)
{
	struct Monitor *m = wl_container_of(listener, m, destroy);
	struct LayerSurface *l, *tmp;
	int i;

	for (i = 0; i <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; i++)
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);

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

void
closemon(struct Monitor *m)
{
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	struct Client *c;
	if (wl_list_empty(&server->monitors)) {
		server->selmon = NULL;
	} else if (m == server->selmon) {
		int nmons = wl_list_length(&server->monitors), i = 0;
		do /* don't switch to disabled mons */
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
	focusclient(focustop(server->selmon), 1);
	printstatus();
}

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	struct LayerSurface *layersurface = wl_container_of(listener, layersurface, surface_commit);
	struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
	struct wlr_output *wlr_output = wlr_layer_surface->output;
	struct wlr_scene_tree *layer = server->layers[layermap[wlr_layer_surface->current.layer]];

	/* For some reason this layersurface have no monitor, this can be because
	 * its monitor has just been destroyed */
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

void
commitnotify(struct wl_listener *listener, void *data)
{
	struct Client *c = wl_container_of(listener, c, commit);
	struct wlr_box box = {0};
	client_get_geometry(c, &box);

	if (c->mon && !wlr_box_empty(&box) && (box.width != c->geom.width - 2 * c->bw
			|| box.height != c->geom.height - 2 * c->bw))
		c->is_floating ? resize(c, c->geom, 1) : arrange(c->mon);

	/* mark a pending resize as completed */
	if (c->resize && c->resize <= c->surface->current.configure_serial)
		c->resize = 0;
}

void
createdecoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *dec = data;
	wlr_xdg_toplevel_decoration_v1_set_mode(dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	wl_signal_add(&idle_inhibitor->events.destroy, &server->idle_inhibitor_destroy);

	checkidleinhibitor(NULL);
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	struct Keyboard *kb = keyboard->data = ecalloc(1, sizeof(*kb));
	kb->wlr_keyboard = keyboard;

	/* Prepare an XKB keymap and assign it to the keyboard. */
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	keymap = xkb_keymap_new_from_names(context, &xkb_rules,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(keyboard, repeat_rate, repeat_delay);

	/* Here we set up listeners for keyboard events. */
	LISTEN(&keyboard->events.modifiers, &kb->modifiers, keypressmod);
	LISTEN(&keyboard->events.key, &kb->key, keypress);
	LISTEN(&keyboard->base.events.destroy, &kb->destroy, cleanupkeyboard);

	wlr_seat_set_keyboard(server->seat, keyboard);

	kb->key_repeat_source = wl_event_loop_add_timer(
			wl_display_get_event_loop(server->display), keyrepeat, kb);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &kb->link);
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
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

	/* Temporarily set the layer's current state to pending
	 * so that we can easily arrange it
	 */
	old_state = wlr_layer_surface->current;
	wlr_layer_surface->current = wlr_layer_surface->pending;
	layersurface->mapped = 1;
	arrangelayers(layersurface->mon);
	wlr_layer_surface->current = old_state;
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
	struct SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	struct Monitor *m = lock_surface->output->data;
	struct wlr_scene_tree *scene_tree = lock_surface->surface->data =
		wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);

	if (m == server->selmon)
		client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(server->seat));
}

void
createmon(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	const struct MonitorRule *r;
	size_t i;
	struct Monitor *m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* Initialize monitor state using configured rules */
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

	/* The mode is a tuple of (width, height, refresh rate), and each
	 * monitor supports only a specific set of modes. We just pick the
	 * monitor's preferred mode; a more sophisticated compositor would let
	 * the user configure it. */
	wlr_output_set_mode(wlr_output, wlr_output_preferred_mode(wlr_output));

	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);

	wlr_output_enable(wlr_output, 1);
	if (!wlr_output_commit(wlr_output))
		return;

	/* Try to enable adaptive sync, note that not all monitors support it.
	 * wlr_output_commit() will deactivate it in case it cannot be enabled */
	wlr_output_enable_adaptive_sync(wlr_output, 1);
	wlr_output_commit(wlr_output);

	wl_list_insert(&server->monitors, &m->link);
	printstatus();

	/* The xdg-protocol specifies:
	 *
	 * If the fullscreened surface is not opaque, the compositor must make
	 * sure that other screen content not part of the same surface tree (made
	 * up of subsurfaces, popups or similarly coupled surfaces) are not
	 * visible below the fullscreened surface.
	 *
	 */
	/* updatemons() will resize and set correct position */
	m->fullscreen_bg = wlr_scene_rect_create(server->layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	/* Adds this to the output layout in the order it was configured in.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (m->m.x < 0 || m->m.y < 0)
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	else
		wlr_output_layout_add(server->output_layout, wlr_output, m->m.x, m->m.y);
}

void
createnotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup,
	 * or when wlr_layer_shell receives a new popup from a layer.
	 * If you want to do something tricky with popups you should check if
	 * its parent is wlr_xdg_shell or wlr_layer_shell */
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

	/* Allocate a Client for this surface */
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

void
createpointer(struct wlr_pointer *pointer)
{
	if (wlr_input_device_is_libinput(&pointer->base)) {
		struct libinput_device *libinput_device = (struct libinput_device*)
			wlr_libinput_get_device_handle(&pointer->base);

		if (libinput_device_config_tap_get_finger_count(libinput_device)) {
			libinput_device_config_tap_set_enabled(libinput_device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(libinput_device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(libinput_device, drag_lock);
			libinput_device_config_tap_set_button_map(libinput_device, button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(libinput_device))
			libinput_device_config_scroll_set_natural_scroll_enabled(libinput_device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(libinput_device))
			libinput_device_config_dwt_set_enabled(libinput_device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(libinput_device))
			libinput_device_config_left_handed_set(libinput_device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(libinput_device))
			libinput_device_config_middle_emulation_set_enabled(libinput_device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(libinput_device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method (libinput_device, scroll_method);

		if (libinput_device_config_click_get_methods(libinput_device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method (libinput_device, click_method);

		if (libinput_device_config_send_events_get_modes(libinput_device))
			libinput_device_config_send_events_set_mode(libinput_device, send_events_mode);

		if (libinput_device_config_accel_is_available(libinput_device)) {
			libinput_device_config_accel_set_profile(libinput_device, accel_profile);
			libinput_device_config_accel_set_speed(libinput_device, accel_speed);
		}
	}

	wlr_cursor_attach_input_device(server->cursor, &pointer->base);
}

struct Monitor *
dirtomon(enum wlr_direction dir)
{
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

void
focusclient(struct Client *c, int lift)
{
	struct wlr_surface *old = server->seat->keyboard_state.focused_surface;
	int i, unused_lx, unused_ly, old_client_type;
	struct Client *old_c = NULL;
	struct LayerSurface *old_l = NULL;

	if (server->locked)
		return;

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && client_surface(c) == old)
		return;

	if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	/* Put the new client atop the focus stack and select its monitor */
	if (c) {
		wl_list_remove(&c->flink);
		wl_list_insert(&server->focus_stack, &c->flink);
		server->selmon = c->mon;
		c->is_urgent = 0;
		client_restack_surface(c);

		/* Don't change border color if there is an exclusive focus or we are
		 * handling a drag operation */
		if (!exclusive_focus && !server->seat->drag)
			for (i = 0; i < 4; i++)
				wlr_scene_rect_set_color(c->border[i], focuscolor);
	}

	/* Deactivate old client if focus is changing */
	if (old && (!c || client_surface(c) != old)) {
		/* If an overlay is focused, don't focus or activate the client,
		 * but only update its position in fstack to render its border with focuscolor
		 * and focus it after the overlay is closed. */
		if (old_client_type == LayerShell && wlr_scene_node_coords(
					&old_l->scene->node, &unused_lx, &unused_ly)
				&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			return;
		} else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
			return;
		/* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
		 * and probably other clients */
		} else if (old_c && (!c || !client_wants_focus(c))) {
			for (i = 0; i < 4; i++)
				wlr_scene_rect_set_color(old_c->border[i], bordercolor);

			client_activate_surface(old, 0);
		}
	}
	printstatus();

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}

	/* Change cursor surface */
	motionnotify(0);

	/* Have a client, so focus its top-level wlr_surface */
	client_notify_enter(client_surface(c), wlr_seat_get_keyboard(server->seat));

	/* Activate the new client */
	client_activate_surface(client_surface(c), 1);
}

void focusmon(int dir) {
	int i = 0, nmons = wl_list_length(&server->monitors);
	if (nmons)
		do /* don't switch to disabled mons */
			server->selmon = dirtomon(dir);
		while (!server->selmon->wlr_output->enabled && i++ < nmons);
	focusclient(focustop(server->selmon), 1);
}

void
focusstack(const union Arg *arg)
{
	/* Focus the next or previous client (in tiling order) on selmon */
	struct Client *c, *sel = focustop(server->selmon);
	if (!sel || sel->is_fullscreen)
		return;
	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &server->clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, server->selmon))
				break; /* found it */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &server->clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, server->selmon))
				break; /* found it */
		}
	}
	/* If only one client is visible on server->selmon, then c == sel */
	focusclient(c, 1);
}

/* We probably should change the name of this, it sounds like
 * will focus the topmost client of this mon, when actually will
 * only return that client */
struct Client *
focustop(struct Monitor *m)
{
	struct Client *c;
	wl_list_for_each(c, &server->focus_stack, flink)
		if (VISIBLEON(c, m))
			return c;
	return NULL;
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	struct Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

void
incnmaster(const union Arg *arg)
{
	if (!arg || !server->selmon)
		return;
	server->selmon->nmaster = MAX(server->selmon->nmaster + arg->i, 0);
	arrange(server->selmon);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In dwl we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(server->seat, caps);
}

static void spawn_terminal(void) {
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execl("/usr/bin/footclient", "/usr/bin/footclient", NULL);
		die("dwl: execl /usr/bin/footclient failed:");
	}
}

static void spawn_bemenu(void) {
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execl("/usr/bin/bemenu-run", "/usr/bin/bemenu-run", NULL);
		die("bemenu-run: execl /usr/bin/bemenu-run failed:");
	}
}

int keybinding(uint32_t mods, xkb_keysym_t sym) {
	if (!(mods & MODKEY)) return 0;

	if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
		view(&(union Arg){.ui = 1 << (sym - XKB_KEY_1)});
		return 1;
	}
	
	switch (sym) {
		case XKB_KEY_d:
			spawn_bemenu();
			break;
		case XKB_KEY_Return:
			spawn_terminal();
			return 1;
		case XKB_KEY_Left:
		case XKB_KEY_j:
			focusstack(&(union Arg){.i = +1});
			return 1;
		case XKB_KEY_Right:
		case XKB_KEY_k:
			focusstack(&(union Arg){.i = -1});
			return 1;
		case XKB_KEY_Up:
		case XKB_KEY_i:
			incnmaster(&(union Arg){.i = +1});
			return 1;
		case XKB_KEY_Down:
		case XKB_KEY_u:
			incnmaster(&(union Arg){.i = -1});
			return 1;
		case XKB_KEY_Tab:
			view(NULL);
			return 1;
		case XKB_KEY_f:
			togglefullscreen(NULL);
			return 1;
		case XKB_KEY_comma:
			focusmon(WLR_DIRECTION_LEFT);
			return 1;
		case XKB_KEY_period:
			focusmon(WLR_DIRECTION_RIGHT);
			return 1;
		default:
			break;
	}

	if (!(mods & WLR_MODIFIER_SHIFT)) return 0;

	switch (sym) {
		case XKB_KEY_exclam:
			tag(&(union Arg){.ui = 1 << 0});
			view(&(union Arg){.ui = 1 << 0});
			return 1;
		case XKB_KEY_at:	
			tag(&(union Arg){.ui = 1 << 1});
			view(&(union Arg){.ui = 1 << 1});
			return 1;
		case XKB_KEY_numbersign:
			tag(&(union Arg){.ui = 1 << 2});
			view(&(union Arg){.ui = 1 << 2});
			return 1;
		case XKB_KEY_dollar:	
			tag(&(union Arg){.ui = 1 << 3});
			view(&(union Arg){.ui = 1 << 3});
			return 1;
		case XKB_KEY_percent:	
			tag(&(union Arg){.ui = 1 << 4});
			view(&(union Arg){.ui = 1 << 4});
			return 1;
		case XKB_KEY_asciicircum:	
			tag(&(union Arg){.ui = 1 << 5});
			view(&(union Arg){.ui = 1 << 5});
			return 1;
		case XKB_KEY_ampersand:	
			tag(&(union Arg){.ui = 1 << 6});
			view(&(union Arg){.ui = 1 << 6});
			return 1;
		case XKB_KEY_asterisk:
			tag(&(union Arg){.ui = 1 << 7});
			view(&(union Arg){.ui = 1 << 7});
			return 1;
		case XKB_KEY_parenleft:
			tag(&(union Arg){.ui = 1 << 8});
			view(&(union Arg){.ui = 1 << 8});
			return 1;
		default:
			break;
	}

	switch (sym) {
		case XKB_KEY_E:
			wl_display_terminate(server->display);
			return 1;
		case XKB_KEY_Q:
			killclient(NULL);
			return 1;
		case XKB_KEY_Left:
		case XKB_KEY_h:
			setmfact(&(union Arg){.f = -0.05});
			return 1;
		case XKB_KEY_Right:
		case XKB_KEY_l:
			setmfact(&(union Arg){.f = +0.05});
			return 1;
		case XKB_KEY_less:
			tagmon(&(union Arg){.i = WLR_DIRECTION_LEFT});
			focusmon(WLR_DIRECTION_LEFT);
			return 1;
		case XKB_KEY_greater:
			tagmon(&(union Arg){.i = WLR_DIRECTION_RIGHT});
			focusmon(WLR_DIRECTION_RIGHT);
			return 1;

		default:
			break;
	}

	return 0;
}

void
keypress(struct wl_listener *listener, void *data)
{
	int i;
	/* This event is raised when a key is pressed or released. */
	struct Keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_keyboard_key_event *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			kb->wlr_keyboard->xkb_state, keycode, &syms);

	int handled = 0;
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

	IDLE_NOTIFY_ACTIVITY;

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!server->locked && !server->input_inhibit_mgr->active_inhibitor
			&& event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		for (i = 0; i < nsyms; i++)
			handled = keybinding(mods, syms[i]) || handled;

	if (handled && kb->wlr_keyboard->repeat_info.delay > 0) {
		kb->mods = mods;
		kb->keysyms = syms;
		kb->nsyms = nsyms;
		wl_event_source_timer_update(kb->key_repeat_source,
				kb->wlr_keyboard->repeat_info.delay);
	} else {
		kb->nsyms = 0;
		wl_event_source_timer_update(kb->key_repeat_source, 0);
	}

	if (!handled) {
		/* Pass unhandled keycodes along to the client. */
		wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
	}
}

void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct Keyboard *kb = wl_container_of(listener, kb, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(server->seat,
		&kb->wlr_keyboard->modifiers);
}

int
keyrepeat(void *data)
{
	struct Keyboard *kb = data;
	int i;
	if (kb->nsyms && kb->wlr_keyboard->repeat_info.rate > 0) {
		wl_event_source_timer_update(kb->key_repeat_source,
				1000 / kb->wlr_keyboard->repeat_info.rate);

		for (i = 0; i < kb->nsyms; i++)
			keybinding(kb->mods, kb->keysyms[i]);
	}

	return 0;
}

void
killclient(const union Arg *arg)
{
	struct Client *sel = focustop(server->selmon);
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
pointerfocus(struct Client *c, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;
	int internal_call = !time;

	if (!internal_call && c)
		focusclient(c, 0);

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(server->seat);
		return;
	}

	if (internal_call) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
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
		if ((c = focustop(m))) {
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

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const union Arg *arg)
{
	float f;

	if (!arg || !server->selmon)
		return;
	f = arg->f < 1.0 ? arg->f + server->selmon->mfact : arg->f - 1.0;
	if (f < 0.1 || f > 0.9)
		return;
	server->selmon->mfact = f;
	arrange(server->selmon);
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
	focusclient(focustop(server->selmon), 1);
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
tag(const union Arg *arg)
{
	struct Client *sel = focustop(server->selmon);
	if (sel && arg->ui & TAGMASK) {
		sel->tags = arg->ui & TAGMASK;
		focusclient(focustop(server->selmon), 1);
		arrange(server->selmon);
	}
	printstatus();
}

void
tagmon(const union Arg *arg)
{
	struct Client *sel = focustop(server->selmon);
	if (sel)
		setmon(sel, dirtomon(arg->i), 0);
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

void
togglefullscreen(const union Arg *arg)
{
	struct Client *sel = focustop(server->selmon);
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
	if (layersurface == exclusive_focus)
		exclusive_focus = NULL;
	if (layersurface->layer_surface->output
			&& (layersurface->mon = layersurface->layer_surface->output->data))
		arrangelayers(layersurface->mon);
	if (layersurface->layer_surface->surface ==
			server->seat->keyboard_state.focused_surface)
		focusclient(focustop(server->selmon), 1);
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
		focusclient(focustop(server->selmon), 1);
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
	if (c == focustop(c->mon))
		printstatus();
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (c && c != focustop(server->selmon)) {
		c->is_urgent = 1;
		printstatus();
	}
}

void
view(const union Arg *arg)
{
	if (!server->selmon || (arg->ui & TAGMASK) == server->selmon->tagset[server->selmon->seltags])
		return;
	server->selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		server->selmon->tagset[server->selmon->seltags] = arg->ui & TAGMASK;
	focusclient(focustop(server->selmon), 1);
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

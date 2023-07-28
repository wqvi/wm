#include "wm.h"

static void focus_prev(void) {
	struct Client *c, *sel = get_top(server->selmon);
	if (!sel || sel->is_fullscreen)
		return;

	wl_list_for_each_reverse(c, &sel->link, link) {
		if (&c->link == &server->clients)
			continue; // wrap past the sentinel node
		if (VISIBLEON(c, server->selmon))
			break; // found it
	}

	// if only one client is visible on server->selmon, then c == sel
	focusclient(c, 1);
}

static void focus_next(void) {
	struct Client *c, *sel = get_top(server->selmon);
	if (!sel || sel->is_fullscreen)
		return;

	wl_list_for_each(c, &sel->link, link) {
		if (&c->link == &server->clients)
			continue; // wrap past the sentinel node
		if (VISIBLEON(c, server->selmon))
			break; // found it - dwl source code author
	}

	// if only one client is visible on server->selmon, then c == sel
	focusclient(c, 1);
}

void axisnotify(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	IDLE_NOTIFY_ACTIVITY;
	/* TODO: allow usage of scroll whell for mousebindings, it can be implemented
	 * checking the event's orientation and the delta of the event */
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source);
}

static void spawn_wpctl(const char *percentage) {
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execl("/usr/bin/wpctl", "/usr/bin/wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", percentage, NULL);
		die("dwl: execl /usr/bin/playerctl failed:");
	}
}

static void spawn_playerctl(const char *operation) {
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execl("/usr/bin/playerctl", "/usr/bin/playerctl", operation, NULL);
		die("dwl: execl /usr/bin/playerctl failed:");
	}
}

static void handle_mouse_button(uint32_t mods, unsigned int button) {
	if (mods & MODKEY && button == BTN_SIDE) {
		spawn_playerctl("play-pause");
		return;
	}

	if (button == BTN_EXTRA && mods & WLR_MODIFIER_SHIFT) {
		spawn_wpctl("2%+");
		return;
	} else if (button == BTN_SIDE && mods & WLR_MODIFIER_SHIFT) {
		spawn_wpctl("2%-");
		return;
	}

	if (button == BTN_EXTRA) {
		spawn_playerctl("next");
		return;
	} else if (button == BTN_SIDE) {
		spawn_playerctl("previous");
		return;
	}
}

void buttonpress(struct wl_listener *listener, void *data) {
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	struct Client *c;

	IDLE_NOTIFY_ACTIVITY;

	switch (event->state) {
	case WLR_BUTTON_PRESSED:
		server->cursor_mode = CurPressed;
		if (server->locked)
			break;

		/* Change focus if the button was _pressed_ over a client */
		xytonode(server->cursor->x, server->cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && (client_wants_focus(c)))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(server->seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		handle_mouse_button(mods, event->button);
		break;
	case WLR_BUTTON_RELEASED:
		/* If you released any buttons, we exit interactive move/resize mode. */
		if (!server->locked && server->cursor_mode != CurNormal && server->cursor_mode != CurPressed) {
			server->cursor_mode = CurNormal;
			/* Clear the pointer focus, this way if the cursor is over a surface
			 * we will send an enter event after which the client will provide us
			 * a cursor surface */
			wlr_seat_pointer_clear_focus(server->seat);
			motionnotify(0);
			/* Drop the window off on its new monitor */
			server->selmon = xytomon(server->output_layout, server->cursor->x, server->cursor->y);
			setmon(server->grabc, server->selmon, 0);
			return;
		} else {
			server->cursor_mode = CurNormal;
		}
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
}

void cursorframe(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

void motionnotify(uint32_t time) {
	double sx = 0, sy = 0;
	struct Client *c = NULL, *w = NULL;
	struct LayerSurface *l = NULL;
	int type;
	struct wlr_surface *surface = NULL;

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		IDLE_NOTIFY_ACTIVITY;

		/* Update selmon (even while dragging a window) */
		server->selmon = xytomon(server->output_layout, server->cursor->x, server->cursor->y);
	}

	/* Update drag icon's position */

	/* If we are currently grabbing the mouse, handle and return */
	if (server->cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		resize(server->grabc, (struct wlr_box){.x = server->cursor->x - server->grabcx, .y = server->cursor->y - server->grabcy,
			.width = server->grabc->geom.width, .height = server->grabc->geom.height}, 1);
		return;
	} else if (server->cursor_mode == CurResize) {
		resize(server->grabc, (struct wlr_box){.x = server->grabc->geom.x, .y = server->grabc->geom.y,
			.width = server->cursor->x - server->grabc->geom.x, .height = server->cursor->y - server->grabc->geom.y}, 1);
		return;
	}

	/* Find the client under the pointer and send the event along. */
	xytonode(server->cursor->x, server->cursor->y, &surface, &c, NULL, &sx, &sy);

	if (server->cursor_mode == CurPressed && !server->seat->drag) {
		if ((type = toplevel_from_wlr_surface(
				 server->seat->pointer_state.focused_surface, &w, &l)) >= 0) {
			c = w;
			surface = server->seat->pointer_state.focused_surface;
			sx = server->cursor->x - (type == LayerShell ? l->geom.x : w->geom.x);
			sy = server->cursor->y - (type == LayerShell ? l->geom.y : w->geom.y);
		}
	}

	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !server->seat->drag && (!server->cursor_image || strcmp(server->cursor_image, "left_ptr")))
		wlr_xcursor_manager_set_cursor_image(server->cursor_mgr, (server->cursor_image = "left_ptr"), server->cursor);

	pointerfocus(c, surface, sx, sy, time);
}

void motionrelative(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
	motionnotify(event->time_msec);
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
		view(1 << (sym - XKB_KEY_1));
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
			focus_next();
			return 1;
		case XKB_KEY_Right:
		case XKB_KEY_k:
			focus_prev();
			return 1;
		case XKB_KEY_Up:
		case XKB_KEY_i:
			incnmaster(+1);
			return 1;
		case XKB_KEY_Down:
		case XKB_KEY_u:
			incnmaster(-1);
			return 1;
		case XKB_KEY_Tab:
			view(0);
			return 1;
		case XKB_KEY_f:
			togglefullscreen();
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
			tag(1 << 0);
			view(1 << 0);
			return 1;
		case XKB_KEY_at:	
			tag(1 << 1);
			view(1 << 1);
			return 1;
		case XKB_KEY_numbersign:
			tag(1 << 2);
			view(1 << 2);
			return 1;
		case XKB_KEY_dollar:	
			tag(1 << 3);
			view(1 << 3);
			return 1;
		case XKB_KEY_percent:	
			tag(1 << 4);
			view(1 << 4);
			return 1;
		case XKB_KEY_asciicircum:	
			tag(1 << 5);
			view(1 << 5);
			return 1;
		case XKB_KEY_ampersand:	
			tag(1 << 6);
			view(1 << 6);
			return 1;
		case XKB_KEY_asterisk:
			tag(1 << 7);
			view(1 << 7);
			return 1;
		case XKB_KEY_parenleft:
			tag(1 << 8);
			view(1 << 8);
			return 1;
		case XKB_KEY_E:
			wl_display_terminate(server->display);
			return 1;
		case XKB_KEY_Q:
			killclient();
			return 1;
		case XKB_KEY_less:
			tagmon(WLR_DIRECTION_LEFT);
			focusmon(WLR_DIRECTION_LEFT);
			return 1;
		case XKB_KEY_greater:
			tagmon(WLR_DIRECTION_RIGHT);
			focusmon(WLR_DIRECTION_RIGHT);
			return 1;
		default:
			break;
	}

	return 0;
}

void keypress(struct wl_listener *listener, void *data) {
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

void keypressmod(struct wl_listener *listener, void *data) {
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

int keyrepeat(void *data) {
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

void cleanupkeyboard(struct wl_listener *listener, void *data) {
	struct Keyboard *kb = wl_container_of(listener, kb, destroy);

	wl_event_source_remove(kb->key_repeat_source);
	wl_list_remove(&kb->link);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	free(kb);
}

void createkeyboard(struct wlr_keyboard *keyboard) {
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	struct Keyboard *kb = keyboard->data = ecalloc(1, sizeof(*kb));
	kb->wlr_keyboard = keyboard;

	// Prepare an XKB keymap and assign it to the keyboard.
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(keyboard, 25, 600);

	// Here we set up listeners for keyboard events. 
	LISTEN(&keyboard->events.modifiers, &kb->modifiers, keypressmod);
	LISTEN(&keyboard->events.key, &kb->key, keypress);
	LISTEN(&keyboard->base.events.destroy, &kb->destroy, cleanupkeyboard);

	wlr_seat_set_keyboard(server->seat, keyboard);

	kb->key_repeat_source = wl_event_loop_add_timer(
			wl_display_get_event_loop(server->display), keyrepeat, kb);

	// And add the keyboard to our list of keyboards
	wl_list_insert(&server->keyboards, &kb->link);
}

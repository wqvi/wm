#include "wm.h"

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
	if (!surface && !server->seat->drag)
		wlr_xcursor_manager_set_cursor_image(server->cursor_mgr, "left_ptr", server->cursor);

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


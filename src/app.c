#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
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
#include "wm.h"

static void quit(const union Arg *arg);

static void handlesig(int signo);

void quit(const union Arg *arg) {
	// wl_display_terminate(dpy);
}

void handlesig(int signo) {
	if (signo == SIGCHLD) {
		while (waitpid(-1, NULL, WNOHANG) > 0);
	} else if (signo == SIGINT || signo == SIGTERM) {
		quit(NULL);
	}
}

void setup(struct server *server) {
	int sig[4] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (int i = 0; i < 4; i++) {
		sigaction(sig[i], &sa, NULL);
	}

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	server->display = wl_display_create();

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. The NULL argument here optionally allows you
	 * to pass in a custom renderer if wlr_renderer doesn't meet your needs. The
	 * backend uses the renderer, for example, to fall back to software cursors
	 * if the backend does not support hardware cursors (some older GPUs
	 * don't). */
	if (!(server->backend = wlr_backend_autocreate(server->display)))
		die("couldn't create backend");

	/* Initialize the scene graph used to lay out windows */
	server->scene = wlr_scene_create();
	for (int i = 0; i < NUM_LAYERS; i++) {
		server->layers[i] = wlr_scene_tree_create(&server->scene->tree);
	}

	/* Create a renderer with the default implementation */
	if (!(server->renderer = wlr_renderer_autocreate(server->backend)))
		die("couldn't create renderer");
	wlr_renderer_init_wl_display(server->renderer, server->display);

	/* Create a default allocator */
	if (!(server->allocator = wlr_allocator_autocreate(server->backend, server->renderer)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	server->compositor = wlr_compositor_create(server->display, server->renderer);
	wlr_export_dmabuf_manager_v1_create(server->display);
	wlr_screencopy_manager_v1_create(server->display);
	wlr_data_control_manager_v1_create(server->display);
	wlr_data_device_manager_create(server->display);
	wlr_gamma_control_manager_v1_create(server->display);
	wlr_primary_selection_v1_device_manager_create(server->display);
	wlr_viewporter_create(server->display);
	wlr_single_pixel_buffer_manager_v1_create(server->display);
	wlr_subcompositor_create(server->display);

	/* Initializes the interface used to implement urgency hints */
	server->activation = wlr_xdg_activation_v1_create(server->display);
	wl_signal_add(&server->activation->events.request_activate, &server->request_activate);

	wl_list_init(&server->processes);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server->output_layout = wlr_output_layout_create();
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);
	wlr_xdg_output_manager_v1_create(server->display, server->output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server->monitors);
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	/* Set up our client lists and the xdg-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&server->clients);
	wl_list_init(&server->focus_stack);

	server->idle = wlr_idle_create(server->display);
	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);

	server->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server->display);
	wl_signal_add(&server->idle_inhibit_mgr->events.new_inhibitor, &server->idle_inhibitor_create);

	server->layer_shell = wlr_layer_shell_v1_create(server->display);
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_shell_surface);

	server->xdg_shell = wlr_xdg_shell_create(server->display, 4);
	wl_signal_add(&server->xdg_shell->events.new_surface, &server->new_xdg_surface);

	server->input_inhibit_mgr = wlr_input_inhibit_manager_create(server->display);
	server->session_lock_mgr = wlr_session_lock_manager_v1_create(server->display);
	wl_signal_add(&server->session_lock_mgr->events.new_lock, &server->session_lock_create_lock);
	wl_signal_add(&server->session_lock_mgr->events.destroy, &server->session_lock_mgr_destroy);
	server->locked_bg = wlr_scene_rect_create(server->layers[LyrBlock], server->sgeom.width, server->sgeom.height,
			(float [4]){0.1, 0.1, 0.1, 1.0});
	wlr_scene_node_set_enabled(&server->locked_bg->node, 0);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(server->display),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	server->xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(server->display);
	wl_signal_add(&server->xdg_decoration_mgr->events.new_toplevel_decoration, &server->new_xdg_decoration);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in my
	 * input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server->keyboards);
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
	server->virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(server->display);
	wl_signal_add(&server->virtual_keyboard_mgr->events.new_virtual_keyboard,
			&server->new_virtual_keyboard);
	server->seat = wlr_seat_create(server->display, "seat0");
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_sel);
	wl_signal_add(&server->seat->events.request_set_primary_selection, &server->request_set_psel);

	server->output_mgr = wlr_output_manager_v1_create(server->display);
	wl_signal_add(&server->output_mgr->events.apply, &server->output_mgr_apply);
	wl_signal_add(&server->output_mgr->events.test, &server->output_mgr_test);

	wlr_scene_set_presentation(server->scene, wlr_presentation_create(server->display, server->backend));
}

void run(struct server *server) {
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server->display);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server->backend))
		die("startup: backend_start");

	run_daemon("/usr/bin/foot --server", &server->processes, server->activation, server->seat);
	//run_subprocess("/usr/bin/dbus-update-activation-environment --all");
	//run_subprocess("/usr/bin/gentoo-pipewire-launcher");
	run_child("/home/mynah/Documents/Programming/somebar/build/somebar", &server->processes, server->activation, server->seat);

	//printstatus();

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	server->selmon = xytomon(server->output_layout, server->cursor->x, server->cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping. still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(server->cursor, NULL, server->cursor->x, server->cursor->y);
	wlr_xcursor_manager_set_cursor_image(server->cursor_mgr, "left_ptr", server->cursor);

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(server->display);
}

void cleanup(struct server *server) {
	wl_display_destroy_clients(server->display);
	wlr_backend_destroy(server->backend);
	wlr_scene_node_destroy(&server->scene->tree.node);
	wlr_renderer_destroy(server->renderer);
	wlr_allocator_destroy(server->allocator);
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	wlr_cursor_destroy(server->cursor);
	wlr_output_layout_destroy(server->output_layout);
	wlr_seat_destroy(server->seat);
	wl_display_destroy(server->display);
}

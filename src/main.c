#include <stdlib.h>
#include <wlr/util/log.h>
#include "wm.h"

int main(int argc, char *argv[]) {
	int c;

	while ((c = getopt(argc, argv, "s:hv")) != -1) {
		if (c == 'v')
			die("dwl " VERSION);
		else
			goto usage;
	}
	if (optind < argc)
		goto usage;

	wlr_log_init(WLR_DEBUG, NULL);
	/* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");

	setup();
	run();
	cleanup();
	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-v] [-s startup command]", argv[0]);
}

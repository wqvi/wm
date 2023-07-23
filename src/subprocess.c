#include "wm.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/util/log.h>

static void process_destroy(struct process *process) {
	if (!process) return;

	wl_list_remove(&process->token_destroy.link);
	wl_list_remove(&process->link);
	wlr_xdg_activation_token_v1_destroy(process->token);
	free(process);
	wlr_log(WLR_INFO, "process finished!\n");
}

static void token_destroy(struct wl_listener *listener, void *data) {
	struct process *process = wl_container_of(listener, process, token_destroy);
	process->token = NULL;
	process_destroy(process);
}

int run_daemon(const char *cmd, struct wl_list *processes, struct wlr_xdg_activation_v1 *activation, struct wlr_seat *seat) {
	pid_t pid;
	pid_t child;
	struct wlr_xdg_activation_token_v1 *token;
	struct process *process;
	ssize_t s = 0;
	int fd[2];
	if (pipe(fd) == -1) return -1;

	token = wlr_xdg_activation_token_v1_create(activation);
	token->seat = seat;
	process = malloc(sizeof(struct process));
	process->token = token;
	process->token_destroy.notify = token_destroy;
	wl_list_init(&process->link);

	if ((pid = fork()) == 0) {
		sigset_t set;
		setsid();
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGPIPE, SIG_DFL);
		close(fd[0]);

		if ((child = fork()) == 0) {
			const char *xdg_token_name = wlr_xdg_activation_token_v1_get_name(token);
			setenv("XDG_ACTIVATION_TOKEN", xdg_token_name, 1);
			close(fd[1]);
			execlp("sh", "sh", "-c", cmd, NULL);
			_exit(1);
		}

		s = 0;
		while ((size_t)s < sizeof(pid_t)) {
			s += write(fd[1], ((uint8_t *)&child) + s, sizeof(pid_t) - s);
		}
		close(fd[1]);
		_exit(0);
	} else if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}

	close(fd[1]);
	s = 0;
	while ((size_t)s < sizeof(pid_t)) {
		s += read(fd[0], ((uint8_t *)&child) + s, sizeof(pid_t) - s);
	}
	close(fd[0]);

	waitpid(pid, NULL, 0);

	wl_signal_add(&token->events.destroy, &process->token_destroy);

	wl_list_insert(processes, &process->link);

	wlr_log(WLR_INFO, "Created process \"%s\"", cmd);

	return 0;
}

int run_child(const char *cmd, struct wl_list *processes, struct wlr_xdg_activation_v1 *activation, struct wlr_seat *seat) {
	return 0;
}

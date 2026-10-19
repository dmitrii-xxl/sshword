#include <sys/epoll.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <pty.h>
#include <utmp.h>
#include <errno.h>
#include <string.h>

#define CHECK_ERRNO(func) ({\
	int result = func; \
	if (result == -1) { \
		printf("Error calling " #func ": %s\n", strerror(errno));\
		exit(1);\
	}\
	result; \
})

static inline void handle_master_fd(int master_fd) {
	char buf[1024];
	ssize_t count = read(master_fd, buf, sizeof(buf));
	printf("%.*s", count, buf);
	fflush(stdout);
}

static inline void handle_input(int master_fd) {
	char buf[1024];
	ssize_t count = CHECK_ERRNO(read(STDIN_FILENO, buf, sizeof buf));
	write(master_fd, buf, count);
}

int main(int argc, char ** argv) {
	if (argc < 2) {
		printf("not enough arguments: %d, the argument should be login@host\n", argc);
		exit(1);
	}
	char *login_cmd = argv[1];
	int master_fd;
	int slave_fd;
	int result = openpty(&master_fd, &slave_fd, NULL, NULL, NULL);
	pid_t pid = fork();
	if (!pid) {
		login_tty(slave_fd);
		char *command = "/usr/bin/ssh";
		char *const arguments[] = {"ssh", login_cmd, NULL};
		execve(command, arguments, NULL);
	} else {
		char buf[8192];

		// read password prompt
		ssize_t count = read(master_fd, buf, sizeof(buf));
		printf("%.*s", count, buf);
		fflush(stdout);

		// enter password from env variable
		char env_str[32];
		char *env_pwd = getenv("SSH_PASSWORD");
		snprintf(env_str, sizeof env_str, "%s\n", env_pwd);
		write(master_fd, env_str, strlen(env_str));

		// read ssh connection message
		count = read(master_fd, buf, sizeof(buf));
		printf("%.*s", count, buf);
		fflush(stdout);

		count = read(master_fd, buf, sizeof(buf));
		printf("%.*s", count, buf);
		fflush(stdout);

		// prepare epoll for loop usage
		int epoll = CHECK_ERRNO(epoll_create(1));
		struct epoll_event ev, events[32];
		ev.events = EPOLLIN;

		ev.data.fd = master_fd;
		CHECK_ERRNO(epoll_ctl(epoll, EPOLL_CTL_ADD, master_fd, &ev));

		ev.data.fd = STDIN_FILENO;
		CHECK_ERRNO(epoll_ctl(epoll, EPOLL_CTL_ADD, STDIN_FILENO, &ev));

		// communication loop
		for (;;) {
			int number_ready = epoll_wait(epoll, events, 32, 0); //TODO: Replace 32 with sizeof(events) / sizeof (*events)
			for (int i = 0; i < number_ready; i++) {
				struct epoll_event event = events[i];
				if (event.data.fd == master_fd) {
					handle_master_fd(master_fd);
				} else if (event.data.fd == STDIN_FILENO) {
					handle_input(master_fd);
				} else {
					printf("unknown fd: %d\n", event.data.fd);
					exit(1);
				}
			}
		}
	}
}


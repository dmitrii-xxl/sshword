#include <sys/epoll.h>
#include <syslog.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <pty.h>
#include <utmp.h>
#include <errno.h>
#include <string.h>
#include <termios.h>
#include <signal.h>

// volatile:     since the signal handler is called outside normal execution flow
// sig_atomic_t: since reading normal int/long/etc... are not necesserily atomic and signal may be received during execution of any instruction
static volatile sig_atomic_t sigchld;

#define CHECK_ERRNO(func) ({\
	int result = func; \
	if (result == -1) { \
		syslog(LOG_ERR, "Error calling " #func ": %s\n", strerror(errno));\
		exit(1);\
	}\
	result; \
})

static inline void handle_master_fd(int master_fd) {
	char buf[1024];
	ssize_t count = CHECK_ERRNO(read(master_fd, buf, sizeof buf));
	syslog(LOG_INFO, "read %zd bytes from %d master_fd", count, master_fd);
	printf("%.*s", count, buf);
	fflush(stdout);
}

static inline void handle_input(int master_fd) {
	char buf[1024];
	ssize_t count = CHECK_ERRNO(read(STDIN_FILENO, buf, sizeof buf));
	syslog(LOG_INFO, "read %zd bytes from stdin", count);
	write(master_fd, buf, count);
}

void sigchld_handler(int signum) {
	sigchld = 1;
}

int main(int argc, char ** argv) {
	if (argc < 2) {
		fprintf(stderr, "not enough arguments: %d, the argument should be login@host\n", argc);
		exit(1);
	}

	const char *tty_name = ttyname(STDIN_FILENO);
	if (!tty_name) {
		fprintf(stderr, "no controlling terminal found, please run from terminal");
		exit(1);
	}
	openlog("ssh_nopass", LOG_PID, LOG_USER);
	syslog(LOG_INFO, "controlling terminal = %s\n", tty_name);

	// register sigchld signal handler
	struct sigaction act;
	act.sa_handler = sigchld_handler;
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);
	CHECK_ERRNO(sigaction(SIGCHLD, &act, NULL));

	// int tty_fd = open(tty_name, O_RDWR);
	// an fd attached to a terminal
	int tty_fd = STDIN_FILENO;
	syslog(LOG_INFO, "tty_fd = %d\n", tty_fd);

	// set raw mode to the controlling terminal, i.e. the one who ran this program
	struct termios termios;
	int retval_getattr = tcgetattr(tty_fd, &termios);
	struct termios current = termios;
	syslog(LOG_INFO, "getattr retval = %d\n", retval_getattr);
	cfmakeraw(&termios);
	int retval_setattr = tcsetattr(tty_fd, TCSANOW, &termios);
	syslog(LOG_INFO, "setattr retval = %d\n", retval_setattr);

	// ssh login command
	char *login_cmd = argv[1];

	// open terminal for ssh
	int master_fd;
	int slave_fd;
	int result = openpty(&master_fd, &slave_fd, NULL, NULL, NULL);
	syslog(LOG_INFO, "master terminal = %s\n", ttyname(master_fd));
	syslog(LOG_INFO, "slave terminal = %s\n", ttyname(slave_fd));
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
		syslog(LOG_INFO, "%.*s", count, buf);

		// enter password from env variable
		char env_str[32];
		char *env_pwd = getenv("SSH_PASSWORD");
		snprintf(env_str, sizeof env_str, "%s\n", env_pwd);
		write(master_fd, env_str, strlen(env_str));

		// read ssh connection message
		count = read(master_fd, buf, sizeof(buf));
		syslog(LOG_INFO, "%.*s", count, buf); //TODO: logs [39B blob data] instead of actual data

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
			if (sigchld) {
				syslog(LOG_INFO, "Recieved signal: SIGCHLD");
				tcsetattr(STDIN_FILENO, TCSANOW, &current);
				_exit(0);
			}
			int number_ready = epoll_wait(epoll, events, 32, 0); //TODO: Replace 32 with sizeof(events) / sizeof (*events)
			for (int i = 0; i < number_ready; i++) {
				struct epoll_event event = events[i];
				if (event.data.fd == master_fd) {
					handle_master_fd(master_fd);
				} else if (event.data.fd == STDIN_FILENO) {
					handle_input(master_fd);
				} else {
					syslog(LOG_ERR, "unknown fd: %d\n", event.data.fd);
					exit(1);
				}
			}
		}
	}
}


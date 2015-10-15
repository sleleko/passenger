/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2013-2014 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  See LICENSE file for license information.
 */

/* This tool touches everything in a directory every 30 minutes to prevent
 * /tmp cleaners from removing it.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <Constants.h>

#define ERROR_PREFIX "*** TempDirToucher error"

static char *dir;
/**
 * When Passenger Standalone is started with --daemonize, then it will
 * pass --cleanup to this tool so that this tool is responsible
 * for cleaning up the Standalone temp dir. This is because Passenger
 * Standalone may be started in daemonize mode, which makes it exit asap
 * in order to conserve memory. Passenger Standalone can therefore not
 * be responsible for cleaning up the temp dir.
 */
static int shouldCleanup = 0;
static int shouldDaemonize = 0;
static bool verbose = false;
static const char *pidFile = NULL;
static const char *logFile = NULL;
static int sleepInterval = 1800;
static int terminationPipe[2];
static sig_atomic_t shouldIgnoreNextTermSignal = 0;

#define DEBUG(message) \
	do { \
		if (verbose) { \
			puts(message); \
		} \
	} while (false)


static void
usage() {
	printf("Usage: " AGENT_EXE " temp-dir-watcher <DIRECTORY> [OPTIONS...]\n");
	printf("Touches everything in a directory every 30 minutes, to "
		"prevent /tmp cleaners from removing the directory.\n");
	printf("\n");
	printf("Options:\n");
	printf("  --cleanup           Remove directory on exit\n");
	printf("  --daemonize         Daemonize into background\n");
	printf("  --interval SECONDS  Customize interval\n");
	printf("  --pid-file PATH     Save PID into the given file\n");
	printf("  --log-file PATH     Use the given log file\n");
	printf("  --verbose           Print debugging messages\n");
}

static void
parseArguments(int argc, char *argv[], int offset) {
	dir = argv[offset];
	int i;

	if (dir == NULL) {
		usage();
		exit(1);
	}

	for (i = offset + 1; i < argc; i++) {
		if (strcmp(argv[i], "--cleanup") == 0) {
			shouldCleanup = 1;
		} else if (strcmp(argv[i], "--daemonize") == 0) {
			shouldDaemonize = 1;
		} else if (strcmp(argv[i], "--interval") == 0) {
			if (i == argc - 1) {
				fprintf(stderr, ERROR_PREFIX ": --interval requires an argument\n");
				exit(1);
			}
			sleepInterval = atoi(argv[i + 1]);
			i++;
		} else if (strcmp(argv[i], "--pid-file") == 0) {
			pidFile = argv[i + 1];
			i++;
		} else if (strcmp(argv[i], "--log-file") == 0) {
			logFile = argv[i + 1];
			i++;
		} else if (strcmp(argv[i], "--verbose") == 0) {
			verbose = true;
		} else {
			fprintf(stderr, ERROR_PREFIX ": unrecognized argument %s\n",
				argv[i]);
			exit(1);
		}
	}
}

static void
setNonBlocking(int fd) {
	int flags, ret, e;

	do {
		flags = fcntl(fd, F_GETFL);
	} while (flags == -1 && errno == EINTR);
	if (flags == -1) {
		e = errno;
		fprintf(stderr, ERROR_PREFIX
			": cannot set pipe to non-blocking mode: "
			"cannot get file descriptor flags. Error: %s (errno %d)\n",
			strerror(e), e);
		exit(1);
	}
	do {
		ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	} while (ret == -1 && errno == EINTR);
	if (ret == -1) {
		e = errno;
		fprintf(stderr, ERROR_PREFIX
			": cannot set pipe to non-blocking mode: "
			"cannot set file descriptor flags. Error: %s (errno %d)\n",
			strerror(e), e);
		exit(1);
	}
}

static void
initialize(int argc, char *argv[], int offset) {
	int e, fd;

	parseArguments(argc, argv, offset);

	if (logFile != NULL) {
		fd = open(logFile, O_WRONLY | O_APPEND | O_CREAT, 0644);
		if (fd == -1) {
			e = errno;
			fprintf(stderr, ERROR_PREFIX
				": cannot open log file %s for writing: %s (errno %d)\n",
				logFile, strerror(e), e);
			exit(1);
		}

		if (dup2(fd, 1) == -1) {
			e = errno;
			fprintf(stderr, ERROR_PREFIX ": cannot dup2(%d, 1): %s (errno %d)\n",
				fd, strerror(e), e);
		}
		if (dup2(fd, 2) == -1) {
			e = errno;
			fprintf(stderr, ERROR_PREFIX ": cannot dup2(%d, 2): %s (errno %d)\n",
				fd, strerror(e), e);
		}
		close(fd);
	}

	if (pipe(terminationPipe) == -1) {
		e = errno;
		fprintf(stderr, ERROR_PREFIX ": cannot create a pipe: %s (errno %d)\n",
			strerror(e), e);
		exit(1);
	}

	setNonBlocking(terminationPipe[1]);

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
}

static void
exitHandler(int signo) {
	if (shouldIgnoreNextTermSignal) {
		shouldIgnoreNextTermSignal = 0;
	} else {
		int ret = write(terminationPipe[1], "x", 1);
		// We can't do anything about failures, so ignore
		// compiler warnings about not using the result.
		(void) ret;
	}
}

static void
ignoreNextTermSignalHandler(int signo) {
	shouldIgnoreNextTermSignal = 1;
}

static void
installSignalHandlers() {
	struct sigaction action;

	action.sa_handler = exitHandler;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	action.sa_handler = ignoreNextTermSignalHandler;
	action.sa_flags = SA_RESTART;
	sigaction(SIGHUP, &action, NULL);
}

static void
redirectStdinToNull() {
	int fd = open("/dev/null", O_RDONLY);
	if (fd != -1) {
		dup2(fd, 0);
		close(fd);
	}
}

static void
maybeDaemonize() {
	pid_t pid;
	int e;

	if (shouldDaemonize) {
		pid = fork();
		if (pid == 0) {
			setsid();
			if (chdir("/") == -1) {
				e = errno;
				fprintf(stderr, ERROR_PREFIX
					": cannot change working directory to /: %s (errno=%d)\n",
					strerror(e), e);
				_exit(1);
			}
			redirectStdinToNull();
		} else if (pid == -1) {
			e = errno;
			fprintf(stderr, ERROR_PREFIX ": cannot fork: %s (errno=%d)\n",
				strerror(e), e);
			exit(1);
		} else {
			_exit(0);
		}
	}
}

static void
maybeWritePidfile() {
	FILE *f;

	if (pidFile != NULL) {
		f = fopen(pidFile, "w");
		if (f != NULL) {
			fprintf(f, "%d\n", (int) getpid());
			fclose(f);
		} else {
			fprintf(stderr, ERROR_PREFIX ": cannot open PID file %s for writing\n",
				pidFile);
			exit(1);
		}
	}
}

static int
dirExists(const char *dir) {
	struct stat buf;
	return stat(dir, &buf) == 0 && S_ISDIR(buf.st_mode);
}

static void
touchDir(const char *dir) {
	pid_t pid;
	int e, status;

	pid = fork();
	if (pid == 0) {
		close(terminationPipe[0]);
		close(terminationPipe[1]);
		if (chdir(dir) == -1) {
			e = errno;
			fprintf(stderr, ERROR_PREFIX
				": cannot change working directory to %s: %s (errno %d)\n",
				dir, strerror(e), e);
			_exit(1);
		}
		execlp("/bin/sh", "/bin/sh", "-c",
			"find \"$1\" | xargs touch", "/bin/sh", ".",
			(const char * const) 0);
		e = errno;
		fprintf(stderr, ERROR_PREFIX ": cannot execute /bin/sh: %s (errno %d)\n",
			strerror(e), e);
		_exit(1);
	} else if (pid == -1) {
		e = errno;
		fprintf(stderr, ERROR_PREFIX ": cannot fork: %s (errno %d)\n",
			strerror(e), e);
		exit(1);
	} else {
		if (waitpid(pid, &status, 0) == -1) {
			if (errno != ESRCH && errno != EPERM) {
				fprintf(stderr, ERROR_PREFIX
					": unable to wait for shell command 'find %s | xargs touch'\n",
					dir);
				exit(1);
			}
		} else if (WEXITSTATUS(status) != 0) {
			fprintf(stderr, ERROR_PREFIX
				": shell command 'find %s | xargs touch' failed with exit status %d\n",
				dir, WEXITSTATUS(status));
			exit(1);
		}
	}
}

static int
doSleep(int sec) {
	fd_set readfds;
	struct timeval timeout;
	int ret, e;

	FD_ZERO(&readfds);
	FD_SET(terminationPipe[0], &readfds);
	timeout.tv_sec = sec;
	timeout.tv_usec = 0;
	do {
		ret = select(terminationPipe[0] + 1, &readfds, NULL, NULL, &timeout);
	} while (ret == -1 && errno == EINTR);
	if (ret == -1) {
		e = errno;
		fprintf(stderr, ERROR_PREFIX ": cannot select(): %s (errno %d)\n",
			strerror(e), e);
		exit(1);
		return -1; /* Never reached */
	} else {
		return ret == 0;
	}
}

static void
maybeDeletePidFile() {
	if (pidFile != NULL) {
		unlink(pidFile);
	}
}

static void
performCleanup(const char *dir) {
	pid_t pid;
	int e, status;

	pid = fork();
	if (pid == 0) {
		close(terminationPipe[0]);
		close(terminationPipe[1]);
		execlp("/bin/sh", "/bin/sh", "-c",
			"rm -rf \"$1\"", "/bin/sh", dir,
			(const char * const) 0);
		e = errno;
		fprintf(stderr, ERROR_PREFIX ": cannot execute /bin/sh: %s (errno %d)\n",
			strerror(e), e);
		_exit(1);
	} else if (pid == -1) {
		e = errno;
		fprintf(stderr, ERROR_PREFIX ": cannot fork: %s (errno %d)\n",
			strerror(e), e);
		exit(1);
	} else {
		if (waitpid(pid, &status, 0) == -1) {
			if (errno != ESRCH && errno != EPERM) {
				fprintf(stderr, ERROR_PREFIX
					": unable to wait for shell command 'rm -rf %s'\n",
					dir);
				exit(1);
			}
		} else if (WEXITSTATUS(status) != 0) {
			fprintf(stderr, ERROR_PREFIX
				": shell command 'rm -rf %s' failed with exit status %d\n",
				dir, WEXITSTATUS(status));
			exit(1);
		}
	}
}

int
tempDirToucherMain(int argc, char *argv[]) {
	initialize(argc, argv, 2);
	installSignalHandlers();
	maybeDaemonize();
	maybeWritePidfile();

	DEBUG("TempDirToucher started");

	while (1) {
		if (dirExists(dir)) {
			DEBUG("Touching directory");
			touchDir(dir);
			if (!doSleep(sleepInterval)) {
				break;
			}
		} else {
			DEBUG("Directory no longer exists, exiting");
			break;
		}
	}

	maybeDeletePidFile();
	if (shouldCleanup) {
		DEBUG("Cleaning up directory");
		performCleanup(dir);
	}

	return 0;
}

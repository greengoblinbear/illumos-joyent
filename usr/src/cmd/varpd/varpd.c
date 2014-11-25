/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright (c) 2014 Joyent, Inc.  All rights reserved.
 */

/*
 * virtual arp daemon -- varpd
 *
 * The virtual arp daemon is the user land counterpart to overlay(9XXX). It's
 * purpose is to provide a means for looking up mappings between layer two hosts
 * and a corresponding encapsulation plugin.
 */

#include <libvarpd.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdlib.h>
#include <paths.h>
#include <limits.h>
#include <sys/corectl.h>
#include <signal.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread.h>

#define	VARPD_EXIT_REQUESTED	0
#define	VARPD_EXIT_FATAL	1
#define	VARPD_EXIT_USAGE	2

#define	VARPD_RUNDIR	"/var/run/varpd"
#define	VARPD_DEFAULT_DOOR	"/var/run/varpd/varpd.door"

static varpd_handle_t varpd_handle;
static const char *varpd_pname;
static volatile boolean_t varpd_exit = B_FALSE;

/*
 * Debug builds are automatically wired up for umem debugging.
 */
#ifdef	DEBUG
const char *
_umem_debug_init()
{
	return ("default,verbose");
}

const char *
_umem_logging_init(void)
{
	return ("fail,contents");
}
#endif	/* DEBUG */

static void
varpd_vwarn(FILE *out, const char *fmt, va_list ap)
{
	int error = errno;

	(void) fprintf(out, "%s: ", varpd_pname);
	(void) vfprintf(out, fmt, ap);

	if (fmt[strlen(fmt) - 1] != '\n')
		(void) fprintf(out, ": %s\n", strerror(error));
}

static void
varpd_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	varpd_vwarn(stderr, fmt, ap);
	va_end(ap);
}

static void
varpd_fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	varpd_vwarn(stderr, fmt, ap);
	va_end(ap);

	exit(VARPD_EXIT_FATAL);
}

static void
varpd_info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	varpd_vwarn(stdout, fmt, ap);
	va_end(ap);
	(void) fflush(stdout);
}

static void
varpd_dfatal(int dfd, const char *fmt, ...)
{
	int status = VARPD_EXIT_FATAL;
	va_list ap;

	va_start(ap, fmt);
	varpd_vwarn(stdout, fmt, ap);
	va_end(ap);

	/* Take a single shot at this */
	(void) write(dfd, &status, sizeof (status));
	exit(status);
}

static int
varpd_plugin_walk_cb(varpd_handle_t vph, const char *name, void *unused)
{
	printf("loaded %s!\n", name);
	return (0);
}

static void
varpd_dir_setup(void)
{
	int fd;

	if (mkdir(VARPD_RUNDIR, 0700) != 0) {
		if (errno != EEXIST)
			varpd_fatal("failed to create %s", VARPD_RUNDIR);
	}

	fd = open(VARPD_RUNDIR, O_RDONLY);
	if (fd < 0)
		varpd_fatal("failed to open %s", VARPD_RUNDIR);
}

/*
 * Because varpd is generally run under SMF, we opt to keep its stdout and
 * stderr to be whatever our parent set them up to be.
 */
static void
varpd_fd_setup(void)
{
	int dupfd;

	closefrom(STDERR_FILENO + 1);
	dupfd = open(_PATH_DEVNULL, O_RDONLY);
	if (dupfd < 0)
		varpd_fatal("failed to open %s", _PATH_DEVNULL);
	if (dup2(dupfd, STDIN_FILENO) == -1)
		varpd_fatal("failed to dup out stdin");
}

/*
 * We borrow fmd's daemonization style. Basically, the parent waits for the
 * child to successfully set up a door and recover all of the old configurations
 * before we say that we're good to go.
 */
static int
varpd_daemonize(void)
{
	char path[PATH_MAX];
	struct rlimit rlim;
	sigset_t set, oset;
	int estatus, pfds[2];
	pid_t child;

	/*
	 * Set a per-process core path to be inside of /var/run/varpd. Make sure
	 * that we aren't limited in our dump size.
	 */
	(void) snprintf(path, sizeof (path),
	    "/var/run/varpd/core.%s.%%p", varpd_pname);
	(void) core_set_process_path(path, strlen(path) + 1, getpid());

	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;
	(void) setrlimit(RLIMIT_CORE, &rlim);

	/*
	 * Claim as many file descriptors as the system will let us.
	 */
	if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
		rlim.rlim_cur = rlim.rlim_max;
		(void) setrlimit(RLIMIT_NOFILE, &rlim);
	}

	/*
	 * chdir /var/run/varpd
	 */
	if (chdir(VARPD_RUNDIR) != 0)
		varpd_fatal("failed to chdir to %s", VARPD_RUNDIR);

	/*
	 * XXX Drop privileges here some day. For the moment, we'll drop a few
	 * that we know we shouldn't need.
	 */


	/*
	 * At this point block all signals going in so we don't have the parent
	 * mistakingly exit when the child is running, but never block SIGABRT.
	 */
	if (sigfillset(&set) != 0)
		abort();
	if (sigdelset(&set, SIGABRT) != 0)
		abort();
	if (sigprocmask(SIG_BLOCK, &set, &oset) != 0)
		abort();

	/*
	 * Do the fork+setsid dance.
	 */
	if (pipe(pfds) != 0)
		varpd_fatal("failed to create pipe for daemonizing");

	if ((child = fork()) == -1)
		varpd_fatal("failed to fork for daemonizing");

	if (child != 0) {
		/* We'll be exiting shortly, so allow for silent failure */
		(void) close(pfds[1]);
		if (read(pfds[0], &estatus, sizeof (estatus)) ==
		    sizeof (estatus))
			_exit(estatus);

		if (waitpid(child, &estatus, 0) == child && WIFEXITED(estatus))
			_exit(WEXITSTATUS(estatus));

		_exit(VARPD_EXIT_FATAL);
	}

	if (close(pfds[0]) != 0)
		abort();
	if (setsid() == -1)
		abort();
	if (sigprocmask(SIG_SETMASK, &oset, NULL) != 0)
		abort();
	(void) umask(0022);

	return (pfds[1]);
}

static int
varpd_setup_lookup_threads(void)
{
	int ret;
	long i, ncpus = sysconf(_SC_NPROCESSORS_ONLN) * 2 + 1;

	if (ncpus <= 0)
		abort();
	for (i = 0; i < ncpus; i++) {
		thread_t thr;

		ret = thr_create(NULL, 0,
		    (void *(*)(void *))libvarpd_overlay_lookup_run,
		    (void *)varpd_handle, THR_DETACHED | THR_DAEMON, &thr);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static void
varpd_cleanup(void)
{
	varpd_exit = B_TRUE;
}

/*
 * There are a bunch of things we need to do to be a proper daemon here.
 *
 *   o Ensure that /var/run/varpd exists or create it
 *   o make stdin /dev/null (stdout?)
 *   o Ensure any other fds that we somehow inherited are closed, eg.
 *     closefrom()
 *   o Properly daemonize
 *   o Mask all signals except sigabrt before creating our first door -- all
 *     other doors will inherit from that.
 *   o Have the main thread sigsuspend looking for most things that are
 *     actionable...
 */
int
main(int argc, char *argv[])
{
	int err, c, dfd, i;
	const char *doorpath = VARPD_DEFAULT_DOOR;
	sigset_t set;
	struct sigaction act;
	int nincpath = 0, nextincpath = 0;
	char **incpath = NULL;

	varpd_pname = basename(argv[0]);

	/*
	 * We want to clean up our file descriptors before we do anything else
	 * as we can't assume that libvarpd won't open file descriptors, etc.
	 */
	varpd_fd_setup();

	if ((err = libvarpd_create(&varpd_handle)) != 0) {
		varpd_fatal("failed to open a libvarpd handle");
		return (1);
	}

	while ((c = getopt(argc, argv, ":i:d:")) != -1) {
		switch (c) {
		case 'i':
			if (nextincpath == nincpath) {
				if (nincpath == 0)
					nincpath = 16;
				else
					nincpath *= 2;
				incpath = realloc(incpath, sizeof (char *) *
				    nincpath);
				if (incpath == NULL) {
					(void) fprintf(stderr, "failed to "
					    "allocate memory for the %dth "
					    "-I option: %s\n", nextincpath + 1,
					    strerror(errno));
				}

			}
			incpath[nextincpath] = optarg;
			nextincpath++;
			break;
		case 'd':
			doorpath = optarg;
			break;
		default:
			(void) fprintf(stderr, "unknown option: %c\n", c);
			return (1);
		}
	}

	varpd_dir_setup();

	libvarpd_plugin_walk(varpd_handle, varpd_plugin_walk_cb, NULL);

	dfd = varpd_daemonize();

	/*
	 * Now that we're in the child, go ahead and load all of our plug-ins.
	 * We do this, in part, because these plug-ins may need threads of their
	 * own and fork won't preserve those and we'd rather the plug-ins don't
	 * have to learn about fork-handlers.
	 */
	for (i = 0; i < nextincpath; i++) {
		err = libvarpd_plugin_load(varpd_handle, incpath[i]);
		if (err != 0) {
			(void) fprintf(stderr, "failed to load from %s: %s\n",
			    optarg, strerror(err));
			return (1);
		}
	}

	if ((err = libvarpd_persist_enable(varpd_handle, VARPD_RUNDIR)) != 0)
		varpd_dfatal(dfd, "failed to enable varpd persistence: %s\n",
		    strerror(err));

	if ((err = libvarpd_persist_restore(varpd_handle)) != 0)
		varpd_dfatal(dfd, "failed to enable varpd persistence: %s\n",
		    strerror(err));

	/*
	 * The ur-door thread will inherit from this signal mask. So set it to
	 * what we want before doing anything else. In addition, so will our
	 * threads that handle varpd lookups.
	 */
	if (sigfillset(&set) != 0)
		varpd_dfatal(dfd, "failed to fill a signal set...");

	if (sigdelset(&set, SIGABRT) != 0)
		varpd_dfatal(dfd, "failed to unmask SIGABRT");

	if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
		varpd_dfatal(dfd, "failed to set our door signal mask");

	if ((err = varpd_setup_lookup_threads()) != 0)
		varpd_dfatal(dfd, "failed to create lookup threads: %s\n",
		    strerror(err));

	if ((err = libvarpd_door_server_create(varpd_handle, doorpath)) != 0)
		varpd_dfatal(dfd, "failed to create door server at %s: %s\n",
		    doorpath, strerror(err));

	/*
	 * At this point, finish up signal intialization and finally go ahead,
	 * notify the parent that we're okay, and enter the sigsuspend loop.
	 */
	bzero(&act, sizeof (struct sigaction));
	act.sa_handler = varpd_cleanup;
	if (sigfillset(&act.sa_mask) != 0)
		varpd_dfatal(dfd, "failed to fill sigaction mask");
	act.sa_flags = 0;
	if (sigaction(SIGHUP, &act, NULL) != 0)
		varpd_dfatal(dfd, "failed to register HUP handler");
	if (sigaction(SIGQUIT, &act, NULL) != 0)
		varpd_dfatal(dfd, "failed to register QUIT handler");
	if (sigaction(SIGINT, &act, NULL) != 0)
		varpd_dfatal(dfd, "failed to register INT handler");
	if (sigaction(SIGTERM, &act, NULL) != 0)
		varpd_dfatal(dfd, "failed to register TERM handler");

	err = 0;
	(void) write(dfd, &err, sizeof (err));
	(void) close(dfd);

	for (;;) {
		if (sigsuspend(&set) == -1)
			if (errno == EFAULT)
				abort();
		if (varpd_exit == B_TRUE)
			break;
	}

	libvarpd_door_server_destroy(varpd_handle);
	libvarpd_destroy(varpd_handle);

	return (VARPD_EXIT_REQUESTED);
}
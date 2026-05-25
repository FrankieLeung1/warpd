/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * © 2019 Raheman Vaiya (see: LICENSE).
 */

#include "warpd.h"

#ifndef _WIN32
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#endif

struct platform *platform = NULL;

void __attribute__((used)) releaseDevices()
{
	printf("releaseDevices() called");
	if (platform && platform->input_ungrab_keyboard)
	{
		platform->input_ungrab_keyboard();
	}
}

#ifndef _WIN32
/* fd for dedicated crash log file; written in addition to stderr */
static int crash_log_fd = -1;

static void crash_handler(int sig)
{
	void *array[128];
	size_t size;
	const char *signame;
	time_t t;
	char timebuf[64];

	switch (sig) {
		case SIGSEGV: signame = "SIGSEGV (Segmentation fault)"; break;
		case SIGABRT: signame = "SIGABRT (Abort)";              break;
		case SIGFPE:  signame = "SIGFPE (Floating point exception)"; break;
		case SIGILL:  signame = "SIGILL (Illegal instruction)"; break;
		case SIGBUS:  signame = "SIGBUS (Bus error)";           break;
		default:      signame = "unknown";                       break;
	}

	t = time(NULL);
	/* ctime_r includes a trailing newline */
	ctime_r(&t, timebuf);
	timebuf[sizeof timebuf - 1] = '\0';

	size = backtrace(array, 128);

	int fds[2] = {STDERR_FILENO, crash_log_fd};
	int nfds = (crash_log_fd >= 0) ? 2 : 1;

	for (int i = 0; i < nfds; i++) {
		int fd = fds[i];
		dprintf(fd, "\n===== warpd crash report =====\n");
		dprintf(fd, "Time:    %s", timebuf);
		dprintf(fd, "Version: %s\n", VERSION);
		dprintf(fd, "Signal:  %d (%s)\n", sig, signame);
		dprintf(fd, "Backtrace (%zu frames):\n", size);
		backtrace_symbols_fd(array, size, fd);
		dprintf(fd, "===== end of crash report =====\n\n");
	}

	exit(1);
}

static void setup_crash_handler(const char *crash_log_path)
{
	if (crash_log_path) {
		crash_log_fd = open(crash_log_path,
		                    O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (crash_log_fd < 0)
			fprintf(stderr,
			        "warpd: warning: could not open crash log %s: %s\n",
			        crash_log_path, strerror(errno));
	}

	signal(SIGSEGV, crash_handler);
	signal(SIGABRT, crash_handler);
	signal(SIGFPE,  crash_handler);
	signal(SIGILL,  crash_handler);
	signal(SIGBUS,  crash_handler);
}
#else
static void setup_crash_handler(const char *crash_log_path) { (void)crash_log_path; }
#endif

static const char *config_path;

uint64_t get_time_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_nsec / 1E3 + ts.tv_sec * 1E6;
}


const char *get_data_path(const char *file)
{
	static char path[PATH_MAX];

	if (getenv("XDG_DATA_DIR")) {
		sprintf(path, "%s/warpd", getenv("XDG_DATA_DIR"));
		mkdir(path, 0700);
	} else {
		sprintf(path, "%s/.local", getenv("HOME"));
		mkdir(path, 0700);
		strcat(path, "/share");
		mkdir(path, 0700);
		strcat(path, "/warpd");
		mkdir(path, 0700);
	}

	strcat(path, "/");
	strcat(path, file);

	return path;
}

const char *get_config_path(const char *file)
{
	static char path[PATH_MAX];

	if (getenv("XDG_CONFIG_HOME")) {
		sprintf(path, "%s/warpd", getenv("XDG_CONFIG_HOME"));
		mkdir(path, 0700);
	} else {
		sprintf(path, "%s/.config", getenv("HOME"));
		mkdir(path, 0700);
		strcat(path, "/warpd");
		mkdir(path, 0700);
	}

	strcat(path, "/");
	strcat(path, file);

	return path;
}


static void lock()
{
	int fd;
	char path[64];

	sprintf(path, "/tmp/warpd_%d.lock", getuid());
	fd = open(path, O_RDONLY|O_CREAT, 0600);

	if (fd < 0) {
		perror("flock open");
		exit(-1);
	}

	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		fprintf(
		    stderr,
		    "ERROR: Another instance of warpd is already running.\n");
		exit(-1);
	}
}

static void daemonize(const char *log_path)
{
	if (fork())
		exit(0);
	if (fork())
		exit(0);

	int null_fd = open("/dev/null", O_WRONLY);
	if (null_fd < 0) {
		perror("open /dev/null");
		exit(-1);
	}

	int log_fd = -1;
	if (log_path)
		log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);

	close(1);
	close(2);

	/* stdout → log file (or /dev/null) */
	dup2(log_fd >= 0 ? log_fd : null_fd, 1);
	/* stderr → same log file so crash handler output is captured */
	dup2(log_fd >= 0 ? log_fd : null_fd, 2);

	if (log_fd >= 0)
		close(log_fd);
	close(null_fd);
}

static void print_usage()
{
	const char *usage =
		"warpd: [options]\n\n"
		"  -f, --foreground            Run warpd in the foreground (useful for debugging).\n"
		"  -h, --help                  Print this help message.\n"
		"  -v, --version               Print the version and exit.\n"
		"  -c, --config <config file>  Use the supplied config file.\n"
		"  -l, --list-keys             Print all valid keys.\n"
		"  --list-options              Print all available config options.\n"

		"  --hint                      Start warpd in hint mode and exit after the end of the session.\n"
		"  --hint2                     Start warpd in two pass hint mode and exit after the end of the session.\n"
		"  --normal                    Start warpd in normal mode and exit after the end of the session.\n"
		"  --grid                      Start warpd in hint grid and exit after the end of the session.\n"
		"  --screen                    Start warpd in screen selection mode and exit after the end of the session.\n"
		"  --oneshot                   When paired with one of the mode flags, exit warpd as soon as the mode is complete (i.e don't drop into normal mode). Principally useful for scripting.\n"
		"  --move '<x> <y>'            Move the pointer to the specified coordinates.\n"
		"  --click <button>            Send a mouse click corresponding to the supplied button and exit. May be paired with --move.\n"
		"  -q, --query                 Consumes a list of hints from stdin and presents a one off hint selection.\n"
		"  --record                    When used with --click, records the event in warpd's hint history.\n\n"
		;

	printf("%s", usage);
}

static void print_version()
{
	printf("warpd " VERSION"\n");
}


int dragging = 0;
static int oneshot_flag = 0;
static int click_flag = 0;
static int x_flag = -1;
static int y_flag = -1;
static int record_flag = 0;
static int mode = 0;

/* Platform entry points. */
int oneshot_main(struct platform *_platform)
{
	int ret = 0;
	screen_t scr;
	platform = _platform;

	parse_config(config_path);
	init_mouse();
	init_hints();

	platform->mouse_get_position(&scr, NULL, NULL);
	if (x_flag == -1 && y_flag == -1) {
		if (dragging)
			platform->mouse_down(config_get_int("drag_button"));

		ret = mode_loop(mode, oneshot_flag, record_flag);

		if (dragging)
			platform->mouse_up(config_get_int("drag_button"));

	} else {
		platform->mouse_move(scr, x_flag, y_flag);
	}

	if (click_flag)
		platform->mouse_click(click_flag);

	return ret;
}


int daemon_main(struct platform *_platform)
{
	platform = _platform;

	parse_config(config_path);
	init_mouse();
	init_hints();

	daemon_loop(config_path);

	return 0;
}

int print_keys_main(struct platform *platform)
{
	size_t i;
	for (i = 1; i < 256; i++) {
		const char *name = platform->input_lookup_name(i, 0);

		if (name && name[0])
			printf("%s\n", name);

		name = platform->input_lookup_name(i, 1);
		if (name && name[0])
			printf("%s\n", name);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int c;
	int foreground = 0;

	config_path = get_config_path("config");

	struct option opts[] = {
		{"version", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{"query", no_argument, NULL, 'q'},
		{"list-keys", no_argument, NULL, 'l'},
		{"foreground", no_argument, NULL, 'f'},
		{"config", required_argument, NULL, 'c'},

		{"hint", no_argument, NULL, 257},
		{"grid", no_argument, NULL, 258},
		{"normal", no_argument, NULL, 259},
		{"hint2", no_argument, NULL, 261},
		{"history", no_argument, NULL, 262},
		{"list-options", no_argument, NULL, 260},
		{"oneshot", no_argument, NULL, 263},
		{"click", required_argument, NULL, 264},
		{"move", required_argument, NULL, 265},
		{"record", no_argument, NULL, 266},
		{"drag", no_argument, NULL, 267},
		{"screen", no_argument, NULL, 268},
		{0}
	};

	while ((c = getopt_long(argc, argv, "qrhfvlc:", opts, NULL)) != -1) {
		switch (c) {
			case 'v':
				print_version();
				return 0;
			case 'h':
				print_usage();
				return 0;
			case 'l':
				platform_run(print_keys_main);
				return 0;
			case 'c':
				config_path = optarg;
				break;
			case 'f':
				foreground = 1;
				break;
			case 'q':
				mode = MODE_HINTSPEC;
				oneshot_flag = 1;
				break;
			case 257:
				mode = MODE_HINT;
				break;
			case 258:
				mode = MODE_GRID;
				break;
			case 259:
				mode = MODE_NORMAL;
				break;
			case 261:
				mode = MODE_HINT2;
				break;
			case 262:
				mode = MODE_HISTORY;
				break;
			case 268:
				mode = MODE_SCREEN_SELECTION;
				break;
			case 263:
				if (!mode)
					mode = MODE_NORMAL;

				oneshot_flag = 1;
				break;
			case 264:
				click_flag = atoi(optarg);
				oneshot_flag = 1;
				break;
			case 265:
				sscanf(optarg, "%d %d", &x_flag, &y_flag);
				oneshot_flag = 1;
				break;
			case 266:
				record_flag = 1;
				break;
			case 267:
				dragging = 1;
				break;
			case 260:
				config_print_options();
				return 0;
			case '?':
				return -1;
		}
	}

	if (mode || oneshot_flag) {
		/* oneshot/query modes: crash log in data dir, stderr stays visible */
		setup_crash_handler(get_data_path("crash.log"));
		platform_run(oneshot_main);
	} else {
		lock();

		/*
		 * Compute log paths before daemonizing (get_data_path uses a
		 * static buffer so copy the results out first).
		 */
		static char log_path[PATH_MAX];
		static char crash_log_path[PATH_MAX];
		strncpy(log_path,       get_data_path("warpd.log"),  PATH_MAX - 1);
		strncpy(crash_log_path, get_data_path("crash.log"), PATH_MAX - 1);

		/*
		 * Set up the crash handler before daemonizing so it works for
		 * both foreground (-f) and daemon modes.  The crash log gives
		 * us a persistent record even when stderr goes to /dev/null.
		 */
		setup_crash_handler(crash_log_path);

		if (!foreground)
			daemonize(log_path);

		setvbuf(stdout, NULL, _IOLBF, 0);
		printf("Starting warpd " VERSION "\n");
		printf("Logging to %s, crash log: %s\n", log_path, crash_log_path);

		platform_run(daemon_main);
	}
}

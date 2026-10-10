/*
HOST_MAIN.C

Entry point of the Knulli port: an ordinary aarch64 Linux (glibc) program
that runs the Android port's guest image (port/android/README.md) on
handhelds whose GPU driver offers only OpenGL ES through the framebuffer,
as the Allwinner H700 devices (Anbernic RG35XX H) under Knulli do.

The host is the Android host library (port/android/host) with three files
replaced: this one (no JNI, no APK: the image and the game data are files
next to the executable), host_sdl2.c (SDL3's calls answered by the
firmware's SDL2, which drives the Mali framebuffer) and the NDK's log
functions (the standard error stream). Refer to port/knulli/README.md.

Paths, each overridable from the environment:
- HALO_GUEST_IMAGE: the guest image (default: halo_guest.elf next to the
  executable);
- HALO_DATA_ROOT: the folder that holds maps/ and config.toml (default: the
  executable's folder);
- HALO_SAVE_ROOT: the saved games (default: save/ in the data folder);
- HALO_DISPLAY_WIDTH: the columns of the 480-line picture (default: from
  the framebuffer's mode; 640 on a 640x480 screen).
*/

#include "host.h"
#include "host_knulli.h"
#include "tomlc17.h"

#include <android/log.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void host_install_signal_handlers(void);

/* ---------- logging and termination */

static const char *priority_name(int priority)
{
	switch (priority)
	{
	case ANDROID_LOG_WARN: return "W";
	case ANDROID_LOG_ERROR: return "E";
	case ANDROID_LOG_FATAL: return "F";
	default: return "I";
	}
}

int __android_log_write(int priority, const char *tag, const char *text)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	fprintf(stderr, "%5ld.%03ld %s %s: %s\n", (long)now.tv_sec, now.tv_nsec / 1000000L, priority_name(priority),
		tag, text);
	return 1;
}

int __android_log_vprint(int priority, const char *tag, const char *format, va_list arguments)
{
	char text[2048];

	vsnprintf(text, sizeof(text), format, arguments);
	return __android_log_write(priority, tag, text);
}

int __android_log_print(int priority, const char *tag, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = __android_log_vprint(priority, tag, format, arguments);
	va_end(arguments);
	return result;
}

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	__android_log_vprint(priority, "halo", format, arguments);
	va_end(arguments);
}

void host_log(int priority, const char *text)
{
	__android_log_write(priority, "halo", text);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	__android_log_write(ANDROID_LOG_FATAL, "halo", message);
	_exit(1);
}

void host_abort(const char *reason)
{
	__android_log_print(ANDROID_LOG_FATAL, "halo", "guest abort: %s", reason);
	abort();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	host_profile_write();
	fflush(stderr);
	_exit(code);
}

int host_errno(void)
{
	return errno;
}

/* ---------- paths */

static char executable_root[PATH_MAX];
static char data_root[PATH_MAX];
static char save_root[PATH_MAX];

void host_android_path(int which, char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", which ? save_root : data_root);
}

static void find_executable_root(void)
{
	ssize_t length = readlink("/proc/self/exe", executable_root, sizeof(executable_root) - 1);
	char *slash;

	if (length <= 0)
	{
		strcpy(executable_root, ".");
		return;
	}
	executable_root[length] = 0;
	slash = strrchr(executable_root, '/');
	if (slash && slash != executable_root)
		*slash = 0;
}

static int directory_has_maps(const char *root)
{
	char path[PATH_MAX + 32];
	struct stat information;

	snprintf(path, sizeof(path), "%s/maps/ui.map", root);
	return stat(path, &information) == 0;
}

/* the columns of the 480-line picture for the framebuffer's shape: the
first mode of fb0, for example "U:640x480p-59" */
static int display_width(void)
{
	FILE *modes = fopen("/sys/class/graphics/fb0/modes", "r");
	int width = 0, height = 0, result = 640;

	if (modes)
	{
		char line[128];

		if (fgets(line, sizeof(line), modes) && sscanf(strchr(line, ':') ? strchr(line, ':') + 1 : line,
			"%dx%d", &width, &height) == 2 && width > 0 && height > 0)
		{
			int longer = width > height ? width : height;
			int shorter = width > height ? height : width;

			result = (480 * longer / shorter) & ~1;
		}
		fclose(modes);
	}
	return result;
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 64

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name);
	char *entry;
	int index;

	entry = malloc(length + strlen(value) + 2);
	sprintf(entry, "%s=%s", name, value);
	for (index = 0; index < environment->count; index++)
	{
		if (!strncmp(environment->entries[index], name, length) && environment->entries[index][length] == '=')
		{
			free(environment->entries[index]);
			environment->entries[index] = entry;
			return;
		}
	}
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
}

/* passes the host's HALO_ variables on to the guest: the settings the game
also takes from the environment (port/linux/src/port_config.c) */
static void environment_copy_halo(struct environment *environment)
{
	extern char **environ;
	char **entry;

	for (entry = environ; *entry; entry++)
	{
		const char *equals = strchr(*entry, '=');
		char name[128];

		if (strncmp(*entry, "HALO_", 5) || !equals || (size_t)(equals - *entry) >= sizeof(name))
			continue;
		memcpy(name, *entry, (size_t)(equals - *entry));
		name[equals - *entry] = 0;
		environment_set(environment, name, equals + 1);
	}
}

/* debug.sample_seconds from config.toml, as text for the sampler, or 0 */
static int config_sample_seconds(const char *path, char *text, size_t size)
{
	toml_result_t result = toml_parse_file_ex(path);
	int found = 0;

	if (!result.ok)
		return 0;
	{
		toml_datum_t seconds = toml_seek(result.toptab, "debug.sample_seconds");
		double value = seconds.type == TOML_FP64 ? seconds.u.fp64 :
			seconds.type == TOML_INT64 ? (double)seconds.u.int64 : 0.0;

		if (value > 0.0)
		{
			snprintf(text, size, "%g", value);
			found = 1;
		}
	}
	toml_free(result);
	return found;
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
	long offset;

	localtime_r(&now, &local);
	offset = -local.tm_gmtoff;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, PROT_READ | PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = (uint32_t)getpagesize();
	return (uint32_t)(uintptr_t)boot;
}

static void *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	void *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length);
		if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
		{
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	fclose(file);
	return data;
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static void *game_main(void *unused)
{
	struct environment environment = { { 0 }, 0 };
	const char *setting;
	char zone[64];
	char path[PATH_MAX + 32];
	char width[16];
	size_t image_size = 0;
	void *image;
	uint32_t boot;

	(void)unused;
	setting = getenv("HALO_DATA_ROOT");
	snprintf(data_root, sizeof(data_root), "%s", setting && *setting ? setting : executable_root);
	setting = getenv("HALO_SAVE_ROOT");
	if (setting && *setting)
		snprintf(save_root, sizeof(save_root), "%s", setting);
	else
		snprintf(save_root, sizeof(save_root), "%s/save", data_root);
	mkdir(save_root, 0755);
	if (!directory_has_maps(data_root))
		host_fatal("The Halo game data was not found: %s/maps/ui.map is missing. Put the maps folder of an Xbox "
			"disc image there (the launcher extracts it from a .iso in the same folder).", data_root);

	environment_copy_halo(&environment);
	environment_set(&environment, "HOME", save_root);
	environment_set(&environment, "HALO_DATA_ROOT", data_root);
	environment_set(&environment, "HALO_SAVE_ROOT", save_root);
	setting = getenv("HALO_DISPLAY_WIDTH");
	if (setting && *setting)
		snprintf(width, sizeof(width), "%s", setting);
	else
		snprintf(width, sizeof(width), "%d", display_width());
	environment_set(&environment, "HALO_DISPLAY_WIDTH", width);
	host_logf(HOST_LOG_INFO, "rendering %sx480", width);
	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);

	setting = getenv("HALO_GUEST_IMAGE");
	if (setting && *setting)
		snprintf(path, sizeof(path), "%s", setting);
	else
		snprintf(path, sizeof(path), "%s/halo_guest.elf", executable_root);
	image = read_file(path, &image_size);
	if (!image)
		host_fatal("cannot read the game image %s: %s", path, strerror(errno));
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game image %s", path);
	free(image);

	snprintf(path, sizeof(path), "%s/config.toml", data_root);
	{
		char seconds[32];

		if (config_sample_seconds(path, seconds, sizeof(seconds)))
			host_debug_start_sampler(seconds);
	}
	boot = make_boot(&environment);
	host_logf(HOST_LOG_INFO, "data %s, saves %s", data_root, save_root);
	host_profile_start(data_root);
	host_run_guest_main(boot);
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	setvbuf(stderr, NULL, _IOLBF, 0);
	find_executable_root();
	host_logf(HOST_LOG_INFO, "Halo for Knulli starting (%s)", executable_root);
	host_install_signal_handlers();
	if (host_native_thread_create(game_main, NULL, MAIN_STACK_SIZE) != 0)
		host_fatal("cannot start the game thread");
	/* the game ends the process itself (host_exit) */
	for (;;)
		pause();
}

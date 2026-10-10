/*
HOST_PROFILE.C

A sampling profiler for the Knulli port, to find where a frame's time goes
on the handheld, which has no perf. HALO_PROFILE_HZ=<rate> interrupts
every thread of the process at that rate (HALO_PROFILE_THREADS=game: the
game's thread only), after HALO_PROFILE_DELAY seconds (default 0), and
records its program counter, link register and frame chain (the guest's
frame records, and those of the host code that keeps them), up to
HALO_PROFILE_SAMPLES samples; HALO_PROFILE_FILE (default profile.txt in the
data folder) receives them when the game exits, after the process's
mappings and each thread's name and CPU time. port/knulli/profile.py turns
the file into a report.
*/

#include "host.h"

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PROFILE_SIGNAL SIGPROF
#define PROFILE_DEPTH 16
/* samples kept, unless HALO_PROFILE_SAMPLES says otherwise (152 bytes each) */
#define PROFILE_CAPACITY (256 * 1024)

struct profile_sample
{
	int32_t tid;
	int32_t depth;
	/* set last, once the sample is whole (one being written as the profile
	is saved is left out) */
	uint32_t whole;
	uint64_t pc;
	uint64_t lr;
	uint64_t frames[PROFILE_DEPTH];
};

static struct profile_sample *samples;
static uint32_t capacity = PROFILE_CAPACITY;
static volatile uint32_t sample_count;
static int profiling;
/* set once the delay is over */
static volatile int sampling;
/* HALO_PROFILE_THREADS=game: only the game's thread (the one that starts
the profiler) is interrupted, so that a high rate costs the others nothing
and fills the samples with its own */
static int game_only;
static pid_t game_thread;
static char profile_path[1024];
static unsigned int profile_delay;

static void profile_handler(int signal_number, siginfo_t *information, void *context)
{
	const ucontext_t *ucontext = context;
	const struct sigcontext *registers = (const struct sigcontext *)&ucontext->uc_mcontext;
	uint32_t index = __sync_fetch_and_add(&sample_count, 1);
	struct profile_sample *sample;
	uint64_t fp = registers->regs[29];
	int depth;

	(void)signal_number;
	(void)information;
	if (index >= capacity)
		return;
	sample = &samples[index];
	sample->tid = game_only ? (int32_t)game_thread : (int32_t)syscall(SYS_gettid);
	sample->pc = registers->pc;
	sample->lr = registers->regs[30];
	/* frame records on the stacks the host made for guest threads (below
	4 GB); other stacks are not followed. The bounds come from TLS: a lock
	here would deadlock against a thread interrupted while holding it */
	for (depth = 0; depth < PROFILE_DEPTH && fp && !(fp & 7) && fp >= host_thread_stack_low &&
		host_thread_stack_high >= 16 && fp <= host_thread_stack_high - 16; depth++)
	{
		const uint64_t *frame = (const uint64_t *)fp;

		sample->frames[depth] = frame[1];
		if (frame[0] <= fp)
		{
			depth++;
			break;
		}
		fp = frame[0];
	}
	sample->depth = depth;
	__atomic_store_n(&sample->whole, 1, __ATOMIC_RELEASE);
}

static void *profile_thread(void *context)
{
	long interval_ns = (long)(uintptr_t)context;
	pid_t self = (pid_t)syscall(SYS_gettid);
	pid_t process = getpid();

	sleep(profile_delay);
	sampling = 1;
	for (;;)
	{
		struct timespec interval = { interval_ns / 1000000000L, interval_ns % 1000000000L };
		DIR *tasks;
		struct dirent *entry;

		nanosleep(&interval, NULL);
		if (sample_count >= capacity)
			continue;
		if (game_only)
		{
			syscall(SYS_tgkill, process, game_thread, PROFILE_SIGNAL);
			continue;
		}
		tasks = opendir("/proc/self/task");
		if (!tasks)
			continue;
		while ((entry = readdir(tasks)))
		{
			pid_t tid = (pid_t)atoi(entry->d_name);

			if (tid > 0 && tid != self)
				syscall(SYS_tgkill, process, tid, PROFILE_SIGNAL);
		}
		closedir(tasks);
	}
	return NULL;
}

void host_profile_start(const char *data_root)
{
	const char *rate = getenv("HALO_PROFILE_HZ");
	const char *file = getenv("HALO_PROFILE_FILE");
	const char *delay = getenv("HALO_PROFILE_DELAY");
	const char *kept = getenv("HALO_PROFILE_SAMPLES");
	const char *which = getenv("HALO_PROFILE_THREADS");
	struct sigaction action;
	pthread_t thread;
	long hertz;

	hertz = rate && *rate ? atol(rate) : 0;
	if (hertz <= 0)
		return;
	game_only = which && !strcmp(which, "game");
	/* (the game's thread starts the profiler, before the guest's main) */
	game_thread = (pid_t)syscall(SYS_gettid);
	if (kept && *kept)
		capacity = (uint32_t)atol(kept);
	samples = calloc(capacity, sizeof(*samples));
	if (!samples)
		return;
	if (file && *file)
		snprintf(profile_path, sizeof(profile_path), "%s", file);
	else
		snprintf(profile_path, sizeof(profile_path), "%s/profile.txt", data_root);
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = profile_handler;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(PROFILE_SIGNAL, &action, NULL);
	profile_delay = delay && *delay ? (unsigned int)atoi(delay) : 0;
	profiling = 1;
	if (pthread_create(&thread, NULL, profile_thread, (void *)(uintptr_t)(1000000000L / hertz)) == 0)
		pthread_detach(thread);
	host_logf(HOST_LOG_INFO, "profiling at %ld Hz into %s", hertz, profile_path);
}

int host_profile_sampling(void)
{
	return sampling;
}

/* a mark among the samples, as a sample of thread -1: the game thread's
frame that ended there and its time (port/knulli/profile.py and the scripts
that pick the long frames out read them) */
void host_profile_mark(uint32_t frame, uint32_t microseconds)
{
	uint32_t index;

	if (!sampling || sample_count >= capacity)
		return;
	index = __sync_fetch_and_add(&sample_count, 1);
	if (index >= capacity)
		return;
	samples[index].tid = -1;
	samples[index].depth = 0;
	samples[index].pc = frame;
	samples[index].lr = microseconds;
	__atomic_store_n(&samples[index].whole, 1, __ATOMIC_RELEASE);
}

static void copy_file(FILE *output, const char *path)
{
	FILE *input = fopen(path, "r");
	char line[1024];

	if (!input)
		return;
	while (fgets(line, sizeof(line), input))
		fputs(line, output);
	fclose(input);
}

void host_profile_write(void)
{
	FILE *output;
	DIR *tasks;
	struct dirent *entry;
	uint32_t count, index;

	if (!profiling)
		return;
	profiling = 0;
	signal(PROFILE_SIGNAL, SIG_IGN);
	/* (read once: handlers still running can raise it past the capacity) */
	count = __atomic_load_n(&sample_count, __ATOMIC_ACQUIRE);
	if (count > capacity)
		count = capacity;
	output = fopen(profile_path, "w");
	if (!output)
		return;
	fprintf(output, "# maps\n");
	copy_file(output, "/proc/self/maps");
	fprintf(output, "# threads: tid name utime stime (clock ticks)\n");
	tasks = opendir("/proc/self/task");
	while (tasks && (entry = readdir(tasks)))
	{
		char path[300], stat[1024], name[64] = "?";
		unsigned long user = 0, system = 0;
		FILE *file;

		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/proc/self/task/%s/stat", entry->d_name);
		file = fopen(path, "r");
		if (!file)
			continue;
		if (fgets(stat, sizeof(stat), file))
		{
			char *open = strchr(stat, '('), *close = strrchr(stat, ')');

			if (open && close && close > open)
			{
				size_t length = (size_t)(close - open - 1);

				if (length >= sizeof(name))
					length = sizeof(name) - 1;
				memcpy(name, open + 1, length);
				name[length] = 0;
				/* after ") ": state, then fields 4 to 13, utime (14), stime (15) */
				sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &user, &system);
			}
		}
		fclose(file);
		fprintf(output, "thread %s %s %lu %lu\n", entry->d_name, name, user, system);
	}
	if (tasks)
		closedir(tasks);
	fprintf(output, "# samples: tid pc lr frames...\n");
	for (index = 0; index < count; index++)
	{
		const struct profile_sample *sample = &samples[index];
		int depth;

		if (!__atomic_load_n(&sample->whole, __ATOMIC_ACQUIRE))
			continue;
		fprintf(output, "%d %llx %llx", sample->tid, (unsigned long long)sample->pc, (unsigned long long)sample->lr);
		for (depth = 0; depth < sample->depth; depth++)
			fprintf(output, " %llx", (unsigned long long)sample->frames[depth]);
		fputc('\n', output);
	}
	fclose(output);
	host_logf(HOST_LOG_INFO, "profile: %u samples in %s", count, profile_path);
}

/*
HOST_GL_TIMING.C

An OpenGL call timer for the Knulli port, to see which of the driver's
functions a frame's time goes to (the driver has no symbols to profile
with). With HALO_GL_TIMING=1 every GL function the guest imports is
wrapped: a stub made here enters host_gl_timing_common (host_gl_timing.S),
which counts the calls and the generic timer's ticks spent in them.
host_gl_timing_report logs each function's calls and time per frame.
*/

#include "host.h"
#include "host_knulli.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define TIMING_MAXIMUM 160

struct timing_slot
{
	void *function;
	uint64_t calls;
	uint64_t ticks;
	uint64_t longest;
	uint64_t longest_caller;
};

void host_gl_timing_common(void);

static struct timing_slot slots[TIMING_MAXIMUM];
static const char *names[TIMING_MAXIMUM];
static int slot_count;
static unsigned char *stubs;
static int enabled = -1;

/* the stub: ldr x16, slot; ldr x17, common; br x17 */
#define STUB_SIZE 32

int host_gl_timing_enabled(void)
{
	if (enabled < 0)
	{
		const char *setting = getenv("HALO_GL_TIMING");

		enabled = setting && *setting && *setting != '0';
	}
	return enabled;
}

void *host_gl_wrap(const char *name, void *function)
{
	uint32_t *code;

	if (!host_gl_timing_enabled() || !function || slot_count == TIMING_MAXIMUM)
		return function;
	if (!stubs)
	{
		stubs = mmap(NULL, TIMING_MAXIMUM * STUB_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (stubs == MAP_FAILED)
		{
			stubs = NULL;
			enabled = 0;
			return function;
		}
	}
	slots[slot_count].function = function;
	names[slot_count] = strdup(name);
	code = (uint32_t *)(stubs + slot_count * STUB_SIZE);
	code[0] = 0x58000090; /* ldr x16, #16 */
	code[1] = 0x580000b1; /* ldr x17, #20 (from here: the word at +24) */
	code[2] = 0xd61f0220; /* br x17 */
	code[3] = 0xd503201f; /* nop */
	*(uint64_t *)&code[4] = (uint64_t)(uintptr_t)&slots[slot_count];
	*(uint64_t *)&code[6] = (uint64_t)(uintptr_t)host_gl_timing_common;
	__builtin___clear_cache((char *)code, (char *)code + STUB_SIZE);
	slot_count++;
	return code;
}

static int by_ticks(const void *a, const void *b)
{
	const struct timing_slot *first = &slots[*(const int *)a], *second = &slots[*(const int *)b];

	return first->ticks < second->ticks ? 1 : first->ticks > second->ticks ? -1 : 0;
}

void host_gl_timing_report(uint32_t frames)
{
	int order[TIMING_MAXIMUM], index, count = 0;
	uint64_t frequency, total = 0;

	if (!host_gl_timing_enabled() || !frames)
		return;
	frequency = host_tick_frequency();
	for (index = 0; index < slot_count; index++)
	{
		if (slots[index].calls)
			order[count++] = index;
		total += slots[index].ticks;
	}
	qsort(order, (size_t)count, sizeof(order[0]), by_ticks);
	host_logf(HOST_LOG_INFO, "gl: %.2f ms a frame in the driver's functions",
		(double)total * 1000.0 / (double)frequency / frames);
	for (index = 0; index < count && index < 16; index++)
	{
		struct timing_slot *slot = &slots[order[index]];

		host_logf(HOST_LOG_INFO, "gl:   %-28s %7.1f calls %7.3f ms %6.1f us each, longest %7.1f us from %llx",
			names[order[index]], (double)slot->calls / frames,
			(double)slot->ticks * 1000.0 / (double)frequency / frames,
			(double)slot->ticks * 1000000.0 / (double)frequency / (double)slot->calls,
			(double)slot->longest * 1000000.0 / (double)frequency, (unsigned long long)slot->longest_caller);
	}
	glthread_timing_report(frames);
	for (index = 0; index < slot_count; index++)
	{
		slots[index].calls = 0;
		slots[index].ticks = 0;
		slots[index].longest = 0;
	}
}

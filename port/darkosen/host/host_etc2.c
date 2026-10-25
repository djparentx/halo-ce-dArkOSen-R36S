/*
HOST_ETC2.C

The guest's DXT textures as ETC2 on Mali, which has no S3TC. Without S3TC
xbox_textures.c decodes every DXT texture on the CPU into 16-bit texels
(DXT1, 2 bytes a pixel instead of 0.5) or 32-bit ones (DXT3 and DXT5, 4
bytes a pixel instead of 1). Here S3TC is reported as supported
(host_gl_has_extension), so the guest uploads its DXT levels with
glCompressedTexImage2D; each is decoded (bcdec), encoded again as ETC2
(etcpak) and uploaded:

- DXT3 and DXT5 as ETC2 RGBA8 (1 byte a pixel);
- DXT1 as ETC2 RGB (0.5 bytes a pixel), or as ETC2 RGBA8 when the
  texture's first level has transparent texels (DXT1's three-colour
  blocks: Halo's cut-outs). The first level decides for the levels after
  it, since a texture's levels must all have one format; a cube map's
  DXT1 is always RGBA8, so its six faces agree;
- 3D textures (ES has no ETC2 for them) decoded to RGBA8.

Encoded levels are kept in cache/etc2.pak in the game's folder (the
folder the launcher starts the game in): a record for each level, found
by a hash of its DXT data, so a later run uploads them with no encoding.

Two threads can upload: the guest's texture worker on its own context,
and the GL thread replaying the render thread's uploads. One lock covers
the conversions, the scratch buffers and the cache. A large level is
encoded on four cores: the calling thread and three workers each take a
quarter of its block rows.

HALO_ETC2=0 leaves S3TC unsupported (the guest decodes, as before).
HALO_ETC2_CACHE=0 converts without the cache; HALO_ETC2_CACHE_MB caps
its size (default 1024).
*/

#include "host.h"
#include "host_etc2.h"

#include <GLES3/gl3.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BCDECDEF static inline
#define BCDEC_IMPLEMENTATION
#include "../third_party/bcdec.h"

#define DXT1_RGB 0x83f0
#define DXT1_RGBA 0x83f1
#define DXT3 0x83f2
#define DXT5 0x83f3
#define S3TC_EXTENSION "GL_EXT_texture_compression_s3tc"

#define WORKERS 3
#define SPLIT_MIN_ROWS 16
#define BUFFER_KEEP (4u << 20)
#define LOG_EVERY 256

#define CACHE_MAGIC "HALOETC2"
/* (a change to the encoding, or to the record, makes the cache start afresh) */
#define CACHE_VERSION 1u
#define CACHE_CHECK 0x45544332u

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* decoded texels (BGRA, whole blocks) and the ETC2 blocks */
static uint32_t *pixels;
static size_t pixels_capacity;
static uint64_t *blocks;
static size_t blocks_capacity;

/* whether the DXT1 texture whose levels this thread is uploading is RGBA8 */
static __thread int texture_rgba;

static struct
{
	unsigned textures, levels, cached, failed, decoded_3d;
	uint64_t bytes, nanoseconds;
} stats;

static char cache_folder[4096];

/* ---------- settings */

static int setting_off(const char *name)
{
	const char *value = getenv(name);

	return value && !strcmp(value, "0");
}

static int etc2_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = !setting_off("HALO_ETC2");
	return enabled;
}

/* the cache's folder, from the folder the game starts in (before the guest
can change it) */
__attribute__((constructor)) static void etc2_folder(void)
{
	const char *folder = getenv("HALO_ETC2_CACHE_DIR");

	if (folder && *folder)
		snprintf(cache_folder, sizeof(cache_folder), "%s", folder);
	else if (getcwd(cache_folder, sizeof(cache_folder) - sizeof("/cache")))
		strcat(cache_folder, "/cache");
	else
		cache_folder[0] = 0;
}

static uint64_t now_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static int grow(void **buffer, size_t *capacity, size_t need)
{
	void *grown;

	if (*capacity >= need)
		return 1;
	grown = realloc(*buffer, need);
	if (!grown)
		return 0;
	*buffer = grown;
	*capacity = need;
	return 1;
}

static int dxt_kind(GLenum format)
{
	switch (format)
	{
	case DXT1_RGB:
	case DXT1_RGBA:
		return 1;
	case DXT3:
		return 3;
	case DXT5:
		return 5;
	default:
		return 0;
	}
}

/* ---------- the encoder's workers */

struct job
{
	const uint32_t *source;
	uint64_t *destination;
	uint32_t blocks;
	size_t width;
	int rgba;
};

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_go = PTHREAD_COND_INITIALIZER;
static pthread_cond_t pool_done = PTHREAD_COND_INITIALIZER;
static struct job jobs[WORKERS];
static unsigned pool_generation, pool_left;
static int pool_ready;

static void job_run(const struct job *job)
{
	if (!job->blocks)
		return;
	if (job->rgba)
		host_etc2_encode_rgba(job->source, job->destination, job->blocks, job->width);
	else
		host_etc2_encode_rgb(job->source, job->destination, job->blocks, job->width);
}

static void *pool_worker(void *argument)
{
	int index = (int)(intptr_t)argument;
	unsigned seen = 0;

	for (;;)
	{
		struct job job;

		pthread_mutex_lock(&pool_lock);
		while (pool_generation == seen)
			pthread_cond_wait(&pool_go, &pool_lock);
		seen = pool_generation;
		job = jobs[index];
		pthread_mutex_unlock(&pool_lock);
		job_run(&job);
		pthread_mutex_lock(&pool_lock);
		if (--pool_left == 0)
			pthread_cond_signal(&pool_done);
		pthread_mutex_unlock(&pool_lock);
	}
	return NULL;
}

static void pool_start(void)
{
	pthread_attr_t attributes;
	int index, started = 0;

	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	for (index = 0; index < WORKERS; index++)
	{
		pthread_t thread;

		if (pthread_create(&thread, &attributes, pool_worker, (void *)(intptr_t)index) == 0)
		{
			pthread_setname_np(thread, "halo-etc2");
			started++;
		}
	}
	pthread_attr_destroy(&attributes);
	/* (all or none: each worker has its own job) */
	pool_ready = started == WORKERS;
}

/* pixels (blocks_x * 4 by blocks_y * 4) to blocks */
static void encode(int rgba, size_t blocks_x, size_t blocks_y)
{
	size_t width = blocks_x * 4, words = rgba ? 2 : 1, rows, row = 0;
	struct job last;
	int index;

	if (!pool_ready || blocks_y < SPLIT_MIN_ROWS)
	{
		struct job all = { pixels, blocks, (uint32_t)(blocks_x * blocks_y), width, rgba };

		job_run(&all);
		return;
	}
	rows = blocks_y / (WORKERS + 1);
	pthread_mutex_lock(&pool_lock);
	for (index = 0; index < WORKERS; index++, row += rows)
	{
		jobs[index].source = pixels + row * 4 * width;
		jobs[index].destination = blocks + row * blocks_x * words;
		jobs[index].blocks = (uint32_t)(rows * blocks_x);
		jobs[index].width = width;
		jobs[index].rgba = rgba;
	}
	pool_left = WORKERS;
	pool_generation++;
	pthread_cond_broadcast(&pool_go);
	pthread_mutex_unlock(&pool_lock);
	/* (the last quarter and the rows left over, here) */
	last.source = pixels + row * 4 * width;
	last.destination = blocks + row * blocks_x * words;
	last.blocks = (uint32_t)((blocks_y - row) * blocks_x);
	last.width = width;
	last.rgba = rgba;
	job_run(&last);
	pthread_mutex_lock(&pool_lock);
	while (pool_left)
		pthread_cond_wait(&pool_done, &pool_lock);
	pthread_mutex_unlock(&pool_lock);
}

/* ---------- DXT */

static void decode_block(int kind, const unsigned char *source, unsigned char *destination, int pitch)
{
	if (kind == 1)
		bcdec_bc1(source, destination, pitch);
	else if (kind == 3)
		bcdec_bc2(source, destination, pitch);
	else
		bcdec_bc3(source, destination, pitch);
}

/* whether a DXT1 level has a transparent texel inside it: a three-colour
block (c0 <= c1) that gives one its fourth index */
static int dxt1_has_alpha(const unsigned char *source, unsigned long width, unsigned long height)
{
	unsigned long blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4, bx, by, x, y;

	for (by = 0; by < blocks_y; by++)
	{
		for (bx = 0; bx < blocks_x; bx++, source += 8)
		{
			uint32_t bits;

			if ((source[0] | (source[1] << 8)) > (source[2] | (source[3] << 8)))
				continue;
			bits = (uint32_t)source[4] | ((uint32_t)source[5] << 8) | ((uint32_t)source[6] << 16) |
				((uint32_t)source[7] << 24);
			for (y = 0; y < 4 && by * 4 + y < height; y++)
			{
				for (x = 0; x < 4 && bx * 4 + x < width; x++)
				{
					if (((bits >> ((y * 4 + x) * 2)) & 3) == 3)
						return 1;
				}
			}
		}
	}
	return 0;
}

/* DXT1's transparent texels are black: given the average colour of the
block's others instead (alpha stays 0), so that ETC2 spends none of the
block's colour on texels that do not show */
static void transparent_fill(size_t blocks_x, size_t blocks_y)
{
	size_t width = blocks_x * 4, bx, by, x, y;

	for (by = 0; by < blocks_y; by++)
	{
		for (bx = 0; bx < blocks_x; bx++)
		{
			uint32_t *block = pixels + by * 4 * width + bx * 4, fill;
			unsigned sum[3] = { 0, 0, 0 }, shown = 0;

			for (y = 0; y < 4; y++)
			{
				for (x = 0; x < 4; x++)
				{
					uint32_t texel = block[y * width + x];

					if (texel >> 24)
					{
						sum[0] += texel & 0xff;
						sum[1] += (texel >> 8) & 0xff;
						sum[2] += (texel >> 16) & 0xff;
						shown++;
					}
				}
			}
			if (!shown || shown == 16)
				continue;
			fill = (sum[0] / shown) | ((sum[1] / shown) << 8) | ((sum[2] / shown) << 16);
			for (y = 0; y < 4; y++)
			{
				for (x = 0; x < 4; x++)
				{
					if (!(block[y * width + x] >> 24))
						block[y * width + x] = fill;
				}
			}
		}
	}
}

/* a DXT level decoded into pixels (whole blocks, BGRA for etcpak); DXT1 as
RGB has its alpha 1 */
static int decode_level(int kind, int rgba, unsigned long width, unsigned long height, const unsigned char *source)
{
	size_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4, count = blocks_x * blocks_y * 16, bx, by, index;
	size_t block_bytes = kind == 1 ? 8 : 16, pitch = blocks_x * 16;
	uint32_t opaque = kind == 1 && !rgba ? 0xff000000u : 0;

	if (!grow((void **)&pixels, &pixels_capacity, count * sizeof(uint32_t)))
		return 0;
	for (by = 0; by < blocks_y; by++)
	{
		for (bx = 0; bx < blocks_x; bx++, source += block_bytes)
			decode_block(kind, source, (unsigned char *)pixels + by * 4 * pitch + bx * 16, (int)pitch);
	}
	/* (bcdec writes R, G, B, A; etcpak reads B, G, R, A) */
	for (index = 0; index < count; index++)
	{
		uint32_t value = pixels[index];

		pixels[index] = (value & 0xff00ff00u) | ((value & 0xffu) << 16) | ((value >> 16) & 0xffu) | opaque;
	}
	if (kind == 1 && rgba)
		transparent_fill(blocks_x, blocks_y);
	return 1;
}

/* ---------- the cache

cache/etc2.pak: CACHE_MAGIC and CACHE_VERSION, then a record for each
level: its key (a hash of the DXT data, the format and the size), its
size, a check word, and the ETC2 blocks. A record cut short (the game
stopped while one was written) is dropped. */

struct record
{
	uint64_t key;
	uint32_t size;
	uint32_t check;
};

struct entry
{
	uint64_t key;
	uint64_t offset;
	uint32_t size;
	uint32_t used;
};

static int cache_file = -1;
static uint64_t cache_end, cache_limit;
static struct entry *entries;
static size_t entry_capacity, entry_count;

static uint64_t mix(uint64_t value)
{
	value ^= value >> 33;
	value *= 0xff51afd7ed558ccdull;
	value ^= value >> 33;
	value *= 0xc4ceb9fe1a85ec53ull;
	value ^= value >> 33;
	return value;
}

static uint64_t hash_bytes(const unsigned char *data, size_t size, uint64_t seed)
{
	uint64_t hash = mix(seed ^ (size * 0x9e3779b97f4a7c15ull));

	for (; size >= 8; data += 8, size -= 8)
	{
		uint64_t word;

		memcpy(&word, data, 8);
		hash ^= mix(word);
		hash = ((hash << 27) | (hash >> 37)) * 0x9e3779b97f4a7c15ull + 0x52dce729u;
	}
	for (; size; data++, size--)
		hash = (hash ^ *data) * 0x100000001b3ull;
	return mix(hash);
}

static uint32_t record_check(const struct record *record)
{
	return (uint32_t)(record->key >> 32) ^ (uint32_t)record->key ^ record->size ^ CACHE_CHECK;
}

static struct entry *entry_find(uint64_t key)
{
	size_t index;

	if (!entry_capacity)
		return NULL;
	for (index = (size_t)key & (entry_capacity - 1); entries[index].used; index = (index + 1) & (entry_capacity - 1))
	{
		if (entries[index].key == key)
			return &entries[index];
	}
	return NULL;
}

static int entry_add(uint64_t key, uint64_t offset, uint32_t size)
{
	struct entry *entry;
	size_t index;

	if ((entry_count + 1) * 10 > entry_capacity * 7)
	{
		size_t capacity = entry_capacity ? entry_capacity * 2 : 4096, old;
		struct entry *grown = calloc(capacity, sizeof(*grown)), *previous = entries;

		if (!grown)
			return 0;
		entries = grown;
		old = entry_capacity;
		entry_capacity = capacity;
		entry_count = 0;
		for (index = 0; index < old; index++)
		{
			if (previous[index].used)
				entry_add(previous[index].key, previous[index].offset, previous[index].size);
		}
		free(previous);
	}
	entry = entry_find(key);
	if (!entry)
	{
		for (index = (size_t)key & (entry_capacity - 1); entries[index].used; index = (index + 1) & (entry_capacity - 1))
			;
		entry = &entries[index];
		entry_count++;
	}
	entry->key = key;
	entry->offset = offset;
	entry->size = size;
	entry->used = 1;
	return 1;
}

static void cache_open(void)
{
	char path[sizeof(cache_folder) + 16];
	unsigned char header[16];
	struct stat status;
	const char *limit = getenv("HALO_ETC2_CACHE_MB");
	uint64_t offset;

	cache_limit = (uint64_t)(limit && atoi(limit) > 0 ? atoi(limit) : 1024) << 20;
	if (setting_off("HALO_ETC2_CACHE") || !cache_folder[0])
		return;
	if (mkdir(cache_folder, 0755) != 0 && errno != EEXIST)
	{
		host_logf(HOST_LOG_WARN, "textures: no ETC2 cache (%s: %s)", cache_folder, strerror(errno));
		return;
	}
	snprintf(path, sizeof(path), "%s/etc2.pak", cache_folder);
	cache_file = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (cache_file < 0 || fstat(cache_file, &status) != 0)
	{
		host_logf(HOST_LOG_WARN, "textures: no ETC2 cache (%s: %s)", path, strerror(errno));
		if (cache_file >= 0)
			close(cache_file);
		cache_file = -1;
		return;
	}
	memcpy(header, CACHE_MAGIC, 8);
	header[8] = (unsigned char)CACHE_VERSION;
	header[9] = header[10] = header[11] = 0;
	memset(header + 12, 0, 4);
	{
		unsigned char found[16];

		if (status.st_size < 16 || pread(cache_file, found, 16, 0) != 16 || memcmp(found, header, 16))
		{
			/* (a new cache, or one of another version) */
			if (ftruncate(cache_file, 0) != 0 || pwrite(cache_file, header, 16, 0) != 16)
			{
				close(cache_file);
				cache_file = -1;
				return;
			}
			status.st_size = 16;
		}
	}
	for (offset = 16; offset + sizeof(struct record) <= (uint64_t)status.st_size;)
	{
		struct record record;

		if (pread(cache_file, &record, sizeof(record), (off_t)offset) != (ssize_t)sizeof(record) ||
			record.check != record_check(&record) ||
			offset + sizeof(record) + record.size > (uint64_t)status.st_size ||
			!entry_add(record.key, offset + sizeof(record), record.size))
		{
			break;
		}
		offset += sizeof(record) + record.size;
	}
	if (offset != (uint64_t)status.st_size && ftruncate(cache_file, (off_t)offset) != 0)
		host_logf(HOST_LOG_WARN, "textures: ETC2 cache: %s", strerror(errno));
	cache_end = offset;
	host_logf(HOST_LOG_INFO, "textures: ETC2 cache %s, %zu levels, %.1f MB", path, entry_count,
		(double)cache_end / (1024.0 * 1024.0));
}

/* a cached level's blocks, read into blocks: whether it was there */
static int cache_read(uint64_t key, uint32_t size)
{
	struct entry *entry = cache_file >= 0 ? entry_find(key) : NULL;

	if (!entry || entry->size != size || !grow((void **)&blocks, &blocks_capacity, size))
		return 0;
	return pread(cache_file, blocks, size, (off_t)entry->offset) == (ssize_t)size;
}

static void cache_write(uint64_t key, uint32_t size)
{
	struct record record = { key, size, 0 };

	if (cache_file < 0 || cache_end + sizeof(record) + size > cache_limit)
		return;
	record.check = record_check(&record);
	if (pwrite(cache_file, &record, sizeof(record), (off_t)cache_end) != (ssize_t)sizeof(record) ||
		pwrite(cache_file, blocks, size, (off_t)(cache_end + sizeof(record))) != (ssize_t)size)
	{
		/* (the record dropped: the next start cuts it off) */
		host_logf(HOST_LOG_WARN, "textures: ETC2 cache not written: %s", strerror(errno));
		close(cache_file);
		cache_file = -1;
		return;
	}
	entry_add(key, cache_end + sizeof(record), size);
	cache_end += sizeof(record) + size;
}

/* ---------- uploads */

static void log_stats(void)
{
	host_logf(HOST_LOG_INFO,
		"textures: %u converted to ETC2 (%u levels, %u from the cache), %.1f MB uploaded, %.1f s converting, "
		"%u 3D decoded, %u failed",
		stats.textures, stats.levels, stats.cached, (double)stats.bytes / (1024.0 * 1024.0),
		(double)stats.nanoseconds / 1e9, stats.decoded_3d, stats.failed);
}

static void etc2_start(void)
{
	pool_start();
	cache_open();
	atexit(log_stats);
	host_logf(HOST_LOG_INFO, "textures: DXT to ETC2 on (%d threads)", pool_ready ? WORKERS + 1 : 1);
}

static void scratch_trim(void)
{
	if (pixels_capacity > BUFFER_KEEP || blocks_capacity > BUFFER_KEEP)
	{
		free(pixels);
		free(blocks);
		pixels = NULL;
		blocks = NULL;
		pixels_capacity = blocks_capacity = 0;
	}
}

/* one DXT level as ETC2: whether it was uploaded */
static int upload_2d(GLenum target, GLint level, int kind, GLsizei width, GLsizei height, GLsizei size,
	const unsigned char *data)
{
	size_t blocks_x = ((size_t)width + 3) / 4, blocks_y = ((size_t)height + 3) / 4, count = blocks_x * blocks_y;
	size_t in_size = count * (kind == 1 ? 8 : 16);
	int rgba;
	uint32_t out_size;
	uint64_t key;

	if ((size_t)size < in_size)
		return 0;
	if (kind != 1)
		rgba = 1;
	else
	{
		if (level == 0)
			texture_rgba = target != GL_TEXTURE_2D || dxt1_has_alpha(data, (unsigned long)width, (unsigned long)height);
		rgba = texture_rgba;
	}
	out_size = (uint32_t)(count * (rgba ? 16 : 8));
	key = hash_bytes(data, in_size, ((uint64_t)kind << 56) ^ ((uint64_t)rgba << 48) ^
		((uint64_t)(uint32_t)width << 24) ^ (uint64_t)(uint32_t)height);
	if (cache_read(key, out_size))
		stats.cached++;
	else
	{
		if (!decode_level(kind, rgba, (unsigned long)width, (unsigned long)height, data) ||
			!grow((void **)&blocks, &blocks_capacity, out_size))
		{
			return 0;
		}
		encode(rgba, blocks_x, blocks_y);
		cache_write(key, out_size);
	}
	glCompressedTexImage2D(target, level, rgba ? GL_COMPRESSED_RGBA8_ETC2_EAC : GL_COMPRESSED_RGB8_ETC2,
		width, height, 0, (GLsizei)out_size, blocks);
	stats.bytes += out_size;
	return 1;
}

static void GL_APIENTRY etc2_compressed_tex_image_2d(GLenum target, GLint level, GLenum format, GLsizei width,
	GLsizei height, GLint border, GLsizei size, const void *data)
{
	int kind = dxt_kind(format);
	/* (a texture's first level: a cube map's, its first face's) */
	int first = level == 0 && (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP_POSITIVE_X);
	uint64_t start;

	if (!kind || !data || width <= 0 || height <= 0)
	{
		glCompressedTexImage2D(target, level, format, width, height, border, size, data);
		return;
	}
	pthread_once(&once, etc2_start);
	pthread_mutex_lock(&lock);
	start = now_ns();
	if (first)
		stats.textures++;
	stats.levels++;
	if (!upload_2d(target, level, kind, width, height, size, data))
	{
		stats.failed++;
		host_logf(HOST_LOG_WARN, "textures: a %dx%d DXT%d level not converted", (int)width, (int)height, kind);
	}
	scratch_trim();
	stats.nanoseconds += now_ns() - start;
	if (first && stats.textures % LOG_EVERY == 0)
		log_stats();
	pthread_mutex_unlock(&lock);
}

/* a 3D level decoded to RGBA8 (slices of whole blocks, one after another) */
static void GL_APIENTRY etc2_compressed_tex_image_3d(GLenum target, GLint level, GLenum format, GLsizei width,
	GLsizei height, GLsizei depth, GLint border, GLsizei size, const void *data)
{
	int kind = dxt_kind(format);
	size_t blocks_x, blocks_y, block_bytes, pitch;
	const unsigned char *source = data;
	unsigned char block[64];
	uint32_t *texels;
	GLsizei z;

	if (!kind || !data || width <= 0 || height <= 0 || depth <= 0)
	{
		glCompressedTexImage3D(target, level, format, width, height, depth, border, size, data);
		return;
	}
	blocks_x = ((size_t)width + 3) / 4;
	blocks_y = ((size_t)height + 3) / 4;
	block_bytes = kind == 1 ? 8 : 16;
	pitch = (size_t)width * 4;
	pthread_once(&once, etc2_start);
	pthread_mutex_lock(&lock);
	stats.decoded_3d++;
	if ((size_t)size < blocks_x * blocks_y * block_bytes * (size_t)depth ||
		!grow((void **)&pixels, &pixels_capacity, (size_t)width * (size_t)height * (size_t)depth * 4))
	{
		stats.failed++;
		pthread_mutex_unlock(&lock);
		return;
	}
	texels = pixels;
	for (z = 0; z < depth; z++)
	{
		unsigned char *slice = (unsigned char *)texels + (size_t)z * (size_t)height * pitch;
		size_t bx, by, y;

		for (by = 0; by < blocks_y; by++)
		{
			for (bx = 0; bx < blocks_x; bx++, source += block_bytes)
			{
				size_t columns = (size_t)width - bx * 4 < 4 ? (size_t)width - bx * 4 : 4;

				decode_block(kind, source, block, 16);
				for (y = 0; y < 4 && by * 4 + y < (size_t)height; y++)
					memcpy(slice + (by * 4 + y) * pitch + bx * 16, block + y * 16, columns * 4);
			}
		}
	}
	glTexImage3D(target, level, GL_RGBA8, width, height, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	stats.bytes += (uint64_t)width * (uint64_t)height * (uint64_t)depth * 4;
	scratch_trim();
	pthread_mutex_unlock(&lock);
}

/* ---------- the guest's imports */

static int (*has_extension_next)(const char *name);

static int etc2_has_extension(const char *name)
{
	if (name && !strcmp(name, S3TC_EXTENSION))
		return 1;
	return has_extension_next(name);
}

void *host_etc2_has_extension(void *next)
{
	if (!next || !etc2_enabled())
		return next;
	has_extension_next = (int (*)(const char *))next;
	return (void *)etc2_has_extension;
}

void *host_etc2_wrap(const char *name, void *function)
{
	if (!function || !etc2_enabled())
		return function;
	if (!strcmp(name, "glCompressedTexImage2D"))
		return (void *)etc2_compressed_tex_image_2d;
	if (!strcmp(name, "glCompressedTexImage3D"))
		return (void *)etc2_compressed_tex_image_3d;
	return function;
}

/*
HOST_ETC2.H

The guest's DXT textures turned into ETC2 for Mali (host_etc2.c).
*/

#ifndef HOST_ETC2_H
#define HOST_ETC2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* host_import_wrap: the converter in place of the driver's
glCompressedTexImage2D and glCompressedTexImage3D (name without "hostgl_"),
else the function given */
void *host_etc2_wrap(const char *name, void *function);
/* host_import_wrap: host_gl_has_extension, with S3TC reported as supported;
next answers for every other extension */
void *host_etc2_has_extension(void *next);

/* host_etc2_encode.cpp: etcpak's encoders. src is BGRA, width pixels to a
row; blocks are taken in raster order */
void host_etc2_encode_rgb(const uint32_t *src, uint64_t *dst, uint32_t blocks, size_t width);
void host_etc2_encode_rgba(const uint32_t *src, uint64_t *dst, uint32_t blocks, size_t width);

#ifdef __cplusplus
}
#endif

#endif

/*
HOST_ETC2_ENCODE.CPP

etcpak's ETC2 encoders for host_etc2.c, which is C. Built without
exceptions or RTTI: etcpak needs nothing from the C++ library but its
headers, so the host links without libstdc++.
*/

#include "host_etc2.h"
#include "ProcessRGB.hpp"

/* (useHeuristics off: faster and better on Cortex-A, measured in Bully) */
extern "C" void host_etc2_encode_rgb(const uint32_t *src, uint64_t *dst, uint32_t blocks, size_t width)
{
	CompressEtc2Rgb(src, dst, blocks, width, false);
}

extern "C" void host_etc2_encode_rgba(const uint32_t *src, uint64_t *dst, uint32_t blocks, size_t width)
{
	CompressEtc2Rgba(src, dst, blocks, width, false);
}

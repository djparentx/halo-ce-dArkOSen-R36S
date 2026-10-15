/*
SYS/SYSTEM_PROPERTIES.H

Android's system properties, which the Android host files read
(port/android/host/host_memory.c: its memory reports and the Java
runtime's checks), for the Knulli build: there are none, so each reads
empty.
*/

#ifndef __HALO_KNULLI_SYS_SYSTEM_PROPERTIES_H
#define __HALO_KNULLI_SYS_SYSTEM_PROPERTIES_H

#define PROP_VALUE_MAX 92

static inline int __system_property_get(const char *name, char *value)
{
	(void)name;
	value[0] = '\0';
	return 0;
}

#endif

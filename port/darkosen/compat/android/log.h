/*
ANDROID/LOG.H

The part of the NDK's logging interface that the Android host files use
(port/android/host), for the Knulli build: the log goes to the standard
error stream, which the launcher script keeps in log.txt.
*/

#ifndef __HALO_KNULLI_ANDROID_LOG_H
#define __HALO_KNULLI_ANDROID_LOG_H

#include <stdarg.h>

enum
{
	ANDROID_LOG_INFO = 4,
	ANDROID_LOG_WARN = 5,
	ANDROID_LOG_ERROR = 6,
	ANDROID_LOG_FATAL = 7,
};

int __android_log_write(int priority, const char *tag, const char *text);
int __android_log_vprint(int priority, const char *tag, const char *format, va_list arguments);
int __android_log_print(int priority, const char *tag, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

#endif

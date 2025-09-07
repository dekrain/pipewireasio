#pragma once

#if !defined(_Nullable) || !defined(_Nonnull)
#if defined(__clang__)
#else
#define _Nullable
#define _Nonnull
#endif
#endif

#include <spa/utils/defs.h>

#include <pthread.h>

typedef int (*pw_helper_thread_creator_t)(
	pthread_t *out_thread,
	const pthread_attr_t *attrs,
	void *(*function)(void *arg),
	void *arg);

struct pw_helper_init_args {
	/// The application that uses the driver.
	char const *_Nullable app_name;
	/// Place to store created pw_thread_loop.
	struct pw_thread_loop *_Nonnull *_Nullable loop;
	/// Place to store created pw_context.
	struct pw_context *_Nonnull *_Nullable context;
	/// Place to store pw_core proxy.
	struct pw_core *_Nonnull *_Nullable core;

	pw_helper_thread_creator_t _Nullable thread_creator;
};

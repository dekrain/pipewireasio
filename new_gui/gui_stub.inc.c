#pragma once

#ifdef GUI_API
#error "GUI stub must be included before gui.h"
#endif

#define GUI_API static

#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <libgen.h>

struct __gui_lib_funcs {
	bool init;
	struct pwasio_gui *(*pwasio_init_gui)(struct pwasio_gui_conf *conf);
	void (*pwasio_destroy_gui)(struct pwasio_gui *gui);
};

static struct __gui_lib_funcs __gui_loader_funcs;

static void __gui_loader_load_lib() {
	Dl_info info = {};
	dladdr(__gui_loader_load_lib, &info);
	char *myself = strdup(info.dli_fname);
	char *path = NULL;
	char const *mydir = dirname(myself);
	void *lib;
	if (*mydir) {
		asprintf(&path, "%s/%s", mydir, GUI_LIB_NAME);
		lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
		free(path);
	} else {
		lib = dlopen(GUI_LIB_NAME, RTLD_NOW | RTLD_GLOBAL);
	}
	free(myself);

	if (!lib) {
		fprintf(stderr, "ERROR: Failed to load GUI lib: %s\n", dlerror());
		return;
	}

	bool load_success = true;
	#define GUI_LOADER_LOAD_FUNC(name) \
		__gui_loader_funcs.name = dlsym(lib, #name); \
		if (!__gui_loader_funcs.name) \
			load_success = false;

	GUI_LOADER_LOAD_FUNC(pwasio_init_gui)
	GUI_LOADER_LOAD_FUNC(pwasio_destroy_gui)
	if (!load_success) {
		fprintf(stderr, "ERROR: Failed to load functions from GUI lib: %s\n", dlerror());
		return;
	}

	#undef GUI_LOADER_LOAD_FUNC

	__gui_loader_funcs.init = true;
}

GUI_API struct pwasio_gui *pwasio_init_gui(struct pwasio_gui_conf *conf) {
	if (!__gui_loader_funcs.init)
		__gui_loader_load_lib();

	if (!__gui_loader_funcs.init)
		return NULL;

	return __gui_loader_funcs.pwasio_init_gui(conf);
}

GUI_API void pwasio_destroy_gui(struct pwasio_gui *gui) {
	if (!__gui_loader_funcs.init)
		__gui_loader_load_lib();

	if (!__gui_loader_funcs.init)
		return;

	return __gui_loader_funcs.pwasio_destroy_gui(gui);
}

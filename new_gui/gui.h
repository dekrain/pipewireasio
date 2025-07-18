#pragma once

#ifndef GUI_API
#define GUI_API
#endif

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pwasio_gui;

enum pwasio_io_config {
	PWASIO_IO_SIMPLE,
	PWASIO_IO_ADVANCED,
};

typedef struct pw_node *pwasio_node_or_default;
#define PWASIO_NODE_NONE ((struct pw_node *)0)
#define PWASIO_NODE_DEFAULT ((struct pw_node *)~(uintptr_t)0)

struct pwasio_ioc_simple {
	pwasio_node_or_default input;
	pwasio_node_or_default output;
};

// node 0 is the default node, node 1.. indexes the `nodes` array.
// ports range in 0..0xFFFF, selecting a port of the node.
typedef uint32_t pwasio_port_sel;

inline static pwasio_port_sel pwasio_port_make(uint32_t node, uint16_t port) {
	//assert(node <= 0xFFFF);
	return (node << 16) | (uint32_t)port;
}

inline static void pwasio_port_get(pwasio_port_sel sel, uint32_t *node, uint16_t *port) {
	*node = sel >> 16;
	*port = (uint16_t)(sel & 0xFFFF);
}

struct pwasio_ioc_advanced {
	struct pw_node **nodes;
	uint32_t cnt_nodes;
	uint32_t cnt_inputs;
	uint32_t cnt_outputs;
	pwasio_port_sel *inputs;
	pwasio_port_sel *outputs;
};

// The configuration shared between the GUI and its client.
struct pwasio_gui_conf {
	// Client's state pointer
	void *user;
	// Called when the GUI wants to close. Called after `apply_config`,
	// if confirmed. The application should call pwasio_destroy_gui in
	// response.
	void (*closed)(struct pwasio_gui_conf *gui);
	// Apply the config using state in this struct.
	void (*apply_config)(struct pwasio_gui_conf *gui);
	// The application should fill state in this struct with
	// stored configuration, or with defaults if not available.
	void (*load_config)(struct pwasio_gui_conf *gui);

	struct user_pw_helper *pw_helper;
	// The state
	uint32_t cf_buffer_size;
	enum pwasio_io_config cf_io_type;
	union {
		struct pwasio_ioc_simple simple;
		struct pwasio_ioc_advanced advanced;
	} cf_io_config;
};

// The config must persist for as long as the GUI is live.
GUI_API struct pwasio_gui *pwasio_init_gui(struct pwasio_gui_conf *conf);
GUI_API void pwasio_destroy_gui(struct pwasio_gui *gui);

#define GUI_LIB_NAME "libpwasio_gui.so"

#ifdef __cplusplus
}
#endif

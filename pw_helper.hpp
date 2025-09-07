#pragma once

#include <pipewire/node.h>
#include <string>
#include <vector>
#include <span>

#include "pw_helper_common.h"

namespace PwHelper {

struct Helper;
typedef struct pw_helper_init_args InitArgs;

Helper *create_helper(int argc, char **argv, InitArgs const *conf);
void destroy_helper(Helper *helper);

std::vector<struct pw_node *> enumerate_pipewire_endpoints(Helper *helper);
struct pw_node *get_default_node(Helper *helper, enum spa_direction direction);
struct pw_node *find_node_by_name(Helper *helper, char const *name);

void get_node_props(Helper *helper, struct pw_node *proxy, std::span<std::pair<std::string_view, std::string*>> props);

}

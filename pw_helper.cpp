#include "pw_helper.hpp"
#include "pw_helper_c.h"
#include "pw_helper_common.h"

#include <chrono>
#include <memory>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <cassert>

#include <thread>
#include <unordered_map>

#include <spa/utils/dict.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/main-loop.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>
#include <pipewire/thread.h>
#include <pipewire/thread-loop.h>
#include <pipewire/extensions/metadata.h>

namespace PwHelper {

struct SpaPod {
	struct spa_pod *ptr;

	~SpaPod() {
		std::free(ptr);
	}

	constexpr explicit SpaPod(struct spa_pod *pod = nullptr)
		: ptr(pod) {}

	inline SpaPod(SpaPod &&o) : ptr(o.ptr) { o.ptr = nullptr; }

	inline SpaPod &operator=(SpaPod&& o) {
		std::swap(ptr, o.ptr);
		o = nullptr;
		return *this;
	}

	inline SpaPod &operator=(std::nullptr_t) {
		std::free(ptr);
		ptr = nullptr;
		return *this;
	}

	inline operator struct spa_pod const *() const {
		return ptr;
	}

	static SpaPod make(struct spa_pod const *pod) {
		return SpaPod(spa_pod_copy(pod));
	}

	// Explicit static method to avoid copying by accident
	static SpaPod copy(SpaPod const &from) {
		return make(from.ptr);
	}
};

// Source: <https://en.cppreference.com/w/cpp/container/unordered_map/find#Example>
struct string_view_hasher {
	using hash_type = std::hash<std::string_view>;
	using is_transparent = void;

	std::size_t operator()(const char* str) const        { return hash_type{}(str); }
	std::size_t operator()(std::string_view str) const   { return hash_type{}(str); }
	std::size_t operator()(std::string const& str) const { return hash_type{}(str); }
};

using string_map = std::unordered_map<std::string, std::string, string_view_hasher, std::equal_to<>>;

enum class InitState {
	Init,
	Ready,
	Running,
};

enum class PwInterface {
	Unknown,
	Node,
	Metadata,
};

enum class ProxyState {
	Init,
	PropsFilled,
	Fetching,
	UpdateInProgress,
};

template <typename Custom>
struct ProxyPtr {
	typename Custom::ProxyType *proxy;

	__always_inline constexpr ProxyPtr(typename Custom::ProxyType *proxy = {}) : proxy{proxy} {}

	__always_inline static ProxyPtr from_bound(void *bound) {
		return ProxyPtr{reinterpret_cast<typename Custom::ProxyType *>(bound)};
	}

	__always_inline operator typename Custom::ProxyType *() const {
		return proxy;
	}

	__always_inline Custom *custom() const {
		return reinterpret_cast<Custom *>(pw_proxy_get_user_data(reinterpret_cast<struct pw_proxy *>(proxy)));
	}

	__always_inline static struct pw_proxy_events const *proxy_events() {
		return &Custom::s_proxy_events;
	}

	inline void intall_proxy_events() const {
		pw_proxy_add_listener(reinterpret_cast<struct pw_proxy *>(proxy), &custom()->Proxy::proxy_listener, proxy_events(), proxy);
	}

	template <typename TBase>
	requires(std::is_base_of_v<TBase, Custom>)
	__always_inline operator ProxyPtr<TBase>() const {
		return ProxyPtr<TBase>{reinterpret_cast<typename TBase::ProxyType *>(proxy)};
	}

	template <typename TDerived>
	requires(std::is_base_of_v<Custom, TDerived>)
	__always_inline ProxyPtr<TDerived> to_derived() const {
		return ProxyPtr<TDerived>{reinterpret_cast<typename TDerived::ProxyType *>(proxy)};
	}

	template <typename TOther>
	__always_inline auto operator<=>(ProxyPtr<TOther> other) const {
		return reinterpret_cast<struct pw_proxy *>(proxy) <=> reinterpret_cast<struct pw_proxy *>(other.proxy);
	}

	template <typename TOther>
	__always_inline bool operator==(ProxyPtr<TOther> other) const {
		return reinterpret_cast<struct pw_proxy *>(proxy) == reinterpret_cast<struct pw_proxy *>(other.proxy);
	}

	__always_inline PwInterface type() const;

	static void destroy_handler(void *proxy) {
		ProxyPtr::from_bound(proxy).custom()->~Custom();
	}
};

struct Proxy {
	using ProxyType = struct pw_proxy;
	PwInterface type;
	struct spa_hook proxy_listener;
};

template <typename T>
__always_inline PwInterface ProxyPtr<T>::type() const {
	return ProxyPtr<struct Proxy>(*this).custom()->type;
}

struct Node final: Proxy {
	using ProxyType = struct pw_node;

	std::atomic<ProxyState> info_state;
	std::atomic<ProxyState> param_state;
	struct spa_hook listener;
	struct pw_node_info info;
	string_map properties;
	std::unordered_map<uint32_t, SpaPod> params;

	static struct pw_node_events const s_events;
	static struct pw_proxy_events const s_proxy_events;

	void init(ProxyPtr<Node> proxy) {
		new (this) Node;
		Proxy::type = PwInterface::Node;
		info_state.store(ProxyState::Init, std::memory_order_relaxed);
		param_state.store(ProxyState::Init, std::memory_order_relaxed);
		listener = {};
		struct pw_node *raw_proxy = proxy;
		pw_node_add_listener(raw_proxy, &listener, &s_events, raw_proxy);
		//pw_node_enum_params(raw_proxy, 0, /* param_id */, 0, ~(uint32_t)0, nullptr);
	}

	// Bad name
	bool inited(std::mutex& mutex) {
		auto state = info_state.load(std::memory_order_relaxed);
		switch (state) {
			case ProxyState::Init:
			case ProxyState::Fetching:
				mutex.unlock();
				info_state.wait(state);
				mutex.lock();
				return false;
			case ProxyState::PropsFilled:
			case ProxyState::UpdateInProgress:
				return true;
		}
	}

	void get_or_wait_for_info(
		struct pw_node_info *out_info,
		string_map *all_props,
		std::span<std::pair<std::string_view, std::string *> const> props
	) {
		// Wait for init
		info_state.wait(ProxyState::Init, std::memory_order_relaxed);
		// Then for updates
		ProxyState state = ProxyState::PropsFilled;
		while (!info_state.compare_exchange_weak(state, ProxyState::Fetching)) {
			// Unchanged, try again.
			if (state == ProxyState::PropsFilled)
				continue;
			if (state != ProxyState::UpdateInProgress) {
				std::fprintf(stderr, "[FATAL] Info state (%u) not in UpdateInProgress state\n", static_cast<unsigned>(state));
				std::abort();
			}
			info_state.wait(ProxyState::UpdateInProgress);
		}

		// Our turn to read
		if (out_info)
			*out_info = info;
		#if 0
		if (props) {
			if (props->empty()) {
				// Copy all props
				*props = properties;
			} else {
				// Only copy requested ones
				for (auto it = props->begin(); it != props->end(); ++it) {
					if (auto prop = properties.find(it->first); prop != properties.end()) {
						it->second = prop->second;
					}
				}
			}
		}
		#endif
		if (all_props) {
			// Copy all props
			*all_props = properties;
		}
		if (!props.empty()) {
			// Copy requested ones
			for (auto it = props.begin(); it != props.end(); ++it) {
				if (auto prop = properties.find(it->first); prop != properties.end()) {
					*it->second = prop->second;
				}
			}
		}
		info_state.store(ProxyState::PropsFilled, std::memory_order_release);
		info_state.notify_one();
	}

	#if 0
	SpaPod get_or_wait_for_param(uint32_t id) {
		// Wait for init
		// TODO: Handle multi-step init
		param_state.wait(ProxyState::Init, std::memory_order_relaxed);
		// Then for updates
		ProxyState state = ProxyState::PropsFilled;
		while (!param_state.compare_exchange_weak(state, ProxyState::Fetching)) {
			assert(state == ProxyState::UpdateInProgress);
			param_state.wait(ProxyState::UpdateInProgress);
		}

		// Our turn to read
		if (auto it = params.find(name); it != params.end())
			SpaPod pod = SpaPod::copy();

		param_state.store(ProxyState::PropsFilled, std::memory_order_release);
		param_state.notify_one();
		return pod;
	}
	#endif

	void update(struct pw_node_info const *new_info) {
		ProxyState state = ProxyState::Init;
		if (!info_state.compare_exchange_weak(state, ProxyState::UpdateInProgress)) {
			state = ProxyState::PropsFilled;
			while (!info_state.compare_exchange_weak(state, ProxyState::UpdateInProgress)) {
				assert(state == ProxyState::Fetching);
				info_state.wait(ProxyState::Fetching);
			}
		}

		// We can write now
		info = *new_info;
		properties.clear();
		for (auto *it = info.props->items, *end = it + info.props->n_items; it != end; ++it) {
			std::string key = it->key;
			std::string value = it->value;
			properties.emplace(std::move(key), std::move(value));
		}
		info_state.store(ProxyState::PropsFilled, std::memory_order_release);
		info_state.notify_one();
	}

	void update_param(void *proxy, uint32_t id, uint32_t index, uint32_t next, struct spa_pod const *param) {
		ProxyState state = ProxyState::Init;
		if (!param_state.compare_exchange_weak(state, ProxyState::UpdateInProgress)) {
			state = ProxyState::PropsFilled;
			while (!param_state.compare_exchange_weak(state, ProxyState::UpdateInProgress)) {
				assert(state == ProxyState::Fetching);
				param_state.wait(ProxyState::Fetching);
			}
		}

		SpaPod pod = SpaPod::make(param);
		if (auto it = params.find(id); it != params.end()) {
			it->second = std::move(pod);
		} else {
			params.emplace(id, std::move(pod));
			//pw_node_subscribe_params(proxy, &id, 1);
		}

		if (state == ProxyState::Init && next != PW_ID_ANY) {
			param_state.store(ProxyState::Init, std::memory_order_release);
		} else {
			param_state.store(ProxyState::PropsFilled, std::memory_order_release);
			param_state.notify_one();
		}
	}
};

struct pw_proxy_events const Node::s_proxy_events = {
	.version = PW_VERSION_PROXY_EVENTS,
	.destroy = ProxyPtr<Node>::destroy_handler,
};

static void node_info_handler(void *proxy, struct pw_node_info const *info) {
	ProxyPtr<Node>::from_bound(proxy).custom()->update(info);
}

static void node_param_handler(void *proxy, int seq, uint32_t id, uint32_t index, uint32_t next, struct spa_pod const *param) {
	ProxyPtr<Node>::from_bound(proxy).custom()->update_param(proxy, id, index, next, param);
}

struct pw_node_events const Node::s_events = {
	.version = PW_VERSION_NODE_EVENTS,
	.info = node_info_handler,
	.param = node_param_handler,
};

struct Metadata: Proxy {
	using ProxyType = struct pw_metadata;

	struct spa_hook listener;
	struct MetadataHandler const *vtable;

	static struct pw_metadata_events const s_events;

	void init(ProxyPtr<Metadata> proxy) {
		Proxy::type = PwInterface::Metadata;
		vtable = nullptr;
		struct pw_metadata *raw_proxy = proxy;
		pw_metadata_add_listener(raw_proxy, &listener, &s_events, raw_proxy);
	}
};

enum MetaPropResult {
	Continue, // continue searching
	Stop, // stop search
};

struct MetadataProp {
	uint32_t subject;
	char const *key;
	char const *type;
	char const *value;

	// more data...
};

struct MetadataPropHandler {
	char const *key; // null if any
	char const *type; // null if any
	size_t proc_size;
	MetaPropResult (*handler)(ProxyPtr<Metadata> proxy, MetadataPropHandler const *handler, MetadataProp *prop);
};

struct MetadataHandler {
	MetadataPropHandler const *const *properties;
	size_t num_properties;
	// Called when specific properties were not satisfied.
	void (*generic_prop)(ProxyPtr<Metadata> proxy, uint32_t subject, char const *key, char const *type, char const *value);
};

static int meta_property_handler(void *proxy, uint32_t subject, char const *key, char const *type, char const *value) {
	auto mproxy = ProxyPtr<Metadata>::from_bound(proxy);
	Metadata *meta = mproxy.custom();
	if (!meta->vtable) {
		return 0;
	}
	std::string_view svkey = key;
	std::string_view svtype = type;
	//std::printf("[DEBUG] Got property '%s' of type '%s'\n", key, type);
	for (size_t idx = 0; idx != meta->vtable->num_properties; ++idx) {
		auto const *prop = meta->vtable->properties[idx];
		if (prop->key and svkey != prop->key)
			continue;
		if (prop->type and svtype != prop->type)
			continue;

		//std::printf("[DEBUG] Handling property '%s' of type '%s'\n", key, type);

		MetadataProp *scratch = reinterpret_cast<MetadataProp *>(std::malloc(prop->proc_size));
		scratch->subject = subject;
		scratch->key = key;
		scratch->type = type;
		scratch->value = value;
		auto res = prop->handler(mproxy, prop, scratch);
		std::free(scratch);
		switch (res) {
			case MetaPropResult::Continue: break;
			case MetaPropResult::Stop: return 0;
		}
	}
	if (meta->vtable->generic_prop) {
		meta->vtable->generic_prop(mproxy, subject, key, type, value);
	}
	return 0;
}

struct pw_metadata_events const Metadata::s_events = {
	.version = PW_VERSION_METADATA_EVENTS,
	.property = meta_property_handler,
};

struct MetadataJson: MetadataProp {
	char pod_buffer[0x800];
};

#define SPA_TYPE_STRING_JSON SPA_TYPE_INFO_BASE "String:JSON"

struct MetadataJsonHandler: MetadataPropHandler {
	//JsonType const *type;
	MetaPropResult (*handler)(ProxyPtr<Metadata> proxy, MetadataJsonHandler const *handler, MetadataJson *prop, struct spa_json *parser);
};

static MetaPropResult meta_preproc_json(ProxyPtr<Metadata> proxy, MetadataPropHandler const *handler, MetadataProp *prop) {
	auto *jhandler = static_cast<MetadataJsonHandler const *>(handler);
	MetadataJson *json = static_cast<MetadataJson *>(prop);
	struct spa_json parser;
	spa_json_init(&parser, prop->value, std::strlen(prop->value));
	return jhandler->handler(proxy, jhandler, json, &parser);
}

static bool parse_json_dict(struct spa_json *parser, char *heap, size_t heap_size, std::span<char const * const> keys, std::span<char *> values) {
	char const *token;
	struct spa_json obj;
	int l;
	size_t entries;
	if (keys.size() != values.size())
		return false;
	entries = keys.size();
	if (spa_json_enter_object(parser, &obj) <= 0)
		return false;
	while ((l = spa_json_get_string(&obj, heap, heap_size)) > 0) {
		std::string_view key = heap;
		assert(key.size() + 1 <= heap_size);
		for (size_t idx = 0; idx < entries; ++idx) {
			if (keys[idx] == key and !values[idx]) {
				l = spa_json_get_string(&obj, heap, heap_size);
				if (l <= 0)
					return false;
				std::string_view value = heap;
				assert(value.size() + 1 <= heap_size);

				values[idx] = heap;
				heap += value.size() + 1;
				heap_size -= value.size() + 1;
				goto next_item;
			}
		}

		// Skip value
		l = spa_json_next(&obj, &token);
		if (l <= 0)
			return false;
		else if (l > 0) {
			if (spa_json_is_container(token, l)) {
				struct spa_json sub;
				spa_json_enter(&obj, &sub);
				while ((l = spa_json_next(&sub, &token) > 0)) {}
				if (l < 0)
					return false;
			}
		}

		next_item:;
	}
	if (l < 0)
		return false;

	return true;
}

struct DefaultNodes: Metadata {
	std::mutex mutex;
	std::string default_source;
	std::string default_sink;

	static struct pw_proxy_events const s_proxy_events;
	static MetadataHandler const s_handler;

	void init(ProxyPtr<DefaultNodes> proxy) {
		new (this) DefaultNodes;
		Metadata::init(proxy);
		Metadata::vtable = &s_handler;
	}
};

struct pw_proxy_events const DefaultNodes::s_proxy_events = {
	.version = PW_VERSION_PROXY_EVENTS,
	.destroy = ProxyPtr<DefaultNodes>::destroy_handler,
};

static char const *default_nodes_keys[] = {
	"name",
};

static MetaPropResult default_nodes_source_handler(ProxyPtr<Metadata> proxy, MetadataJsonHandler const*, MetadataJson *prop, struct spa_json *parser) {
	char const *source;
	char *values[1] = {};
	if (!parse_json_dict(parser, prop->pod_buffer, sizeof prop->pod_buffer, default_nodes_keys, values)) {
		std::fputs("[DEBUG] Invalid JSON dict\n", stderr);
	}

	source = values[0];
	if (source) {
		auto nodes = proxy.to_derived<DefaultNodes>().custom();
		nodes->mutex.lock();
		nodes->default_source.assign(source);
		nodes->mutex.unlock();
		//std::printf("[DEBUG] Found default source: %s\n", source);
	} else {
		std::fputs("[DEBUG] Default source node not found in metadata\n", stderr);
	}
	return MetaPropResult::Stop;
}

static MetaPropResult default_nodes_sink_handler(ProxyPtr<Metadata> proxy, MetadataJsonHandler const*, MetadataJson *prop, struct spa_json *parser) {
	char const *sink;
	char *values[1] = {};
	if (!parse_json_dict(parser, prop->pod_buffer, sizeof prop->pod_buffer, default_nodes_keys, values)) {
		std::fputs("[DEBUG] Invalid JSON dict\n", stderr);
	}

	sink = values[0];
	if (sink) {
		auto nodes = proxy.to_derived<DefaultNodes>().custom();
		nodes->mutex.lock();
		nodes->default_sink.assign(sink);
		nodes->mutex.unlock();
		//std::printf("[DEBUG] Found default sink: %s\n", sink);
	} else {
		std::fputs("[DEBUG] Default sink node not found in metadata\n", stderr);
	}
	return MetaPropResult::Stop;
}

static MetadataJsonHandler const default_nodes_source_prop {
	MetadataPropHandler {
		.key = "default.audio.source",
		.type = SPA_TYPE_STRING_JSON,
		.proc_size = sizeof(MetadataJson),
		.handler = meta_preproc_json,
	},
	default_nodes_source_handler,
};

static MetadataJsonHandler const default_nodes_sink_prop {
	MetadataPropHandler {
		.key = "default.audio.sink",
		.type = SPA_TYPE_STRING_JSON,
		.proc_size = sizeof(MetadataJson),
		.handler = meta_preproc_json,
	},
	default_nodes_sink_handler,
};

static MetadataPropHandler const *const default_nodes_props[] = {
	&default_nodes_source_prop,
	&default_nodes_sink_prop,
};

MetadataHandler const DefaultNodes::s_handler = {
	.properties = default_nodes_props,
	.num_properties = std::size(default_nodes_props),
	.generic_prop = nullptr,
};

struct Helper {
	//struct pw_main_loop *main_loop = {};
	struct pw_thread_loop *thread_loop = {};
	struct pw_context *context = {};
	struct pw_core *core = {};
	struct pw_registry *registry = {};
	struct spa_hook registry_listener = {};

	struct spa_thread_utils *thread_impl = {};
	pw_helper_thread_creator_t thread_creator = {};
	struct spa_thread_utils thread_utils;

	std::unordered_map<uint32_t, ProxyPtr<Proxy>> bound_proxies;
	ProxyPtr<DefaultNodes> default_nodes = {};

	std::atomic<InitState> init_state = InitState::Init;
	std::atomic<int> roundtrip_state = -1;
	std::mutex state_mutex;

	struct spa_hook roundtrip = {};

	~Helper() {
		lock();
		for (auto& proxy: bound_proxies) {
			pw_proxy_destroy(proxy.second);
		}
		if (core) {
			pw_core_disconnect(core);
		}
		if (context) {
			pw_context_destroy(context);
		}
		if (thread_loop) {
			pw_thread_loop_destroy(thread_loop);
		}
		unlock();
	}

	void stop() {
		pw_thread_loop_stop(this->thread_loop);
	}

	void lock() {
		if (init_state.load(std::memory_order_relaxed) == InitState::Running) {
			state_mutex.lock();
		}
	}

	void unlock() {
		if (init_state.load(std::memory_order_relaxed) == InitState::Running) {
			state_mutex.unlock();
		}
	}

	void wait_for_roundtrip() {
		init_state.wait(InitState::Ready, std::memory_order_relaxed);
	}

	PwInterface get_proxy(uint32_t id, ProxyPtr<Proxy> &proxy) {
		if (auto it = bound_proxies.find(id); it != bound_proxies.end()) {
			proxy = it->second;
			return proxy.type();
		}
		return PwInterface::Unknown;
	}
};

#include "pw_thread.inc.cpp"

using namespace std::string_view_literals;
#define _SV(x) x##sv
#define SV(x) _SV(x)

static PwInterface get_known_interface(char const *type) {
	std::string_view svtype = type;
	if (svtype == SV(PW_TYPE_INTERFACE_Node)) {
		return PwInterface::Node;
	}
	if (svtype == SV(PW_TYPE_INTERFACE_Metadata)) {
		return PwInterface::Metadata;
	}
	return PwInterface::Unknown;
}

static void registry_global_handler(
	void *data, uint32_t id, uint32_t permissions,
	char const *type, uint32_t version, struct spa_dict const *props
) {
	Helper *This = reinterpret_cast<Helper *>(data);
	switch (get_known_interface(type)) {
		case PwInterface::Node: {
			This->lock();
			auto proxy = ProxyPtr<Node>::from_bound(
				pw_registry_bind(This->registry, id, type, std::min(version, (uint32_t)PW_VERSION_NODE), sizeof(Node)));
			proxy.custom()->init(proxy);
			proxy.intall_proxy_events();
			This->bound_proxies.emplace(id, proxy);
			This->unlock();
			break;
		}
		case PwInterface::Metadata: {
			if ("default"sv == spa_dict_lookup(props, PW_KEY_METADATA_NAME)) {
				This->lock();
				auto proxy = ProxyPtr<DefaultNodes>::from_bound(
					pw_registry_bind(This->registry, id, type, std::min(version, (uint32_t)PW_VERSION_METADATA), sizeof(DefaultNodes)));
				proxy.custom()->init(proxy);
				proxy.intall_proxy_events();
				This->bound_proxies.emplace(id, proxy);
				if (This->default_nodes) {
					std::puts("New default nodes object? Overriding old one.");
				}
				This->default_nodes = proxy;
				This->unlock();
			}
			break;
		}
		case PwInterface::Unknown: break;
	}
}

static void registry_global_remove_handler(void *data, uint32_t id) {
	Helper *This = reinterpret_cast<Helper *>(data);
	ProxyPtr<Proxy> global;
	This->lock();
	switch (This->get_proxy(id, global)) {
		case PwInterface::Metadata:
			if (This->default_nodes == global) {
				This->default_nodes = nullptr;
			}
			goto destroy_proxy;

		case PwInterface::Node:
		destroy_proxy:
			This->bound_proxies.erase(id);
			pw_proxy_destroy(global);
			break;
		case PwInterface::Unknown: break;
	}
	This->unlock();
}

static struct pw_registry_events const s_registry_events = {
	.version = PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global_handler,
	.global_remove = registry_global_remove_handler,
};

static void roundtrip_handler(void *data, uint32_t id, int seq) {
	Helper *This = reinterpret_cast<Helper *>(data);
	if (false) {
		// This is to test whether the initialization code propely waits for the roundtrip.
		std::this_thread::sleep_for(std::chrono::seconds(2));
	}
	This->init_state.store(InitState::Running, std::memory_order_relaxed);
	This->init_state.notify_all();
}

static void ping_handler(void *data, uint32_t id, int seq) {
	Helper *This = reinterpret_cast<Helper *>(data);
	std::puts("[DEBUG] Received ping");
	pw_core_pong(This->core, id, seq);
}

static void error_handler(void *data, uint32_t id, int seq, int res, const char *message) {
	Helper *This = reinterpret_cast<Helper *>(data);
	char const *obj_type = nullptr;
	// id is a local proxy ID, to index into bound_proxies, get the global ID first
	struct pw_proxy *proxy = pw_core_find_proxy(This->core, id);
	uint32_t global_id = PW_ID_ANY;
	if (proxy) {
		obj_type = pw_proxy_get_type(proxy, nullptr);
		global_id = pw_proxy_get_bound_id(proxy);
	}
	std::fprintf(stderr, "[ERROR] PipeWire error on object %u (%s, global %d): %s: %s\n", id, obj_type, global_id, message, spa_strerror(res));
}

static struct pw_core_events const s_core_events = {
	.version = PW_VERSION_CORE_EVENTS,
	.done = roundtrip_handler,
	.ping = ping_handler,
	.error = error_handler,
};

Helper *create_helper(int argc, char **argv, InitArgs const *conf) {
	pw_init(&argc, &argv);
	printf("PipeWire initialized with version: %s\n", pw_get_library_version());

	std::unique_ptr<Helper> This(new Helper);

	if (!(This->thread_loop = pw_thread_loop_new("pw-loop", NULL)))
	{
		std::fputs("Unable to create the PipeWire loop\n", stderr);
		return nullptr;
	}

	struct pw_properties *init_props = pw_properties_new(
		PW_KEY_CLIENT_NAME, "pw-asio",
		PW_KEY_CLIENT_API, "ASIO",
		nullptr);
	if (conf->app_name) {
		pw_properties_set(init_props, PW_KEY_APP_NAME, conf->app_name);
	}

	if (!(This->context = pw_context_new(
		pw_thread_loop_get_loop(This->thread_loop),
		init_props, 0)))
	{
		std::fputs("Unable to create a PipeWire context\n", stderr);
		return nullptr;
	}

	if (conf->thread_creator) {
		This->thread_creator = conf->thread_creator;
	}

	This->thread_impl = reinterpret_cast<struct spa_thread_utils *>(pw_context_get_object(This->context, SPA_TYPE_INTERFACE_ThreadUtils));
	if (!This->thread_impl) {
		This->thread_impl = pw_thread_utils_get();
	}
	This->thread_utils.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_ThreadUtils,
			SPA_VERSION_THREAD_UTILS,
			&thread_utils_impl, This.get());
	pw_context_set_object(This->context, SPA_TYPE_INTERFACE_ThreadUtils, &This->thread_utils);

	if (!(This->core = pw_context_connect(This->context, NULL, 0)))
	{
		std::fputs("Unable to connect to a PipeWire server\n", stderr);
		return nullptr;
	}

	if (!(This->registry = pw_core_get_registry(This->core, PW_VERSION_REGISTRY, 0)))
	{
		std::fputs("Unable to get the PipeWire registry\n", stderr);
		return nullptr;
	}

	pw_registry_add_listener(This->registry, &This->registry_listener, &s_registry_events, This.get());

	// Make sure to get all registered nodes before the client asks for them. (could be done
	// at a later point, but let's just wait now)
	pw_core_add_listener(This->core, &This->roundtrip, &s_core_events, This.get());

	This->init_state.store(InitState::Ready, std::memory_order_relaxed);

	This->roundtrip_state.store(0, std::memory_order_relaxed);
	pw_core_sync(This->core, PW_ID_CORE, 0);

	std::puts("[DEBUG] Starting thread");
	if (pw_thread_loop_start(This->thread_loop)) {
		std::fputs("Unable to start the PipeWire loop\n", stderr);
		return nullptr;
	}

	This->wait_for_roundtrip();
	std::puts("[DEBUG] Rountrip done");

	if (conf->loop)
		*conf->loop = pw_thread_loop_get_loop(This->thread_loop);
	if (conf->context)
		*conf->context = This->context;
	if (conf->core)
		*conf->core = This->core;

	return This.release();
}

void destroy_helper(Helper *helper) {
	helper->stop();
	delete helper;
}

std::vector<struct pw_node *> enumerate_pipewire_endpoints(Helper *helper) {
	std::vector<struct pw_node *> nodes;
	helper->lock();
	for (auto it = helper->bound_proxies.begin(), end = helper->bound_proxies.end(); it != end; ++it) {
		if (it->second.type() == PwInterface::Node) {
			nodes.push_back(it->second.to_derived<Node>());
		}
	}
	helper->unlock();
	return nodes;
}

static struct pw_node *find_node_by_name_locked(Helper *helper, std::string_view name) {
	std::string nod_name;
	std::pair<std::string_view, std::string*> const props[] {
			{PW_KEY_NODE_NAME, &nod_name},
	};
	struct pw_node *found = nullptr;
	for (auto it = helper->bound_proxies.begin(), end = helper->bound_proxies.end(); it != end; ++it) {
		if (it->second.type() == PwInterface::Node) {
			it->second.to_derived<Node>().custom()->get_or_wait_for_info(nullptr, nullptr, props);
			if (nod_name == name) {
				found = it->second.to_derived<Node>();
				break;
			}
		}
	}
	return found;
}

static void wait_for_nodes_init(Helper *helper, std::mutex &mutex) {
	retry:
	for (auto it = helper->bound_proxies.begin(), end = helper->bound_proxies.end(); it != end; ++it) {
		if (it->second.type() == PwInterface::Node) {
			if (!it->second.to_derived<Node>().custom()->inited(mutex)) {
				goto retry;
			}
		}
	}
}

struct pw_node *get_default_node(Helper *helper, enum spa_direction direction) {
	struct pw_node *node = nullptr;
	helper->lock();
	if (helper->default_nodes) {
		auto *nodes = helper->default_nodes.custom();
		nodes->mutex.lock();
		wait_for_nodes_init(helper, nodes->mutex);
		std::string_view name;
		switch (direction) {
			case SPA_DIRECTION_INPUT: name = nodes->default_source; break;
			case SPA_DIRECTION_OUTPUT: name = nodes->default_sink; break;
		}
		node = find_node_by_name_locked(helper, name);
		nodes->mutex.unlock();
	}
	helper->unlock();
	return node;
}

struct pw_node *find_node_by_name(Helper *helper, char const *name) {
	helper->lock();
	struct pw_node *found = find_node_by_name_locked(helper, name);
	helper->unlock();
	return found;
}

void get_node_props(Helper *helper, struct pw_node *proxy, std::span<std::pair<std::string_view, std::string*>> props) {
	Node *node = ProxyPtr<Node>(proxy).custom();
	node->get_or_wait_for_info(nullptr, nullptr, props);
}

void lock_loop(Helper *helper) {
	pw_thread_loop_lock(helper->thread_loop);
}

void unlock_loop(Helper *helper) {
	pw_thread_loop_unlock(helper->thread_loop);
}

// C API

extern "C" {

struct user_pw_helper *user_pw_create_helper(int argc, char **argv, struct pw_helper_init_args const *conf) {
	return reinterpret_cast<struct user_pw_helper *>(create_helper(argc, argv, conf));
}

void user_pw_destroy_helper(struct user_pw_helper *helper) {
	destroy_helper(reinterpret_cast<Helper *>(helper));
}

struct pw_node *user_pw_get_default_node(struct user_pw_helper *helper, enum spa_direction direction) {
	return get_default_node(reinterpret_cast<Helper *>(helper), direction);
}

struct pw_node *user_pw_find_node_by_name(struct user_pw_helper *helper, char const *name) {
	return find_node_by_name(reinterpret_cast<Helper *>(helper), name);
}

void user_pw_lock_loop(struct user_pw_helper *helper) {
	lock_loop(reinterpret_cast<Helper *>(helper));
}

void user_pw_unlock_loop(struct user_pw_helper *helper) {
	unlock_loop(reinterpret_cast<Helper *>(helper));
}

}

}

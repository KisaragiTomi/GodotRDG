#pragma once

#include "RenderGraphResources.h"

#include "core/math/projection.h"
#include "scene/resources/compositor.h"
#include "scene/resources/texture_rd.h"

class RenderData;
class RenderSceneBuffersRD;
class FRDGBuilder;
class TGScreenGIHistory;

/** TG-style live diffuse GI: sparse screen / triangle BVH tracing into L0+L1 SH, temporal accumulation and a variance
 *  guided post denoise, written as UE renderer code on RDG. No lighting is baked; the BVH comes from
 *  tools/prepare-sponza.py (res://generated/sponza/).
 *
 *  Runs before the opaque pass (the materials sample sh_texture / position_texture / normal_texture); the companion
 *  effect returned by get_history_effect() runs after the opaque pass and keeps this frame's radiance + depth for the
 *  next frame's screen hits. Scratch textures are RDG transients, the history is extracted from one graph and
 *  registered in the next (UE's view state history pattern). The script API matches the former GDScript TGScreenGI:
 *  setters marked "render thread" are queued with RenderingServer::call_on_render_thread(). */
class TGScreenGI : public CompositorEffect {
	GDCLASS(TGScreenGI, CompositorEffect);

public:
	static constexpr int32 WorldCacheSize = 65536;

	enum SamplingProfile {
		SAMPLING_SPARSE = 0,
		SAMPLING_QUALITY = 1,
		SAMPLING_REFERENCE = 2,
		SAMPLING_ADAPTIVE = 3,
	};

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	/** lights: sun direction, sun color, lamp position (w: range), lamp color (w: falloff); box: min (w: visible),
	 *  max, color. Render thread. */
	void set_inputs(const PackedFloat32Array &p_lights, const PackedFloat32Array &p_box, bool p_active = true, bool p_screen_trace = true, int p_history_length = 48, const Vector3 &p_sky = Vector3());
	/** Allocates the GPU resources for the internal size; emits buffers_ready. Render thread. */
	void prepare(const Vector2i &p_size);
	/** Calls p_callback (deferred, main thread) with the counters, SH, guides, candidates and history of the last frame. */
	void read_debug(const Callable &p_callback, bool p_include_world_cache = false);
	/** Clears the GI history like a hard lighting change; with p_world_cache also the world cache. Render thread only. */
	void reset_history(bool p_world_cache = false);
	void shutdown();
	/** Called by the history effect after the opaque pass. Render thread. */
	void capture_history(const RenderData *p_render_data);

	void set_sampling(int p_profile);
	int get_sampling() const { return sampling_profile; }
	void set_post_denoise(bool p_enabled);
	bool get_post_denoise() const { return post_denoise; }
	void set_world_cache(bool p_enabled);
	bool get_world_cache() const { return world_cache; }
	void set_stable_denoise(bool p_enabled);
	bool get_stable_denoise() const { return stable_denoise; }
	void set_surface_history(bool p_enabled);
	bool get_surface_history() const { return surface_history; }
	void set_adaptive_priority(bool p_enabled) { adaptive_priority = p_enabled; }
	bool get_adaptive_priority() const { return adaptive_priority; }
	void set_world_cache_updates(bool p_enabled) { world_cache_updates = p_enabled; }
	bool get_world_cache_updates() const { return world_cache_updates; }
	/** Extract the transient candidates every frame so read_debug() answers from the last frame without waiting for
	 *  the next one (validation / capture runs). Off: they stay transient and a read costs one extra frame. */
	void set_debug_readback(bool p_enabled) { debug_readback = p_enabled; }
	bool get_debug_readback() const { return debug_readback; }
	void set_geometry_root(const String &p_root) { geometry_root = p_root; }
	String get_geometry_root() const { return geometry_root; }
	/** Frame counter of the temporal sequences. Render thread. */
	void set_frame_index(int p_frame) { frame = p_frame; }
	int get_frame_index() const { return frame; }

	Dictionary get_stats() const { return stats; }
	Ref<Texture2DRD> get_sh_texture() const { return sh_texture; }
	Ref<Texture2DRD> get_position_texture() const { return position_texture; }
	Ref<Texture2DRD> get_normal_texture() const { return normal_texture; }
	Ref<CompositorEffect> get_history_effect() const;
	/** Stats of the last pre-opaque graph (passes, culled, transient textures / memory, ...). */
	Dictionary get_graph_stats() const;
	static String get_default_geometry_root() { return "res://generated/sponza/"; }

	TGScreenGI();
	~TGScreenGI();

private:
	void _render_gi(int p_effect_callback_type, const RenderData *p_render_data);
	bool _initialize_gpu();
	void _ensure_size(const Vector2i &p_size, bool p_force = false);
	void _release_size_dependent();
	void _cleanup();
	void _allocate_world_cache(bool p_full);
	bool _world_cache_active() const;
	void _invalidate_world_cache(int p_cooldown = 3);
	int _ray_layers() const;
	void _read_timing();
	void _collect_graph_stats(const FRDGBuilder &GraphBuilder);

	void _set_inputs_gpu(const PackedFloat32Array &p_lights, const PackedFloat32Array &p_box, bool p_active, bool p_screen_trace, int p_history_length, const PackedFloat32Array &p_sky);
	void _prepare_gpu(const Vector2i &p_size);
	void _set_sampling_gpu(int p_profile);
	void _set_post_denoise_gpu(bool p_enabled);
	void _set_world_cache_gpu(bool p_enabled);
	void _read_debug_gpu(const Callable &p_callback, bool p_include_world_cache);
	void _queue_debug_read(const Callable &p_callback, bool p_include_world_cache);

	// Main thread (deferred) reports.
	void _reset_report();
	void _timing_report(double p_ms, const Dictionary &p_stages, int p_frame);
	void _frame_report(int p_frame, const Vector2i &p_size);
	void _failure(const String &p_message);
	void _emit_buffers_ready();
	void _emit_resize_requested(const Vector2i &p_size);

	Ref<Texture2DRD> sh_texture;
	Ref<Texture2DRD> position_texture;
	Ref<Texture2DRD> normal_texture;
	Ref<TGScreenGIHistory> history_effect;
	Dictionary stats;
	LocalVector<Dictionary> stage_timing_window;
	String geometry_root;

	// Settings (render thread once the effect is attached).
	bool stable_denoise = true;
	bool post_denoise = true;
	int sampling_profile = SAMPLING_SPARSE;
	bool adaptive_priority = true;
	/** Independent, pre-fog surface radiance; never reads the final display TAA. */
	bool surface_history = false;
	/** Optional screen-fed world cache. Sources are diffuse SH, never display RGB. */
	bool world_cache = false;
	bool world_cache_updates = true;
	bool debug_readback = false;

	// Render thread state.
	RenderingDevice *RD = nullptr;
	bool closed = false;
	bool active = true;
	bool screen_trace = true;
	int max_history = 48;
	PackedFloat32Array lights;
	PackedFloat32Array box;
	PackedFloat32Array sky;

	Vector2i size;
	Vector2i half_size;
	Vector2i trace_size;
	Vector2i requested_size;
	uint32 node_count = 0;
	int frame = 0;
	bool history_valid = false;
	int history_cooldown = 3;
	int feedback_cooldown = 0;
	int reactive_frames = 0;
	int motion_frames = 0;
	bool rendered_frame = false;
	bool taa_history_active = false;
	uint64 last_timing_frame = UINT64_MAX;
	Projection previous_vp;
	Projection previous_inverse;
	Projection current_vp;
	Projection current_inverse;
	Transform3D previous_camera;
	Vector2 previous_projection_scale;
	/** TGView contents of the current frame (16-float matrices + vec4s), shared by the pre-opaque and history graphs. */
	LocalVector<float> view_constants;

	bool world_allocated = false;
	bool clear_world_cache = false;
	uint32 world_epoch = 1;
	uint32 world_tick = 1;
	int world_cooldown = 3;

	// Static scene data (plain RD resources registered as external RDG resources every frame).
	RID nodes_buffer;
	RID triangles_buffer;
	RID materials_buffer;
	uint32 num_nodes_bytes = 0;
	uint32 num_triangles_bytes = 0;
	uint32 num_materials_bytes = 0;
	RID albedo_array;
	RID metal_array;

	// Persistent GPU state.
	TRefCountPtr<FRDGPooledBuffer> counter_buffer;
	TRefCountPtr<FRDGPooledBuffer> world_buffer;
	TRefCountPtr<FRDGPooledBuffer> world_owners;
	/** Read by the materials through the Texture2DRDs: stable textures, recreated only on resize. */
	TRefCountPtr<IPooledRenderTarget> filtered;
	TRefCountPtr<IPooledRenderTarget> display_position;
	TRefCountPtr<IPooledRenderTarget> display_normal;
	bool clear_display = false;
	/** Previous frame (extracted). Null after a resize: the passes read cleared textures, like fresh history. */
	TRefCountPtr<IPooledRenderTarget> history_position;
	TRefCountPtr<IPooledRenderTarget> history_normal;
	TRefCountPtr<IPooledRenderTarget> history_sh;
	TRefCountPtr<IPooledRenderTarget> history_age;
	TRefCountPtr<IPooledRenderTarget> history_moments;
	TRefCountPtr<IPooledRenderTarget> history_color;
	TRefCountPtr<IPooledRenderTarget> history_depth;
	/** Transient textures extracted only while a debug read is pending. */
	bool extract_debug = false;
	TRefCountPtr<IPooledRenderTarget> debug_candidate;
	TRefCountPtr<IPooledRenderTarget> debug_direction;
	TRefCountPtr<IPooledRenderTarget> debug_ray_budget;
	Callable pending_debug_callback;
	bool pending_debug_world_cache = false;

	mutable Mutex stats_mutex;
	Dictionary graph_stats;
};

VARIANT_ENUM_CAST(TGScreenGI::SamplingProfile)

/** Post-opaque companion of TGScreenGI (TGScreenGI::get_history_effect()). */
class TGScreenGIHistory : public CompositorEffect {
	GDCLASS(TGScreenGIHistory, CompositorEffect);

protected:
	static void _bind_methods() {}
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_owner_id(ObjectID p_owner) { owner = p_owner; }

	TGScreenGIHistory();

private:
	void _render_history(int p_effect_callback_type, const RenderData *p_render_data);

	ObjectID owner;
};

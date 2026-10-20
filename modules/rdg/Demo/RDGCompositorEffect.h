#pragma once

#include "RenderGraphResources.h"

#include "core/os/mutex.h"
#include "scene/resources/compositor.h"

class RenderData;

/** CompositorEffect whose post process is written exactly like UE renderer code: one FRDGBuilder per view,
 *  global shaders with parameter structs, RDG_EVENT_SCOPEs, transient textures, a history texture kept across
 *  frames with QueueTextureExtraction / RegisterExternalTexture, and a debug pass that RDG culls. */
class RDGCompositorEffect : public CompositorEffect {
	GDCLASS(RDGCompositorEffect, CompositorEffect);

public:
	enum DebugView {
		DEBUG_VIEW_NONE,
		DEBUG_VIEW_BLOOM,
		DEBUG_VIEW_BLOOM_SETUP,
	};

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_bloom_intensity(float p_value) { bloom_intensity = p_value; }
	float get_bloom_intensity() const { return bloom_intensity; }
	void set_bloom_threshold(float p_value) { bloom_threshold = p_value; }
	float get_bloom_threshold() const { return bloom_threshold; }
	void set_bloom_levels(int p_value) { bloom_levels = CLAMP(p_value, 1, 6); }
	int get_bloom_levels() const { return bloom_levels; }
	void set_trail_enabled(bool p_value) { trail_enabled = p_value; }
	bool get_trail_enabled() const { return trail_enabled; }
	void set_trail_weight(float p_value) { trail_weight = CLAMP(p_value, 0.0f, 0.98f); }
	float get_trail_weight() const { return trail_weight; }
	void set_debug_view(DebugView p_value) { debug_view = p_value; }
	DebugView get_debug_view() const { return debug_view; }
	void set_add_unused_debug_pass(bool p_value) { add_unused_debug_pass = p_value; }
	bool get_add_unused_debug_pass() const { return add_unused_debug_pass; }

	/** Stats of the last executed graph (passes, culled, textures, RD textures, ...). */
	Dictionary get_last_stats() const;

	RDGCompositorEffect();
	~RDGCompositorEffect();

private:
	void _render_rdg(int p_effect_callback_type, const RenderData *p_render_data);

	float bloom_intensity = 0.6f;
	float bloom_threshold = 1.0f;
	int bloom_levels = 5;
	bool trail_enabled = false;
	float trail_weight = 0.85f;
	DebugView debug_view = DEBUG_VIEW_NONE;
	bool add_unused_debug_pass = true;

	/** UE's view-state history (like FTemporalAAHistory): survives across graphs. */
	TRefCountPtr<IPooledRenderTarget> trail_history;

	mutable Mutex stats_mutex;
	Dictionary last_stats;
};

VARIANT_ENUM_CAST(RDGCompositorEffect::DebugView)

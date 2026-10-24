#pragma once

#include "scene/resources/compositor.h"

class RenderData;

/** Capture-independent native fog composition after the unfogged opaque pass: one RDG raster pass that blends
 *  Godot's volumetric fog froxels (RenderSceneBuffersRD::get_volumetric_fog_texture(), engine patch) onto the
 *  original (MSAA) color target, before the native sky and transparency. TGScreenGI then traces fog-free surface
 *  radiance while the image keeps the native fog look. */
class TGSurfaceFog : public CompositorEffect {
	GDCLASS(TGSurfaceFog, CompositorEffect);

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	/** The engine exposes the native fog volume to compositor effects (always true in the rdg-engine build). */
	static bool supported();
	void shutdown() { closed = true; }

	TGSurfaceFog();

private:
	void _render_fog(int p_effect_callback_type, const RenderData *p_render_data);

	bool closed = false;
};

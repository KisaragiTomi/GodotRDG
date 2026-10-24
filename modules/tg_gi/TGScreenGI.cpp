#include "TGScreenGI.h"

#include "RDGDeviceContext.h"
#include "RenderGraph.h"

#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/render_data.h"
#include "servers/rendering/storage/render_scene_data.h"

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Global shaders (GLSL in res://shaders/tg/TGScreenGI.glsl). All passes share one parameter struct; the AddPass
// utils clear the members a pass's shader does not declare (ClearUnusedGraphResources), so each pass only depends
// on what it really reads / writes.

BEGIN_UNIFORM_BUFFER_STRUCT(FTGViewUniformParameters, )
SHADER_PARAMETER(FMatrix44f, InvViewProj)
SHADER_PARAMETER(FMatrix44f, ViewProj)
SHADER_PARAMETER(FMatrix44f, CameraToWorld)
SHADER_PARAMETER(FMatrix44f, WorldToView)
SHADER_PARAMETER(FMatrix44f, PrevViewProj)
SHADER_PARAMETER(FMatrix44f, PrevInvViewProj)
SHADER_PARAMETER(FVector4f, SunDirection)
SHADER_PARAMETER(FVector4f, SunColor)
SHADER_PARAMETER(FVector4f, LampPosition)
SHADER_PARAMETER(FVector4f, LampColor)
SHADER_PARAMETER(FVector4f, BoxMin)
SHADER_PARAMETER(FVector4f, BoxMax)
SHADER_PARAMETER(FVector4f, BoxColor)
SHADER_PARAMETER(FVector4f, SkyRadiance)
END_UNIFORM_BUFFER_STRUCT()
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FTGViewUniformParameters, "TGView");

BEGIN_SHADER_PARAMETER_STRUCT(FTGScreenGIParameters, )
SHADER_PARAMETER(uint32, Frame)
SHADER_PARAMETER(uint32, Width)
SHADER_PARAMETER(uint32, Height)
SHADER_PARAMETER(uint32, HalfWidth)
SHADER_PARAMETER(uint32, HalfHeight)
SHADER_PARAMETER(uint32, NodeCount)
SHADER_PARAMETER(uint32, HistoryValid)
SHADER_PARAMETER(uint32, ScreenTrace)
SHADER_PARAMETER(uint32, MaxHistory)
SHADER_PARAMETER(uint32, StableDenoise)
SHADER_PARAMETER(uint32, ReactiveFrames)
SHADER_PARAMETER(uint32, TraceStride)
SHADER_PARAMETER(uint32, BaseRays)
SHADER_PARAMETER(uint32, FreshRays)
SHADER_PARAMETER(uint32, SamplingProfile)
SHADER_PARAMETER(uint32, PostDenoise)
SHADER_PARAMETER(uint32, AdaptivePriority)
SHADER_PARAMETER(uint32, SurfaceHistory)
SHADER_PARAMETER(uint32, RadianceTemporalOk)
SHADER_PARAMETER(uint32, WorldCacheEnabled)
SHADER_PARAMETER(uint32, WorldCacheEpoch)
SHADER_PARAMETER(uint32, WorldCacheTick)
SHADER_PARAMETER(uint32, WorldCacheWrite)
SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FTGViewUniformParameters, TGView)
// Scene inputs.
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, DepthTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, NormalTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorTextureSampler)
// Static geometry for the BVH fallback.
SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FNode>, Nodes)
SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FTriangle>, Triangles)
SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FMaterial>, Materials)
SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, AlbedoTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, AlbedoTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, MetalTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, MetalTextureSampler)
// Previous frame.
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevPositionTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, PrevPositionTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevNormalTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, PrevNormalTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevAgeTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, PrevAgeTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevMomentsTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, PrevMomentsTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevSHTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, PrevSHTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevColorTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, PrevColorTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevDepthTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, PrevDepthTextureSampler)
// This frame.
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, GuidePosition)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, GuideNormal)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RayBudget)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, CandidateColor)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, CandidateDirection)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, CandidateSH)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SHOutput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, AgeOutput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, MomentsOutput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, FilterInput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, FilterOutput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, VarianceInput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, VarianceOutput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, FilteredOutput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DisplayPosition)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, DisplayNormal)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ColorHistoryOutput)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthHistoryOutput)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FWorldCacheEntry>, WorldCache)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, WorldCacheOwners)
END_SHADER_PARAMETER_STRUCT()

#define TG_DECLARE_SHADER(ShaderClass)        \
	DECLARE_GLOBAL_SHADER(ShaderClass);       \
	using FParameters = FTGScreenGIParameters; \
	SHADER_USE_PARAMETER_STRUCT(ShaderClass, FGlobalShader);

class FTGGuideCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGGuideCS)
};
IMPLEMENT_GLOBAL_SHADER(FTGGuideCS, "/Project/tg/TGScreenGI.glsl", "TGGuideCS", SF_Compute);

class FTGBudgetCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGBudgetCS)
};
IMPLEMENT_GLOBAL_SHADER(FTGBudgetCS, "/Project/tg/TGScreenGI.glsl", "TGBudgetCS", SF_Compute);

class FTGTraceCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGTraceCS)
	class FAdaptive : SHADER_PERMUTATION_BOOL("TG_ADAPTIVE");
	class FWorldCache : SHADER_PERMUTATION_BOOL("TG_WORLD_CACHE");
	using FPermutationDomain = TShaderPermutationDomain<FAdaptive, FWorldCache>;
};
IMPLEMENT_GLOBAL_SHADER(FTGTraceCS, "/Project/tg/TGScreenGI.glsl", "TGTraceCS", SF_Compute);

class FTGProjectCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGProjectCS)
};
IMPLEMENT_GLOBAL_SHADER(FTGProjectCS, "/Project/tg/TGScreenGI.glsl", "TGProjectCS", SF_Compute);

class FTGReconstructCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGReconstructCS)
	class FSparse : SHADER_PERMUTATION_BOOL("TG_SPARSE");
	using FPermutationDomain = TShaderPermutationDomain<FSparse>;
};
IMPLEMENT_GLOBAL_SHADER(FTGReconstructCS, "/Project/tg/TGScreenGI.glsl", "TGReconstructCS", SF_Compute);

class FTGBasicFilterCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGBasicFilterCS)
};
IMPLEMENT_GLOBAL_SHADER(FTGBasicFilterCS, "/Project/tg/TGScreenGI.glsl", "TGBasicFilterCS", SF_Compute);

class FTGFilterCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGFilterCS)
	class FFilterPass : SHADER_PERMUTATION_SPARSE_INT("FILTER_PASS", 3, 6, 7, 8);
	using FPermutationDomain = TShaderPermutationDomain<FFilterPass>;
};
IMPLEMENT_GLOBAL_SHADER(FTGFilterCS, "/Project/tg/TGScreenGI.glsl", "TGFilterCS", SF_Compute);

class FTGHistoryCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGHistoryCS)
	class FSurfaceHistory : SHADER_PERMUTATION_BOOL("TG_SURFACE_HISTORY");
	using FPermutationDomain = TShaderPermutationDomain<FSurfaceHistory>;
};
IMPLEMENT_GLOBAL_SHADER(FTGHistoryCS, "/Project/tg/TGScreenGI.glsl", "TGHistoryCS", SF_Compute);

class FTGCacheSelectCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGCacheSelectCS)
};
IMPLEMENT_GLOBAL_SHADER(FTGCacheSelectCS, "/Project/tg/TGScreenGI.glsl", "TGCacheSelectCS", SF_Compute);

class FTGCacheResolveCS : public FGlobalShader {
	TG_DECLARE_SHADER(FTGCacheResolveCS)
};
IMPLEMENT_GLOBAL_SHADER(FTGCacheResolveCS, "/Project/tg/TGScreenGI.glsl", "TGCacheResolveCS", SF_Compute);

// GPU stats: "TG pass <name> begin / end" timestamps, the names the former GDScript effect captured.
DECLARE_GPU_STAT_NAMED(TGGuide, "TG pass guide");
DECLARE_GPU_STAT_NAMED(TGTrace, "TG pass trace");
DECLARE_GPU_STAT_NAMED(TGReconstruct, "TG pass reconstruct");
DECLARE_GPU_STAT_NAMED(TGFilter, "TG pass filter");
DECLARE_GPU_STAT_NAMED(TGHistory, "TG pass history");
DECLARE_GPU_STAT_NAMED(TGProject, "TG pass project");
DECLARE_GPU_STAT_NAMED(TGFilterMedium, "TG pass filter_medium");
DECLARE_GPU_STAT_NAMED(TGFilterWide, "TG pass filter_wide");
DECLARE_GPU_STAT_NAMED(TGFilterLarge, "TG pass filter_large");
DECLARE_GPU_STAT_NAMED(TGBudget, "TG pass budget");
DECLARE_GPU_STAT_NAMED(TGCacheSelect, "TG pass cache_select");
DECLARE_GPU_STAT_NAMED(TGCacheResolve, "TG pass cache_resolve");

static const char *const TGPassNames[] = { "guide", "trace", "reconstruct", "filter", "history", "project", "filter_medium", "filter_wide", "filter_large", "budget", "cache_select", "cache_resolve", "fog_composite" };
static constexpr uint32 TGCounterCount = 17;

namespace {

FMatrix44f ToMatrix(const Projection &P) {
	FMatrix44f Result;
	for (int32 Column = 0; Column < 4; Column++) {
		for (int32 Row = 0; Row < 4; Row++) Result.M[Column][Row] = (float)P.columns[Column][Row];
	}
	return Result;
}

FVector4f ToVector(const LocalVector<float> &Values, uint32 Offset) {
	return FVector4f(Values[Offset], Values[Offset + 1], Values[Offset + 2], Values[Offset + 3]);
}

FIntVector GroupCount8x8(const Vector2i &Size, int32 Layers = 1) {
	return FIntVector(FMath::DivideAndRoundUp(Size.x, 8), FMath::DivideAndRoundUp(Size.y, 8), Layers);
}

FRDGTextureDesc Desc2D(const Vector2i &Size, EPixelFormat Format) {
	return FRDGTextureDesc::Create2D(FIntPoint(Size.x, Size.y), Format, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
}

/** The previous frame's texture, or a cleared one when there is no compatible history (first frame, resize). */
FRDGTextureRef RegisterHistoryOrBlack(FRDGBuilder &GraphBuilder, const TRefCountPtr<IPooledRenderTarget> &History, const FRDGTextureDesc &Desc, const char *Name) {
	if (History.IsValid() && History->GetDesc() == Desc) return GraphBuilder.RegisterExternalTexture(History, Name);
	FRDGTextureRef Texture = GraphBuilder.CreateTexture(Desc, Name);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Texture), FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
	return Texture;
}

} //namespace

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TGScreenGI.

void TGScreenGI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_inputs", "lights", "box", "active", "screen_trace", "history_length", "sky"), &TGScreenGI::set_inputs, DEFVAL(true), DEFVAL(true), DEFVAL(48), DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("prepare", "size"), &TGScreenGI::prepare);
	ClassDB::bind_method(D_METHOD("read_debug", "callback", "include_world_cache"), &TGScreenGI::read_debug, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("reset_history", "world_cache"), &TGScreenGI::reset_history, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("shutdown"), &TGScreenGI::shutdown);
	ClassDB::bind_method(D_METHOD("set_sampling", "profile"), &TGScreenGI::set_sampling);
	ClassDB::bind_method(D_METHOD("get_sampling"), &TGScreenGI::get_sampling);
	ClassDB::bind_method(D_METHOD("set_post_denoise", "enabled"), &TGScreenGI::set_post_denoise);
	ClassDB::bind_method(D_METHOD("get_post_denoise"), &TGScreenGI::get_post_denoise);
	ClassDB::bind_method(D_METHOD("set_world_cache", "enabled"), &TGScreenGI::set_world_cache);
	ClassDB::bind_method(D_METHOD("get_world_cache"), &TGScreenGI::get_world_cache);
	ClassDB::bind_method(D_METHOD("set_stable_denoise", "enabled"), &TGScreenGI::set_stable_denoise);
	ClassDB::bind_method(D_METHOD("get_stable_denoise"), &TGScreenGI::get_stable_denoise);
	ClassDB::bind_method(D_METHOD("set_surface_history", "enabled"), &TGScreenGI::set_surface_history);
	ClassDB::bind_method(D_METHOD("get_surface_history"), &TGScreenGI::get_surface_history);
	ClassDB::bind_method(D_METHOD("set_adaptive_priority", "enabled"), &TGScreenGI::set_adaptive_priority);
	ClassDB::bind_method(D_METHOD("get_adaptive_priority"), &TGScreenGI::get_adaptive_priority);
	ClassDB::bind_method(D_METHOD("set_world_cache_updates", "enabled"), &TGScreenGI::set_world_cache_updates);
	ClassDB::bind_method(D_METHOD("get_world_cache_updates"), &TGScreenGI::get_world_cache_updates);
	ClassDB::bind_method(D_METHOD("set_debug_readback", "enabled"), &TGScreenGI::set_debug_readback);
	ClassDB::bind_method(D_METHOD("get_debug_readback"), &TGScreenGI::get_debug_readback);
	ClassDB::bind_method(D_METHOD("set_geometry_root", "root"), &TGScreenGI::set_geometry_root);
	ClassDB::bind_method(D_METHOD("get_geometry_root"), &TGScreenGI::get_geometry_root);
	ClassDB::bind_method(D_METHOD("set_frame_index", "frame"), &TGScreenGI::set_frame_index);
	ClassDB::bind_method(D_METHOD("get_frame_index"), &TGScreenGI::get_frame_index);
	ClassDB::bind_method(D_METHOD("get_stats"), &TGScreenGI::get_stats);
	ClassDB::bind_method(D_METHOD("get_sh_texture"), &TGScreenGI::get_sh_texture);
	ClassDB::bind_method(D_METHOD("get_position_texture"), &TGScreenGI::get_position_texture);
	ClassDB::bind_method(D_METHOD("get_normal_texture"), &TGScreenGI::get_normal_texture);
	ClassDB::bind_method(D_METHOD("get_history_effect"), &TGScreenGI::get_history_effect);
	ClassDB::bind_method(D_METHOD("get_graph_stats"), &TGScreenGI::get_graph_stats);
	ClassDB::bind_static_method("TGScreenGI", D_METHOD("get_default_geometry_root"), &TGScreenGI::get_default_geometry_root);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "sampling_profile", PROPERTY_HINT_ENUM, "Sparse,Quality,Reference,Adaptive"), "set_sampling", "get_sampling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "post_denoise"), "set_post_denoise", "get_post_denoise");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stable_denoise"), "set_stable_denoise", "get_stable_denoise");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "adaptive_priority"), "set_adaptive_priority", "get_adaptive_priority");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "surface_history"), "set_surface_history", "get_surface_history");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "world_cache"), "set_world_cache", "get_world_cache");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "world_cache_updates"), "set_world_cache_updates", "get_world_cache_updates");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_readback"), "set_debug_readback", "get_debug_readback");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "geometry_root", PROPERTY_HINT_DIR), "set_geometry_root", "get_geometry_root");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frame_index", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_frame_index", "get_frame_index");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "stats", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_stats");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sh_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2DRD", PROPERTY_USAGE_NONE), "", "get_sh_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "position_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2DRD", PROPERTY_USAGE_NONE), "", "get_position_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2DRD", PROPERTY_USAGE_NONE), "", "get_normal_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "history_effect", PROPERTY_HINT_RESOURCE_TYPE, "CompositorEffect", PROPERTY_USAGE_NONE), "", "get_history_effect");

	ADD_SIGNAL(MethodInfo("buffers_ready"));
	ADD_SIGNAL(MethodInfo("failed", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("resize_requested", PropertyInfo(Variant::VECTOR2I, "size")));

	ClassDB::bind_integer_constant(get_class_static(), StringName(), "WORLD_CACHE_SIZE", WorldCacheSize);
	BIND_ENUM_CONSTANT(SAMPLING_SPARSE);
	BIND_ENUM_CONSTANT(SAMPLING_QUALITY);
	BIND_ENUM_CONSTANT(SAMPLING_REFERENCE);
	BIND_ENUM_CONSTANT(SAMPLING_ADAPTIVE);
}

void TGScreenGI::_validate_property(PropertyInfo &p_property) const {
	// Pre-opaque: the materials of the opaque pass read this frame's GI. The callback type is fixed.
	if (p_property.name == "effect_callback_type") p_property.usage = PROPERTY_USAGE_NONE;
}

TGScreenGI::TGScreenGI() {
	geometry_root = get_default_geometry_root();
	for (const String &Arg : OS::get_singleton()->get_cmdline_user_args()) {
		if (Arg.begins_with("--tg-bvh-dir=")) geometry_root = Arg.trim_prefix("--tg-bvh-dir=").replace("\\", "/").trim_suffix("/") + "/";
		if (Arg == "--tg-uniform-budget") adaptive_priority = false;
	}
	sh_texture.instantiate();
	position_texture.instantiate();
	normal_texture.instantiate();
	history_effect.instantiate();
	history_effect->set_owner_id(get_instance_id());
	lights.resize(16);
	lights.fill(0.0f);
	box.resize(12);
	box.fill(0.0f);
	sky.resize(4);
	sky.fill(0.0f);
	stats["frames"] = 0;
	stats["history_resets"] = 0;
	stats["transport_builds"] = 0;
	stats["transfer_bytes"] = 0;
	stats["gpu_samples_ms"] = Array();
	set_needs_normal_roughness(true);
	set_access_resolved_depth(true);
	RenderingServer *RS = RenderingServer::get_singleton();
	if (RS) RS->compositor_effect_set_callback(get_rid(), RenderingServer::COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_OPAQUE, callable_mp(this, &TGScreenGI::_render_gi));
}

TGScreenGI::~TGScreenGI() {
	if (history_effect.is_valid()) history_effect->set_owner_id(ObjectID());
	RenderingServer *RS = RenderingServer::get_singleton();
	if (RS && RS->is_on_render_thread()) _cleanup();
}

Ref<CompositorEffect> TGScreenGI::get_history_effect() const {
	return history_effect;
}

Dictionary TGScreenGI::get_graph_stats() const {
	MutexLock Lock(stats_mutex);
	return graph_stats.duplicate();
}

void TGScreenGI::set_inputs(const PackedFloat32Array &p_lights, const PackedFloat32Array &p_box, bool p_active, bool p_screen_trace, int p_history_length, const Vector3 &p_sky) {
	const PackedFloat32Array Sky = { (float)p_sky.x, (float)p_sky.y, (float)p_sky.z, 0.0f };
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &TGScreenGI::_set_inputs_gpu).bind(p_lights.duplicate(), p_box.duplicate(), p_active, p_screen_trace, CLAMP(p_history_length, 1, 48), Sky));
}

void TGScreenGI::prepare(const Vector2i &p_size) {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &TGScreenGI::_prepare_gpu).bind(p_size));
}

void TGScreenGI::set_sampling(int p_profile) {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &TGScreenGI::_set_sampling_gpu).bind(CLAMP(p_profile, 0, 3)));
}

void TGScreenGI::set_post_denoise(bool p_enabled) {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &TGScreenGI::_set_post_denoise_gpu).bind(p_enabled));
}

void TGScreenGI::set_world_cache(bool p_enabled) {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &TGScreenGI::_set_world_cache_gpu).bind(p_enabled));
}

void TGScreenGI::set_stable_denoise(bool p_enabled) {
	stable_denoise = p_enabled;
}

void TGScreenGI::set_surface_history(bool p_enabled) {
	if (surface_history == p_enabled) return;
	surface_history = p_enabled;
	history_color.SafeRelease();
	history_valid = false;
	history_cooldown = 3;
}

void TGScreenGI::read_debug(const Callable &p_callback, bool p_include_world_cache) {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &TGScreenGI::_queue_debug_read).bind(p_callback, p_include_world_cache));
}

void TGScreenGI::shutdown() {
	closed = true;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &TGScreenGI::_cleanup));
}

void TGScreenGI::reset_history(bool p_world_cache) {
	frame = 0;
	history_valid = false;
	history_cooldown = 3;
	if (!p_world_cache) return;
	world_tick = 1;
	world_epoch = 1;
	world_cooldown = 3;
	clear_world_cache = true;
}

void TGScreenGI::_set_world_cache_gpu(bool p_enabled) {
	if (world_cache == p_enabled) return;
	world_cache = p_enabled;
	_invalidate_world_cache();
	history_valid = false;
	history_cooldown = 3;
	if (p_enabled && RD && !world_allocated) _allocate_world_cache(true);
}

void TGScreenGI::_invalidate_world_cache(int p_cooldown) {
	world_epoch = (world_epoch + 1) & 0x7fffffff;
	if (world_epoch == 0) {
		world_epoch = 1;
		clear_world_cache = true;
	}
	world_cooldown = p_cooldown;
}

void TGScreenGI::_allocate_world_cache(bool p_full) {
	const uint32 Count = p_full ? WorldCacheSize : 1;
	world_buffer = AllocatePooledBuffer(RD, FRDGBufferDesc::CreateStructuredDesc(64, Count), "TG.WorldCache");
	world_owners = AllocatePooledBuffer(RD, FRDGBufferDesc::CreateStructuredDesc(4, Count), "TG.WorldCacheOwners");
	world_allocated = p_full;
	// Entries start empty (epoch 0 never matches); owners start unclaimed (0xffffffff).
	clear_world_cache = true;
}

bool TGScreenGI::_world_cache_active() const {
	return world_cache && world_allocated && max_history > 1 && half_size.x * half_size.y < 0xffffff;
}

void TGScreenGI::_set_sampling_gpu(int p_profile) {
	if (p_profile == sampling_profile) return;
	sampling_profile = p_profile;
	if (RD) _ensure_size(size, true);
}

void TGScreenGI::_set_post_denoise_gpu(bool p_enabled) {
	post_denoise = p_enabled;
}

void TGScreenGI::_set_inputs_gpu(const PackedFloat32Array &p_lights, const PackedFloat32Array &p_box, bool p_active, bool p_screen_trace, int p_history_length, const PackedFloat32Array &p_sky) {
	if (p_lights != lights || p_box != box || p_active != active || p_screen_trace != screen_trace || p_history_length != max_history || p_sky != sky) {
		bool bSmallMotion = stable_denoise && p_box == box && p_active == active && p_screen_trace == screen_trace && p_history_length == max_history && p_sky == sky;
		bSmallMotion = bSmallMotion && p_lights.size() == 16 && lights.size() == 16 && p_lights.slice(0, 8) == lights.slice(0, 8) && p_lights.slice(11, 16) == lights.slice(11, 16);
		const float Displacement = bSmallMotion ? Vector3(p_lights[8] - lights[8], p_lights[9] - lights[9], p_lights[10] - lights[10]).length() : 0.0f;
		if (bSmallMotion && Displacement > 0.0f && Displacement < 0.35f) {
			// Keep valid geometry history while shortening accumulation near the
			// moving lamp. Remote static surfaces retain their converged result.
			reactive_frames = 8;
			feedback_cooldown = 3;
			// Do not extend the moving lamp's temporal lag into offscreen memory.
			_invalidate_world_cache(48);
		} else {
			history_valid = false;
			// Hard lighting changes must not reuse stale lit screen radiance.
			history_cooldown = 3;
			reactive_frames = 0;
			callable_mp(this, &TGScreenGI::_reset_report).call_deferred();
			_invalidate_world_cache();
		}
	}
	lights = p_lights;
	box = p_box;
	sky = p_sky;
	active = p_active;
	screen_trace = p_screen_trace;
	max_history = p_history_length;
}

void TGScreenGI::_prepare_gpu(const Vector2i &p_size) {
	if (!RD && !_initialize_gpu()) return;
	_ensure_size(p_size);
	requested_size = Vector2i();
}

bool TGScreenGI::_initialize_gpu() {
	RD = RenderingServer::get_singleton()->get_rendering_device();
	if (!RD) return false;
	const String Root = String("res://generated/sponza/");
	const Variant Metadata = JSON::parse_string(FileAccess::get_file_as_string(Root + "cache.json"));
	const Variant GeometryMetadata = JSON::parse_string(FileAccess::get_file_as_string(geometry_root + "cache.json"));
	if (Metadata.get_type() != Variant::DICTIONARY || GeometryMetadata.get_type() != Variant::DICTIONARY) {
		callable_mp(this, &TGScreenGI::_failure).call_deferred(String("BVH directory must contain acceleration data for the current Sponza geometry/materials: ") + geometry_root);
		RD = nullptr;
		return false;
	}
	const Dictionary MetadataDict = Metadata;
	const Dictionary GeometryDict = GeometryMetadata;
	const Array Materials = MetadataDict.get("materials", Array());
	if ((int64_t)GeometryDict.get("bvh_nodes", 0) <= 0 || Array(GeometryDict.get("materials", Array())) != Materials || GeometryDict.get("triangles", 0) != MetadataDict.get("triangles", -1)) {
		callable_mp(this, &TGScreenGI::_failure).call_deferred(String("BVH directory must contain acceleration data for the current Sponza geometry/materials: ") + geometry_root);
		RD = nullptr;
		return false;
	}
	node_count = (uint32)(int64_t)GeometryDict["bvh_nodes"];

	const Vector<uint8_t> Nodes = FileAccess::get_file_as_bytes(geometry_root + "bvh.bin");
	const Vector<uint8_t> Triangles = FileAccess::get_file_as_bytes(geometry_root + "triangles.bin");
	nodes_buffer = RD->storage_buffer_create(Nodes.size(), Nodes);
	triangles_buffer = RD->storage_buffer_create(Triangles.size(), Triangles);
	num_nodes_bytes = Nodes.size();
	num_triangles_bytes = Triangles.size();

	PackedFloat32Array MaterialValues;
	for (int32 Index = 0; Index < Materials.size(); Index++) {
		const Dictionary Material = Materials[Index];
		const Array Color = Material["color"];
		for (int32 Channel = 0; Channel < 4; Channel++) MaterialValues.push_back(Channel < Color.size() ? (float)(double)Color[Channel] : 0.0f);
		MaterialValues.push_back((float)(double)Material["alpha_cutoff"]);
		MaterialValues.push_back((float)(double)Material["metallic"]);
		MaterialValues.push_back((float)(double)Material["metal_channel"]);
		MaterialValues.push_back(0.0f);
	}
	Vector<uint8_t> MaterialBytes;
	MaterialBytes.resize(MaterialValues.size() * sizeof(float));
	memcpy(MaterialBytes.ptrw(), MaterialValues.ptr(), MaterialBytes.size());
	materials_buffer = RD->storage_buffer_create(MaterialBytes.size(), MaterialBytes);
	num_materials_bytes = MaterialBytes.size();

	auto LoadTextureArray = [&](const char *Prefix) {
		Vector<Vector<uint8_t>> Layers;
		for (int32 Index = 0; Index < Materials.size(); Index++) {
			Ref<Image> Layer;
			Layer.instantiate();
			Layer->load_png_from_buffer(FileAccess::get_file_as_bytes(Root + vformat("%s_%d.png", Prefix, Index)));
			Layer->convert(Image::FORMAT_RGBA8);
			Layers.push_back(Layer->get_data());
		}
		RenderingDevice::TextureFormat Format;
		Format.texture_type = RenderingDevice::TEXTURE_TYPE_2D_ARRAY;
		Format.width = 512;
		Format.height = 512;
		Format.array_layers = Materials.size();
		Format.format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
		Format.usage_bits = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
		const RID Texture = RD->texture_create(Format, RenderingDevice::TextureView(), Layers);
		RD->set_resource_name(Texture, vformat("TG.%sArray", Prefix));
		return Texture;
	};
	albedo_array = LoadTextureArray("albedo");
	metal_array = LoadTextureArray("metal");

	counter_buffer = AllocatePooledBuffer(RD, FRDGBufferDesc::CreateStructuredDesc(4, TGCounterCount), "TG.Counters");
	_allocate_world_cache(world_cache);
	return nodes_buffer.is_valid() && triangles_buffer.is_valid() && materials_buffer.is_valid() && albedo_array.is_valid() && metal_array.is_valid();
}

int TGScreenGI::_ray_layers() const {
	static const int Layers[] = { 1, 8, 32, 2 };
	return Layers[CLAMP(sampling_profile, 0, 3)];
}

void TGScreenGI::_ensure_size(const Vector2i &p_size, bool p_force) {
	if (p_size == size && !p_force) return;
	size = p_size;
	half_size = Vector2i((size.x + 1) / 2, (size.y + 1) / 2);
	const bool bSparse = sampling_profile == SAMPLING_SPARSE || sampling_profile == SAMPLING_ADAPTIVE;
	trace_size = bSparse ? Vector2i((half_size.x + 1) / 2, (half_size.y + 1) / 2) : half_size;

	// Replace the public texture views while their old backing textures are alive: detaching the views would
	// invalidate the materials' RenderingServer RIDs.
	const TRefCountPtr<IPooledRenderTarget> OldFiltered = filtered;
	const TRefCountPtr<IPooledRenderTarget> OldPosition = display_position;
	const TRefCountPtr<IPooledRenderTarget> OldNormal = display_normal;
	filtered = AllocatePooledTexture(RD, Desc2D(Vector2i(half_size.x * 3, half_size.y), PF_FloatRGBA), "TG.Filtered");
	display_position = AllocatePooledTexture(RD, Desc2D(half_size, PF_A32B32G32R32F), "TG.DisplayPosition");
	display_normal = AllocatePooledTexture(RD, Desc2D(half_size, PF_FloatRGBA), "TG.DisplayNormal");
	sh_texture->set_texture_rd_rid(filtered->GetRHI()->GetRID());
	position_texture->set_texture_rd_rid(display_position->GetRHI()->GetRID());
	normal_texture->set_texture_rd_rid(display_normal->GetRHI()->GetRID());
	clear_display = true;
	_release_size_dependent();
	history_valid = false;
	history_cooldown = 3;
	callable_mp(this, &TGScreenGI::_emit_buffers_ready).call_deferred();
}

void TGScreenGI::_release_size_dependent() {
	history_position.SafeRelease();
	history_normal.SafeRelease();
	history_sh.SafeRelease();
	history_age.SafeRelease();
	history_moments.SafeRelease();
	history_color.SafeRelease();
	history_depth.SafeRelease();
	debug_candidate.SafeRelease();
	debug_direction.SafeRelease();
	debug_ray_budget.SafeRelease();
}

void TGScreenGI::_cleanup() {
	if (!RD) return;
	sh_texture->set_texture_rd_rid(RID());
	position_texture->set_texture_rd_rid(RID());
	normal_texture->set_texture_rd_rid(RID());
	_release_size_dependent();
	filtered.SafeRelease();
	display_position.SafeRelease();
	display_normal.SafeRelease();
	counter_buffer.SafeRelease();
	world_buffer.SafeRelease();
	world_owners.SafeRelease();
	for (RID *Resource : { &nodes_buffer, &triangles_buffer, &materials_buffer, &albedo_array, &metal_array }) {
		if (Resource->is_valid()) RD->free_rid(*Resource);
		*Resource = RID();
	}
	world_allocated = false;
	RD = nullptr;
}

void TGScreenGI::_render_gi(int p_effect_callback_type, const RenderData *p_render_data) {
	rendered_frame = false;
	if (closed || !active || !p_render_data || p_effect_callback_type != EFFECT_CALLBACK_TYPE_PRE_OPAQUE) return;
	if (!RD && !_initialize_gpu()) return;
	Ref<RenderSceneBuffersRD> Buffers = p_render_data->get_render_scene_buffers();
	if (Buffers.is_null() || Buffers->get_view_count() != 1) return;
	const Vector2i InternalSize = Buffers->get_internal_size();
	if (InternalSize != size || !filtered.IsValid()) {
		history_valid = false;
		// Replacing Texture2DRD inside a draw invalidates material descriptor sets.
		// Resize from the main-thread notification, before the next render begins.
		if (requested_size != InternalSize) {
			requested_size = InternalSize;
			callable_mp(this, &TGScreenGI::_emit_resize_requested).call_deferred(InternalSize);
		}
		return;
	}
	_read_timing();

	const RenderSceneData *Scene = p_render_data->get_render_scene_data();
	const Transform3D Camera = Scene->get_cam_transform();
	const Projection CameraProjection = Scene->get_cam_projection();
	const Vector2 ProjectionScale(CameraProjection.columns[0][0], CameraProjection.columns[1][1]);
	if (!Camera.is_equal_approx(previous_camera) || !ProjectionScale.is_equal_approx(previous_projection_scale)) motion_frames = 3;
	else motion_frames = MAX(motion_frames - 1, 0);
	previous_camera = Camera;
	previous_projection_scale = ProjectionScale;
	current_vp = CameraProjection * Projection(Camera.affine_inverse());
	current_inverse = current_vp.inverse();

	view_constants.clear();
	for (const Projection &Matrix : { current_inverse, current_vp, Projection(Camera), Projection(Camera.affine_inverse()), previous_vp, previous_inverse }) {
		for (int32 Column = 0; Column < 4; Column++) {
			for (int32 Row = 0; Row < 4; Row++) view_constants.push_back((float)Matrix.columns[Column][Row]);
		}
	}
	for (float Value : lights) view_constants.push_back(Value);
	for (float Value : box) view_constants.push_back(Value);
	for (float Value : sky) view_constants.push_back(Value);

	const bool bSparse = sampling_profile == SAMPLING_SPARSE || sampling_profile == SAMPLING_ADAPTIVE;
	const bool bAdaptive = sampling_profile == SAMPLING_ADAPTIVE;
	const bool bWorldCache = _world_cache_active();
	const bool bWorldCacheUpdate = bWorldCache && world_cooldown == 0 && world_cache_updates;
	const int32 RayLayers = _ray_layers();
	const FIntPoint Half(half_size.x, half_size.y);
	const FIntPoint Trace(trace_size.x, trace_size.y);

	FRHICommandListImmediate &RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	FGlobalShaderMap *ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("TGScreenGI %dx%d", size.x, size.y));
	RDG_EVENT_SCOPE(GraphBuilder, "TGScreenGI");

	// Frame-wide inputs.
	FTGViewUniformParameters *ViewParameters = GraphBuilder.AllocParameters<FTGViewUniformParameters>();
	ViewParameters->InvViewProj = ToMatrix(current_inverse);
	ViewParameters->ViewProj = ToMatrix(current_vp);
	ViewParameters->CameraToWorld = ToMatrix(Projection(Camera));
	ViewParameters->WorldToView = ToMatrix(Projection(Camera.affine_inverse()));
	ViewParameters->PrevViewProj = ToMatrix(previous_vp);
	ViewParameters->PrevInvViewProj = ToMatrix(previous_inverse);
	ViewParameters->SunDirection = ToVector(view_constants, 96);
	ViewParameters->SunColor = ToVector(view_constants, 100);
	ViewParameters->LampPosition = ToVector(view_constants, 104);
	ViewParameters->LampColor = ToVector(view_constants, 108);
	ViewParameters->BoxMin = ToVector(view_constants, 112);
	ViewParameters->BoxMax = ToVector(view_constants, 116);
	ViewParameters->BoxColor = ToVector(view_constants, 120);
	ViewParameters->SkyRadiance = ToVector(view_constants, 124);
	TRDGUniformBufferRef<FTGViewUniformParameters> ViewUniformBuffer = GraphBuilder.CreateUniformBuffer(ViewParameters);

	FRDGTextureRef DepthTexture = RegisterExternalTexture(GraphBuilder, Buffers->get_depth_texture(0), "SceneDepth");
	FRDGTextureRef NormalTexture = RegisterExternalTexture(GraphBuilder, Buffers->get_texture_slice(SNAME("forward_clustered"), SNAME("normal_roughness"), 0, 0, 1, 1), "NormalRoughness");
	FRDGBufferRef Nodes = RegisterExternalBuffer(GraphBuilder, nodes_buffer, FRDGBufferDesc::CreateStructuredDesc(48, num_nodes_bytes / 48), "TG.BVHNodes");
	FRDGBufferRef Triangles = RegisterExternalBuffer(GraphBuilder, triangles_buffer, FRDGBufferDesc::CreateStructuredDesc(96, num_triangles_bytes / 96), "TG.Triangles");
	FRDGBufferRef Materials = RegisterExternalBuffer(GraphBuilder, materials_buffer, FRDGBufferDesc::CreateStructuredDesc(32, num_materials_bytes / 32), "TG.Materials");
	FRDGTextureRef AlbedoTexture = RegisterExternalTexture(GraphBuilder, albedo_array, "TG.AlbedoArray");
	FRDGTextureRef MetalTexture = RegisterExternalTexture(GraphBuilder, metal_array, "TG.MetalArray");
	FRDGBufferRef Counters = GraphBuilder.RegisterExternalBuffer(counter_buffer, "TG.Counters");
	FRDGBufferUAVRef CountersUAV = GraphBuilder.CreateUAV(Counters);
	AddClearUAVPass(GraphBuilder, CountersUAV, 0u);
	FRDGBufferRef WorldCache = GraphBuilder.RegisterExternalBuffer(world_buffer, "TG.WorldCache");
	FRDGBufferRef WorldOwners = GraphBuilder.RegisterExternalBuffer(world_owners, "TG.WorldCacheOwners");
	if (clear_world_cache) {
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(WorldCache), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(WorldOwners), 0xffffffffu);
		clear_world_cache = false;
	}

	// Previous frame. In Godot 4.6 Forward+, "taa/history" is last frame's resolved HDR TAA result until TAA runs
	// after this frame's opaque / transparent passes.
	taa_history_active = stable_denoise && !surface_history && Buffers->has_texture(SNAME("taa"), SNAME("history"));
	const FRDGTextureDesc PositionDesc = Desc2D(half_size, PF_A32B32G32R32F);
	const FRDGTextureDesc NormalDesc = Desc2D(half_size, PF_FloatRGBA);
	const FRDGTextureDesc SHDesc = Desc2D(Vector2i(half_size.x * 3, half_size.y), PF_FloatRGBA);
	const FRDGTextureDesc AgeDesc = Desc2D(half_size, PF_R16F);
	const FRDGTextureDesc MomentsDesc = Desc2D(half_size, PF_G32R32F);
	const FRDGTextureDesc VarianceDesc = Desc2D(half_size, PF_R32_FLOAT);
	const FRDGTextureDesc ColorHistoryDesc = Desc2D(size, PF_FloatRGBA);
	const FRDGTextureDesc DepthHistoryDesc = Desc2D(size, PF_R32_FLOAT);
	FRDGTextureRef PrevPosition = RegisterHistoryOrBlack(GraphBuilder, history_position, PositionDesc, "TG.History.Position");
	FRDGTextureRef PrevNormal = RegisterHistoryOrBlack(GraphBuilder, history_normal, NormalDesc, "TG.History.Normal");
	FRDGTextureRef PrevSH = RegisterHistoryOrBlack(GraphBuilder, history_sh, SHDesc, "TG.History.SH");
	FRDGTextureRef PrevAge = RegisterHistoryOrBlack(GraphBuilder, history_age, AgeDesc, "TG.History.Age");
	FRDGTextureRef PrevMoments = RegisterHistoryOrBlack(GraphBuilder, history_moments, MomentsDesc, "TG.History.Moments");
	FRDGTextureRef PrevColor = taa_history_active ? RegisterExternalTexture(GraphBuilder, Buffers->get_texture_slice(SNAME("taa"), SNAME("history"), 0, 0, 1, 1), "TAAHistory") : RegisterHistoryOrBlack(GraphBuilder, history_color, ColorHistoryDesc, "TG.History.Color");
	FRDGTextureRef PrevDepth = RegisterHistoryOrBlack(GraphBuilder, history_depth, DepthHistoryDesc, "TG.History.Depth");

	// This frame: the guides / SH / age / moments become next frame's history; everything else is transient.
	FRDGTextureRef GuidePosition = GraphBuilder.CreateTexture(PositionDesc, "TG.GuidePosition");
	FRDGTextureRef GuideNormal = GraphBuilder.CreateTexture(NormalDesc, "TG.GuideNormal");
	FRDGTextureRef SH = GraphBuilder.CreateTexture(SHDesc, "TG.SH");
	FRDGTextureRef Age = GraphBuilder.CreateTexture(AgeDesc, "TG.Age");
	FRDGTextureRef Moments = GraphBuilder.CreateTexture(MomentsDesc, "TG.Moments");
	FRDGTextureRef Candidate = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(Trace, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV, RayLayers), "TG.CandidateColor");
	FRDGTextureRef Direction = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(Trace, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV, RayLayers), "TG.CandidateDirection");
	FRDGTextureRef CandidateSH = bSparse ? nullptr : GraphBuilder.CreateTexture(Desc2D(Vector2i(trace_size.x * 3, trace_size.y), PF_FloatRGBA), "TG.CandidateSH");
	FRDGTextureRef RayBudget = bAdaptive ? GraphBuilder.CreateTexture(Desc2D(trace_size, PF_R8_UINT), "TG.RayBudget") : nullptr;
	FRDGTextureRef Filtered = GraphBuilder.RegisterExternalTexture(filtered, "TG.Filtered");
	FRDGTextureRef DisplayPosition = GraphBuilder.RegisterExternalTexture(display_position, "TG.DisplayPosition");
	FRDGTextureRef DisplayNormal = GraphBuilder.RegisterExternalTexture(display_normal, "TG.DisplayNormal");
	if (clear_display) {
		// The materials sample these before the first GI frame writes them.
		for (FRDGTextureRef Texture : { Filtered, DisplayPosition, DisplayNormal }) AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Texture), FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
		clear_display = false;
	}

	FRHISamplerState *PointSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	FRHISamplerState *RepeatSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Wrap, AM_Clamp>::GetRHI();
	const uint32 Rays = bSparse ? 1u : (sampling_profile == SAMPLING_REFERENCE ? 32u : (motion_frames > 0 ? 4u : 2u));
	FRDGTextureUAVRef GuidePositionUAV = GraphBuilder.CreateUAV(GuidePosition);
	FRDGTextureUAVRef GuideNormalUAV = GraphBuilder.CreateUAV(GuideNormal);

	// Every pass gets the same frame-wide bindings; the AddPass utils keep the ones its shader declares.
	auto CreateParameters = [&]() {
		FTGScreenGIParameters *Parameters = GraphBuilder.AllocParameters<FTGScreenGIParameters>();
		Parameters->Frame = (uint32)frame;
		Parameters->Width = size.x;
		Parameters->Height = size.y;
		Parameters->HalfWidth = half_size.x;
		Parameters->HalfHeight = half_size.y;
		Parameters->NodeCount = node_count;
		Parameters->HistoryValid = history_valid ? 1 : 0;
		Parameters->ScreenTrace = screen_trace && feedback_cooldown == 0 ? 1 : 0;
		Parameters->MaxHistory = max_history;
		Parameters->StableDenoise = stable_denoise ? 1 : 0;
		Parameters->ReactiveFrames = reactive_frames;
		Parameters->TraceStride = bSparse ? 2 : 1;
		Parameters->BaseRays = Rays;
		Parameters->FreshRays = 8;
		Parameters->SamplingProfile = sampling_profile;
		Parameters->PostDenoise = post_denoise ? 1 : 0;
		Parameters->AdaptivePriority = adaptive_priority ? 1 : 0;
		Parameters->SurfaceHistory = surface_history ? 1 : 0;
		Parameters->RadianceTemporalOk = feedback_cooldown == 0 ? 1 : 0;
		Parameters->WorldCacheEnabled = bWorldCache ? 1 : 0;
		Parameters->WorldCacheEpoch = world_epoch;
		Parameters->WorldCacheTick = world_tick;
		Parameters->WorldCacheWrite = world_cache_updates && world_cooldown == 0 ? 1 : 0;
		Parameters->TGView = ViewUniformBuffer;
		Parameters->DepthTexture = DepthTexture;
		Parameters->DepthTextureSampler = PointSampler;
		Parameters->NormalTexture = NormalTexture;
		Parameters->NormalTextureSampler = PointSampler;
		Parameters->Nodes = GraphBuilder.CreateSRV(Nodes);
		Parameters->Triangles = GraphBuilder.CreateSRV(Triangles);
		Parameters->Materials = GraphBuilder.CreateSRV(Materials);
		Parameters->AlbedoTexture = AlbedoTexture;
		Parameters->AlbedoTextureSampler = RepeatSampler;
		Parameters->MetalTexture = MetalTexture;
		Parameters->MetalTextureSampler = RepeatSampler;
		Parameters->PrevPositionTexture = PrevPosition;
		Parameters->PrevPositionTextureSampler = PointSampler;
		Parameters->PrevNormalTexture = PrevNormal;
		Parameters->PrevNormalTextureSampler = PointSampler;
		Parameters->PrevAgeTexture = PrevAge;
		Parameters->PrevAgeTextureSampler = PointSampler;
		Parameters->PrevMomentsTexture = PrevMoments;
		Parameters->PrevMomentsTextureSampler = PointSampler;
		Parameters->PrevSHTexture = PrevSH;
		Parameters->PrevSHTextureSampler = PointSampler;
		Parameters->PrevColorTexture = PrevColor;
		Parameters->PrevColorTextureSampler = PointSampler;
		Parameters->PrevDepthTexture = PrevDepth;
		Parameters->PrevDepthTextureSampler = PointSampler;
		Parameters->GuidePosition = GuidePositionUAV;
		Parameters->GuideNormal = GuideNormalUAV;
		Parameters->Counters = CountersUAV;
		return Parameters;
	};

	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, TGGuide);
		TShaderMapRef<FTGGuideCS> ComputeShader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Guide %dx%d", half_size.x, half_size.y), ComputeShader, CreateParameters(), GroupCount8x8(half_size));
	}
	if (bAdaptive) {
		RDG_GPU_STAT_SCOPE(GraphBuilder, TGBudget);
		TShaderMapRef<FTGBudgetCS> ComputeShader(ShaderMap);
		FTGScreenGIParameters *Parameters = CreateParameters();
		Parameters->RayBudget = GraphBuilder.CreateUAV(RayBudget);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Budget %dx%d", trace_size.x, trace_size.y), ComputeShader, Parameters, GroupCount8x8(trace_size));
	}
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, TGTrace);
		FTGTraceCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FTGTraceCS::FAdaptive>(bAdaptive);
		PermutationVector.Set<FTGTraceCS::FWorldCache>(bWorldCache);
		TShaderMapRef<FTGTraceCS> ComputeShader(ShaderMap, PermutationVector);
		FTGScreenGIParameters *Parameters = CreateParameters();
		if (RayBudget) Parameters->RayBudget = GraphBuilder.CreateUAV(RayBudget);
		Parameters->CandidateColor = GraphBuilder.CreateUAV(Candidate);
		Parameters->CandidateDirection = GraphBuilder.CreateUAV(Direction);
		Parameters->WorldCache = GraphBuilder.CreateUAV(WorldCache);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Trace %dx%dx%d", trace_size.x, trace_size.y, RayLayers), ComputeShader, Parameters, GroupCount8x8(trace_size, RayLayers));
	}
	if (!bSparse) {
		RDG_GPU_STAT_SCOPE(GraphBuilder, TGProject);
		TShaderMapRef<FTGProjectCS> ComputeShader(ShaderMap);
		FTGScreenGIParameters *Parameters = CreateParameters();
		Parameters->CandidateColor = GraphBuilder.CreateUAV(Candidate);
		Parameters->CandidateDirection = GraphBuilder.CreateUAV(Direction);
		Parameters->CandidateSH = GraphBuilder.CreateUAV(CandidateSH);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Project %dx%d", trace_size.x, trace_size.y), ComputeShader, Parameters, GroupCount8x8(trace_size));
	}
	FRDGTextureUAVRef SHUAV = GraphBuilder.CreateUAV(SH);
	FRDGTextureUAVRef AgeUAV = GraphBuilder.CreateUAV(Age);
	FRDGTextureUAVRef MomentsUAV = GraphBuilder.CreateUAV(Moments);
	FRDGTextureUAVRef FilteredUAV = GraphBuilder.CreateUAV(Filtered);
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, TGReconstruct);
		FTGReconstructCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FTGReconstructCS::FSparse>(bSparse);
		TShaderMapRef<FTGReconstructCS> ComputeShader(ShaderMap, PermutationVector);
		FTGScreenGIParameters *Parameters = CreateParameters();
		Parameters->CandidateColor = GraphBuilder.CreateUAV(Candidate);
		Parameters->CandidateDirection = GraphBuilder.CreateUAV(Direction);
		if (CandidateSH) Parameters->CandidateSH = GraphBuilder.CreateUAV(CandidateSH);
		Parameters->SHOutput = SHUAV;
		Parameters->AgeOutput = AgeUAV;
		Parameters->MomentsOutput = MomentsUAV;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Reconstruct %dx%d", half_size.x, half_size.y), ComputeShader, Parameters, GroupCount8x8(half_size));
	}
	if (post_denoise) {
		// Four variance guided levels: SH -> Fine -> Medium -> Wide -> Filtered (strides 1, 2, 4, 8).
		struct FLevel {
			int32 Pass;
			const FRDGGPUStat *Stat;
			const char *Name;
		};
		static const FLevel Levels[] = { { 3, &GPUStat_TGFilter, "Fine" }, { 6, &GPUStat_TGFilterMedium, "Medium" }, { 7, &GPUStat_TGFilterWide, "Wide" }, { 8, &GPUStat_TGFilterLarge, "Large" } };
		FRDGTextureRef Input = SH;
		FRDGTextureRef Variance = nullptr;
		for (const FLevel &Level : Levels) {
			FRDGGPUStatScopeGuard StatScope(GraphBuilder, *Level.Stat);
			FTGFilterCS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FTGFilterCS::FFilterPass>(Level.Pass);
			TShaderMapRef<FTGFilterCS> ComputeShader(ShaderMap, PermutationVector);
			FTGScreenGIParameters *Parameters = CreateParameters();
			const bool bLast = Level.Pass == 8;
			FRDGTextureRef Output = bLast ? Filtered : GraphBuilder.CreateTexture(SHDesc, "TG.Filter");
			FRDGTextureRef VarianceOutput = bLast ? nullptr : GraphBuilder.CreateTexture(VarianceDesc, "TG.FilterVariance");
			Parameters->FilterInput = Input == SH ? SHUAV : GraphBuilder.CreateUAV(Input);
			Parameters->FilterOutput = bLast ? FilteredUAV : GraphBuilder.CreateUAV(Output);
			if (Variance) Parameters->VarianceInput = GraphBuilder.CreateUAV(Variance);
			if (VarianceOutput) Parameters->VarianceOutput = GraphBuilder.CreateUAV(VarianceOutput);
			Parameters->AgeOutput = AgeUAV;
			Parameters->MomentsOutput = MomentsUAV;
			Parameters->DisplayPosition = GraphBuilder.CreateUAV(DisplayPosition);
			Parameters->DisplayNormal = GraphBuilder.CreateUAV(DisplayNormal);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Filter%s %dx%d", Level.Name, half_size.x, half_size.y), ComputeShader, Parameters, GroupCount8x8(half_size));
			Input = Output;
			Variance = VarianceOutput;
		}
	} else {
		RDG_GPU_STAT_SCOPE(GraphBuilder, TGFilter);
		TShaderMapRef<FTGBasicFilterCS> ComputeShader(ShaderMap);
		FTGScreenGIParameters *Parameters = CreateParameters();
		Parameters->SHOutput = SHUAV;
		Parameters->AgeOutput = AgeUAV;
		Parameters->FilteredOutput = FilteredUAV;
		Parameters->DisplayPosition = GraphBuilder.CreateUAV(DisplayPosition);
		Parameters->DisplayNormal = GraphBuilder.CreateUAV(DisplayNormal);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BasicFilter %dx%d", half_size.x, half_size.y), ComputeShader, Parameters, GroupCount8x8(half_size));
	}
	if (bWorldCacheUpdate) {
		{
			RDG_GPU_STAT_SCOPE(GraphBuilder, TGCacheSelect);
			TShaderMapRef<FTGCacheSelectCS> ComputeShader(ShaderMap);
			FTGScreenGIParameters *Parameters = CreateParameters();
			Parameters->AgeOutput = AgeUAV;
			Parameters->WorldCacheOwners = GraphBuilder.CreateUAV(WorldOwners);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("WorldCacheSelect %dx%d", half_size.x, half_size.y), ComputeShader, Parameters, GroupCount8x8(half_size));
		}
		{
			RDG_GPU_STAT_SCOPE(GraphBuilder, TGCacheResolve);
			TShaderMapRef<FTGCacheResolveCS> ComputeShader(ShaderMap);
			FTGScreenGIParameters *Parameters = CreateParameters();
			Parameters->FilteredOutput = FilteredUAV;
			Parameters->WorldCache = GraphBuilder.CreateUAV(WorldCache);
			Parameters->WorldCacheOwners = GraphBuilder.CreateUAV(WorldOwners);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("WorldCacheResolve"), ComputeShader, Parameters, GroupCount8x8(Vector2i(256, 256)));
		}
	}

	GraphBuilder.QueueTextureExtraction(GuidePosition, &history_position);
	GraphBuilder.QueueTextureExtraction(GuideNormal, &history_normal);
	GraphBuilder.QueueTextureExtraction(SH, &history_sh);
	GraphBuilder.QueueTextureExtraction(Age, &history_age);
	GraphBuilder.QueueTextureExtraction(Moments, &history_moments);
	if (extract_debug || debug_readback) {
		GraphBuilder.QueueTextureExtraction(Candidate, &debug_candidate);
		GraphBuilder.QueueTextureExtraction(Direction, &debug_direction);
		if (RayBudget) GraphBuilder.QueueTextureExtraction(RayBudget, &debug_ray_budget);
		else debug_ray_budget.SafeRelease();
		extract_debug = false;
	}

	RD->capture_timestamp("TG GI begin");
	GraphBuilder.Execute();
	RD->capture_timestamp("TG GI end");
	_collect_graph_stats(GraphBuilder);
	rendered_frame = true;
	callable_mp(this, &TGScreenGI::_frame_report).call_deferred(frame + 1, size);
}

void TGScreenGI::capture_history(const RenderData *p_render_data) {
	if (closed || !active || !rendered_frame || !RD || !p_render_data || view_constants.size() < 128) return;
	Ref<RenderSceneBuffersRD> Buffers = p_render_data->get_render_scene_buffers();
	if (Buffers.is_null() || Buffers->get_internal_size() != size) return;

	FRHICommandListImmediate &RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	FGlobalShaderMap *ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("TGScreenGI History %dx%d", size.x, size.y));
	{
		RDG_EVENT_SCOPE(GraphBuilder, "TGScreenGI");
		RDG_GPU_STAT_SCOPE(GraphBuilder, TGHistory);
		FTGViewUniformParameters *ViewParameters = GraphBuilder.AllocParameters<FTGViewUniformParameters>();
		FMatrix44f *Matrices[] = { &ViewParameters->InvViewProj, &ViewParameters->ViewProj, &ViewParameters->CameraToWorld, &ViewParameters->WorldToView, &ViewParameters->PrevViewProj, &ViewParameters->PrevInvViewProj };
		for (int32 Index = 0; Index < 6; Index++) memcpy(Matrices[Index]->M, &view_constants[Index * 16], sizeof(float) * 16);
		FVector4f *Vectors[] = { &ViewParameters->SunDirection, &ViewParameters->SunColor, &ViewParameters->LampPosition, &ViewParameters->LampColor, &ViewParameters->BoxMin, &ViewParameters->BoxMax, &ViewParameters->BoxColor, &ViewParameters->SkyRadiance };
		for (int32 Index = 0; Index < 8; Index++) *Vectors[Index] = ToVector(view_constants, 96 + Index * 4);

		const FRDGTextureDesc ColorHistoryDesc = Desc2D(size, PF_FloatRGBA);
		const FRDGTextureDesc DepthHistoryDesc = Desc2D(size, PF_R32_FLOAT);
		FRDGTextureRef ColorHistory = GraphBuilder.CreateTexture(ColorHistoryDesc, "TG.History.Color");
		FRDGTextureRef DepthHistory = GraphBuilder.CreateTexture(DepthHistoryDesc, "TG.History.Depth");

		FTGHistoryCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FTGHistoryCS::FSurfaceHistory>(surface_history);
		TShaderMapRef<FTGHistoryCS> ComputeShader(ShaderMap, PermutationVector);
		FRHISamplerState *PointSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		FTGScreenGIParameters *Parameters = GraphBuilder.AllocParameters<FTGScreenGIParameters>();
		Parameters->Width = size.x;
		Parameters->Height = size.y;
		Parameters->HalfWidth = half_size.x;
		Parameters->HalfHeight = half_size.y;
		Parameters->HistoryValid = history_valid ? 1 : 0;
		Parameters->StableDenoise = stable_denoise ? 1 : 0;
		Parameters->SurfaceHistory = surface_history ? 1 : 0;
		Parameters->RadianceTemporalOk = feedback_cooldown == 0 ? 1 : 0;
		Parameters->TGView = GraphBuilder.CreateUniformBuffer(ViewParameters);
		Parameters->DepthTexture = RegisterExternalTexture(GraphBuilder, Buffers->get_depth_texture(0), "SceneDepth");
		Parameters->DepthTextureSampler = PointSampler;
		Parameters->SceneColorTexture = RegisterExternalTexture(GraphBuilder, Buffers->get_internal_texture(0), "SceneColor");
		Parameters->SceneColorTextureSampler = PointSampler;
		if (surface_history) {
			Parameters->PrevColorTexture = RegisterHistoryOrBlack(GraphBuilder, history_color, ColorHistoryDesc, "TG.History.Color");
			Parameters->PrevColorTextureSampler = PointSampler;
			Parameters->PrevDepthTexture = RegisterHistoryOrBlack(GraphBuilder, history_depth, DepthHistoryDesc, "TG.History.Depth");
			Parameters->PrevDepthTextureSampler = PointSampler;
		}
		Parameters->ColorHistoryOutput = GraphBuilder.CreateUAV(ColorHistory);
		Parameters->DepthHistoryOutput = GraphBuilder.CreateUAV(DepthHistory);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("History %dx%d", size.x, size.y), ComputeShader, Parameters, GroupCount8x8(size));
		GraphBuilder.QueueTextureExtraction(ColorHistory, &history_color);
		GraphBuilder.QueueTextureExtraction(DepthHistory, &history_depth);
	}
	GraphBuilder.Execute();

	previous_vp = current_vp;
	previous_inverse = current_inverse;
	history_cooldown = MAX(history_cooldown - 1, 0);
	history_valid = history_cooldown == 0;
	feedback_cooldown = MAX(feedback_cooldown - 1, 0);
	reactive_frames = MAX(reactive_frames - 1, 0);
	world_cooldown = MAX(world_cooldown - 1, 0);
	world_tick = (world_tick + 1) & 0x7fffffff;
	frame++;
	if (pending_debug_callback.is_valid() && !extract_debug) {
		// The extracted transients of this frame are complete once the frame is submitted: read them from the main
		// thread's next RenderingServer sync, like the persistent textures.
		callable_mp(this, &TGScreenGI::_queue_debug_read).call_deferred(pending_debug_callback, pending_debug_world_cache);
		pending_debug_callback = Callable();
	}
}

void TGScreenGI::_queue_debug_read(const Callable &p_callback, bool p_include_world_cache) {
	RenderingServer *RS = RenderingServer::get_singleton();
	if (!RS->is_on_render_thread()) {
		RS->call_on_render_thread(callable_mp(this, &TGScreenGI::_queue_debug_read).bind(p_callback, p_include_world_cache));
		return;
	}
	// The candidates are transient: extract them from the next frame, then read (see capture_history()).
	if (!debug_readback && !debug_candidate.IsValid() && active && !closed && !pending_debug_callback.is_valid()) {
		extract_debug = true;
		pending_debug_callback = p_callback;
		pending_debug_world_cache = p_include_world_cache;
		return;
	}
	_read_debug_gpu(p_callback, p_include_world_cache);
}

void TGScreenGI::_read_debug_gpu(const Callable &p_callback, bool p_include_world_cache) {
	if (!RD || !filtered.IsValid()) return;
	auto ReadImage = [&](const TRefCountPtr<IPooledRenderTarget> &Texture, Image::Format Format) {
		if (!Texture.IsValid()) return Ref<Image>(memnew(Image));
		const FRHITextureDesc &Desc = Texture->GetDesc();
		return Image::create_from_data(Desc.Extent.X, Desc.Extent.Y, false, Format, RD->texture_get_data(Texture->GetRHI()->GetRID(), 0));
	};
	Array Counters;
	const Vector<uint8_t> CounterBytes = RD->buffer_get_data(counter_buffer->GetRHI()->GetRID());
	for (int32 Index = 0; Index + 4 <= CounterBytes.size(); Index += 4) Counters.push_back(*reinterpret_cast<const int32 *>(CounterBytes.ptr() + Index));
	Dictionary Result;
	Result["counters"] = Counters;
	Result["sh"] = ReadImage(filtered, Image::FORMAT_RGBAH);
	Result["age"] = ReadImage(history_age, Image::FORMAT_RH);
	Result["position"] = ReadImage(display_position, Image::FORMAT_RGBAF);
	Result["normal"] = ReadImage(display_normal, Image::FORMAT_RGBAH);
	Result["candidate"] = ReadImage(debug_candidate, Image::FORMAT_RGBAH);
	Result["direction"] = ReadImage(debug_direction, Image::FORMAT_RGBAH);
	Result["radiance"] = ReadImage(history_color, Image::FORMAT_RGBAH);
	Result["surface_history"] = surface_history;
	Result["ray_budget"] = sampling_profile == SAMPLING_ADAPTIVE && debug_ray_budget.IsValid() ? RD->texture_get_data(debug_ray_budget->GetRHI()->GetRID(), 0) : Vector<uint8_t>();
	Result["trace_size"] = trace_size;
	Result["adaptive_priority"] = adaptive_priority;
	Result["lights"] = Variant(lights).operator Array();
	Result["sky"] = Variant(sky).operator Array();
	Result["history_valid"] = history_valid;
	Result["cooldown"] = history_cooldown;
	Result["frame"] = frame;
	Result["taa_history"] = taa_history_active;
	Result["sampling_profile"] = sampling_profile;
	Result["camera_moving"] = motion_frames > 0;
	Result["world_cache"] = world_cache;
	Result["world_epoch"] = world_epoch;
	Result["world_tick"] = world_tick;
	Result["world_cooldown"] = world_cooldown;
	Result["world_entries"] = p_include_world_cache && world_buffer.IsValid() ? RD->buffer_get_data(world_buffer->GetRHI()->GetRID()) : Vector<uint8_t>();
	if (!debug_readback) {
		debug_candidate.SafeRelease();
		debug_direction.SafeRelease();
		debug_ray_budget.SafeRelease();
	}
	p_callback.call_deferred(Result);
}

void TGScreenGI::_read_timing() {
	const uint64 TimestampFrame = RD->get_captured_timestamps_frame();
	if (TimestampFrame == last_timing_frame) return;
	last_timing_frame = TimestampFrame;
	HashMap<String, uint64> Timestamps;
	for (uint32 Index = 0; Index < RD->get_captured_timestamps_count(); Index++) {
		const String Name = RD->get_captured_timestamp_name(Index);
		if (Name.begins_with("TG ")) Timestamps[Name] = RD->get_captured_timestamp_gpu_time(Index);
	}
	if (!Timestamps.has("TG GI begin") || !Timestamps.has("TG GI end")) return;
	const double Ms = (double)(Timestamps["TG GI end"] - Timestamps["TG GI begin"]) / 1000000.0;
	Dictionary Stages;
	for (const char *PassName : TGPassNames) {
		const String Begin = vformat("TG pass %s begin", PassName);
		const String End = vformat("TG pass %s end", PassName);
		if (Timestamps.has(Begin) && Timestamps.has(End)) Stages[PassName] = (double)(Timestamps[End] - Timestamps[Begin]) / 1000000.0;
	}
	// History copy is after opaque rendering; exclude the intervening draw time.
	Stages["total"] = Ms + (double)Stages.get("history", 0.0);
	callable_mp(this, &TGScreenGI::_timing_report).call_deferred(Ms, Stages, (int)TimestampFrame);
}

void TGScreenGI::_collect_graph_stats(const FRDGBuilder &GraphBuilder) {
	const FRDGBuilderStats &Stats = GraphBuilder.GetStats();
	Dictionary Result;
	Result["passes"] = Stats.NumPasses;
	Result["culled_passes"] = Stats.NumCulledPasses;
	Result["textures"] = Stats.NumTextures;
	Result["transient_textures"] = Stats.NumTransientTextures;
	Result["physical_textures"] = Stats.NumPhysicalTransientTextures;
	Result["buffers"] = Stats.NumBuffers;
	Result["placed_textures"] = Stats.NumPlacedTextures;
	Result["transient_allocated_mb"] = (double)Stats.TransientAllocatedBytes / (1024.0 * 1024.0);
	Result["transient_peak_mb"] = (double)Stats.TransientPeakBytes / (1024.0 * 1024.0);
	Result["extracted_textures"] = Stats.NumExtractedTextures;
	Result["binding_errors"] = Stats.NumBindingErrors;
	Result["validation_errors"] = Stats.NumValidationErrors;
	MutexLock Lock(stats_mutex);
	graph_stats = Result;
}

void TGScreenGI::_reset_report() {
	stats["history_resets"] = (int)stats["history_resets"] + 1;
}

void TGScreenGI::_timing_report(double p_ms, const Dictionary &p_stages, int p_frame) {
	Array Samples = stats["gpu_samples_ms"];
	Samples.push_back(p_ms);
	if (Samples.size() > 180) Samples.pop_front();
	// A skipped pass must immediately show as skipped, not retain old timings.
	if (!stage_timing_window.is_empty()) {
		const Dictionary &Last = stage_timing_window[stage_timing_window.size() - 1];
		for (const char *Key : { "project", "budget", "filter_wide", "cache_resolve" }) {
			if (Last.has(Key) != p_stages.has(Key)) {
				stage_timing_window.clear();
				break;
			}
		}
	}
	stage_timing_window.push_back(p_stages);
	if (stage_timing_window.size() > 16) stage_timing_window.remove_at(0);
	Dictionary Averaged;
	for (const Variant &Key : p_stages.keys()) {
		double Sum = 0.0;
		for (const Dictionary &Sample : stage_timing_window) Sum += (double)Sample.get(Key, 0.0);
		Averaged[Key] = Sum / stage_timing_window.size();
	}
	stats["stage_gpu_ms"] = Averaged;
	stats["timing_frame"] = p_frame;
	stats["timing_samples"] = stage_timing_window.size();
}

void TGScreenGI::_frame_report(int p_frame, const Vector2i &p_size) {
	stats["frames"] = p_frame;
	Array Resolution;
	Resolution.push_back(p_size.x);
	Resolution.push_back(p_size.y);
	stats["resolution"] = Resolution;
}

void TGScreenGI::_failure(const String &p_message) {
	ERR_PRINT(p_message);
	emit_signal(SNAME("failed"), p_message);
}

void TGScreenGI::_emit_buffers_ready() {
	emit_signal(SNAME("buffers_ready"));
}

void TGScreenGI::_emit_resize_requested(const Vector2i &p_size) {
	emit_signal(SNAME("resize_requested"), p_size);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TGScreenGIHistory.

void TGScreenGIHistory::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "effect_callback_type") p_property.usage = PROPERTY_USAGE_NONE;
}

TGScreenGIHistory::TGScreenGIHistory() {
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	RenderingServer *RS = RenderingServer::get_singleton();
	if (RS) RS->compositor_effect_set_callback(get_rid(), RenderingServer::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE, callable_mp(this, &TGScreenGIHistory::_render_history));
}

void TGScreenGIHistory::_render_history(int p_effect_callback_type, const RenderData *p_render_data) {
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_OPAQUE) return;
	TGScreenGI *Owner = Object::cast_to<TGScreenGI>(ObjectDB::get_instance(owner));
	if (Owner) Owner->capture_history(p_render_data);
}

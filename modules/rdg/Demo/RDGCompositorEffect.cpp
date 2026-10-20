#include "RDGCompositorEffect.h"

#include "PixelShaderUtils.h"
#include "RDGDeviceContext.h"
#include "RenderGraph.h"

#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/render_data.h"

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Global shaders (GLSL in res://shaders/rdg_demo/RDGPostProcess.glsl, addressed as /Project/... like UE).

class FRDGDemoBloomSetupCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FRDGDemoBloomSetupCS);
	SHADER_USE_PARAMETER_STRUCT(FRDGDemoBloomSetupCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FVector2f, OutputInvSize)
	SHADER_PARAMETER(float, BloomThreshold)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FRDGDemoBloomSetupCS, "/Project/rdg_demo/RDGPostProcess.glsl", "BloomSetupCS", SF_Compute);

class FRDGDemoBloomDownsampleCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FRDGDemoBloomDownsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FRDGDemoBloomDownsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FVector2f, OutputInvSize)
	SHADER_PARAMETER(FVector2f, InputInvSize)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FRDGDemoBloomDownsampleCS, "/Project/rdg_demo/RDGPostProcess.glsl", "BloomDownsampleCS", SF_Compute);

class FRDGDemoBloomUpsamplePS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FRDGDemoBloomUpsamplePS);
	SHADER_USE_PARAMETER_STRUCT(FRDGDemoBloomUpsamplePS, FGlobalShader);

	class FHasPrevious : SHADER_PERMUTATION_BOOL("HAS_PREVIOUS");
	using FPermutationDomain = TShaderPermutationDomain<FHasPrevious>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FVector2f, PreviousInvSize)
	SHADER_PARAMETER(float, Radius)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BloomDownTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PreviousUpTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, BloomSampler)
	RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FRDGDemoBloomUpsamplePS, "/Project/rdg_demo/RDGPostProcess.glsl", "BloomUpsamplePS", SF_Pixel);

class FRDGDemoTrailCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FRDGDemoTrailCS);
	SHADER_USE_PARAMETER_STRUCT(FRDGDemoTrailCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(float, HistoryWeight)
	SHADER_PARAMETER(int32, bHasHistory)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HistoryTexture)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FRDGDemoTrailCS, "/Project/rdg_demo/RDGPostProcess.glsl", "TrailCS", SF_Compute);

class FRDGDemoCompositeCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FRDGDemoCompositeCS);
	SHADER_USE_PARAMETER_STRUCT(FRDGDemoCompositeCS, FGlobalShader);

	class FUseTrail : SHADER_PERMUTATION_BOOL("USE_TRAIL");
	using FPermutationDomain = TShaderPermutationDomain<FUseTrail>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FVector2f, InvSize)
	SHADER_PARAMETER(float, BloomIntensity)
	SHADER_PARAMETER(int32, DebugView)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BloomTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, BloomSampler)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TrailTexture)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SceneColorOutput)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FRDGDemoCompositeCS, "/Project/rdg_demo/RDGPostProcess.glsl", "CompositeCS", SF_Compute);

/** Debug pass whose output is never consumed: RDG culls it, so it costs nothing. */
class FRDGDemoLuminanceHistogramCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FRDGDemoLuminanceHistogramCS);
	SHADER_USE_PARAMETER_STRUCT(FRDGDemoLuminanceHistogramCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, HistogramBuffer)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FRDGDemoLuminanceHistogramCS, "/Project/rdg_demo/RDGPostProcess.glsl", "LuminanceHistogramCS", SF_Compute);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Effect.

void RDGCompositorEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bloom_intensity", "value"), &RDGCompositorEffect::set_bloom_intensity);
	ClassDB::bind_method(D_METHOD("get_bloom_intensity"), &RDGCompositorEffect::get_bloom_intensity);
	ClassDB::bind_method(D_METHOD("set_bloom_threshold", "value"), &RDGCompositorEffect::set_bloom_threshold);
	ClassDB::bind_method(D_METHOD("get_bloom_threshold"), &RDGCompositorEffect::get_bloom_threshold);
	ClassDB::bind_method(D_METHOD("set_bloom_levels", "value"), &RDGCompositorEffect::set_bloom_levels);
	ClassDB::bind_method(D_METHOD("get_bloom_levels"), &RDGCompositorEffect::get_bloom_levels);
	ClassDB::bind_method(D_METHOD("set_trail_enabled", "value"), &RDGCompositorEffect::set_trail_enabled);
	ClassDB::bind_method(D_METHOD("get_trail_enabled"), &RDGCompositorEffect::get_trail_enabled);
	ClassDB::bind_method(D_METHOD("set_trail_weight", "value"), &RDGCompositorEffect::set_trail_weight);
	ClassDB::bind_method(D_METHOD("get_trail_weight"), &RDGCompositorEffect::get_trail_weight);
	ClassDB::bind_method(D_METHOD("set_debug_view", "value"), &RDGCompositorEffect::set_debug_view);
	ClassDB::bind_method(D_METHOD("get_debug_view"), &RDGCompositorEffect::get_debug_view);
	ClassDB::bind_method(D_METHOD("set_add_unused_debug_pass", "value"), &RDGCompositorEffect::set_add_unused_debug_pass);
	ClassDB::bind_method(D_METHOD("get_add_unused_debug_pass"), &RDGCompositorEffect::get_add_unused_debug_pass);
	ClassDB::bind_method(D_METHOD("get_last_stats"), &RDGCompositorEffect::get_last_stats);

	ADD_GROUP("Bloom", "bloom_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bloom_intensity", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_bloom_intensity", "get_bloom_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bloom_threshold", PROPERTY_HINT_RANGE, "0,8,0.01"), "set_bloom_threshold", "get_bloom_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bloom_levels", PROPERTY_HINT_RANGE, "1,6,1"), "set_bloom_levels", "get_bloom_levels");
	ADD_GROUP("Trail", "trail_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "trail_enabled"), "set_trail_enabled", "get_trail_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "trail_weight", PROPERTY_HINT_RANGE, "0,0.98,0.01"), "set_trail_weight", "get_trail_weight");
	ADD_GROUP("Debug", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_view", PROPERTY_HINT_ENUM, "None,Bloom,Bloom Setup"), "set_debug_view", "get_debug_view");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "add_unused_debug_pass"), "set_add_unused_debug_pass", "get_add_unused_debug_pass");

	BIND_ENUM_CONSTANT(DEBUG_VIEW_NONE);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_BLOOM);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_BLOOM_SETUP);
}

void RDGCompositorEffect::_validate_property(PropertyInfo &p_property) const {
	// The RDG pass chain runs after transparents (HDR scene color, before tonemapping); the callback type is fixed.
	if (p_property.name == "effect_callback_type") p_property.usage = PROPERTY_USAGE_NONE;
}

RDGCompositorEffect::RDGCompositorEffect() {
	RenderingServer *RS = RenderingServer::get_singleton();
	if (RS) RS->compositor_effect_set_callback(get_rid(), RenderingServer::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT, callable_mp(this, &RDGCompositorEffect::_render_rdg));
}

RDGCompositorEffect::~RDGCompositorEffect() {
	trail_history.SafeRelease();
}

Dictionary RDGCompositorEffect::get_last_stats() const {
	MutexLock Lock(stats_mutex);
	return last_stats.duplicate();
}

void RDGCompositorEffect::_render_rdg(int p_effect_callback_type, const RenderData *p_render_data) {
	if (!get_enabled() || p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT || !p_render_data) return;
	Ref<RenderSceneBuffersRD> RenderSceneBuffers = p_render_data->get_render_scene_buffers();
	if (RenderSceneBuffers.is_null() || !RenderSceneBuffers->has_internal_texture()) return;
	const FIntPoint ViewSize = RenderSceneBuffers->get_internal_size();
	if (ViewSize.X < 16 || ViewSize.Y < 16) return;

	FRHICommandListImmediate &RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	FGlobalShaderMap *ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Like UE's scene renderer, the graph opts in to parallel pass execution (r.RDG.ParallelExecute).
	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("RDGCompositorEffect %dx%d", ViewSize.X, ViewSize.Y), ERDGBuilderFlags::Parallel);
	FRDGTextureRef SceneColor = RegisterExternalTexture(GraphBuilder, RenderSceneBuffers->get_internal_texture(0), TEXT("SceneColor"));
	if (!SceneColor) return;

	FRHISamplerState *BilinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	const int32 NumLevels = bloom_levels;

	// Bloom: threshold + downsample chain (compute) + upsample chain (raster), all transient.
	FRDGTextureRef BloomSetup = nullptr;
	FRDGTextureRef BloomResult = nullptr;
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Bloom");
		LocalVector<FRDGTextureRef> DownTextures;
		FIntPoint LevelSize = FIntPoint::DivideAndRoundUp(ViewSize, 2);

		{
			BloomSetup = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(LevelSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("Bloom.Setup"));
			TShaderMapRef<FRDGDemoBloomSetupCS> ComputeShader(ShaderMap);
			FRDGDemoBloomSetupCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FRDGDemoBloomSetupCS::FParameters>();
			PassParameters->OutputInvSize = FVector2f(1.0f / LevelSize.X, 1.0f / LevelSize.Y);
			PassParameters->BloomThreshold = bloom_threshold;
			PassParameters->SceneColorTexture = SceneColor;
			PassParameters->SceneColorSampler = BilinearClampSampler;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(BloomSetup);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BloomSetup %dx%d", LevelSize.X, LevelSize.Y), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(LevelSize, FComputeShaderUtils::kGolden2DGroupSize));
			DownTextures.push_back(BloomSetup);
		}

		TShaderMapRef<FRDGDemoBloomDownsampleCS> DownsampleShader(ShaderMap);
		for (int32 Level = 1; Level < NumLevels; Level++) {
			const FIntPoint InputSize = LevelSize;
			LevelSize = FIntPoint::ComponentMax(FIntPoint::DivideAndRoundUp(LevelSize, 2), FIntPoint(1, 1));
			FRDGTextureRef Output = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(LevelSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("Bloom.Down"));
			FRDGDemoBloomDownsampleCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FRDGDemoBloomDownsampleCS::FParameters>();
			PassParameters->OutputInvSize = FVector2f(1.0f / LevelSize.X, 1.0f / LevelSize.Y);
			PassParameters->InputInvSize = FVector2f(1.0f / InputSize.X, 1.0f / InputSize.Y);
			PassParameters->InputTexture = DownTextures[DownTextures.size() - 1];
			PassParameters->InputSampler = BilinearClampSampler;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(Output);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BloomDown %dx%d", LevelSize.X, LevelSize.Y), DownsampleShader, PassParameters, FComputeShaderUtils::GetGroupCount(LevelSize, FComputeShaderUtils::kGolden2DGroupSize));
			DownTextures.push_back(Output);
		}

		// Upsample from the smallest level back to half resolution: Up[i] = Down[i] + Tent(Up[i + 1]).
		FRDGTextureRef Previous = nullptr;
		for (int32 Level = (int32)DownTextures.size() - 1; Level >= 0; Level--) {
			FRDGTextureRef Down = DownTextures[Level];
			FRDGTextureRef Up = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Down->Desc.Extent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_RenderTargetable), TEXT("Bloom.Up"));

			FRDGDemoBloomUpsamplePS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FRDGDemoBloomUpsamplePS::FHasPrevious>(Previous != nullptr);
			TShaderMapRef<FRDGDemoBloomUpsamplePS> PixelShader(ShaderMap, PermutationVector);

			FRDGDemoBloomUpsamplePS::FParameters *PassParameters = GraphBuilder.AllocParameters<FRDGDemoBloomUpsamplePS::FParameters>();
			PassParameters->Radius = 1.0f;
			PassParameters->BloomDownTexture = Down;
			PassParameters->PreviousUpTexture = Previous;
			PassParameters->PreviousInvSize = Previous ? FVector2f(1.0f / Previous->Desc.Extent.X, 1.0f / Previous->Desc.Extent.Y) : FVector2f(0.0f, 0.0f);
			PassParameters->BloomSampler = BilinearClampSampler;
			PassParameters->RenderTargets[0] = FRenderTargetBinding(Up, ERenderTargetLoadAction::ENoAction);
			FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("BloomUp %dx%d", Up->Desc.Extent.X, Up->Desc.Extent.Y), PixelShader, PassParameters, FIntRect(FIntPoint(0, 0), Up->Desc.Extent));
			Previous = Up;
		}
		BloomResult = Previous;
	}

	// UE's renderer flushes the setup queue at points like this one: the bloom passes are set up by a task while the
	// rest of the graph is added (ERDGBuilderFlags::ParallelSetup).
	GraphBuilder.FlushSetupQueue();

	// Trail: blends the scene with the previous frame's history, kept alive across frames by extraction.
	FRDGTextureRef TrailTexture = nullptr;
	if (trail_enabled) {
		RDG_EVENT_SCOPE(GraphBuilder, "Trail");
		const FRDGTextureDesc HistoryDesc = FRDGTextureDesc::Create2D(ViewSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef History = nullptr;
		if (trail_history.IsValid() && trail_history->GetDesc() == HistoryDesc) History = GraphBuilder.RegisterExternalTexture(trail_history, TEXT("Trail.History"));

		TrailTexture = GraphBuilder.CreateTexture(HistoryDesc, TEXT("Trail.Current"));
		TShaderMapRef<FRDGDemoTrailCS> ComputeShader(ShaderMap);
		FRDGDemoTrailCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FRDGDemoTrailCS::FParameters>();
		PassParameters->HistoryWeight = trail_weight;
		PassParameters->bHasHistory = History ? 1 : 0;
		PassParameters->SceneColorTexture = SceneColor;
		PassParameters->HistoryTexture = History ? History : SceneColor;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(TrailTexture);
		// Independent of the bloom chain: runs on the async compute queue in parallel with it and joins at the composite.
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Trail %dx%d", ViewSize.X, ViewSize.Y), ERDGPassFlags::AsyncCompute, ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(ViewSize, FComputeShaderUtils::kGolden2DGroupSize));
		GraphBuilder.QueueTextureExtraction(TrailTexture, &trail_history);
	} else {
		trail_history.SafeRelease();
	}

	// Composite into Godot's scene color (external texture -> this pass is a cull root).
	{
		FRDGDemoCompositeCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRDGDemoCompositeCS::FUseTrail>(TrailTexture != nullptr);
		TShaderMapRef<FRDGDemoCompositeCS> ComputeShader(ShaderMap, PermutationVector);
		FRDGDemoCompositeCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FRDGDemoCompositeCS::FParameters>();
		PassParameters->InvSize = FVector2f(1.0f / ViewSize.X, 1.0f / ViewSize.Y);
		PassParameters->BloomIntensity = bloom_intensity;
		PassParameters->DebugView = debug_view == DEBUG_VIEW_NONE ? 0 : 1;
		PassParameters->BloomTexture = debug_view == DEBUG_VIEW_BLOOM_SETUP ? BloomSetup : BloomResult;
		PassParameters->BloomSampler = BilinearClampSampler;
		PassParameters->TrailTexture = TrailTexture;
		PassParameters->SceneColorOutput = GraphBuilder.CreateUAV(SceneColor);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Composite %dx%d", ViewSize.X, ViewSize.Y), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(ViewSize, FComputeShaderUtils::kGolden2DGroupSize));
	}

	// A debug pass nobody reads: RDG culls it (and never allocates its buffer).
	if (add_unused_debug_pass) {
		FRDGBufferRef Histogram = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 64), TEXT("Debug.LuminanceHistogram"));
		TShaderMapRef<FRDGDemoLuminanceHistogramCS> ComputeShader(ShaderMap);
		FRDGDemoLuminanceHistogramCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FRDGDemoLuminanceHistogramCS::FParameters>();
		PassParameters->SceneColorTexture = SceneColor;
		PassParameters->HistogramBuffer = GraphBuilder.CreateUAV(Histogram);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Debug LuminanceHistogram (unused)"), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(ViewSize, 16));
	}

	GraphBuilder.Execute();

	const FRDGBuilderStats &Stats = GraphBuilder.GetStats();
	FRDGDeviceContext *Context = GraphBuilder.GetDeviceContext();
	Dictionary Result;
	Result["graph"] = String::utf8(GraphBuilder.GetName());
	Result["passes"] = Stats.NumPasses;
	Result["culled_passes"] = Stats.NumCulledPasses;
	Result["merged_render_passes"] = Stats.NumMergedRenderPasses;
	Result["textures"] = Stats.NumTextures;
	Result["transient_textures"] = Stats.NumTransientTextures;
	Result["physical_textures"] = Stats.NumPhysicalTransientTextures;
	Result["buffers"] = Stats.NumBuffers;
	Result["transient_buffers"] = Stats.NumTransientBuffers;
	Result["placed_textures"] = Stats.NumPlacedTextures;
	Result["placed_buffers"] = Stats.NumPlacedBuffers;
	Result["transient_allocated_mb"] = (double)Stats.TransientAllocatedBytes / (1024.0 * 1024.0);
	Result["transient_peak_mb"] = (double)Stats.TransientPeakBytes / (1024.0 * 1024.0);
	Result["transient_heaps"] = Context ? Context->TransientAllocator.GetStats().NumHeaps : 0;
	Result["transient_heap_mb"] = Context ? (double)Context->TransientAllocator.GetStats().HeapMemoryBytes / (1024.0 * 1024.0) : 0.0;
	Result["extracted_textures"] = Stats.NumExtractedTextures;
	Result["binding_errors"] = Stats.NumBindingErrors;
	Result["validation_errors"] = Stats.NumValidationErrors;
	Result["immediate_mode"] = Stats.bImmediateMode;
	Result["async_compute"] = Stats.bAsyncCompute;
	Result["parallel_execute"] = Stats.bParallelExecute;
	Result["parallel_passes"] = Stats.NumParallelPasses;
	Result["parallel_setup"] = Stats.bParallelSetup;
	Result["async_setup_passes"] = Stats.NumAsyncSetupPasses;
	Result["parallel_pass_sets"] = Stats.NumParallelPassSets;
	Result["async_compute_passes"] = Stats.NumAsyncComputePasses;
	Result["pool_textures"] = Context ? Context->RenderTargetPool.GetNumElements() : 0;
	Result["pool_texture_mb"] = Context ? (double)Context->RenderTargetPool.GetStats().MemoryBytes / (1024.0 * 1024.0) : 0.0;
	Result["compiled_shaders"] = Context ? Context->GlobalShaderMap.GetNumCompiledShaders() : 0;
	Result["view_size"] = Vector2i(ViewSize.X, ViewSize.Y);
	Result["has_trail_history"] = trail_history.IsValid();
	// The RenderingDevice translates the whole frame (Godot's passes and this graph) when the frame ends: previous frame.
	const RenderingDevice::TranslateStats &TranslateStats = RHICmdList.GetDevice()->parallel_translate_get_last_frame_stats();
	Result["parallel_translate"] = RHICmdList.GetDevice()->parallel_translate_is_enabled();
	Result["translate_ms"] = (double)TranslateStats.usec / 1000.0;
	Result["translate_commands"] = TranslateStats.command_count;
	Result["translate_tasks"] = TranslateStats.task_count;
	MutexLock Lock(stats_mutex);
	last_stats = Result;
}

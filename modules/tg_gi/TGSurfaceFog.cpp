#include "TGSurfaceFog.h"

#include "PixelShaderUtils.h"
#include "RenderGraph.h"

#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/render_data.h"
#include "servers/rendering/storage/render_scene_data.h"

class FTGSurfaceFogPS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTGSurfaceFogPS);
	SHADER_USE_PARAMETER_STRUCT(FTGSurfaceFogPS, FGlobalShader);

	class FMSAA : SHADER_PERMUTATION_BOOL("FOG_MSAA");
	using FPermutationDomain = TShaderPermutationDomain<FMSAA>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FMatrix44f, InverseProjection)
	SHADER_PARAMETER(FVector4f, ViewportAndFog)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D, FogTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, FogTextureSampler)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, DepthTextureSampler)
	RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTGSurfaceFogPS, "/Project/tg/TGSurfaceFog.glsl", "TGSurfaceFogPS", SF_Pixel);

DECLARE_GPU_STAT_NAMED(TGFogComposite, "TG pass fog_composite");

void TGSurfaceFog::_bind_methods() {
	ClassDB::bind_static_method("TGSurfaceFog", D_METHOD("supported"), &TGSurfaceFog::supported);
	ClassDB::bind_method(D_METHOD("shutdown"), &TGSurfaceFog::shutdown);
}

void TGSurfaceFog::_validate_property(PropertyInfo &p_property) const {
	// After the opaque pass, before the sky: the callback type is fixed.
	if (p_property.name == "effect_callback_type") p_property.usage = PROPERTY_USAGE_NONE;
}

bool TGSurfaceFog::supported() {
	return ClassDB::has_method("RenderSceneBuffersRD", "get_volumetric_fog_texture") && ClassDB::has_method("RenderSceneBuffersRD", "get_volumetric_fog_parameters");
}

TGSurfaceFog::TGSurfaceFog() {
	RenderingServer *RS = RenderingServer::get_singleton();
	if (RS) RS->compositor_effect_set_callback(get_rid(), RenderingServer::COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE, callable_mp(this, &TGSurfaceFog::_render_fog));
}

void TGSurfaceFog::_render_fog(int p_effect_callback_type, const RenderData *p_render_data) {
	if (closed || !p_render_data || p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_OPAQUE) return;
	Ref<RenderSceneBuffersRD> Buffers = p_render_data->get_render_scene_buffers();
	if (Buffers.is_null() || Buffers->get_view_count() != 1) return;
	const RID Fog = Buffers->get_volumetric_fog_texture();
	if (!Fog.is_valid()) return;
	const Vector2 FogParameters = Buffers->get_volumetric_fog_parameters();
	if (FogParameters.x <= 0.0f || FogParameters.y <= 0.0f) return;
	const bool bMSAA = Buffers->get_msaa_3d() != RS::VIEWPORT_MSAA_DISABLED;
	const Vector2i Size = Buffers->get_internal_size();

	FRHICommandListImmediate &RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	FGlobalShaderMap *ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("TGSurfaceFog %dx%d", Size.x, Size.y));
	{
		RDG_EVENT_SCOPE(GraphBuilder, "TGSurfaceFog");
		RDG_GPU_STAT_SCOPE(GraphBuilder, TGFogComposite);
		FRDGTextureRef ColorTarget = RegisterExternalTexture(GraphBuilder, bMSAA ? Buffers->get_color_msaa(0) : Buffers->get_internal_texture(0), bMSAA ? "SceneColorMSAA" : "SceneColor");
		FRDGTextureRef Depth = RegisterExternalTexture(GraphBuilder, bMSAA ? Buffers->get_depth_msaa(0) : Buffers->get_depth_texture(0), bMSAA ? "SceneDepthMSAA" : "SceneDepth");
		FRDGTextureRef FogVolume = RegisterExternalTexture(GraphBuilder, Fog, "VolumetricFog");
		if (ColorTarget && Depth && FogVolume) {
			FTGSurfaceFogPS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FTGSurfaceFogPS::FMSAA>(bMSAA);
			TShaderMapRef<FTGSurfaceFogPS> PixelShader(ShaderMap, PermutationVector);
			FTGSurfaceFogPS::FParameters *Parameters = GraphBuilder.AllocParameters<FTGSurfaceFogPS::FParameters>();
			const Projection Inverse = p_render_data->get_render_scene_data()->get_cam_projection().inverse();
			for (int32 Column = 0; Column < 4; Column++) {
				for (int32 Row = 0; Row < 4; Row++) Parameters->InverseProjection.M[Column][Row] = (float)Inverse.columns[Column][Row];
			}
			Parameters->ViewportAndFog = FVector4f((float)Size.x, (float)Size.y, (float)FogParameters.x, (float)FogParameters.y);
			Parameters->FogTexture = FogVolume;
			Parameters->FogTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			Parameters->DepthTexture = Depth;
			Parameters->DepthTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			Parameters->RenderTargets[0] = FRenderTargetBinding(ColorTarget, ERenderTargetLoadAction::ELoad);
			// scattering + surface_radiance * transmittance; the target's alpha (coverage) is kept.
			FRHIBlendState *BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha>::GetRHI();
			FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("FogComposite %dx%d%s", Size.x, Size.y, bMSAA ? " MSAA" : ""), PixelShader, Parameters, FIntRect(0, 0, Size.x, Size.y), BlendState);
		}
	}
	GraphBuilder.Execute();
}

#pragma once

#include "CommonRenderResources.h"
#include "RenderGraphUtils.h"

/** All utils for pixel shaders (UE's FPixelShaderUtils). */
struct FPixelShaderUtils {
	/** Draws a single triangle covering the whole viewport (3 procedural vertices). */
	static void DrawFullscreenTriangle(FRHICommandList &RHICmdList, uint32 InstanceCount = 1);
	/** Two triangles covering the viewport (6 procedural vertices; the VS must handle 6 vertices). */
	static void DrawFullscreenQuad(FRHICommandList &RHICmdList, uint32 InstanceCount = 1);

	/** Initializes a PSO for a full viewport pass with the screen vertex shader. */
	static void InitFullscreenPipelineState(FRHICommandList &RHICmdList, const FGlobalShaderMap *GlobalShaderMap, const TShaderRef<FShader> &PixelShader, FGraphicsPipelineStateInitializer &GraphicsPSOInit);

	template <typename TShaderClass>
	static void DrawFullscreenPixelShader(FRHICommandList &RHICmdList, const FGlobalShaderMap *GlobalShaderMap, const TShaderRef<TShaderClass> &PixelShader, const typename TShaderClass::FParameters &Parameters, const FIntRect &Viewport,
			FRHIBlendState *BlendState = nullptr, FRHIRasterizerState *RasterizerState = nullptr, FRHIDepthStencilState *DepthStencilState = nullptr, uint32 StencilRef = 0) {
		if (!PixelShader.IsValid()) return;
		RHICmdList.SetViewport((float)Viewport.Min.X, (float)Viewport.Min.Y, 0.0f, (float)Viewport.Max.X, (float)Viewport.Max.Y, 1.0f);

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		InitFullscreenPipelineState(RHICmdList, GlobalShaderMap, TShaderRef<FShader>(PixelShader.GetShader()), GraphicsPSOInit);
		GraphicsPSOInit.BlendState = BlendState ? BlendState : GraphicsPSOInit.BlendState;
		GraphicsPSOInit.RasterizerState = RasterizerState ? RasterizerState : GraphicsPSOInit.RasterizerState;
		GraphicsPSOInit.DepthStencilState = DepthStencilState ? DepthStencilState : GraphicsPSOInit.DepthStencilState;

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, StencilRef);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), Parameters);
		DrawFullscreenTriangle(RHICmdList);
	}

	template <typename TShaderClass>
	static void AddFullscreenPass(FRDGBuilder &GraphBuilder, const FGlobalShaderMap *GlobalShaderMap, FRDGEventName &&PassName, const TShaderRef<TShaderClass> &PixelShader, typename TShaderClass::FParameters *Parameters, const FIntRect &Viewport,
			FRHIBlendState *BlendState = nullptr, FRHIRasterizerState *RasterizerState = nullptr, FRHIDepthStencilState *DepthStencilState = nullptr, uint32 StencilRef = 0, ERDGPassFlags AdditionalPassFlags = ERDGPassFlags::None) {
		if (!RDG_ENSURE_MSG(PixelShader.IsValid(), "AddFullscreenPass '%s': invalid pixel shader.", PassName.GetTCHAR())) return;
		ClearUnusedGraphResources(PixelShader, Parameters);
		GraphBuilder.AddPass(std::move(PassName), Parameters, ERDGPassFlags::Raster | AdditionalPassFlags,
				[Parameters, GlobalShaderMap, PixelShader, Viewport, BlendState, RasterizerState, DepthStencilState, StencilRef](FRDGAsyncTask, FRHICommandList &RHICmdList) {
					FPixelShaderUtils::DrawFullscreenPixelShader(RHICmdList, GlobalShaderMap, PixelShader, *Parameters, Viewport, BlendState, RasterizerState, DepthStencilState, StencilRef);
				});
	}
};

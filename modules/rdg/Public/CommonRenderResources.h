#pragma once

#include "GlobalShader.h"
#include "RHIStaticStates.h"

/** Fullscreen triangle vertex shader (UE's FScreenVertexShaderVS), generated from gl_VertexIndex. */
class FScreenVertexShaderVS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FScreenVertexShaderVS);
	SHADER_USE_NO_PARAMETER_STRUCT(FScreenVertexShaderVS, FGlobalShader);
};

/** Vertex shader used by FPixelShaderUtils::AddRasterizeToRectsPass-like helpers: draws one viewport-sized triangle with UVs. */
using FFullscreenVertexShaderVS = FScreenVertexShaderVS;

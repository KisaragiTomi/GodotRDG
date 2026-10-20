#pragma once

// UE's TStatic*State<...>::GetRHI() helpers. Each instantiation owns one immutable state object.

#include "RHIResources.h"

template <ESamplerFilter Filter = SF_Point,
		ESamplerAddressMode AddressU = AM_Clamp,
		ESamplerAddressMode AddressV = AM_Clamp,
		ESamplerAddressMode AddressW = AM_Clamp,
		int32 MipBias = 0,
		int32 MaxAnisotropy = 0,
		uint32 BorderColor = 0,
		ESamplerCompareFunction SamplerComparisonFunction = SCF_Never>
class TStaticSamplerState {
public:
	static FRHISamplerState *GetRHI() {
		static FRHISamplerState State([] {
			FSamplerStateInitializerRHI Initializer;
			Initializer.Filter = Filter;
			Initializer.AddressU = AddressU;
			Initializer.AddressV = AddressV;
			Initializer.AddressW = AddressW;
			Initializer.MipBias = (float)MipBias;
			Initializer.MaxAnisotropy = MaxAnisotropy;
			Initializer.BorderColor = BorderColor;
			Initializer.SamplerComparisonFunction = SamplerComparisonFunction;
			return Initializer;
		}());
		return &State;
	}
};

template <EColorWriteMask RT0ColorWriteMask = CW_RGBA,
		EBlendOperation RT0ColorBlendOp = BO_Add,
		EBlendFactor RT0ColorSrcBlend = BF_One,
		EBlendFactor RT0ColorDestBlend = BF_Zero,
		EBlendOperation RT0AlphaBlendOp = BO_Add,
		EBlendFactor RT0AlphaSrcBlend = BF_One,
		EBlendFactor RT0AlphaDestBlend = BF_Zero>
class TStaticBlendState {
public:
	static FRHIBlendState *GetRHI() {
		static FRHIBlendState State([] {
			FBlendStateInitializerRHI Initializer;
			for (FBlendStateInitializerRHI::FRenderTarget &RenderTarget : Initializer.RenderTargets) {
				RenderTarget.ColorWriteMask = RT0ColorWriteMask;
				RenderTarget.ColorBlendOp = RT0ColorBlendOp;
				RenderTarget.ColorSrcBlend = RT0ColorSrcBlend;
				RenderTarget.ColorDestBlend = RT0ColorDestBlend;
				RenderTarget.AlphaBlendOp = RT0AlphaBlendOp;
				RenderTarget.AlphaSrcBlend = RT0AlphaSrcBlend;
				RenderTarget.AlphaDestBlend = RT0AlphaDestBlend;
			}
			return Initializer;
		}());
		return &State;
	}
};

/** Blend state that only sets the write mask, like UE's TStaticBlendStateWriteMask. */
template <EColorWriteMask RT0ColorWriteMask = CW_RGBA>
class TStaticBlendStateWriteMask : public TStaticBlendState<RT0ColorWriteMask> {};

template <ERasterizerFillMode FillMode = FM_Solid, ERasterizerCullMode CullMode = CM_None>
class TStaticRasterizerState {
public:
	static FRHIRasterizerState *GetRHI() {
		static FRHIRasterizerState State([] {
			FRasterizerStateInitializerRHI Initializer;
			Initializer.FillMode = FillMode;
			Initializer.CullMode = CullMode;
			return Initializer;
		}());
		return &State;
	}
};

template <bool bEnableDepthWrite = true,
		ECompareFunction DepthTest = CF_DepthNearOrEqual,
		bool bEnableFrontFaceStencil = false,
		ECompareFunction FrontFaceStencilTest = CF_Always,
		EStencilOp FrontFaceStencilFailStencilOp = SO_Keep,
		EStencilOp FrontFaceDepthFailStencilOp = SO_Keep,
		EStencilOp FrontFacePassStencilOp = SO_Keep,
		bool bEnableBackFaceStencil = false,
		ECompareFunction BackFaceStencilTest = CF_Always,
		EStencilOp BackFaceStencilFailStencilOp = SO_Keep,
		EStencilOp BackFaceDepthFailStencilOp = SO_Keep,
		EStencilOp BackFacePassStencilOp = SO_Keep,
		uint8 StencilReadMask = 0xFF,
		uint8 StencilWriteMask = 0xFF>
class TStaticDepthStencilState {
public:
	static FRHIDepthStencilState *GetRHI() {
		static FRHIDepthStencilState State([] {
			FDepthStencilStateInitializerRHI Initializer;
			Initializer.bEnableDepthWrite = bEnableDepthWrite;
			Initializer.DepthTest = DepthTest;
			Initializer.bEnableFrontFaceStencil = bEnableFrontFaceStencil;
			Initializer.FrontFaceStencilTest = FrontFaceStencilTest;
			Initializer.FrontFaceStencilFailStencilOp = FrontFaceStencilFailStencilOp;
			Initializer.FrontFaceDepthFailStencilOp = FrontFaceDepthFailStencilOp;
			Initializer.FrontFacePassStencilOp = FrontFacePassStencilOp;
			Initializer.bEnableBackFaceStencil = bEnableBackFaceStencil;
			Initializer.BackFaceStencilTest = BackFaceStencilTest;
			Initializer.BackFaceStencilFailStencilOp = BackFaceStencilFailStencilOp;
			Initializer.BackFaceDepthFailStencilOp = BackFaceDepthFailStencilOp;
			Initializer.BackFacePassStencilOp = BackFacePassStencilOp;
			Initializer.StencilReadMask = StencilReadMask;
			Initializer.StencilWriteMask = StencilWriteMask;
			return Initializer;
		}());
		return &State;
	}
};

/** Vertex declaration for vertex-buffer-less draws (fullscreen triangle etc.). */
extern FRHIVertexDeclaration GEmptyVertexDeclaration;
extern FRHIVertexDeclaration GFilterVertexDeclaration;

#include "RenderGraphUtils.h"

#include "CommonRenderResources.h"
#include "PixelShaderUtils.h"
#include "RDGDeviceContext.h"
#include "RenderGraphPrivate.h"

#include "servers/rendering/rendering_device.h"

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Engine shaders.

class FClearUAVBufferCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FClearUAVBufferCS);
	SHADER_USE_PARAMETER_STRUCT(FClearUAVBufferCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_UAV(RWByteAddressBuffer, ClearUAV)
	SHADER_PARAMETER(uint32, ClearValue)
	SHADER_PARAMETER(uint32, NumEntries)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FClearUAVBufferCS, "/Engine/Private/ClearReplacementShaders.glsl", "ClearBufferCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FScreenVertexShaderVS, "/Engine/Private/Tools/FullscreenVertexShader.glsl", "MainVS", SF_Vertex);

void RDGPrivate::ClearBufferUAV(FRHIComputeCommandList &RHICmdList, FRHIBuffer *Buffer, uint32 Value) {
	ERR_FAIL_NULL(Buffer);
	if (Value == 0) {
		RHICmdList.FlushComputeList();
		const RID BufferRID = Buffer->GetRID();
		const uint32 Size = Buffer->GetSize() & ~3u;
		EnqueueRenderThreadCommand(RHICmdList.GetDevice(), [=](RenderingDevice *RD) { RD->buffer_clear(BufferRID, 0, Size); });
		return;
	}
	TShaderMapRef<FClearUAVBufferCS> ComputeShader(GetGlobalShaderMap(RHICmdList.GetDevice()));
	if (!ComputeShader.IsValid()) return;
	FClearUAVBufferCS::FParameters Parameters;
	Parameters.ClearUAV = Buffer->GetUAV();
	Parameters.ClearValue = Value;
	Parameters.NumEntries = Buffer->GetSize() / 4;
	FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, Parameters, FComputeShaderUtils::GetGroupCount((int32)Parameters.NumEntries, 64));
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Compute utils.

FIntVector FComputeShaderUtils::GetGroupCountWrapped(const int32 TargetGroupCount) {
	const int32 MaxGroups = 65535;
	FIntVector GroupCount(TargetGroupCount, 1, 1);
	if (GroupCount.X > MaxGroups) {
		GroupCount.Y = FMath::DivideAndRoundUp(GroupCount.X, WrappedGroupStride);
		GroupCount.X = WrappedGroupStride;
	}
	if (GroupCount.Y > MaxGroups) {
		GroupCount.Z = FMath::DivideAndRoundUp(GroupCount.Y, WrappedGroupStride);
		GroupCount.Y = WrappedGroupStride;
	}
	return GroupCount;
}

void FComputeShaderUtils::ValidateGroupCount(const FIntVector &GroupCount) {
	if (GroupCount.X > 65535 || GroupCount.Y > 65535 || GroupCount.Z > 65535) RDGPrivate::ReportValidationError(vformat("Dispatch group count (%d, %d, %d) exceeds the per-dimension limit of 65535.", GroupCount.X, GroupCount.Y, GroupCount.Z));
}

void FComputeShaderUtils::ValidateIndirectArgsBuffer(uint32 IndirectArgsBufferSize, uint32 IndirectArgOffset) {
	if (IndirectArgOffset % 4 != 0) RDGPrivate::ReportValidationError("IndirectArgOffset for compute shader indirect dispatch needs to be a multiple of 4.");
	if (IndirectArgOffset + sizeof(FRHIDispatchIndirectParameters) > IndirectArgsBufferSize) RDGPrivate::ReportValidationError(vformat("Indirect parameters buffer for compute shader indirect dispatch at byte offset %d doesn't have enough room for one element.", IndirectArgOffset));
}

void FComputeShaderUtils::ValidateIndirectArgsBuffer(const FRDGBufferRef IndirectArgsBuffer, uint32 IndirectArgOffset) {
	if (!IndirectArgsBuffer) return;
	if (!EnumHasAnyFlags(IndirectArgsBuffer->Desc.Usage, BUF_DrawIndirect)) RDGPrivate::ReportValidationError(vformat("The buffer %s for indirect dispatch parameters was not flagged with BUF_DrawIndirect", IndirectArgsBuffer->Name));
	ValidateIndirectArgsBuffer(IndirectArgsBuffer->GetSize(), IndirectArgOffset);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Registration helpers.

FRDGTextureRef RegisterExternalTexture(FRDGBuilder &GraphBuilder, RID Texture, const char *Name) {
	TRefCountPtr<IPooledRenderTarget> RenderTarget = CreateRenderTarget(GraphBuilder.GetDevice(), Texture, Name);
	if (!RenderTarget.IsValid()) return nullptr;
	return GraphBuilder.RegisterExternalTexture(RenderTarget, Name);
}

FRDGBufferRef RegisterExternalBuffer(FRDGBuilder &GraphBuilder, RID Buffer, const FRDGBufferDesc &Desc, const char *Name) {
	FRDGDeviceContext *Context = GraphBuilder.GetDeviceContext();
	if (!Context) return nullptr;
	return GraphBuilder.RegisterExternalBuffer(Context->GetOrCreateExternalBuffer(Buffer, Desc, Name), Name);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copy / clear passes.

BEGIN_SHADER_PARAMETER_STRUCT(FCopyTextureParameters, )
RDG_TEXTURE_ACCESS(Input, ERHIAccess::CopySrc)
RDG_TEXTURE_ACCESS(Output, ERHIAccess::CopyDest)
END_SHADER_PARAMETER_STRUCT()

void AddCopyTexturePass(FRDGBuilder &GraphBuilder, FRDGTextureRef InputTexture, FRDGTextureRef OutputTexture, const FRHICopyTextureInfo &CopyInfo) {
	ERR_FAIL_COND_MSG(!InputTexture || !OutputTexture, "[RDG] AddCopyTexturePass: null texture.");
	if (InputTexture == OutputTexture) {
		RDGPrivate::ReportValidationError("AddCopyTexturePass(): Source and destination textures must be different.");
		return;
	}
	if (GetPixelFormatInfo(InputTexture->Desc.Format).BlockBytes != GetPixelFormatInfo(OutputTexture->Desc.Format).BlockBytes) RDGPrivate::ReportValidationError(vformat("AddCopyTexturePass(): Input (%s) and output (%s) formats must match.", InputTexture->Name, OutputTexture->Name));

	FCopyTextureParameters *Parameters = GraphBuilder.AllocParameters<FCopyTextureParameters>();
	Parameters->Input = InputTexture;
	Parameters->Output = OutputTexture;
	GraphBuilder.AddPass(RDG_EVENT_NAME("CopyTexture(%s -> %s)", InputTexture->Name, OutputTexture->Name), Parameters, ERDGPassFlags::Copy,
			[InputTexture, OutputTexture, CopyInfo](FRHIComputeCommandList &RHICmdList) {
				RHICmdList.CopyTexture(InputTexture->GetRHI(), OutputTexture->GetRHI(), CopyInfo);
			});
}

BEGIN_SHADER_PARAMETER_STRUCT(FCopyBufferParameters, )
RDG_BUFFER_ACCESS(SrcBuffer, ERHIAccess::CopySrc)
RDG_BUFFER_ACCESS(DstBuffer, ERHIAccess::CopyDest)
END_SHADER_PARAMETER_STRUCT()

void AddCopyBufferPass(FRDGBuilder &GraphBuilder, FRDGBufferRef DstBuffer, uint64 DstOffset, FRDGBufferRef SrcBuffer, uint64 SrcOffset, uint64 NumBytes) {
	ERR_FAIL_COND_MSG(!DstBuffer || !SrcBuffer, "[RDG] AddCopyBufferPass: null buffer.");
	FCopyBufferParameters *Parameters = GraphBuilder.AllocParameters<FCopyBufferParameters>();
	Parameters->SrcBuffer = SrcBuffer;
	Parameters->DstBuffer = DstBuffer;
	GraphBuilder.AddPass(RDG_EVENT_NAME("CopyBuffer(%s Size=%ubytes)", SrcBuffer->Name, (uint32)NumBytes), Parameters, ERDGPassFlags::Copy,
			[SrcBuffer, SrcOffset, DstBuffer, DstOffset, NumBytes](FRHIComputeCommandList &RHICmdList) {
				RHICmdList.CopyBufferRegion(DstBuffer->GetRHI(), DstOffset, SrcBuffer->GetRHI(), SrcOffset, NumBytes);
			});
}

void AddCopyBufferPass(FRDGBuilder &GraphBuilder, FRDGBufferRef DstBuffer, FRDGBufferRef SrcBuffer) {
	ERR_FAIL_COND(!DstBuffer || !SrcBuffer);
	AddCopyBufferPass(GraphBuilder, DstBuffer, 0, SrcBuffer, 0, FMath::Min(DstBuffer->GetSize(), SrcBuffer->GetSize()));
}

BEGIN_SHADER_PARAMETER_STRUCT(FClearBufferUAVParameters, )
SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, BufferUAV)
END_SHADER_PARAMETER_STRUCT()

void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGBufferUAVRef BufferUAV, uint32 Value, ERDGPassFlags ComputePassFlags) {
	ERR_FAIL_NULL(BufferUAV);
	FClearBufferUAVParameters *Parameters = GraphBuilder.AllocParameters<FClearBufferUAVParameters>();
	Parameters->BufferUAV = BufferUAV;
	FRDGBufferRef Buffer = BufferUAV->GetParent();
	GraphBuilder.AddPass(RDG_EVENT_NAME("ClearBuffer(%s Size=%ubytes)", Buffer->Name, Buffer->GetSize()), Parameters, ComputePassFlags,
			[Buffer, Value](FRHIComputeCommandList &RHICmdList) {
				RDGPrivate::ClearBufferUAV(RHICmdList, Buffer->GetRHI(), Value);
			});
}

void AddClearUAVFloatPass(FRDGBuilder &GraphBuilder, FRDGBufferUAVRef BufferUAV, float Value, ERDGPassFlags ComputePassFlags) {
	uint32 Bits = 0;
	memcpy(&Bits, &Value, 4);
	AddClearUAVPass(GraphBuilder, BufferUAV, Bits, ComputePassFlags);
}

BEGIN_SHADER_PARAMETER_STRUCT(FClearTextureUAVParameters, )
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, TextureUAV)
END_SHADER_PARAMETER_STRUCT()

void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, const FVector4f &ClearValues, ERDGPassFlags ComputePassFlags) {
	ERR_FAIL_NULL(TextureUAV);
	FClearTextureUAVParameters *Parameters = GraphBuilder.AllocParameters<FClearTextureUAVParameters>();
	Parameters->TextureUAV = TextureUAV;
	GraphBuilder.AddPass(RDG_EVENT_NAME("ClearUAV(%s Mip=%d)", TextureUAV->GetParent()->Name, TextureUAV->Desc.MipLevel), Parameters, ComputePassFlags,
			[TextureUAV, ClearValues](FRHIComputeCommandList &RHICmdList) {
				RHICmdList.ClearUAVFloat(TextureUAV->GetRHI(), ClearValues);
			});
}

void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, const FUintVector4 &ClearValues, ERDGPassFlags ComputePassFlags) {
	ERR_FAIL_NULL(TextureUAV);
	FClearTextureUAVParameters *Parameters = GraphBuilder.AllocParameters<FClearTextureUAVParameters>();
	Parameters->TextureUAV = TextureUAV;
	GraphBuilder.AddPass(RDG_EVENT_NAME("ClearUAV(%s Mip=%d)", TextureUAV->GetParent()->Name, TextureUAV->Desc.MipLevel), Parameters, ComputePassFlags,
			[TextureUAV, ClearValues](FRHIComputeCommandList &RHICmdList) {
				RHICmdList.ClearUAVUint(TextureUAV->GetRHI(), ClearValues);
			});
}

void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, float ClearValue, ERDGPassFlags ComputePassFlags) {
	AddClearUAVPass(GraphBuilder, TextureUAV, FVector4f(ClearValue, ClearValue, ClearValue, ClearValue), ComputePassFlags);
}

void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, uint32 ClearValue, ERDGPassFlags ComputePassFlags) {
	AddClearUAVPass(GraphBuilder, TextureUAV, FUintVector4(ClearValue), ComputePassFlags);
}

void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, const FLinearColor &ClearColor, ERDGPassFlags ComputePassFlags) {
	AddClearUAVPass(GraphBuilder, TextureUAV, FVector4f(ClearColor.R, ClearColor.G, ClearColor.B, ClearColor.A), ComputePassFlags);
}

BEGIN_SHADER_PARAMETER_STRUCT(FClearTextureCopyParameters, )
RDG_TEXTURE_ACCESS(Texture, ERHIAccess::CopyDest)
END_SHADER_PARAMETER_STRUCT()

void AddClearRenderTargetPass(FRDGBuilder &GraphBuilder, FRDGTextureRef Texture) {
	ERR_FAIL_NULL(Texture);
	AddClearRenderTargetPass(GraphBuilder, Texture, Texture->Desc.ClearValue.GetClearColor());
}

void AddClearRenderTargetPass(FRDGBuilder &GraphBuilder, FRDGTextureRef Texture, const FLinearColor &ClearColor) {
	ERR_FAIL_NULL(Texture);
	const FIntPoint Extent = Texture->Desc.Extent;
	if (Texture->Desc.ClearValue.ColorBinding == EClearBinding::EColorBound && Texture->Desc.ClearValue.GetClearColor() == ClearColor && EnumHasAnyFlags(Texture->Desc.Flags, TexCreate_RenderTargetable)) {
		// Optimized clear value: a render pass with ERenderTargetLoadAction::EClear and no draws, like UE.
		FRenderTargetParameters *Parameters = GraphBuilder.AllocParameters<FRenderTargetParameters>();
		Parameters->RenderTargets[0] = FRenderTargetBinding(Texture, ERenderTargetLoadAction::EClear);
		GraphBuilder.AddPass(RDG_EVENT_NAME("ClearRenderTarget(%s) %dx%d ClearAction", Texture->Name, Extent.X, Extent.Y), Parameters, ERDGPassFlags::Raster, [](FRHICommandList &) {});
		return;
	}
	FClearTextureCopyParameters *Parameters = GraphBuilder.AllocParameters<FClearTextureCopyParameters>();
	Parameters->Texture = Texture;
	GraphBuilder.AddPass(RDG_EVENT_NAME("ClearRenderTarget(%s) %dx%d", Texture->Name, Extent.X, Extent.Y), Parameters, ERDGPassFlags::Copy,
			[Texture, ClearColor](FRHIComputeCommandList &RHICmdList) {
				RHICmdList.ClearTexture(Texture->GetRHI(), ClearColor, 0, Texture->Desc.NumMips, 0, Texture->Desc.GetRDLayerCount());
			});
}

void AddClearDepthStencilPass(FRDGBuilder &GraphBuilder, FRDGTextureRef Texture, bool bClearDepth, float Depth, bool bClearStencil, uint8 Stencil) {
	ERR_FAIL_NULL(Texture);
	if (Texture->Desc.ClearValue.Depth != Depth || Texture->Desc.ClearValue.Stencil != Stencil) RDGPrivate::ReportValidationWarning(vformat("AddClearDepthStencilPass(%s): RenderingDevice clears to the texture's optimized clear value (depth %f, stencil %d).", Texture->Name, Texture->Desc.ClearValue.Depth, Texture->Desc.ClearValue.Stencil));
	FRenderTargetParameters *Parameters = GraphBuilder.AllocParameters<FRenderTargetParameters>();
	Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(Texture, bClearDepth ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad, bClearStencil ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilWrite);
	GraphBuilder.AddPass(RDG_EVENT_NAME("ClearDepthStencil(%s)", Texture->Name), Parameters, ERDGPassFlags::Raster, [](FRHICommandList &) {});
}

BEGIN_SHADER_PARAMETER_STRUCT(FReadbackBufferParameters, )
RDG_BUFFER_ACCESS(Buffer, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

void AddEnqueueCopyPass(FRDGBuilder &GraphBuilder, FRHIGPUBufferReadback *Readback, FRDGBufferRef SourceBuffer, uint32 NumBytes) {
	ERR_FAIL_COND(!Readback || !SourceBuffer);
	FReadbackBufferParameters *Parameters = GraphBuilder.AllocParameters<FReadbackBufferParameters>();
	Parameters->Buffer = SourceBuffer;
	GraphBuilder.AddPass(RDG_EVENT_NAME("EnqueueCopy(%s)", SourceBuffer->Name), Parameters, ERDGPassFlags::Readback,
			[Readback, SourceBuffer, NumBytes](FRHIComputeCommandList &RHICmdList) {
				Readback->EnqueueCopy(RHICmdList, SourceBuffer->GetRHI(), NumBytes);
			});
}

BEGIN_SHADER_PARAMETER_STRUCT(FReadbackTextureParameters, )
RDG_TEXTURE_ACCESS(Texture, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

void AddEnqueueCopyPass(FRDGBuilder &GraphBuilder, FRHIGPUTextureReadback *Readback, FRDGTextureRef SourceTexture, FResolveRect Rect, uint32 SourceSlice) {
	ERR_FAIL_COND(!Readback || !SourceTexture);
	FReadbackTextureParameters *Parameters = GraphBuilder.AllocParameters<FReadbackTextureParameters>();
	Parameters->Texture = SourceTexture;
	GraphBuilder.AddPass(RDG_EVENT_NAME("EnqueueCopy(%s)", SourceTexture->Name), Parameters, ERDGPassFlags::Readback,
			[Readback, SourceTexture, SourceSlice](FRHIComputeCommandList &RHICmdList) {
				Readback->EnqueueCopy(RHICmdList, SourceTexture->GetRHI(), SourceSlice);
			});
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Buffer helpers.

FRDGBufferRef CreateStructuredBuffer(FRDGBuilder &GraphBuilder, const char *Name, uint32 BytesPerElement, uint32 NumElements, const void *InitialData, uint64 InitialDataSize, ERDGInitialDataFlags InitialDataFlags) {
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(BytesPerElement, FMath::Max(1u, NumElements)), Name);
	if (InitialData && InitialDataSize) GraphBuilder.QueueBufferUpload(Buffer, InitialData, InitialDataSize, InitialDataFlags);
	return Buffer;
}

FRDGBufferRef CreateStructuredBuffer(FRDGBuilder &GraphBuilder, const char *Name, uint32 BytesPerElement, FRDGBufferNumElementsCallback &&NumElementsCallback, FRDGBufferInitialDataCallback &&InitialDataCallback, FRDGBufferInitialDataSizeCallback &&InitialDataSizeCallback) {
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(BytesPerElement, 1), Name, std::move(NumElementsCallback));
	GraphBuilder.QueueBufferUpload(Buffer, std::move(InitialDataCallback), std::move(InitialDataSizeCallback));
	return Buffer;
}

FRDGBufferRef CreateByteAddressBuffer(FRDGBuilder &GraphBuilder, const char *Name, uint32 NumBytes, const void *InitialData, uint64 InitialDataSize, ERDGInitialDataFlags InitialDataFlags) {
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(FMath::Max(4u, NumBytes)), Name);
	if (InitialData && InitialDataSize) GraphBuilder.QueueBufferUpload(Buffer, InitialData, InitialDataSize, InitialDataFlags);
	return Buffer;
}

FRDGBufferRef CreateUploadBuffer(FRDGBuilder &GraphBuilder, const char *Name, uint32 BytesPerElement, uint32 NumElements, const void *InitialData, uint64 InitialDataSize, ERDGInitialDataFlags InitialDataFlags) {
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateUploadDesc(BytesPerElement, FMath::Max(1u, NumElements)), Name);
	if (InitialData && InitialDataSize) GraphBuilder.QueueBufferUpload(Buffer, InitialData, InitialDataSize, InitialDataFlags);
	return Buffer;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Pixel shader utils.

void FPixelShaderUtils::DrawFullscreenTriangle(FRHICommandList &RHICmdList, uint32 InstanceCount) {
	RHICmdList.DrawPrimitive(0, 1, InstanceCount);
}

void FPixelShaderUtils::DrawFullscreenQuad(FRHICommandList &RHICmdList, uint32 InstanceCount) {
	RHICmdList.DrawPrimitive(0, 2, InstanceCount);
}

void FPixelShaderUtils::InitFullscreenPipelineState(FRHICommandList &RHICmdList, const FGlobalShaderMap *GlobalShaderMap, const TShaderRef<FShader> &PixelShader, FGraphicsPipelineStateInitializer &GraphicsPSOInit) {
	TShaderMapRef<FScreenVertexShaderVS> VertexShader(GlobalShaderMap);
	GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = &GFilterVertexDeclaration;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
}

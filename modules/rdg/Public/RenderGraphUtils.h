#pragma once

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "ShaderParameterStruct.h"

inline bool HasBeenProduced(FRDGViewableResource *Resource) { return Resource && Resource->HasBeenProduced(); }
inline FRDGTextureRef GetIfProduced(FRDGTextureRef Texture, FRDGTextureRef FallbackTexture = nullptr) { return HasBeenProduced(Texture) ? Texture : FallbackTexture; }
inline FRDGBufferRef GetIfProduced(FRDGBufferRef Buffer, FRDGBufferRef FallbackBuffer = nullptr) { return HasBeenProduced(Buffer) ? Buffer : FallbackBuffer; }
inline ERenderTargetLoadAction GetLoadActionIfProduced(FRDGTextureRef Texture, ERenderTargetLoadAction ActionIfNotProduced) { return HasBeenProduced(Texture) ? ERenderTargetLoadAction::ELoad : ActionIfNotProduced; }
inline FRenderTargetBinding GetLoadBindingIfProduced(FRDGTextureRef Texture, ERenderTargetLoadAction ActionIfNotProduced) { return FRenderTargetBinding(Texture, GetLoadActionIfProduced(Texture, ActionIfNotProduced)); }

inline FRDGTextureRef RegisterExternalTexture(FRDGBuilder &GraphBuilder, FRHITexture *Texture, const char *NameIfUnregistered) {
	if (FRDGTextureRef Found = GraphBuilder.FindExternalTexture(Texture)) return Found;
	return GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Texture, NameIfUnregistered));
}

/** Godot convenience: registers an RD texture RID (e.g. from RenderSceneBuffersRD) as an external RDG texture. */
FRDGTextureRef RegisterExternalTexture(FRDGBuilder &GraphBuilder, RID Texture, const char *Name);
/** Godot convenience: registers an RD storage buffer RID as an external RDG buffer. */
FRDGBufferRef RegisterExternalBuffer(FRDGBuilder &GraphBuilder, RID Buffer, const FRDGBufferDesc &Desc, const char *Name);

/** All utils for compute shaders. */
namespace FComputeShaderUtils {
static constexpr int32 kGolden2DGroupSize = 8;

inline FIntVector GetGroupCount(const int32 ThreadCount, const int32 GroupSize) { return FIntVector(FMath::DivideAndRoundUp(ThreadCount, GroupSize), 1, 1); }
inline FIntVector GetGroupCount(const FIntPoint &ThreadCount, const FIntPoint &GroupSize) { return FIntVector(FMath::DivideAndRoundUp(ThreadCount.X, GroupSize.X), FMath::DivideAndRoundUp(ThreadCount.Y, GroupSize.Y), 1); }
inline FIntVector GetGroupCount(const FIntPoint &ThreadCount, const int32 GroupSize) { return FIntVector(FMath::DivideAndRoundUp(ThreadCount.X, GroupSize), FMath::DivideAndRoundUp(ThreadCount.Y, GroupSize), 1); }
inline FIntVector GetGroupCount(const FIntVector &ThreadCount, const FIntVector &GroupSize) { return FIntVector(FMath::DivideAndRoundUp(ThreadCount.X, GroupSize.X), FMath::DivideAndRoundUp(ThreadCount.Y, GroupSize.Y), FMath::DivideAndRoundUp(ThreadCount.Z, GroupSize.Z)); }
inline FIntVector GetGroupCount(const FIntVector &ThreadCount, const int32 GroupSize) { return FIntVector(FMath::DivideAndRoundUp(ThreadCount.X, GroupSize), FMath::DivideAndRoundUp(ThreadCount.Y, GroupSize), FMath::DivideAndRoundUp(ThreadCount.Z, GroupSize)); }

static constexpr int32 WrappedGroupStride = 128;
FIntVector GetGroupCountWrapped(const int32 TargetGroupCount);
inline FIntVector GetGroupCountWrapped(const int32 ThreadCount, const int32 GroupSize) { return GetGroupCountWrapped(FMath::DivideAndRoundUp(ThreadCount, GroupSize)); }
void ValidateGroupCount(const FIntVector &GroupCount);
void ValidateIndirectArgsBuffer(uint32 IndirectArgsBufferSize, uint32 IndirectArgOffset);
void ValidateIndirectArgsBuffer(const FRDGBufferRef IndirectArgsBuffer, uint32 IndirectArgOffset);

template <typename TShaderClass>
inline void Dispatch(FRHIComputeCommandList &RHICmdList, const TShaderRef<TShaderClass> &ComputeShader, const FShaderParametersMetadata *ParametersMetadata, const typename TShaderClass::FParameters &Parameters, FIntVector GroupCount) {
	ValidateGroupCount(GroupCount);
	FRHIComputeShader *ShaderRHI = ComputeShader.GetComputeShader();
	SetComputePipelineState(RHICmdList, ShaderRHI);
	SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, ParametersMetadata, Parameters);
	RHICmdList.DispatchComputeShader(GroupCount.X, GroupCount.Y, GroupCount.Z);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
}

template <typename TShaderClass>
inline void Dispatch(FRHIComputeCommandList &RHICmdList, const TShaderRef<TShaderClass> &ComputeShader, const typename TShaderClass::FParameters &Parameters, FIntVector GroupCount) {
	Dispatch(RHICmdList, ComputeShader, TShaderClass::FParameters::FTypeInfo::GetStructMetadata(), Parameters, GroupCount);
}

template <typename TShaderClass>
inline void DispatchIndirect(FRHIComputeCommandList &RHICmdList, const TShaderRef<TShaderClass> &ComputeShader, const typename TShaderClass::FParameters &Parameters, FRHIBuffer *IndirectArgsBuffer, uint32 IndirectArgOffset) {
	ValidateIndirectArgsBuffer(IndirectArgsBuffer ? IndirectArgsBuffer->GetSize() : 0, IndirectArgOffset);
	FRHIComputeShader *ShaderRHI = ComputeShader.GetComputeShader();
	SetComputePipelineState(RHICmdList, ShaderRHI);
	SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);
	RHICmdList.DispatchIndirectComputeShader(IndirectArgsBuffer, IndirectArgOffset);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
}

template <typename TShaderClass>
inline void DispatchIndirect(FRHIComputeCommandList &RHICmdList, const TShaderRef<TShaderClass> &ComputeShader, const typename TShaderClass::FParameters &Parameters, FRDGBufferRef IndirectArgsBuffer, uint32 IndirectArgOffset) {
	ValidateIndirectArgsBuffer(IndirectArgsBuffer, IndirectArgOffset);
	DispatchIndirect(RHICmdList, ComputeShader, Parameters, IndirectArgsBuffer->GetIndirectRHICallBuffer(), IndirectArgOffset);
}

template <typename TShaderClass>
inline FRDGPassRef AddPass(FRDGBuilder &GraphBuilder, FRDGEventName &&PassName, ERDGPassFlags PassFlags, const TShaderRef<TShaderClass> &ComputeShader, const FShaderParametersMetadata *ParametersMetadata, typename TShaderClass::FParameters *Parameters, FIntVector GroupCount) {
	RDG_ENSURE_MSG(EnumHasAnyFlags(PassFlags, ERDGPassFlags::Compute | ERDGPassFlags::AsyncCompute) && !EnumHasAnyFlags(PassFlags, ERDGPassFlags::Copy | ERDGPassFlags::Raster), "AddPass only supports 'Compute' or 'AsyncCompute'.");
	ValidateGroupCount(GroupCount);
	ClearUnusedGraphResources(ComputeShader, ParametersMetadata, Parameters);
	return GraphBuilder.AddPass(std::move(PassName), ParametersMetadata, Parameters, PassFlags,
			[ParametersMetadata, Parameters, ComputeShader, GroupCount](FRDGAsyncTask, FRHIComputeCommandList &RHICmdList) {
				FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, ParametersMetadata, *Parameters, GroupCount);
			});
}

template <typename TShaderClass>
inline FRDGPassRef AddPass(FRDGBuilder &GraphBuilder, FRDGEventName &&PassName, ERDGPassFlags PassFlags, const TShaderRef<TShaderClass> &ComputeShader, typename TShaderClass::FParameters *Parameters, FIntVector GroupCount) {
	return AddPass(GraphBuilder, std::move(PassName), PassFlags, ComputeShader, TShaderClass::FParameters::FTypeInfo::GetStructMetadata(), Parameters, GroupCount);
}

template <typename TShaderClass>
inline FRDGPassRef AddPass(FRDGBuilder &GraphBuilder, FRDGEventName &&PassName, const TShaderRef<TShaderClass> &ComputeShader, typename TShaderClass::FParameters *Parameters, FIntVector GroupCount) {
	return AddPass(GraphBuilder, std::move(PassName), ERDGPassFlags::Compute, ComputeShader, TShaderClass::FParameters::FTypeInfo::GetStructMetadata(), Parameters, GroupCount);
}

/** Group count supplied by a callback right before the pass executes. */
template <typename TShaderClass, typename GroupCountCallbackType>
inline FRDGPassRef AddPassDeferredGroupCount(FRDGBuilder &GraphBuilder, FRDGEventName &&PassName, ERDGPassFlags PassFlags, const TShaderRef<TShaderClass> &ComputeShader, typename TShaderClass::FParameters *Parameters, GroupCountCallbackType &&GroupCountCallback) {
	ClearUnusedGraphResources(ComputeShader, Parameters);
	return GraphBuilder.AddPass(std::move(PassName), Parameters, PassFlags,
			[Parameters, ComputeShader, GroupCountCallback = std::forward<GroupCountCallbackType>(GroupCountCallback)](FRDGAsyncTask, FRHIComputeCommandList &RHICmdList) {
				const FIntVector GroupCount = GroupCountCallback();
				if (GroupCount.X > 0 && GroupCount.Y > 0 && GroupCount.Z > 0) FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *Parameters, GroupCount);
			});
}

/** Indirect dispatch. The parameter struct must declare the args buffer with RDG_BUFFER_ACCESS(..., ERHIAccess::IndirectArgs). */
template <typename TShaderClass>
inline FRDGPassRef AddPass(FRDGBuilder &GraphBuilder, FRDGEventName &&PassName, ERDGPassFlags PassFlags, const TShaderRef<TShaderClass> &ComputeShader, typename TShaderClass::FParameters *Parameters, FRDGBufferRef IndirectArgsBuffer, uint32 IndirectArgsOffset) {
	RDG_ENSURE_MSG(PassFlags == ERDGPassFlags::Compute || PassFlags == ERDGPassFlags::AsyncCompute, "AddPass only supports 'Compute' or 'AsyncCompute'.");
	RDG_ENSURE_MSG(EnumHasAnyFlags(IndirectArgsBuffer->Desc.Usage, BUF_DrawIndirect), "The buffer %s was not flagged for indirect draw parameters", IndirectArgsBuffer->Name);
	ValidateIndirectArgsBuffer(IndirectArgsBuffer, IndirectArgsOffset);
	ClearUnusedGraphResources(ComputeShader, Parameters, { IndirectArgsBuffer });
	return GraphBuilder.AddPass(std::move(PassName), Parameters, PassFlags,
			[Parameters, ComputeShader, IndirectArgsBuffer, IndirectArgsOffset](FRDGAsyncTask, FRHIComputeCommandList &RHICmdList) {
				IndirectArgsBuffer->MarkResourceAsUsed();
				FComputeShaderUtils::DispatchIndirect(RHICmdList, ComputeShader, *Parameters, IndirectArgsBuffer->GetIndirectRHICallBuffer(), IndirectArgsOffset);
			});
}

template <typename TShaderClass>
inline FRDGPassRef AddPass(FRDGBuilder &GraphBuilder, FRDGEventName &&PassName, const TShaderRef<TShaderClass> &ComputeShader, typename TShaderClass::FParameters *Parameters, FRDGBufferRef IndirectArgsBuffer, uint32 IndirectArgsOffset) {
	return AddPass(GraphBuilder, std::move(PassName), ERDGPassFlags::Compute, ComputeShader, Parameters, IndirectArgsBuffer, IndirectArgsOffset);
}
} //namespace FComputeShaderUtils

/** Copies a region from one texture to another (RD texture_copy). Formats and sizes must be compatible. */
void AddCopyTexturePass(FRDGBuilder &GraphBuilder, FRDGTextureRef InputTexture, FRDGTextureRef OutputTexture, const FRHICopyTextureInfo &CopyInfo);
inline void AddCopyTexturePass(FRDGBuilder &GraphBuilder, FRDGTextureRef InputTexture, FRDGTextureRef OutputTexture, FIntPoint InputPosition = FIntPoint::ZeroValue(), FIntPoint OutputPosition = FIntPoint::ZeroValue(), FIntPoint Size = FIntPoint::ZeroValue()) {
	FRHICopyTextureInfo CopyInfo;
	CopyInfo.SourcePosition = FIntVector(InputPosition.X, InputPosition.Y, 0);
	CopyInfo.DestPosition = FIntVector(OutputPosition.X, OutputPosition.Y, 0);
	if (Size != FIntPoint::ZeroValue()) CopyInfo.Size = FIntVector(Size.X, Size.Y, 1);
	AddCopyTexturePass(GraphBuilder, InputTexture, OutputTexture, CopyInfo);
}

void AddCopyBufferPass(FRDGBuilder &GraphBuilder, FRDGBufferRef DstBuffer, uint64 DstOffset, FRDGBufferRef SrcBuffer, uint64 SrcOffset, uint64 NumBytes);
void AddCopyBufferPass(FRDGBuilder &GraphBuilder, FRDGBufferRef DstBuffer, FRDGBufferRef SrcBuffer);

void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGBufferUAVRef BufferUAV, uint32 Value, ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute);
void AddClearUAVFloatPass(FRDGBuilder &GraphBuilder, FRDGBufferUAVRef BufferUAV, float Value, ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute);
void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, float ClearValue, ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute);
void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, uint32 ClearValue, ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute);
void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, const FVector4f &ClearValues, ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute);
void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, const FUintVector4 &ClearValues, ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute);
void AddClearUAVPass(FRDGBuilder &GraphBuilder, FRDGTextureUAVRef TextureUAV, const FLinearColor &ClearColor, ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute);

/** Clears a render target with its optimized clear value (or the given color) through a render pass load action. */
void AddClearRenderTargetPass(FRDGBuilder &GraphBuilder, FRDGTextureRef Texture);
void AddClearRenderTargetPass(FRDGBuilder &GraphBuilder, FRDGTextureRef Texture, const FLinearColor &ClearColor);
void AddClearDepthStencilPass(FRDGBuilder &GraphBuilder, FRDGTextureRef Texture, bool bClearDepth = true, float Depth = 0.0f, bool bClearStencil = true, uint8 Stencil = 0);

void AddEnqueueCopyPass(FRDGBuilder &GraphBuilder, FRHIGPUBufferReadback *Readback, FRDGBufferRef SourceBuffer, uint32 NumBytes);
void AddEnqueueCopyPass(FRDGBuilder &GraphBuilder, FRHIGPUTextureReadback *Readback, FRDGTextureRef SourceTexture, FResolveRect Rect = FResolveRect(), uint32 SourceSlice = 0);

FRDGBufferRef CreateStructuredBuffer(FRDGBuilder &GraphBuilder, const char *Name, uint32 BytesPerElement, uint32 NumElements, const void *InitialData, uint64 InitialDataSize, ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None);
template <typename ElementType>
inline FRDGBufferRef CreateStructuredBuffer(FRDGBuilder &GraphBuilder, const char *Name, const LocalVector<ElementType> &InitialData, ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None) {
	const uint32 NumElements = FMath::Max(1u, (uint32)InitialData.size());
	return CreateStructuredBuffer(GraphBuilder, Name, (uint32)sizeof(ElementType), NumElements, InitialData.ptr(), (uint64)InitialData.size() * sizeof(ElementType), InitialDataFlags);
}
FRDGBufferRef CreateByteAddressBuffer(FRDGBuilder &GraphBuilder, const char *Name, uint32 NumBytes, const void *InitialData, uint64 InitialDataSize, ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None);
/** The number of elements and the initial data come from callbacks, called when the graph compiles (e.g. filled by a
 *  setup task, see FRDGBuilder::AddSetupTask). */
FRDGBufferRef CreateStructuredBuffer(FRDGBuilder &GraphBuilder, const char *Name, uint32 BytesPerElement, FRDGBufferNumElementsCallback &&NumElementsCallback, FRDGBufferInitialDataCallback &&InitialDataCallback, FRDGBufferInitialDataSizeCallback &&InitialDataSizeCallback);

FRDGBufferRef CreateUploadBuffer(FRDGBuilder &GraphBuilder, const char *Name, uint32 BytesPerElement, uint32 NumElements, const void *InitialData, uint64 InitialDataSize, ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None);

/** Pass with no parameters that is never culled (useful for CPU-side callbacks in GPU order). */
template <typename ExecuteLambdaType>
inline void AddPass(FRDGBuilder &GraphBuilder, FRDGEventName &&Name, ExecuteLambdaType &&ExecuteLambda) {
	GraphBuilder.AddPass(std::move(Name), ERDGPassFlags::NeverCull, std::forward<ExecuteLambdaType>(ExecuteLambda));
}

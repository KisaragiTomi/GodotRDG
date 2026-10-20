#pragma once

// UE-style RHI command lists recorded straight into Godot's RenderingDevice.
// Compute work is batched into one RD compute list per pass, raster passes record into an RD draw list that RDG opens
// from the pass' RENDER_TARGET_BINDING_SLOTS. Shader parameters are resolved to RD uniform sets + push constants when
// the dispatch / draw is issued.

#include "RHIResources.h"
#include "RHIStaticStates.h"
#include "ShaderCore.h"

#include "core/templates/hash_map.h"

class FShader;
struct FShaderParameterBindings;
class FRDGBuilder;
class FRDGPass;

class FRHIComputeCommandList {
public:
	explicit FRHIComputeCommandList(RenderingDevice *InDevice);
	FRHIComputeCommandList(const FRHIComputeCommandList &) = delete;
	FRHIComputeCommandList &operator=(const FRHIComputeCommandList &) = delete;
	~FRHIComputeCommandList();

	/** Godot escape hatch: the underlying device. Prefer the UE-style API below. */
	RenderingDevice *GetDevice() const { return Device; }
	/** Godot escape hatch: current RD compute list (begins one if needed). */
	RenderingDevice::ComputeListID GetComputeList();
	/** Godot escape hatch: current RD draw list (only inside a raster pass). */
	RenderingDevice::DrawListID GetDrawList() const { return DrawList; }

	// Compute.
	void SetComputePipelineState(FRHIComputeShader *ComputeShader);
	void DispatchComputeShader(uint32 ThreadGroupCountX, uint32 ThreadGroupCountY, uint32 ThreadGroupCountZ);
	void DispatchIndirectComputeShader(FRHIBuffer *ArgumentBuffer, uint32 ArgumentOffset);

	// Parameters (normally called through SetShaderParameters()).
	void SetShaderParameters(const FShader *Shader, const void *ParametersData);
	void SetPushConstantData(const void *Data, uint32 Size, uint32 Offset = 0);

	// Copy / clear.
	void CopyTexture(FRHITexture *SourceTexture, FRHITexture *DestTexture, const FRHICopyTextureInfo &CopyInfo);
	void CopyBufferRegion(FRHIBuffer *DestBuffer, uint64 DstOffset, FRHIBuffer *SourceBuffer, uint64 SrcOffset, uint64 NumBytes);
	void ClearUAVFloat(FRHIUnorderedAccessView *UnorderedAccessViewRHI, const FVector4f &Values);
	void ClearUAVUint(FRHIUnorderedAccessView *UnorderedAccessViewRHI, const FUintVector4 &Values);
	void ClearTexture(FRHITexture *Texture, const FLinearColor &Color, uint32 MipIndex = 0, uint32 NumMips = 1, uint32 FirstSlice = 0, uint32 NumSlices = 1);
	void UpdateBuffer(FRHIBuffer *Buffer, uint32 Offset, uint32 Size, const void *Data);
	void UpdateTexture(FRHITexture *Texture, uint32 Layer, const Vector<uint8_t> &Data);

	// Events.
	void PushEvent(const char *Name, const FLinearColor &Color = FLinearColor::White);
	void PopEvent();

	/** Ends any open compute list. Called by RDG at the end of every pass. */
	void FlushComputeList();

	/** Number of dispatches / draws that were skipped because of binding errors (tests check this). */
	uint32 GetNumBindingErrors() const { return NumBindingErrors; }

protected:
	friend class FRDGBuilder;

	struct FPendingUniform {
		uint32 Set = 0;
		uint32 Binding = 0;
		RenderingDevice::UniformType Type = RenderingDevice::UNIFORM_TYPE_MAX;
		LocalVector<RID> IDs;
	};

	/** Builds and binds uniform sets + push constants for the given shader interface. Returns false on error. */
	bool FlushShaderBindings(RID ShaderRID, const FShaderReflectionInfo &Reflection, const String &ShaderName, bool bGraphics);
	void ResetBindings();
	FPendingUniform &FindOrAddPendingUniform(uint32 Set, uint32 Binding);

	RenderingDevice *Device = nullptr;
	RenderingDevice::ComputeListID ComputeList = -1;
	RenderingDevice::DrawListID DrawList = -1;

	// Compute state.
	FRHIComputeShader *CurrentComputeShader = nullptr;

	// Graphics state.
	RenderingDevice::FramebufferFormatID CurrentFramebufferFormat = RenderingDevice::INVALID_ID;
	uint32 CurrentNumColorTargets = 0;
	FIntPoint CurrentRenderTargetSize;
	RID CurrentGraphicsShaderRID;
	const FShaderReflectionInfo *CurrentGraphicsReflection = nullptr;
	String CurrentGraphicsShaderName;
	EPrimitiveType CurrentPrimitiveType = PT_TriangleList;
	bool bGraphicsPipelineValid = false;

	LocalVector<FPendingUniform> PendingUniforms;
	// Uniform set bound last for each set index: consecutive dispatches / draws with the same bindings skip the device
	// cache. Only valid within a pass (uniform sets can be freed between passes), see FlushComputeList / EndRenderPass.
	struct FLastUniformSet {
		uint64 Key = 0;
		RID UniformSet;
	};
	static constexpr uint32 NumLastUniformSets = 4;
	FLastUniformSet LastUniformSets[NumLastUniformSets];
	void ResetLastUniformSets();
	LocalVector<uint8_t> PushConstantData;
	uint32 PushConstantSize = 0;
	bool bBindingError = false;
	uint32 NumBindingErrors = 0;
	uint32 NumOpenEvents = 0;
};

class FRHICommandList : public FRHIComputeCommandList {
public:
	explicit FRHICommandList(RenderingDevice *InDevice) :
			FRHIComputeCommandList(InDevice) {}

	/** Opens an RD draw list for the given attachments (done by RDG for raster passes). */
	struct FRenderPassInfo {
		RID ColorRIDs[MaxSimultaneousRenderTargets];
		ERenderTargetLoadAction ColorLoadActions[MaxSimultaneousRenderTargets] = {};
		FLinearColor ClearColors[MaxSimultaneousRenderTargets];
		uint32 NumColorTargets = 0;
		RID DepthStencilRID;
		ERenderTargetLoadAction DepthLoadAction = ERenderTargetLoadAction::ENoAction;
		ERenderTargetLoadAction StencilLoadAction = ERenderTargetLoadAction::ENoAction;
		float ClearDepth = 0.0f;
		uint32 ClearStencil = 0;
		bool bHasStencil = false;
		FIntPoint Extent;
		const char *Name = "";
	};
	void BeginRenderPass(const FRenderPassInfo &Info);
	void EndRenderPass();
	bool IsInsideRenderPass() const { return DrawList >= 0; }

	void SetGraphicsPipelineState(const FGraphicsPipelineStateInitializer &Initializer, uint32 StencilRef);
	void SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ);
	void SetScissorRect(bool bEnable, uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY);
	void SetBlendFactor(const FLinearColor &BlendFactor);
	/** Vertex buffer of a stream of the current vertex declaration (UE's SetStreamSource). Kept until the render pass ends. */
	void SetStreamSource(uint32 StreamIndex, FRHIBuffer *VertexBuffer, uint32 Offset);
	/** Non-indexed draw. With a vertex declaration the vertices come from the stream sources, otherwise from
	 *  gl_VertexIndex. RenderingDevice has no base vertex: BaseVertexIndex offsets the per-vertex streams instead. */
	void DrawPrimitive(uint32 BaseVertexIndex, uint32 NumPrimitives, uint32 NumInstances);
	/** Indexed draw. NumVertices is the vertex range the indices reference (used to validate the streams), FirstInstance
	 *  must be 0 (RenderingDevice has no first instance). */
	void DrawIndexedPrimitive(FRHIBuffer *IndexBuffer, int32 BaseVertexIndex, uint32 FirstInstance, uint32 NumVertices, uint32 StartIndex, uint32 NumPrimitives, uint32 NumInstances);
	void DrawPrimitiveIndirect(FRHIBuffer *ArgumentBuffer, uint32 ArgumentOffset);
	void DrawIndexedPrimitiveIndirect(FRHIBuffer *IndexBuffer, FRHIBuffer *ArgumentBuffer, uint32 ArgumentOffset);

private:
	struct FStreamSource {
		FRHIBuffer *Buffer = nullptr;
		uint32 Offset = 0;
	};
	FStreamSource StreamSources[MaxVertexElementCount];
	const FRHIVertexDeclaration *CurrentVertexDeclaration = nullptr;
	RenderingDevice::VertexFormatID CurrentVertexFormat = RenderingDevice::INVALID_FORMAT_ID;

	uint32 GetNumVertices(uint32 NumPrimitives) const;
	/** Binds the stream sources for the current vertex declaration. NumVertices = 0 binds as many vertices as the streams hold. */
	bool BindStreamSources(int32 BaseVertexIndex, uint32 NumVertices);
	void ResetStreamSources();
};

class FRHICommandListImmediate : public FRHICommandList {
public:
	explicit FRHICommandListImmediate(RenderingDevice *InDevice) :
			FRHICommandList(InDevice) {}

	/** Submits and waits (local devices only; the main device is submitted by Godot every frame). */
	void SubmitCommandsAndFlushGPU();
};

class FRHICommandListExecutor {
public:
	/** Immediate command list of the main RenderingDevice (render thread only). */
	static FRHICommandListImmediate &GetImmediateCommandList();
	/** Destroys the cached immediate command list (module shutdown). */
	static void ShutdownImmediateCommandList();
	/** Applies r.RHICmd.ParallelTranslate.* to the device (render thread): the RenderingDevice translates its command graph when the frame ends. */
	static void UpdateParallelTranslate(RenderingDevice *Device);
};

inline FRHICommandListImmediate &GetImmediateCommandList_ForRenderCommand() { return FRHICommandListExecutor::GetImmediateCommandList(); }

/** UE free functions. */
void SetComputePipelineState(FRHIComputeCommandList &RHICmdList, FRHIComputeShader *ComputeShader);
void SetGraphicsPipelineState(FRHICommandList &RHICmdList, const FGraphicsPipelineStateInitializer &Initializer, uint32 StencilRef, bool bApplyAdditionalState = true);

/** Asynchronous GPU -> CPU buffer readback (UE's FRHIGPUBufferReadback). */
class FRHIGPUBufferReadback {
public:
	explicit FRHIGPUBufferReadback(const char *InName);
	~FRHIGPUBufferReadback();

	void EnqueueCopy(FRHIComputeCommandList &RHICmdList, FRHIBuffer *SourceBuffer, uint32 NumBytes = 0);
	bool IsReady() const { return bReady; }
	const void *Lock(uint32 NumBytes);
	void Unlock() {}
	uint32 GetGPUSizeBytes() const { return (uint32)Data.size(); }
	const Vector<uint8_t> &GetData() const { return Data; }

	static void OnDataReceived(const Vector<uint8_t> &InData, uint64 ReadbackId);

private:
	String Name;
	uint64 Id = 0;
	bool bReady = false;
	bool bPending = false;
	Vector<uint8_t> Data;
};

/** Asynchronous GPU -> CPU texture readback (UE's FRHIGPUTextureReadback). Reads a whole layer of mip 0. */
class FRHIGPUTextureReadback {
public:
	explicit FRHIGPUTextureReadback(const char *InName);
	~FRHIGPUTextureReadback();

	void EnqueueCopy(FRHIComputeCommandList &RHICmdList, FRHITexture *SourceTexture, uint32 SourceSlice = 0);
	bool IsReady() const { return bReady; }
	const void *Lock(int32 &OutRowPitchInPixels);
	void Unlock() {}
	const Vector<uint8_t> &GetData() const { return Data; }
	FIntPoint GetSize() const { return Size; }

	static void OnDataReceived(const Vector<uint8_t> &InData, uint64 ReadbackId);

private:
	String Name;
	uint64 Id = 0;
	bool bReady = false;
	bool bPending = false;
	FIntPoint Size;
	Vector<uint8_t> Data;
};

#include "RenderGraphBuilder.h"

#include "RDGDeviceContext.h"
#include "RenderGraphPrivate.h"
#include "RenderGraphResourcePool.h"

#include "core/object/worker_thread_pool.h"
#include "servers/rendering/rendering_device.h"

TAutoConsoleVariable<int32> CVarRDGImmediateMode(
		"r.RDG.ImmediateMode", 0,
		"Executes passes as they get created. Useful to have a callstack of the wiring code when crashing in the pass' lambda.",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGDebug(
		"r.RDG.Debug", 0,
		"Allow to output warnings for inefficiencies found during wiring and execution of the passes.\n"
		" 0: disabled;\n"
		" 1: emit warnings (unused resources, passes without parameters, ...);",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGValidation(
		"r.RDG.Validation", 1,
		"Validates graph usage (pass flags, resource access in lambdas, read before write, ...). Errors are reported with ERR_PRINT.",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGCullPasses(
		"r.RDG.CullPasses", 1,
		"The graph will cull passes with unused outputs.\n"
		" 0:off;\n"
		" 1:on(default);",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGMergeRenderPasses(
		"r.RDG.MergeRenderPasses", 1,
		"The graph will merge identical, contiguous render passes into a single render pass.\n"
		" 0:off;\n"
		" 1:on(default);",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGTransientAllocator(
		"r.RDG.TransientAllocator", 1,
		"RDG will use the RHITransientResourceAllocator to allocate all transient resources.\n"
		" 0: disables the transient allocator: transient resources come from the resource pools and go back to them after their last pass;\n"
		" 1: enables the transient allocator (default): transient resources are placed in transient heaps and alias each other's memory;",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGTransientIndirectArgBuffers(
		"r.RDG.TransientAllocator.IndirectArgumentBuffers", 0,
		"Whether indirect argument buffers should use transient resource allocator. Default: 0",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGClobberResources(
		"r.RDG.ClobberResources", 0,
		"Clears all render targets and texture / buffer UAVs with a sentinel value at allocation time. Useful for debugging.\n"
		" 0:off (default);\n"
		" 1: 1000 on RTs and UAVs;",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGDumpGraph(
		"r.RDG.DumpGraph", 0,
		"Dumps the producer / consumer graph to user://rdg/<graph>_<frame>.dot (Graphviz).\n"
		" 0: off;\n"
		" 1: dump every executed graph;\n"
		" 2: dump the next executed graph, then reset to 0;",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGAsyncCompute(
		"r.RDG.AsyncCompute", 1,
		"Controls the async compute policy.\n"
		" 0:disabled, no async compute is used;\n"
		" 1:enabled for passes tagged for async compute (default);\n"
		" 2:enabled for all compute passes implemented to use the compute command list;",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGParallelSetup(
		"r.RDG.ParallelSetup", 1,
		"RDG will launch setup tasks from AddSetupTask.\n"
		" 0: setup tasks are executed inline;\n"
		" 1: setup tasks are launched (default);",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGParallelCompile(
		"r.RDG.ParallelCompile", 1,
		"RDG will launch tasks to compile the RDG graph.\n"
		" 0: compile tasks are executed inline;\n"
		" 1: compile tasks are launched (default);",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGAsyncSetupQueue(
		"r.RDG.AsyncSetupQueue", 1,
		"RDG will setup passes in parallel when prompted by calls to FRDGBuilder::FlushSetupQueue.\n"
		" 0: pass setup is done synchronously in AddPass;\n"
		" 1: pass setup is done asynchronously (default);",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGParallelExecute(
		"r.RDG.ParallelExecute", 2,
		"Whether to enable parallel execution of passes when supported.\n"
		" 0: off\n"
		" 1: parallel with all tasks awaited\n"
		" 2: parallel with async tasks (default). Godot submits the recorded commands on the render thread, so the tasks are awaited before Execute() returns, like 1.",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGParallelExecutePassMin(
		"r.RDG.ParallelExecute.PassMin", 1,
		"The minimum span of contiguous passes eligible for parallel execution for the span to be offloaded to a task.",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGParallelExecutePassMax(
		"r.RDG.ParallelExecute.PassMax", 32,
		"The maximum span of contiguous passes eligible for parallel execution for the span to be offloaded to a task.",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRDGEvents(
		"r.RDG.Events", 1,
		"Emits RenderingDevice debug labels for every pass (visible in RenderDoc / profilers).",
		ECVF_RenderThreadSafe);

namespace {
String GLastDumpPath;

constexpr int32 RDG_ASYNC_COMPUTE_FORCE_ENABLED = 2;

bool IsTransientReleasable(const FRDGViewableResource *Resource, bool bMultiFrame) {
	return !Resource->IsExternal() && !Resource->IsExtracted() && !bMultiFrame;
}
} //namespace

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / allocation.

FRDGBuilder::FRDGBuilder(FRHICommandListImmediate &InRHICmdList, FRDGEventName InName, ERDGBuilderFlags Flags, EShaderPlatform ShaderPlatform) :
		RHICmdList(InRHICmdList), Device(InRHICmdList.GetDevice()), BuilderName(std::move(InName)) {
	if (BuilderName.IsEmpty()) BuilderName = FRDGEventName("RDG");
	bImmediateMode = IsImmediateMode();
	Stats.bImmediateMode = bImmediateMode;
	ValidationErrorsAtStart = RDGPrivate::GetValidationErrorCount();
	ERR_FAIL_NULL_MSG(Device, "[RDG] FRDGBuilder created without a RenderingDevice (is the renderer Forward+ / Mobile?).");
	// UE's IsAsyncComputeSupported: the policy CVar, no immediate mode, and an async compute queue on the device.
	bSupportsAsyncCompute = CVarRDGAsyncCompute.GetValueOnRenderThread() > 0 && !bImmediateMode && Device->async_compute_is_supported();
	Stats.bAsyncCompute = bSupportsAsyncCompute;
	// UE's IsParallelExecuteEnabled: opt-in per graph, and off in immediate / debug mode.
	bParallelExecute = EnumHasAnyFlags(Flags, ERDGBuilderFlags::ParallelExecute) && CVarRDGParallelExecute.GetValueOnRenderThread() > 0 && !bImmediateMode && !CVarRDGDebug.GetValueOnRenderThread() && WorkerThreadPool::get_singleton();
	Stats.bParallelExecute = bParallelExecute;
	// UE's IsParallelSetupEnabled: r.RDG.ParallelSetup, no immediate mode; the async setup queue and the parallel compile
	// need it as well.
	const bool bParallelSetupAllowed = CVarRDGParallelSetup.GetValueOnRenderThread() > 0 && !bImmediateMode && WorkerThreadPool::get_singleton();
	ParallelSetup.bEnabled = bParallelSetupAllowed && EnumHasAnyFlags(Flags, ERDGBuilderFlags::ParallelSetup);
	AsyncSetupQueue.bEnabled = ParallelSetup.bEnabled && CVarRDGAsyncSetupQueue.GetValueOnRenderThread() > 0;
	bParallelCompileEnabled = bParallelSetupAllowed && EnumHasAnyFlags(Flags, ERDGBuilderFlags::ParallelCompile) && CVarRDGParallelCompile.GetValueOnRenderThread() > 0;
	Stats.bParallelSetup = ParallelSetup.bEnabled;
	Stats.bParallelCompile = bParallelCompileEnabled;
	DeviceContext = FRDGDeviceContext::Get(Device);
	if (Device == FRDGDeviceContext::GetMainDevice()) DeviceContext->TickIfNewEngineFrame();
	FRHICommandListExecutor::UpdateParallelTranslate(Device);
}

FRDGBuilder::~FRDGBuilder() {
	// Setup tasks and the async setup queue task reference the graph memory.
	WaitForParallelSetupTasks(ERDGSetupTaskWaitPoint::Execute);
	AsyncSetupQueue.LastTask.Wait();
	for (FUploadedBuffer *UploadedBuffer : UploadedBuffers) {
		if (!UploadedBuffer->bUploaded && UploadedBuffer->DataFreeCallback) UploadedBuffer->DataFreeCallback(UploadedBuffer->Data);
	}
	if (!bExecuted && (!Passes.is_empty() || !Textures.is_empty() || !Buffers.is_empty())) {
		RDGPrivate::ReportValidationError(vformat("Render graph '%s' destroyed without Execute(). Render graph execution is required to ensure consistency with immediate mode.", BuilderName.GetTCHAR()));
		RHICmdList.FlushComputeList();
		if (RHICmdList.IsInsideRenderPass()) RHICmdList.EndRenderPass();
	}
	for (int64 Index = (int64)Destructors.size() - 1; Index >= 0; Index--) Destructors[Index].Function(Destructors[Index].Object);
	Destructors.clear();
	for (FMemoryBlock &Block : MemoryBlocks) memfree(Block.Data);
	MemoryBlocks.clear();
}

void *FRDGBuilder::Alloc(uint64 SizeInBytes, uint32 AlignInBytes) {
	const uint64 Align = FMath::Max<uint64>(AlignInBytes, 8);
	if (!MemoryBlocks.is_empty()) {
		FMemoryBlock &Block = MemoryBlocks[MemoryBlocks.size() - 1];
		const uintptr_t Current = (uintptr_t)(Block.Data + Block.Used);
		const uintptr_t Aligned = (Current + Align - 1) & ~(uintptr_t)(Align - 1);
		const uint64 Offset = Aligned - (uintptr_t)Block.Data;
		if (Offset + SizeInBytes <= Block.Size) {
			Block.Used = Offset + SizeInBytes;
			return Block.Data + Offset;
		}
	}
	FMemoryBlock Block;
	Block.Size = FMath::Max<uint64>(64 * 1024, SizeInBytes + Align);
	Block.Data = reinterpret_cast<uint8 *>(memalloc(Block.Size));
	const uintptr_t Aligned = ((uintptr_t)Block.Data + Align - 1) & ~(uintptr_t)(Align - 1);
	const uint64 Offset = Aligned - (uintptr_t)Block.Data;
	Block.Used = Offset + SizeInBytes;
	MemoryBlocks.push_back(Block);
	return Block.Data + Offset;
}

const char *FRDGBuilder::CopyName(const char *Name) {
	if (!Name) Name = "Unnamed";
	const size_t Length = strlen(Name);
	char *Copy = reinterpret_cast<char *>(Alloc(Length + 1, 1));
	memcpy(Copy, Name, Length + 1);
	return Copy;
}

ERDGPassFlags FRDGBuilder::OverridePassFlags(ERDGPassFlags Flags) const {
	// UE's FRDGBuilder::OverridePassFlags: without async compute, AsyncCompute passes become Compute passes.
	if (bSupportsAsyncCompute) {
		if (EnumHasAnyFlags(Flags, ERDGPassFlags::Compute) && CVarRDGAsyncCompute.GetValueOnRenderThread() == RDG_ASYNC_COMPUTE_FORCE_ENABLED) {
			Flags &= ~ERDGPassFlags::Compute;
			Flags |= ERDGPassFlags::AsyncCompute;
		}
	} else if (EnumHasAnyFlags(Flags, ERDGPassFlags::AsyncCompute)) {
		Flags &= ~ERDGPassFlags::AsyncCompute;
		Flags |= ERDGPassFlags::Compute;
	}
	return Flags;
}

FGlobalShaderMap *FRDGBuilder::GetShaderMap() const {
	return DeviceContext ? &DeviceContext->GlobalShaderMap : nullptr;
}

bool FRDGBuilder::IsImmediateMode() {
	return CVarRDGImmediateMode.GetValueOnRenderThread() != 0;
}

void FRDGBuilder::TickPoolElements() {
	TickPoolElements(FRDGDeviceContext::GetMainDevice());
}

void FRDGBuilder::TickPoolElements(RenderingDevice *InDevice) {
	if (!InDevice) return;
	FRDGDeviceContext::Get(InDevice)->TickFrame();
}

String FRDGBuilder::GetLastDumpPath() {
	return GLastDumpPath;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Resources.

FRDGTextureRef FRDGBuilder::FindExternalTexture(FRHITexture *Texture) const {
	FRDGTexture *const *Found = ExternalTextures.getptr(Texture);
	return Found ? *Found : nullptr;
}

FRDGTextureRef FRDGBuilder::FindExternalTexture(IPooledRenderTarget *ExternalPooledTexture) const {
	return ExternalPooledTexture ? FindExternalTexture(ExternalPooledTexture->GetRHI()) : nullptr;
}

FRDGBufferRef FRDGBuilder::FindExternalBuffer(FRHIBuffer *Buffer) const {
	FRDGBuffer *const *Found = ExternalBuffers.getptr(Buffer);
	return Found ? *Found : nullptr;
}

FRDGBufferRef FRDGBuilder::FindExternalBuffer(FRDGPooledBuffer *ExternalPooledBuffer) const {
	return ExternalPooledBuffer ? FindExternalBuffer(ExternalPooledBuffer->GetRHI()) : nullptr;
}

FRDGTextureRef FRDGBuilder::RegisterExternalTexture(const TRefCountPtr<IPooledRenderTarget> &ExternalPooledTexture, ERDGTextureFlags Flags) {
	const char *Name = ExternalPooledTexture.IsValid() ? nullptr : "External";
	CharString NameUtf8;
	if (ExternalPooledTexture.IsValid()) {
		NameUtf8 = ExternalPooledTexture->GetRHI()->GetName().utf8();
		Name = NameUtf8.get_data();
	}
	return RegisterExternalTexture(ExternalPooledTexture, Name, Flags);
}

FRDGTextureRef FRDGBuilder::RegisterExternalTexture(const TRefCountPtr<IPooledRenderTarget> &ExternalPooledTexture, const char *NameIfNotRegistered, ERDGTextureFlags Flags) {
	if (!ExternalPooledTexture.IsValid() || !ExternalPooledTexture->GetRHI()) {
		RDGPrivate::ReportValidationError("Attempted to register NULL external texture.");
		return nullptr;
	}
	if (FRDGTextureRef Existing = FindExternalTexture(ExternalPooledTexture->GetRHI())) return Existing;
	if (bExecuted) RDGPrivate::ReportValidationError(vformat("Render graph operation 'RegisterExternalTexture' with resource '%s' must be performed prior to graph execution.", NameIfNotRegistered));

	FRDGTexture *Texture = AllocObjectInternal<FRDGTexture>(CopyName(NameIfNotRegistered), ExternalPooledTexture->GetDesc(), Flags);
	Texture->Index = Textures.size();
	Texture->bExternal = true;
	Texture->bProduced = true;
	Texture->bAllocated = true;
	Texture->PooledRenderTarget = ExternalPooledTexture;
	Texture->RHI = ExternalPooledTexture->GetRHI();
	Textures.push_back(Texture);
	ExternalTextures.insert(Texture->RHI, Texture);
	AllocatedTextureRIDs.insert(Texture, Texture->RHI->GetRID());
	return Texture;
}

FRDGBufferRef FRDGBuilder::RegisterExternalBuffer(const TRefCountPtr<FRDGPooledBuffer> &ExternalPooledBuffer, ERDGBufferFlags Flags) {
	CharString NameUtf8;
	if (ExternalPooledBuffer.IsValid()) NameUtf8 = ExternalPooledBuffer->GetRHI()->GetName().utf8();
	return RegisterExternalBuffer(ExternalPooledBuffer, ExternalPooledBuffer.IsValid() ? NameUtf8.get_data() : "External", Flags);
}

FRDGBufferRef FRDGBuilder::RegisterExternalBuffer(const TRefCountPtr<FRDGPooledBuffer> &ExternalPooledBuffer, const char *NameIfNotRegistered, ERDGBufferFlags Flags) {
	if (!ExternalPooledBuffer.IsValid() || !ExternalPooledBuffer->GetRHI()) {
		RDGPrivate::ReportValidationError("Attempted to register NULL external buffer.");
		return nullptr;
	}
	if (FRDGBufferRef Existing = FindExternalBuffer(ExternalPooledBuffer->GetRHI())) return Existing;

	FRDGBuffer *Buffer = AllocObjectInternal<FRDGBuffer>(CopyName(NameIfNotRegistered), ExternalPooledBuffer->Desc, Flags);
	Buffer->Index = Buffers.size();
	Buffer->bExternal = true;
	Buffer->bProduced = true;
	Buffer->bAllocated = true;
	Buffer->PooledBuffer = ExternalPooledBuffer;
	Buffer->RHI = ExternalPooledBuffer->GetRHI();
	Buffers.push_back(Buffer);
	ExternalBuffers.insert(Buffer->RHI, Buffer);
	AllocatedBufferRIDs.insert(Buffer, Buffer->RHI->GetRID());
	return Buffer;
}

FRDGTextureRef FRDGBuilder::CreateTexture(const FRDGTextureDesc &Desc, const char *Name, ERDGTextureFlags Flags) {
	if (RDGPrivate::IsValidationEnabled()) {
		if (!Name) RDGPrivate::ReportValidationError("Creating a texture requires a valid debug name.");
		if (!Desc.IsValid()) RDGPrivate::ReportValidationError(vformat("Texture %s was created with an invalid descriptor (%dx%dx%d, %d mips, format %s).", Name ? Name : "Unnamed", Desc.Extent.X, Desc.Extent.Y, Desc.Depth, Desc.NumMips, GetPixelFormatInfo(Desc.Format).Name));
		if (EnumHasAnyFlags(Desc.Flags, TexCreate_Presentable)) RDGPrivate::ReportValidationError(vformat("Illegal to create texture %s with presentable flag.", Name));
		if (EnumHasAnyFlags(Desc.Flags, TexCreate_UAV) && Desc.NumSamples > 1) RDGPrivate::ReportValidationError(vformat("TexCreate_UAV is not allowed on MSAA texture %s.", Name));
		if (EnumHasAnyFlags(Flags, ERDGTextureFlags::SkipTracking)) RDGPrivate::ReportValidationError(vformat("Cannot create texture %s with the SkipTracking flag. Only registered textures can use this flag.", Name));
		if (bExecuted) RDGPrivate::ReportValidationError(vformat("Render graph operation 'CreateTexture' with resource '%s' must be performed prior to graph execution.", Name));
	}
	FRDGTexture *Texture = AllocObjectInternal<FRDGTexture>(CopyName(Name), Desc, Flags);
	Texture->Index = Textures.size();
	Textures.push_back(Texture);
	return Texture;
}

FRDGBufferRef FRDGBuilder::CreateBuffer(const FRDGBufferDesc &Desc, const char *Name, ERDGBufferFlags Flags) {
	if (RDGPrivate::IsValidationEnabled()) {
		if (!Name) RDGPrivate::ReportValidationError("Creating a buffer requires a valid debug name.");
		if (Desc.GetSize() == 0) RDGPrivate::ReportValidationError(vformat("Creating buffer '%s' is zero bytes in size.", Name));
		if (EnumHasAllFlags(Desc.Usage, BUF_StructuredBuffer | BUF_ByteAddressBuffer) && Desc.BytesPerElement != 4) RDGPrivate::ReportValidationError(vformat("Creating buffer '%s' as a structured buffer that is also byte addressable, BytesPerElement must be 4! Instead it is %d", Name, Desc.BytesPerElement));
		if (EnumHasAnyFlags(Flags, ERDGBufferFlags::SkipTracking)) RDGPrivate::ReportValidationError(vformat("Cannot create buffer %s with the SkipTracking flag. Only registered buffers can use this flag.", Name));
	}
	FRDGBuffer *Buffer = AllocObjectInternal<FRDGBuffer>(CopyName(Name), Desc, Flags);
	Buffer->Index = Buffers.size();
	Buffers.push_back(Buffer);
	return Buffer;
}

FRDGBufferRef FRDGBuilder::CreateBuffer(const FRDGBufferDesc &Desc, const char *Name, FRDGBufferNumElementsCallback &&NumElementsCallback, ERDGBufferFlags Flags) {
	FRDGBufferRef Buffer = CreateBuffer(Desc, Name, Flags);
	if (NumElementsCallback) {
		Buffer->NumElementsCallback = std::move(NumElementsCallback);
		NumElementsCallbackBuffers.push_back(Buffer);
	}
	return Buffer;
}

FRDGTextureSRVRef FRDGBuilder::CreateSRV(const FRDGTextureSRVDesc &Desc) {
	if (!Desc.Texture) {
		RDGPrivate::ReportValidationError("Texture SRV created with a null texture.");
		return nullptr;
	}
	if (RDGPrivate::IsValidationEnabled() && Desc.MipLevel >= Desc.Texture->Desc.NumMips) RDGPrivate::ReportValidationError(vformat("Failed to create SRV at mip %d: the texture %s has only %d mip levels.", Desc.MipLevel, Desc.Texture->Name, Desc.Texture->Desc.NumMips));
	FRDGTextureSRV *SRV = AllocObjectInternal<FRDGTextureSRV>(Desc.Texture->Name, Desc);
	Views.push_back(SRV);
	return SRV;
}

FRDGBufferSRVRef FRDGBuilder::CreateSRV(const FRDGBufferSRVDesc &Desc) {
	if (!Desc.Buffer) {
		RDGPrivate::ReportValidationError("Buffer SRV created with a null buffer.");
		return nullptr;
	}
	FRDGBufferSRV *SRV = AllocObjectInternal<FRDGBufferSRV>(Desc.Buffer->Name, Desc);
	Views.push_back(SRV);
	return SRV;
}

FRDGTextureUAVRef FRDGBuilder::CreateUAV(const FRDGTextureUAVDesc &Desc, ERDGUnorderedAccessViewFlags Flags) {
	if (!Desc.Texture) {
		RDGPrivate::ReportValidationError("Texture UAV created with a null texture.");
		return nullptr;
	}
	if (RDGPrivate::IsValidationEnabled()) {
		if (!EnumHasAnyFlags(Desc.Texture->Desc.Flags, TexCreate_UAV)) RDGPrivate::ReportValidationError(vformat("Attempted to create UAV from texture %s which was not created with TexCreate_UAV", Desc.Texture->Name));
		if (Desc.MipLevel >= Desc.Texture->Desc.NumMips) RDGPrivate::ReportValidationError(vformat("Failed to create UAV at mip %d: the texture %s has only %d mip levels.", Desc.MipLevel, Desc.Texture->Name, Desc.Texture->Desc.NumMips));
	}
	FRDGTextureUAV *UAV = AllocObjectInternal<FRDGTextureUAV>(Desc.Texture->Name, Desc, Flags);
	Views.push_back(UAV);
	return UAV;
}

FRDGBufferUAVRef FRDGBuilder::CreateUAV(const FRDGBufferUAVDesc &Desc, ERDGUnorderedAccessViewFlags Flags) {
	if (!Desc.Buffer) {
		RDGPrivate::ReportValidationError("Buffer UAV created with a null buffer.");
		return nullptr;
	}
	if (RDGPrivate::IsValidationEnabled() && !EnumHasAnyFlags(Desc.Buffer->Desc.Usage, BUF_UnorderedAccess)) RDGPrivate::ReportValidationError(vformat("Attempted to create UAV from buffer %s which was not created with BUF_UnorderedAccess", Desc.Buffer->Name));
	FRDGBufferUAV *UAV = AllocObjectInternal<FRDGBufferUAV>(Desc.Buffer->Name, Desc, Flags);
	Views.push_back(UAV);
	return UAV;
}

void FRDGBuilder::RegisterUniformBuffer(FRDGUniformBuffer *UniformBuffer) {
	UniformBuffer->Builder = this;
	UniformBuffers.push_back(UniformBuffer);
}

FRDGBuilder::FUploadedBuffer *FRDGBuilder::QueueBufferUploadInternal(FRDGBufferRef Buffer) {
	ERR_FAIL_NULL_V(Buffer, nullptr);
	if (Buffer->bQueuedForUpload) {
		RDGPrivate::ReportValidationError(vformat("Buffer %s already has an upload queued. Only one upload can be done for each graph.", Buffer->Name));
		return nullptr;
	}
	Buffer->bQueuedForUpload = true;
	Buffer->bProduced = true;
	Buffer->UploadIndex = UploadedBuffers.size();
	FUploadedBuffer *UploadedBuffer = AllocObjectInternal<FUploadedBuffer>();
	UploadedBuffer->Buffer = Buffer;
	UploadedBuffers.push_back(UploadedBuffer);
	return UploadedBuffer;
}

void FRDGBuilder::QueueBufferUpload(FRDGBufferRef Buffer, const void *InitialData, uint64 InitialDataSize, ERDGInitialDataFlags InitialDataFlags) {
	if (!InitialData || InitialDataSize == 0) return;
	FUploadedBuffer *UploadedBuffer = QueueBufferUploadInternal(Buffer);
	if (!UploadedBuffer) return;
	const void *Data = InitialData;
	if (!EnumHasAnyFlags(InitialDataFlags, ERDGInitialDataFlags::NoCopy)) {
		void *Copy = Alloc(InitialDataSize, 16);
		memcpy(Copy, InitialData, InitialDataSize);
		Data = Copy;
	}
	UploadedBuffer->Data = Data;
	UploadedBuffer->DataSize = InitialDataSize;
	UploadedBuffer->bPrepared = true;
	// Immediate mode / already allocated buffers upload right away.
	if (Buffer->bAllocated && Buffer->RHI) SubmitBufferUpload(Buffer);
}

void FRDGBuilder::QueueBufferUpload(FRDGBufferRef Buffer, const void *InitialData, uint64 InitialDataSize, FRDGBufferInitialDataFreeCallback &&InitialDataFreeCallback) {
	if (InitialDataSize == 0) return;
	FUploadedBuffer *UploadedBuffer = QueueBufferUploadInternal(Buffer);
	if (!UploadedBuffer) return;
	UploadedBuffer->Data = InitialData;
	UploadedBuffer->DataSize = InitialDataSize;
	UploadedBuffer->DataFreeCallback = std::move(InitialDataFreeCallback);
	UploadedBuffer->bPrepared = true;
	if (Buffer->bAllocated && Buffer->RHI) SubmitBufferUpload(Buffer);
}

void FRDGBuilder::QueueBufferUpload(FRDGBufferRef Buffer, FRDGBufferInitialDataFillCallback &&InitialDataFillCallback) {
	FUploadedBuffer *UploadedBuffer = QueueBufferUploadInternal(Buffer);
	if (!UploadedBuffer) return;
	UploadedBuffer->DataFillCallback = std::move(InitialDataFillCallback);
	bHasUploadCallbacks = true;
	if (Buffer->bAllocated && Buffer->RHI) SubmitBufferUpload(Buffer);
}

void FRDGBuilder::QueueBufferUpload(FRDGBufferRef Buffer, FRDGBufferInitialDataCallback &&InitialDataCallback, FRDGBufferInitialDataSizeCallback &&InitialDataSizeCallback) {
	QueueBufferUpload(Buffer, std::move(InitialDataCallback), std::move(InitialDataSizeCallback), FRDGBufferInitialDataFreeCallback());
}

void FRDGBuilder::QueueBufferUpload(FRDGBufferRef Buffer, FRDGBufferInitialDataCallback &&InitialDataCallback, FRDGBufferInitialDataSizeCallback &&InitialDataSizeCallback, FRDGBufferInitialDataFreeCallback &&InitialDataFreeCallback) {
	FUploadedBuffer *UploadedBuffer = QueueBufferUploadInternal(Buffer);
	if (!UploadedBuffer) return;
	UploadedBuffer->DataCallback = std::move(InitialDataCallback);
	UploadedBuffer->DataSizeCallback = std::move(InitialDataSizeCallback);
	UploadedBuffer->DataFreeCallback = std::move(InitialDataFreeCallback);
	bHasUploadCallbacks = true;
	if (Buffer->bAllocated && Buffer->RHI) SubmitBufferUpload(Buffer);
}

void FRDGBuilder::FinalizeDescs() {
	for (FRDGBuffer *Buffer : NumElementsCallbackBuffers) Buffer->FinalizeDesc();
	NumElementsCallbackBuffers.clear();
}

void FRDGBuilder::PrepareBufferUpload(FUploadedBuffer &UploadedBuffer) {
	if (UploadedBuffer.bPrepared) return;
	UploadedBuffer.bPrepared = true;
	FRDGBuffer *Buffer = UploadedBuffer.Buffer;
	Buffer->FinalizeDesc();
	if (UploadedBuffer.DataFillCallback) {
		const uint32 Size = Buffer->Desc.GetSize();
		UploadedBuffer.FillData.resize(Size);
		UploadedBuffer.DataFillCallback(UploadedBuffer.FillData.ptr(), Size);
		UploadedBuffer.DataFillCallback = nullptr;
		UploadedBuffer.Data = UploadedBuffer.FillData.ptr();
		UploadedBuffer.DataSize = Size;
	} else if (UploadedBuffer.DataCallback) {
		UploadedBuffer.Data = UploadedBuffer.DataCallback();
		UploadedBuffer.DataSize = UploadedBuffer.DataSizeCallback ? UploadedBuffer.DataSizeCallback() : 0;
		UploadedBuffer.DataCallback = nullptr;
		UploadedBuffer.DataSizeCallback = nullptr;
	}
}

void FRDGBuilder::PrepareBufferUploads() {
	for (FUploadedBuffer *UploadedBuffer : UploadedBuffers) PrepareBufferUpload(*UploadedBuffer);
}

void FRDGBuilder::SubmitBufferUpload(FRDGBuffer *Buffer) {
	if (Buffer->UploadIndex < 0) return;
	FUploadedBuffer &UploadedBuffer = *UploadedBuffers[Buffer->UploadIndex];
	if (UploadedBuffer.bUploaded) return;
	// Resolved by the compile task, or here (immediate mode, buffers allocated during setup).
	PrepareBufferUpload(UploadedBuffer);
	UploadedBuffer.bUploaded = true;
	if (UploadedBuffer.Data && UploadedBuffer.DataSize) {
		if (UploadedBuffer.DataSize > Buffer->Desc.GetSize()) RDGPrivate::ReportValidationError(vformat("Initial data of buffer %s is %d bytes, the buffer only %d.", Buffer->Name, UploadedBuffer.DataSize, Buffer->Desc.GetSize()));
		// UpdateBuffer copies the data (now or into the deferred command).
		RHICmdList.UpdateBuffer(Buffer->RHI, 0, (uint32)FMath::Min<uint64>(UploadedBuffer.DataSize, Buffer->Desc.GetSize()), UploadedBuffer.Data);
	}
	if (UploadedBuffer.DataFreeCallback) {
		UploadedBuffer.DataFreeCallback(UploadedBuffer.Data);
		UploadedBuffer.DataFreeCallback = nullptr;
	}
	UploadedBuffer.FillData.reset();
}

void FRDGBuilder::QueueTextureExtraction(FRDGTextureRef Texture, TRefCountPtr<IPooledRenderTarget> *OutPooledTexturePtr, ERDGResourceExtractionFlags Flags) {
	ERR_FAIL_NULL(Texture);
	if (!OutPooledTexturePtr) {
		RDGPrivate::ReportValidationError(vformat("Texture %s was extracted, but the output texture pointer is null.", Texture->Name));
		return;
	}
	if (RDGPrivate::IsValidationEnabled() && !Texture->bProduced) RDGPrivate::ReportValidationError(vformat("Unable to queue the extraction of the resource %s because it has not been produced by any pass.", Texture->Name));
	Texture->bExtracted = true;
	if (Texture->bAllocated && Texture->RHI && !Texture->bExternal) Device->texture_set_discardable(Texture->RHI->GetRID(), false);
	FExtractedTexture Extracted;
	Extracted.Texture = Texture;
	Extracted.PooledTexture = OutPooledTexturePtr;
	ExtractedTextures.push_back(Extracted);
}

void FRDGBuilder::QueueBufferExtraction(FRDGBufferRef Buffer, TRefCountPtr<FRDGPooledBuffer> *OutPooledBufferPtr) {
	ERR_FAIL_NULL(Buffer);
	if (!OutPooledBufferPtr) {
		RDGPrivate::ReportValidationError(vformat("Buffer %s was extracted, but the output buffer pointer is null.", Buffer->Name));
		return;
	}
	if (RDGPrivate::IsValidationEnabled() && !Buffer->bProduced) RDGPrivate::ReportValidationError(vformat("Unable to queue the extraction of the resource %s because it has not been produced by any pass.", Buffer->Name));
	Buffer->bExtracted = true;
	FExtractedBuffer Extracted;
	Extracted.Buffer = Buffer;
	Extracted.PooledBuffer = OutPooledBufferPtr;
	ExtractedBuffers.push_back(Extracted);
}

const TRefCountPtr<IPooledRenderTarget> &FRDGBuilder::ConvertToExternalTexture(FRDGTextureRef Texture) {
	static TRefCountPtr<IPooledRenderTarget> Null;
	ERR_FAIL_NULL_V(Texture, Null);
	if (bExecuted) RDGPrivate::ReportValidationError(vformat("Unable to convert resource %s to external because passes in the graph have already executed.", Texture->Name));
	if (!Texture->bExternal) {
		// External before allocating: an external texture needs a pooled allocation of its own (never transient).
		Texture->bExternal = true;
		Texture->bForceNonTransient = true;
		AllocateTexture(Texture);
		if (Texture->RHI) {
			Device->texture_set_discardable(Texture->RHI->GetRID(), false);
			ExternalTextures.insert(Texture->RHI, Texture);
		}
	}
	return Texture->PooledRenderTarget;
}

const TRefCountPtr<FRDGPooledBuffer> &FRDGBuilder::ConvertToExternalBuffer(FRDGBufferRef Buffer) {
	static TRefCountPtr<FRDGPooledBuffer> Null;
	ERR_FAIL_NULL_V(Buffer, Null);
	if (bExecuted) RDGPrivate::ReportValidationError(vformat("Unable to convert resource %s to external because passes in the graph have already executed.", Buffer->Name));
	if (!Buffer->bExternal) {
		// The pass setup decides whether the buffer is bound as vertex / index buffer (its RD buffer type).
		if (AsyncSetupQueue.bEnabled) WaitForAsyncSetupQueue();
		Buffer->bExternal = true;
		Buffer->bForceNonTransient = true;
		AllocateBuffer(Buffer);
		if (Buffer->RHI) ExternalBuffers.insert(Buffer->RHI, Buffer);
	}
	return Buffer->PooledBuffer;
}

const TRefCountPtr<IPooledRenderTarget> &FRDGBuilder::GetPooledTexture(FRDGTextureRef Texture) const {
	return Texture->PooledRenderTarget;
}

const TRefCountPtr<FRDGPooledBuffer> &FRDGBuilder::GetPooledBuffer(FRDGBufferRef Buffer) const {
	return Buffer->PooledBuffer;
}

RID FRDGBuilder::GetAllocatedTextureRID(FRDGTextureRef Texture) const {
	const RID *Found = AllocatedTextureRIDs.getptr(Texture);
	return Found ? *Found : RID();
}

RID FRDGBuilder::GetAllocatedBufferRID(FRDGBufferRef Buffer) const {
	const RID *Found = AllocatedBufferRIDs.getptr(Buffer);
	return Found ? *Found : RID();
}

FRHITransientHeapAllocation FRDGBuilder::GetTransientAllocation(FRDGViewableResource *Resource) const {
	const FRHITransientHeapAllocation *Found = TransientAllocations.getptr(Resource);
	return Found ? *Found : FRHITransientHeapAllocation();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Allocation.

bool FRDGBuilder::IsTransient(const FRDGTexture *Texture) const {
	// UE's FRDGBuilder::IsTransient: external, extracted and multi-frame resources need memory of their own.
	if (CVarRDGTransientAllocator.GetValueOnRenderThread() == 0 || bImmediateMode) return false;
	return !Texture->bExternal && !Texture->bExtracted && !Texture->bForceNonTransient && !EnumHasAnyFlags(Texture->Flags, ERDGTextureFlags::MultiFrame);
}

bool FRDGBuilder::IsTransient(const FRDGBuffer *Buffer) const {
	if (CVarRDGTransientAllocator.GetValueOnRenderThread() == 0 || bImmediateMode) return false;
	if (EnumHasAnyFlags(Buffer->Desc.Usage, BUF_DrawIndirect) && CVarRDGTransientIndirectArgBuffers.GetValueOnRenderThread() == 0) return false;
	// Only storage buffers can be placed in transient heaps.
	if (Buffer->bVertexOrIndexBinding) return false;
	return !Buffer->bExternal && !Buffer->bExtracted && !Buffer->bForceNonTransient && !EnumHasAnyFlags(Buffer->Flags, ERDGBufferFlags::MultiFrame);
}

void FRDGBuilder::OnTransientAllocated(FRDGViewableResource *Resource, const FRHITransientHeapAllocation &Allocation) {
	TransientAllocations.insert(Resource, Allocation);
	CurrentTransientBytes += Allocation.Size;
	Stats.TransientAllocatedBytes += Allocation.Size;
	Stats.TransientPeakBytes = FMath::Max(Stats.TransientPeakBytes, CurrentTransientBytes);
}

void FRDGBuilder::AllocateTexture(FRDGTexture *Texture) {
	if (Texture->bAllocated || !DeviceContext) return;

	FRHITransientTexture *TransientTexture = IsTransient(Texture) ? DeviceContext->TransientAllocator.CreateTexture(Texture->Desc, Texture->Name, DeviceContext->GetFrameNumber(), GetAllocateFences(Texture)) : nullptr;
	if (TransientTexture) {
		Texture->TransientTexture = TransientTexture;
		Texture->RHI = TransientTexture->GetRHI();
		Texture->bAllocated = true;
		const RID TextureRID = Texture->RHI->GetRID();
		Device->texture_set_discardable(TextureRID, true);

		uint32 *Uses = PhysicalTextureUses.getptr(TextureRID);
		if (Uses) (*Uses)++;
		else PhysicalTextureUses.insert(TextureRID, 1);
		AllocatedTextureRIDs.insert(Texture, TextureRID);
		OnTransientAllocated(Texture, TransientTexture->GetHeapAllocation());
		Stats.NumTransientTextures++;
		Stats.NumPlacedTextures++;

		if (CVarRDGClobberResources.GetValueOnRenderThread()) ClobberTexture(Texture);
		return;
	}

	TRefCountPtr<FPooledRenderTarget> Element = DeviceContext->RenderTargetPool.FindFreeElement(Texture->Desc, Texture->Name, DeviceContext->GetFrameNumber());
	if (!Element.IsValid()) {
		RDGPrivate::ReportValidationError(vformat("Failed to allocate texture %s.", Texture->Name));
		return;
	}
	Texture->PooledRenderTarget = TRefCountPtr<IPooledRenderTarget>(Element.GetReference());
	Texture->RHI = Element->GetRHI();
	Texture->bAllocated = true;

	// Transient textures don't need their contents preserved across frames: let Godot's graph drop stores / loads.
	const bool bPreserveContents = Texture->bExtracted || Texture->bExternal || Texture->bForceNonTransient || EnumHasAnyFlags(Texture->Flags, ERDGTextureFlags::MultiFrame);
	const RID TextureRID = Texture->RHI->GetRID();
	Device->texture_set_discardable(TextureRID, !bPreserveContents);

	uint32 *Uses = PhysicalTextureUses.getptr(TextureRID);
	if (Uses) (*Uses)++;
	else PhysicalTextureUses.insert(TextureRID, 1);
	AllocatedTextureRIDs.insert(Texture, TextureRID);
	Stats.NumTransientTextures++;

	if (CVarRDGClobberResources.GetValueOnRenderThread()) ClobberTexture(Texture);
}

void FRDGBuilder::AllocateBuffer(FRDGBuffer *Buffer) {
	if (Buffer->bAllocated || !DeviceContext) return;
	// Resolved by the compile task, or here for buffers allocated during setup.
	Buffer->FinalizeDesc();
	FRHITransientBuffer *TransientBuffer = IsTransient(Buffer) ? DeviceContext->TransientAllocator.CreateBuffer(Buffer->Desc, Buffer->Name, DeviceContext->GetFrameNumber(), GetAllocateFences(Buffer)) : nullptr;
	if (TransientBuffer) {
		Buffer->TransientBuffer = TransientBuffer;
		Buffer->RHI = TransientBuffer->GetRHI();
		Buffer->bAllocated = true;
		OnTransientAllocated(Buffer, TransientBuffer->GetHeapAllocation());
		Stats.NumPlacedBuffers++;
	} else {
		TRefCountPtr<FRDGPooledBuffer> Element = DeviceContext->BufferPool.FindFreeBuffer(Buffer->Desc, Buffer->Name, DeviceContext->GetFrameNumber(), FRDGBufferPool::GetRDBufferType(Buffer->Desc, Buffer->bVertexOrIndexBinding));
		if (!Element.IsValid()) {
			RDGPrivate::ReportValidationError(vformat("Failed to allocate buffer %s.", Buffer->Name));
			return;
		}
		Buffer->PooledBuffer = Element;
		Buffer->RHI = Element->GetRHI();
		Buffer->bAllocated = true;
	}

	const RID BufferRID = Buffer->RHI->GetRID();
	uint32 *Uses = PhysicalBufferUses.getptr(BufferRID);
	if (Uses) (*Uses)++;
	else PhysicalBufferUses.insert(BufferRID, 1);
	AllocatedBufferRIDs.insert(Buffer, BufferRID);
	Stats.NumTransientBuffers++;

	if (Buffer->bQueuedForUpload) {
		SubmitBufferUpload(Buffer);
	} else if (CVarRDGClobberResources.GetValueOnRenderThread() && !RHICmdList.IsInsideRenderPass()) {
		RHICmdList.FlushComputeList();
		const uint32 Size = Buffer->RHI->GetSize();
		RDGPrivate::EnqueueRenderThreadCommand(Device, [=](RenderingDevice *RD) { RD->buffer_clear(BufferRID, 0, Size); });
	}
}

void FRDGBuilder::ReleaseTexture(FRDGTexture *Texture) {
	if (!Texture->bAllocated || Texture->bExternal || Texture->bExtracted) return;
	if (Texture->TransientTexture) {
		DeviceContext->TransientAllocator.DeallocateMemory(Texture->TransientTexture, GetDeallocateFences(Texture));
		CurrentTransientBytes -= Texture->TransientTexture->GetHeapAllocation().Size;
		Texture->TransientTexture = nullptr;
	}
	Texture->PooledRenderTarget.SafeRelease();
	Texture->RHI = nullptr;
	Texture->bAllocated = false;
}

void FRDGBuilder::ReleaseBuffer(FRDGBuffer *Buffer) {
	if (!Buffer->bAllocated || Buffer->bExternal || Buffer->bExtracted) return;
	if (Buffer->TransientBuffer) {
		DeviceContext->TransientAllocator.DeallocateMemory(Buffer->TransientBuffer, GetDeallocateFences(Buffer));
		CurrentTransientBytes -= Buffer->TransientBuffer->GetHeapAllocation().Size;
		Buffer->TransientBuffer = nullptr;
	}
	Buffer->PooledBuffer.SafeRelease();
	Buffer->RHI = nullptr;
	Buffer->bAllocated = false;
}

void FRDGBuilder::AcquireResource(FRDGViewableResource *Resource) {
	if (Resource->Type == ERDGViewableResourceType::Texture) AllocateTexture(static_cast<FRDGTexture *>(Resource));
	else AllocateBuffer(static_cast<FRDGBuffer *>(Resource));
}

void FRDGBuilder::ReleaseResource(FRDGViewableResource *Resource, FRDGPass *Pass) {
	const bool bTexture = Resource->Type == ERDGViewableResourceType::Texture;
	const bool bPlaced = bTexture ? static_cast<FRDGTexture *>(Resource)->TransientTexture != nullptr : static_cast<FRDGBuffer *>(Resource)->TransientBuffer != nullptr;
	const FRDGPassHandle AsyncComputePass = Resource->LastPasses[GetRHIPipelineIndex(ERHIPipeline::AsyncCompute)];
	if (!bPlaced && AsyncComputePass.IsValid()) {
		// A pooled resource used on async compute goes back to the pool once the async work joined, so no graphics pass
		// running in parallel gets it (UE extends async compute resource lifetimes to the fork / join interval). Placed
		// resources are released now: the allocation fences keep the other pipe from aliasing their memory.
		const FRDGPassHandle JoinPass = Passes[AsyncComputePass.GetIndex()]->GraphicsJoinPass;
		if (JoinPass.IsNull()) return; // Released at the end of the graph.
		if (JoinPass > Pass->Handle) {
			Passes[JoinPass.GetIndex()]->ResourcesToRelease.push_back(Resource);
			return;
		}
	}

	if (bTexture) ReleaseTexture(static_cast<FRDGTexture *>(Resource));
	else ReleaseBuffer(static_cast<FRDGBuffer *>(Resource));
}

FRHITransientAllocationFences::FInterval FRDGBuilder::GetGraphicsForkJoinInterval(const FRDGPass *Pass) const {
	// Fence values: 0 is the prologue, pass N is N + 1, and the epilogue comes after the last pass.
	FRHITransientAllocationFences::FInterval Interval;
	Interval.Min = Pass->GraphicsForkPass.IsNull() ? 0 : Pass->GraphicsForkPass.GetIndex() + 1;
	Interval.Max = Pass->GraphicsJoinPass.IsNull() ? Passes.size() + 1 : Pass->GraphicsJoinPass.GetIndex() + 1;
	return Interval;
}

FRHITransientAllocationFences FRDGBuilder::GetAllocateFences(const FRDGViewableResource *Resource) const {
	// UE's FRDGBuilder::GetAllocateFences.
	if (Resource->FirstPass.IsNull()) {
		FRHITransientAllocationFences Fences(ERHIPipeline::Graphics);
		Fences.SetGraphics(0);
		return Fences;
	}

	const FRDGPass *FirstPass = Passes[Resource->FirstPass.GetIndex()];
	FRHITransientAllocationFences Fences(FirstPass->Pipeline);
	if (FirstPass->IsAsyncCompute()) Fences.SetAsyncCompute(FirstPass->Handle.GetIndex() + 1, GetGraphicsForkJoinInterval(FirstPass));
	else Fences.SetGraphics(FirstPass->Handle.GetIndex() + 1);
	return Fences;
}

FRHITransientAllocationFences FRDGBuilder::GetDeallocateFences(const FRDGViewableResource *Resource) const {
	// UE's FRDGBuilder::GetDeallocateFences.
	FRDGPassHandle GraphicsPassHandle = Resource->LastPasses[GetRHIPipelineIndex(ERHIPipeline::Graphics)];
	FRDGPassHandle AsyncComputePassHandle = Resource->LastPasses[GetRHIPipelineIndex(ERHIPipeline::AsyncCompute)];
	ERHIPipeline Pipelines = GraphicsPassHandle.IsValid() ? ERHIPipeline::Graphics : ERHIPipeline::None;
	FRHITransientAllocationFences::FInterval GraphicsForkJoin;

	if (AsyncComputePassHandle.IsValid()) {
		Pipelines |= ERHIPipeline::AsyncCompute;
		const FRDGPass *AsyncComputePass = Passes[AsyncComputePassHandle.GetIndex()];
		GraphicsForkJoin = GetGraphicsForkJoinInterval(AsyncComputePass);

		if (GraphicsPassHandle.IsValid()) {
			const uint32 Graphics = GraphicsPassHandle.GetIndex() + 1;
			// Ignore the graphics pass if earlier than the fork to async compute, the async compute pass if earlier than the join.
			if (Graphics <= GraphicsForkJoin.Min) GraphicsPassHandle = FRDGPassHandle();
			else if (Graphics >= GraphicsForkJoin.Max) AsyncComputePassHandle = FRDGPassHandle();
		}
	}

	if (Pipelines == ERHIPipeline::None) {
		// Never used by a pass (e.g. converted to external up front): treat it as released at the prologue.
		FRHITransientAllocationFences Fences(ERHIPipeline::Graphics);
		Fences.SetGraphics(0);
		return Fences;
	}

	FRHITransientAllocationFences Fences(Pipelines);
	if (GraphicsPassHandle.IsValid()) Fences.SetGraphics(GraphicsPassHandle.GetIndex() + 1);
	if (AsyncComputePassHandle.IsValid()) Fences.SetAsyncCompute(AsyncComputePassHandle.GetIndex() + 1, GraphicsForkJoin);
	return Fences;
}

void FRDGBuilder::ClobberTexture(FRDGTexture *Texture) {
	if (RHICmdList.IsInsideRenderPass() || IsDepthOrStencilFormat(Texture->Desc.Format)) return;
	RHICmdList.FlushComputeList();
	const RID TextureRID = Texture->RHI->GetRID();
	const uint32 NumMips = Texture->Desc.NumMips;
	const uint32 NumLayers = Texture->Desc.GetRDLayerCount();
	// Recorded into the pass' recording context when allocating for a parallel pass set.
	RDGPrivate::EnqueueRenderThreadCommand(Device, [=](RenderingDevice *RD) { RD->texture_clear(TextureRID, Color(1000.0f, 1000.0f, 1000.0f, 1000.0f), 0, NumMips, 0, NumLayers); });
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Pass setup.

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Access modes (UE's UseExternalAccessMode / UseInternalAccessMode / FlushAccessModeQueue).

BEGIN_SHADER_PARAMETER_STRUCT(FAccessModePassParameters, )
RDG_TEXTURE_ACCESS_ARRAY(Textures)
RDG_BUFFER_ACCESS_ARRAY(Buffers)
END_SHADER_PARAMETER_STRUCT()

void FRDGBuilder::UseExternalAccessMode(FRDGViewableResource *Resource, ERHIAccess ReadOnlyAccess, ERHIPipeline Pipelines) {
	ERR_FAIL_NULL(Resource);
	if (!bSupportsAsyncCompute) Pipelines = ERHIPipeline::Graphics;
	FRDGViewableResource::FAccessModeState &AccessModeState = Resource->AccessModeState;

	if (RDGPrivate::IsValidationEnabled()) {
		// UE's FRDGUserValidation::ValidateUseExternalAccessMode.
		const ERHIAccess AccessMaskRaster = ERHIAccess::ResolveSrc | ERHIAccess::ResolveDst | ERHIAccess::DSVRead | ERHIAccess::DSVWrite | ERHIAccess::RTV | ERHIAccess::SRVGraphics | ERHIAccess::UAVGraphics | ERHIAccess::Present | ERHIAccess::VertexOrIndexBuffer;
		String Error;
		if (AccessModeState.bLocked) Error = vformat("Resource is locked in external access mode by the SkipTracking resource flag: %s.", Resource->Name);
		else if (!IsReadOnlyExclusiveAccess(ReadOnlyAccess)) Error = vformat("A read only access is required when use external access mode. (Resource: %s, Access: %s, Pipelines: %s)", Resource->Name, String(GetRHIAccessName(ReadOnlyAccess)), GetRHIPipelineName(Pipelines));
		else if (AccessModeState.Mode == FRDGViewableResource::EAccessMode::External && (AccessModeState.Access != ReadOnlyAccess || AccessModeState.Pipelines != Pipelines)) Error = vformat("UseExternalAccessMode called on a resource that is already in external access mode, but different parameters were used. Resource: %s, existing access: %s, requested access: %s, existing pipelines: %s, requested pipelines: %s.", Resource->Name, String(GetRHIAccessName(AccessModeState.Access)), String(GetRHIAccessName(ReadOnlyAccess)), GetRHIPipelineName(AccessModeState.Pipelines), GetRHIPipelineName(Pipelines));
		else if (!EnumHasAnyFlags(Pipelines, ERHIPipeline::Graphics) && EnumHasAnyFlags(ReadOnlyAccess, AccessMaskRaster)) Error = vformat("Raster access flags were specified for external access but the graphics pipe was not specified. Resource: %s, requested access: %s, requested pipelines: %s.", Resource->Name, String(GetRHIAccessName(ReadOnlyAccess)), GetRHIPipelineName(Pipelines));
		else if (!Resource->bExternal && !Resource->bExtracted) Error = vformat("UseExternalAccessMode is only allowed for registered, externally converted or extracted resources: %s.", Resource->Name);
		if (!Error.is_empty()) {
			RDGPrivate::ReportValidationError(Error);
			return;
		}
	}

	// Back-to-back calls with the same parameters are no-ops.
	if (AccessModeState.Mode == FRDGViewableResource::EAccessMode::External || AccessModeState.bLocked) return;

	// Queued internal -> external: the queued 'end' access mode pass must contain the passes that used the resource in its
	// previous external access mode, so it is added first.
	if (AccessModeState.bQueued) FlushAccessModeQueue();
	AccessModeQueue.push_back(Resource);
	AccessModeState.bQueued = true;
	Resource->SetExternalAccessMode(ReadOnlyAccess, Pipelines);
}

void FRDGBuilder::UseInternalAccessMode(FRDGViewableResource *Resource) {
	ERR_FAIL_NULL(Resource);
	FRDGViewableResource::FAccessModeState &AccessModeState = Resource->AccessModeState;
	if (RDGPrivate::IsValidationEnabled() && AccessModeState.bLocked) {
		RDGPrivate::ReportValidationError(vformat("Resource is locked in external access mode by the SkipTracking resource flag: %s.", Resource->Name));
		return;
	}
	if (AccessModeState.Mode == FRDGViewableResource::EAccessMode::Internal || AccessModeState.bLocked) return;

	if (AccessModeState.bQueued) {
		// The switch to external access never reached a pass: back it out.
		AccessModeQueue.erase(Resource);
		AccessModeState.bQueued = false;
	} else {
		AccessModeQueue.push_back(Resource);
		AccessModeState.bQueued = true;
	}
	AccessModeState.Mode = FRDGViewableResource::EAccessMode::Internal;
}

void FRDGBuilder::FlushAccessModeQueue() {
	if (AccessModeQueue.is_empty()) return;
	// The access mode passes go through AddPassInternal, which flushes again: take the queue first.
	LocalVector<FRDGViewableResource *> Queue = std::move(AccessModeQueue);
	AccessModeQueue.clear();

	FAccessModePassParameters *ParametersByPipeline[] = { AllocParameters<FAccessModePassParameters>(), AllocParameters<FAccessModePassParameters>() };
	const ERHIAccess AccessMaskByPipeline[] = { ERHIAccess::ReadOnlyExclusiveMask, ERHIAccess::ReadOnlyExclusiveComputeMask };
	ERHIPipeline ParameterPipelines = ERHIPipeline::None;
	LocalVector<FRDGPass::FExternalAccessOp> Ops;

	for (FRDGViewableResource *Resource : Queue) {
		FRDGViewableResource::FAccessModeState &AccessModeState = Resource->AccessModeState;
		AccessModeState.bQueued = false;
		// With the async setup queue the pass setup runs later: the mode switch is replayed after the access mode pass setup.
		if (AsyncSetupQueue.bEnabled) {
			FRDGPass::FExternalAccessOp Op;
			Op.Resource = Resource;
			Op.Mode = AccessModeState.Mode;
			Op.Access = AccessModeState.Access;
			Op.Pipelines = AccessModeState.Pipelines;
			Ops.push_back(Op);
		} else {
			AccessModeState.ActiveMode = AccessModeState.Mode;
			AccessModeState.ActiveAccess = AccessModeState.Access;
			AccessModeState.ActivePipelines = AccessModeState.Pipelines;
		}
		ParameterPipelines |= AccessModeState.Pipelines;

		for (uint32 PipelineIndex = 0; PipelineIndex < (uint32)ERHIPipeline::Num; PipelineIndex++) {
			const ERHIPipeline Pipeline = (ERHIPipeline)(1u << PipelineIndex);
			if (!EnumHasAnyFlags(AccessModeState.Pipelines, Pipeline)) continue;
			const ERHIAccess Access = AccessModeState.Access & AccessMaskByPipeline[PipelineIndex];
			if (Access == ERHIAccess::None) {
				RDGPrivate::ReportValidationError(vformat("External access %s of resource %s has no state valid on the %s pipe.", String(GetRHIAccessName(AccessModeState.Access)), Resource->Name, GetRHIPipelineName(Pipeline)));
				continue;
			}
			if (Resource->Type == ERDGViewableResourceType::Texture) ParametersByPipeline[PipelineIndex]->Textures.push_back(FRDGTextureAccess(static_cast<FRDGTexture *>(Resource), Access));
			else ParametersByPipeline[PipelineIndex]->Buffers.push_back(FRDGBufferAccess(static_cast<FRDGBuffer *>(Resource), Access));
		}
	}

	auto AddAccessModePass = [&](ERHIPipeline Pipeline, ERDGPassFlags Flags) {
		FAccessModePassParameters *Parameters = ParametersByPipeline[GetRHIPipelineIndex(Pipeline)];
		auto ExecuteLambda = [](FRDGAsyncTask, FRHIComputeCommandList &) {};
		using LambdaPassType = TRDGLambdaPass<FAccessModePassParameters, decltype(ExecuteLambda)>;
		// Not through AddPass: the flags must not be overridden.
		FRDGPass *Pass = AllocObjectInternal<LambdaPassType>(RDG_EVENT_NAME("AccessModePass[%s] (Textures: %d, Buffers: %d)", Pipeline == ERHIPipeline::Graphics ? "Graphics" : "AsyncCompute", Parameters->Textures.size(), Parameters->Buffers.size()), FAccessModePassParameters::FTypeInfo::GetStructMetadata(), Parameters, Flags, std::move(ExecuteLambda));
		Pass->ExternalAccessOps = std::move(Ops);
		Ops.clear();
		Pass->bExternalAccessPass = true;
		AddPassInternal(Pass);
	};
	// The graphics pass uses all of the work flags so that any access is valid.
	if (EnumHasAnyFlags(ParameterPipelines, ERHIPipeline::Graphics)) AddAccessModePass(ERHIPipeline::Graphics, ERDGPassFlags::Copy | ERDGPassFlags::Compute | ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass | ERDGPassFlags::NeverCull);
	if (EnumHasAnyFlags(ParameterPipelines, ERHIPipeline::AsyncCompute)) AddAccessModePass(ERHIPipeline::AsyncCompute, ERDGPassFlags::AsyncCompute | ERDGPassFlags::NeverCull);
}

void FRDGBuilder::AddPassDependency(FRDGPass *Producer, FRDGPass *Consumer) {
	ERR_FAIL_COND(!Producer || !Consumer);
	FExplicitDependency Dependency;
	Dependency.Producer = Producer;
	Dependency.Consumer = Consumer;
	ExplicitDependencies.push_back(Dependency);
}

namespace {
struct FPassSetupVisitor final : public RDGPrivate::FGraphResourceVisitor {
	FRDGPass *Pass = nullptr;
	LocalVector<FRDGPass::FTextureState> *TextureStates = nullptr;
	LocalVector<FRDGPass::FBufferState> *BufferStates = nullptr;
	LocalVector<FRDGUniformBuffer *> *UniformBuffers = nullptr;
	LocalVector<FRDGTexture *> ReadWithoutProducerTextures;
	LocalVector<FRDGBuffer *> ReadWithoutProducerBuffers;
	bool bRenderPassOnlyWrites = true;
	/** bProduced belongs to the setup timeline of the user: not read by the async setup queue task. */
	bool bCheckProducers = true;

	void OnTexture(FRDGTexture *Texture, ERHIAccess Access, bool bRenderTarget, FRDGTextureSubresourceRange Range, const char *MemberName) override {
		if (bCheckProducers && !IsWritableAccess(Access) && !Texture->HasBeenProduced()) ReadWithoutProducerTextures.push_back(Texture);
		if (IsWritableAccess(Access) && !bRenderTarget) bRenderPassOnlyWrites = false;
		for (FRDGPass::FTextureState &State : *TextureStates) {
			if (State.Texture == Texture) {
				State.Access |= Access;
				State.ReferenceCount++;
				return;
			}
		}
		FRDGPass::FTextureState State;
		State.Texture = Texture;
		State.Access = Access;
		State.ReferenceCount = 1;
		TextureStates->push_back(State);
	}

	void OnBuffer(FRDGBuffer *Buffer, ERHIAccess Access, const char *MemberName) override {
		if (bCheckProducers && !IsWritableAccess(Access) && !Buffer->HasBeenProduced()) ReadWithoutProducerBuffers.push_back(Buffer);
		if (IsWritableAccess(Access)) bRenderPassOnlyWrites = false;
		for (FRDGPass::FBufferState &State : *BufferStates) {
			if (State.Buffer == Buffer) {
				State.Access |= Access;
				State.ReferenceCount++;
				return;
			}
		}
		FRDGPass::FBufferState State;
		State.Buffer = Buffer;
		State.Access = Access;
		State.ReferenceCount = 1;
		BufferStates->push_back(State);
	}

	void OnUniformBuffer(FRDGUniformBuffer *UniformBuffer) override {
		if (!UniformBuffers->has(UniformBuffer)) UniformBuffers->push_back(UniformBuffer);
	}
};
} //namespace

void FRDGBuilder::SetupPassResources(FRDGPass *Pass) {
	const FShaderParametersMetadata *Metadata = Pass->ParameterMetadata;
	const ERDGPassFlags Flags = Pass->Flags;
	const bool bValidate = RDGPrivate::IsValidationEnabled();

	FPassSetupVisitor Visitor;
	Visitor.Pass = Pass;
	Visitor.TextureStates = &Pass->TextureStates;
	Visitor.BufferStates = &Pass->BufferStates;
	Visitor.UniformBuffers = &Pass->UniformBuffers;
	Visitor.bCheckProducers = !AsyncSetupQueue.bEnabled;
	RDGPrivate::EnumerateGraphResources(Metadata, Pass->ParameterStruct, Flags, Visitor);

	// Resources in external access mode are not tracked by the passes using them (only by the access mode passes).
	if (!Pass->bExternalAccessPass) {
		auto ValidateExternalAccess = [Pass](FRDGViewableResource *Resource, ERHIAccess Access) {
			// UE's FRDGUserValidation::ValidateExternalAccess.
			const FRDGViewableResource::FAccessModeState &AccessModeState = Resource->AccessModeState;
			if (!EnumHasAnyFlags(AccessModeState.ActiveAccess, Access)) RDGPrivate::ReportValidationError(vformat("Resource %s is in external access mode and is valid for access with the following states: %s, but is being used in pass %s with access %s.", Resource->Name, String(GetRHIAccessName(AccessModeState.ActiveAccess)), Pass->GetName(), String(GetRHIAccessName(Access))));
			if (!EnumHasAnyFlags(AccessModeState.ActivePipelines, Pass->Pipeline)) RDGPrivate::ReportValidationError(vformat("Resource %s is in external access mode and is valid for access on the following pipelines: %s, but is being used on the %s pipe in pass %s.", Resource->Name, GetRHIPipelineName(AccessModeState.ActivePipelines), GetRHIPipelineName(Pass->Pipeline), Pass->GetName()));
		};
		for (FRDGPass::FTextureState &State : Pass->TextureStates) {
			if (!State.Texture->AccessModeState.IsExternalAccess()) continue;
			State.bExternalAccess = true;
			if (bValidate && !State.Texture->AccessModeState.bLocked) ValidateExternalAccess(State.Texture, State.Access);
		}
		for (FRDGPass::FBufferState &State : Pass->BufferStates) {
			if (!State.Buffer->AccessModeState.IsExternalAccess()) continue;
			State.bExternalAccess = true;
			if (bValidate && !State.Buffer->AccessModeState.bLocked) ValidateExternalAccess(State.Buffer, State.Access);
		}
	}

	Pass->bEmptyParameters = Pass->TextureStates.is_empty() && Pass->BufferStates.is_empty();
	Pass->bRenderPassOnlyWrites = Visitor.bRenderPassOnlyWrites;
	Pass->bHasExternalOutputs = Metadata && Metadata->HasExternalOutputs();

	if (bValidate) {
		const char *PassName = Pass->GetName();
		const bool bHasGraphParameters = Metadata && (Metadata->HasGraphResources() || Metadata->HasExternalOutputs());
		const bool bAnyQueueFlag = EnumHasAnyFlags(Flags, ERDGPassFlags::Copy | ERDGPassFlags::Compute | ERDGPassFlags::AsyncCompute | ERDGPassFlags::Raster);
		if (!Pass->ParameterStruct) RDGPrivate::ReportValidationError(vformat("Pass '%s' created with null parameters.", PassName));
		if (!bHasGraphParameters && bAnyQueueFlag) RDGPrivate::ReportValidationError(vformat("Pass %s may not specify any of the (Copy, Compute, AsyncCompute, Raster) flags, because it has no parameters. Use None instead.", PassName));
		if (bHasGraphParameters && !bAnyQueueFlag) RDGPrivate::ReportValidationError(vformat("Pass %s must specify at least one of the following flags: (Copy, Compute, AsyncCompute, Raster)", PassName));
		if (EnumHasAllFlags(Flags, ERDGPassFlags::Compute | ERDGPassFlags::AsyncCompute)) RDGPrivate::ReportValidationError(vformat("Pass %s specified both Compute and AsyncCompute. They are mutually exclusive.", PassName));
		if (EnumHasAllFlags(Flags, ERDGPassFlags::Raster | ERDGPassFlags::AsyncCompute)) RDGPrivate::ReportValidationError(vformat("Pass %s specified both Raster and AsyncCompute. They are mutually exclusive.", PassName));
		if (EnumHasAnyFlags(Flags, ERDGPassFlags::SkipRenderPass) && !EnumHasAnyFlags(Flags, ERDGPassFlags::Raster)) RDGPrivate::ReportValidationError(vformat("Pass %s specified SkipRenderPass without Raster. Only raster passes support this flag.", PassName));
		if (EnumHasAnyFlags(Flags, ERDGPassFlags::NeverMerge) && !EnumHasAnyFlags(Flags, ERDGPassFlags::Raster)) RDGPrivate::ReportValidationError(vformat("Pass %s specified NeverMerge without Raster. Only raster passes support this flag.", PassName));
		if (!bHasGraphParameters && !EnumHasAnyFlags(Flags, ERDGPassFlags::NeverCull) && CVarRDGDebug.GetValueOnRenderThread()) RDGPrivate::ReportValidationWarning(vformat("Pass '%s' has no graph parameters defined on its parameter struct and did not specify 'NeverCull'. The pass will always be culled.", PassName));

		const bool bHasRenderTargets = Metadata && Metadata->HasRenderTargets();
		if (bHasRenderTargets && !EnumHasAnyFlags(Flags, ERDGPassFlags::Raster)) {
			const FRenderTargetBindingSlots &RenderTargets = *reinterpret_cast<const FRenderTargetBindingSlots *>(reinterpret_cast<const uint8 *>(Pass->ParameterStruct) + Metadata->GetRenderTargetsOffset());
			if (RenderTargets.GetActiveCount() > 0 || RenderTargets.DepthStencil.GetTexture()) RDGPrivate::ReportValidationError(vformat("Pass '%s' has render target binding slots but is not set to 'Raster'.", PassName));
		}
		if (EnumHasAnyFlags(Flags, ERDGPassFlags::Raster) && !EnumHasAnyFlags(Flags, ERDGPassFlags::SkipRenderPass) && !bHasRenderTargets) RDGPrivate::ReportValidationError(vformat("Pass '%s' is set to 'Raster' but is missing render target binding slots. Use 'SkipRenderPass' if this is desired.", PassName));

		if (bHasRenderTargets && EnumHasAnyFlags(Flags, ERDGPassFlags::Raster)) {
			const FRenderTargetBindingSlots &RenderTargets = *reinterpret_cast<const FRenderTargetBindingSlots *>(reinterpret_cast<const uint8 *>(Pass->ParameterStruct) + Metadata->GetRenderTargetsOffset());
			bool bFoundEmpty = false;
			for (uint32 Index = 0; Index < MaxSimultaneousRenderTargets; Index++) {
				FRDGTexture *Texture = RenderTargets.Output[Index].GetTexture();
				if (!Texture) {
					bFoundEmpty = true;
					continue;
				}
				if (bFoundEmpty) RDGPrivate::ReportValidationError("Render targets must be packed. No empty spaces in the array.");
				if (!EnumHasAnyFlags(Texture->Desc.Flags, TexCreate_RenderTargetable | TexCreate_ResolveTargetable)) RDGPrivate::ReportValidationError(vformat("Pass '%s' attempted to bind texture '%s' as a render target, but the texture has not been created with TexCreate_RenderTargetable.", PassName, Texture->Name));
			}
			if (FRDGTexture *DepthTexture = RenderTargets.DepthStencil.GetTexture()) {
				if (!EnumHasAnyFlags(DepthTexture->Desc.Flags, TexCreate_DepthStencilTargetable)) RDGPrivate::ReportValidationError(vformat("Pass '%s' attempted to bind texture '%s' as a depth stencil render target, but the texture has not been created with TexCreate_DepthStencilTargetable.", PassName, DepthTexture->Name));
			}
		}

		for (FRDGTexture *Texture : Visitor.ReadWithoutProducerTextures) RDGPrivate::ReportValidationError(vformat("Pass %s has a read dependency on %s, but it was never written to.", PassName, Texture->Name));
		for (FRDGBuffer *Buffer : Visitor.ReadWithoutProducerBuffers) RDGPrivate::ReportValidationError(vformat("Pass %s has a read dependency on %s, but it was never written to.", PassName, Buffer->Name));
	}

	// With the async setup queue, MarkResourcesAsProduced did it when the pass was added.
	if (!AsyncSetupQueue.bEnabled) {
		for (const FRDGPass::FTextureState &State : Pass->TextureStates) {
			if (IsWritableAccess(State.Access)) State.Texture->bProduced = true;
		}
	}
	for (const FRDGPass::FBufferState &State : Pass->BufferStates) {
		if (IsWritableAccess(State.Access) && !AsyncSetupQueue.bEnabled) State.Buffer->bProduced = true;
		if (EnumHasAnyFlags(State.Access, ERHIAccess::VertexOrIndexBuffer)) State.Buffer->bVertexOrIndexBinding = true;
	}
	for (const FRDGPass::FExternalAccessOp &Op : Pass->ExternalAccessOps) {
		Op.Resource->AccessModeState.ActiveMode = Op.Mode;
		Op.Resource->AccessModeState.ActiveAccess = Op.Access;
		Op.Resource->AccessModeState.ActivePipelines = Op.Pipelines;
	}
	Pass->bSetupDone = true;
}

void FRDGBuilder::MarkResourcesAsProduced(FRDGPass *Pass) {
	// UE's MarkResourcesAsProduced (and the read dependency validation of ValidateAddPass), on the user timeline.
	struct FVisitor final : public RDGPrivate::FGraphResourceVisitor {
		LocalVector<FRDGViewableResource *> Produced;
		LocalVector<FRDGViewableResource *> ReadWithoutProducer;

		void Visit(FRDGViewableResource *Resource, ERHIAccess Access) {
			if (IsWritableAccess(Access)) Produced.push_back(Resource);
			else if (!Resource->HasBeenProduced()) ReadWithoutProducer.push_back(Resource);
		}
		void OnTexture(FRDGTexture *Texture, ERHIAccess Access, bool bRenderTarget, FRDGTextureSubresourceRange Range, const char *MemberName) override { Visit(Texture, Access); }
		void OnBuffer(FRDGBuffer *Buffer, ERHIAccess Access, const char *MemberName) override { Visit(Buffer, Access); }
	} Visitor;
	RDGPrivate::EnumerateGraphResources(Pass->ParameterMetadata, Pass->ParameterStruct, Pass->Flags, Visitor);
	if (RDGPrivate::IsValidationEnabled()) {
		for (FRDGViewableResource *Resource : Visitor.ReadWithoutProducer) RDGPrivate::ReportValidationError(vformat("Pass %s has a read dependency on %s, but it was never written to.", Pass->GetName(), Resource->Name));
	}
	for (FRDGViewableResource *Resource : Visitor.Produced) Resource->bProduced = true;
}

void FRDGBuilder::FlushSetupQueue() {
	if (AsyncSetupQueue.bEnabled) LaunchAsyncSetupQueueTask();
}

void FRDGBuilder::LaunchAsyncSetupQueueTask() {
	// UE: a task is launched when the previous one completed. Passes queued while it winds down wait for the next flush,
	// or for Execute().
	if (!AsyncSetupQueue.LastTask.IsCompleted()) return;
	AsyncSetupQueue.LastTask.Wait();
	AsyncSetupQueue.LastTask = UE::Tasks::Launch("FRDGBuilder::ProcessAsyncSetupQueue", [this]() { ProcessAsyncSetupQueue(true); }, {}, UE::Tasks::ETaskPriority::High);
}

void FRDGBuilder::ProcessAsyncSetupQueue(bool bOnTask) {
	while (true) {
		LocalVector<FRDGPass *> PoppedPasses;
		{
			MutexLock Lock(AsyncSetupQueue.Lock);
			PoppedPasses = std::move(AsyncSetupQueue.Passes);
			AsyncSetupQueue.Passes.clear();
		}
		if (PoppedPasses.is_empty()) break;
		for (FRDGPass *Pass : PoppedPasses) SetupPassResources(Pass);
		if (bOnTask) AsyncSetupQueue.NumPassesOnTask += PoppedPasses.size();
	}
}

void FRDGBuilder::WaitForAsyncSetupQueue() {
	AsyncSetupQueue.LastTask.Wait();
	AsyncSetupQueue.LastTask = UE::Tasks::FTask();
	ProcessAsyncSetupQueue(false);
}

void FRDGBuilder::WaitForParallelSetupTasks(ERDGSetupTaskWaitPoint WaitPoint) {
	auto WaitForTasks = [this](ERDGSetupTaskWaitPoint Point) {
		LocalVector<UE::Tasks::FTask> &Tasks = ParallelSetup.Tasks[(int32)Point];
		UE::Tasks::Wait(Tasks);
		Tasks.clear();
	};
	// Execute also flushes the compile tasks added after the compile wait point.
	if (WaitPoint == ERDGSetupTaskWaitPoint::Execute) WaitForTasks(ERDGSetupTaskWaitPoint::Execute);
	WaitForTasks(ERDGSetupTaskWaitPoint::Compile);
}

FRDGPassRef FRDGBuilder::AddPassInternal(FRDGPass *Pass) {
	if (bExecuted) {
		RDGPrivate::ReportValidationError(vformat("Render graph operation 'AddPass' with pass '%s' must be performed prior to graph execution.", Pass->GetName()));
		return Pass;
	}
	FlushAccessModeQueue();
	Pass->Handle = FRDGPassHandle(Passes.size());
	Pass->Scope = CurrentScope;
	Pass->Pipeline = EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::AsyncCompute) ? ERHIPipeline::AsyncCompute : ERHIPipeline::Graphics;
	if (Pass->IsAsyncCompute()) AsyncComputePassCount++;
	Passes.push_back(Pass);
	if (AsyncSetupQueue.bEnabled) {
		MarkResourcesAsProduced(Pass);
		MutexLock Lock(AsyncSetupQueue.Lock);
		AsyncSetupQueue.Passes.push_back(Pass);
	} else {
		SetupPassResources(Pass);
	}

	if (bImmediateMode && Device) {
		// Immediate mode: no culling, no aliasing; every referenced resource is allocated now and kept until Execute().
		for (const FRDGPass::FTextureState &State : Pass->TextureStates) Pass->ResourcesToAcquire.push_back(State.Texture);
		for (const FRDGPass::FBufferState &State : Pass->BufferStates) Pass->ResourcesToAcquire.push_back(State.Buffer);
		ExecutePass(Pass);
	}
	return Pass;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Compile.

void FRDGBuilder::AddCullingDependency(FRDGPass *Producer, FRDGPass *Consumer) {
	if (Consumer->Producers.has(Producer)) return;
	Consumer->Producers.push_back(Producer);

	if (Producer->Pipeline != Consumer->Pipeline) {
		// Consumers could be culled, so all of them are stored, sorted.
		LocalVector<FRDGPassHandle> &Consumers = Producer->CrossPipelineConsumers;
		uint32 Index = 0;
		while (Index < Consumers.size() && Consumers[Index] < Consumer->Handle) Index++;
		if (Index == Consumers.size() || Consumers[Index] != Consumer->Handle) Consumers.insert(Index, Consumer->Handle);

		// The latest producer on the other pipeline for the consumer.
		if (Consumer->CrossPipelineProducer.IsNull() || Producer->Handle > Consumer->CrossPipelineProducer) Consumer->CrossPipelineProducer = Producer->Handle;
	}
}

void FRDGBuilder::Compile() {
	const bool bCullPasses = CVarRDGCullPasses.GetValueOnRenderThread() != 0;

	for (FRDGTexture *Texture : Textures) {
		Texture->LastProducer = nullptr;
		Texture->LastReaders[0] = Texture->LastReaders[1] = nullptr;
	}
	for (FRDGBuffer *Buffer : Buffers) {
		Buffer->LastProducer = nullptr;
		Buffer->LastReaders[0] = Buffer->LastReaders[1] = nullptr;
	}
	for (const FExplicitDependency &Dependency : ExplicitDependencies) AddCullingDependency(Dependency.Producer, Dependency.Consumer);

	LocalVector<FRDGPass *> CullStack;
	auto FlushCullStack = [&]() {
		while (!CullStack.is_empty()) {
			FRDGPass *Pass = CullStack[CullStack.size() - 1];
			CullStack.resize(CullStack.size() - 1);
			if (!Pass->bCulled) continue;
			Pass->bCulled = false;
			for (FRDGPass *Producer : Pass->Producers) CullStack.push_back(Producer);
		}
	};

	// Tracks the producer / consumer dependencies of a resource state (UE's AddCullingDependency): consumers depend on the
	// last producer, and a write depends on the last read of the other pipe (needed for async compute fencing).
	auto AddResourceDependencies = [&](FRDGPass *Pass, FRDGViewableResource *Resource, ERHIAccess Access) {
		const uint32 PipelineIndex = GetRHIPipelineIndex(Pass->Pipeline);
		if (Resource->LastProducer && Resource->LastProducer != Pass) AddCullingDependency(Resource->LastProducer, Pass);
		if (!IsWritableAccess(Access)) {
			Resource->LastReaders[PipelineIndex] = Pass;
			return false;
		}
		FRDGPass *OtherPipelineReader = Resource->LastReaders[1 - PipelineIndex];
		if (OtherPipelineReader && OtherPipelineReader != Pass) AddCullingDependency(OtherPipelineReader, Pass);
		Resource->LastProducer = Pass;
		Resource->LastReaders[0] = Resource->LastReaders[1] = nullptr;
		return Resource->IsCullRoot();
	};

	// Producer / consumer dependencies. Passes are culled by default and un-culled by walking back from the roots
	// (passes writing external or extracted resources, or flagged NeverCull), exactly like UE.
	for (FRDGPass *Pass : Passes) {
		bool bIsCullRootProducer = false;
		for (const FRDGPass::FTextureState &State : Pass->TextureStates) {
			if (!State.bExternalAccess) bIsCullRootProducer |= AddResourceDependencies(Pass, State.Texture, State.Access);
		}
		for (const FRDGPass::FBufferState &State : Pass->BufferStates) {
			if (!State.bExternalAccess) bIsCullRootProducer |= AddResourceDependencies(Pass, State.Buffer, State.Access);
		}

		Pass->bCulled = bCullPasses;
		if (bCullPasses && (bIsCullRootProducer || Pass->bHasExternalOutputs || EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::NeverCull))) {
			CullStack.push_back(Pass);
			FlushCullStack();
		}
	}

	CompileAsyncCompute();
	CompileLifetimes();
	if (CVarRDGMergeRenderPasses.GetValueOnRenderThread()) MergeRenderPasses();
	SetupParallelExecute();
}

void FRDGBuilder::SetupParallelExecute() {
	ParallelPassGroups.clear();
	ParallelPassSets.clear();
	for (FRDGPass *Pass : Passes) Pass->ParallelPassSetIndex = -1;
	if (!bParallelExecute) return;

	const uint32 PassMin = (uint32)FMath::Max(1, CVarRDGParallelExecutePassMin.GetValueOnRenderThread());
	const uint32 PassMax = (uint32)FMath::Max(1, CVarRDGParallelExecutePassMax.GetValueOnRenderThread());

	// UE's ERDGPassTaskMode::Inline: passes recorded on the render thread. Besides NeverParallel and immediate command
	// list lambdas, the async compute fork / join points and passes without parameters (arbitrary render thread code)
	// stay inline.
	auto IsInlinePass = [](const FRDGPass *Pass) {
		return EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::NeverParallel) || !EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::Copy | ERDGPassFlags::Compute | ERDGPassFlags::Raster) ||
				Pass->IsAsyncCompute() || Pass->bGraphicsFork || Pass->bGraphicsJoin || Pass->RequiresImmediateCommandList();
	};

	struct FGroup {
		uint32 FirstPassIndex = 0;
		uint32 LastPassIndex = 0;
		uint32 NumPasses = 0;
		bool bInline = false;
	};
	LocalVector<FGroup> Groups;
	bool bInsideMergedRenderPass = false;
	for (uint32 PassIndex = 0; PassIndex < Passes.size(); PassIndex++) {
		const FRDGPass *Pass = Passes[PassIndex];
		if (Pass->bCulled) continue;
		if (bInsideMergedRenderPass) {
			FGroup &Group = Groups[Groups.size() - 1];
			Group.LastPassIndex = PassIndex;
			Group.NumPasses++;
			Group.bInline |= IsInlinePass(Pass);
			if (Pass->IsMergedRenderPassEnd()) bInsideMergedRenderPass = false;
			continue;
		}
		FGroup Group;
		Group.FirstPassIndex = Group.LastPassIndex = PassIndex;
		Group.NumPasses = 1;
		Group.bInline = IsInlinePass(Pass);
		Groups.push_back(Group);
		bInsideMergedRenderPass = Pass->IsMergedRenderPassBegin();
	}

	// Sets: runs of contiguous parallel groups, split every PassMax passes. Runs shorter than PassMin stay inline.
	LocalVector<uint32> Candidates;
	uint32 NumCandidatePasses = 0;
	auto FlushCandidates = [&]() {
		if (!Candidates.is_empty() && NumCandidatePasses >= PassMin) {
			FParallelPassSet Set;
			Set.Builder = this;
			Set.FirstGroupIndex = ParallelPassGroups.size();
			const int32 SetIndex = ParallelPassSets.size();
			for (const uint32 GroupIndex : Candidates) {
				const FGroup &Group = Groups[GroupIndex];
				FParallelPassGroup ParallelGroup;
				ParallelGroup.FirstPassIndex = Group.FirstPassIndex;
				ParallelGroup.LastPassIndex = Group.LastPassIndex;
				ParallelPassGroups.push_back(ParallelGroup);
				for (uint32 PassIndex = Group.FirstPassIndex; PassIndex <= Group.LastPassIndex; PassIndex++) {
					if (!Passes[PassIndex]->bCulled) Passes[PassIndex]->ParallelPassSetIndex = SetIndex;
				}
			}
			Set.LastGroupIndex = ParallelPassGroups.size() - 1;
			ParallelPassSets.push_back(Set);
		}
		Candidates.clear();
		NumCandidatePasses = 0;
	};
	for (uint32 GroupIndex = 0; GroupIndex < Groups.size(); GroupIndex++) {
		if (Groups[GroupIndex].bInline) {
			FlushCandidates();
			continue;
		}
		Candidates.push_back(GroupIndex);
		NumCandidatePasses += Groups[GroupIndex].NumPasses;
		if (NumCandidatePasses >= PassMax) FlushCandidates();
	}
	FlushCandidates();
	Stats.NumParallelPassSets = ParallelPassSets.size();
}

void FRDGBuilder::CompileAsyncCompute() {
	if (AsyncComputePassCount == 0) return;

	// Establishes fork / join overlap regions for async compute (UE's FRDGBuilder::Compile). The async compute work of an
	// interval waits for its graphics fork pass and the graphics join pass waits for it; graphics passes in between run in
	// parallel with it. Null fork / join passes are the graph prologue / epilogue.
	FRDGPassHandle CurrentGraphicsForkPassHandle;
	bool bHasCurrentFork = false;
	for (FRDGPass *AsyncComputePass : Passes) {
		if (!AsyncComputePass->IsAsyncCompute() || AsyncComputePass->bCulled) continue;

		const FRDGPassHandle GraphicsForkPassHandle = FRDGPassHandle::Max(AsyncComputePass->CrossPipelineProducer, CurrentGraphicsForkPassHandle);
		AsyncComputePass->GraphicsForkPass = GraphicsForkPassHandle;

		if (!bHasCurrentFork || CurrentGraphicsForkPassHandle != GraphicsForkPassHandle) {
			bHasCurrentFork = true;
			CurrentGraphicsForkPassHandle = GraphicsForkPassHandle;
			AsyncComputePass->bAsyncComputeBegin = true;
			if (GraphicsForkPassHandle.IsNull()) bPrologueFork = true;
			else Passes[GraphicsForkPassHandle.GetIndex()]->bGraphicsFork = true;
			Stats.NumAsyncComputeForks++;
		}
	}

	FRDGPassHandle CurrentGraphicsJoinPassHandle;
	bool bHasCurrentJoin = false;
	for (int64 PassIndex = (int64)Passes.size() - 1; PassIndex >= 0; PassIndex--) {
		FRDGPass *AsyncComputePass = Passes[PassIndex];
		if (!AsyncComputePass->IsAsyncCompute() || AsyncComputePass->bCulled) continue;

		// Cross pipeline consumers are sorted. Find the earliest consumer that isn't culled.
		FRDGPassHandle CrossPipelineConsumer;
		for (FRDGPassHandle ConsumerHandle : AsyncComputePass->CrossPipelineConsumers) {
			if (!Passes[ConsumerHandle.GetIndex()]->bCulled) {
				CrossPipelineConsumer = ConsumerHandle;
				break;
			}
		}

		const FRDGPassHandle GraphicsJoinPassHandle = FRDGPassHandle::Min(CrossPipelineConsumer, CurrentGraphicsJoinPassHandle);
		AsyncComputePass->GraphicsJoinPass = GraphicsJoinPassHandle;

		if (!bHasCurrentJoin || CurrentGraphicsJoinPassHandle != GraphicsJoinPassHandle) {
			bHasCurrentJoin = true;
			CurrentGraphicsJoinPassHandle = GraphicsJoinPassHandle;
			AsyncComputePass->bAsyncComputeEnd = true;
			if (GraphicsJoinPassHandle.IsNull()) bEpilogueJoin = true;
			else Passes[GraphicsJoinPassHandle.GetIndex()]->bGraphicsJoin = true;
			Stats.NumAsyncComputeJoins++;
		}
	}
}

void FRDGBuilder::CompileLifetimes() {
	for (FRDGTexture *Texture : Textures) {
		Texture->FirstPass = FRDGPassHandle();
		Texture->LastPass = FRDGPassHandle();
		Texture->LastPasses[0] = Texture->LastPasses[1] = FRDGPassHandle();
		Texture->ReferenceCount = 0;
	}
	for (FRDGBuffer *Buffer : Buffers) {
		Buffer->FirstPass = FRDGPassHandle();
		Buffer->LastPass = FRDGPassHandle();
		Buffer->LastPasses[0] = Buffer->LastPasses[1] = FRDGPassHandle();
		Buffer->ReferenceCount = 0;
	}

	for (FRDGPass *Pass : Passes) {
		Pass->ResourcesToAcquire.clear();
		Pass->ResourcesToRelease.clear();
		if (Pass->bCulled) continue;
		const uint32 PipelineIndex = GetRHIPipelineIndex(Pass->Pipeline);
		for (const FRDGPass::FTextureState &State : Pass->TextureStates) {
			if (State.bExternalAccess) continue;
			FRDGTexture *Texture = State.Texture;
			if (Texture->FirstPass.IsNull()) Texture->FirstPass = Pass->Handle;
			Texture->LastPass = Pass->Handle;
			Texture->LastPasses[PipelineIndex] = Pass->Handle;
			Texture->ReferenceCount += State.ReferenceCount;
		}
		for (const FRDGPass::FBufferState &State : Pass->BufferStates) {
			if (State.bExternalAccess) continue;
			FRDGBuffer *Buffer = State.Buffer;
			if (Buffer->FirstPass.IsNull()) Buffer->FirstPass = Pass->Handle;
			Buffer->LastPass = Pass->Handle;
			Buffer->LastPasses[PipelineIndex] = Pass->Handle;
			Buffer->ReferenceCount += State.ReferenceCount;
		}
	}

	for (FRDGTexture *Texture : Textures) {
		if (Texture->FirstPass.IsNull()) continue;
		if (!Texture->bAllocated) Passes[Texture->FirstPass.GetIndex()]->ResourcesToAcquire.push_back(Texture);
		if (IsTransientReleasable(Texture, EnumHasAnyFlags(Texture->Flags, ERDGTextureFlags::MultiFrame)) && !Texture->bForceNonTransient) Passes[Texture->LastPass.GetIndex()]->ResourcesToRelease.push_back(Texture);
	}
	for (FRDGBuffer *Buffer : Buffers) {
		if (Buffer->FirstPass.IsNull()) continue;
		if (!Buffer->bAllocated) Passes[Buffer->FirstPass.GetIndex()]->ResourcesToAcquire.push_back(Buffer);
		if (IsTransientReleasable(Buffer, EnumHasAnyFlags(Buffer->Flags, ERDGBufferFlags::MultiFrame)) && !Buffer->bForceNonTransient) Passes[Buffer->LastPass.GetIndex()]->ResourcesToRelease.push_back(Buffer);
	}
}

void FRDGBuilder::MergeRenderPasses() {
	LocalVector<FRDGPass *> PassesToMerge;
	FRDGPass *PrevPass = nullptr;
	const FRenderTargetBindingSlots *PrevRenderTargets = nullptr;

	auto GetRenderTargets = [](const FRDGPass *Pass) -> const FRenderTargetBindingSlots * {
		const FShaderParametersMetadata *Metadata = Pass->ParameterMetadata;
		if (!Metadata || !Metadata->HasRenderTargets()) return nullptr;
		return reinterpret_cast<const FRenderTargetBindingSlots *>(reinterpret_cast<const uint8 *>(Pass->ParameterStruct) + Metadata->GetRenderTargetsOffset());
	};

	auto CommitMerge = [&]() {
		if (PassesToMerge.size() > 1) {
			// [B, X, X, E]: B begins the RD draw list and acquires every resource of the interval, E ends it and releases.
			FRDGPass *First = PassesToMerge[0];
			FRDGPass *Last = PassesToMerge[PassesToMerge.size() - 1];
			for (uint32 Index = 0; Index < PassesToMerge.size(); Index++) {
				FRDGPass *Pass = PassesToMerge[Index];
				if (Pass != First) {
					Pass->bSkipRenderPassBegin = true;
					for (FRDGViewableResource *Resource : Pass->ResourcesToAcquire) First->ResourcesToAcquire.push_back(Resource);
					Pass->ResourcesToAcquire.clear();
				}
				if (Pass != Last) {
					Pass->bSkipRenderPassEnd = true;
					for (FRDGViewableResource *Resource : Pass->ResourcesToRelease) Last->ResourcesToRelease.push_back(Resource);
					Pass->ResourcesToRelease.clear();
				}
			}
			Stats.NumMergedRenderPasses += PassesToMerge.size();
		}
		PassesToMerge.clear();
		PrevPass = nullptr;
		PrevRenderTargets = nullptr;
	};

	for (FRDGPass *NextPass : Passes) {
		if (NextPass->bCulled || NextPass->bEmptyParameters) continue;
		if (EnumHasAnyFlags(NextPass->Flags, ERDGPassFlags::Raster)) {
			if (EnumHasAnyFlags(NextPass->Flags, ERDGPassFlags::SkipRenderPass | ERDGPassFlags::NeverMerge) || !NextPass->bRenderPassOnlyWrites) {
				CommitMerge();
				continue;
			}
			const FRenderTargetBindingSlots *RenderTargets = GetRenderTargets(NextPass);
			if (!RenderTargets) {
				CommitMerge();
				continue;
			}
			if (PrevPass) {
				if (PrevRenderTargets->CanMergeBefore(*RenderTargets)) {
					if (PassesToMerge.is_empty()) PassesToMerge.push_back(PrevPass);
					PassesToMerge.push_back(NextPass);
				} else {
					CommitMerge();
				}
			}
			PrevPass = NextPass;
			PrevRenderTargets = RenderTargets;
		} else {
			// Any non-raster pass on the graphics pipe breaks the merge (compute lists can't run inside draw lists).
			CommitMerge();
		}
	}
	CommitMerge();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Execute.

String FRDGBuilder::BuildPassLabel(const FRDGPass *Pass) const {
	String Label = Pass->Name.ToString();
	for (const FRDGEventScope *Scope = Pass->Scope; Scope; Scope = Scope->ParentScope) Label = Scope->Name.ToString() + "/" + Label;
	return BuilderName.ToString() + "/" + Label;
}

void FRDGBuilder::SetPassRHIAccess(FRDGPass *Pass, bool bAllow) {
	for (const FRDGPass::FTextureState &State : Pass->TextureStates) State.Texture->bAllowRHIAccess = bAllow;
	for (const FRDGPass::FBufferState &State : Pass->BufferStates) State.Buffer->bAllowRHIAccess = bAllow;
}

void FRDGBuilder::BeginRenderPass(FRDGPass *Pass, FRHICommandList &CommandList) {
	const FShaderParametersMetadata *Metadata = Pass->ParameterMetadata;
	if (!Metadata || !Metadata->HasRenderTargets()) return;
	const FRenderTargetBindingSlots &RenderTargets = *reinterpret_cast<const FRenderTargetBindingSlots *>(reinterpret_cast<const uint8 *>(Pass->ParameterStruct) + Metadata->GetRenderTargetsOffset());

	FRHICommandList::FRenderPassInfo Info;
	Info.Name = Pass->GetName();
	for (uint32 Index = 0; Index < MaxSimultaneousRenderTargets; Index++) {
		const FRenderTargetBinding &Binding = RenderTargets.Output[Index];
		FRDGTexture *Texture = Binding.GetTexture();
		if (!Texture) break;
		if (!Texture->RHI) {
			RDGPrivate::ReportValidationError(vformat("Pass '%s': render target '%s' is not allocated.", Pass->GetName(), Texture->Name));
			return;
		}
		Info.ColorRIDs[Index] = Texture->RHI->GetRenderTargetView(Binding.GetMipIndex(), Binding.GetArraySlice());
		Info.ColorLoadActions[Index] = Binding.GetLoadAction();
		Info.ClearColors[Index] = Texture->Desc.ClearValue.GetClearColor();
		Info.Extent = Texture->Desc.GetMipExtent(Binding.GetMipIndex());
		Info.NumColorTargets++;
	}
	const FDepthStencilBinding &DepthStencil = RenderTargets.DepthStencil;
	if (FRDGTexture *Texture = DepthStencil.GetTexture()) {
		if (!Texture->RHI) {
			RDGPrivate::ReportValidationError(vformat("Pass '%s': depth target '%s' is not allocated.", Pass->GetName(), Texture->Name));
			return;
		}
		Info.DepthStencilRID = Texture->RHI->GetRenderTargetView(0, -1);
		Info.DepthLoadAction = DepthStencil.GetDepthLoadAction();
		Info.StencilLoadAction = DepthStencil.GetStencilLoadAction();
		Info.ClearDepth = Texture->Desc.ClearValue.Depth;
		Info.ClearStencil = Texture->Desc.ClearValue.Stencil;
		Info.bHasStencil = IsStencilFormat(Texture->Desc.Format);
		if (Info.NumColorTargets == 0) Info.Extent = Texture->Desc.Extent;
	}
	if (Info.NumColorTargets == 0 && !Info.DepthStencilRID.is_valid()) return;
	CommandList.BeginRenderPass(Info);
}

void FRDGBuilder::ExecutePassPrologue(FRDGPass *Pass) {
	// The join pass waits for the async compute interval, the first async compute pass of an interval waits for the fork.
	if (Pass->bGraphicsJoin) Device->async_compute_join_wait(Pass->JoinFence);
	if (Pass->IsAsyncCompute()) {
		if (Pass->bAsyncComputeBegin) Device->async_compute_fork_wait(Pass->GraphicsForkPass.IsNull() ? PrologueForkFence : Passes[Pass->GraphicsForkPass.GetIndex()]->ForkFence);
		// Everything the pass records goes to the async compute queue, including the uploads and clears of the resources it acquires.
		Device->async_compute_begin();
	}

	for (FRDGViewableResource *Resource : Pass->ResourcesToAcquire) AcquireResource(Resource);

	const bool bRaster = EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::Raster);
	if (CVarRDGEvents.GetValueOnRenderThread() && !Pass->bSkipRenderPassBegin) {
		const CharString Label = BuildPassLabel(Pass).utf8();
		Device->draw_command_begin_label(Span<char>(Label.get_data(), Label.length()), bRaster ? Color(0.2f, 0.6f, 1.0f) : Color(1.0f, 0.6f, 0.2f));
	}
	if (bRaster && !EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::SkipRenderPass) && !Pass->bSkipRenderPassBegin) BeginRenderPass(Pass, RHICmdList);
	SetPassRHIAccess(Pass, true);
}

void FRDGBuilder::ExecutePassEpilogue(FRDGPass *Pass) {
	SetPassRHIAccess(Pass, false);
	const bool bRaster = EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::Raster);
	if (bRaster) {
		if (!Pass->bSkipRenderPassEnd && RHICmdList.IsInsideRenderPass()) RHICmdList.EndRenderPass();
	} else {
		RHICmdList.FlushComputeList();
		if (RHICmdList.IsInsideRenderPass()) {
			RDGPrivate::ReportValidationError(vformat("Pass '%s' left a render pass open.", Pass->GetName()));
			RHICmdList.EndRenderPass();
		}
	}
	if (CVarRDGEvents.GetValueOnRenderThread() && !Pass->bSkipRenderPassEnd) Device->draw_command_end_label();
	for (FRDGViewableResource *Resource : Pass->ResourcesToRelease) ReleaseResource(Resource, Pass);

	if (Pass->IsAsyncCompute()) {
		Device->async_compute_end();
		if (Pass->bAsyncComputeEnd) {
			const RenderingDevice::AsyncComputeFence JoinFence = Device->async_compute_join_signal();
			if (Pass->GraphicsJoinPass.IsNull()) EpilogueJoinFence = JoinFence;
			else Passes[Pass->GraphicsJoinPass.GetIndex()]->JoinFence = JoinFence;
		}
	}

	// The fork covers the graphics work recorded so far. A draw list is only recorded when it ends, so the fork of a pass
	// inside a merged render pass is signaled at the end of the merged render pass.
	if (Pass->bGraphicsFork) PendingForkPasses.push_back(Pass);
	if (!PendingForkPasses.is_empty() && !RHICmdList.IsInsideRenderPass()) {
		const RenderingDevice::AsyncComputeFence ForkFence = Device->async_compute_fork_signal();
		for (FRDGPass *ForkPass : PendingForkPasses) ForkPass->ForkFence = ForkFence;
		PendingForkPasses.clear();
	}
}

void FRDGBuilder::ExecutePass(FRDGPass *Pass) {
	ExecutePassPrologue(Pass);
	bInPassExecution = true;
	Pass->Execute(RHICmdList);
	bInPassExecution = false;
	ExecutePassEpilogue(Pass);
	Pass->bExecuted = true;
}

void FRDGBuilder::ExecutePassInTask(FRDGPass *Pass, FRHICommandList &CommandList) {
	// Same as ExecutePass(), minus the resource lifetimes and async compute fork / join (done by the render thread).
	const bool bRaster = EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::Raster);
	if (bEmitPassEvents && !Pass->bSkipRenderPassBegin) {
		const CharString Label = BuildPassLabel(Pass).utf8();
		Device->draw_command_begin_label(Span<char>(Label.get_data(), Label.length()), bRaster ? Color(0.2f, 0.6f, 1.0f) : Color(1.0f, 0.6f, 0.2f));
	}
	if (bRaster && !EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::SkipRenderPass) && !Pass->bSkipRenderPassBegin) BeginRenderPass(Pass, CommandList);

	Pass->Execute(CommandList);

	if (bRaster) {
		if (!Pass->bSkipRenderPassEnd && CommandList.IsInsideRenderPass()) CommandList.EndRenderPass();
	} else {
		CommandList.FlushComputeList();
		if (CommandList.IsInsideRenderPass()) {
			RDGPrivate::ReportValidationError(vformat("Pass '%s' left a render pass open.", Pass->GetName()));
			CommandList.EndRenderPass();
		}
	}
	if (bEmitPassEvents && !Pass->bSkipRenderPassEnd) Device->draw_command_end_label();
}

void FRDGBuilder::ParallelPassSetTask(void *Data) {
	FParallelPassSet *Set = static_cast<FParallelPassSet *>(Data);
	FRDGBuilder *Builder = Set->Builder;
	FRHICommandList CommandList(Builder->Device);
	for (uint32 GroupIndex = Set->FirstGroupIndex; GroupIndex <= Set->LastGroupIndex; GroupIndex++) {
		const FParallelPassGroup &Group = Builder->ParallelPassGroups[GroupIndex];
		Builder->Device->recording_context_begin(Group.Context);
		for (uint32 PassIndex = Group.FirstPassIndex; PassIndex <= Group.LastPassIndex; PassIndex++) {
			FRDGPass *Pass = Builder->Passes[PassIndex];
			if (!Pass->bCulled) Builder->ExecutePassInTask(Pass, CommandList);
		}
		CommandList.FlushComputeList();
		if (CommandList.IsInsideRenderPass()) CommandList.EndRenderPass();
		Builder->Device->recording_context_end();
	}
	Set->NumBindingErrors = CommandList.GetNumBindingErrors();
}

void FRDGBuilder::ExecuteParallelPassSets(uint32 FirstSetIndex, uint32 LastSetIndex) {
	const uint32 FirstGroupIndex = ParallelPassSets[FirstSetIndex].FirstGroupIndex;
	const uint32 LastGroupIndex = ParallelPassSets[LastSetIndex].LastGroupIndex;

	// Render thread, in pass order: resource allocation. What it records (aliasing acquires, uploads, clobbers) goes to
	// the recording context of the group, ahead of the commands of the group's passes.
	struct FDeferredRelease {
		FRDGViewableResource *Resource = nullptr;
		void *RHI = nullptr;
	};
	LocalVector<FDeferredRelease> DeferredReleases;
	for (uint32 GroupIndex = FirstGroupIndex; GroupIndex <= LastGroupIndex; GroupIndex++) {
		FParallelPassGroup &Group = ParallelPassGroups[GroupIndex];
		Group.Context = DeviceContext->AcquireRecordingContext();
		Device->recording_context_begin(Group.Context);
		for (uint32 PassIndex = Group.FirstPassIndex; PassIndex <= Group.LastPassIndex; PassIndex++) {
			FRDGPass *Pass = Passes[PassIndex];
			if (Pass->bCulled) continue;
			for (FRDGViewableResource *Resource : Pass->ResourcesToAcquire) AcquireResource(Resource);
		}
		Device->recording_context_end();

		for (uint32 PassIndex = Group.FirstPassIndex; PassIndex <= Group.LastPassIndex; PassIndex++) {
			FRDGPass *Pass = Passes[PassIndex];
			if (Pass->bCulled) continue;
			SetPassRHIAccess(Pass, true);
			// The memory / pool entry goes back right away, so later groups reuse it exactly like serial execution, but
			// the pass lambdas still run: keep the RHI pointer (the allocator cache or the pool keeps it alive) until then.
			for (FRDGViewableResource *Resource : Pass->ResourcesToRelease) {
				const bool bTexture = Resource->Type == ERDGViewableResourceType::Texture;
				FDeferredRelease Release;
				Release.Resource = Resource;
				Release.RHI = bTexture ? (void *)static_cast<FRDGTexture *>(Resource)->RHI : (void *)static_cast<FRDGBuffer *>(Resource)->RHI;
				ReleaseResource(Resource, Pass);
				if (Resource->bAllocated) continue;
				if (bTexture) static_cast<FRDGTexture *>(Resource)->RHI = static_cast<FRHITexture *>(Release.RHI);
				else static_cast<FRDGBuffer *>(Resource)->RHI = static_cast<FRHIBuffer *>(Release.RHI);
				DeferredReleases.push_back(Release);
			}
		}
	}

	// Worker threads: one task per set records the pass lambdas.
	LocalVector<WorkerThreadPool::TaskID> Tasks;
	for (uint32 SetIndex = FirstSetIndex; SetIndex <= LastSetIndex; SetIndex++) {
		Tasks.push_back(WorkerThreadPool::get_singleton()->add_native_task(&FRDGBuilder::ParallelPassSetTask, &ParallelPassSets[SetIndex], true, "RDG Parallel Pass Set"));
	}
	for (const WorkerThreadPool::TaskID Task : Tasks) WorkerThreadPool::get_singleton()->wait_for_task_completion(Task);

	// Render thread: submit in pass order, so Godot's graph sees the same command sequence as serial execution.
	for (uint32 GroupIndex = FirstGroupIndex; GroupIndex <= LastGroupIndex; GroupIndex++) {
		FParallelPassGroup &Group = ParallelPassGroups[GroupIndex];
		Device->recording_context_submit(Group.Context);
		DeviceContext->ReleaseRecordingContext(Group.Context);
		Group.Context = nullptr;
		for (uint32 PassIndex = Group.FirstPassIndex; PassIndex <= Group.LastPassIndex; PassIndex++) {
			FRDGPass *Pass = Passes[PassIndex];
			if (Pass->bCulled) continue;
			SetPassRHIAccess(Pass, false);
			Pass->bExecuted = true;
			Stats.NumParallelPasses++;
		}
	}
	for (uint32 SetIndex = FirstSetIndex; SetIndex <= LastSetIndex; SetIndex++) ParallelBindingErrors += ParallelPassSets[SetIndex].NumBindingErrors;
	for (const FDeferredRelease &Release : DeferredReleases) {
		if (Release.Resource->bAllocated) continue;
		if (Release.Resource->Type == ERDGViewableResourceType::Texture) static_cast<FRDGTexture *>(Release.Resource)->RHI = nullptr;
		else static_cast<FRDGBuffer *>(Release.Resource)->RHI = nullptr;
	}
}

void FRDGBuilder::FinalizeExtraction() {
	for (const FExtractedTexture &Extracted : ExtractedTextures) {
		FRDGTexture *Texture = Extracted.Texture;
		if (!Texture->bAllocated) AllocateTexture(Texture);
		*Extracted.PooledTexture = Texture->PooledRenderTarget;
	}
	for (const FExtractedBuffer &Extracted : ExtractedBuffers) {
		FRDGBuffer *Buffer = Extracted.Buffer;
		if (!Buffer->bAllocated) AllocateBuffer(Buffer);
		*Extracted.PooledBuffer = Buffer->PooledBuffer;
	}
}

void FRDGBuilder::Execute() {
	if (bExecuted) {
		RDGPrivate::ReportValidationError("Render graph execution should only happen once to ensure consistency with immediate mode.");
		return;
	}
	if (!Device) {
		bExecuted = true;
		return;
	}
	const uint32 BindingErrorsAtStart = RHICmdList.GetNumBindingErrors();
	FlushAccessModeQueue();
	// UE: the setup tasks of the Compile wait point and the async setup queue are synced before the compilation.
	WaitForParallelSetupTasks(ERDGSetupTaskWaitPoint::Compile);
	if (AsyncSetupQueue.bEnabled) WaitForAsyncSetupQueue();
	Stats.NumAsyncSetupPasses = AsyncSetupQueue.NumPassesOnTask;

	if (!bImmediateMode) {
		// UE's parallel compile: the buffer NumElements / initial data callbacks are resolved by a task while the graph
		// compiles (nothing in Compile() reads them). Synced before the execution, like the Execute setup tasks.
		bCompiling = true;
		AddSetupTask([this]() {
			FinalizeDescs();
			PrepareBufferUploads();
		}, UE::Tasks::ETaskPriority::High, !NumElementsCallbackBuffers.is_empty() || bHasUploadCallbacks, ERDGSetupTaskWaitPoint::Execute);
		Compile();
		bCompiling = false;
		WaitForParallelSetupTasks(ERDGSetupTaskWaitPoint::Execute);
		// Async compute passes forking from the prologue wait for the work recorded before the graph.
		if (bPrologueFork) PrologueForkFence = Device->async_compute_fork_signal();
		bEmitPassEvents = CVarRDGEvents.GetValueOnRenderThread() != 0;
		for (uint32 PassIndex = 0; PassIndex < Passes.size();) {
			FRDGPass *Pass = Passes[PassIndex];
			if (Pass->bCulled) {
				PassIndex++;
				continue;
			}
			if (Pass->ParallelPassSetIndex < 0) {
				ExecutePass(Pass);
				PassIndex++;
				continue;
			}
			// Consecutive parallel sets (split by r.RDG.ParallelExecute.PassMax) run as concurrent tasks.
			const uint32 FirstSetIndex = Pass->ParallelPassSetIndex;
			uint32 LastSetIndex = FirstSetIndex;
			uint32 NextPassIndex = ParallelPassGroups[ParallelPassSets[LastSetIndex].LastGroupIndex].LastPassIndex + 1;
			while (true) {
				while (NextPassIndex < Passes.size() && Passes[NextPassIndex]->bCulled) NextPassIndex++;
				if (NextPassIndex >= Passes.size() || Passes[NextPassIndex]->ParallelPassSetIndex != (int32)LastSetIndex + 1) break;
				LastSetIndex++;
				NextPassIndex = ParallelPassGroups[ParallelPassSets[LastSetIndex].LastGroupIndex].LastPassIndex + 1;
			}
			bInPassExecution = true;
			ExecuteParallelPassSets(FirstSetIndex, LastSetIndex);
			bInPassExecution = false;
			PassIndex = NextPassIndex;
		}
		// Async compute work joining at the epilogue is joined before any graphics work recorded after the graph.
		if (bEpilogueJoin) Device->async_compute_join_wait(EpilogueJoinFence);
	} else {
		WaitForParallelSetupTasks(ERDGSetupTaskWaitPoint::Execute);
	}
	RHICmdList.FlushComputeList();
	if (RHICmdList.IsInsideRenderPass()) RHICmdList.EndRenderPass();

	FinalizeExtraction();

	Stats.NumPasses = Passes.size();
	Stats.NumCulledPasses = 0;
	Stats.NumAsyncComputePasses = 0;
	for (FRDGPass *Pass : Passes) {
		if (Pass->bCulled) Stats.NumCulledPasses++;
		else if (Pass->IsAsyncCompute()) Stats.NumAsyncComputePasses++;
	}
	Stats.NumTextures = Textures.size();
	Stats.NumBuffers = Buffers.size();
	for (FRDGTexture *Texture : Textures) {
		if (Texture->bExternal && !Texture->bForceNonTransient) Stats.NumExternalTextures++;
		if (Texture->bExtracted) Stats.NumExtractedTextures++;
	}
	Stats.NumPhysicalTransientTextures = PhysicalTextureUses.size();
	Stats.NumPhysicalTransientBuffers = PhysicalBufferUses.size();
	Stats.NumBindingErrors = RHICmdList.GetNumBindingErrors() - BindingErrorsAtStart + ParallelBindingErrors;
	Stats.NumValidationErrors = RDGPrivate::GetValidationErrorCount() - ValidationErrorsAtStart;

	const int32 DumpGraphMode = CVarRDGDumpGraph.GetValueOnRenderThread();
	if (DumpGraphMode > 0) {
		GLastDumpPath = RDGPrivate::DumpGraph(*this);
		if (DumpGraphMode == 2) CVarRDGDumpGraph->Set(0);
	}

	bExecuted = true;

	// Drop graph references so pooled resources become free for the next graph.
	for (FRDGTexture *Texture : Textures) {
		if (!Texture->bExternal && !Texture->bExtracted) ReleaseTexture(Texture);
	}
	for (FRDGBuffer *Buffer : Buffers) {
		if (!Buffer->bExternal && !Buffer->bExtracted) ReleaseBuffer(Buffer);
	}
	DeviceContext->TransientAllocator.Flush();

	for (std::function<void()> &Callback : PostExecuteCallbacks) Callback();
	PostExecuteCallbacks.clear();
	Blackboard.Clear();
}

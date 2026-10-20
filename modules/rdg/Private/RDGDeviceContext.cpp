#include "RDGDeviceContext.h"

#include "HAL/IConsoleManager.h"
#include "RHIStaticStates.h"

#include "core/config/engine.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

static TAutoConsoleVariable<int32> CVarRDGPoolFramesUntilRelease(
		"r.RDG.ResourcePool.FramesUntilRelease", 30,
		"Number of frames a free pooled texture / buffer is kept before it is released.",
		ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRDGCacheFramesUntilRelease(
		"r.RDG.Cache.FramesUntilRelease", 120,
		"Number of frames an unused uniform set / framebuffer stays cached.",
		ECVF_RenderThreadSafe);

namespace {
HashMap<RenderingDevice *, FRDGDeviceContext *> &GetContexts() {
	static HashMap<RenderingDevice *, FRDGDeviceContext *> Contexts;
	return Contexts;
}

uint64 Hash64(const void *Data, uint32 Size, uint64 Seed = 0) {
	const uint32 Low = hash_murmur3_buffer(Data, Size, (uint32)(Seed & 0xFFFFFFFF) ^ 0x9E3779B9);
	const uint32 High = hash_murmur3_buffer(Data, Size, (uint32)(Seed >> 32) ^ 0x7F4A7C15);
	return ((uint64)High << 32) | Low;
}
} //namespace

RenderingDevice *FRDGDeviceContext::GetMainDevice() {
	RenderingServer *RS = RenderingServer::get_singleton();
	return RS ? RS->get_rendering_device() : nullptr;
}

FRDGDeviceContext *FRDGDeviceContext::Get(RenderingDevice *Device) {
	ERR_FAIL_NULL_V(Device, nullptr);
	if (FRDGDeviceContext **Existing = GetContexts().getptr(Device)) return *Existing;
	FRDGDeviceContext *Context = memnew(FRDGDeviceContext(Device));
	GetContexts().insert(Device, Context);
	return Context;
}

FRDGDeviceContext *FRDGDeviceContext::Find(RenderingDevice *Device) {
	FRDGDeviceContext **Existing = GetContexts().getptr(Device);
	return Existing ? *Existing : nullptr;
}

void FRDGDeviceContext::Release(RenderingDevice *Device) {
	FRHIVertexDeclaration::ReleaseDevice(Device);
	FRDGDeviceContext **Existing = GetContexts().getptr(Device);
	if (!Existing) return;
	FRDGDeviceContext *Context = *Existing;
	GetContexts().erase(Device);
	memdelete(Context);
}

void FRDGDeviceContext::ReleaseAll() {
	LocalVector<RenderingDevice *> Devices;
	for (KeyValue<RenderingDevice *, FRDGDeviceContext *> &It : GetContexts()) Devices.push_back(It.key);
	for (RenderingDevice *Device : Devices) Release(Device);
}

FRDGDeviceContext::FRDGDeviceContext(RenderingDevice *InDevice) :
		Device(InDevice), RenderTargetPool(InDevice), BufferPool(InDevice), TransientAllocator(InDevice), GlobalShaderMap(InDevice) {}

FRDGDeviceContext::~FRDGDeviceContext() {
	for (RenderingDevice::RecordingContext *Context : FreeRecordingContexts) Device->recording_context_free(Context);
	FreeRecordingContexts.clear();
	ExternalTextures.clear();
	ExternalBuffers.clear();
	FreeAllCachedRIDs();
	GlobalShaderMap.Empty();
	TransientAllocator.ReleaseAll();
	RenderTargetPool.ReleaseAll();
	BufferPool.ReleaseAll();
}

void FRDGDeviceContext::FreeAllCachedRIDs() {
	MutexLock Lock(CacheMutex);
	// Freeing a uniform set / framebuffer triggers the invalidation callback, which erases the entry.
	LocalVector<RID> ToFree;
	{
		FReadScopeLock HotLock(HotCacheLock);
		for (KeyValue<uint64, FUniformSetEntry *> &It : UniformSets) ToFree.push_back(It.value->UniformSet);
	}
	for (KeyValue<uint64, FFramebufferEntry *> &It : Framebuffers) ToFree.push_back(It.value->Framebuffer);
	for (const RID &Rid : ToFree) {
		if (Device->uniform_set_is_valid(Rid) || Device->framebuffer_is_valid(Rid)) Device->free_rid(Rid);
	}
	{
		FWriteScopeLock HotLock(HotCacheLock);
		for (KeyValue<uint64, FUniformSetEntry *> &It : UniformSets) memdelete(It.value);
		UniformSets.clear();
	}
	for (KeyValue<uint64, FFramebufferEntry *> &It : Framebuffers) memdelete(It.value);
	Framebuffers.clear();

	for (KeyValue<uint64, FPipelineEntry> &It : GraphicsPipelines) {
		if (Device->render_pipeline_is_valid(It.value.Pipeline)) Device->free_rid(It.value.Pipeline);
	}
	GraphicsPipelines.clear();
	for (KeyValue<uint64, FBoundShaderState *> &It : BoundShaderStates) {
		if (It.value->ShaderRID.is_valid()) Device->free_rid(It.value->ShaderRID);
		memdelete(It.value);
	}
	BoundShaderStates.clear();

	{
		FWriteScopeLock HotLock(HotCacheLock);
		for (KeyValue<uint32, RID> &It : Samplers) Device->free_rid(It.value);
		Samplers.clear();
		if (DefaultSampler.is_valid()) Device->free_rid(DefaultSampler);
		DefaultSampler = RID();
	}

	for (const FFrameUniformBuffer &Buffer : FrameUniformBuffers) Device->free_rid(Buffer.Buffer);
	FrameUniformBuffers.clear();
}

void FRDGDeviceContext::TickFrame() {
	MutexLock Lock(CacheMutex);
	FrameNumber++;
	const uint32 PoolFrames = (uint32)FMath::Max(0, CVarRDGPoolFramesUntilRelease.GetValueOnRenderThread());
	RenderTargetPool.TickPoolElements(FrameNumber, PoolFrames);
	BufferPool.TickPoolElements(FrameNumber, PoolFrames);
	RenderTargetPool.ResetFrameStats();
	BufferPool.ResetFrameStats();
	TransientAllocator.GarbageCollect(FrameNumber);
	TransientAllocator.ResetCycleStats();

	const uint64 CacheFrames = (uint64)FMath::Max(1, CVarRDGCacheFramesUntilRelease.GetValueOnRenderThread());
	LocalVector<RID> ToFree;
	{
		FReadScopeLock HotLock(HotCacheLock);
		for (KeyValue<uint64, FUniformSetEntry *> &It : UniformSets) {
			if (FrameNumber - It.value->LastUsedFrame.load(std::memory_order_relaxed) > CacheFrames) ToFree.push_back(It.value->UniformSet);
		}
	}
	for (KeyValue<uint64, FFramebufferEntry *> &It : Framebuffers) {
		if (FrameNumber - It.value->LastUsedFrame > CacheFrames) ToFree.push_back(It.value->Framebuffer);
	}
	for (const RID &Rid : ToFree) Device->free_rid(Rid);

	LocalVector<RID> StaleExternal;
	for (KeyValue<RID, FExternalTexture> &It : ExternalTextures) {
		if (!Device->texture_is_valid(It.key) || (FrameNumber - It.value.LastUsedFrame > CacheFrames && It.value.RenderTarget.GetRefCount() == 1)) StaleExternal.push_back(It.key);
	}
	for (const RID &Rid : StaleExternal) ExternalTextures.erase(Rid);
	StaleExternal.clear();
	for (KeyValue<RID, FExternalBuffer> &It : ExternalBuffers) {
		if (FrameNumber - It.value.LastUsedFrame > CacheFrames && It.value.Buffer.GetRefCount() == 1) StaleExternal.push_back(It.key);
	}
	for (const RID &Rid : StaleExternal) ExternalBuffers.erase(Rid);
}

void FRDGDeviceContext::TickIfNewEngineFrame() {
	const uint64 EngineFrame = Engine::get_singleton() ? Engine::get_singleton()->get_frames_drawn() : 0;
	if (EngineFrame == LastEngineFrame) return;
	LastEngineFrame = EngineFrame;
	TickFrame();
}

RID FRDGDeviceContext::GetSampler(const FRHISamplerState *SamplerState) {
	if (!SamplerState) return GetDefaultSampler();
	const uint32 Hash = SamplerState->GetHash();
	{
		FReadScopeLock Lock(HotCacheLock);
		if (const RID *Existing = Samplers.getptr(Hash)) return *Existing;
	}
	FWriteScopeLock Lock(HotCacheLock);
	if (const RID *Existing = Samplers.getptr(Hash)) return *Existing;
	const RID Sampler = Device->sampler_create(SamplerState->ToRD());
	Samplers.insert(Hash, Sampler);
	return Sampler;
}

RID FRDGDeviceContext::GetDefaultSampler() {
	{
		FReadScopeLock Lock(HotCacheLock);
		if (DefaultSampler.is_valid()) return DefaultSampler;
	}
	FWriteScopeLock Lock(HotCacheLock);
	if (!DefaultSampler.is_valid()) {
		RenderingDevice::SamplerState State;
		State.mag_filter = RenderingDevice::SAMPLER_FILTER_LINEAR;
		State.min_filter = RenderingDevice::SAMPLER_FILTER_LINEAR;
		State.mip_filter = RenderingDevice::SAMPLER_FILTER_LINEAR;
		DefaultSampler = Device->sampler_create(State);
	}
	return DefaultSampler;
}

void FRDGDeviceContext::OnFramebufferInvalidated(void *UserData) {
	FRDGDeviceContext *LockOwner = reinterpret_cast<FFramebufferEntry *>(UserData)->Owner;
	MutexLock Lock(LockOwner->CacheMutex);
	FFramebufferEntry *Entry = reinterpret_cast<FFramebufferEntry *>(UserData);
	FRDGDeviceContext *Owner = Entry->Owner;
	if (FFramebufferEntry **Found = Owner->Framebuffers.getptr(Entry->Key)) {
		if (*Found == Entry) Owner->Framebuffers.erase(Entry->Key);
	}
	memdelete(Entry);
}

RID FRDGDeviceContext::GetFramebuffer(const RID *Attachments, uint32 NumAttachments) {
	MutexLock Lock(CacheMutex);
	uint64 Ids[MaxSimultaneousRenderTargets + 1] = {};
	for (uint32 Index = 0; Index < NumAttachments; Index++) Ids[Index] = Attachments[Index].get_id();
	const uint64 Key = Hash64(Ids, sizeof(uint64) * NumAttachments, NumAttachments);
	// Freed framebuffers are removed by OnFramebufferInvalidated(), so cached entries are always valid.
	if (FFramebufferEntry **Existing = Framebuffers.getptr(Key)) {
		(*Existing)->LastUsedFrame = FrameNumber;
		return (*Existing)->Framebuffer;
	}

	Vector<RID> Textures;
	for (uint32 Index = 0; Index < NumAttachments; Index++) Textures.push_back(Attachments[Index]);
	const RID Framebuffer = Device->framebuffer_create(Textures);
	ERR_FAIL_COND_V_MSG(!Framebuffer.is_valid(), RID(), "[RDG] Failed to create framebuffer for render pass.");

	FFramebufferEntry *Entry = memnew(FFramebufferEntry);
	Entry->Owner = this;
	Entry->Key = Key;
	Entry->Framebuffer = Framebuffer;
	Entry->LastUsedFrame = FrameNumber;
	Framebuffers.insert(Key, Entry);
	Device->framebuffer_set_invalidation_callback(Framebuffer, &FRDGDeviceContext::OnFramebufferInvalidated, Entry);
	return Framebuffer;
}

void FRDGDeviceContext::OnUniformSetInvalidated(void *UserData) {
	FRDGDeviceContext *LockOwner = reinterpret_cast<FUniformSetEntry *>(UserData)->Owner;
	FWriteScopeLock Lock(LockOwner->HotCacheLock);
	FUniformSetEntry *Entry = reinterpret_cast<FUniformSetEntry *>(UserData);
	FRDGDeviceContext *Owner = Entry->Owner;
	if (FUniformSetEntry **Found = Owner->UniformSets.getptr(Entry->Key)) {
		if (*Found == Entry) Owner->UniformSets.erase(Entry->Key);
	}
	memdelete(Entry);
}

RID FRDGDeviceContext::GetUniformSet(RID Shader, uint32 SetIndex, const LocalVector<RenderingDevice::Uniform> &Uniforms) {
	return GetUniformSet(GetUniformSetKey(Shader, SetIndex, Uniforms), Shader, SetIndex, Uniforms);
}

uint64 FRDGDeviceContext::GetUniformSetKey(RID Shader, uint32 SetIndex, const LocalVector<RenderingDevice::Uniform> &Uniforms) {
	thread_local LocalVector<uint64> KeyData;
	KeyData.clear();
	KeyData.push_back(Shader.get_id());
	KeyData.push_back(SetIndex);
	for (const RenderingDevice::Uniform &Uniform : Uniforms) {
		KeyData.push_back(((uint64)Uniform.uniform_type << 32) | Uniform.binding);
		for (uint32 Index = 0; Index < Uniform.get_id_count(); Index++) KeyData.push_back(Uniform.get_id(Index).get_id());
	}
	return Hash64(KeyData.ptr(), KeyData.size() * sizeof(uint64), Uniforms.size());
}

RID FRDGDeviceContext::GetUniformSet(uint64 Key, RID Shader, uint32 SetIndex, const LocalVector<RenderingDevice::Uniform> &Uniforms) {
	// Freed uniform sets are removed by OnUniformSetInvalidated(), so cached entries are always valid.
	{
		FReadScopeLock Lock(HotCacheLock);
		if (FUniformSetEntry *const *Existing = UniformSets.getptr(Key)) {
			(*Existing)->LastUsedFrame.store(FrameNumber, std::memory_order_relaxed);
			return (*Existing)->UniformSet;
		}
	}

	FWriteScopeLock Lock(HotCacheLock);
	if (FUniformSetEntry *const *Existing = UniformSets.getptr(Key)) return (*Existing)->UniformSet;

	const RID UniformSet = Device->uniform_set_create(VectorView<RenderingDevice::Uniform>(Uniforms.ptr(), Uniforms.size()), Shader, SetIndex);
	if (!UniformSet.is_valid()) return RID();

	FUniformSetEntry *Entry = memnew(FUniformSetEntry);
	Entry->Owner = this;
	Entry->Key = Key;
	Entry->UniformSet = UniformSet;
	Entry->LastUsedFrame = FrameNumber;
	UniformSets.insert(Key, Entry);
	Device->uniform_set_set_invalidation_callback(UniformSet, &FRDGDeviceContext::OnUniformSetInvalidated, Entry);
	return UniformSet;
}

const FRDGDeviceContext::FBoundShaderState *FRDGDeviceContext::GetBoundShaderState(FRHIVertexShader *VertexShader, FRHIPixelShader *PixelShader) {
	MutexLock Lock(CacheMutex);
	ERR_FAIL_COND_V_MSG(!VertexShader || !PixelShader || !VertexShader->Shader || !PixelShader->Shader, nullptr, "[RDG] Graphics pipeline requires a valid vertex and pixel shader.");
	const uint64 Pointers[2] = { (uint64)(uintptr_t)VertexShader, (uint64)(uintptr_t)PixelShader };
	const uint64 Key = Hash64(Pointers, sizeof(Pointers));
	if (FBoundShaderState **Existing = BoundShaderStates.getptr(Key)) return *Existing;

	const FShaderCompiledData *VSData = VertexShader->Shader->GetCompiledData();
	const FShaderCompiledData *PSData = PixelShader->Shader->GetCompiledData();

	Vector<RenderingDevice::ShaderStageSPIRVData> Stages;
	RenderingDevice::ShaderStageSPIRVData VSStage;
	VSStage.shader_stage = RenderingDevice::SHADER_STAGE_VERTEX;
	VSStage.spirv = VSData->SPIRV;
	Stages.push_back(VSStage);
	RenderingDevice::ShaderStageSPIRVData PSStage;
	PSStage.shader_stage = RenderingDevice::SHADER_STAGE_FRAGMENT;
	PSStage.spirv = PSData->SPIRV;
	Stages.push_back(PSStage);

	const String Name = VSData->DebugName + "+" + PSData->DebugName;
	const RID ShaderRID = Device->shader_create_from_spirv(Stages, Name);
	ERR_FAIL_COND_V_MSG(!ShaderRID.is_valid(), nullptr, vformat("[RDG] Failed to link shaders '%s'.", Name));

	FBoundShaderState *State = memnew(FBoundShaderState);
	State->ShaderRID = ShaderRID;
	State->Name = Name;
	// Merged reflection: descriptors of both stages (same set/binding must describe the same resource).
	State->Reflection = VSData->Reflection;
	for (const FShaderResourceReflection &Resource : PSData->Reflection.Resources) {
		if (!State->Reflection.FindResource(Resource.Set, Resource.Binding)) State->Reflection.Resources.push_back(Resource);
	}
	State->Reflection.PushConstantSize = FMath::Max(VSData->Reflection.PushConstantSize, PSData->Reflection.PushConstantSize);
	for (const FShaderResourceReflection::FMember &Member : PSData->Reflection.PushConstantMembers) {
		if (!State->Reflection.FindPushConstant(Member.Name)) State->Reflection.PushConstantMembers.push_back(Member);
	}
	BoundShaderStates.insert(Key, State);
	return State;
}

static RenderingDevice::RenderPrimitive ToRDPrimitive(EPrimitiveType Type) {
	switch (Type) {
		case PT_TriangleStrip:
			return RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS;
		case PT_LineList:
			return RenderingDevice::RENDER_PRIMITIVE_LINES;
		case PT_PointList:
			return RenderingDevice::RENDER_PRIMITIVE_POINTS;
		default:
			return RenderingDevice::RENDER_PRIMITIVE_TRIANGLES;
	}
}

RID FRDGDeviceContext::GetGraphicsPipeline(const FBoundShaderState &BoundShaderState, RenderingDevice::FramebufferFormatID FramebufferFormat, uint32 NumColorTargets, const FGraphicsPipelineStateInitializer &Initializer, uint32 StencilRef) {
	MutexLock Lock(CacheMutex);
	FRHIBlendState *BlendState = Initializer.BlendState ? Initializer.BlendState : TStaticBlendState<>::GetRHI();
	FRHIRasterizerState *RasterizerState = Initializer.RasterizerState ? Initializer.RasterizerState : TStaticRasterizerState<>::GetRHI();
	FRHIDepthStencilState *DepthStencilState = Initializer.DepthStencilState ? Initializer.DepthStencilState : TStaticDepthStencilState<false, CF_Always>::GetRHI();

	const RenderingDevice::VertexFormatID VertexFormat = Initializer.BoundShaderState.VertexDeclarationRHI ? Initializer.BoundShaderState.VertexDeclarationRHI->GetVertexFormat(Device) : RenderingDevice::INVALID_FORMAT_ID;
	const uint64 KeyData[6] = { BoundShaderState.ShaderRID.get_id(), (uint64)FramebufferFormat, ((uint64)NumColorTargets << 32) | Initializer.PrimitiveType, ((uint64)BlendState->GetHash() << 32) | RasterizerState->GetHash(), ((uint64)DepthStencilState->GetHash() << 32) | StencilRef, (uint64)VertexFormat };
	const uint64 Key = Hash64(KeyData, sizeof(KeyData));
	if (FPipelineEntry *Existing = GraphicsPipelines.getptr(Key)) {
		if (Device->render_pipeline_is_valid(Existing->Pipeline)) return Existing->Pipeline;
		GraphicsPipelines.erase(Key);
	}

	RenderingDevice::PipelineMultisampleState Multisample;
	Multisample.sample_count = Device->framebuffer_format_get_texture_samples(FramebufferFormat, 0);
	const RID Pipeline = Device->render_pipeline_create(BoundShaderState.ShaderRID, FramebufferFormat, VertexFormat, ToRDPrimitive(Initializer.PrimitiveType),
			RasterizerState->ToRD(), Multisample, DepthStencilState->ToRD(StencilRef), BlendState->ToRD(NumColorTargets),
			RenderingDevice::DYNAMIC_STATE_BLEND_CONSTANTS);
	ERR_FAIL_COND_V_MSG(!Pipeline.is_valid(), RID(), vformat("[RDG] Failed to create graphics pipeline for '%s'.", BoundShaderState.Name));
	FPipelineEntry Entry;
	Entry.Pipeline = Pipeline;
	Entry.Shader = BoundShaderState.ShaderRID;
	GraphicsPipelines.insert(Key, Entry);
	return Pipeline;
}

RID FRDGDeviceContext::AllocateFrameUniformBuffer(uint32 Size) {
	MutexLock Lock(CacheMutex);
	const uint32 AlignedSize = (FMath::Max(Size, 16u) + 15u) & ~15u;
	for (FFrameUniformBuffer &Buffer : FrameUniformBuffers) {
		if (Buffer.Size == AlignedSize && Buffer.Frame < FrameNumber) {
			Buffer.Frame = FrameNumber;
			return Buffer.Buffer;
		}
	}
	FFrameUniformBuffer Buffer;
	Buffer.Buffer = Device->uniform_buffer_create(AlignedSize);
	Buffer.Size = AlignedSize;
	Buffer.Frame = FrameNumber;
	ERR_FAIL_COND_V(!Buffer.Buffer.is_valid(), RID());
	FrameUniformBuffers.push_back(Buffer);
	return Buffer.Buffer;
}

TRefCountPtr<IPooledRenderTarget> FRDGDeviceContext::GetOrCreateExternalRenderTarget(RID Texture, const char *Name) {
	MutexLock Lock(CacheMutex);
	ERR_FAIL_COND_V_MSG(!Device->texture_is_valid(Texture), TRefCountPtr<IPooledRenderTarget>(), vformat("[RDG] External texture '%s' is not a valid RD texture.", Name));
	const RenderingDevice::TextureFormat Format = Device->texture_get_format(Texture);
	const FRHITextureDesc Desc = FromRDTextureFormat(Format);
	if (FExternalTexture *Existing = ExternalTextures.getptr(Texture)) {
		if (Existing->RenderTarget->GetDesc() == Desc) {
			Existing->LastUsedFrame = FrameNumber;
			return Existing->RenderTarget;
		}
		ExternalTextures.erase(Texture);
	}
	FRHITexture *RHITexture = new FRHITexture(Device, Texture, Desc, String::utf8(Name), false);
	FExternalTexture Entry;
	Entry.RenderTarget = TRefCountPtr<IPooledRenderTarget>(new FPooledRenderTarget(RHITexture, false));
	Entry.LastUsedFrame = FrameNumber;
	ExternalTextures.insert(Texture, Entry);
	return Entry.RenderTarget;
}

TRefCountPtr<FRDGPooledBuffer> FRDGDeviceContext::GetOrCreateExternalBuffer(RID Buffer, const FRDGBufferDesc &Desc, const char *Name) {
	MutexLock Lock(CacheMutex);
	if (FExternalBuffer *Existing = ExternalBuffers.getptr(Buffer)) {
		if (Existing->Buffer->Desc == Desc) {
			Existing->LastUsedFrame = FrameNumber;
			return Existing->Buffer;
		}
		ExternalBuffers.erase(Buffer);
	}
	FRHIBuffer *RHIBuffer = new FRHIBuffer(Device, Buffer, Desc.GetSize(), Desc.BytesPerElement, Desc.Usage, String::utf8(Name), false);
	FExternalBuffer Entry;
	Entry.Buffer = TRefCountPtr<FRDGPooledBuffer>(new FRDGPooledBuffer(RHIBuffer, Desc, false));
	Entry.LastUsedFrame = FrameNumber;
	ExternalBuffers.insert(Buffer, Entry);
	return Entry.Buffer;
}

void FRDGDeviceContext::RecompileShaders() {
	MutexLock Lock(CacheMutex);
	for (KeyValue<uint64, FPipelineEntry> &It : GraphicsPipelines) {
		if (Device->render_pipeline_is_valid(It.value.Pipeline)) Device->free_rid(It.value.Pipeline);
	}
	GraphicsPipelines.clear();
	for (KeyValue<uint64, FBoundShaderState *> &It : BoundShaderStates) {
		if (It.value->ShaderRID.is_valid()) Device->free_rid(It.value->ShaderRID);
		memdelete(It.value);
	}
	BoundShaderStates.clear();
	GlobalShaderMap.Empty();
}

FRDGDeviceContext::FCacheStats FRDGDeviceContext::GetCacheStats() const {
	MutexLock Lock(CacheMutex);
	FCacheStats Result;
	{
		FReadScopeLock HotLock(HotCacheLock);
		Result.NumUniformSets = UniformSets.size();
		Result.NumSamplers = Samplers.size();
	}
	Result.NumFramebuffers = Framebuffers.size();
	Result.NumPipelines = GraphicsPipelines.size();
	Result.NumBoundShaderStates = BoundShaderStates.size();
	Result.NumFrameUniformBuffers = FrameUniformBuffers.size();
	Result.NumExternalTextures = ExternalTextures.size();
	return Result;
}

RenderingDevice::RecordingContext *FRDGDeviceContext::AcquireRecordingContext() {
	MutexLock Lock(CacheMutex);
	if (!FreeRecordingContexts.is_empty()) {
		RenderingDevice::RecordingContext *Context = FreeRecordingContexts[FreeRecordingContexts.size() - 1];
		FreeRecordingContexts.resize(FreeRecordingContexts.size() - 1);
		return Context;
	}
	return Device->recording_context_create();
}

void FRDGDeviceContext::ReleaseRecordingContext(RenderingDevice::RecordingContext *Context) {
	ERR_FAIL_NULL(Context);
	MutexLock Lock(CacheMutex);
	FreeRecordingContexts.push_back(Context);
}

#pragma once

// Godot-specific "RHI device" state: everything RDG caches per RenderingDevice (pools, compiled shaders, samplers,
// framebuffers, uniform sets, pipelines). Local RenderingDevices must call FRDGDeviceContext::Release(Device) before
// they are freed; the main device is released when the module shuts down.

#include "GlobalShader.h"
#include "RHITransientResourceAllocator.h"
#include "RenderGraphResourcePool.h"

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/rendering_device.h"

#include <atomic>

class FRDGDeviceContext {
public:
	static FRDGDeviceContext *Get(RenderingDevice *Device);
	static FRDGDeviceContext *Find(RenderingDevice *Device);
	static void Release(RenderingDevice *Device);
	static void ReleaseAll();
	static RenderingDevice *GetMainDevice();

	explicit FRDGDeviceContext(RenderingDevice *InDevice);
	~FRDGDeviceContext();

	RenderingDevice *const Device;

	FRenderTargetPool RenderTargetPool;
	FRDGBufferPool BufferPool;
	FRHITransientResourceAllocator TransientAllocator;
	FGlobalShaderMap GlobalShaderMap;

	uint64 GetFrameNumber() const { return FrameNumber; }
	/** Advances the frame: ticks pools, recycles per-frame uniform buffers and evicts stale cache entries. */
	void TickFrame();
	/** Calls TickFrame() when the engine drew a new frame since the last tick (main device only). */
	void TickIfNewEngineFrame();

	RID GetSampler(const FRHISamplerState *SamplerState);
	RID GetDefaultSampler();
	RID GetFramebuffer(const RID *Attachments, uint32 NumAttachments);
	RID GetUniformSet(RID Shader, uint32 SetIndex, const LocalVector<RenderingDevice::Uniform> &Uniforms);
	/** Same, with the key computed by GetUniformSetKey() (command lists use the key for their own last-set cache). */
	RID GetUniformSet(uint64 Key, RID Shader, uint32 SetIndex, const LocalVector<RenderingDevice::Uniform> &Uniforms);
	static uint64 GetUniformSetKey(RID Shader, uint32 SetIndex, const LocalVector<RenderingDevice::Uniform> &Uniforms);

	struct FBoundShaderState {
		RID ShaderRID;
		FShaderReflectionInfo Reflection;
		String Name;
	};
	const FBoundShaderState *GetBoundShaderState(FRHIVertexShader *VertexShader, FRHIPixelShader *PixelShader);
	RID GetGraphicsPipeline(const FBoundShaderState &BoundShaderState, RenderingDevice::FramebufferFormatID FramebufferFormat, uint32 NumColorTargets, const FGraphicsPipelineStateInitializer &Initializer, uint32 StencilRef);

	/** Uniform buffer valid for the current frame only (recycled once the frame number advanced). */
	RID AllocateFrameUniformBuffer(uint32 Size);

	/** Wraps an external RD texture; wrappers are cached by RID so their view caches survive across frames. */
	TRefCountPtr<IPooledRenderTarget> GetOrCreateExternalRenderTarget(RID Texture, const char *Name);
	TRefCountPtr<FRDGPooledBuffer> GetOrCreateExternalBuffer(RID Buffer, const FRDGBufferDesc &Desc, const char *Name);

	/** Drops all compiled shaders and dependent pipelines (shader hot reload). */
	void RecompileShaders();

	/** Recording contexts for passes executed by parallel tasks (r.RDG.ParallelExecute), reused between graphs. */
	RenderingDevice::RecordingContext *AcquireRecordingContext();
	void ReleaseRecordingContext(RenderingDevice::RecordingContext *Context);

	struct FCacheStats {
		uint32 NumUniformSets = 0;
		uint32 NumFramebuffers = 0;
		uint32 NumPipelines = 0;
		uint32 NumSamplers = 0;
		uint32 NumBoundShaderStates = 0;
		uint32 NumFrameUniformBuffers = 0;
		uint32 NumExternalTextures = 0;
	};
	FCacheStats GetCacheStats() const;

private:
	struct FUniformSetEntry {
		FRDGDeviceContext *Owner = nullptr;
		uint64 Key = 0;
		RID UniformSet;
		// Updated by cache hits under the shared lock.
		std::atomic<uint64> LastUsedFrame{ 0 };
	};
	struct FFramebufferEntry {
		FRDGDeviceContext *Owner = nullptr;
		uint64 Key = 0;
		RID Framebuffer;
		uint64 LastUsedFrame = 0;
	};
	struct FFrameUniformBuffer {
		RID Buffer;
		uint32 Size = 0;
		uint64 Frame = 0;
	};
	struct FExternalTexture {
		TRefCountPtr<IPooledRenderTarget> RenderTarget;
		uint64 LastUsedFrame = 0;
	};
	struct FExternalBuffer {
		TRefCountPtr<FRDGPooledBuffer> Buffer;
		uint64 LastUsedFrame = 0;
	};
	struct FPipelineEntry {
		RID Pipeline;
		RID Shader;
	};

	static void OnUniformSetInvalidated(void *UserData);
	static void OnFramebufferInvalidated(void *UserData);
	void FreeAllCachedRIDs();

	uint64 FrameNumber = 1;
	uint64 LastEngineFrame = UINT64_MAX;

	// The caches below are used by the pass lambdas of parallel tasks. Recursive (Godot's Mutex): RD invalidation
	// callbacks re-enter while a cache entry is freed.
	mutable Mutex CacheMutex;
	// Uniform sets and samplers are looked up by every dispatch / draw: cache hits only take the shared lock. Never held
	// while freeing RIDs (the invalidation callback takes it exclusively).
	mutable FRWLock HotCacheLock;
	LocalVector<RenderingDevice::RecordingContext *> FreeRecordingContexts;

	HashMap<uint64, FUniformSetEntry *> UniformSets;
	HashMap<uint64, FFramebufferEntry *> Framebuffers;
	HashMap<uint32, RID> Samplers;
	RID DefaultSampler;
	HashMap<uint64, FBoundShaderState *> BoundShaderStates;
	HashMap<uint64, FPipelineEntry> GraphicsPipelines;
	LocalVector<FFrameUniformBuffer> FrameUniformBuffers;
	HashMap<RID, FExternalTexture> ExternalTextures;
	HashMap<RID, FExternalBuffer> ExternalBuffers;
};

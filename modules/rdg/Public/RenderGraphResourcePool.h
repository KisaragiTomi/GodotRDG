#pragma once

// Cross-frame resource pools (UE's GRenderTargetPool / GRenderGraphResourcePool equivalents), one per device.
// Elements whose only reference is the pool are free; RDG returns transient resources to the pool right after their
// last use, so later passes of the same graph can reuse the same RD texture / buffer (resource-level aliasing).

#include "RenderGraphResources.h"

struct FRDGPoolStats {
	uint32 NumElements = 0;
	uint32 NumFreeElements = 0;
	uint32 NumCreatedThisFrame = 0;
	uint32 NumReusedThisFrame = 0;
	uint32 NumReleasedThisFrame = 0;
	uint64 TotalCreated = 0;
	uint64 TotalReused = 0;
	uint64 MemoryBytes = 0;
};

class FRenderTargetPool {
public:
	explicit FRenderTargetPool(RenderingDevice *InDevice) :
			Device(InDevice) {}
	~FRenderTargetPool();

	/** Returns a free element matching the desc, or creates one. The returned element is referenced by the caller. */
	TRefCountPtr<FPooledRenderTarget> FindFreeElement(const FRHITextureDesc &Desc, const char *Name, uint64 FrameNumber);
	/** Releases elements unused for more than FramesUntilRelease frames. */
	void TickPoolElements(uint64 FrameNumber, uint32 FramesUntilRelease);
	void FreeUnusedResources();
	void ReleaseAll();

	const FRDGPoolStats &GetStats() const { return Stats; }
	void ResetFrameStats();
	uint32 GetNumElements() const { return Elements.size(); }

private:
	RenderingDevice *Device;
	LocalVector<TRefCountPtr<FPooledRenderTarget>> Elements;
	FRDGPoolStats Stats;
};

class FRDGBufferPool {
public:
	explicit FRDGBufferPool(RenderingDevice *InDevice) :
			Device(InDevice) {}
	~FRDGBufferPool();

	TRefCountPtr<FRDGPooledBuffer> FindFreeBuffer(const FRDGBufferDesc &Desc, const char *Name, uint64 FrameNumber, ERDBufferType Type = ERDBufferType::Storage);
	void TickPoolElements(uint64 FrameNumber, uint32 FramesUntilRelease);
	void FreeUnusedResources();
	void ReleaseAll();

	const FRDGPoolStats &GetStats() const { return Stats; }
	void ResetFrameStats();
	uint32 GetNumElements() const { return Elements.size(); }

	/** Creates a buffer that is not pooled (e.g. for external use). */
	static TRefCountPtr<FRDGPooledBuffer> CreateBuffer(RenderingDevice *Device, const FRDGBufferDesc &Desc, const char *Name, bool bPooled, ERDBufferType Type = ERDBufferType::Storage);
	/** RD buffer type for a desc: uniform buffers by usage, vertex / index buffers when bound with VertexOrIndexBuffer access. */
	static ERDBufferType GetRDBufferType(const FRDGBufferDesc &Desc, bool bVertexOrIndexBinding);

private:
	RenderingDevice *Device;
	LocalVector<TRefCountPtr<FRDGPooledBuffer>> Elements;
	FRDGPoolStats Stats;
};

/** Creates a texture outside of any graph (UE's AllocatePooledTexture / GRenderTargetPool.FindFreeElement). */
TRefCountPtr<IPooledRenderTarget> AllocatePooledTexture(RenderingDevice *Device, const FRDGTextureDesc &Desc, const char *Name);
TRefCountPtr<IPooledRenderTarget> AllocatePooledTexture(const FRDGTextureDesc &Desc, const char *Name);
TRefCountPtr<FRDGPooledBuffer> AllocatePooledBuffer(RenderingDevice *Device, const FRDGBufferDesc &Desc, const char *Name);
TRefCountPtr<FRDGPooledBuffer> AllocatePooledBuffer(const FRDGBufferDesc &Desc, const char *Name);

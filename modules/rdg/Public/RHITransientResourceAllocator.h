#pragma once

// Transient resource allocator with memory aliasing (UE's IRHITransientResourceAllocator / FRHITransientHeap from RHICore).
// Transient textures and buffers of a graph are placed in large RenderingDevice memory heaps. Memory is allocated at the
// first pass that uses a resource and given back after its last pass, so resources with disjoint lifetimes share memory.
// Placed RD resources are cached per (heap, offset, desc) and reused by later graphs. Every allocation issues an aliasing
// acquire to the RenderingDevice, which makes Godot's graph order the GPU work of resources that overlap in memory.
// With async compute, allocations carry FRHITransientAllocationFences so memory used by one queue isn't aliased by the
// other one inside an async compute fork / join interval.

#include "RenderGraphResources.h"

class FRHITransientHeap;

enum class ERHITransientResourceType : uint8 {
	Texture,
	Buffer
};

/** UE's FRHITransientAllocationFences: where on the GPU timeline a transient allocation is acquired / discarded. Values
 *  are pass indices of the graph (0 is the prologue, then the passes from 1, and the epilogue). A resource used on async
 *  compute also carries the graphics fork / join interval of its async compute pass, because async compute work in that
 *  interval runs in parallel with the graphics passes inside it. Memory can't be aliased by allocations whose regions
 *  contain each other (Contains): both pipes could use it at the same time. */
class FRHITransientAllocationFences {
public:
	static constexpr uint32 Invalid = UINT32_MAX;

	struct FInterval {
		uint32 Min = Invalid;
		uint32 Max = Invalid;
		bool IsValid() const { return Min != Invalid && Max != Invalid; }
	};

	FRHITransientAllocationFences() = default;
	/** Expects the pipelines the transient allocation was accessed on. */
	explicit FRHITransientAllocationFences(ERHIPipeline InPipelines) :
			Pipelines(InPipelines) {}

	void SetGraphics(uint32 InGraphics) { Graphics = InGraphics; }
	void SetAsyncCompute(uint32 InAsyncCompute, FInterval InGraphicsForkJoin) {
		AsyncCompute = InAsyncCompute;
		GraphicsForkJoin = InGraphicsForkJoin;
	}

	ERHIPipeline GetPipelines() const { return Pipelines; }
	uint32 GetGraphics() const { return Graphics; }
	uint32 GetAsyncCompute() const { return AsyncCompute; }
	FInterval GetGraphicsForkJoin() const { return GraphicsForkJoin; }

	/** Whether the regions of a discarded allocation and of a new one contain each other, i.e. the memory could be used by
	 *  both pipes at the same time and can't be aliased. */
	static bool Contains(const FRHITransientAllocationFences &Discard, const FRHITransientAllocationFences &Acquire) {
		return Contains(Discard.GraphicsForkJoin, Acquire.Graphics) || Contains(Acquire.GraphicsForkJoin, Discard.Graphics);
	}

private:
	static bool Contains(FInterval Interval, uint32 Element) { return Interval.IsValid() && Element != Invalid && Element > Interval.Min && Element < Interval.Max; }

	uint32 Graphics = Invalid;
	uint32 AsyncCompute = Invalid;
	FInterval GraphicsForkJoin;
	ERHIPipeline Pipelines = ERHIPipeline::None;
};

struct FRHITransientHeapAllocation {
	FRHITransientHeap *Heap = nullptr;
	uint64 Offset = 0;
	uint64 Size = 0;
	uint64 Alignment = 0;

	bool IsValid() const { return Heap != nullptr; }
	bool Overlaps(const FRHITransientHeapAllocation &Other) const { return Heap == Other.Heap && Offset < Other.Offset + Other.Size && Other.Offset < Offset + Size; }
};

/** A placed RD resource, cached by the allocator. Only valid between CreateTexture / CreateBuffer and DeallocateMemory. */
class FRHITransientResource {
public:
	virtual ~FRHITransientResource() = default;

	const ERHITransientResourceType Type;
	const FRHITransientHeapAllocation &GetHeapAllocation() const { return HeapAllocation; }
	bool IsAllocated() const { return bAllocated; }
	virtual RID GetRID() const = 0;

protected:
	FRHITransientResource(ERHITransientResourceType InType, const FRHITransientHeapAllocation &InHeapAllocation) :
			Type(InType), HeapAllocation(InHeapAllocation) {}

	FRHITransientHeapAllocation HeapAllocation;
	uint64 LastUsedFrame = 0;
	bool bAllocated = false;

	friend class FRHITransientResourceAllocator;
};

class FRHITransientTexture final : public FRHITransientResource {
public:
	FRHITransientTexture(FRHITexture *InTexture, const FRHITransientHeapAllocation &InHeapAllocation) :
			FRHITransientResource(ERHITransientResourceType::Texture, InHeapAllocation), Texture(InTexture) {}

	FRHITexture *GetRHI() const { return Texture.GetReference(); }
	const FRHITextureDesc &GetDesc() const { return Texture->GetDesc(); }
	RID GetRID() const override { return Texture->GetRID(); }

private:
	TRefCountPtr<FRHITexture> Texture;
};

class FRHITransientBuffer final : public FRHITransientResource {
public:
	FRHITransientBuffer(FRHIBuffer *InBuffer, const FRDGBufferDesc &InDesc, const FRHITransientHeapAllocation &InHeapAllocation) :
			FRHITransientResource(ERHITransientResourceType::Buffer, InHeapAllocation), Desc(InDesc), Buffer(InBuffer) {}

	const FRDGBufferDesc Desc;
	FRHIBuffer *GetRHI() const { return Buffer.GetReference(); }
	RID GetRID() const override { return Buffer->GetRID(); }

private:
	TRefCountPtr<FRHIBuffer> Buffer;
};

/** A RenderingDevice memory heap with a first-fit range allocator. */
class FRHITransientHeap {
public:
	FRHITransientHeap(RenderingDevice *InDevice, RID InHeapRID, uint64 InCapacity);
	~FRHITransientHeap();

	RID GetRID() const { return HeapRID; }
	uint64 GetCapacity() const { return Capacity; }
	uint64 GetUsedSize() const { return UsedSize; }
	bool IsEmpty() const { return UsedSize == 0; }

	/** First fit that skips memory discarded by allocations whose fences contain the new one (see FRHITransientAllocationFences). */
	bool Allocate(uint64 Size, uint64 Alignment, const FRHITransientAllocationFences &Fences, uint64 &OutOffset);
	void Deallocate(uint64 Offset, uint64 Size, const FRHITransientAllocationFences &Fences);
	/** Forgets the fences of the discarded ranges (they are pass indices of one graph). */
	void Flush();

private:
	struct FRange {
		uint64 Offset = 0;
		uint64 Size = 0;
	};

	RenderingDevice *Device;
	RID HeapRID;
	uint64 Capacity = 0;
	uint64 UsedSize = 0;
	uint64 LastUsedFrame = 0;
	/** Free ranges sorted by offset, adjacent ranges merged. */
	LocalVector<FRange> FreeRanges;
	/** Ranges given back during the current graph with the fences of their last use. */
	struct FDiscardedRange {
		uint64 Offset = 0;
		uint64 Size = 0;
		FRHITransientAllocationFences Fences;
	};
	LocalVector<FDiscardedRange> DiscardedRanges;
	uint32 NumAsyncComputeAliasingSkips = 0;
	/** Cache of the placed resources created in this heap. */
	LocalVector<FRHITransientResource *> Resources;

	friend class FRHITransientResourceAllocator;
};

struct FRHITransientAllocationStats {
	uint32 NumHeaps = 0;
	uint64 HeapMemoryBytes = 0;
	uint32 NumCachedTextures = 0;
	uint32 NumCachedBuffers = 0;
	uint64 TotalTexturesCreated = 0;
	uint64 TotalBuffersCreated = 0;
	uint64 TotalCacheHits = 0;
	/** Since the last ResetCycleStats() (one engine frame on the main device). */
	uint32 NumAllocations = 0;
	uint64 AllocatedBytes = 0;
	uint64 PeakUsedBytes = 0;
	uint32 NumFailedPlacements = 0;
	/** Placements moved to keep memory used by one pipe from being aliased by the other inside an async compute region. */
	uint32 NumAsyncComputeAliasingSkips = 0;
};

class FRHITransientResourceAllocator {
public:
	explicit FRHITransientResourceAllocator(RenderingDevice *InDevice);
	~FRHITransientResourceAllocator();

	/** Returns nullptr when the texture can't be placed (unsupported driver / format), callers fall back to the pool.
	 *  The fences tell where the resource is first used (UE's FRDGBuilder::GetAllocateFences). */
	FRHITransientTexture *CreateTexture(const FRHITextureDesc &Desc, const char *Name, uint64 FrameNumber, const FRHITransientAllocationFences &Fences = FRHITransientAllocationFences(ERHIPipeline::Graphics));
	FRHITransientBuffer *CreateBuffer(const FRDGBufferDesc &Desc, const char *Name, uint64 FrameNumber, const FRHITransientAllocationFences &Fences = FRHITransientAllocationFences(ERHIPipeline::Graphics));
	/** Gives the memory back after the last use (fences: UE's FRDGBuilder::GetDeallocateFences); later allocations may alias it. */
	void DeallocateMemory(FRHITransientResource *Resource, const FRHITransientAllocationFences &Fences = FRHITransientAllocationFences(ERHIPipeline::Graphics));
	/** End of a graph: the fences of the discarded memory don't apply to the next graph (UE's IRHITransientResourceAllocator::Flush). */
	void Flush();

	/** Releases cached resources and empty heaps unused for RHI.TransientAllocator.GarbageCollectLatency frames. */
	void GarbageCollect(uint64 FrameNumber);
	void ReleaseAll();

	bool IsSupported() const { return !bUnsupported; }
	const FRHITransientAllocationStats &GetStats() const { return Stats; }
	void ResetCycleStats();

private:
	struct FTextureRequirements {
		FRHITextureDesc Desc;
		RenderingDevice::MemoryRequirements Requirements;
		bool bPlaceable = false;
	};
	struct FBufferRequirements {
		uint32 Size = 0;
		bool bIndirect = false;
		RenderingDevice::MemoryRequirements Requirements;
		bool bPlaceable = false;
	};

	const FTextureRequirements &GetTextureRequirements(const FRHITextureDesc &Desc);
	const FBufferRequirements &GetBufferRequirements(uint32 Size, bool bIndirect);
	bool AllocateMemory(uint64 Size, uint64 Alignment, const FRHITransientAllocationFences &Fences, FRHITransientHeapAllocation &OutAllocation, uint64 FrameNumber);
	FRHITransientHeap *CreateHeap(uint64 MinimumSize);
	void AcquireAliasing(FRHITransientResource *Resource);
	void ReleaseResource(FRHITransientHeap *Heap, uint32 Index);
	void UpdateStats();

	RenderingDevice *Device;
	LocalVector<FRHITransientHeap *> Heaps;
	LocalVector<FTextureRequirements> TextureRequirements;
	LocalVector<FBufferRequirements> BufferRequirements;
	FRHITransientAllocationStats Stats;
	uint64 CurrentUsedBytes = 0;
	bool bUnsupported = false;
};

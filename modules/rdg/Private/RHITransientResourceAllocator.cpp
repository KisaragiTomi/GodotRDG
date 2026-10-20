#include "RHITransientResourceAllocator.h"

#include "HAL/IConsoleManager.h"

#include "servers/rendering/rendering_device.h"

static TAutoConsoleVariable<int32> CVarRHITransientAllocatorMinimumHeapSize(
		"RHI.TransientAllocator.MinimumHeapSize", 128,
		"Minimum size of an RHI transient heap in MB. Heaps will default to this size and grow to the maximum based on the first allocation (Default 128).",
		ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRHITransientAllocatorBufferCacheSize(
		"RHI.TransientAllocator.BufferCacheSize", 64,
		"The maximum number of RHI buffers to cache on each heap before garbage collecting.",
		ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRHITransientAllocatorTextureCacheSize(
		"RHI.TransientAllocator.TextureCacheSize", 64,
		"The maximum number of RHI textures to cache on each heap before garbage collecting.",
		ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRHITransientAllocatorGarbageCollectLatency(
		"RHI.TransientAllocator.GarbageCollectLatency", 16,
		"Amount of update cycles before memory is reclaimed.",
		ECVF_RenderThreadSafe);

namespace {
constexpr uint64 HeapSizeGranularity = 4ull * 1024 * 1024;

uint64 AlignUp(uint64 Value, uint64 Alignment) {
	return Alignment > 1 ? (Value + Alignment - 1) / Alignment * Alignment : Value;
}
} //namespace

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// FRHITransientHeap

FRHITransientHeap::FRHITransientHeap(RenderingDevice *InDevice, RID InHeapRID, uint64 InCapacity) :
		Device(InDevice), HeapRID(InHeapRID), Capacity(InCapacity) {
	FRange Range;
	Range.Size = Capacity;
	FreeRanges.push_back(Range);
}

FRHITransientHeap::~FRHITransientHeap() {
	for (FRHITransientResource *Resource : Resources) delete Resource;
	Resources.clear();
	// Freeing the heap also frees anything still placed in it (RD dependency).
	if (HeapRID.is_valid() && Device->memory_heap_is_valid(HeapRID)) Device->free_rid(HeapRID);
}

bool FRHITransientHeap::Allocate(uint64 Size, uint64 Alignment, const FRHITransientAllocationFences &Fences, uint64 &OutOffset) {
	for (uint32 Index = 0; Index < FreeRanges.size(); Index++) {
		FRange &Range = FreeRanges[Index];
		const uint64 RangeEnd = Range.Offset + Range.Size;
		uint64 Offset = AlignUp(Range.Offset, Alignment);

		// Skip memory that the other pipe may still use at the same time (UE's FRHITransientAllocationFences::Contains
		// check in FRHITransientHeapAllocator::Allocate). Aliasing is allowed again once the async compute work joined.
		bool bMoved = true;
		while (bMoved && Offset + Size <= RangeEnd) {
			bMoved = false;
			for (const FDiscardedRange &Discarded : DiscardedRanges) {
				const bool bOverlaps = Discarded.Offset < Offset + Size && Offset < Discarded.Offset + Discarded.Size;
				if (!bOverlaps || !FRHITransientAllocationFences::Contains(Discarded.Fences, Fences)) continue;
				Offset = AlignUp(Discarded.Offset + Discarded.Size, Alignment);
				NumAsyncComputeAliasingSkips++;
				bMoved = true;
			}
		}
		if (Offset + Size > RangeEnd) continue;

		// Split the range into [padding][allocation][tail].
		const FRange Tail = { Offset + Size, RangeEnd - (Offset + Size) };
		const uint64 Padding = Offset - Range.Offset;
		if (Padding > 0) {
			Range.Size = Padding;
			if (Tail.Size > 0) FreeRanges.insert(Index + 1, Tail);
		} else if (Tail.Size > 0) {
			Range = Tail;
		} else {
			FreeRanges.remove_at(Index);
		}

		UsedSize += Size;
		OutOffset = Offset;
		return true;
	}
	return false;
}

void FRHITransientHeap::Deallocate(uint64 Offset, uint64 Size, const FRHITransientAllocationFences &Fences) {
	if (Fences.GetPipelines() != ERHIPipeline::None) {
		FDiscardedRange Discarded;
		Discarded.Offset = Offset;
		Discarded.Size = Size;
		Discarded.Fences = Fences;
		DiscardedRanges.push_back(Discarded);
	}

	uint32 Index = 0;
	while (Index < FreeRanges.size() && FreeRanges[Index].Offset < Offset) Index++;
	FreeRanges.insert(Index, { Offset, Size });

	// Merge with the next and previous ranges.
	if (Index + 1 < FreeRanges.size() && FreeRanges[Index].Offset + FreeRanges[Index].Size == FreeRanges[Index + 1].Offset) {
		FreeRanges[Index].Size += FreeRanges[Index + 1].Size;
		FreeRanges.remove_at(Index + 1);
	}
	if (Index > 0 && FreeRanges[Index - 1].Offset + FreeRanges[Index - 1].Size == FreeRanges[Index].Offset) {
		FreeRanges[Index - 1].Size += FreeRanges[Index].Size;
		FreeRanges.remove_at(Index);
	}

	DEV_ASSERT(UsedSize >= Size);
	UsedSize -= Size;
}

void FRHITransientHeap::Flush() {
	DiscardedRanges.clear();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// FRHITransientResourceAllocator

FRHITransientResourceAllocator::FRHITransientResourceAllocator(RenderingDevice *InDevice) :
		Device(InDevice) {}

FRHITransientResourceAllocator::~FRHITransientResourceAllocator() {
	ReleaseAll();
}

const FRHITransientResourceAllocator::FTextureRequirements &FRHITransientResourceAllocator::GetTextureRequirements(const FRHITextureDesc &Desc) {
	for (const FTextureRequirements &Entry : TextureRequirements) {
		if (Entry.Desc == Desc) return Entry;
	}
	FTextureRequirements Entry;
	Entry.Desc = Desc;
	Entry.bPlaceable = Device->texture_get_memory_requirements(ToRDTextureFormat(Device, Desc), Entry.Requirements);
	TextureRequirements.push_back(Entry);
	return TextureRequirements[TextureRequirements.size() - 1];
}

const FRHITransientResourceAllocator::FBufferRequirements &FRHITransientResourceAllocator::GetBufferRequirements(uint32 Size, bool bIndirect) {
	for (const FBufferRequirements &Entry : BufferRequirements) {
		if (Entry.Size == Size && Entry.bIndirect == bIndirect) return Entry;
	}
	FBufferRequirements Entry;
	Entry.Size = Size;
	Entry.bIndirect = bIndirect;
	BitField<RenderingDevice::StorageBufferUsage> Usage = 0;
	if (bIndirect) Usage.set_flag(RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	Entry.bPlaceable = Device->storage_buffer_get_memory_requirements(Size, Usage, Entry.Requirements);
	BufferRequirements.push_back(Entry);
	return BufferRequirements[BufferRequirements.size() - 1];
}

FRHITransientHeap *FRHITransientResourceAllocator::CreateHeap(uint64 MinimumSize) {
	const uint64 MinimumHeapSize = (uint64)FMath::Max(1, CVarRHITransientAllocatorMinimumHeapSize.GetValueOnRenderThread()) * 1024 * 1024;
	const uint64 Capacity = AlignUp(FMath::Max(MinimumHeapSize, MinimumSize), HeapSizeGranularity);
	const RID HeapRID = Device->memory_heap_create(Capacity);
	if (!HeapRID.is_valid()) {
		// Only the first heap tells whether the driver supports placed resources at all.
		if (Heaps.is_empty()) bUnsupported = true;
		return nullptr;
	}
	Device->set_resource_name(HeapRID, vformat("RDG Transient Heap %d", Heaps.size()));
	FRHITransientHeap *Heap = new FRHITransientHeap(Device, HeapRID, Capacity);
	Heaps.push_back(Heap);
	return Heap;
}

bool FRHITransientResourceAllocator::AllocateMemory(uint64 Size, uint64 Alignment, const FRHITransientAllocationFences &Fences, FRHITransientHeapAllocation &OutAllocation, uint64 FrameNumber) {
	FRHITransientHeap *Heap = nullptr;
	uint64 Offset = 0;
	for (FRHITransientHeap *Candidate : Heaps) {
		const uint32 SkipsBefore = Candidate->NumAsyncComputeAliasingSkips;
		const bool bAllocated = Candidate->Allocate(Size, Alignment, Fences, Offset);
		Stats.NumAsyncComputeAliasingSkips += Candidate->NumAsyncComputeAliasingSkips - SkipsBefore;
		if (bAllocated) {
			Heap = Candidate;
			break;
		}
	}
	if (!Heap) {
		Heap = CreateHeap(Size);
		if (!Heap || !Heap->Allocate(Size, Alignment, Fences, Offset)) return false;
	}
	Heap->LastUsedFrame = FrameNumber;

	OutAllocation.Heap = Heap;
	OutAllocation.Offset = Offset;
	OutAllocation.Size = Size;
	OutAllocation.Alignment = Alignment;

	CurrentUsedBytes += Size;
	Stats.NumAllocations++;
	Stats.AllocatedBytes += Size;
	Stats.PeakUsedBytes = FMath::Max(Stats.PeakUsedBytes, CurrentUsedBytes);
	return true;
}

void FRHITransientResourceAllocator::AcquireAliasing(FRHITransientResource *Resource) {
	// Every cached resource overlapping the new allocation may still have GPU work in flight on that memory (this graph,
	// or a previous frame). UE builds the same overlap list in FRHITransientHeap::Acquire.
	const FRHITransientHeapAllocation &Allocation = Resource->HeapAllocation;
	LocalVector<RID> Overlaps;
	for (FRHITransientResource *Other : Allocation.Heap->Resources) {
		if (Other != Resource && Other->HeapAllocation.Overlaps(Allocation)) Overlaps.push_back(Other->GetRID());
	}
	Device->resource_aliasing_acquire(Resource->GetRID(), VectorView<RID>(Overlaps.ptr(), Overlaps.size()));
}

FRHITransientTexture *FRHITransientResourceAllocator::CreateTexture(const FRHITextureDesc &Desc, const char *Name, uint64 FrameNumber, const FRHITransientAllocationFences &Fences) {
	if (bUnsupported) return nullptr;
	const FTextureRequirements &Requirements = GetTextureRequirements(Desc);
	if (!Requirements.bPlaceable) {
		Stats.NumFailedPlacements++;
		return nullptr;
	}

	FRHITransientHeapAllocation Allocation;
	if (!AllocateMemory(Requirements.Requirements.size, Requirements.Requirements.alignment, Fences, Allocation, FrameNumber)) {
		Stats.NumFailedPlacements++;
		return nullptr;
	}

	// Reuse a cached placed texture with the same desc at the same offset (UE's FRHITransientResourceCache).
	FRHITransientHeap *Heap = Allocation.Heap;
	FRHITransientTexture *Texture = nullptr;
	for (FRHITransientResource *Resource : Heap->Resources) {
		if (Resource->bAllocated || Resource->Type != ERHITransientResourceType::Texture || Resource->HeapAllocation.Offset != Allocation.Offset) continue;
		FRHITransientTexture *Candidate = static_cast<FRHITransientTexture *>(Resource);
		if (Candidate->GetDesc() == Desc) {
			Texture = Candidate;
			Stats.TotalCacheHits++;
			break;
		}
	}

	if (!Texture) {
		const RID TextureRID = Device->texture_create_placed(ToRDTextureFormat(Device, Desc), RenderingDevice::TextureView(), Heap->GetRID(), Allocation.Offset);
		if (!TextureRID.is_valid()) {
			Heap->Deallocate(Allocation.Offset, Allocation.Size, FRHITransientAllocationFences());
			CurrentUsedBytes -= Allocation.Size;
			Stats.NumFailedPlacements++;
			return nullptr;
		}
		Texture = new FRHITransientTexture(new FRHITexture(Device, TextureRID, Desc, String::utf8(Name), true), Allocation);
		Heap->Resources.push_back(Texture);
		Stats.TotalTexturesCreated++;
		UpdateStats();
	}

	Texture->HeapAllocation = Allocation;
	Texture->bAllocated = true;
	Texture->LastUsedFrame = FrameNumber;
	Device->set_resource_name(Texture->GetRID(), vformat("RDG Transient: %s", Name));
	AcquireAliasing(Texture);
	return Texture;
}

FRHITransientBuffer *FRHITransientResourceAllocator::CreateBuffer(const FRDGBufferDesc &Desc, const char *Name, uint64 FrameNumber, const FRHITransientAllocationFences &Fences) {
	if (bUnsupported || EnumHasAnyFlags(Desc.Usage, BUF_UniformBuffer)) return nullptr;
	const uint32 Size = (FMath::Max(4u, Desc.GetSize()) + 3u) & ~3u;
	const bool bIndirect = EnumHasAnyFlags(Desc.Usage, BUF_DrawIndirect);
	const FBufferRequirements &Requirements = GetBufferRequirements(Size, bIndirect);
	if (!Requirements.bPlaceable) {
		Stats.NumFailedPlacements++;
		return nullptr;
	}

	FRHITransientHeapAllocation Allocation;
	if (!AllocateMemory(Requirements.Requirements.size, Requirements.Requirements.alignment, Fences, Allocation, FrameNumber)) {
		Stats.NumFailedPlacements++;
		return nullptr;
	}

	FRHITransientHeap *Heap = Allocation.Heap;
	FRHITransientBuffer *Buffer = nullptr;
	for (FRHITransientResource *Resource : Heap->Resources) {
		if (Resource->bAllocated || Resource->Type != ERHITransientResourceType::Buffer || Resource->HeapAllocation.Offset != Allocation.Offset) continue;
		FRHITransientBuffer *Candidate = static_cast<FRHITransientBuffer *>(Resource);
		if (Candidate->Desc == Desc) {
			Buffer = Candidate;
			Stats.TotalCacheHits++;
			break;
		}
	}

	if (!Buffer) {
		BitField<RenderingDevice::StorageBufferUsage> Usage = 0;
		if (bIndirect) Usage.set_flag(RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		const RID BufferRID = Device->storage_buffer_create_placed(Size, Usage, Heap->GetRID(), Allocation.Offset);
		if (!BufferRID.is_valid()) {
			Heap->Deallocate(Allocation.Offset, Allocation.Size, FRHITransientAllocationFences());
			CurrentUsedBytes -= Allocation.Size;
			Stats.NumFailedPlacements++;
			return nullptr;
		}
		Buffer = new FRHITransientBuffer(new FRHIBuffer(Device, BufferRID, Size, Desc.BytesPerElement, Desc.Usage, String::utf8(Name), true), Desc, Allocation);
		Heap->Resources.push_back(Buffer);
		Stats.TotalBuffersCreated++;
		UpdateStats();
	}

	Buffer->HeapAllocation = Allocation;
	Buffer->bAllocated = true;
	Buffer->LastUsedFrame = FrameNumber;
	Device->set_resource_name(Buffer->GetRID(), vformat("RDG Transient: %s", Name));
	AcquireAliasing(Buffer);
	return Buffer;
}

void FRHITransientResourceAllocator::DeallocateMemory(FRHITransientResource *Resource, const FRHITransientAllocationFences &Fences) {
	ERR_FAIL_NULL(Resource);
	ERR_FAIL_COND_MSG(!Resource->bAllocated, "[RDG] Transient resource deallocated twice.");
	const FRHITransientHeapAllocation &Allocation = Resource->HeapAllocation;
	Allocation.Heap->Deallocate(Allocation.Offset, Allocation.Size, Fences);
	CurrentUsedBytes -= Allocation.Size;
	Resource->bAllocated = false;
}

void FRHITransientResourceAllocator::Flush() {
	for (FRHITransientHeap *Heap : Heaps) Heap->Flush();
}

void FRHITransientResourceAllocator::ReleaseResource(FRHITransientHeap *Heap, uint32 Index) {
	delete Heap->Resources[Index];
	Heap->Resources.remove_at_unordered(Index);
}

void FRHITransientResourceAllocator::GarbageCollect(uint64 FrameNumber) {
	const uint64 Latency = (uint64)FMath::Max(1, CVarRHITransientAllocatorGarbageCollectLatency.GetValueOnRenderThread());
	const uint32 TextureCacheSize = (uint32)FMath::Max(0, CVarRHITransientAllocatorTextureCacheSize.GetValueOnRenderThread());
	const uint32 BufferCacheSize = (uint32)FMath::Max(0, CVarRHITransientAllocatorBufferCacheSize.GetValueOnRenderThread());

	for (int64 HeapIndex = (int64)Heaps.size() - 1; HeapIndex >= 0; HeapIndex--) {
		FRHITransientHeap *Heap = Heaps[HeapIndex];

		// Stale placed resources.
		for (int64 Index = (int64)Heap->Resources.size() - 1; Index >= 0; Index--) {
			FRHITransientResource *Resource = Heap->Resources[Index];
			if (!Resource->bAllocated && FrameNumber - Resource->LastUsedFrame > Latency) ReleaseResource(Heap, Index);
		}

		// Cache size limits: drop the least recently used free resources.
		for (const ERHITransientResourceType Type : { ERHITransientResourceType::Texture, ERHITransientResourceType::Buffer }) {
			const uint32 Limit = Type == ERHITransientResourceType::Texture ? TextureCacheSize : BufferCacheSize;
			while (true) {
				uint32 Count = 0;
				int64 OldestIndex = -1;
				for (uint32 Index = 0; Index < Heap->Resources.size(); Index++) {
					FRHITransientResource *Resource = Heap->Resources[Index];
					if (Resource->Type != Type) continue;
					Count++;
					if (!Resource->bAllocated && (OldestIndex < 0 || Resource->LastUsedFrame < Heap->Resources[OldestIndex]->LastUsedFrame)) OldestIndex = Index;
				}
				if (Count <= Limit || OldestIndex < 0) break;
				ReleaseResource(Heap, (uint32)OldestIndex);
			}
		}

		// Empty heaps nobody allocated from for a while.
		if (Heap->IsEmpty() && FrameNumber - Heap->LastUsedFrame > Latency) {
			delete Heap;
			Heaps.remove_at(HeapIndex);
		}
	}
	UpdateStats();
}

void FRHITransientResourceAllocator::ReleaseAll() {
	for (FRHITransientHeap *Heap : Heaps) {
		for (FRHITransientResource *Resource : Heap->Resources) {
			if (Resource->bAllocated) WARN_PRINT("[RDG] Transient heap released while one of its resources is still allocated.");
		}
		delete Heap;
	}
	Heaps.clear();
	TextureRequirements.clear();
	BufferRequirements.clear();
	CurrentUsedBytes = 0;
	Stats = FRHITransientAllocationStats();
}

void FRHITransientResourceAllocator::ResetCycleStats() {
	Stats.NumAllocations = 0;
	Stats.AllocatedBytes = 0;
	Stats.PeakUsedBytes = CurrentUsedBytes;
	Stats.NumFailedPlacements = 0;
	Stats.NumAsyncComputeAliasingSkips = 0;
}

void FRHITransientResourceAllocator::UpdateStats() {
	Stats.NumHeaps = Heaps.size();
	Stats.HeapMemoryBytes = 0;
	Stats.NumCachedTextures = 0;
	Stats.NumCachedBuffers = 0;
	for (FRHITransientHeap *Heap : Heaps) {
		Stats.HeapMemoryBytes += Heap->GetCapacity();
		for (FRHITransientResource *Resource : Heap->Resources) {
			if (Resource->Type == ERHITransientResourceType::Texture) Stats.NumCachedTextures++;
			else Stats.NumCachedBuffers++;
		}
	}
}

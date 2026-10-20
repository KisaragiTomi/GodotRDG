#pragma once

#include <functional>

#include "RHIDefinitions.h"
#include "RHIResources.h"

class FRDGBuilder;
class FRDGPass;
class FRDGResource;
class FRDGViewableResource;
class FRDGTexture;
class FRDGBuffer;
class FRDGView;
class FRDGTextureSRV;
class FRDGTextureUAV;
class FRDGBufferSRV;
class FRDGBufferUAV;
class FRDGUniformBuffer;
class FRDGEventScope;
template <typename TUniformStruct>
class TRDGUniformBuffer;

using FRDGPassRef = FRDGPass *;
using FRDGTextureRef = FRDGTexture *;
using FRDGBufferRef = FRDGBuffer *;
using FRDGTextureSRVRef = FRDGTextureSRV *;
using FRDGTextureUAVRef = FRDGTextureUAV *;
using FRDGBufferSRVRef = FRDGBufferSRV *;
using FRDGBufferUAVRef = FRDGBufferUAV *;
using FRDGUniformBufferRef = FRDGUniformBuffer *;
template <typename TUniformStruct>
using TRDGUniformBufferRef = TRDGUniformBuffer<TUniformStruct> *;

/** Flags passed to FRDGBuilder::AddPass, identical to UE's ERDGPassFlags. */
enum class ERDGPassFlags : uint16 {
	/** Pass without parameters. */
	None = 0,
	/** Pass uses rasterization on the graphics pipe. */
	Raster = 1 << 0,
	/** Pass uses compute on the graphics pipe. */
	Compute = 1 << 1,
	/** Pass uses compute on the async compute pipe (the RenderingDevice's async compute queue, Compute when unavailable). */
	AsyncCompute = 1 << 2,
	/** Pass uses copy commands on the graphics pipe. */
	Copy = 1 << 3,
	/** Pass (and its producers) will never be culled. */
	NeverCull = 1 << 4,
	/** Render pass begin / end is skipped and left to the user. Only valid with Raster. */
	SkipRenderPass = 1 << 5,
	/** Pass will never have its render pass merged with other passes. */
	NeverMerge = 1 << 6,
	/** Pass will never run off the render thread. */
	NeverParallel = 1 << 7,
	/** Copy pass that reads back to the CPU. */
	Readback = Copy | NeverCull,
};
ENUM_CLASS_FLAGS(ERDGPassFlags)

enum class ERDGTextureFlags : uint8 {
	None = 0,
	/** Keeps the texture alive (in the pool) across frames; data is preserved, so it is never marked discardable. */
	MultiFrame = 1 << 0,
	/** The resource is not tracked by the graph for access / transitions. Only valid for registered resources. */
	SkipTracking = 1 << 1,
	/** Force-disables transient allocation (RDG pool reuse inside the graph). */
	ForceImmediateFirstBarrier = 1 << 2,
	MaintainCompression = 1 << 3,
};
ENUM_CLASS_FLAGS(ERDGTextureFlags)

enum class ERDGBufferFlags : uint8 {
	None = 0,
	MultiFrame = 1 << 0,
	SkipTracking = 1 << 1,
	ForceImmediateFirstBarrier = 1 << 2,
};
ENUM_CLASS_FLAGS(ERDGBufferFlags)

enum class ERDGUnorderedAccessViewFlags : uint8 {
	None = 0,
	SkipBarrier = 1 << 0,
};
ENUM_CLASS_FLAGS(ERDGUnorderedAccessViewFlags)

enum class ERDGInitialDataFlags : uint8 {
	None = 0,
	/** Data is not copied; the caller guarantees its lifetime until the graph executes. */
	NoCopy = 1 << 0,
};
ENUM_CLASS_FLAGS(ERDGInitialDataFlags)

enum class ERDGResourceExtractionFlags : uint8 {
	None = 0,
	AllowTransient = 1 << 0,
};
ENUM_CLASS_FLAGS(ERDGResourceExtractionFlags)

enum class ERDGBuilderFlags : uint8 {
	None = 0,
	/** Setup tasks (AddSetupTask) are launched on worker threads and the pass resource setup is queued for an async task
	 *  (FlushSetupQueue). r.RDG.ParallelSetup / r.RDG.AsyncSetupQueue. */
	ParallelSetup = 1 << 0,
	/** Buffer NumElements / initial data callbacks are resolved by a task while the graph compiles. r.RDG.ParallelCompile. */
	ParallelCompile = 1 << 1,
	/** Pass lambdas may be recorded by parallel tasks (r.RDG.ParallelExecute). */
	ParallelExecute = 1 << 2,
	Parallel = ParallelSetup | ParallelCompile | ParallelExecute,
	/** Deprecated in UE 5.5, use Parallel. */
	AllowParallelExecute = Parallel,
};
ENUM_CLASS_FLAGS(ERDGBuilderFlags)

/** When the graph waits for a setup task (FRDGBuilder::AddSetupTask). */
enum class ERDGSetupTaskWaitPoint : uint8 {
	/** (Default) Synced prior to compilation. Use it when the task mutates RDG resources (buffer upload contents, buffer
	 *  size callbacks, etc.). */
	Compile = 0,
	/** Synced prior to execution. Use it when the task only feeds pass lambdas. */
	Execute = 1,
	MAX
};

/** Buffer callbacks resolved when the graph compiles (after the setup tasks of the Compile wait point). */
using FRDGBufferNumElementsCallback = std::function<uint32()>;
using FRDGBufferInitialDataCallback = std::function<const void *()>;
using FRDGBufferInitialDataSizeCallback = std::function<uint64()>;
using FRDGBufferInitialDataFreeCallback = std::function<void(const void *InData)>;
using FRDGBufferInitialDataFillCallback = std::function<void(void *InData, uint32 InDataSize)>;

enum class ERDGViewableResourceType : uint8 {
	Texture,
	Buffer,
	MAX
};

enum class ERDGViewType : uint8 {
	TextureUAV,
	TextureSRV,
	BufferUAV,
	BufferSRV,
	MAX
};

enum class ERDGTextureAccessFlags : uint8 {
	None = 0,
	RenderTarget = 1 << 0,
};
ENUM_CLASS_FLAGS(ERDGTextureAccessFlags)

/** Async task tag passed to pass lambdas in UE 5.5+. Godot executes passes serially on the render thread. */
struct FRDGAsyncTask {};

/** Handle to a pass inside the builder (index into the pass registry). */
struct FRDGPassHandle {
	static constexpr uint32 NullIndex = 0xFFFFFFFF;
	uint32 Index = NullIndex;

	constexpr FRDGPassHandle() = default;
	explicit constexpr FRDGPassHandle(uint32 InIndex) :
			Index(InIndex) {}
	constexpr bool IsValid() const { return Index != NullIndex; }
	constexpr bool IsNull() const { return Index == NullIndex; }
	constexpr uint32 GetIndex() const { return Index; }
	constexpr bool operator==(FRDGPassHandle O) const { return Index == O.Index; }
	constexpr bool operator!=(FRDGPassHandle O) const { return Index != O.Index; }
	constexpr bool operator<(FRDGPassHandle O) const { return Index < O.Index; }
	constexpr bool operator>(FRDGPassHandle O) const { return Index > O.Index; }
	constexpr bool operator<=(FRDGPassHandle O) const { return Index <= O.Index; }
	constexpr bool operator>=(FRDGPassHandle O) const { return Index >= O.Index; }
	FRDGPassHandle &operator++() {
		++Index;
		return *this;
	}
	FRDGPassHandle &operator--() {
		--Index;
		return *this;
	}
	static FRDGPassHandle Min(FRDGPassHandle A, FRDGPassHandle B) { return A.IsNull() ? B : (B.IsNull() ? A : (A < B ? A : B)); }
	static FRDGPassHandle Max(FRDGPassHandle A, FRDGPassHandle B) { return A.IsNull() ? B : (B.IsNull() ? A : (A > B ? A : B)); }
};

/** Texture subresource range used by SRV / UAV / access enumeration. */
struct FRDGTextureSubresourceRange {
	uint32 MipIndex = 0;
	uint32 NumMips = 0;
	uint32 ArraySlice = 0;
	uint32 NumArraySlices = 0;
	bool IsWholeResource(uint32 InNumMips, uint32 InNumSlices) const { return MipIndex == 0 && ArraySlice == 0 && NumMips == InNumMips && NumArraySlices == InNumSlices; }
};

#pragma once

#include "RenderGraphBlackboard.h"
#include "RenderGraphPass.h"
#include "RHITransientResourceAllocator.h"
#include "RenderGraphResources.h"
#include "Tasks/Task.h"

#include "core/templates/list.h"

#include <functional>

/** Per-graph statistics (subset of UE's RDG stats). */
struct FRDGBuilderStats {
	uint32 NumPasses = 0;
	uint32 NumCulledPasses = 0;
	uint32 NumMergedRenderPasses = 0;
	uint32 NumTextures = 0;
	uint32 NumBuffers = 0;
	uint32 NumExternalTextures = 0;
	uint32 NumExtractedTextures = 0;
	/** Number of distinct RD textures used by the transient (non external) textures of the graph. */
	uint32 NumPhysicalTransientTextures = 0;
	uint32 NumTransientTextures = 0;
	uint32 NumPhysicalTransientBuffers = 0;
	uint32 NumTransientBuffers = 0;
	/** Transient resources placed in transient heaps (memory aliasing); the others came from the resource pools. */
	uint32 NumPlacedTextures = 0;
	uint32 NumPlacedBuffers = 0;
	/** Sum of the placed allocations, i.e. the memory needed without aliasing. */
	uint64 TransientAllocatedBytes = 0;
	/** Highest amount of placed memory in use at once during the graph. */
	uint64 TransientPeakBytes = 0;
	uint32 NumBindingErrors = 0;
	uint32 NumValidationErrors = 0;
	/** Async compute passes that ran on the async compute queue, and the fork / join points between the pipes. */
	uint32 NumAsyncComputePasses = 0;
	uint32 NumAsyncComputeForks = 0;
	uint32 NumAsyncComputeJoins = 0;
	/** Passes recorded by parallel tasks (r.RDG.ParallelExecute) and the parallel pass sets (one task each). */
	uint32 NumParallelPasses = 0;
	uint32 NumParallelPassSets = 0;
	/** Setup tasks launched on worker threads (AddSetupTask), and passes whose resources were set up by the async setup
	 *  queue task (FlushSetupQueue) rather than by Execute(). */
	uint32 NumSetupTasks = 0;
	uint32 NumAsyncSetupPasses = 0;
	bool bParallelSetup = false;
	bool bParallelCompile = false;
	bool bImmediateMode = false;
	bool bAsyncCompute = false;
	bool bParallelExecute = false;
};

/** UE's FRDGBuilder. */
class FRDGBuilder {
public:
	FRDGBuilder(FRHICommandListImmediate &RHICmdList, FRDGEventName Name = {}, ERDGBuilderFlags Flags = ERDGBuilderFlags::None, EShaderPlatform ShaderPlatform = GMaxRHIShaderPlatform);
	FRDGBuilder(const FRDGBuilder &) = delete;
	~FRDGBuilder();

	FRDGTextureRef FindExternalTexture(FRHITexture *Texture) const;
	FRDGTextureRef FindExternalTexture(IPooledRenderTarget *ExternalPooledTexture) const;
	FRDGBufferRef FindExternalBuffer(FRHIBuffer *Buffer) const;
	FRDGBufferRef FindExternalBuffer(FRDGPooledBuffer *ExternalPooledBuffer) const;

	FRDGTextureRef RegisterExternalTexture(const TRefCountPtr<IPooledRenderTarget> &ExternalPooledTexture, ERDGTextureFlags Flags = ERDGTextureFlags::None);
	FRDGTextureRef RegisterExternalTexture(const TRefCountPtr<IPooledRenderTarget> &ExternalPooledTexture, const char *NameIfNotRegistered, ERDGTextureFlags Flags = ERDGTextureFlags::None);
	FRDGBufferRef RegisterExternalBuffer(const TRefCountPtr<FRDGPooledBuffer> &ExternalPooledBuffer, ERDGBufferFlags Flags = ERDGBufferFlags::None);
	FRDGBufferRef RegisterExternalBuffer(const TRefCountPtr<FRDGPooledBuffer> &ExternalPooledBuffer, const char *NameIfNotRegistered, ERDGBufferFlags Flags = ERDGBufferFlags::None);

	FRDGTextureRef CreateTexture(const FRDGTextureDesc &Desc, const char *Name, ERDGTextureFlags Flags = ERDGTextureFlags::None);
	FRDGBufferRef CreateBuffer(const FRDGBufferDesc &Desc, const char *Name, ERDGBufferFlags Flags = ERDGBufferFlags::None);
	/** The number of elements comes from the callback, called when the graph compiles (after the Compile setup tasks). */
	FRDGBufferRef CreateBuffer(const FRDGBufferDesc &Desc, const char *Name, FRDGBufferNumElementsCallback &&NumElementsCallback, ERDGBufferFlags Flags = ERDGBufferFlags::None);

	FRDGTextureSRVRef CreateSRV(const FRDGTextureSRVDesc &Desc);
	FRDGBufferSRVRef CreateSRV(const FRDGBufferSRVDesc &Desc);
	FRDGBufferSRVRef CreateSRV(FRDGBufferRef Buffer, EPixelFormat Format) { return CreateSRV(FRDGBufferSRVDesc(Buffer, Format)); }
	FRDGTextureUAVRef CreateUAV(const FRDGTextureUAVDesc &Desc, ERDGUnorderedAccessViewFlags Flags = ERDGUnorderedAccessViewFlags::None);
	FRDGTextureUAVRef CreateUAV(FRDGTextureRef Texture, ERDGUnorderedAccessViewFlags Flags = ERDGUnorderedAccessViewFlags::None, EPixelFormat Format = PF_Unknown) {
		return CreateUAV(FRDGTextureUAVDesc(Texture, 0, Format), Flags);
	}
	FRDGBufferUAVRef CreateUAV(const FRDGBufferUAVDesc &Desc, ERDGUnorderedAccessViewFlags Flags = ERDGUnorderedAccessViewFlags::None);
	FRDGBufferUAVRef CreateUAV(FRDGBufferRef Buffer, EPixelFormat Format, ERDGUnorderedAccessViewFlags Flags = ERDGUnorderedAccessViewFlags::None) {
		return CreateUAV(FRDGBufferUAVDesc(Buffer, Format), Flags);
	}

	template <typename ParameterStructType>
	TRDGUniformBufferRef<ParameterStructType> CreateUniformBuffer(const ParameterStructType *ParameterStruct) {
		TRDGUniformBuffer<ParameterStructType> *UniformBuffer = AllocObjectInternal<TRDGUniformBuffer<ParameterStructType>>(CopyName(ParameterStructType::FTypeInfo::GetStructMetadata()->GetShaderVariableName()), ParameterStruct);
		RegisterUniformBuffer(UniformBuffer);
		return UniformBuffer;
	}

	void *Alloc(uint64 SizeInBytes, uint32 AlignInBytes = 16);
	template <typename PODType>
	PODType *AllocPOD() { return reinterpret_cast<PODType *>(Alloc(sizeof(PODType), alignof(PODType))); }
	template <typename PODType>
	PODType *AllocPODArray(uint32 Count) { return reinterpret_cast<PODType *>(Alloc(sizeof(PODType) * Count, alignof(PODType))); }
	template <typename ObjectType, typename... TArgs>
	ObjectType *AllocObject(TArgs &&...Args) { return AllocObjectInternal<ObjectType>(std::forward<TArgs>(Args)...); }

	/** Allocates a zero-initialized parameter struct owned by the graph. */
	template <typename ParameterStructType>
	ParameterStructType *AllocParameters() {
		void *Memory = Alloc(sizeof(ParameterStructType), alignof(ParameterStructType));
		memset(Memory, 0, sizeof(ParameterStructType));
		ParameterStructType *Result = ::new (Memory) ParameterStructType();
		RegisterDestructorIfNeeded(Result);
		return Result;
	}
	template <typename ParameterStructType>
	ParameterStructType *AllocParameters(const ParameterStructType *StructToCopy) {
		ParameterStructType *Result = AllocParameters<ParameterStructType>();
		*Result = *StructToCopy;
		return Result;
	}

	/** Adds a pass whose resources are described by the parameter struct. */
	template <typename ParameterStructType, typename ExecuteLambdaType>
	FRDGPassRef AddPass(FRDGEventName &&Name, const ParameterStructType *ParameterStruct, ERDGPassFlags Flags, ExecuteLambdaType &&ExecuteLambda) {
		using LambdaPassType = TRDGLambdaPass<ParameterStructType, ExecuteLambdaType>;
		FRDGPass *Pass = AllocObjectInternal<LambdaPassType>(std::move(Name), ParameterStructType::FTypeInfo::GetStructMetadata(), ParameterStruct, OverridePassFlags(Flags), std::forward<ExecuteLambdaType>(ExecuteLambda));
		return AddPassInternal(Pass);
	}

	template <typename ExecuteLambdaType>
	FRDGPassRef AddPass(FRDGEventName &&Name, const FShaderParametersMetadata *ParametersMetadata, const void *ParameterStruct, ERDGPassFlags Flags, ExecuteLambdaType &&ExecuteLambda) {
		using LambdaPassType = TRDGLambdaPass<void, ExecuteLambdaType>;
		FRDGPass *Pass = AllocObjectInternal<LambdaPassType>(std::move(Name), ParametersMetadata, ParameterStruct, OverridePassFlags(Flags), std::forward<ExecuteLambdaType>(ExecuteLambda));
		return AddPassInternal(Pass);
	}

	/** Adds a pass without parameters (it can't access graph resources, and needs NeverCull or it is culled). */
	template <typename ExecuteLambdaType>
	FRDGPassRef AddPass(FRDGEventName &&Name, ERDGPassFlags Flags, ExecuteLambdaType &&ExecuteLambda) {
		using LambdaPassType = TRDGLambdaPass<void, ExecuteLambdaType>;
		FRDGPass *Pass = AllocObjectInternal<LambdaPassType>(std::move(Name), FEmptyShaderParameters::FTypeInfo::GetStructMetadata(), AllocParameters<FEmptyShaderParameters>(), OverridePassFlags(Flags), std::forward<ExecuteLambdaType>(ExecuteLambda));
		return AddPassInternal(Pass);
	}

	/** Adds an explicit dependency between two passes (culling, and async compute fork / join across pipes; ordering is
	 *  always submission order). */
	void AddPassDependency(FRDGPass *Producer, FRDGPass *Consumer);

	void QueueBufferUpload(FRDGBufferRef Buffer, const void *InitialData, uint64 InitialDataSize, ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None);
	template <typename ElementType>
	void QueueBufferUpload(FRDGBufferRef Buffer, const LocalVector<ElementType> &Container, ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None) {
		QueueBufferUpload(Buffer, Container.ptr(), (uint64)Container.size() * sizeof(ElementType), InitialDataFlags);
	}
	/** The data is not copied; the free callback is called once it was uploaded (or when the buffer was never used). */
	void QueueBufferUpload(FRDGBufferRef Buffer, const void *InitialData, uint64 InitialDataSize, FRDGBufferInitialDataFreeCallback &&InitialDataFreeCallback);
	/** The callback writes the whole buffer when the graph compiles. */
	void QueueBufferUpload(FRDGBufferRef Buffer, FRDGBufferInitialDataFillCallback &&InitialDataFillCallback);
	/** The data and its size come from the callbacks, called when the graph compiles. */
	void QueueBufferUpload(FRDGBufferRef Buffer, FRDGBufferInitialDataCallback &&InitialDataCallback, FRDGBufferInitialDataSizeCallback &&InitialDataSizeCallback);
	void QueueBufferUpload(FRDGBufferRef Buffer, FRDGBufferInitialDataCallback &&InitialDataCallback, FRDGBufferInitialDataSizeCallback &&InitialDataSizeCallback, FRDGBufferInitialDataFreeCallback &&InitialDataFreeCallback);

	void QueueTextureExtraction(FRDGTextureRef Texture, TRefCountPtr<IPooledRenderTarget> *OutPooledTexturePtr, ERDGResourceExtractionFlags Flags = ERDGResourceExtractionFlags::None);
	void QueueTextureExtraction(FRDGTextureRef Texture, TRefCountPtr<IPooledRenderTarget> *OutPooledTexturePtr, ERHIAccess AccessFinal, ERDGResourceExtractionFlags Flags = ERDGResourceExtractionFlags::None) { QueueTextureExtraction(Texture, OutPooledTexturePtr, Flags); }
	void QueueBufferExtraction(FRDGBufferRef Buffer, TRefCountPtr<FRDGPooledBuffer> *OutPooledBufferPtr);
	void QueueBufferExtraction(FRDGBufferRef Buffer, TRefCountPtr<FRDGPooledBuffer> *OutPooledBufferPtr, ERHIAccess AccessFinal) { QueueBufferExtraction(Buffer, OutPooledBufferPtr); }

	/** Allocates the resource immediately and keeps it alive for the whole graph; the returned pointer outlives the graph. */
	const TRefCountPtr<IPooledRenderTarget> &ConvertToExternalTexture(FRDGTextureRef Texture);
	const TRefCountPtr<FRDGPooledBuffer> &ConvertToExternalBuffer(FRDGBufferRef Buffer);
	const TRefCountPtr<IPooledRenderTarget> &GetPooledTexture(FRDGTextureRef Texture) const;
	const TRefCountPtr<FRDGPooledBuffer> &GetPooledBuffer(FRDGBufferRef Buffer) const;

	/** Barriers / final states are owned by Godot's RenderingDeviceGraph; kept for source compatibility. */
	void SetTextureAccessFinal(FRDGTextureRef Texture, ERHIAccess Access) {}
	void SetBufferAccessFinal(FRDGBufferRef Buffer, ERHIAccess Access) {}
	/** Configures the resource for external access for all subsequent passes, or until UseInternalAccessMode is called.
	 *  Only read-only access states are allowed, and only for registered, externally converted or extracted resources.
	 *  An access mode pass reads the resource with that access (keeping its producers); later passes using it are not
	 *  tracked (no dependency, no async compute fork / join), and may only use it with that access on those pipelines. */
	void UseExternalAccessMode(FRDGViewableResource *Resource, ERHIAccess ReadOnlyAccess, ERHIPipeline Pipelines = ERHIPipeline::Graphics);
	void UseExternalAccessMode(std::initializer_list<FRDGViewableResource *> Resources, ERHIAccess ReadOnlyAccess, ERHIPipeline Pipelines = ERHIPipeline::Graphics) {
		for (FRDGViewableResource *Resource : Resources) UseExternalAccessMode(Resource, ReadOnlyAccess, Pipelines);
	}
	/** Resumes tracking of a resource after UseExternalAccessMode (no-op when the resource is not in external access mode). */
	void UseInternalAccessMode(FRDGViewableResource *Resource);
	void UseInternalAccessMode(std::initializer_list<FRDGViewableResource *> Resources) {
		for (FRDGViewableResource *Resource : Resources) UseInternalAccessMode(Resource);
	}
	/** Previous graphs always join their async compute work at their epilogue, there is no initial fence to skip. */
	void SkipInitialAsyncComputeFence() {}
	void AddDispatchHint() {}
	void SetFlushResourcesRHI() {}
	/** Whether AsyncCompute passes of this graph run on the async compute queue (r.RDG.AsyncCompute and device support). */
	bool IsAsyncComputeEnabled() const { return bSupportsAsyncCompute; }
	/** Launches a task that is synced prior to graph compilation or execution (WaitPoint). Without parallel setup
	 *  (ERDGBuilderFlags::ParallelSetup and r.RDG.ParallelSetup) the lambda runs immediately. Setup tasks must not call
	 *  into the builder: they prepare the data that buffer callbacks and pass lambdas consume. */
	template <typename TaskLambdaType>
	UE::Tasks::FTask AddSetupTask(TaskLambdaType &&TaskLambda, bool bCondition = true, ERDGSetupTaskWaitPoint WaitPoint = ERDGSetupTaskWaitPoint::Compile) {
		return AddSetupTask(std::forward<TaskLambdaType>(TaskLambda), LocalVector<UE::Tasks::FTask>(), UE::Tasks::ETaskPriority::Normal, bCondition, WaitPoint);
	}
	template <typename TaskLambdaType>
	UE::Tasks::FTask AddSetupTask(TaskLambdaType &&TaskLambda, UE::Tasks::ETaskPriority Priority, bool bCondition = true, ERDGSetupTaskWaitPoint WaitPoint = ERDGSetupTaskWaitPoint::Compile) {
		return AddSetupTask(std::forward<TaskLambdaType>(TaskLambda), LocalVector<UE::Tasks::FTask>(), Priority, bCondition, WaitPoint);
	}
	/** Prerequisites: a task or a collection of tasks. */
	template <typename TaskLambdaType, typename PrerequisitesCollectionType, std::enable_if_t<UE::Tasks::Private::TIsPrerequisites<PrerequisitesCollectionType>, int> = 0>
	UE::Tasks::FTask AddSetupTask(TaskLambdaType &&TaskLambda, PrerequisitesCollectionType &&Prerequisites, UE::Tasks::ETaskPriority Priority = UE::Tasks::ETaskPriority::Normal, bool bCondition = true, ERDGSetupTaskWaitPoint WaitPoint = ERDGSetupTaskWaitPoint::Compile) {
		LocalVector<UE::Tasks::FTask> PrerequisiteTasks;
		UE::Tasks::Private::AppendPrerequisites(PrerequisiteTasks, Prerequisites);
		const bool bParallelEnabled = bCompiling ? bParallelCompileEnabled : ParallelSetup.bEnabled;
		if (!bCondition || !bParallelEnabled) {
			UE::Tasks::Wait(PrerequisiteTasks);
			TaskLambda();
			return UE::Tasks::FTask();
		}
		UE::Tasks::FTask Task = UE::Tasks::Launch("FRDGBuilder::AddSetupTask", std::forward<TaskLambdaType>(TaskLambda), std::move(PrerequisiteTasks), Priority);
		ParallelSetup.Tasks[(int32)WaitPoint].push_back(Task);
		Stats.NumSetupTasks++;
		return Task;
	}

	/** Lets the async setup queue task set up the passes added so far (r.RDG.AsyncSetupQueue). Without a call, the queued
	 *  passes are set up by Execute(). */
	void FlushSetupQueue();

	/** Whether RDG launches tasks when AddSetupTask is called. */
	bool IsParallelSetupEnabled() const { return ParallelSetup.bEnabled; }
	/** Whether pass lambdas are recorded by parallel tasks (ERDGBuilderFlags::ParallelExecute and r.RDG.ParallelExecute). */
	bool IsParallelExecuteEnabled() const { return bParallelExecute; }

	void AddPostExecuteCallback(std::function<void()> &&Callback) { PostExecuteCallbacks.push_back(std::move(Callback)); }

	/** Compiles (culling, lifetimes, render pass merging) and executes the graph. */
	void Execute();

	/** Advances the pools of the main device (UE's FRDGBuilder::TickPoolElements). */
	static void TickPoolElements();
	static void TickPoolElements(RenderingDevice *Device);
	static bool IsImmediateMode();

	FRDGBlackboard Blackboard;
	FRHICommandListImmediate &RHICmdList;

	/** Godot additions. */
	RenderingDevice *GetDevice() const { return Device; }
	class FRDGDeviceContext *GetDeviceContext() const { return DeviceContext; }
	FGlobalShaderMap *GetShaderMap() const;
	const FRDGBuilderStats &GetStats() const { return Stats; }
	const char *GetName() const { return BuilderName.GetTCHAR(); }
	/** Passes in submission order (for inspection / tests). */
	const LocalVector<FRDGPass *> &GetPasses() const { return Passes; }
	const LocalVector<FRDGTexture *> &GetTextures() const { return Textures; }
	const LocalVector<FRDGBuffer *> &GetBuffers() const { return Buffers; }
	/** Physical RD texture each transient texture was assigned (valid after Execute, for tests and the graph dump). */
	RID GetAllocatedTextureRID(FRDGTextureRef Texture) const;
	RID GetAllocatedBufferRID(FRDGBufferRef Buffer) const;
	/** Transient heap placement of a texture / buffer (valid after Execute, null heap when it came from a pool). */
	FRHITransientHeapAllocation GetTransientAllocation(FRDGViewableResource *Resource) const;
	/** Path of the last graph dump written with r.RDG.DumpGraph. */
	static String GetLastDumpPath();

private:
	friend class FRDGEventScopeGuard;
	friend class FRDGGPUStatScopeGuard;
	friend class FRDGUserValidation;
	friend class FRDGGraphDumper;

	template <typename ObjectType, typename... TArgs>
	ObjectType *AllocObjectInternal(TArgs &&...Args) {
		void *Memory = Alloc(sizeof(ObjectType), alignof(ObjectType));
		ObjectType *Object = ::new (Memory) ObjectType(std::forward<TArgs>(Args)...);
		RegisterDestructorIfNeeded(Object);
		return Object;
	}
	template <typename ObjectType>
	void RegisterDestructorIfNeeded(ObjectType *Object) {
		if constexpr (!std::is_trivially_destructible_v<ObjectType>) Destructors.push_back({ Object, [](void *Ptr) { reinterpret_cast<ObjectType *>(Ptr)->~ObjectType(); } });
	}

	const char *CopyName(const char *Name);
	ERDGPassFlags OverridePassFlags(ERDGPassFlags Flags) const;
	FRDGPassRef AddPassInternal(FRDGPass *Pass);
	void RegisterUniformBuffer(FRDGUniformBuffer *UniformBuffer);

	void SetupPassResources(FRDGPass *Pass);
	/** Marks the resources written by the pass as produced, on the setup timeline of the user (async setup queue). */
	void MarkResourcesAsProduced(FRDGPass *Pass);
	void LaunchAsyncSetupQueueTask();
	void ProcessAsyncSetupQueue(bool bOnTask);
	/** Waits for the async setup queue task and sets up the passes it left. */
	void WaitForAsyncSetupQueue();
	void WaitForParallelSetupTasks(ERDGSetupTaskWaitPoint WaitPoint);
	/** Resolves the buffer NumElements and upload callbacks (UE's FinalizeDescs / SubmitBufferUploads preparation). */
	void FinalizeDescs();
	void PrepareBufferUploads();
	struct FUploadedBuffer;
	void PrepareBufferUpload(FUploadedBuffer &UploadedBuffer);
	FUploadedBuffer *QueueBufferUploadInternal(FRDGBufferRef Buffer);
	void SubmitBufferUpload(FRDGBuffer *Buffer);
	/** Adds the access mode passes of the queued UseExternalAccessMode / UseInternalAccessMode calls (before the next pass). */
	void FlushAccessModeQueue();
	void Compile();
	void AddCullingDependency(FRDGPass *Producer, FRDGPass *Consumer);
	void CompileAsyncCompute();
	void CompileLifetimes();
	void MergeRenderPasses();
	void ExecutePass(FRDGPass *Pass);
	/** Closes the active GPU stat and opens Stat with RenderingDevice timestamps (r.RDG.GPUStats). */
	void SetActiveGPUStat(const FRDGGPUStat *Stat);
	void ExecutePassPrologue(FRDGPass *Pass);
	void ExecutePassEpilogue(FRDGPass *Pass);
	void BeginRenderPass(FRDGPass *Pass, FRHICommandList &CommandList);

	// Parallel execution (UE's SetupParallelExecute / FParallelPassSet). A group is one pass, or every pass of a merged
	// render pass (an RD draw list can't span recording contexts), and records into one RenderingDevice recording
	// context. A set is a run of groups recorded in order by one task.
	struct FParallelPassGroup {
		uint32 FirstPassIndex = 0;
		uint32 LastPassIndex = 0;
		RenderingDevice::RecordingContext *Context = nullptr;
	};
	struct FParallelPassSet {
		FRDGBuilder *Builder = nullptr;
		uint32 FirstGroupIndex = 0;
		uint32 LastGroupIndex = 0;
		uint32 NumBindingErrors = 0;
	};
	void SetupParallelExecute();
	void ExecuteParallelPassSets(uint32 FirstSetIndex, uint32 LastSetIndex);
	void ExecutePassInTask(FRDGPass *Pass, FRHICommandList &CommandList);
	static void ParallelPassSetTask(void *Data);
	void SetPassRHIAccess(FRDGPass *Pass, bool bAllow);

	void AllocateTexture(FRDGTexture *Texture);
	void AllocateBuffer(FRDGBuffer *Buffer);
	void ReleaseTexture(FRDGTexture *Texture);
	void ReleaseBuffer(FRDGBuffer *Buffer);
	bool IsTransient(const FRDGTexture *Texture) const;
	bool IsTransient(const FRDGBuffer *Buffer) const;
	void OnTransientAllocated(FRDGViewableResource *Resource, const FRHITransientHeapAllocation &Allocation);
	void AcquireResource(FRDGViewableResource *Resource);
	void ReleaseResource(FRDGViewableResource *Resource, FRDGPass *Pass);
	FRHITransientAllocationFences GetAllocateFences(const FRDGViewableResource *Resource) const;
	FRHITransientAllocationFences GetDeallocateFences(const FRDGViewableResource *Resource) const;
	FRHITransientAllocationFences::FInterval GetGraphicsForkJoinInterval(const FRDGPass *Pass) const;
	void ClobberTexture(FRDGTexture *Texture);
	void FinalizeExtraction();
	void ReleaseFrameUniformBuffers();
	String BuildPassLabel(const FRDGPass *Pass) const;

	RenderingDevice *Device = nullptr;
	class FRDGDeviceContext *DeviceContext = nullptr;
	FRDGEventName BuilderName;
	bool bExecuted = false;
	bool bImmediateMode = false;
	bool bInPassExecution = false;
	bool bParallelExecute = false;
	bool bEmitPassEvents = true;
	LocalVector<FParallelPassGroup> ParallelPassGroups;
	LocalVector<FParallelPassSet> ParallelPassSets;
	uint32 ParallelBindingErrors = 0;
	bool bSupportsAsyncCompute = false;

	/** UE's parallel setup: setup tasks per wait point. */
	struct FParallelSetup {
		LocalVector<UE::Tasks::FTask> Tasks[(int32)ERDGSetupTaskWaitPoint::MAX];
		bool bEnabled = false;
	} ParallelSetup;
	bool bParallelCompileEnabled = false;
	/** Set while Execute() compiles the graph: AddSetupTask then follows r.RDG.ParallelCompile. */
	bool bCompiling = false;

	/** UE's async setup queue: passes whose resources are set up by a task (SetupPassResources). Only one task processes
	 *  the queue at a time; Execute() processes what is left. */
	struct FAsyncSetupQueue {
		Mutex Lock;
		LocalVector<FRDGPass *> Passes;
		UE::Tasks::FTask LastTask;
		uint32 NumPassesOnTask = 0;
		bool bEnabled = false;
	} AsyncSetupQueue;

	/** UE's FUploadedBuffer. */
	struct FUploadedBuffer {
		FRDGBuffer *Buffer = nullptr;
		const void *Data = nullptr;
		uint64 DataSize = 0;
		FRDGBufferInitialDataCallback DataCallback;
		FRDGBufferInitialDataSizeCallback DataSizeCallback;
		FRDGBufferInitialDataFillCallback DataFillCallback;
		FRDGBufferInitialDataFreeCallback DataFreeCallback;
		/** Written by the fill callback. */
		LocalVector<uint8> FillData;
		/** Data / DataSize are final (callbacks resolved). */
		bool bPrepared = false;
		bool bUploaded = false;
	};
	/** Allocated by the graph: stable addresses (LocalVector relocates with memrealloc, std::function can't be). */
	LocalVector<FUploadedBuffer *> UploadedBuffers;
	LocalVector<FRDGBuffer *> NumElementsCallbackBuffers;
	bool bHasUploadCallbacks = false;

	/** Async compute (UE's fork / join fences between the pipes). The prologue fork is signaled before the first pass, the
	 *  epilogue join is waited after the last one. Fork signals of passes inside a merged render pass wait for its end. */
	uint32 AsyncComputePassCount = 0;
	bool bPrologueFork = false;
	bool bEpilogueJoin = false;
	RenderingDevice::AsyncComputeFence PrologueForkFence;
	RenderingDevice::AsyncComputeFence EpilogueJoinFence;
	LocalVector<FRDGPass *> PendingForkPasses;

	struct FMemoryBlock {
		uint8 *Data = nullptr;
		uint64 Size = 0;
		uint64 Used = 0;
	};
	LocalVector<FMemoryBlock> MemoryBlocks;
	struct FDestructor {
		void *Object = nullptr;
		void (*Function)(void *) = nullptr;
	};
	LocalVector<FDestructor> Destructors;

	LocalVector<FRDGPass *> Passes;
	LocalVector<FRDGTexture *> Textures;
	LocalVector<FRDGBuffer *> Buffers;
	LocalVector<FRDGView *> Views;
	LocalVector<FRDGUniformBuffer *> UniformBuffers;
	HashMap<FRHITexture *, FRDGTexture *> ExternalTextures;
	HashMap<FRHIBuffer *, FRDGBuffer *> ExternalBuffers;

	struct FExtractedTexture {
		FRDGTexture *Texture = nullptr;
		TRefCountPtr<IPooledRenderTarget> *PooledTexture = nullptr;
	};
	struct FExtractedBuffer {
		FRDGBuffer *Buffer = nullptr;
		TRefCountPtr<FRDGPooledBuffer> *PooledBuffer = nullptr;
	};
	LocalVector<FExtractedTexture> ExtractedTextures;
	LocalVector<FExtractedBuffer> ExtractedBuffers;
	/** A List: LocalVector relocates with memrealloc, which std::function doesn't survive. */
	List<std::function<void()>> PostExecuteCallbacks;
	LocalVector<RID> FrameUniformBuffersInUse;

	struct FExplicitDependency {
		FRDGPass *Producer = nullptr;
		FRDGPass *Consumer = nullptr;
	};
	LocalVector<FExplicitDependency> ExplicitDependencies;
	LocalVector<FRDGViewableResource *> AccessModeQueue;

	const FRDGEventScope *CurrentScope = nullptr;
	const FRDGGPUStat *CurrentGPUStat = nullptr;
	/** GPU stat whose begin timestamp was captured last during Execute() (closed when the stat changes). */
	const FRDGGPUStat *ActiveGPUStat = nullptr;

	/** Physical allocations seen during execution, for stats. */
	HashMap<RID, uint32> PhysicalTextureUses;
	HashMap<RID, uint32> PhysicalBufferUses;
	HashMap<FRDGTexture *, RID> AllocatedTextureRIDs;
	HashMap<FRDGBuffer *, RID> AllocatedBufferRIDs;
	HashMap<FRDGViewableResource *, FRHITransientHeapAllocation> TransientAllocations;
	uint64 CurrentTransientBytes = 0;

	FRDGBuilderStats Stats;
	uint32 ValidationErrorsAtStart = 0;
};

inline FRDGEventScopeGuard::FRDGEventScopeGuard(FRDGBuilder &InGraphBuilder, FRDGEventName &&ScopeName, bool bCondition) :
		GraphBuilder(InGraphBuilder) {
	if (!bCondition) return;
	GraphBuilder.CurrentScope = GraphBuilder.AllocObject<FRDGEventScope>(GraphBuilder.CurrentScope, std::move(ScopeName));
	bPushed = true;
}

inline FRDGEventScopeGuard::~FRDGEventScopeGuard() {
	if (bPushed) GraphBuilder.CurrentScope = GraphBuilder.CurrentScope->ParentScope;
}

inline FRDGGPUStatScopeGuard::FRDGGPUStatScopeGuard(FRDGBuilder &InGraphBuilder, const FRDGGPUStat &Stat) :
		GraphBuilder(InGraphBuilder), ParentStat(InGraphBuilder.CurrentGPUStat) {
	GraphBuilder.CurrentGPUStat = &Stat;
}

inline FRDGGPUStatScopeGuard::~FRDGGPUStatScopeGuard() {
	GraphBuilder.CurrentGPUStat = ParentStat;
}

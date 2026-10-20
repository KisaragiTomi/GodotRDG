#pragma once

#include "RHICommandList.h"
#include "RenderGraphEvent.h"
#include "RenderGraphResources.h"

class FRDGPass {
public:
	FRDGPass(FRDGEventName &&InName, const FShaderParametersMetadata *InParameterMetadata, const void *InParameterStruct, ERDGPassFlags InFlags);
	FRDGPass(const FRDGPass &) = delete;
	virtual ~FRDGPass() = default;

	const char *GetName() const { return Name.GetTCHAR(); }
	const FRDGEventName &GetEventName() const { return Name; }
	ERDGPassFlags GetFlags() const { return Flags; }
	/** AsyncCompute passes run on the async compute pipe when the graph enables async compute (see FRDGBuilder::IsAsyncComputeEnabled). */
	ERHIPipeline GetPipeline() const { return Pipeline; }
	FRDGPassHandle GetHandle() const { return Handle; }
	bool IsCulled() const { return bCulled; }
	bool IsAsyncCompute() const { return Pipeline == ERHIPipeline::AsyncCompute; }
	/** (AsyncCompute only) Whether this is the first / last async compute pass of an async interval (fork / join). */
	bool IsAsyncComputeBegin() const { return bAsyncComputeBegin; }
	bool IsAsyncComputeEnd() const { return bAsyncComputeEnd; }
	/** (Graphics only) Whether async compute work forks from the end of this pass / joins at its beginning. */
	bool IsGraphicsFork() const { return bGraphicsFork; }
	bool IsGraphicsJoin() const { return bGraphicsJoin; }
	/** (AsyncCompute only) Graphics passes the async interval of this pass forks from / joins into. Null means the graph
	 *  prologue (fork before the first pass) / epilogue (join after the last pass). */
	FRDGPassHandle GetGraphicsForkPass() const { return GraphicsForkPass; }
	FRDGPassHandle GetGraphicsJoinPass() const { return GraphicsJoinPass; }
	/** Latest producer on the other pipeline and the consumers on the other pipeline (sorted). */
	FRDGPassHandle GetCrossPipelineProducer() const { return CrossPipelineProducer; }
	const LocalVector<FRDGPassHandle> &GetCrossPipelineConsumers() const { return CrossPipelineConsumers; }
	bool IsMergedRenderPassBegin() const { return !bSkipRenderPassBegin && bSkipRenderPassEnd; }
	bool IsMergedRenderPassEnd() const { return bSkipRenderPassBegin && !bSkipRenderPassEnd; }
	bool SkipRenderPassBegin() const { return bSkipRenderPassBegin; }
	bool SkipRenderPassEnd() const { return bSkipRenderPassEnd; }
	const FShaderParametersMetadata *GetParametersMetadata() const { return ParameterMetadata; }
	const void *GetParameters() const { return ParameterStruct; }
	const FRDGEventScope *GetScope() const { return Scope; }
	const LocalVector<FRDGPass *> &GetProducers() const { return Producers; }
	/** UE's pass task mode: whether the pass was recorded by a parallel task (r.RDG.ParallelExecute) and in which set. */
	bool IsParallelExecute() const { return ParallelPassSetIndex >= 0; }
	int32 GetParallelPassSetIndex() const { return ParallelPassSetIndex; }

	/** Resources referenced by this pass (deduplicated), filled during setup. */
	/** bExternalAccess: the resource was in external access mode when the pass was set up. UE leaves such resources out
	 *  of the pass states; here they stay (so the pass may access their RHI) but add no dependency and no lifetime. */
	struct FTextureState {
		FRDGTexture *Texture = nullptr;
		ERHIAccess Access = ERHIAccess::Unknown;
		uint32 ReferenceCount = 0;
		bool bExternalAccess = false;
	};
	struct FBufferState {
		FRDGBuffer *Buffer = nullptr;
		ERHIAccess Access = ERHIAccess::Unknown;
		uint32 ReferenceCount = 0;
		bool bExternalAccess = false;
	};
	/** Access mode pass (FRDGBuilder::FlushAccessModeQueue) that switches resources into / out of external access mode. */
	bool IsExternalAccessPass() const { return bExternalAccessPass; }
	const LocalVector<FTextureState> &GetTextureStates() const { return TextureStates; }
	const LocalVector<FBufferState> &GetBufferStates() const { return BufferStates; }

protected:
	virtual void Execute(FRHIComputeCommandList &RHICmdList) = 0;
	/** Lambdas taking FRHICommandListImmediate& must run on the render thread (UE's ERDGPassTaskMode::Inline). */
	virtual bool RequiresImmediateCommandList() const { return false; }
	/** Lambdas that capture state need destruction; the builder calls this before freeing its allocator. */

	const FRDGEventName Name;
	const FShaderParametersMetadata *const ParameterMetadata;
	const void *const ParameterStruct;
	const ERDGPassFlags Flags;
	ERHIPipeline Pipeline = ERHIPipeline::Graphics;
	FRDGPassHandle Handle;
	const FRDGEventScope *Scope = nullptr;

	LocalVector<FTextureState> TextureStates;
	LocalVector<FBufferState> BufferStates;
	LocalVector<FRDGUniformBuffer *> UniformBuffers;
	LocalVector<FRDGPass *> Producers;
	FRDGPassHandle CrossPipelineProducer;
	LocalVector<FRDGPassHandle> CrossPipelineConsumers;
	FRDGPassHandle GraphicsForkPass;
	FRDGPassHandle GraphicsJoinPass;

	/** RenderingDevice fences of the async compute fork / join, signaled while the graph executes: the fork fence by a
	 *  graphics fork pass, the join fence by the async compute pass that ends an interval (stored on its join pass). */
	RenderingDevice::AsyncComputeFence ForkFence;
	RenderingDevice::AsyncComputeFence JoinFence;

	/** Resource lifetime events computed in Compile(). */
	LocalVector<FRDGViewableResource *> ResourcesToAcquire;
	LocalVector<FRDGViewableResource *> ResourcesToRelease;

	bool bCulled = false;
	bool bSkipRenderPassBegin = false;
	bool bSkipRenderPassEnd = false;
	bool bEmptyParameters = false;
	bool bRenderPassOnlyWrites = true;
	bool bHasExternalOutputs = false;
	bool bExecuted = false;
	bool bSetupDone = false;
	bool bAsyncComputeBegin = false;
	bool bAsyncComputeEnd = false;
	bool bGraphicsFork = false;
	bool bGraphicsJoin = false;

	/** Parallel pass set recording this pass, -1 when the pass runs on the render thread. */
	int32 ParallelPassSetIndex = -1;

	/** Access mode switches replayed once this access mode pass is set up (UE's FExternalAccessOp, used when the pass
	 *  setup runs asynchronously, see r.RDG.AsyncSetupQueue). */
	struct FExternalAccessOp {
		FRDGViewableResource *Resource = nullptr;
		FRDGViewableResource::EAccessMode Mode = FRDGViewableResource::EAccessMode::Internal;
		ERHIAccess Access = ERHIAccess::None;
		ERHIPipeline Pipelines = ERHIPipeline::None;
	};
	LocalVector<FExternalAccessOp> ExternalAccessOps;
	bool bExternalAccessPass = false;

	friend class FRDGBuilder;
	friend class FRDGUserValidation;
	friend class FRDGGraphDumper;
};

/** Pass executing a user lambda. Accepts both `(FRHICommandList&)` and UE 5.5 style `(FRDGAsyncTask, FRHICommandList&)` lambdas. */
template <typename ParameterStructType, typename ExecuteLambdaType>
class TRDGLambdaPass final : public FRDGPass {
public:
	TRDGLambdaPass(FRDGEventName &&InName, const FShaderParametersMetadata *InParameterMetadata, const ParameterStructType *InParameterStruct, ERDGPassFlags InFlags, ExecuteLambdaType &&InExecuteLambda) :
			FRDGPass(std::move(InName), InParameterMetadata, InParameterStruct, InFlags), ExecuteLambda(std::forward<ExecuteLambdaType>(InExecuteLambda)) {}

private:
	void Execute(FRHIComputeCommandList &RHICmdList) override {
		FRHICommandList &CommandList = static_cast<FRHICommandList &>(RHICmdList);
		if constexpr (std::is_invocable_v<ExecuteLambdaType &, FRDGAsyncTask, FRHICommandList &>) ExecuteLambda(FRDGAsyncTask(), CommandList);
		else if constexpr (std::is_invocable_v<ExecuteLambdaType &, FRHICommandList &>) ExecuteLambda(CommandList);
		else if constexpr (std::is_invocable_v<ExecuteLambdaType &, FRHICommandListImmediate &>) ExecuteLambda(static_cast<FRHICommandListImmediate &>(RHICmdList));
		else static_assert(std::is_invocable_v<ExecuteLambdaType &, FRHICommandList &>, "Pass lambda must take (FRHIComputeCommandList&), (FRHICommandList&) or (FRDGAsyncTask, FRHICommandList&).");
	}

	bool RequiresImmediateCommandList() const override {
		return !std::is_invocable_v<ExecuteLambdaType &, FRDGAsyncTask, FRHICommandList &> && !std::is_invocable_v<ExecuteLambdaType &, FRHICommandList &>;
	}

	std::decay_t<ExecuteLambdaType> ExecuteLambda;
};

/** Pass without lambda (pure dependency / resource holder), like UE's FRDGEmptyPass. */
class FRDGEmptyPass final : public FRDGPass {
public:
	FRDGEmptyPass(FRDGEventName &&InName, const FShaderParametersMetadata *InParameterMetadata, const void *InParameterStruct, ERDGPassFlags InFlags) :
			FRDGPass(std::move(InName), InParameterMetadata, InParameterStruct, InFlags) {}

private:
	void Execute(FRHIComputeCommandList &) override {}
};

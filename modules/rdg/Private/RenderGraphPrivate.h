#pragma once

#include <functional>

#include "HAL/IConsoleManager.h"
#include "RHICommandList.h"
#include "RenderGraphResources.h"

class FRDGBuilder;
class FRDGPass;

extern TAutoConsoleVariable<int32> CVarRDGImmediateMode;
extern TAutoConsoleVariable<int32> CVarRDGDebug;
extern TAutoConsoleVariable<int32> CVarRDGValidation;
extern TAutoConsoleVariable<int32> CVarRDGCullPasses;
extern TAutoConsoleVariable<int32> CVarRDGMergeRenderPasses;
extern TAutoConsoleVariable<int32> CVarRDGTransientAllocator;
extern TAutoConsoleVariable<int32> CVarRDGClobberResources;
extern TAutoConsoleVariable<int32> CVarRDGDumpGraph;
extern TAutoConsoleVariable<int32> CVarRDGEvents;

namespace RDGPrivate {

inline bool IsValidationEnabled() { return CVarRDGValidation.GetValueOnRenderThread() != 0; }

/** Number of validation errors reported since startup (tests use the delta). */
uint32 GetValidationErrorCount();
void ReportValidationError(const String &Message);
void ReportValidationWarning(const String &Message);

/** Runs an RD command that can't be recorded into a parallel recording context (copies, clears, updates, readbacks) on
 *  the render thread: right away, or at the point of the recording context bound to the calling thread when it is
 *  submitted, so it keeps its place relative to the lists recorded around it. */
void EnqueueRenderThreadCommand(RenderingDevice *Device, std::function<void(RenderingDevice *)> &&Command);

/** Fills a whole buffer with a 32-bit value (buffer_clear for 0, compute shader otherwise). */
void ClearBufferUAV(FRHIComputeCommandList &RHICmdList, FRHIBuffer *Buffer, uint32 Value);

/** Visits every graph resource referenced by a parameter struct (recursing into uniform buffers). */
struct FGraphResourceVisitor {
	virtual ~FGraphResourceVisitor() = default;
	virtual void OnTexture(FRDGTexture *Texture, ERHIAccess Access, bool bRenderTarget, FRDGTextureSubresourceRange Range, const char *MemberName) {}
	virtual void OnBuffer(FRDGBuffer *Buffer, ERHIAccess Access, const char *MemberName) {}
	virtual void OnUniformBuffer(FRDGUniformBuffer *UniformBuffer) {}
	virtual void OnTextureView(FRDGView *View) {}
	virtual void OnBufferView(FRDGView *View) {}
};
void EnumerateGraphResources(const FShaderParametersMetadata *Metadata, const void *Parameters, ERDGPassFlags PassFlags, FGraphResourceVisitor &Visitor);

/** Graphviz dump of a compiled graph (r.RDG.DumpGraph). Returns the written path. */
String DumpGraph(const FRDGBuilder &GraphBuilder);

} //namespace RDGPrivate

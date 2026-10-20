#include "RenderGraphPass.h"

FRDGPass::FRDGPass(FRDGEventName &&InName, const FShaderParametersMetadata *InParameterMetadata, const void *InParameterStruct, ERDGPassFlags InFlags) :
		Name(std::move(InName)), ParameterMetadata(InParameterMetadata), ParameterStruct(InParameterStruct), Flags(InFlags) {}

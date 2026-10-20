#pragma once

// UE's SetShaderParameters / ClearUnusedGraphResources helpers.

#include "GlobalShader.h"
#include "RHICommandList.h"

template <typename TShaderClass>
inline void SetShaderParameters(FRHIComputeCommandList &RHICmdList, const TShaderRef<TShaderClass> &Shader, FRHIShader *ShaderRHI, const FShaderParametersMetadata *ParametersMetadata, const typename TShaderClass::FParameters &Parameters) {
	RHICmdList.SetShaderParameters(Shader.GetShader(), &Parameters);
}

template <typename TShaderClass>
inline void SetShaderParameters(FRHIComputeCommandList &RHICmdList, const TShaderRef<TShaderClass> &Shader, FRHIShader *ShaderRHI, const typename TShaderClass::FParameters &Parameters) {
	RHICmdList.SetShaderParameters(Shader.GetShader(), &Parameters);
}

template <typename TShaderClass>
inline void SetShaderParameters(FRHIComputeCommandList &RHICmdList, const TShaderRef<TShaderClass> &Shader, const typename TShaderClass::FParameters &Parameters) {
	RHICmdList.SetShaderParameters(Shader.GetShader(), &Parameters);
}

/** UAVs are bound per dispatch in Godot; kept for source compatibility. */
template <typename TShaderClass>
inline void UnsetShaderUAVs(FRHIComputeCommandList &RHICmdList, const TShaderRef<TShaderClass> &Shader, FRHIShader *ShaderRHI) {}

namespace RDGPrivate {
/** Nulls every RDG resource of the struct that the shader(s) do not bind, so RDG does not track false dependencies. */
void ClearUnusedGraphResourcesImpl(const FShaderParameterBindings *const *Bindings, uint32 NumBindings, const FShaderParametersMetadata *Metadata, void *Parameters, const void *const *ExcludeList, uint32 NumExclude);
} //namespace RDGPrivate

template <typename TShaderClass>
inline void ClearUnusedGraphResources(const TShaderRef<TShaderClass> &Shader, const FShaderParametersMetadata *ParametersMetadata, typename TShaderClass::FParameters *InoutParameters, std::initializer_list<const void *> ExcludeList = {}) {
	if (!Shader.IsValid()) return;
	const FShaderParameterBindings *Bindings[] = { &Shader->GetParameterBindings() };
	RDGPrivate::ClearUnusedGraphResourcesImpl(Bindings, 1, ParametersMetadata, InoutParameters, ExcludeList.begin(), (uint32)ExcludeList.size());
}

template <typename TShaderClass>
inline void ClearUnusedGraphResources(const TShaderRef<TShaderClass> &Shader, typename TShaderClass::FParameters *InoutParameters, std::initializer_list<const void *> ExcludeList = {}) {
	ClearUnusedGraphResources(Shader, TShaderClass::FParameters::FTypeInfo::GetStructMetadata(), InoutParameters, ExcludeList);
}

template <typename TShaderClassA, typename TShaderClassB, typename TPassParameterStruct>
inline void ClearUnusedGraphResources(const TShaderRef<TShaderClassA> &ShaderA, const TShaderRef<TShaderClassB> &ShaderB, TPassParameterStruct *InoutPassParameters, std::initializer_list<const void *> ExcludeList = {}) {
	if (!ShaderA.IsValid() || !ShaderB.IsValid()) return;
	const FShaderParameterBindings *Bindings[] = { &ShaderA->GetParameterBindings(), &ShaderB->GetParameterBindings() };
	RDGPrivate::ClearUnusedGraphResourcesImpl(Bindings, 2, TPassParameterStruct::FTypeInfo::GetStructMetadata(), InoutPassParameters, ExcludeList.begin(), (uint32)ExcludeList.size());
}

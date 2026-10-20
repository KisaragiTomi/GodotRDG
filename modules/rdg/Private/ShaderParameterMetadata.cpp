#include "ShaderParameterMacros.h"

#include "RenderGraphResources.h"

const char *GetShaderParameterMacroName(EUniformBufferBaseType ShaderParameterBaseType) {
	switch (ShaderParameterBaseType) {
		case UBMT_BOOL:
		case UBMT_INT32:
		case UBMT_UINT32:
		case UBMT_FLOAT32:
			return "SHADER_PARAMETER";
		case UBMT_TEXTURE:
			return "SHADER_PARAMETER_TEXTURE";
		case UBMT_SRV:
			return "SHADER_PARAMETER_SRV";
		case UBMT_UAV:
			return "SHADER_PARAMETER_UAV";
		case UBMT_SAMPLER:
			return "SHADER_PARAMETER_SAMPLER";
		case UBMT_RDG_TEXTURE:
			return "SHADER_PARAMETER_RDG_TEXTURE";
		case UBMT_RDG_TEXTURE_ACCESS:
			return "RDG_TEXTURE_ACCESS";
		case UBMT_RDG_TEXTURE_ACCESS_ARRAY:
			return "RDG_TEXTURE_ACCESS_ARRAY";
		case UBMT_RDG_TEXTURE_SRV:
			return "SHADER_PARAMETER_RDG_TEXTURE_SRV";
		case UBMT_RDG_TEXTURE_NON_PIXEL_SRV:
			return "SHADER_PARAMETER_RDG_TEXTURE_NON_PIXEL_SRV";
		case UBMT_RDG_TEXTURE_UAV:
			return "SHADER_PARAMETER_RDG_TEXTURE_UAV";
		case UBMT_RDG_BUFFER_ACCESS:
			return "RDG_BUFFER_ACCESS";
		case UBMT_RDG_BUFFER_ACCESS_ARRAY:
			return "RDG_BUFFER_ACCESS_ARRAY";
		case UBMT_RDG_BUFFER_SRV:
			return "SHADER_PARAMETER_RDG_BUFFER_SRV";
		case UBMT_RDG_BUFFER_UAV:
			return "SHADER_PARAMETER_RDG_BUFFER_UAV";
		case UBMT_RDG_UNIFORM_BUFFER:
			return "SHADER_PARAMETER_RDG_UNIFORM_BUFFER";
		case UBMT_NESTED_STRUCT:
			return "SHADER_PARAMETER_STRUCT";
		case UBMT_INCLUDED_STRUCT:
			return "SHADER_PARAMETER_STRUCT_INCLUDE";
		case UBMT_REFERENCED_STRUCT:
			return "SHADER_PARAMETER_STRUCT_REF";
		case UBMT_RENDER_TARGET_BINDING_SLOTS:
			return "RENDER_TARGET_BINDING_SLOTS";
		default:
			return "UBMT_INVALID";
	}
}

uint32 FShaderParametersMetadata::FMember::GetElementSize() const {
	switch (BaseType) {
		case UBMT_BOOL:
			return sizeof(bool) * NumRows * NumColumns;
		case UBMT_INT32:
		case UBMT_UINT32:
		case UBMT_FLOAT32:
			return 4 * NumRows * NumColumns;
		case UBMT_NESTED_STRUCT:
		case UBMT_INCLUDED_STRUCT:
			return Struct ? Struct->GetSize() : 0;
		case UBMT_RDG_TEXTURE_ACCESS:
			return sizeof(FRDGTextureAccess);
		case UBMT_RDG_BUFFER_ACCESS:
			return sizeof(FRDGBufferAccess);
		case UBMT_RDG_TEXTURE_ACCESS_ARRAY:
			return sizeof(FRDGTextureAccessArray);
		case UBMT_RDG_BUFFER_ACCESS_ARRAY:
			return sizeof(FRDGBufferAccessArray);
		case UBMT_RDG_UNIFORM_BUFFER:
			return sizeof(FRDGUniformBufferBinding);
		case UBMT_RENDER_TARGET_BINDING_SLOTS:
			return sizeof(FRenderTargetBindingSlots);
		default:
			return sizeof(void *);
	}
}

uint32 FShaderParametersMetadata::FMember::GetMemberSize() const {
	return GetElementSize() * FMath::Max(1u, NumElements);
}

FShaderParametersMetadata::FShaderParametersMetadata(EUseCase InUseCase, const char *InLayoutName, const char *InStructTypeName, const char *InShaderVariableName, uint32 InSize, const LocalVector<FMember> &InMembers) :
		UseCase(InUseCase), LayoutName(InLayoutName), StructTypeName(InStructTypeName), ShaderVariableName(InShaderVariableName), Size(InSize), Members(InMembers) {
	Flatten(*this, 0, String());
}

void FShaderParametersMetadata::Flatten(const FShaderParametersMetadata &Struct, uint32 BaseOffset, const String &Prefix) {
	for (const FMember &Member : Struct.GetMembers()) {
		const EUniformBufferBaseType BaseType = Member.GetBaseType();
		const uint32 Offset = BaseOffset + Member.GetOffset();
		const String MemberName = Prefix + String::utf8(Member.GetName());

		if (BaseType == UBMT_NESTED_STRUCT || BaseType == UBMT_INCLUDED_STRUCT) {
			const FShaderParametersMetadata *Child = Member.GetStructMetadata();
			if (!Child) continue;
			const bool bInclude = BaseType == UBMT_INCLUDED_STRUCT;
			if (Member.GetNumElements() == 0) {
				Flatten(*Child, Offset, bInclude ? Prefix : MemberName + "_");
			} else {
				for (uint32 Index = 0; Index < Member.GetNumElements(); Index++) Flatten(*Child, Offset + Index * Child->GetSize(), MemberName + "_" + itos(Index) + "_");
			}
			continue;
		}

		FFlatParameter Parameter;
		Parameter.Name = MemberName;
		Parameter.ByteOffset = Offset;
		Parameter.BaseType = BaseType;
		Parameter.NumElements = Member.GetNumElements();
		Parameter.ElementSize = Member.GetElementSize();
		Parameter.Member = &Member;

		if (IsLooseShaderParameterType(BaseType)) {
			LooseParameters.push_back(Parameter);
			continue;
		}

		if (BaseType == UBMT_RENDER_TARGET_BINDING_SLOTS) {
			if (RenderTargetsOffset == UINT32_MAX) RenderTargetsOffset = Offset;
			bHasGraphResources = true;
		}
		if (BaseType == UBMT_UAV) bHasExternalOutputs = true;
		if (IsRDGResourceReferenceShaderParameterType(BaseType)) bHasGraphResources = true;
		if (BaseType == UBMT_RDG_UNIFORM_BUFFER) Parameter.UniformBufferStruct = Member.GetStructMetadata();
		ResourceParameters.push_back(Parameter);
	}
}

String FShaderParametersMetadata::ToString() const {
	String Result = vformat("%s (size %d)\n", StructTypeName, Size);
	for (const FFlatParameter &Parameter : LooseParameters) Result += vformat("  [%3d] %-24s %s x%d (%d bytes)\n", Parameter.ByteOffset, Parameter.Name, GetShaderParameterMacroName(Parameter.BaseType), FMath::Max(1u, Parameter.NumElements), Parameter.ElementSize);
	for (const FFlatParameter &Parameter : ResourceParameters) Result += vformat("  [%3d] %-24s %s x%d\n", Parameter.ByteOffset, Parameter.Name, GetShaderParameterMacroName(Parameter.BaseType), FMath::Max(1u, Parameter.NumElements));
	return Result;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Uniform buffer registry.

FUniformBufferStructRegistration *&FUniformBufferStructRegistration::GetHead() {
	static FUniformBufferStructRegistration *Head = nullptr;
	return Head;
}

FUniformBufferStructRegistration::FUniformBufferStructRegistration(FGetter InGetter) :
		Getter(InGetter) {
	Next = GetHead();
	GetHead() = this;
}

void FUniformBufferStructRegistration::ForEach(void (*Function)(const FShaderParametersMetadata *, void *), void *UserData) {
	for (FUniformBufferStructRegistration *It = GetHead(); It; It = It->Next) Function(It->Getter(), UserData);
}

const FShaderParametersMetadata *FindUniformBufferStructByShaderVariableName(const char *Name) {
	struct FSearch {
		const char *Name;
		const FShaderParametersMetadata *Result;
	} Search = { Name, nullptr };
	FUniformBufferStructRegistration::ForEach([](const FShaderParametersMetadata *Metadata, void *UserData) {
		FSearch *S = reinterpret_cast<FSearch *>(UserData);
		if (Metadata && Metadata->GetShaderVariableName() && strcmp(Metadata->GetShaderVariableName(), S->Name) == 0) S->Result = Metadata;
	},
			&Search);
	return Search.Result;
}

#pragma once

// UE-compatible shader parameter structs:
//
//	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//		SHADER_PARAMETER(FVector2f, InvSize)
//		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
//		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
//		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
//		RENDER_TARGET_BINDING_SLOTS()
//	END_SHADER_PARAMETER_STRUCT()
//
// The macros generate the C++ members plus an FShaderParametersMetadata describing every member (name, offset,
// type). RDG walks that metadata to find the resources a pass reads / writes, and the shader binder matches the
// member names against the names reflected from the GLSL SPIR-V (descriptors and push-constant members).

#include "RenderGraphDefinitions.h"

#include <cstddef>

enum EUniformBufferBaseType : uint8 {
	UBMT_INVALID,

	// Constant (loose) parameters.
	UBMT_BOOL,
	UBMT_INT32,
	UBMT_UINT32,
	UBMT_FLOAT32,

	// RHI resources not tracked by the graph.
	UBMT_TEXTURE,
	UBMT_SRV,
	UBMT_UAV,
	UBMT_SAMPLER,

	// Resources tracked by the render graph.
	UBMT_RDG_TEXTURE,
	UBMT_RDG_TEXTURE_ACCESS,
	UBMT_RDG_TEXTURE_ACCESS_ARRAY,
	UBMT_RDG_TEXTURE_SRV,
	UBMT_RDG_TEXTURE_NON_PIXEL_SRV,
	UBMT_RDG_TEXTURE_UAV,
	UBMT_RDG_BUFFER_ACCESS,
	UBMT_RDG_BUFFER_ACCESS_ARRAY,
	UBMT_RDG_BUFFER_SRV,
	UBMT_RDG_BUFFER_UAV,
	UBMT_RDG_UNIFORM_BUFFER,

	// Nested structures.
	UBMT_NESTED_STRUCT,
	UBMT_INCLUDED_STRUCT,
	UBMT_REFERENCED_STRUCT,
	UBMT_RENDER_TARGET_BINDING_SLOTS,

	EUniformBufferBaseType_Num,
};

namespace EShaderPrecisionModifier {
enum Type : uint8 {
	Float,
	Half,
	Fixed,
};
}

inline bool IsShaderParameterTypeForUniformBufferLayout(EUniformBufferBaseType Type) { return Type >= UBMT_TEXTURE && Type <= UBMT_RDG_UNIFORM_BUFFER; }
inline bool IsRDGResourceReferenceShaderParameterType(EUniformBufferBaseType Type) { return Type >= UBMT_RDG_TEXTURE && Type <= UBMT_RDG_UNIFORM_BUFFER; }
inline bool IsRDGTextureReferenceShaderParameterType(EUniformBufferBaseType Type) { return Type >= UBMT_RDG_TEXTURE && Type <= UBMT_RDG_TEXTURE_UAV; }
inline bool IsRDGBufferReferenceShaderParameterType(EUniformBufferBaseType Type) { return Type >= UBMT_RDG_BUFFER_ACCESS && Type <= UBMT_RDG_BUFFER_UAV; }
inline bool IsLooseShaderParameterType(EUniformBufferBaseType Type) { return Type >= UBMT_BOOL && Type <= UBMT_FLOAT32; }
const char *GetShaderParameterMacroName(EUniformBufferBaseType ShaderParameterBaseType);

/** Reflection data of a shader parameter struct, equivalent to UE's FShaderParametersMetadata. */
class FShaderParametersMetadata {
public:
	enum class EUseCase : uint8 {
		ShaderParameterStruct,
		UniformBuffer,
		DataDrivenUniformBuffer,
	};

	class FMember {
	public:
		FMember() = default;
		FMember(const char *InName, const char *InShaderType, int32 InFileLine, uint32 InOffset, EUniformBufferBaseType InBaseType, EShaderPrecisionModifier::Type InPrecision, uint32 InNumRows, uint32 InNumColumns, uint32 InNumElements, const FShaderParametersMetadata *InStruct) :
				Name(InName), ShaderType(InShaderType), FileLine(InFileLine), Offset(InOffset), BaseType(InBaseType), Precision(InPrecision), NumRows(InNumRows), NumColumns(InNumColumns), NumElements(InNumElements), Struct(InStruct) {}

		const char *GetName() const { return Name; }
		const char *GetShaderType() const { return ShaderType; }
		int32 GetFileLine() const { return FileLine; }
		uint32 GetOffset() const { return Offset; }
		EUniformBufferBaseType GetBaseType() const { return BaseType; }
		EShaderPrecisionModifier::Type GetPrecision() const { return Precision; }
		uint32 GetNumRows() const { return NumRows; }
		uint32 GetNumColumns() const { return NumColumns; }
		/** 0 for non-array members. */
		uint32 GetNumElements() const { return NumElements; }
		const FShaderParametersMetadata *GetStructMetadata() const { return Struct; }
		/** Size in bytes of one element of a loose (constant) member. */
		uint32 GetMemberSize() const;
		uint32 GetElementSize() const;

	private:
		const char *Name = "";
		const char *ShaderType = "";
		int32 FileLine = 0;
		uint32 Offset = 0;
		EUniformBufferBaseType BaseType = UBMT_INVALID;
		EShaderPrecisionModifier::Type Precision = EShaderPrecisionModifier::Float;
		uint32 NumRows = 1;
		uint32 NumColumns = 1;
		uint32 NumElements = 0;
		const FShaderParametersMetadata *Struct = nullptr;
	};

	/** A leaf parameter after flattening nested / included structs. ByteOffset is relative to the root struct. */
	struct FFlatParameter {
		String Name;
		uint32 ByteOffset = 0;
		EUniformBufferBaseType BaseType = UBMT_INVALID;
		uint32 NumElements = 0;
		uint32 ElementSize = 0;
		const FMember *Member = nullptr;
		/** Set for UBMT_RDG_UNIFORM_BUFFER members. */
		const FShaderParametersMetadata *UniformBufferStruct = nullptr;
	};

	FShaderParametersMetadata(EUseCase InUseCase, const char *InLayoutName, const char *InStructTypeName, const char *InShaderVariableName, uint32 InSize, const LocalVector<FMember> &InMembers);

	EUseCase GetUseCase() const { return UseCase; }
	const char *GetLayoutName() const { return LayoutName; }
	const char *GetStructTypeName() const { return StructTypeName; }
	const char *GetShaderVariableName() const { return ShaderVariableName; }
	uint32 GetSize() const { return Size; }
	const LocalVector<FMember> &GetMembers() const { return Members; }

	/** All loose (constant) parameters, flattened with UE naming ("Struct_Member" for nested structs). */
	const LocalVector<FFlatParameter> &GetLooseParameters() const { return LooseParameters; }
	/** All resource parameters (RHI + RDG + uniform buffers + render target slots), flattened. */
	const LocalVector<FFlatParameter> &GetResourceParameters() const { return ResourceParameters; }

	bool HasRenderTargets() const { return RenderTargetsOffset != UINT32_MAX; }
	uint32 GetRenderTargetsOffset() const { return RenderTargetsOffset; }
	/** True if the struct writes to resources outside of the graph (raw RHI UAVs), which prevents culling. */
	bool HasExternalOutputs() const { return bHasExternalOutputs; }
	bool HasGraphResources() const { return bHasGraphResources; }

	/** Debug dump of the layout. */
	String ToString() const;

private:
	void Flatten(const FShaderParametersMetadata &Struct, uint32 BaseOffset, const String &Prefix);

	EUseCase UseCase;
	const char *LayoutName;
	const char *StructTypeName;
	const char *ShaderVariableName;
	uint32 Size;
	LocalVector<FMember> Members;
	LocalVector<FFlatParameter> LooseParameters;
	LocalVector<FFlatParameter> ResourceParameters;
	uint32 RenderTargetsOffset = UINT32_MAX;
	bool bHasExternalOutputs = false;
	bool bHasGraphResources = false;
};

/** Finds a registered uniform buffer struct by its shader variable name. */
const FShaderParametersMetadata *FindUniformBufferStructByShaderVariableName(const char *Name);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Type infos.

template <typename TypeParameter>
struct TShaderParameterTypeInfo;

#define RDG_DECLARE_LOOSE_TYPE_INFO(CppType, InBaseType, InRows, InColumns)                       \
	template <>                                                                                   \
	struct TShaderParameterTypeInfo<CppType> {                                                    \
		static constexpr EUniformBufferBaseType BaseType = InBaseType;                            \
		static constexpr int32 NumRows = InRows;                                                  \
		static constexpr int32 NumColumns = InColumns;                                            \
		static constexpr int32 NumElements = 0;                                                   \
		static constexpr int32 Alignment = alignof(CppType);                                      \
		static constexpr bool bIsStoredInConstantBuffer = true;                                   \
		using TAlignedType = CppType;                                                             \
		static const FShaderParametersMetadata *GetStructMetadata() { return nullptr; }           \
	};

RDG_DECLARE_LOOSE_TYPE_INFO(bool, UBMT_BOOL, 1, 1)
RDG_DECLARE_LOOSE_TYPE_INFO(uint32, UBMT_UINT32, 1, 1)
RDG_DECLARE_LOOSE_TYPE_INFO(int32, UBMT_INT32, 1, 1)
RDG_DECLARE_LOOSE_TYPE_INFO(float, UBMT_FLOAT32, 1, 1)
RDG_DECLARE_LOOSE_TYPE_INFO(FVector2f, UBMT_FLOAT32, 1, 2)
RDG_DECLARE_LOOSE_TYPE_INFO(FVector3f, UBMT_FLOAT32, 1, 3)
RDG_DECLARE_LOOSE_TYPE_INFO(FVector4f, UBMT_FLOAT32, 1, 4)
RDG_DECLARE_LOOSE_TYPE_INFO(FLinearColor, UBMT_FLOAT32, 1, 4)
RDG_DECLARE_LOOSE_TYPE_INFO(FIntPoint, UBMT_INT32, 1, 2)
RDG_DECLARE_LOOSE_TYPE_INFO(FIntVector, UBMT_INT32, 1, 3)
RDG_DECLARE_LOOSE_TYPE_INFO(FIntVector4, UBMT_INT32, 1, 4)
RDG_DECLARE_LOOSE_TYPE_INFO(FUintVector2, UBMT_UINT32, 1, 2)
RDG_DECLARE_LOOSE_TYPE_INFO(FUintVector3, UBMT_UINT32, 1, 3)
RDG_DECLARE_LOOSE_TYPE_INFO(FUintVector4, UBMT_UINT32, 1, 4)
RDG_DECLARE_LOOSE_TYPE_INFO(FMatrix44f, UBMT_FLOAT32, 4, 4)

template <typename T, size_t InNumElements>
struct TShaderParameterTypeInfo<T[InNumElements]> {
	static constexpr EUniformBufferBaseType BaseType = TShaderParameterTypeInfo<T>::BaseType;
	static constexpr int32 NumRows = TShaderParameterTypeInfo<T>::NumRows;
	static constexpr int32 NumColumns = TShaderParameterTypeInfo<T>::NumColumns;
	static constexpr int32 NumElements = (int32)InNumElements;
	static constexpr int32 Alignment = alignof(T);
	static constexpr bool bIsStoredInConstantBuffer = true;
	using TAlignedType = T[InNumElements];
	static const FShaderParametersMetadata *GetStructMetadata() { return nullptr; }
};

template <typename TypeParameter>
struct TShaderResourceParameterTypeInfo {
	static constexpr int32 NumRows = 1;
	static constexpr int32 NumColumns = 1;
	static constexpr int32 NumElements = 0;
	static constexpr int32 Alignment = alignof(TypeParameter);
	static constexpr bool bIsStoredInConstantBuffer = false;
	using TAlignedType = TypeParameter;
	static const FShaderParametersMetadata *GetStructMetadata() { return nullptr; }
};

template <typename TypeParameter, size_t InNumElements>
struct TShaderResourceParameterTypeInfo<TypeParameter[InNumElements]> {
	static constexpr int32 NumRows = 1;
	static constexpr int32 NumColumns = 1;
	static constexpr int32 NumElements = (int32)InNumElements;
	static constexpr int32 Alignment = alignof(TypeParameter);
	static constexpr bool bIsStoredInConstantBuffer = false;
	using TAlignedType = TypeParameter[InNumElements];
	static const FShaderParametersMetadata *GetStructMetadata() { return nullptr; }
};

template <typename StructType, size_t InNumElements>
struct TShaderParameterStructTypeInfo {
	static constexpr int32 NumRows = 1;
	static constexpr int32 NumColumns = 1;
	static constexpr int32 NumElements = (int32)InNumElements;
	static constexpr int32 Alignment = alignof(StructType);
	static constexpr bool bIsStoredInConstantBuffer = true;
	using TAlignedType = StructType[InNumElements];
	static const FShaderParametersMetadata *GetStructMetadata() { return StructType::FTypeInfo::GetStructMetadata(); }
};

template <typename T>
struct TShaderParameterStructArrayHelper;
template <typename StructType, size_t N>
struct TShaderParameterStructArrayHelper<StructType[N]> {
	using TypeInfo = TShaderParameterStructTypeInfo<StructType, N>;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// RDG resource access helpers (RDG_TEXTURE_ACCESS / RDG_BUFFER_ACCESS).

class FRDGTextureAccess {
public:
	FRDGTextureAccess() = default;
	FRDGTextureAccess(FRDGTexture *InTexture, ERHIAccess InAccess) :
			Texture(InTexture), Access(InAccess) {}

	FRDGTexture *GetTexture() const { return Texture; }
	ERHIAccess GetAccess() const { return Access; }
	operator FRDGTexture *() const { return Texture; }
	FRDGTexture *operator->() const { return Texture; }
	explicit operator bool() const { return Texture != nullptr; }

protected:
	FRDGTexture *Texture = nullptr;
	ERHIAccess Access = ERHIAccess::Unknown;
};

template <ERHIAccess InAccess>
class TRDGTextureAccess : public FRDGTextureAccess {
public:
	TRDGTextureAccess() :
			FRDGTextureAccess(nullptr, InAccess) {}
	TRDGTextureAccess(FRDGTexture *InTexture) :
			FRDGTextureAccess(InTexture, InAccess) {}
	TRDGTextureAccess &operator=(FRDGTexture *InTexture) {
		Texture = InTexture;
		return *this;
	}
};

class FRDGBufferAccess {
public:
	FRDGBufferAccess() = default;
	FRDGBufferAccess(FRDGBuffer *InBuffer, ERHIAccess InAccess) :
			Buffer(InBuffer), Access(InAccess) {}

	FRDGBuffer *GetBuffer() const { return Buffer; }
	ERHIAccess GetAccess() const { return Access; }
	operator FRDGBuffer *() const { return Buffer; }
	FRDGBuffer *operator->() const { return Buffer; }
	explicit operator bool() const { return Buffer != nullptr; }

protected:
	FRDGBuffer *Buffer = nullptr;
	ERHIAccess Access = ERHIAccess::Unknown;
};

template <ERHIAccess InAccess>
class TRDGBufferAccess : public FRDGBufferAccess {
public:
	TRDGBufferAccess() :
			FRDGBufferAccess(nullptr, InAccess) {}
	TRDGBufferAccess(FRDGBuffer *InBuffer) :
			FRDGBufferAccess(InBuffer, InAccess) {}
	TRDGBufferAccess &operator=(FRDGBuffer *InBuffer) {
		Buffer = InBuffer;
		return *this;
	}
};

/** Dynamic arrays of accesses (RDG_TEXTURE_ACCESS_ARRAY / RDG_BUFFER_ACCESS_ARRAY). Elements are copied into the pass. */
class FRDGTextureAccessArray : public LocalVector<FRDGTextureAccess> {};
class FRDGBufferAccessArray : public LocalVector<FRDGBufferAccess> {};

template <typename TypeParameter>
struct TRDGResourceAccessTypeInfo {
	static constexpr int32 NumRows = 1;
	static constexpr int32 NumColumns = 1;
	static constexpr int32 NumElements = 0;
	static constexpr int32 Alignment = alignof(TypeParameter);
	static constexpr bool bIsStoredInConstantBuffer = false;
	using TAlignedType = TypeParameter;
	static const FShaderParametersMetadata *GetStructMetadata() { return nullptr; }
};
template <typename TypeParameter>
using TRDGTextureAccessTypeInfo = TRDGResourceAccessTypeInfo<TypeParameter>;

/** Binding of an RDG uniform buffer in a parameter struct. */
class FRDGUniformBufferBinding {
public:
	FRDGUniformBufferBinding() = default;
	FRDGUniformBufferBinding(FRDGUniformBuffer *InUniformBuffer) :
			UniformBuffer(InUniformBuffer) {}
	FRDGUniformBuffer *GetUniformBuffer() const { return UniformBuffer; }
	FRDGUniformBuffer *operator->() const { return UniformBuffer; }
	explicit operator bool() const { return UniformBuffer != nullptr; }

protected:
	FRDGUniformBuffer *UniformBuffer = nullptr;
};

template <typename TBufferStruct>
class TRDGUniformBufferBinding : public FRDGUniformBufferBinding {
public:
	TRDGUniformBufferBinding() = default;
	TRDGUniformBufferBinding(TRDGUniformBuffer<TBufferStruct> *InUniformBuffer) :
			FRDGUniformBufferBinding((FRDGUniformBuffer *)InUniformBuffer) {}
	TRDGUniformBufferBinding &operator=(TRDGUniformBuffer<TBufferStruct> *InUniformBuffer) {
		UniformBuffer = (FRDGUniformBuffer *)InUniformBuffer;
		return *this;
	}
	TRDGUniformBuffer<TBufferStruct> *GetUniformBuffer() const { return (TRDGUniformBuffer<TBufferStruct> *)UniformBuffer; }
	TRDGUniformBuffer<TBufferStruct> *operator->() const { return GetUniformBuffer(); }
};

template <typename TBufferStruct>
struct TShaderParameterTypeInfo<TRDGUniformBufferBinding<TBufferStruct>> {
	static constexpr EUniformBufferBaseType BaseType = UBMT_RDG_UNIFORM_BUFFER;
	static constexpr int32 NumRows = 1;
	static constexpr int32 NumColumns = 1;
	static constexpr int32 NumElements = 0;
	static constexpr int32 Alignment = alignof(void *);
	static constexpr bool bIsStoredInConstantBuffer = false;
	using TAlignedType = TRDGUniformBufferBinding<TBufferStruct>;
	static const FShaderParametersMetadata *GetStructMetadata() { return TBufferStruct::FTypeInfo::GetStructMetadata(); }
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Render target bindings.

class FRenderTargetBinding {
public:
	FRenderTargetBinding() = default;
	FRenderTargetBinding(FRDGTexture *InTexture, ERenderTargetLoadAction InLoadAction, uint8 InMipIndex = 0, int16 InArraySlice = -1) :
			Texture(InTexture), LoadAction(InLoadAction), MipIndex(InMipIndex), ArraySlice(InArraySlice) {}
	FRenderTargetBinding(FRDGTexture *InTexture, FRDGTexture *InResolveTexture, ERenderTargetLoadAction InLoadAction, uint8 InMipIndex = 0, int16 InArraySlice = -1) :
			Texture(InTexture), ResolveTexture(InResolveTexture), LoadAction(InLoadAction), MipIndex(InMipIndex), ArraySlice(InArraySlice) {}

	FRDGTexture *GetTexture() const { return Texture; }
	FRDGTexture *GetResolveTexture() const { return ResolveTexture; }
	ERenderTargetLoadAction GetLoadAction() const { return LoadAction; }
	uint8 GetMipIndex() const { return MipIndex; }
	int16 GetArraySlice() const { return ArraySlice; }

	void SetTexture(FRDGTexture *InTexture) { Texture = InTexture; }
	void SetResolveTexture(FRDGTexture *InTexture) { ResolveTexture = InTexture; }
	void SetLoadAction(ERenderTargetLoadAction InLoadAction) { LoadAction = InLoadAction; }
	void SetMipIndex(uint8 InMipIndex) { MipIndex = InMipIndex; }
	void SetArraySlice(int16 InArraySlice) { ArraySlice = InArraySlice; }

	bool CanMergeBefore(const FRenderTargetBinding &Other) const {
		return Texture == Other.Texture && ResolveTexture == Other.ResolveTexture && Other.LoadAction != ERenderTargetLoadAction::EClear && MipIndex == Other.MipIndex && ArraySlice == Other.ArraySlice;
	}

private:
	FRDGTexture *Texture = nullptr;
	FRDGTexture *ResolveTexture = nullptr;
	ERenderTargetLoadAction LoadAction = ERenderTargetLoadAction::ENoAction;
	uint8 MipIndex = 0;
	int16 ArraySlice = -1;
};

class FDepthStencilBinding {
public:
	FDepthStencilBinding() = default;
	FDepthStencilBinding(FRDGTexture *InTexture, ERenderTargetLoadAction InDepthLoadAction, ERenderTargetLoadAction InStencilLoadAction, FExclusiveDepthStencil InDepthStencilAccess) :
			Texture(InTexture), DepthLoadAction(InDepthLoadAction), StencilLoadAction(InStencilLoadAction), DepthStencilAccess(InDepthStencilAccess) {}
	FDepthStencilBinding(FRDGTexture *InTexture, ERenderTargetLoadAction InDepthLoadAction, FExclusiveDepthStencil InDepthStencilAccess) :
			Texture(InTexture), DepthLoadAction(InDepthLoadAction), DepthStencilAccess(InDepthStencilAccess) {}

	FRDGTexture *GetTexture() const { return Texture; }
	FRDGTexture *GetResolveTexture() const { return ResolveTexture; }
	ERenderTargetLoadAction GetDepthLoadAction() const { return DepthLoadAction; }
	ERenderTargetLoadAction GetStencilLoadAction() const { return StencilLoadAction; }
	FExclusiveDepthStencil GetDepthStencilAccess() const { return DepthStencilAccess; }

	void SetTexture(FRDGTexture *InTexture) { Texture = InTexture; }
	void SetDepthLoadAction(ERenderTargetLoadAction InAction) { DepthLoadAction = InAction; }
	void SetStencilLoadAction(ERenderTargetLoadAction InAction) { StencilLoadAction = InAction; }
	void SetDepthStencilAccess(FExclusiveDepthStencil InAccess) { DepthStencilAccess = InAccess; }

	bool CanMergeBefore(const FDepthStencilBinding &Other) const {
		return Texture == Other.Texture && ResolveTexture == Other.ResolveTexture && Other.DepthLoadAction != ERenderTargetLoadAction::EClear && Other.StencilLoadAction != ERenderTargetLoadAction::EClear && DepthStencilAccess == Other.DepthStencilAccess;
	}

private:
	FRDGTexture *Texture = nullptr;
	FRDGTexture *ResolveTexture = nullptr;
	ERenderTargetLoadAction DepthLoadAction = ERenderTargetLoadAction::ENoAction;
	ERenderTargetLoadAction StencilLoadAction = ERenderTargetLoadAction::ENoAction;
	FExclusiveDepthStencil DepthStencilAccess = FExclusiveDepthStencil::DepthNop_StencilNop;
};

enum class ESubpassHint : uint8 {
	None,
	DepthReadSubpass,
	CustomResolveSubpass,
};

struct FRenderTargetBindingSlots {
	FRenderTargetBinding Output[MaxSimultaneousRenderTargets];
	FDepthStencilBinding DepthStencil;
	FResolveRect ResolveRect;
	uint32 NumOcclusionQueries = 0;
	ESubpassHint SubpassHint = ESubpassHint::None;
	uint8 MultiViewCount = 0;
	FRDGTexture *ShadingRateTexture = nullptr;

	FRenderTargetBinding &operator[](uint32 Index) { return Output[Index]; }
	const FRenderTargetBinding &operator[](uint32 Index) const { return Output[Index]; }

	template <typename TFunction>
	void Enumerate(TFunction Function) const {
		for (uint32 Index = 0; Index < MaxSimultaneousRenderTargets; Index++) {
			if (!Output[Index].GetTexture()) break;
			Function(Output[Index]);
		}
	}

	uint32 GetActiveCount() const {
		uint32 Count = 0;
		for (uint32 Index = 0; Index < MaxSimultaneousRenderTargets; Index++) {
			if (!Output[Index].GetTexture()) break;
			Count++;
		}
		return Count;
	}

	bool CanMergeBefore(const FRenderTargetBindingSlots &Other) const {
		for (uint32 Index = 0; Index < MaxSimultaneousRenderTargets; Index++) {
			if (!Output[Index].CanMergeBefore(Other.Output[Index])) return false;
		}
		if (!DepthStencil.CanMergeBefore(Other.DepthStencil)) return false;
		return MultiViewCount == Other.MultiViewCount && ShadingRateTexture == Other.ShadingRateTexture && SubpassHint == Other.SubpassHint;
	}

	struct FTypeInfo {
		static constexpr int32 NumRows = 1;
		static constexpr int32 NumColumns = 1;
		static constexpr int32 NumElements = 0;
		static constexpr int32 Alignment = alignof(void *);
		static constexpr bool bIsStoredInConstantBuffer = false;
		using TAlignedType = FRenderTargetBindingSlots;
		static const FShaderParametersMetadata *GetStructMetadata() { return nullptr; }
	};
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Struct declaration macros. Same member-chaining technique as UE: every member macro closes the typedef opened by
// the previous one and defines a static function that appends its metadata, then returns the previous function.

#define SHADER_PARAMETER_STRUCT_ALIGNMENT 16

#define INTERNAL_SHADER_PARAMETER_GET_STRUCT_METADATA(StructTypeName)                                               \
	{                                                                                                               \
		static FShaderParametersMetadata StaticStructMetadata(                                                      \
				FShaderParametersMetadata::EUseCase::ShaderParameterStruct,                                         \
				#StructTypeName,                                                                                    \
				#StructTypeName,                                                                                    \
				nullptr,                                                                                            \
				sizeof(StructTypeName),                                                                             \
				StructTypeName::zzGetMembers());                                                                    \
		return &StaticStructMetadata;                                                                               \
	}

#define INTERNAL_UNIFORM_BUFFER_STRUCT_GET_STRUCT_METADATA(StructTypeName) \
	{ return StructTypeName::GetStructMetadata(); }

#define INTERNAL_SHADER_PARAMETER_STRUCT_BEGIN(StructTypeName, PrefixKeywords, ConstructorSuffix, GetStructMetadataScope)      \
	class alignas(SHADER_PARAMETER_STRUCT_ALIGNMENT) StructTypeName {                                                     \
	public:                                                                                                                   \
		StructTypeName() ConstructorSuffix struct FTypeInfo {                                                                 \
			static constexpr int32 NumRows = 1;                                                                               \
			static constexpr int32 NumColumns = 1;                                                                            \
			static constexpr int32 NumElements = 0;                                                                           \
			static constexpr int32 Alignment = SHADER_PARAMETER_STRUCT_ALIGNMENT;                                             \
			static constexpr bool bIsStoredInConstantBuffer = true;                                                           \
			using TAlignedType = StructTypeName;                                                                              \
			static const FShaderParametersMetadata *GetStructMetadata() GetStructMetadataScope                                \
		};                                                                                                                    \
                                                                                                                              \
	private:                                                                                                                  \
		typedef StructTypeName zzTThisStruct;                                                                                 \
		struct zzFirstMemberId {                                                                                              \
			enum { HasDeclaredResource = 0 };                                                                                 \
		};                                                                                                                    \
		typedef void *zzFuncPtr;                                                                                              \
		typedef zzFuncPtr (*zzMemberFunc)(zzFirstMemberId, LocalVector<FShaderParametersMetadata::FMember> *);              \
		static zzFuncPtr zzAppendMemberGetPrev(zzFirstMemberId, LocalVector<FShaderParametersMetadata::FMember> *) {          \
			return nullptr;                                                                                                   \
		}                                                                                                                     \
		typedef zzFirstMemberId

#define INTERNAL_SHADER_PARAMETER_EXPLICIT(BaseType, TypeInfo, MemberType, MemberName, ArrayDecl, DefaultValue, Precision, OptionalShaderType, IsMemberStruct) \
	zzMemberId##MemberName;                                                                                                                                         \
                                                                                                                                                                    \
public:                                                                                                                                                             \
	alignas(TypeInfo::Alignment) TypeInfo::TAlignedType MemberName DefaultValue;                                                                                    \
	static_assert(BaseType != UBMT_INVALID, "Invalid type " #MemberType " of member " #MemberName ".");                                                           \
                                                                                                                                                                    \
private:                                                                                                                                                            \
	struct zzNextMemberId##MemberName {                                                                                                                             \
		enum { HasDeclaredResource = zzMemberId##MemberName::HasDeclaredResource || !TypeInfo::bIsStoredInConstantBuffer };                                       \
	};                                                                                                                                                              \
	static zzFuncPtr zzAppendMemberGetPrev(zzNextMemberId##MemberName, LocalVector<FShaderParametersMetadata::FMember> *Members) {                                 \
		Members->push_back(FShaderParametersMetadata::FMember(                                                                                                      \
				#MemberName,                                                                                                                                        \
				OptionalShaderType,                                                                                                                                 \
				__LINE__,                                                                                                                                           \
				(uint32)offsetof(zzTThisStruct, MemberName),                                                                                                        \
				EUniformBufferBaseType(BaseType),                                                                                                                   \
				Precision,                                                                                                                                          \
				TypeInfo::NumRows,                                                                                                                                  \
				TypeInfo::NumColumns,                                                                                                                               \
				TypeInfo::NumElements,                                                                                                                              \
				TypeInfo::GetStructMetadata()));                                                                                                                    \
		zzFuncPtr (*PrevFunc)(zzMemberId##MemberName, LocalVector<FShaderParametersMetadata::FMember> *);                                                          \
		PrevFunc = zzAppendMemberGetPrev;                                                                                                                           \
		return (zzFuncPtr)PrevFunc;                                                                                                                                 \
	}                                                                                                                                                               \
	typedef zzNextMemberId##MemberName

#define BEGIN_SHADER_PARAMETER_STRUCT(StructTypeName, PrefixKeywords) \
	INTERNAL_SHADER_PARAMETER_STRUCT_BEGIN(StructTypeName, PrefixKeywords, {}, INTERNAL_SHADER_PARAMETER_GET_STRUCT_METADATA(StructTypeName))

#define END_SHADER_PARAMETER_STRUCT()                                                                                \
	zzLastMemberId;                                                                                                  \
                                                                                                                     \
public:                                                                                                              \
	static LocalVector<FShaderParametersMetadata::FMember> zzGetMembers() {                                          \
		LocalVector<FShaderParametersMetadata::FMember> Members;                                                     \
		zzFuncPtr (*LastFunc)(zzLastMemberId, LocalVector<FShaderParametersMetadata::FMember> *);                    \
		LastFunc = zzAppendMemberGetPrev;                                                                            \
		zzFuncPtr Ptr = (zzFuncPtr)LastFunc;                                                                         \
		do {                                                                                                         \
			Ptr = reinterpret_cast<zzMemberFunc>(Ptr)(zzFirstMemberId(), &Members);                                  \
		} while (Ptr);                                                                                               \
		Members.reverse();                                                                                            \
		return Members;                                                                                              \
	}                                                                                                                \
	}                                                                                                                \
	;

/** Uniform buffer structs: declare with BEGIN/END_UNIFORM_BUFFER_STRUCT and implement once in a .cpp with
 *  IMPLEMENT_UNIFORM_BUFFER_STRUCT(FStruct, "ShaderVariableName"). The GLSL side declares
 *  `layout(set = S, binding = B, std140) uniform FStructBlock { ... } ShaderVariableName;` */
#define BEGIN_UNIFORM_BUFFER_STRUCT(StructTypeName, PrefixKeywords) \
	INTERNAL_SHADER_PARAMETER_STRUCT_BEGIN(StructTypeName, PrefixKeywords, {} static const FShaderParametersMetadata *GetStructMetadata();, INTERNAL_UNIFORM_BUFFER_STRUCT_GET_STRUCT_METADATA(StructTypeName))

#define END_UNIFORM_BUFFER_STRUCT() \
	END_SHADER_PARAMETER_STRUCT()

#define IMPLEMENT_UNIFORM_BUFFER_STRUCT(StructTypeName, ShaderVariableName)                \
	const FShaderParametersMetadata *StructTypeName::GetStructMetadata() {                 \
		static FShaderParametersMetadata StaticStructMetadata(                             \
				FShaderParametersMetadata::EUseCase::UniformBuffer,                        \
				#StructTypeName,                                                           \
				#StructTypeName,                                                           \
				ShaderVariableName,                                                        \
				sizeof(StructTypeName),                                                    \
				StructTypeName::zzGetMembers());                                           \
		return &StaticStructMetadata;                                                      \
	}                                                                                      \
	static FUniformBufferStructRegistration zzUniformBufferRegistration##StructTypeName(&StructTypeName::GetStructMetadata);

/** Registers uniform buffer metadata so it can be looked up by shader variable name. */
class FUniformBufferStructRegistration {
public:
	using FGetter = const FShaderParametersMetadata *(*)();
	explicit FUniformBufferStructRegistration(FGetter InGetter);
	static void ForEach(void (*Function)(const FShaderParametersMetadata *, void *), void *UserData);

private:
	FGetter Getter;
	FUniformBufferStructRegistration *Next = nullptr;
	static FUniformBufferStructRegistration *&GetHead();
};

#define SHADER_PARAMETER(MemberType, MemberName) \
	SHADER_PARAMETER_EX(MemberType, MemberName, EShaderPrecisionModifier::Float)

#define SHADER_PARAMETER_EX(MemberType, MemberName, Precision) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(TShaderParameterTypeInfo<MemberType>::BaseType, TShaderParameterTypeInfo<MemberType>, MemberType, MemberName, , , Precision, "", false)

#define SHADER_PARAMETER_ARRAY(MemberType, MemberName, ArrayDecl) \
	SHADER_PARAMETER_ARRAY_EX(MemberType, MemberName, ArrayDecl, EShaderPrecisionModifier::Float)

#define SHADER_PARAMETER_ARRAY_EX(MemberType, MemberName, ArrayDecl, Precision) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(TShaderParameterTypeInfo<MemberType ArrayDecl>::BaseType, TShaderParameterTypeInfo<MemberType ArrayDecl>, MemberType, MemberName, ArrayDecl, , Precision, "", false)

#define SHADER_PARAMETER_SCALAR_ARRAY(MemberType, MemberName, ArrayDecl) \
	SHADER_PARAMETER_ARRAY(MemberType, MemberName, ArrayDecl)

#define SHADER_PARAMETER_TEXTURE(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_TEXTURE, TShaderResourceParameterTypeInfo<FRHITexture *>, FRHITexture *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_TEXTURE_ARRAY(ShaderType, MemberName, ArrayDecl) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_TEXTURE, TShaderResourceParameterTypeInfo<FRHITexture * ArrayDecl>, FRHITexture *, MemberName, ArrayDecl, , EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_SRV(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_SRV, TShaderResourceParameterTypeInfo<FRHIShaderResourceView *>, FRHIShaderResourceView *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_UAV(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_UAV, TShaderResourceParameterTypeInfo<FRHIUnorderedAccessView *>, FRHIUnorderedAccessView *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_SAMPLER(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_SAMPLER, TShaderResourceParameterTypeInfo<FRHISamplerState *>, FRHISamplerState *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_SAMPLER_ARRAY(ShaderType, MemberName, ArrayDecl) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_SAMPLER, TShaderResourceParameterTypeInfo<FRHISamplerState * ArrayDecl>, FRHISamplerState *, MemberName, ArrayDecl, , EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_TEXTURE(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE, TShaderResourceParameterTypeInfo<FRDGTexture *>, FRDGTexture *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_TEXTURE_ARRAY(ShaderType, MemberName, ArrayDecl) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE, TShaderResourceParameterTypeInfo<FRDGTexture * ArrayDecl>, FRDGTexture *, MemberName, ArrayDecl, , EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_TEXTURE_SRV(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE_SRV, TShaderResourceParameterTypeInfo<FRDGTextureSRV *>, FRDGTextureSRV *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_TEXTURE_SRV_ARRAY(ShaderType, MemberName, ArrayDecl) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE_SRV, TShaderResourceParameterTypeInfo<FRDGTextureSRV * ArrayDecl>, FRDGTextureSRV *, MemberName, ArrayDecl, , EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_TEXTURE_NON_PIXEL_SRV(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE_NON_PIXEL_SRV, TShaderResourceParameterTypeInfo<FRDGTextureSRV *>, FRDGTextureSRV *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_TEXTURE_UAV(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE_UAV, TShaderResourceParameterTypeInfo<FRDGTextureUAV *>, FRDGTextureUAV *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_TEXTURE_UAV_ARRAY(ShaderType, MemberName, ArrayDecl) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE_UAV, TShaderResourceParameterTypeInfo<FRDGTextureUAV * ArrayDecl>, FRDGTextureUAV *, MemberName, ArrayDecl, , EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_BUFFER_SRV(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_BUFFER_SRV, TShaderResourceParameterTypeInfo<FRDGBufferSRV *>, FRDGBufferSRV *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_BUFFER_SRV_ARRAY(ShaderType, MemberName, ArrayDecl) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_BUFFER_SRV, TShaderResourceParameterTypeInfo<FRDGBufferSRV * ArrayDecl>, FRDGBufferSRV *, MemberName, ArrayDecl, , EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_BUFFER_UAV(ShaderType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_BUFFER_UAV, TShaderResourceParameterTypeInfo<FRDGBufferUAV *>, FRDGBufferUAV *, MemberName, , = nullptr, EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_BUFFER_UAV_ARRAY(ShaderType, MemberName, ArrayDecl) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_BUFFER_UAV, TShaderResourceParameterTypeInfo<FRDGBufferUAV * ArrayDecl>, FRDGBufferUAV *, MemberName, ArrayDecl, , EShaderPrecisionModifier::Float, #ShaderType, false)

#define SHADER_PARAMETER_RDG_UNIFORM_BUFFER(StructType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_UNIFORM_BUFFER, TShaderParameterTypeInfo<TRDGUniformBufferBinding<StructType>>, TRDGUniformBufferBinding<StructType>, MemberName, , , EShaderPrecisionModifier::Float, #StructType, false)

#define SHADER_PARAMETER_STRUCT(StructType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_NESTED_STRUCT, StructType::FTypeInfo, StructType, MemberName, , , EShaderPrecisionModifier::Float, #StructType, true)

#define SHADER_PARAMETER_STRUCT_ARRAY(StructType, MemberName, ArrayDecl) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_NESTED_STRUCT, TShaderParameterStructArrayHelper<StructType ArrayDecl>::TypeInfo, StructType, MemberName, ArrayDecl, , EShaderPrecisionModifier::Float, #StructType, true)

#define SHADER_PARAMETER_STRUCT_INCLUDE(StructType, MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_INCLUDED_STRUCT, StructType::FTypeInfo, StructType, MemberName, , , EShaderPrecisionModifier::Float, #StructType, true)

#define RDG_BUFFER_ACCESS(MemberName, Access) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_BUFFER_ACCESS, TRDGResourceAccessTypeInfo<TRDGBufferAccess<Access>>, TRDGBufferAccess<Access>, MemberName, , , EShaderPrecisionModifier::Float, "", false)

#define RDG_BUFFER_ACCESS_DYNAMIC(MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_BUFFER_ACCESS, TRDGResourceAccessTypeInfo<FRDGBufferAccess>, FRDGBufferAccess, MemberName, , , EShaderPrecisionModifier::Float, "", false)

#define RDG_BUFFER_ACCESS_ARRAY(MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_BUFFER_ACCESS_ARRAY, TRDGResourceAccessTypeInfo<FRDGBufferAccessArray>, FRDGBufferAccessArray, MemberName, , , EShaderPrecisionModifier::Float, "", false)

#define RDG_TEXTURE_ACCESS(MemberName, Access) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE_ACCESS, TRDGTextureAccessTypeInfo<TRDGTextureAccess<Access>>, TRDGTextureAccess<Access>, MemberName, , , EShaderPrecisionModifier::Float, "", false)

#define RDG_TEXTURE_ACCESS_DYNAMIC(MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE_ACCESS, TRDGTextureAccessTypeInfo<FRDGTextureAccess>, FRDGTextureAccess, MemberName, , , EShaderPrecisionModifier::Float, "", false)

#define RDG_TEXTURE_ACCESS_ARRAY(MemberName) \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RDG_TEXTURE_ACCESS_ARRAY, TRDGResourceAccessTypeInfo<FRDGTextureAccessArray>, FRDGTextureAccessArray, MemberName, , , EShaderPrecisionModifier::Float, "", false)

#define RENDER_TARGET_BINDING_SLOTS() \
	INTERNAL_SHADER_PARAMETER_EXPLICIT(UBMT_RENDER_TARGET_BINDING_SLOTS, FRenderTargetBindingSlots::FTypeInfo, FRenderTargetBindingSlots, RenderTargets, , , EShaderPrecisionModifier::Float, "", false)

/** An empty shader parameter structure ready to be used anywhere. */
BEGIN_SHADER_PARAMETER_STRUCT(FEmptyShaderParameters, )
END_SHADER_PARAMETER_STRUCT()

/** Parameter struct that only has render targets. */
BEGIN_SHADER_PARAMETER_STRUCT(FRenderTargetParameters, )
RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

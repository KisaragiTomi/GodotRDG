#pragma once

#include "RHIResources.h"
#include "ShaderCore.h"
#include "ShaderParameterMacros.h"
#include "ShaderPermutation.h"

class FShader;
class FShaderType;
class FGlobalShaderMap;

struct FGlobalShaderPermutationParameters {
	const char *ShaderType = nullptr;
	EShaderPlatform Platform = SP_VULKAN_SM5;
	int32 PermutationId = 0;
	uint32 Flags = 0;

	FGlobalShaderPermutationParameters() = default;
	FGlobalShaderPermutationParameters(const char *InShaderType, EShaderPlatform InPlatform, int32 InPermutationId) :
			ShaderType(InShaderType), Platform(InPlatform), PermutationId(InPermutationId) {}
};

/** How a parameter struct member maps onto the shader's descriptors / push constants. */
struct FShaderParameterBindings {
	struct FResourceBinding {
		String Name;
		/** Offset inside the root parameter struct, or inside the uniform buffer contents when ParentUniformBuffer >= 0. */
		uint32 ByteOffset = 0;
		EUniformBufferBaseType BaseType = UBMT_INVALID;
		uint32 NumElements = 0;
		uint32 Set = 0;
		uint32 Binding = 0;
		RenderingDevice::UniformType UniformType = RenderingDevice::UNIFORM_TYPE_MAX;
		uint32 ArraySize = 1;
		/** For combined image samplers: offset of the "<Name>Sampler" member, UINT32_MAX for the default sampler. */
		uint32 SamplerByteOffset = UINT32_MAX;
		/** -1: the resource lives in the root struct. >= 0: root byte offset of the RDG uniform buffer member whose contents hold it. */
		int32 ParentUniformBuffer = -1;
		const FShaderParametersMetadata *UniformBufferStruct = nullptr;
		/** Uniform buffers: std140 copy table (C++ offset -> GLSL offset). */
		struct FConstantCopy {
			uint32 SrcOffset = 0;
			uint32 DstOffset = 0;
			uint32 Size = 0;
			uint32 NumElements = 0;
			uint32 SrcStride = 0;
			uint32 DstStride = 0;
			bool bBool = false;
		};
		LocalVector<FConstantCopy> UniformBufferCopies;
		uint32 UniformBufferSize = 0;
	};

	LocalVector<FResourceBinding> ResourceBindings;
	LocalVector<FResourceBinding::FConstantCopy> PushConstantCopies;
	uint32 PushConstantSize = 0;
	/** Shader descriptors that no parameter feeds; binding them fails, so they are reported as errors. */
	LocalVector<String> UnboundResources;
	bool bBound = false;

	void Bind(const FShaderParametersMetadata *RootParametersMetadata, const FShaderReflectionInfo &Reflection, const String &ShaderName);
	bool IsResourceBound(uint32 RootByteOffset) const;
};

/** Compiled shader data shared by FShader and the RHI shader handle. */
struct FShaderCompiledData {
	RenderingDevice *Device = nullptr;
	String DebugName;
	EShaderFrequency Frequency = SF_Compute;
	Vector<uint8_t> SPIRV;
	FShaderReflectionInfo Reflection;
	/** Compute: the RD shader and pipeline. Graphics stages are combined per (VS, PS) pair by the device context. */
	RID ShaderRID;
	RID ComputePipeline;
};

/** UE's FShader. Instances are created by the shader type when a permutation is first requested. */
class FShader {
public:
	struct CompiledShaderInitializerType {
		const FShaderType *Type = nullptr;
		int32 PermutationId = 0;
		FShaderCompiledData *CompiledData = nullptr;
	};

	using FPermutationDomain = FShaderPermutationNone;
	using FPermutationParameters = FGlobalShaderPermutationParameters;

	FShader() = default;
	explicit FShader(const CompiledShaderInitializerType &Initializer);
	virtual ~FShader();

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters &) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters &, FShaderCompilerEnvironment &) {}
	static const FShaderParametersMetadata *GetRootParametersMetadata() { return nullptr; }

	const FShaderType *GetType() const { return Type; }
	int32 GetPermutationId() const { return PermutationId; }
	EShaderFrequency GetFrequency() const { return CompiledData ? CompiledData->Frequency : SF_Compute; }
	const FShaderCompiledData *GetCompiledData() const { return CompiledData; }
	const FShaderReflectionInfo &GetReflection() const { return CompiledData->Reflection; }
	const FShaderParameterBindings &GetParameterBindings() const { return Bindings; }
	const String &GetDebugName() const { return CompiledData->DebugName; }

	FRHIShader *GetRHIShader() const { return RHIShader; }

	/** Builds the name based bindings between the root parameter struct and the reflected shader interface. */
	void BindForRootShaderParameters(const FShaderParametersMetadata *ParametersMetadata);

private:
	const FShaderType *Type = nullptr;
	int32 PermutationId = 0;
	FShaderCompiledData *CompiledData = nullptr;
	TRefCountPtr<FRHIShader> RHIShader;
	FShaderParameterBindings Bindings;
};

/** Static description of a shader class (UE's FShaderType / FGlobalShaderType). */
class FShaderType {
public:
	using FConstructCompiledType = FShader *(*)(const FShader::CompiledShaderInitializerType &);
	using FShouldCompilePermutationType = bool (*)(const FGlobalShaderPermutationParameters &);
	using FModifyCompilationEnvironmentType = void (*)(const FGlobalShaderPermutationParameters &, FShaderCompilerEnvironment &);
	using FRootParametersMetadataType = const FShaderParametersMetadata *(*)();

	FShaderType(const char *InName, const char *InSourceFilename, const char *InFunctionName, EShaderFrequency InFrequency, int32 InTotalPermutationCount,
			FConstructCompiledType InConstructCompiledRef, FModifyCompilationEnvironmentType InModifyCompilationEnvironmentRef,
			FShouldCompilePermutationType InShouldCompilePermutationRef, FRootParametersMetadataType InRootParametersMetadataRef);

	const char *GetName() const { return Name; }
	const char *GetShaderFilename() const { return SourceFilename; }
	const char *GetFunctionName() const { return FunctionName; }
	EShaderFrequency GetFrequency() const { return Frequency; }
	int32 GetPermutationCount() const { return TotalPermutationCount; }
	const FShaderParametersMetadata *GetRootParametersMetadata() const { return RootParametersMetadataRef ? RootParametersMetadataRef() : nullptr; }

	bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters &Parameters) const { return ShouldCompilePermutationRef(Parameters); }
	void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters &Parameters, FShaderCompilerEnvironment &Environment) const { ModifyCompilationEnvironmentRef(Parameters, Environment); }
	FShader *ConstructCompiled(const FShader::CompiledShaderInitializerType &Initializer) const { return ConstructCompiledRef(Initializer); }

	/** Global list of all shader types created with IMPLEMENT_GLOBAL_SHADER. */
	static const LocalVector<FShaderType *> &GetTypeList();
	static FShaderType *FindShaderTypeByName(const char *Name);

private:
	const char *Name;
	const char *SourceFilename;
	const char *FunctionName;
	EShaderFrequency Frequency;
	int32 TotalPermutationCount;
	FConstructCompiledType ConstructCompiledRef;
	FModifyCompilationEnvironmentType ModifyCompilationEnvironmentRef;
	FShouldCompilePermutationType ShouldCompilePermutationRef;
	FRootParametersMetadataType RootParametersMetadataRef;

	static LocalVector<FShaderType *> &GetTypeListMutable();
};

using FGlobalShaderType = FShaderType;

/** UE's TShaderRef: typed handle to a compiled shader permutation. */
template <typename ShaderType>
class TShaderRef {
public:
	TShaderRef() = default;
	TShaderRef(ShaderType *InShader) :
			ShaderContent(InShader) {}
	template <typename OtherShaderType, typename = std::enable_if_t<std::is_convertible_v<OtherShaderType *, ShaderType *>>>
	TShaderRef(const TShaderRef<OtherShaderType> &Rhs) :
			ShaderContent(Rhs.GetShader()) {}

	static TShaderRef<ShaderType> Cast(const TShaderRef<FShader> &Rhs) { return TShaderRef<ShaderType>(static_cast<ShaderType *>(Rhs.GetShader())); }

	bool IsValid() const { return ShaderContent != nullptr; }
	bool IsNull() const { return ShaderContent == nullptr; }
	explicit operator bool() const { return IsValid(); }
	ShaderType *GetShader() const { return ShaderContent; }
	ShaderType *operator->() const { return ShaderContent; }
	ShaderType &operator*() const { return *ShaderContent; }
	bool operator==(const TShaderRef &Rhs) const { return ShaderContent == Rhs.ShaderContent; }
	bool operator!=(const TShaderRef &Rhs) const { return ShaderContent != Rhs.ShaderContent; }

	FRHIShader *GetRHIShaderBase(EShaderFrequency) const { return ShaderContent ? ShaderContent->GetRHIShader() : nullptr; }
	FRHIComputeShader *GetComputeShader() const { return static_cast<FRHIComputeShader *>(GetRHIShaderBase(SF_Compute)); }
	FRHIVertexShader *GetVertexShader() const { return static_cast<FRHIVertexShader *>(GetRHIShaderBase(SF_Vertex)); }
	FRHIPixelShader *GetPixelShader() const { return static_cast<FRHIPixelShader *>(GetRHIShaderBase(SF_Pixel)); }

private:
	ShaderType *ShaderContent = nullptr;
};

/** UE's DECLARE_GLOBAL_SHADER / DECLARE_SHADER_TYPE. */
#define DECLARE_GLOBAL_SHADER(ShaderClass)                                                                       \
public:                                                                                                          \
	using ShaderMetaType = FGlobalShaderType;                                                                    \
	using ShaderMapType = FGlobalShaderMap;                                                                      \
	static ShaderMetaType &GetStaticType();                                                                      \
	static FShader *ConstructCompiledInstance(const FShader::CompiledShaderInitializerType &Initializer) {       \
		return memnew(ShaderClass(Initializer));                                                                 \
	}                                                                                                            \
	/* Like UE, the permutation domain sets its defines before the class' own ModifyCompilationEnvironment. */  \
	static void ModifyCompilationEnvironmentImpl(const FGlobalShaderPermutationParameters &Parameters, FShaderCompilerEnvironment &OutEnvironment) { \
		const ShaderClass::FPermutationDomain PermutationVector(Parameters.PermutationId);                       \
		PermutationVector.ModifyCompilationEnvironment(OutEnvironment);                                          \
		ShaderClass::ModifyCompilationEnvironment(Parameters, OutEnvironment);                                   \
	}                                                                                                            \
	static bool ShouldCompilePermutationImpl(const FGlobalShaderPermutationParameters &Parameters) {             \
		return ShaderClass::ShouldCompilePermutation(Parameters);                                                \
	}

#define DECLARE_EXPORTED_GLOBAL_SHADER(ShaderClass, RequiredAPI) DECLARE_GLOBAL_SHADER(ShaderClass)
#define DECLARE_SHADER_TYPE(ShaderClass, ShaderMetaTypeShortcut) DECLARE_GLOBAL_SHADER(ShaderClass)
#define DECLARE_EXPORTED_SHADER_TYPE(ShaderClass, ShaderMetaTypeShortcut, RequiredAPI) DECLARE_GLOBAL_SHADER(ShaderClass)

/** Binds the shader's FParameters struct to the reflected GLSL interface by name. */
#define SHADER_USE_PARAMETER_STRUCT(ShaderClass, ShaderParentClass)                                            \
	ShaderClass(const FShader::CompiledShaderInitializerType &Initializer) :                                   \
			ShaderParentClass(Initializer) {                                                                   \
		BindForRootShaderParameters(FParameters::FTypeInfo::GetStructMetadata());                              \
	}                                                                                                          \
	ShaderClass() {}                                                                                           \
	static const FShaderParametersMetadata *GetRootParametersMetadata() { return FParameters::FTypeInfo::GetStructMetadata(); }

#define SHADER_USE_ROOT_PARAMETER_STRUCT(ShaderClass, ShaderParentClass) SHADER_USE_PARAMETER_STRUCT(ShaderClass, ShaderParentClass)
#define SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(ShaderClass, ShaderParentClass) SHADER_USE_PARAMETER_STRUCT(ShaderClass, ShaderParentClass)

/** Shader without parameter struct (legacy style); still compiles and can be used for e.g. vertex stages. */
#define SHADER_USE_NO_PARAMETER_STRUCT(ShaderClass, ShaderParentClass)                                         \
	ShaderClass(const FShader::CompiledShaderInitializerType &Initializer) :                                   \
			ShaderParentClass(Initializer) {                                                                   \
		BindForRootShaderParameters(nullptr);                                                                  \
	}                                                                                                          \
	ShaderClass() {}

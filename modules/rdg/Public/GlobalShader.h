#pragma once

#include "core/os/mutex.h"

// UE's global shader system on top of Godot's GLSL -> SPIR-V compiler (glslang).
//
//	class FMyCS : public FGlobalShader {
//		DECLARE_GLOBAL_SHADER(FMyCS);
//		SHADER_USE_PARAMETER_STRUCT(FMyCS, FGlobalShader);
//		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//			...
//		END_SHADER_PARAMETER_STRUCT()
//	};
//	IMPLEMENT_GLOBAL_SHADER(FMyCS, "/Project/MyShader.glsl", "MainCS", SF_Compute);
//
// Virtual shader paths:
//	"/Engine/..."  -> shaders embedded in the rdg module (modules/rdg/Shaders/...)
//	"/RDGTests/..." -> embedded test shaders (modules/rdg/Tests/Shaders/...)
//	"/Project/..."  -> res://shaders/...
//	"res://", "user://" paths are used as-is.
//
// The entry point is selected by `#define <FunctionName> main`, and the stage defines COMPUTESHADER / VERTEXSHADER /
// PIXELSHADER are set like in UE, so one file can hold several entry points.

#include "Shader.h"

class FGlobalShader : public FShader {
public:
	using FPermutationDomain = FShaderPermutationNone;

	FGlobalShader() = default;
	explicit FGlobalShader(const CompiledShaderInitializerType &Initializer) :
			FShader(Initializer) {}
};

/** Registers shader types at static initialization time. */
class FShaderTypeRegistration {
public:
	using FGetter = FShaderType &(*)();
	explicit FShaderTypeRegistration(FGetter InGetter);
	static void CommitAll();

private:
	FGetter Getter;
	FShaderTypeRegistration *Next = nullptr;
	static FShaderTypeRegistration *&GetHead();
};

#define IMPLEMENT_GLOBAL_SHADER(ShaderClass, SourceFilename, FunctionName, Frequency)                                         \
	ShaderClass::ShaderMetaType &ShaderClass::GetStaticType() {                                                               \
		static ShaderClass::ShaderMetaType StaticType(                                                                        \
				#ShaderClass, SourceFilename, FunctionName, Frequency, ShaderClass::FPermutationDomain::PermutationCount,     \
				ShaderClass::ConstructCompiledInstance, ShaderClass::ModifyCompilationEnvironmentImpl,                         \
				ShaderClass::ShouldCompilePermutationImpl, ShaderClass::GetRootParametersMetadata);                               \
		return StaticType;                                                                                                    \
	}                                                                                                                         \
	static FShaderTypeRegistration zzShaderTypeRegistration##ShaderClass(&ShaderClass::GetStaticType);

#define IMPLEMENT_SHADER_TYPE(TemplatePrefix, ShaderClass, SourceFilename, FunctionName, Frequency) \
	IMPLEMENT_GLOBAL_SHADER(ShaderClass, SourceFilename, FunctionName, Frequency)

/** Per RenderingDevice map of compiled global shader permutations. */
class FGlobalShaderMap {
public:
	explicit FGlobalShaderMap(RenderingDevice *InDevice) :
			Device(InDevice) {}
	~FGlobalShaderMap();

	/** Compiles the permutation on first use. Returns nullptr when compilation failed (error is logged once). */
	FShader *GetShader(const FShaderType *ShaderType, int32 PermutationId = 0) const;

	template <typename ShaderType>
	TShaderRef<ShaderType> GetShader(int32 PermutationId = 0) const {
		return TShaderRef<ShaderType>(static_cast<ShaderType *>(GetShader(&ShaderType::GetStaticType(), PermutationId)));
	}
	template <typename ShaderType>
	TShaderRef<ShaderType> GetShader(const typename ShaderType::FPermutationDomain &PermutationVector) const {
		return GetShader<ShaderType>(PermutationVector.ToDimensionValueId());
	}

	RenderingDevice *GetDevice() const { return Device; }
	/** Drops every compiled permutation so the next use recompiles from source (shader hot reload). */
	void Empty();
	uint32 GetNumCompiledShaders() const {
		MutexLock Lock(ShadersMutex);
		return Shaders.size();
	}

private:
	struct FKey {
		const FShaderType *Type = nullptr;
		int32 PermutationId = 0;
		bool operator==(const FKey &O) const { return Type == O.Type && PermutationId == O.PermutationId; }
	};
	struct FKeyHasher {
		static uint32 hash(const FKey &Key);
	};

	RenderingDevice *Device = nullptr;
	mutable HashMap<FKey, FShader *, FKeyHasher> Shaders;
	// Pass lambdas recorded by parallel tasks can look shaders up (and compile them) concurrently.
	mutable Mutex ShadersMutex;
};

/** UE signature: returns the global shader map of the main RenderingDevice. */
FGlobalShaderMap *GetGlobalShaderMap(ERHIFeatureLevel::Type FeatureLevel);
FGlobalShaderMap *GetGlobalShaderMap(EShaderPlatform Platform);
/** Godot: shader map of a specific (e.g. local) RenderingDevice. */
FGlobalShaderMap *GetGlobalShaderMap(RenderingDevice *Device);

/** UE's TShaderMapRef. */
template <typename ShaderType>
class TShaderMapRef : public TShaderRef<ShaderType> {
public:
	explicit TShaderMapRef(const FGlobalShaderMap *ShaderIndex) :
			TShaderRef<ShaderType>(ShaderIndex ? ShaderIndex->template GetShader<ShaderType>(0) : TShaderRef<ShaderType>()) {}
	TShaderMapRef(const FGlobalShaderMap *ShaderIndex, const typename ShaderType::FPermutationDomain &PermutationVector) :
			TShaderRef<ShaderType>(ShaderIndex ? ShaderIndex->template GetShader<ShaderType>(PermutationVector) : TShaderRef<ShaderType>()) {}
	TShaderMapRef(const FGlobalShaderMap *ShaderIndex, int32 PermutationId) :
			TShaderRef<ShaderType>(ShaderIndex ? ShaderIndex->template GetShader<ShaderType>(PermutationId) : TShaderRef<ShaderType>()) {}
};

/** Shader source helpers (exposed for tooling / tests). */
namespace RDGShaderSource {
/** Loads a virtual shader path. */
bool LoadShaderSourceFile(const String &VirtualPath, String &OutSource, String *OutError = nullptr);
/** Full preprocessed source for a type + permutation, as handed to glslang. */
bool BuildShaderSource(const FShaderType *ShaderType, int32 PermutationId, String &OutSource, String &OutError);
/** Registers an embedded shader file (used by the generated table). */
void RegisterEmbeddedShader(const char *VirtualPath, const char *Source);
} //namespace RDGShaderSource

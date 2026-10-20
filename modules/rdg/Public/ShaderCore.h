#pragma once

#include "RHIDefinitions.h"

#include "core/templates/hash_map.h"

/** Defines and flags handed to the GLSL compiler, like UE's FShaderCompilerEnvironment. */
class FShaderCompilerEnvironment {
public:
	void SetDefine(const char *Name, const char *Value) { Defines[String::utf8(Name)] = String::utf8(Value); }
	void SetDefine(const char *Name, const String &Value) { Defines[String::utf8(Name)] = Value; }
	void SetDefine(const char *Name, bool Value) { Defines[String::utf8(Name)] = Value ? "1" : "0"; }
	void SetDefine(const char *Name, int32 Value) { Defines[String::utf8(Name)] = itos(Value); }
	void SetDefine(const char *Name, uint32 Value) { Defines[String::utf8(Name)] = itos(Value); }
	void SetDefine(const char *Name, float Value) { Defines[String::utf8(Name)] = String::num(Value); }

	const HashMap<String, String> &GetDefinitions() const { return Defines; }

	/** UE compatibility: compiler flags are accepted and ignored (glslang decides). */
	struct FCompilerFlags {
		template <typename T>
		void Add(T) {}
	} CompilerFlags;

private:
	HashMap<String, String> Defines;
};

/** Reflection of one descriptor, as found in the SPIR-V (names included). */
struct FShaderResourceReflection {
	String Name;
	String BlockName;
	uint32 Set = 0;
	uint32 Binding = 0;
	RenderingDevice::UniformType UniformType = RenderingDevice::UNIFORM_TYPE_MAX;
	uint32 ArraySize = 1;
	bool bWritable = false;
	uint32 BlockSize = 0;

	/** Flattened members of a uniform buffer block (std140 offsets). */
	struct FMember {
		String Name;
		uint32 Offset = 0;
		uint32 Size = 0;
		uint32 ArrayCount = 0;
		uint32 ArrayStride = 0;
	};
	LocalVector<FMember> BlockMembers;
};

struct FShaderReflectionInfo {
	LocalVector<FShaderResourceReflection> Resources;
	uint32 PushConstantSize = 0;
	LocalVector<FShaderResourceReflection::FMember> PushConstantMembers;
	uint32 ComputeLocalSize[3] = { 1, 1, 1 };

	const FShaderResourceReflection *FindResource(const String &Name) const;
	const FShaderResourceReflection *FindResource(uint32 Set, uint32 Binding) const;
	const FShaderResourceReflection::FMember *FindPushConstant(const String &Name) const;
	uint32 GetNumSets() const;
};

/** Parses SPIR-V and returns descriptor / push constant reflection with names. */
bool ReflectShaderSPIRV(const Vector<uint8_t> &SPIRV, FShaderReflectionInfo &OutReflection, String &OutError);

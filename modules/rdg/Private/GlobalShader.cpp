#include "GlobalShader.h"

#include "HAL/IConsoleManager.h"
#include "RDGDeviceContext.h"
#include "RenderGraphResources.h"
#include "ShaderParameterStruct.h"

#include "core/io/file_access.h"
#include "core/templates/hash_set.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/rendering_device.h"

#include "thirdparty/spirv-reflect/spirv_reflect.h"

static TAutoConsoleVariable<int32> CVarRDGShaderCache(
		"r.RDG.ShaderCompileCache", 1,
		"Use Godot's SPIR-V cache when compiling RDG global shaders.",
		ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRDGDumpShaderSource(
		"r.RDG.DumpShaderSource", 0,
		"Prints the preprocessed GLSL of every RDG shader permutation when it is compiled.",
		ECVF_RenderThreadSafe);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Shader types.

LocalVector<FShaderType *> &FShaderType::GetTypeListMutable() {
	static LocalVector<FShaderType *> TypeList;
	return TypeList;
}

const LocalVector<FShaderType *> &FShaderType::GetTypeList() {
	return GetTypeListMutable();
}

FShaderType *FShaderType::FindShaderTypeByName(const char *Name) {
	for (FShaderType *Type : GetTypeListMutable()) {
		if (strcmp(Type->GetName(), Name) == 0) return Type;
	}
	return nullptr;
}

FShaderType::FShaderType(const char *InName, const char *InSourceFilename, const char *InFunctionName, EShaderFrequency InFrequency, int32 InTotalPermutationCount,
		FConstructCompiledType InConstructCompiledRef, FModifyCompilationEnvironmentType InModifyCompilationEnvironmentRef,
		FShouldCompilePermutationType InShouldCompilePermutationRef, FRootParametersMetadataType InRootParametersMetadataRef) :
		Name(InName), SourceFilename(InSourceFilename), FunctionName(InFunctionName), Frequency(InFrequency), TotalPermutationCount(InTotalPermutationCount), ConstructCompiledRef(InConstructCompiledRef), ModifyCompilationEnvironmentRef(InModifyCompilationEnvironmentRef), ShouldCompilePermutationRef(InShouldCompilePermutationRef), RootParametersMetadataRef(InRootParametersMetadataRef) {
	GetTypeListMutable().push_back(this);
}

FShaderTypeRegistration *&FShaderTypeRegistration::GetHead() {
	static FShaderTypeRegistration *Head = nullptr;
	return Head;
}

FShaderTypeRegistration::FShaderTypeRegistration(FGetter InGetter) :
		Getter(InGetter) {
	Next = GetHead();
	GetHead() = this;
}

void FShaderTypeRegistration::CommitAll() {
	for (FShaderTypeRegistration *It = GetHead(); It; It = It->Next) It->Getter();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Shader sources.

namespace {
struct FEmbeddedShader {
	const char *Path;
	const char *Source;
};

LocalVector<FEmbeddedShader> &GetEmbeddedShaders() {
	static LocalVector<FEmbeddedShader> Shaders;
	return Shaders;
}

String ResolveVirtualPath(const String &VirtualPath) {
	if (VirtualPath.begins_with("/Project/")) return "res://shaders/" + VirtualPath.substr(9);
	return VirtualPath;
}

String GetDirectory(const String &Path) {
	const int32 Slash = Path.rfind("/");
	return Slash >= 0 ? Path.substr(0, Slash + 1) : String();
}

String NormalizePath(const String &Path) {
	// Collapses "a/b/../c" for embedded virtual paths.
	String Prefix;
	String Rest = Path;
	const int32 Scheme = Path.find("://");
	if (Scheme >= 0) {
		Prefix = Path.substr(0, Scheme + 3);
		Rest = Path.substr(Scheme + 3);
	}
	const bool bAbsolute = Rest.begins_with("/");
	Vector<String> Parts = Rest.split("/", false);
	Vector<String> Out;
	for (const String &Part : Parts) {
		if (Part == ".") continue;
		if (Part == ".." && !Out.is_empty()) {
			Out.remove_at(Out.size() - 1);
			continue;
		}
		Out.push_back(Part);
	}
	return Prefix + (bAbsolute ? "/" : "") + String("/").join(Out);
}

bool Preprocess(const String &Path, const String &Source, HashSet<String> &IncludedFiles, String &OutSource, String &OutError, int32 Depth) {
	if (Depth > 32) {
		OutError = "Include depth limit reached in " + Path;
		return false;
	}
	const Vector<String> Lines = Source.split("\n");
	int32 LineNumber = 0;
	for (const String &RawLine : Lines) {
		LineNumber++;
		const String Line = RawLine.strip_edges();
		if (Line.begins_with("#pragma once")) continue;
		if (Line.begins_with("#version")) continue;
		if (Line.begins_with("#include")) {
			const int32 Open = Line.find("\"");
			const int32 Close = Line.rfind("\"");
			if (Open < 0 || Close <= Open) {
				OutError = vformat("%s:%d: malformed #include.", Path, LineNumber);
				return false;
			}
			String IncludePath = Line.substr(Open + 1, Close - Open - 1);
			if (!IncludePath.begins_with("/") && !IncludePath.contains("://")) IncludePath = GetDirectory(Path) + IncludePath;
			IncludePath = NormalizePath(IncludePath);
			if (IncludedFiles.has(IncludePath)) {
				OutSource += "\n";
				continue;
			}
			IncludedFiles.insert(IncludePath);
			String IncludeSource;
			String LoadError;
			if (!RDGShaderSource::LoadShaderSourceFile(IncludePath, IncludeSource, &LoadError)) {
				OutError = vformat("%s:%d: %s", Path, LineNumber, LoadError);
				return false;
			}
			OutSource += vformat("#line 1 \"%s\"\n", IncludePath);
			if (!Preprocess(IncludePath, IncludeSource, IncludedFiles, OutSource, OutError, Depth + 1)) return false;
			OutSource += vformat("#line %d \"%s\"\n", LineNumber + 1, Path);
			continue;
		}
		OutSource += RawLine.trim_suffix("\r") + "\n";
	}
	return true;
}

const char *GetStageDefine(EShaderFrequency Frequency) {
	switch (Frequency) {
		case SF_Vertex:
			return "VERTEXSHADER";
		case SF_Pixel:
			return "PIXELSHADER";
		default:
			return "COMPUTESHADER";
	}
}

RenderingDevice::ShaderStage ToRDStage(EShaderFrequency Frequency) {
	switch (Frequency) {
		case SF_Vertex:
			return RenderingDevice::SHADER_STAGE_VERTEX;
		case SF_Pixel:
			return RenderingDevice::SHADER_STAGE_FRAGMENT;
		default:
			return RenderingDevice::SHADER_STAGE_COMPUTE;
	}
}
} //namespace

void RDGShaderSource::RegisterEmbeddedShader(const char *VirtualPath, const char *Source) {
	FEmbeddedShader Shader;
	Shader.Path = VirtualPath;
	Shader.Source = Source;
	GetEmbeddedShaders().push_back(Shader);
}

bool RDGShaderSource::LoadShaderSourceFile(const String &VirtualPath, String &OutSource, String *OutError) {
	for (const FEmbeddedShader &Shader : GetEmbeddedShaders()) {
		if (VirtualPath == Shader.Path) {
			OutSource = String::utf8(Shader.Source);
			return true;
		}
	}
	const String Path = ResolveVirtualPath(VirtualPath);
	if (Path.begins_with("res://") || Path.begins_with("user://") || Path.is_absolute_path()) {
		Error Err = OK;
		OutSource = FileAccess::get_file_as_string(Path, &Err);
		if (Err == OK) return true;
	}
	if (OutError) *OutError = vformat("Can't open shader file '%s' (resolved to '%s').", VirtualPath, Path);
	return false;
}

bool RDGShaderSource::BuildShaderSource(const FShaderType *ShaderType, int32 PermutationId, String &OutSource, String &OutError) {
	const String Path = String::utf8(ShaderType->GetShaderFilename());
	String Source;
	if (!LoadShaderSourceFile(Path, Source, &OutError)) return false;

	FShaderCompilerEnvironment Environment;
	Environment.SetDefine(GetStageDefine(ShaderType->GetFrequency()), 1);
	Environment.SetDefine("RDG_GODOT", 1);
	const FGlobalShaderPermutationParameters Parameters(ShaderType->GetName(), GMaxRHIShaderPlatform, PermutationId);
	ShaderType->ModifyCompilationEnvironment(Parameters, Environment);

	// Samplerless texelFetch / textureSize on texture2D mirrors HLSL Texture.Load() / GetDimensions().
	OutSource = "#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : enable\n#extension GL_EXT_samplerless_texture_functions : enable\n";
	for (const KeyValue<String, String> &It : Environment.GetDefinitions()) OutSource += "#define " + It.key + " " + It.value + "\n";
	const String EntryPoint = String::utf8(ShaderType->GetFunctionName());
	if (EntryPoint != "main") OutSource += "#define " + EntryPoint + " main\n";
	OutSource += vformat("#line 1 \"%s\"\n", Path);

	HashSet<String> IncludedFiles;
	IncludedFiles.insert(Path);
	return Preprocess(Path, Source, IncludedFiles, OutSource, OutError, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reflection.

const FShaderResourceReflection *FShaderReflectionInfo::FindResource(const String &Name) const {
	for (const FShaderResourceReflection &Resource : Resources) {
		if (Resource.Name == Name) return &Resource;
	}
	for (const FShaderResourceReflection &Resource : Resources) {
		if (Resource.Name.is_empty() && Resource.BlockName == Name) return &Resource;
	}
	return nullptr;
}

const FShaderResourceReflection *FShaderReflectionInfo::FindResource(uint32 Set, uint32 Binding) const {
	for (const FShaderResourceReflection &Resource : Resources) {
		if (Resource.Set == Set && Resource.Binding == Binding) return &Resource;
	}
	return nullptr;
}

const FShaderResourceReflection::FMember *FShaderReflectionInfo::FindPushConstant(const String &Name) const {
	for (const FShaderResourceReflection::FMember &Member : PushConstantMembers) {
		if (Member.Name == Name) return &Member;
	}
	return nullptr;
}

uint32 FShaderReflectionInfo::GetNumSets() const {
	uint32 NumSets = 0;
	for (const FShaderResourceReflection &Resource : Resources) NumSets = FMath::Max(NumSets, Resource.Set + 1);
	return NumSets;
}

static void FlattenBlockMembers(const SpvReflectBlockVariable &Block, uint32 BaseOffset, const String &Prefix, LocalVector<FShaderResourceReflection::FMember> &OutMembers) {
	for (uint32 Index = 0; Index < Block.member_count; Index++) {
		const SpvReflectBlockVariable &Member = Block.members[Index];
		const String Name = Prefix + String::utf8(Member.name ? Member.name : "");
		const uint32 Offset = BaseOffset + Member.offset;
		uint32 ArrayCount = 0;
		for (uint32 Dim = 0; Dim < Member.array.dims_count; Dim++) ArrayCount = (ArrayCount ? ArrayCount : 1) * Member.array.dims[Dim];
		const bool bIsStruct = Member.member_count > 0 && Member.type_description && (Member.type_description->type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT);
		if (bIsStruct) {
			if (ArrayCount == 0) {
				FlattenBlockMembers(Member, Offset, Name + "_", OutMembers);
			} else {
				for (uint32 Element = 0; Element < ArrayCount; Element++) FlattenBlockMembers(Member, Offset + Element * Member.array.stride, Name + "_" + itos(Element) + "_", OutMembers);
			}
			continue;
		}
		FShaderResourceReflection::FMember Out;
		Out.Name = Name;
		Out.Offset = Offset;
		Out.ArrayCount = ArrayCount;
		Out.ArrayStride = Member.array.stride;
		Out.Size = ArrayCount > 0 && Member.array.stride > 0 ? Member.size / FMath::Max(1u, ArrayCount) : Member.size;
		if (ArrayCount > 0 && Member.array.stride > 0) Out.Size = FMath::Min(Out.Size, Member.array.stride);
		OutMembers.push_back(Out);
	}
}

bool ReflectShaderSPIRV(const Vector<uint8_t> &SPIRV, FShaderReflectionInfo &OutReflection, String &OutError) {
	SpvReflectShaderModule Module;
	SpvReflectResult Result = spvReflectCreateShaderModule2(SPV_REFLECT_MODULE_FLAG_NO_COPY, SPIRV.size(), SPIRV.ptr(), &Module);
	if (Result != SPV_REFLECT_RESULT_SUCCESS) {
		OutError = "SPIR-V reflection failed.";
		return false;
	}

	if (Module.entry_point_count > 0 && Module.shader_stage == SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT) {
		OutReflection.ComputeLocalSize[0] = Module.entry_points[0].local_size.x;
		OutReflection.ComputeLocalSize[1] = Module.entry_points[0].local_size.y;
		OutReflection.ComputeLocalSize[2] = Module.entry_points[0].local_size.z;
	}

	uint32 BindingCount = 0;
	spvReflectEnumerateDescriptorBindings(&Module, &BindingCount, nullptr);
	LocalVector<SpvReflectDescriptorBinding *> Bindings;
	Bindings.resize(BindingCount);
	if (BindingCount) spvReflectEnumerateDescriptorBindings(&Module, &BindingCount, Bindings.ptr());

	for (uint32 Index = 0; Index < BindingCount; Index++) {
		const SpvReflectDescriptorBinding &Binding = *Bindings[Index];
		FShaderResourceReflection Resource;
		Resource.Name = String::utf8(Binding.name ? Binding.name : "");
		if (Binding.type_description && Binding.type_description->type_name) Resource.BlockName = String::utf8(Binding.type_description->type_name);
		Resource.Set = Binding.set;
		Resource.Binding = Binding.binding;
		Resource.ArraySize = 1;
		for (uint32 Dim = 0; Dim < Binding.array.dims_count; Dim++) Resource.ArraySize *= Binding.array.dims[Dim];
		switch (Binding.descriptor_type) {
			case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_SAMPLER;
				break;
			case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
				break;
			case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_TEXTURE;
				break;
			case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_IMAGE;
				Resource.bWritable = !(Binding.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE);
				break;
			case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_TEXTURE_BUFFER;
				break;
			case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_IMAGE_BUFFER;
				Resource.bWritable = true;
				break;
			case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER;
				Resource.BlockSize = Binding.block.size;
				FlattenBlockMembers(Binding.block, 0, String(), Resource.BlockMembers);
				break;
			case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
				Resource.BlockSize = Binding.block.size;
				Resource.bWritable = !(Binding.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) && !(Binding.block.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE);
				break;
			case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
				Resource.UniformType = RenderingDevice::UNIFORM_TYPE_INPUT_ATTACHMENT;
				break;
			default:
				continue;
		}
		OutReflection.Resources.push_back(Resource);
	}

	uint32 PushConstantCount = 0;
	spvReflectEnumeratePushConstantBlocks(&Module, &PushConstantCount, nullptr);
	if (PushConstantCount > 0) {
		LocalVector<SpvReflectBlockVariable *> Blocks;
		Blocks.resize(PushConstantCount);
		spvReflectEnumeratePushConstantBlocks(&Module, &PushConstantCount, Blocks.ptr());
		OutReflection.PushConstantSize = Blocks[0]->size;
		FlattenBlockMembers(*Blocks[0], 0, String(), OutReflection.PushConstantMembers);
	}

	spvReflectDestroyShaderModule(&Module);
	return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Parameter bindings.

static bool IsTextureUniformType(RenderingDevice::UniformType Type) {
	return Type == RenderingDevice::UNIFORM_TYPE_TEXTURE || Type == RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE || Type == RenderingDevice::UNIFORM_TYPE_INPUT_ATTACHMENT;
}

static bool IsCompatible(EUniformBufferBaseType BaseType, RenderingDevice::UniformType UniformType) {
	switch (BaseType) {
		case UBMT_TEXTURE:
		case UBMT_RDG_TEXTURE:
		case UBMT_RDG_TEXTURE_SRV:
		case UBMT_RDG_TEXTURE_NON_PIXEL_SRV:
			return IsTextureUniformType(UniformType);
		case UBMT_SRV:
			return IsTextureUniformType(UniformType) || UniformType == RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
		case UBMT_UAV:
			return UniformType == RenderingDevice::UNIFORM_TYPE_IMAGE || UniformType == RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
		case UBMT_RDG_TEXTURE_UAV:
			return UniformType == RenderingDevice::UNIFORM_TYPE_IMAGE;
		case UBMT_RDG_BUFFER_SRV:
		case UBMT_RDG_BUFFER_UAV:
			return UniformType == RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
		case UBMT_SAMPLER:
			return UniformType == RenderingDevice::UNIFORM_TYPE_SAMPLER;
		case UBMT_RDG_UNIFORM_BUFFER:
			return UniformType == RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER;
		default:
			return false;
	}
}

static void BuildConstantCopies(const FShaderParametersMetadata &Metadata, const LocalVector<FShaderResourceReflection::FMember> &Members, LocalVector<FShaderParameterBindings::FResourceBinding::FConstantCopy> &OutCopies) {
	for (const FShaderParametersMetadata::FFlatParameter &Parameter : Metadata.GetLooseParameters()) {
		const FShaderResourceReflection::FMember *Target = nullptr;
		for (const FShaderResourceReflection::FMember &Member : Members) {
			if (Member.Name == Parameter.Name) {
				Target = &Member;
				break;
			}
		}
		if (!Target) continue; // Unused by the shader (optimized out or not declared), same as UE.
		FShaderParameterBindings::FResourceBinding::FConstantCopy Copy;
		Copy.SrcOffset = Parameter.ByteOffset;
		Copy.DstOffset = Target->Offset;
		Copy.NumElements = FMath::Max(1u, FMath::Min(FMath::Max(1u, Parameter.NumElements), FMath::Max(1u, Target->ArrayCount)));
		Copy.SrcStride = Parameter.ElementSize;
		Copy.DstStride = Target->ArrayStride ? Target->ArrayStride : Target->Size;
		Copy.bBool = Parameter.BaseType == UBMT_BOOL;
		Copy.Size = Copy.bBool ? 4 : FMath::Min(Parameter.ElementSize, Target->Size);
		OutCopies.push_back(Copy);
	}
}

void FShaderParameterBindings::Bind(const FShaderParametersMetadata *RootParametersMetadata, const FShaderReflectionInfo &Reflection, const String &ShaderName) {
	ResourceBindings.clear();
	PushConstantCopies.clear();
	UnboundResources.clear();
	PushConstantSize = Reflection.PushConstantSize;
	bBound = true;

	LocalVector<bool> Covered;
	Covered.resize(Reflection.Resources.size());
	for (uint32 Index = 0; Index < Covered.size(); Index++) Covered[Index] = false;

	auto MarkCovered = [&](const FShaderResourceReflection *Resource) {
		for (uint32 Index = 0; Index < Reflection.Resources.size(); Index++) {
			if (&Reflection.Resources[Index] == Resource) Covered[Index] = true;
		}
	};

	auto BindResource = [&](const FShaderParametersMetadata::FFlatParameter &Parameter, const String &Name, int32 ParentUniformBuffer) {
		const FShaderResourceReflection *Resource = Reflection.FindResource(Name);
		if (!Resource) return;
		if (!IsCompatible(Parameter.BaseType, Resource->UniformType)) {
			ERR_PRINT(vformat("[RDG] Shader '%s': parameter '%s' (%s) does not match the GLSL declaration (uniform type %d).", ShaderName, Name, GetShaderParameterMacroName(Parameter.BaseType), (int)Resource->UniformType));
			return;
		}
		FResourceBinding Binding;
		Binding.Name = Name;
		Binding.ByteOffset = Parameter.ByteOffset;
		Binding.BaseType = Parameter.BaseType;
		Binding.NumElements = Parameter.NumElements;
		Binding.Set = Resource->Set;
		Binding.Binding = Resource->Binding;
		Binding.UniformType = Resource->UniformType;
		Binding.ArraySize = Resource->ArraySize;
		Binding.ParentUniformBuffer = ParentUniformBuffer;
		if (Resource->UniformType == RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
			// UE convention: "<Texture>Sampler" provides the sampler of a combined image sampler.
			Binding.SamplerByteOffset = UINT32_MAX;
		}
		ResourceBindings.push_back(Binding);
		MarkCovered(Resource);
	};

	if (RootParametersMetadata) {
		const LocalVector<FShaderParametersMetadata::FFlatParameter> &Resources = RootParametersMetadata->GetResourceParameters();
		for (const FShaderParametersMetadata::FFlatParameter &Parameter : Resources) {
			if (Parameter.BaseType == UBMT_RENDER_TARGET_BINDING_SLOTS || Parameter.BaseType == UBMT_RDG_TEXTURE_ACCESS || Parameter.BaseType == UBMT_RDG_BUFFER_ACCESS || Parameter.BaseType == UBMT_RDG_TEXTURE_ACCESS_ARRAY || Parameter.BaseType == UBMT_RDG_BUFFER_ACCESS_ARRAY) continue;
			if (Parameter.BaseType == UBMT_RDG_UNIFORM_BUFFER) {
				const FShaderParametersMetadata *UBStruct = Parameter.UniformBufferStruct;
				const String UBName = UBStruct && UBStruct->GetShaderVariableName() ? String::utf8(UBStruct->GetShaderVariableName()) : Parameter.Name;
				const FShaderResourceReflection *Resource = Reflection.FindResource(UBName);
				int32 UBIndex = -1;
				if (Resource && Resource->UniformType == RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER) {
					FResourceBinding Binding;
					Binding.Name = UBName;
					Binding.ByteOffset = Parameter.ByteOffset;
					Binding.BaseType = UBMT_RDG_UNIFORM_BUFFER;
					Binding.Set = Resource->Set;
					Binding.Binding = Resource->Binding;
					Binding.UniformType = Resource->UniformType;
					Binding.UniformBufferStruct = UBStruct;
					Binding.UniformBufferSize = Resource->BlockSize;
					if (UBStruct) BuildConstantCopies(*UBStruct, Resource->BlockMembers, Binding.UniformBufferCopies);
					UBIndex = (int32)ResourceBindings.size();
					ResourceBindings.push_back(Binding);
					MarkCovered(Resource);
				}
				// Resources inside the uniform buffer are separate GLSL descriptors named "<UB>_<Member>".
				if (UBStruct) {
					for (const FShaderParametersMetadata::FFlatParameter &UBResource : UBStruct->GetResourceParameters()) {
						if (UBResource.BaseType == UBMT_RDG_UNIFORM_BUFFER || UBResource.BaseType == UBMT_RENDER_TARGET_BINDING_SLOTS) continue;
						FShaderParametersMetadata::FFlatParameter Nested = UBResource;
						const int32 Before = (int32)ResourceBindings.size();
						BindResource(Nested, UBName + "_" + UBResource.Name, -2);
						if ((int32)ResourceBindings.size() > Before) {
							// Remember which uniform buffer member holds this resource; ByteOffset is relative to the UB contents.
							ResourceBindings[Before].ParentUniformBuffer = (int32)Parameter.ByteOffset;
							ResourceBindings[Before].UniformBufferSize = UINT32_MAX;
						}
					}
				}
				continue;
			}
			BindResource(Parameter, Parameter.Name, -1);
		}

		// Combined image samplers: look up "<Name>Sampler" members.
		for (FResourceBinding &Binding : ResourceBindings) {
			if (Binding.UniformType != RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE || Binding.ParentUniformBuffer != -1) continue;
			const String SamplerName = Binding.Name + "Sampler";
			for (const FShaderParametersMetadata::FFlatParameter &Parameter : Resources) {
				if (Parameter.BaseType == UBMT_SAMPLER && Parameter.Name == SamplerName) Binding.SamplerByteOffset = Parameter.ByteOffset;
			}
		}

		LocalVector<FResourceBinding::FConstantCopy> Copies;
		BuildConstantCopies(*RootParametersMetadata, Reflection.PushConstantMembers, Copies);
		PushConstantCopies = Copies;
	}

	for (uint32 Index = 0; Index < Reflection.Resources.size(); Index++) {
		if (!Covered[Index]) {
			const FShaderResourceReflection &Resource = Reflection.Resources[Index];
			UnboundResources.push_back(vformat("%s (set %d, binding %d)", Resource.Name.is_empty() ? Resource.BlockName : Resource.Name, Resource.Set, Resource.Binding));
		}
	}
}

bool FShaderParameterBindings::IsResourceBound(uint32 RootByteOffset) const {
	for (const FResourceBinding &Binding : ResourceBindings) {
		if (Binding.ParentUniformBuffer == -1 && Binding.ByteOffset == RootByteOffset) return true;
		if (Binding.BaseType == UBMT_RDG_UNIFORM_BUFFER && Binding.ByteOffset == RootByteOffset) return true;
		if (Binding.ParentUniformBuffer >= 0 && (uint32)Binding.ParentUniformBuffer == RootByteOffset) return true;
	}
	return false;
}

void RDGPrivate::ClearUnusedGraphResourcesImpl(const FShaderParameterBindings *const *Bindings, uint32 NumBindings, const FShaderParametersMetadata *Metadata, void *Parameters, const void *const *ExcludeList, uint32 NumExclude) {
	if (!Metadata || !Parameters) return;
	uint8 *Base = reinterpret_cast<uint8 *>(Parameters);
	for (const FShaderParametersMetadata::FFlatParameter &Parameter : Metadata->GetResourceParameters()) {
		const EUniformBufferBaseType Type = Parameter.BaseType;
		const bool bClearable = Type == UBMT_RDG_TEXTURE || Type == UBMT_RDG_TEXTURE_SRV || Type == UBMT_RDG_TEXTURE_NON_PIXEL_SRV || Type == UBMT_RDG_TEXTURE_UAV || Type == UBMT_RDG_BUFFER_SRV || Type == UBMT_RDG_BUFFER_UAV || Type == UBMT_RDG_UNIFORM_BUFFER;
		if (!bClearable) continue;
		bool bUsed = false;
		for (uint32 Index = 0; Index < NumBindings && !bUsed; Index++) bUsed = Bindings[Index]->IsResourceBound(Parameter.ByteOffset);
		if (bUsed) continue;

		const uint32 Count = FMath::Max(1u, Parameter.NumElements);
		for (uint32 Element = 0; Element < Count; Element++) {
			void **Slot = reinterpret_cast<void **>(Base + Parameter.ByteOffset + Element * sizeof(void *));
			bool bExcluded = false;
			for (uint32 Exclude = 0; Exclude < NumExclude; Exclude++) {
				if (!*Slot) continue;
				FRDGResource *Resource = reinterpret_cast<FRDGResource *>(*Slot);
				if (ExcludeList[Exclude] == Resource) bExcluded = true;
				if (Type == UBMT_RDG_BUFFER_SRV && ExcludeList[Exclude] == reinterpret_cast<FRDGBufferSRV *>(*Slot)->GetParent()) bExcluded = true;
				if (Type == UBMT_RDG_BUFFER_UAV && ExcludeList[Exclude] == reinterpret_cast<FRDGBufferUAV *>(*Slot)->GetParent()) bExcluded = true;
			}
			if (!bExcluded) *Slot = nullptr;
		}
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// FShader.

FShader::FShader(const CompiledShaderInitializerType &Initializer) :
		Type(Initializer.Type), PermutationId(Initializer.PermutationId), CompiledData(Initializer.CompiledData) {
	FRHIShader *Shader = nullptr;
	switch (CompiledData->Frequency) {
		case SF_Vertex:
			Shader = new FRHIVertexShader();
			break;
		case SF_Pixel:
			Shader = new FRHIPixelShader();
			break;
		default:
			Shader = new FRHIComputeShader();
			break;
	}
	Shader->Frequency = CompiledData->Frequency;
	Shader->Shader = this;
	RHIShader = Shader;
}

FShader::~FShader() {
	if (RHIShader.IsValid()) RHIShader->Shader = nullptr;
	if (CompiledData) {
		RenderingDevice *Device = CompiledData->Device;
		if (Device) {
			if (CompiledData->ComputePipeline.is_valid() && Device->compute_pipeline_is_valid(CompiledData->ComputePipeline)) Device->free_rid(CompiledData->ComputePipeline);
			if (CompiledData->ShaderRID.is_valid()) Device->free_rid(CompiledData->ShaderRID);
		}
		memdelete(CompiledData);
	}
}

void FShader::BindForRootShaderParameters(const FShaderParametersMetadata *ParametersMetadata) {
	Bindings.Bind(ParametersMetadata, CompiledData->Reflection, CompiledData->DebugName);
	for (const String &Unbound : Bindings.UnboundResources) {
		ERR_PRINT(vformat("[RDG] Shader '%s' declares %s, but its parameter struct has no member with that name.", CompiledData->DebugName, Unbound));
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Global shader map.

uint32 FGlobalShaderMap::FKeyHasher::hash(const FKey &Key) {
	return hash_murmur3_one_64((uint64)(uintptr_t)Key.Type, hash_murmur3_one_32((uint32)Key.PermutationId));
}

FGlobalShaderMap::~FGlobalShaderMap() {
	Empty();
}

void FGlobalShaderMap::Empty() {
	for (KeyValue<FKey, FShader *> &It : Shaders) {
		if (It.value) memdelete(It.value);
	}
	Shaders.clear();
}

FShader *FGlobalShaderMap::GetShader(const FShaderType *ShaderType, int32 PermutationId) const {
	ERR_FAIL_NULL_V(ShaderType, nullptr);
	MutexLock Lock(ShadersMutex);
	FKey Key;
	Key.Type = ShaderType;
	Key.PermutationId = PermutationId;
	if (FShader **Existing = Shaders.getptr(Key)) return *Existing;

	// Failed compilations are cached as nullptr so the error is only reported once.
	Shaders.insert(Key, nullptr);
	ERR_FAIL_COND_V_MSG(PermutationId < 0 || PermutationId >= ShaderType->GetPermutationCount(), nullptr, vformat("[RDG] Shader '%s': permutation %d out of range (%d).", ShaderType->GetName(), PermutationId, ShaderType->GetPermutationCount()));
	const FGlobalShaderPermutationParameters Parameters(ShaderType->GetName(), GMaxRHIShaderPlatform, PermutationId);
	if (!ShaderType->ShouldCompilePermutation(Parameters)) {
		ERR_PRINT(vformat("[RDG] Shader '%s' permutation %d is not compiled (ShouldCompilePermutation returned false).", ShaderType->GetName(), PermutationId));
		return nullptr;
	}

	String Source;
	String Error;
	if (!RDGShaderSource::BuildShaderSource(ShaderType, PermutationId, Source, Error)) {
		ERR_PRINT(vformat("[RDG] Shader '%s': %s", ShaderType->GetName(), Error));
		return nullptr;
	}
	if (CVarRDGDumpShaderSource.GetValueOnRenderThread()) print_line(vformat("[RDG] ---- %s (permutation %d) ----\n%s", ShaderType->GetName(), PermutationId, Source));

	const RenderingDevice::ShaderStage Stage = ToRDStage(ShaderType->GetFrequency());
	String CompileError;
	const Vector<uint8_t> SPIRV = Device->shader_compile_spirv_from_source(Stage, Source, RenderingDevice::SHADER_LANGUAGE_GLSL, &CompileError, CVarRDGShaderCache.GetValueOnRenderThread() != 0);
	if (SPIRV.is_empty()) {
		ERR_PRINT(vformat("[RDG] Failed to compile shader '%s' (%s, entry point %s, permutation %d):\n%s", ShaderType->GetName(), ShaderType->GetShaderFilename(), ShaderType->GetFunctionName(), PermutationId, CompileError));
		return nullptr;
	}

	FShaderCompiledData *Data = memnew(FShaderCompiledData);
	Data->Device = Device;
	Data->Frequency = ShaderType->GetFrequency();
	Data->DebugName = PermutationId ? vformat("%s#%d", ShaderType->GetName(), PermutationId) : String::utf8(ShaderType->GetName());
	Data->SPIRV = SPIRV;
	if (!ReflectShaderSPIRV(SPIRV, Data->Reflection, Error)) {
		ERR_PRINT(vformat("[RDG] Shader '%s': %s", ShaderType->GetName(), Error));
		memdelete(Data);
		return nullptr;
	}

	if (Data->Frequency == SF_Compute) {
		Vector<RenderingDevice::ShaderStageSPIRVData> Stages;
		RenderingDevice::ShaderStageSPIRVData StageData;
		StageData.shader_stage = Stage;
		StageData.spirv = SPIRV;
		Stages.push_back(StageData);
		Data->ShaderRID = Device->shader_create_from_spirv(Stages, Data->DebugName);
		if (Data->ShaderRID.is_valid()) Data->ComputePipeline = Device->compute_pipeline_create(Data->ShaderRID);
		if (!Data->ComputePipeline.is_valid()) {
			ERR_PRINT(vformat("[RDG] Failed to create compute pipeline for '%s'.", Data->DebugName));
			if (Data->ShaderRID.is_valid()) Device->free_rid(Data->ShaderRID);
			memdelete(Data);
			return nullptr;
		}
	}

	FShader::CompiledShaderInitializerType Initializer;
	Initializer.Type = ShaderType;
	Initializer.PermutationId = PermutationId;
	Initializer.CompiledData = Data;
	FShader *Shader = ShaderType->ConstructCompiled(Initializer);
	Shaders[Key] = Shader;
	print_verbose(vformat("[RDG] Compiled %s (%d bytes SPIR-V).", Data->DebugName, SPIRV.size()));
	return Shader;
}

FGlobalShaderMap *GetGlobalShaderMap(RenderingDevice *Device) {
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Device);
	return Context ? &Context->GlobalShaderMap : nullptr;
}

FGlobalShaderMap *GetGlobalShaderMap(ERHIFeatureLevel::Type) {
	return GetGlobalShaderMap(FRDGDeviceContext::GetMainDevice());
}

FGlobalShaderMap *GetGlobalShaderMap(EShaderPlatform) {
	return GetGlobalShaderMap(FRDGDeviceContext::GetMainDevice());
}

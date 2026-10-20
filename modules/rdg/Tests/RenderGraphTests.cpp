#include "RenderGraphTests.h"

#include "PixelShaderUtils.h"
#include "RDGDeviceContext.h"
#include "RenderGraph.h"
#include "RenderGraphPrivate.h"

#include "core/io/file_access.h"
#include "core/object/worker_thread_pool.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "servers/rendering/rendering_server.h"

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Test shaders (sources in modules/rdg/Tests/Shaders, served as /RDGTests/...).

class FTestAddValueCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestAddValueCS);
	SHADER_USE_PARAMETER_STRUCT(FTestAddValueCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FIntPoint, Size)
	SHADER_PARAMETER(float, Value)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestAddValueCS, "/RDGTests/RDGTestCompute.glsl", "AddValueCS", SF_Compute);

class FTestFillCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestFillCS);
	SHADER_USE_PARAMETER_STRUCT(FTestFillCS, FGlobalShader);

	class FUseOffset : SHADER_PERMUTATION_BOOL("USE_OFFSET");
	using FPermutationDomain = TShaderPermutationDomain<FUseOffset>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FIntPoint, Size)
	SHADER_PARAMETER(float, Value)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestFillCS, "/RDGTests/RDGTestCompute.glsl", "FillCS", SF_Compute);

/** Same GLSL as FTestFillCS, but the parameter struct carries a texture the shader never reads. */
class FTestFillWithUnusedCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestFillWithUnusedCS);
	SHADER_USE_PARAMETER_STRUCT(FTestFillWithUnusedCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FIntPoint, Size)
	SHADER_PARAMETER(float, Value)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, UnusedTexture)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestFillWithUnusedCS, "/RDGTests/RDGTestCompute.glsl", "FillCS", SF_Compute);

class FTestBufferMadCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestBufferMadCS);
	SHADER_USE_PARAMETER_STRUCT(FTestBufferMadCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(uint32, NumElements)
	SHADER_PARAMETER(float, Scale)
	SHADER_PARAMETER(float, Bias)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InputBuffer)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, OutputBuffer)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestBufferMadCS, "/RDGTests/RDGTestCompute.glsl", "BufferMadCS", SF_Compute);

class FTestDownsampleCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestDownsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FTestDownsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FIntPoint, DestSize)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SourceMip)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DestMip)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestDownsampleCS, "/RDGTests/RDGTestCompute.glsl", "DownsampleCS", SF_Compute);

class FTestWriteIndirectArgsCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestWriteIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FTestWriteIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(uint32, GroupCountX)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestWriteIndirectArgsCS, "/RDGTests/RDGTestCompute.glsl", "WriteIndirectArgsCS", SF_Compute);

class FTestCountThreadsCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestCountThreadsCS);
	SHADER_USE_PARAMETER_STRUCT(FTestCountThreadsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CounterBuffer)
	RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestCountThreadsCS, "/RDGTests/RDGTestCompute.glsl", "CountThreadsCS", SF_Compute);

BEGIN_UNIFORM_BUFFER_STRUCT(FTestUniformBuffer, )
SHADER_PARAMETER(FVector4f, Scale)
SHADER_PARAMETER(float, Offset)
SHADER_PARAMETER(int32, Mode)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SourceTexture)
END_UNIFORM_BUFFER_STRUCT()
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FTestUniformBuffer, "TestUB");

class FTestUniformBufferCS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestUniformBufferCS);
	SHADER_USE_PARAMETER_STRUCT(FTestUniformBufferCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FIntPoint, Size)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FTestUniformBuffer, TestUB)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestUniformBufferCS, "/RDGTests/RDGTestCompute.glsl", "UniformBufferCS", SF_Compute);

class FTestSolidColorPS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestSolidColorPS);
	SHADER_USE_PARAMETER_STRUCT(FTestSolidColorPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER(FLinearColor, Color)
	RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FTestSolidColorPS, "/RDGTests/RDGTestRaster.glsl", "SolidColorPS", SF_Pixel);

class FTestMeshVS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestMeshVS);
	SHADER_USE_NO_PARAMETER_STRUCT(FTestMeshVS, FGlobalShader);
};
IMPLEMENT_GLOBAL_SHADER(FTestMeshVS, "/RDGTests/RDGTestRaster.glsl", "MeshVS", SF_Vertex);

class FTestVertexColorPS : public FGlobalShader {
	DECLARE_GLOBAL_SHADER(FTestVertexColorPS);
	SHADER_USE_NO_PARAMETER_STRUCT(FTestVertexColorPS, FGlobalShader);
};
IMPLEMENT_GLOBAL_SHADER(FTestVertexColorPS, "/RDGTests/RDGTestRaster.glsl", "VertexColorPS", SF_Pixel);

BEGIN_SHADER_PARAMETER_STRUCT(FTestMeshPassParameters, )
RDG_BUFFER_ACCESS(VertexBuffer, ERHIAccess::VertexOrIndexBuffer)
RDG_BUFFER_ACCESS(InstanceBuffer, ERHIAccess::VertexOrIndexBuffer)
RDG_BUFFER_ACCESS(IndexBuffer, ERHIAccess::VertexOrIndexBuffer)
RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Metadata test structs.

BEGIN_SHADER_PARAMETER_STRUCT(FTestNestedParams, )
SHADER_PARAMETER(float, Intensity)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NestedTexture)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FTestIncludedParams, )
SHADER_PARAMETER(FVector2f, InvSize)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FTestMetadataParams, )
SHADER_PARAMETER(float, A)
SHADER_PARAMETER(FVector4f, B)
SHADER_PARAMETER_ARRAY(int32, C, [3])
SHADER_PARAMETER_STRUCT(FTestNestedParams, Bloom)
SHADER_PARAMETER_STRUCT_INCLUDE(FTestIncludedParams, Common)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, Output)
SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
RDG_TEXTURE_ACCESS(CopySource, ERHIAccess::CopySrc)
RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

struct FTestBlackboardData {
	int32 Value = 0;
};
RDG_REGISTER_BLACKBOARD_STRUCT(FTestBlackboardData);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Harness.

namespace {

struct FTestEnv {
	RenderingDevice *RD = nullptr;
	FRHICommandListImmediate *RHICmdList = nullptr;
	FGlobalShaderMap *ShaderMap = nullptr;
};

struct FTestResult {
	bool bPassed = true;
	bool bSkipped = false;
	String Message;
};

using FTestFunction = void (*)(FTestEnv &, FTestResult &);

struct FTestEntry {
	const char *Name;
	FTestFunction Function;
	bool bNeedsGPU;
};

#define RDG_TEST_CHECK(Condition, ...)          \
	if (!(Condition)) {                         \
		Result.bPassed = false;                 \
		Result.Message = vformat(__VA_ARGS__);  \
		return;                                 \
	}

/** Sets a console variable for the scope of a test. */
class FScopedCVar {
public:
	FScopedCVar(const char *Name, int32 Value) :
			Variable(IConsoleManager::Get().FindConsoleVariable(Name)) {
		if (!Variable) return;
		Previous = Variable->GetInt();
		Variable->Set(Value);
	}
	~FScopedCVar() {
		if (Variable) Variable->Set(Previous);
	}

private:
	IConsoleVariable *Variable = nullptr;
	int32 Previous = 0;
};

constexpr int32 kTestSize = 16;

FRDGTextureDesc MakeR32Desc(int32 Size = kTestSize, uint8 NumMips = 1) {
	return FRDGTextureDesc::Create2D(FIntPoint(Size, Size), PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV, NumMips);
}

void AddFillPass(FRDGBuilder &GraphBuilder, FTestEnv &Env, FRDGTextureRef Texture, float Value, bool bUseOffset = false, FRDGPassRef *OutPass = nullptr) {
	FTestFillCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FTestFillCS::FUseOffset>(bUseOffset);
	TShaderMapRef<FTestFillCS> ComputeShader(Env.ShaderMap, PermutationVector);
	FTestFillCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestFillCS::FParameters>();
	PassParameters->Size = Texture->Desc.Extent;
	PassParameters->Value = Value;
	PassParameters->OutputTexture = GraphBuilder.CreateUAV(Texture);
	FRDGPassRef Pass = FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Fill(%s, %.1f)", Texture->Name, Value), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(Texture->Desc.Extent, FComputeShaderUtils::kGolden2DGroupSize));
	if (OutPass) *OutPass = Pass;
}

FRDGPassRef AddAddValuePass(FRDGBuilder &GraphBuilder, FTestEnv &Env, FRDGTextureRef Input, FRDGTextureRef Output, float Value, ERDGPassFlags Flags = ERDGPassFlags::Compute) {
	TShaderMapRef<FTestAddValueCS> ComputeShader(Env.ShaderMap);
	FTestAddValueCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestAddValueCS::FParameters>();
	PassParameters->Size = Output->Desc.Extent;
	PassParameters->Value = Value;
	PassParameters->InputTexture = Input;
	PassParameters->OutputTexture = GraphBuilder.CreateUAV(Output);
	return FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("AddValue(%s -> %s, +%.1f)", Input->Name, Output->Name, Value), Flags, ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(Output->Desc.Extent, FComputeShaderUtils::kGolden2DGroupSize));
}

Vector<float> ReadTextureFloats(FTestEnv &Env, const TRefCountPtr<IPooledRenderTarget> &RenderTarget) {
	Vector<float> Result;
	if (!RenderTarget.IsValid()) return Result;
	const Vector<uint8_t> Data = Env.RD->texture_get_data(RenderTarget->GetRHI()->GetRID(), 0);
	Result.resize(Data.size() / 4);
	if (!Result.is_empty()) memcpy(Result.ptrw(), Data.ptr(), Result.size() * 4);
	return Result;
}

Vector<float> ReadBufferFloats(FTestEnv &Env, const TRefCountPtr<FRDGPooledBuffer> &Buffer) {
	Vector<float> Result;
	if (!Buffer.IsValid()) return Result;
	const Vector<uint8_t> Data = Env.RD->buffer_get_data(Buffer->GetRHI()->GetRID());
	Result.resize(Data.size() / 4);
	if (!Result.is_empty()) memcpy(Result.ptrw(), Data.ptr(), Result.size() * 4);
	return Result;
}

bool AllNear(const Vector<float> &Values, float Expected, uint32 Count, float Tolerance = 1e-4f, String *OutError = nullptr) {
	if ((uint32)Values.size() < Count) {
		if (OutError) *OutError = vformat("expected %d values, got %d", Count, Values.size());
		return false;
	}
	for (uint32 Index = 0; Index < Count; Index++) {
		if (Math::abs(Values[Index] - Expected) > Tolerance) {
			if (OutError) *OutError = vformat("value[%d] = %f, expected %f", Index, Values[Index], Expected);
			return false;
		}
	}
	return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tests.

void TestShaderParameterMetadata(FTestEnv &, FTestResult &Result) {
	const FShaderParametersMetadata *Metadata = FTestMetadataParams::FTypeInfo::GetStructMetadata();
	const LocalVector<FShaderParametersMetadata::FFlatParameter> &Loose = Metadata->GetLooseParameters();
	RDG_TEST_CHECK(Loose.size() == 5, "expected 5 loose parameters, got %d\n%s", Loose.size(), Metadata->ToString());
	const char *ExpectedNames[] = { "A", "B", "C", "Bloom_Intensity", "InvSize" };
	const uint32 ExpectedOffsets[] = {
		(uint32)offsetof(FTestMetadataParams, A),
		(uint32)offsetof(FTestMetadataParams, B),
		(uint32)offsetof(FTestMetadataParams, C),
		(uint32)(offsetof(FTestMetadataParams, Bloom) + offsetof(FTestNestedParams, Intensity)),
		(uint32)(offsetof(FTestMetadataParams, Common) + offsetof(FTestIncludedParams, InvSize)),
	};
	for (uint32 Index = 0; Index < 5; Index++) {
		RDG_TEST_CHECK(Loose[Index].Name == ExpectedNames[Index], "loose parameter %d is '%s', expected '%s'", Index, Loose[Index].Name, ExpectedNames[Index]);
		RDG_TEST_CHECK(Loose[Index].ByteOffset == ExpectedOffsets[Index], "'%s' offset %d, expected %d", Loose[Index].Name, Loose[Index].ByteOffset, ExpectedOffsets[Index]);
	}
	RDG_TEST_CHECK(Loose[2].NumElements == 3, "C should be an array of 3, got %d", Loose[2].NumElements);

	const LocalVector<FShaderParametersMetadata::FFlatParameter> &Resources = Metadata->GetResourceParameters();
	const char *ExpectedResources[] = { "Bloom_NestedTexture", "Output", "LinearSampler", "CopySource", "RenderTargets" };
	RDG_TEST_CHECK(Resources.size() == 5, "expected 5 resource parameters, got %d\n%s", Resources.size(), Metadata->ToString());
	for (uint32 Index = 0; Index < 5; Index++) RDG_TEST_CHECK(Resources[Index].Name == ExpectedResources[Index], "resource %d is '%s', expected '%s'", Index, Resources[Index].Name, ExpectedResources[Index]);
	RDG_TEST_CHECK(Metadata->HasRenderTargets(), "RENDER_TARGET_BINDING_SLOTS not detected");
	RDG_TEST_CHECK(Metadata->GetRenderTargetsOffset() == (uint32)offsetof(FTestMetadataParams, RenderTargets), "render target offset mismatch");
	RDG_TEST_CHECK(Metadata->HasGraphResources() && !Metadata->HasExternalOutputs(), "graph resource flags are wrong");
	RDG_TEST_CHECK(sizeof(FTestMetadataParams) == Metadata->GetSize(), "struct size mismatch");
	Result.Message = vformat("%d loose + %d resource members, size %d", Loose.size(), Resources.size(), Metadata->GetSize());
}

void TestBlackboard(FTestEnv &, FTestResult &Result) {
	FRDGBlackboard Blackboard;
	RDG_TEST_CHECK(Blackboard.Get<FTestBlackboardData>() == nullptr, "blackboard should start empty");
	Blackboard.Create<FTestBlackboardData>().Value = 42;
	RDG_TEST_CHECK(Blackboard.GetChecked<FTestBlackboardData>().Value == 42, "blackboard value lost");
	RDG_TEST_CHECK(String(FRDGBlackboard::GetStructName<FTestBlackboardData>()) == "FTestBlackboardData", "registered name is %s", FRDGBlackboard::GetStructName<FTestBlackboardData>());
}

void TestPermutationDomain(FTestEnv &, FTestResult &Result) {
	FTestFillCS::FPermutationDomain Vector;
	RDG_TEST_CHECK(FTestFillCS::FPermutationDomain::PermutationCount == 2, "permutation count %d", FTestFillCS::FPermutationDomain::PermutationCount);
	RDG_TEST_CHECK(Vector.ToDimensionValueId() == 0, "default permutation id should be 0");
	Vector.Set<FTestFillCS::FUseOffset>(true);
	RDG_TEST_CHECK(Vector.ToDimensionValueId() == 1 && Vector.Get<FTestFillCS::FUseOffset>(), "permutation id should be 1");
	FShaderCompilerEnvironment Environment;
	FTestFillCS::FPermutationDomain(1).ModifyCompilationEnvironment(Environment);
	const String *Define = Environment.GetDefinitions().getptr("USE_OFFSET");
	RDG_TEST_CHECK(Define && *Define == "1", "USE_OFFSET define missing");
	String Source;
	String Error;
	RDG_TEST_CHECK(RDGShaderSource::BuildShaderSource(&FTestFillCS::GetStaticType(), 1, Source, Error), "BuildShaderSource failed: %s", Error);
	RDG_TEST_CHECK(Source.contains("#define USE_OFFSET 1") && Source.contains("#define FillCS main") && Source.contains("#define COMPUTESHADER 1"), "preprocessed source is missing defines");
	RDG_TEST_CHECK(Source.contains("float Luminance("), "#include \"/Engine/Private/Common.glsl\" was not expanded");
}

void TestComputeChain(FTestEnv &Env, FTestResult &Result) {
	TRefCountPtr<IPooledRenderTarget> Output;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestComputeChain"));
		FRDGTextureRef A = GraphBuilder.CreateTexture(MakeR32Desc(), "A");
		FRDGTextureRef B = GraphBuilder.CreateTexture(MakeR32Desc(), "B");
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(A), 1.0f);
		AddAddValuePass(GraphBuilder, Env, A, B, 2.5f);
		GraphBuilder.QueueTextureExtraction(B, &Output);
		GraphBuilder.Execute();
		RDG_TEST_CHECK(GraphBuilder.GetStats().NumBindingErrors == 0, "binding errors: %d", GraphBuilder.GetStats().NumBindingErrors);
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Output), 3.5f, kTestSize * kTestSize, 1e-4f, &Error), "clear(1) + 2.5 != 3.5: %s", Error);
}

void TestImmediateMode(FTestEnv &Env, FTestResult &Result) {
	FScopedCVar Immediate("r.RDG.ImmediateMode", 1);
	TRefCountPtr<IPooledRenderTarget> Output;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestImmediateMode"));
		FRDGTextureRef A = GraphBuilder.CreateTexture(MakeR32Desc(), "A");
		FRDGTextureRef B = GraphBuilder.CreateTexture(MakeR32Desc(), "B");
		AddFillPass(GraphBuilder, Env, A, 4.0f);
		FRDGPassRef Pass = AddAddValuePass(GraphBuilder, Env, A, B, 1.0f);
		RDG_TEST_CHECK(Pass->GetFlags() == ERDGPassFlags::Compute, "unexpected pass flags");
		GraphBuilder.QueueTextureExtraction(B, &Output);
		GraphBuilder.Execute();
		RDG_TEST_CHECK(GraphBuilder.GetStats().bImmediateMode, "builder did not run in immediate mode");
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Output), 5.0f, kTestSize * kTestSize, 1e-4f, &Error), "immediate mode result wrong: %s", Error);
}

void TestPassCulling(FTestEnv &Env, FTestResult &Result) {
	TRefCountPtr<IPooledRenderTarget> Output;
	bool bNeverCullRan = false;
	FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestPassCulling"));
	FRDGTextureRef T1 = GraphBuilder.CreateTexture(MakeR32Desc(), "Extracted");
	FRDGTextureRef T2 = GraphBuilder.CreateTexture(MakeR32Desc(), "Unused");
	FRDGTextureRef T3 = GraphBuilder.CreateTexture(MakeR32Desc(), "DeadEnd");
	FRDGPassRef P1 = nullptr;
	FRDGPassRef P2 = nullptr;
	AddFillPass(GraphBuilder, Env, T1, 1.0f, false, &P1);
	AddFillPass(GraphBuilder, Env, T2, 2.0f, false, &P2);
	FRDGPassRef P3 = GraphBuilder.AddPass(RDG_EVENT_NAME("NeverCullCallback"), ERDGPassFlags::NeverCull, [&bNeverCullRan](FRHICommandList &) { bNeverCullRan = true; });
	FRDGPassRef P4 = AddAddValuePass(GraphBuilder, Env, T1, T3, 1.0f);
	GraphBuilder.QueueTextureExtraction(T1, &Output);
	GraphBuilder.Execute();

	RDG_TEST_CHECK(!P1->IsCulled(), "producer of an extracted texture was culled");
	RDG_TEST_CHECK(P2->IsCulled(), "pass writing an unused texture was not culled");
	RDG_TEST_CHECK(!P3->IsCulled() && bNeverCullRan, "NeverCull pass was culled");
	RDG_TEST_CHECK(P4->IsCulled(), "pass whose output is never consumed was not culled");
	RDG_TEST_CHECK(GraphBuilder.GetStats().NumCulledPasses == 2, "expected 2 culled passes, got %d", GraphBuilder.GetStats().NumCulledPasses);
	RDG_TEST_CHECK(!GraphBuilder.GetAllocatedTextureRID(T2).is_valid() && !GraphBuilder.GetAllocatedTextureRID(T3).is_valid(), "textures of culled passes were allocated");
	Result.Message = "2 of 4 passes culled; culled outputs never allocated";
}

void TestPassCullingDisabled(FTestEnv &Env, FTestResult &Result) {
	FScopedCVar Cull("r.RDG.CullPasses", 0);
	FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestPassCullingDisabled"));
	FRDGTextureRef T2 = GraphBuilder.CreateTexture(MakeR32Desc(), "Unused");
	FRDGPassRef Pass = nullptr;
	AddFillPass(GraphBuilder, Env, T2, 2.0f, false, &Pass);
	GraphBuilder.Execute();
	RDG_TEST_CHECK(!Pass->IsCulled() && GraphBuilder.GetStats().NumCulledPasses == 0, "r.RDG.CullPasses=0 still culled passes");
}

void TestClearUnusedGraphResources(FTestEnv &Env, FTestResult &Result) {
	TRefCountPtr<IPooledRenderTarget> Output;
	FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestClearUnused"));
	FRDGTextureRef Unused = GraphBuilder.CreateTexture(MakeR32Desc(), "UnusedInput");
	FRDGTextureRef Out = GraphBuilder.CreateTexture(MakeR32Desc(), "Output");
	FRDGPassRef Producer = nullptr;
	AddFillPass(GraphBuilder, Env, Unused, 1.0f, false, &Producer);

	TShaderMapRef<FTestFillWithUnusedCS> ComputeShader(Env.ShaderMap);
	FTestFillWithUnusedCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestFillWithUnusedCS::FParameters>();
	PassParameters->Size = Out->Desc.Extent;
	PassParameters->Value = 9.0f;
	PassParameters->UnusedTexture = Unused;
	PassParameters->OutputTexture = GraphBuilder.CreateUAV(Out);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FillWithUnused"), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(Out->Desc.Extent, 8));
	RDG_TEST_CHECK(PassParameters->UnusedTexture == nullptr, "ClearUnusedGraphResources did not null the unbound texture");
	GraphBuilder.QueueTextureExtraction(Out, &Output);
	GraphBuilder.Execute();
	RDG_TEST_CHECK(Producer->IsCulled(), "the producer of a texture the shader doesn't read should be culled");
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Output), 9.0f, kTestSize * kTestSize, 1e-4f, &Error), "output wrong: %s", Error);
}

void TestTransientAliasing(FTestEnv &Env, FTestResult &Result) {
	for (int32 bAliasing = 1; bAliasing >= 0; bAliasing--) {
		FScopedCVar Transient("r.RDG.TransientAllocator", bAliasing);
		TRefCountPtr<IPooledRenderTarget> Out1;
		TRefCountPtr<IPooledRenderTarget> Out2;
		RID XRID;
		RID YRID;
		uint32 NumTransient = 0;
		uint32 NumPhysical = 0;
		uint32 NumPlaced = 0;
		{
			FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestTransientAliasing"));
			const FRDGTextureDesc Desc = MakeR32Desc(32);
			FRDGTextureRef X = GraphBuilder.CreateTexture(Desc, "X");
			FRDGTextureRef Y = GraphBuilder.CreateTexture(Desc, "Y");
			FRDGTextureRef O1 = GraphBuilder.CreateTexture(Desc, "Out1");
			FRDGTextureRef O2 = GraphBuilder.CreateTexture(Desc, "Out2");
			AddFillPass(GraphBuilder, Env, X, 1.0f);
			AddAddValuePass(GraphBuilder, Env, X, O1, 1.0f);
			AddFillPass(GraphBuilder, Env, Y, 10.0f);
			AddAddValuePass(GraphBuilder, Env, Y, O2, 5.0f);
			GraphBuilder.QueueTextureExtraction(O1, &Out1);
			GraphBuilder.QueueTextureExtraction(O2, &Out2);
			GraphBuilder.Execute();
			XRID = GraphBuilder.GetAllocatedTextureRID(X);
			YRID = GraphBuilder.GetAllocatedTextureRID(Y);
			NumTransient = GraphBuilder.GetStats().NumTransientTextures;
			NumPhysical = GraphBuilder.GetStats().NumPhysicalTransientTextures;
			NumPlaced = GraphBuilder.GetStats().NumPlacedTextures;
		}
		Env.RHICmdList->SubmitCommandsAndFlushGPU();
		// Heap placement (1) puts Y at X's offset and reuses the cached placed texture; the pool (0) hands X's texture to Y.
		RDG_TEST_CHECK(XRID.is_valid() && XRID == YRID, "X and Y have disjoint lifetimes and should share one RD texture (r.RDG.TransientAllocator=%d)", bAliasing);
		RDG_TEST_CHECK(NumPhysical == NumTransient - 1, "expected %d RD textures for %d transient textures", NumTransient - 1, NumTransient);
		RDG_TEST_CHECK(bAliasing == (NumPlaced > 0), "r.RDG.TransientAllocator=%d placed %d textures", bAliasing, NumPlaced);
		String Error;
		RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out1), 2.0f, 32 * 32, 1e-4f, &Error), "Out1 wrong (aliasing=%d): %s", bAliasing, Error);
		RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out2), 15.0f, 32 * 32, 1e-4f, &Error), "Out2 wrong (aliasing=%d): %s", bAliasing, Error);
		if (bAliasing) Result.Message = vformat("%d transient textures -> %d RD textures", NumTransient, NumPhysical);
	}
}

bool IsTransientHeapSupported(FTestEnv &Env, FTestResult &Result) {
	if (FRDGDeviceContext::Get(Env.RD)->TransientAllocator.IsSupported()) return true;
	Result.bSkipped = true;
	Result.Message = "the RenderingDevice driver doesn't support memory heaps (placed resources)";
	return false;
}

void RunTransientHeapAliasing(FTestEnv &Env, FTestResult &Result, ERDGBuilderFlags Flags) {
	// A and B have different formats and sizes but disjoint lifetimes: B is placed over A's memory. Fill(B) has no
	// dependency on A's passes, so without the aliasing acquire Godot's graph would run it in the same level as Fill(A).
	FScopedCVar Transient("r.RDG.TransientAllocator", 1);
	constexpr int32 SizeA = 512;
	constexpr int32 SizeB = 384;
	TRefCountPtr<IPooledRenderTarget> Out1;
	TRefCountPtr<IPooledRenderTarget> Out2;
	FRHITransientHeapAllocation AllocationA;
	FRHITransientHeapAllocation AllocationB;
	RID RIDA;
	RID RIDB;
	FRDGBuilderStats Stats;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestTransientHeapAliasing"), Flags);
		FRDGTextureRef A = GraphBuilder.CreateTexture(MakeR32Desc(SizeA), "A");
		FRDGTextureRef B = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(SizeB, SizeB), PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable), "B");
		FRDGTextureRef O1 = GraphBuilder.CreateTexture(MakeR32Desc(SizeA), "Out1");
		FRDGTextureRef O2 = GraphBuilder.CreateTexture(MakeR32Desc(SizeB), "Out2");
		AddFillPass(GraphBuilder, Env, A, 3.0f);
		AddAddValuePass(GraphBuilder, Env, A, O1, 1.0f);
		AddFillPass(GraphBuilder, Env, B, 10.0f);
		AddAddValuePass(GraphBuilder, Env, B, O2, 5.0f);
		GraphBuilder.QueueTextureExtraction(O1, &Out1);
		GraphBuilder.QueueTextureExtraction(O2, &Out2);
		GraphBuilder.Execute();
		AllocationA = GraphBuilder.GetTransientAllocation(A);
		AllocationB = GraphBuilder.GetTransientAllocation(B);
		RIDA = GraphBuilder.GetAllocatedTextureRID(A);
		RIDB = GraphBuilder.GetAllocatedTextureRID(B);
		Stats = GraphBuilder.GetStats();
	}
	if (!IsTransientHeapSupported(Env, Result)) return;
	Env.RHICmdList->SubmitCommandsAndFlushGPU();

	RDG_TEST_CHECK(AllocationA.IsValid() && AllocationB.IsValid(), "A and B should be placed in a transient heap");
	RDG_TEST_CHECK(RIDA != RIDB, "A and B have different descs, they must be different RD textures");
	RDG_TEST_CHECK(AllocationA.Overlaps(AllocationB), "B [%d, +%d) should alias A [%d, +%d)", (int64)AllocationB.Offset, (int64)AllocationB.Size, (int64)AllocationA.Offset, (int64)AllocationA.Size);
	RDG_TEST_CHECK(Stats.TransientPeakBytes < Stats.TransientAllocatedBytes, "aliasing should lower the peak (%d) below the sum of allocations (%d)", (int64)Stats.TransientPeakBytes, (int64)Stats.TransientAllocatedBytes);
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out1), 4.0f, SizeA * SizeA, 1e-4f, &Error), "Out1 = A (3) + 1: %s", Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out2), 15.0f, SizeB * SizeB, 1e-4f, &Error), "Out2 = B (10) + 5: %s", Error);
	Result.Message = vformat("A [%d KB] and B [%d KB] share offset %d; peak %d KB of %d KB allocated", (int64)(AllocationA.Size / 1024), (int64)(AllocationB.Size / 1024), (int64)AllocationB.Offset, (int64)(Stats.TransientPeakBytes / 1024), (int64)(Stats.TransientAllocatedBytes / 1024));
	if (EnumHasAnyFlags(Flags, ERDGBuilderFlags::ParallelExecute)) {
		RDG_TEST_CHECK(Stats.NumParallelPasses == 4 && Stats.NumParallelPassSets == 4, "expected 4 passes in 4 parallel tasks, got %d passes in %d tasks", Stats.NumParallelPasses, Stats.NumParallelPassSets);
		Result.Message += ", 1 parallel task per pass";
	}
}

void TestTransientHeapAliasing(FTestEnv &Env, FTestResult &Result) {
	RunTransientHeapAliasing(Env, Result, ERDGBuilderFlags::None);
}

void TestParallelExecuteAliasing(FTestEnv &Env, FTestResult &Result) {
	// Aliasing acquires are recorded into the recording context of the pass that first uses the resource, so they still
	// come after every command of the overlapping resources once the contexts are submitted.
	FScopedCVar Parallel("r.RDG.ParallelExecute", 1);
	FScopedCVar PassMax("r.RDG.ParallelExecute.PassMax", 1);
	RunTransientHeapAliasing(Env, Result, ERDGBuilderFlags::ParallelExecute);
}

/** Pass lambdas taking the immediate command list must run on the render thread (UE's inline task mode). */
BEGIN_SHADER_PARAMETER_STRUCT(FTestInlinePassParameters, )
RDG_TEXTURE_ACCESS(Texture, ERHIAccess::UAVCompute)
END_SHADER_PARAMETER_STRUCT()

struct FParallelGraphOutputs {
	TRefCountPtr<IPooledRenderTarget> Chain;
	TRefCountPtr<IPooledRenderTarget> Branch;
	TRefCountPtr<IPooledRenderTarget> Cleared;
	TRefCountPtr<IPooledRenderTarget> Copied;
	TRefCountPtr<IPooledRenderTarget> UniformBuffer;
	TRefCountPtr<IPooledRenderTarget> Raster;
	TRefCountPtr<IPooledRenderTarget> Inline;
	TRefCountPtr<FRDGPooledBuffer> Buffer;
	FRDGBuilderStats Stats;
	bool bInlinePassesInline = false;
	bool bChainPassesParallel = false;
};

void BuildParallelTestGraph(FTestEnv &Env, ERDGBuilderFlags Flags, FParallelGraphOutputs &Out) {
	FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestParallelExecute"), Flags);

	// Long compute chain over transient textures (aliasing between passes) with an independent branch interleaved.
	constexpr int32 ChainLength = 10;
	FRDGTextureRef Chain = GraphBuilder.CreateTexture(MakeR32Desc(), "Chain0");
	FRDGTextureRef Branch = GraphBuilder.CreateTexture(MakeR32Desc(), "Branch0");
	FRDGTextureRef Third = nullptr;
	AddFillPass(GraphBuilder, Env, Chain, 1.0f);
	AddFillPass(GraphBuilder, Env, Branch, 100.0f);
	LocalVector<FRDGPassRef> ChainPasses;
	for (int32 Index = 1; Index <= ChainLength; Index++) {
		FRDGTextureRef Next = GraphBuilder.CreateTexture(MakeR32Desc(), "Chain");
		ChainPasses.push_back(AddAddValuePass(GraphBuilder, Env, Chain, Next, 1.0f));
		Chain = Next;
		if (Index == 3) Third = Chain;
		if (Index % 3 == 0) {
			FRDGTextureRef NextBranch = GraphBuilder.CreateTexture(MakeR32Desc(), "Branch");
			AddAddValuePass(GraphBuilder, Env, Branch, NextBranch, 0.5f);
			Branch = NextBranch;
		}
	}
	GraphBuilder.QueueTextureExtraction(Chain, &Out.Chain);
	GraphBuilder.QueueTextureExtraction(Branch, &Out.Branch);

	// Inline passes in the middle of parallel work: NeverParallel and an immediate command list lambda.
	FRDGTextureRef InlineIn = GraphBuilder.CreateTexture(MakeR32Desc(), "InlineIn");
	FRDGTextureRef InlineOut = GraphBuilder.CreateTexture(MakeR32Desc(), "InlineOut");
	AddFillPass(GraphBuilder, Env, InlineIn, 20.0f);
	FRDGPassRef NeverParallelPass = nullptr;
	{
		TShaderMapRef<FTestAddValueCS> ComputeShader(Env.ShaderMap);
		FTestAddValueCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestAddValueCS::FParameters>();
		PassParameters->Size = InlineOut->Desc.Extent;
		PassParameters->Value = 2.0f;
		PassParameters->InputTexture = InlineIn;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(InlineOut);
		NeverParallelPass = GraphBuilder.AddPass(RDG_EVENT_NAME("NeverParallelAdd"), PassParameters, ERDGPassFlags::Compute | ERDGPassFlags::NeverParallel, [PassParameters, ComputeShader](FRHIComputeCommandList &RHICmdList) {
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, FComputeShaderUtils::GetGroupCount(PassParameters->Size, 8));
		});
	}
	FTestInlinePassParameters *ImmediateParameters = GraphBuilder.AllocParameters<FTestInlinePassParameters>();
	ImmediateParameters->Texture = InlineOut;
	FRDGPassRef ImmediatePass = GraphBuilder.AddPass(RDG_EVENT_NAME("ImmediateLambda"), ImmediateParameters, ERDGPassFlags::Compute, [](FRHICommandListImmediate &) {});
	GraphBuilder.QueueTextureExtraction(InlineOut, &Out.Inline);

	// Commands that can't be recorded off the render thread are deferred in order: clear, copy, upload, uniform buffer.
	FRDGTextureRef Cleared = GraphBuilder.CreateTexture(MakeR32Desc(), "Cleared");
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Cleared), FVector4f(7.0f, 7.0f, 7.0f, 7.0f));
	FRDGTextureRef ClearedOut = GraphBuilder.CreateTexture(MakeR32Desc(), "ClearedOut");
	AddAddValuePass(GraphBuilder, Env, Cleared, ClearedOut, 1.0f);
	GraphBuilder.QueueTextureExtraction(ClearedOut, &Out.Cleared);

	FRDGTextureRef Copied = GraphBuilder.CreateTexture(MakeR32Desc(), "Copied");
	AddCopyTexturePass(GraphBuilder, Third, Copied);
	GraphBuilder.QueueTextureExtraction(Copied, &Out.Copied);

	constexpr uint32 NumElements = 64;
	LocalVector<float> InitialData;
	for (uint32 Index = 0; Index < NumElements; Index++) InitialData.push_back((float)Index);
	FRDGBufferRef Input = CreateStructuredBuffer(GraphBuilder, "Input", InitialData);
	FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumElements), "OutputBuffer");
	{
		TShaderMapRef<FTestBufferMadCS> ComputeShader(Env.ShaderMap);
		FTestBufferMadCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestBufferMadCS::FParameters>();
		PassParameters->NumElements = NumElements;
		PassParameters->Scale = 2.0f;
		PassParameters->Bias = 1.0f;
		PassParameters->InputBuffer = GraphBuilder.CreateSRV(Input);
		PassParameters->OutputBuffer = GraphBuilder.CreateUAV(OutputBuffer);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BufferMad"), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount((int32)NumElements, 64));
	}
	GraphBuilder.QueueBufferExtraction(OutputBuffer, &Out.Buffer);

	{
		FRDGTextureRef Source = GraphBuilder.CreateTexture(MakeR32Desc(), "UBSource");
		FRDGTextureRef UBOut = GraphBuilder.CreateTexture(MakeR32Desc(), "UBOut");
		AddFillPass(GraphBuilder, Env, Source, 3.0f);
		FTestUniformBuffer *UniformParameters = GraphBuilder.AllocParameters<FTestUniformBuffer>();
		UniformParameters->Scale = FVector4f(2.0f, 0.0f, 0.0f, 0.0f);
		UniformParameters->Offset = 0.5f;
		UniformParameters->Mode = 1;
		UniformParameters->SourceTexture = Source;
		TShaderMapRef<FTestUniformBufferCS> ComputeShader(Env.ShaderMap);
		FTestUniformBufferCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestUniformBufferCS::FParameters>();
		PassParameters->Size = UBOut->Desc.Extent;
		PassParameters->TestUB = GraphBuilder.CreateUniformBuffer(UniformParameters);
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(UBOut);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("UniformBufferCS"), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(UBOut->Desc.Extent, 8));
		GraphBuilder.QueueTextureExtraction(UBOut, &Out.UniformBuffer);
	}

	// Merged raster passes: one RD draw list, so one recording context for both passes.
	{
		const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(8, 8), PF_FloatRGBA, FClearValueBinding(FLinearColor(1.0f, 0.0f, 0.0f, 1.0f)), TexCreate_RenderTargetable | TexCreate_ShaderResource);
		FRDGTextureRef Target = GraphBuilder.CreateTexture(Desc, "RasterTarget");
		TShaderMapRef<FTestSolidColorPS> PixelShader(Env.ShaderMap);
		const FIntRect Viewport(0, 0, 8, 8);
		FTestSolidColorPS::FParameters *First = GraphBuilder.AllocParameters<FTestSolidColorPS::FParameters>();
		First->Color = FLinearColor(0.25f, 0.5f, 0.0f, 1.0f);
		First->RenderTargets[0] = FRenderTargetBinding(Target, ERenderTargetLoadAction::EClear);
		FPixelShaderUtils::AddFullscreenPass(GraphBuilder, Env.ShaderMap, RDG_EVENT_NAME("SolidColor"), PixelShader, First, Viewport);
		FTestSolidColorPS::FParameters *Second = GraphBuilder.AllocParameters<FTestSolidColorPS::FParameters>();
		Second->Color = FLinearColor(0.25f, 0.0f, 0.5f, 0.0f);
		Second->RenderTargets[0] = FRenderTargetBinding(Target, ERenderTargetLoadAction::ELoad);
		FPixelShaderUtils::AddFullscreenPass(GraphBuilder, Env.ShaderMap, RDG_EVENT_NAME("AdditiveColor"), PixelShader, Second, Viewport, TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI());
		GraphBuilder.QueueTextureExtraction(Target, &Out.Raster);
	}


	GraphBuilder.Execute();
	Out.Stats = GraphBuilder.GetStats();
	Out.bInlinePassesInline = !NeverParallelPass->IsParallelExecute() && !ImmediatePass->IsParallelExecute();
	Out.bChainPassesParallel = true;
	for (FRDGPassRef Pass : ChainPasses) Out.bChainPassesParallel &= Pass->IsParallelExecute();
}

void CheckParallelTestGraph(FTestEnv &Env, FTestResult &Result, const FParallelGraphOutputs &Out, const char *Name) {
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out.Chain), 11.0f, kTestSize * kTestSize, 1e-4f, &Error), "[%s] chain 1 + 10 x 1: %s", Name, Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out.Branch), 101.5f, kTestSize * kTestSize, 1e-4f, &Error), "[%s] branch 100 + 3 x 0.5: %s", Name, Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out.Cleared), 8.0f, kTestSize * kTestSize, 1e-4f, &Error), "[%s] clear 7 + 1: %s", Name, Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out.Copied), 4.0f, kTestSize * kTestSize, 1e-4f, &Error), "[%s] copy of chain step 3: %s", Name, Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out.UniformBuffer), 7.5f, kTestSize * kTestSize, 1e-4f, &Error), "[%s] uniform buffer 3 * 2 + 0.5 + 1: %s", Name, Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out.Inline), 22.0f, kTestSize * kTestSize, 1e-4f, &Error), "[%s] inline pass 20 + 2: %s", Name, Error);
	const Vector<float> Values = ReadBufferFloats(Env, Out.Buffer);
	RDG_TEST_CHECK(Values.size() >= 64, "[%s] buffer readback has %d floats", Name, Values.size());
	for (uint32 Index = 0; Index < 64; Index++) RDG_TEST_CHECK(Math::abs(Values[Index] - (2.0f * Index + 1.0f)) < 1e-4f, "[%s] buffer element %d = %f", Name, Index, Values[Index]);
	const Vector<uint8_t> RasterData = Env.RD->texture_get_data(Out.Raster->GetRHI()->GetRID(), 0);
	RDG_TEST_CHECK(RasterData.size() >= 8 * 8 * 8, "[%s] raster readback has %d bytes", Name, RasterData.size());
	const uint16 *Halves = reinterpret_cast<const uint16 *>(RasterData.ptr());
	const float Expected[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
	for (uint32 Value = 0; Value < 64 * 4; Value++) RDG_TEST_CHECK(Math::abs(Math::half_to_float(Halves[Value]) - Expected[Value % 4]) < 1e-3f, "[%s] raster value %d = %f", Name, Value, Math::half_to_float(Halves[Value]));
}

double MeasureParallelExecuteGraph(FTestEnv &Env, FTestResult &Result, bool bParallel, int32 DispatchesPerPass, uint32 &OutNumTasks, RenderingDevice::TranslateStats *OutTranslateStats = nullptr) {
	constexpr int32 NumChains = 8;
	constexpr int32 ChainLength = 32;
	FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestParallelExecuteTiming"), bParallel ? ERDGBuilderFlags::ParallelExecute : ERDGBuilderFlags::None);
	LocalVector<TRefCountPtr<IPooledRenderTarget>> Outputs;
	Outputs.resize(NumChains);
	TShaderMapRef<FTestAddValueCS> ComputeShader(Env.ShaderMap);
	for (int32 ChainIndex = 0; ChainIndex < NumChains; ChainIndex++) {
		FRDGTextureRef Texture = GraphBuilder.CreateTexture(MakeR32Desc(), "Start");
		AddFillPass(GraphBuilder, Env, Texture, (float)ChainIndex);
		for (int32 Step = 1; Step < ChainLength; Step++) {
			FRDGTextureRef Next = GraphBuilder.CreateTexture(MakeR32Desc(), "Step");
			FTestAddValueCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestAddValueCS::FParameters>();
			PassParameters->Size = Next->Desc.Extent;
			PassParameters->Value = 1.0f;
			PassParameters->InputTexture = Texture;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(Next);
			// Every dispatch writes the same values, more dispatches only add recording work.
			GraphBuilder.AddPass(RDG_EVENT_NAME("AddValue x%d", DispatchesPerPass), PassParameters, ERDGPassFlags::Compute, [PassParameters, ComputeShader, DispatchesPerPass](FRHIComputeCommandList &RHICmdList) {
				for (int32 Dispatch = 0; Dispatch < DispatchesPerPass; Dispatch++) FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, FComputeShaderUtils::GetGroupCount(PassParameters->Size, 8));
			});
			Texture = Next;
		}
		GraphBuilder.QueueTextureExtraction(Texture, &Outputs[ChainIndex]);
	}
	const uint64 Start = OS::get_singleton()->get_ticks_usec();
	GraphBuilder.Execute();
	const double Ms = (OS::get_singleton()->get_ticks_usec() - Start) / 1000.0;
	OutNumTasks = GraphBuilder.GetStats().NumParallelPassSets;
	if (GraphBuilder.GetStats().NumBindingErrors != 0) {
		Result.bPassed = false;
		Result.Message = vformat("binding errors: %d", GraphBuilder.GetStats().NumBindingErrors);
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	// Before the readbacks, which submit frames of their own.
	if (OutTranslateStats) *OutTranslateStats = Env.RD->parallel_translate_get_last_frame_stats();
	String Error;
	for (int32 ChainIndex = 0; ChainIndex < NumChains && Result.bPassed; ChainIndex++) {
		if (!AllNear(ReadTextureFloats(Env, Outputs[ChainIndex]), (float)(ChainIndex + ChainLength - 1), kTestSize * kTestSize, 1e-4f, &Error)) {
			Result.bPassed = false;
			Result.Message = vformat("chain %d (parallel=%d): %s", ChainIndex, bParallel, Error);
		}
	}
	return Ms;
}

void TestParallelExecuteTiming(FTestEnv &Env, FTestResult &Result) {
	// Report only (no threshold): CPU time of Execute() for 256 compute passes in 8 independent chains, serial vs
	// parallel, with cheap passes (1 dispatch) and passes with more recording work (32 dispatches). Then the CPU time of
	// the RenderingDevice's translation of the frame into command buffers (RenderingDeviceGraph::end(), when the frame is
	// submitted), serial vs parallel translate (r.RHICmd.ParallelTranslate.Enable).
	FScopedCVar Parallel("r.RDG.ParallelExecute", 1);
	String Summary;
	for (const int32 DispatchesPerPass : { 1, 32 }) {
		double BestMs[2] = { 1e9, 1e9 };
		double BestTranslateMs[2] = { 1e9, 1e9 };
		RenderingDevice::TranslateStats TranslateStats[2];
		uint32 NumTasks = 0;
		for (int32 Run = 0; Run < 4; Run++) {
			for (int32 bParallel = 0; bParallel <= 1; bParallel++) {
				BestMs[bParallel] = MIN(BestMs[bParallel], MeasureParallelExecuteGraph(Env, Result, bParallel, DispatchesPerPass, NumTasks));
				if (!Result.bPassed) return;
			}
			for (int32 bParallelTranslate = 0; bParallelTranslate <= 1; bParallelTranslate++) {
				FScopedCVar ParallelTranslate("r.RHICmd.ParallelTranslate.Enable", bParallelTranslate);
				RenderingDevice::TranslateStats Stats;
				MeasureParallelExecuteGraph(Env, Result, true, DispatchesPerPass, NumTasks, &Stats);
				if (!Result.bPassed) return;
				RDG_TEST_CHECK((Stats.task_count > 0) == (bParallelTranslate != 0) || WorkerThreadPool::get_singleton()->get_thread_count() == 0, "parallel translate %d: %d translate tasks", bParallelTranslate, Stats.task_count);
				BestTranslateMs[bParallelTranslate] = MIN(BestTranslateMs[bParallelTranslate], Stats.usec / 1000.0);
				TranslateStats[bParallelTranslate] = Stats;
			}
		}
		Summary += vformat("%s%d dispatch/pass: Execute() serial %.2f ms, parallel %.2f ms; RD translate of %d commands serial %.2f ms, parallel %.2f ms (%d tasks)", Summary.is_empty() ? "" : "; ", DispatchesPerPass, BestMs[0], BestMs[1], TranslateStats[1].command_count, BestTranslateMs[0], BestTranslateMs[1], TranslateStats[1].task_count);
	}
	Result.Message = vformat("256 passes in 8 tasks (%d worker threads): %s", WorkerThreadPool::get_singleton()->get_thread_count(), Summary);
}

FRDGPassRef AddBufferMadPass(FRDGBuilder &GraphBuilder, FTestEnv &Env, FRDGBufferRef Input, FRDGBufferRef Output, uint32 NumElements, float Scale, float Bias);

struct FParallelSetupTiming {
	double SetupMs = 0.0;
	double ExecuteMs = 0.0;
	FRDGBuilderStats Stats;
};

/** 256 compute passes in 8 chains, flushing the async setup queue every 32 passes; optionally 4 setup tasks generating
 *  the 4 MB contents of an uploaded buffer (consumed by a fill callback) while the passes are added. */
FParallelSetupTiming MeasureParallelSetupGraph(FTestEnv &Env, FTestResult &Result, ERDGBuilderFlags Flags, bool bSetupTask) {
	constexpr int32 NumChains = 8;
	constexpr int32 ChainLength = 32;
	constexpr uint32 NumGenerated = 1 << 20;
	FParallelSetupTiming Timing;
	LocalVector<TRefCountPtr<IPooledRenderTarget>> Outputs;
	Outputs.resize(NumChains);
	LocalVector<float> Generated;
	TRefCountPtr<FRDGPooledBuffer> GeneratedOut;
	const uint64 SetupStart = OS::get_singleton()->get_ticks_usec();
	FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestParallelSetupTiming"), Flags);
	if (bSetupTask) {
		// Four independent setup tasks, a quarter of the data each.
		constexpr uint32 NumSetupTasks = 4;
		Generated.resize(NumGenerated);
		for (uint32 TaskIndex = 0; TaskIndex < NumSetupTasks; TaskIndex++) {
			GraphBuilder.AddSetupTask([&Generated, TaskIndex]() {
				const uint32 First = TaskIndex * (NumGenerated / NumSetupTasks);
				for (uint32 Index = First; Index < First + NumGenerated / NumSetupTasks; Index++) Generated[Index] = Math::sin((float)Index) * 0.5f + 0.5f;
			});
		}
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumGenerated), "Generated");
		GraphBuilder.QueueBufferUpload(Buffer, [&Generated](void *Data, uint32 Size) { memcpy(Data, Generated.ptr(), MIN((uint64)Size, (uint64)Generated.size() * sizeof(float))); });
		FRDGBufferRef Out = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumGenerated), "GeneratedOut");
		AddBufferMadPass(GraphBuilder, Env, Buffer, Out, NumGenerated, 1.0f, 0.0f);
		GraphBuilder.QueueBufferExtraction(Out, &GeneratedOut);
	}
	int32 NumPasses = 0;
	for (int32 ChainIndex = 0; ChainIndex < NumChains; ChainIndex++) {
		FRDGTextureRef Texture = GraphBuilder.CreateTexture(MakeR32Desc(), "Start");
		AddFillPass(GraphBuilder, Env, Texture, (float)ChainIndex);
		for (int32 Step = 1; Step < ChainLength; Step++) {
			FRDGTextureRef Next = GraphBuilder.CreateTexture(MakeR32Desc(), "Step");
			AddAddValuePass(GraphBuilder, Env, Texture, Next, 1.0f);
			Texture = Next;
			if (++NumPasses % 32 == 0) GraphBuilder.FlushSetupQueue();
		}
		GraphBuilder.QueueTextureExtraction(Texture, &Outputs[ChainIndex]);
	}
	const uint64 ExecuteStart = OS::get_singleton()->get_ticks_usec();
	GraphBuilder.Execute();
	const uint64 End = OS::get_singleton()->get_ticks_usec();
	Timing.SetupMs = (ExecuteStart - SetupStart) / 1000.0;
	Timing.ExecuteMs = (End - ExecuteStart) / 1000.0;
	Timing.Stats = GraphBuilder.GetStats();
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	for (int32 ChainIndex = 0; ChainIndex < NumChains && Result.bPassed; ChainIndex++) {
		if (!AllNear(ReadTextureFloats(Env, Outputs[ChainIndex]), (float)(ChainIndex + ChainLength - 1), kTestSize * kTestSize, 1e-4f, &Error)) {
			Result.bPassed = false;
			Result.Message = vformat("chain %d: %s", ChainIndex, Error);
		}
	}
	if (bSetupTask && Result.bPassed) {
		const Vector<float> Values = ReadBufferFloats(Env, GeneratedOut);
		for (uint32 Index = 0; Index < NumGenerated && Result.bPassed; Index += 4099) {
			if (Math::abs(Values[Index] - (Math::sin((float)Index) * 0.5f + 0.5f)) > 1e-5f) {
				Result.bPassed = false;
				Result.Message = vformat("generated element %d = %f", Index, Values[Index]);
			}
		}
	}
	return Timing;
}

void TestParallelSetupTiming(FTestEnv &Env, FTestResult &Result) {
	// Report only (no threshold): CPU time of the graph setup (AddPass calls on the render thread) and of Execute(),
	// serial vs ParallelSetup | ParallelCompile, without and with 4 setup tasks generating 4 MB for a buffer fill callback.
	String Summary;
	for (const bool bSetupTask : { false, true }) {
		FParallelSetupTiming Best[2];
		Best[0].SetupMs = Best[0].ExecuteMs = Best[1].SetupMs = Best[1].ExecuteMs = 1e9;
		for (int32 Run = 0; Run < 4; Run++) {
			for (int32 bParallel = 0; bParallel <= 1; bParallel++) {
				const FParallelSetupTiming Timing = MeasureParallelSetupGraph(Env, Result, bParallel ? ERDGBuilderFlags::ParallelSetup | ERDGBuilderFlags::ParallelCompile : ERDGBuilderFlags::None, bSetupTask);
				if (!Result.bPassed) return;
				if (Timing.SetupMs + Timing.ExecuteMs < Best[bParallel].SetupMs + Best[bParallel].ExecuteMs) Best[bParallel] = Timing;
			}
		}
		Summary += vformat("%s%s: serial setup %.2f + Execute() %.2f ms, parallel setup %.2f + Execute() %.2f ms (%d of %d passes set up by the queue task)", Summary.is_empty() ? "" : "; ", bSetupTask ? "with 4 setup tasks generating 4 MB" : "passes only", Best[0].SetupMs, Best[0].ExecuteMs, Best[1].SetupMs, Best[1].ExecuteMs, Best[1].Stats.NumAsyncSetupPasses, Best[1].Stats.NumPasses);
	}
	Result.Message = vformat("256 passes: %s", Summary);
}

void TestParallelExecute(FTestEnv &Env, FTestResult &Result) {
	FScopedCVar Parallel("r.RDG.ParallelExecute", 1);
	struct FVariant {
		ERDGBuilderFlags Flags;
		int32 PassMax;
		const char *Name;
	};
	const FVariant Variants[] = {
		{ ERDGBuilderFlags::None, 32, "serial" },
		{ ERDGBuilderFlags::ParallelExecute, 32, "parallel" },
		{ ERDGBuilderFlags::ParallelExecute, 2, "parallel, 2 passes per task" },
		{ ERDGBuilderFlags::ParallelExecute, 1, "parallel, 1 pass per task" },
	};
	String Summary;
	for (const FVariant &Variant : Variants) {
		FScopedCVar PassMax("r.RDG.ParallelExecute.PassMax", Variant.PassMax);
		FParallelGraphOutputs Out;
		BuildParallelTestGraph(Env, Variant.Flags, Out);
		Env.RHICmdList->SubmitCommandsAndFlushGPU();

		const bool bParallel = Variant.Flags != ERDGBuilderFlags::None;
		RDG_TEST_CHECK(Out.Stats.NumBindingErrors == 0 && Out.Stats.NumValidationErrors == 0, "[%s] %d binding errors, %d validation errors", Variant.Name, Out.Stats.NumBindingErrors, Out.Stats.NumValidationErrors);
		RDG_TEST_CHECK(Out.Stats.bParallelExecute == bParallel, "[%s] parallel execute should be %s", Variant.Name, bParallel ? "on" : "off");
		if (bParallel) {
			RDG_TEST_CHECK(Out.bChainPassesParallel, "[%s] the compute chain should be recorded by parallel tasks", Variant.Name);
			RDG_TEST_CHECK(Out.bInlinePassesInline, "[%s] NeverParallel / immediate command list passes must run inline", Variant.Name);
			RDG_TEST_CHECK(Out.Stats.NumParallelPassSets >= 2, "[%s] expected several parallel tasks, got %d", Variant.Name, Out.Stats.NumParallelPassSets);
		} else {
			RDG_TEST_CHECK(Out.Stats.NumParallelPasses == 0, "[%s] %d passes ran in parallel", Variant.Name, Out.Stats.NumParallelPasses);
		}

		CheckParallelTestGraph(Env, Result, Out, Variant.Name);
		if (!Result.bPassed) return;

		if (bParallel) Summary += vformat("%s%s: %d/%d passes in %d tasks", Summary.is_empty() ? "" : "; ", Variant.Name, Out.Stats.NumParallelPasses, Out.Stats.NumPasses - Out.Stats.NumCulledPasses, Out.Stats.NumParallelPassSets);
	}
	Result.Message = Summary;
}

void TestTransientBufferAliasing(FTestEnv &Env, FTestResult &Result) {
	FScopedCVar Transient("r.RDG.TransientAllocator", 1);
	constexpr uint32 NumElements = 4096;
	TRefCountPtr<FRDGPooledBuffer> Output1;
	TRefCountPtr<FRDGPooledBuffer> Output2;
	FRHITransientHeapAllocation AllocationX;
	FRHITransientHeapAllocation AllocationY;
	uint32 NumPlaced = 0;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestTransientBufferAliasing"));
		LocalVector<float> DataX;
		LocalVector<float> DataY;
		for (uint32 Index = 0; Index < NumElements; Index++) DataX.push_back((float)Index);
		for (uint32 Index = 0; Index < NumElements / 2; Index++) DataY.push_back(1000.0f + Index);
		FRDGBufferRef X = CreateStructuredBuffer(GraphBuilder, "X", DataX);
		FRDGBufferRef Y = CreateStructuredBuffer(GraphBuilder, "Y", DataY);
		FRDGBufferRef O1 = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumElements), "Out1");
		FRDGBufferRef O2 = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumElements / 2), "Out2");
		TShaderMapRef<FTestBufferMadCS> ComputeShader(Env.ShaderMap);
		auto AddMadPass = [&](FRDGBufferRef Input, FRDGBufferRef Output, uint32 Count, float Scale) {
			FTestBufferMadCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestBufferMadCS::FParameters>();
			PassParameters->NumElements = Count;
			PassParameters->Scale = Scale;
			PassParameters->Bias = 1.0f;
			PassParameters->InputBuffer = GraphBuilder.CreateSRV(Input);
			PassParameters->OutputBuffer = GraphBuilder.CreateUAV(Output);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BufferMad(%s)", Input->Name), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount((int32)Count, 64));
		};
		AddMadPass(X, O1, NumElements, 2.0f);
		AddMadPass(Y, O2, NumElements / 2, 3.0f);
		GraphBuilder.QueueBufferExtraction(O1, &Output1);
		GraphBuilder.QueueBufferExtraction(O2, &Output2);
		GraphBuilder.Execute();
		AllocationX = GraphBuilder.GetTransientAllocation(X);
		AllocationY = GraphBuilder.GetTransientAllocation(Y);
		NumPlaced = GraphBuilder.GetStats().NumPlacedBuffers;
	}
	if (!IsTransientHeapSupported(Env, Result)) return;
	Env.RHICmdList->SubmitCommandsAndFlushGPU();

	if (NumPlaced == 0) {
		Result.bSkipped = true;
		Result.Message = "the driver can't place buffers in the texture heap memory type";
		return;
	}
	RDG_TEST_CHECK(AllocationX.IsValid() && AllocationY.IsValid() && AllocationX.Overlaps(AllocationY), "Y (uploaded after X's last use) should alias X");
	const Vector<float> Values1 = ReadBufferFloats(Env, Output1);
	const Vector<float> Values2 = ReadBufferFloats(Env, Output2);
	RDG_TEST_CHECK((uint32)Values1.size() >= NumElements && (uint32)Values2.size() >= NumElements / 2, "buffer readbacks are too small");
	for (uint32 Index = 0; Index < NumElements; Index++) RDG_TEST_CHECK(Math::abs(Values1[Index] - (2.0f * Index + 1.0f)) < 1e-3f, "Out1[%d] = %f, expected %f", Index, Values1[Index], 2.0f * Index + 1.0f);
	for (uint32 Index = 0; Index < NumElements / 2; Index++) RDG_TEST_CHECK(Math::abs(Values2[Index] - (3.0f * (1000.0f + Index) + 1.0f)) < 1e-2f, "Out2[%d] = %f, expected %f", Index, Values2[Index], 3.0f * (1000.0f + Index) + 1.0f);
	Result.Message = vformat("%d placed buffers, X and Y share offset %d", NumPlaced, (int64)AllocationY.Offset);
}

void TestTransientHeapCache(FTestEnv &Env, FTestResult &Result) {
	FScopedCVar Transient("r.RDG.TransientAllocator", 1);
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Env.RD);
	if (!IsTransientHeapSupported(Env, Result)) return;
	FRHITransientResourceAllocator &Allocator = Context->TransientAllocator;
	const FRDGTextureDesc Desc = MakeR32Desc(96);

	uint64 CreatedAfterFirstFrame = 0;
	for (int32 Frame = 0; Frame < 3; Frame++) {
		FRDGBuilder::TickPoolElements(Env.RD);
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestTransientHeapCache"));
		FRDGTextureRef Temp = GraphBuilder.CreateTexture(Desc, "Temp");
		FRDGTextureRef Out = GraphBuilder.CreateTexture(Desc, "Out");
		AddFillPass(GraphBuilder, Env, Temp, 1.0f);
		AddAddValuePass(GraphBuilder, Env, Temp, Out, 1.0f);
		TRefCountPtr<IPooledRenderTarget> Keep;
		GraphBuilder.QueueTextureExtraction(Out, &Keep);
		GraphBuilder.Execute();
		RDG_TEST_CHECK(GraphBuilder.GetStats().NumPlacedTextures == 1, "frame %d placed %d textures", Frame, GraphBuilder.GetStats().NumPlacedTextures);
		if (Frame == 0) CreatedAfterFirstFrame = Allocator.GetStats().TotalTexturesCreated;
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	RDG_TEST_CHECK(Allocator.GetStats().TotalTexturesCreated == CreatedAfterFirstFrame, "later frames should reuse the cached placed texture (created %d, then %d)", (int64)CreatedAfterFirstFrame, (int64)Allocator.GetStats().TotalTexturesCreated);
	RDG_TEST_CHECK(Allocator.GetStats().NumHeaps >= 1 && Allocator.GetStats().NumCachedTextures >= 1, "allocator should keep a heap and the cached texture");

	// Unused for longer than RHI.TransientAllocator.GarbageCollectLatency: placed textures and empty heaps are released.
	const int32 Latency = IConsoleManager::Get().FindConsoleVariable("RHI.TransientAllocator.GarbageCollectLatency")->GetInt();
	for (int32 Frame = 0; Frame <= Latency + 1; Frame++) FRDGBuilder::TickPoolElements(Env.RD);
	RDG_TEST_CHECK(Allocator.GetStats().NumHeaps == 0 && Allocator.GetStats().NumCachedTextures == 0, "garbage collection left %d heaps / %d textures", Allocator.GetStats().NumHeaps, Allocator.GetStats().NumCachedTextures);
	Result.Message = vformat("1 placed texture created for 3 frames, released after %d idle frames", Latency + 1);
}

void TestCrossFramePooling(FTestEnv &Env, FTestResult &Result) {
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Env.RD);
	const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(24, 24), PF_G16R16F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	RID FirstRID;
	for (int32 Frame = 0; Frame < 2; Frame++) {
		FRDGBuilder::TickPoolElements(Env.RD);
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestCrossFramePooling"));
		FRDGTextureRef Texture = GraphBuilder.CreateTexture(Desc, "PerFrameTexture");
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Texture), FVector4f(1.0f, 2.0f, 0.0f, 0.0f));
		TRefCountPtr<IPooledRenderTarget> Keep;
		GraphBuilder.QueueTextureExtraction(Texture, &Keep);
		GraphBuilder.Execute();
		const RID Allocated = GraphBuilder.GetAllocatedTextureRID(Texture);
		if (Frame == 0) {
			FirstRID = Allocated;
		} else {
			RDG_TEST_CHECK(Allocated == FirstRID, "the second frame should reuse the pooled texture");
			RDG_TEST_CHECK(Context->RenderTargetPool.GetStats().NumCreatedThisFrame == 0, "the pool created %d textures on the second frame", Context->RenderTargetPool.GetStats().NumCreatedThisFrame);
		}
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	Result.Message = vformat("pool: %d elements, %d reused total", Context->RenderTargetPool.GetNumElements(), (int64)Context->RenderTargetPool.GetStats().TotalReused);
}

void TestExtractionAndExternal(FTestEnv &Env, FTestResult &Result) {
	TRefCountPtr<IPooledRenderTarget> History;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestExtraction_Frame0"));
		FRDGTextureRef Texture = GraphBuilder.CreateTexture(MakeR32Desc(), "History");
		AddFillPass(GraphBuilder, Env, Texture, 7.0f);
		GraphBuilder.QueueTextureExtraction(Texture, &History);
		GraphBuilder.Execute();
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	FRDGBuilder::TickPoolElements(Env.RD);
	RDG_TEST_CHECK(History.IsValid() && History->GetRefCount() >= 2, "extracted texture must stay referenced outside the pool");

	TRefCountPtr<IPooledRenderTarget> Output;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestExtraction_Frame1"));
		FRDGTextureRef HistoryTexture = GraphBuilder.RegisterExternalTexture(History);
		RDG_TEST_CHECK(HistoryTexture && HistoryTexture->IsExternal(), "RegisterExternalTexture failed");
		RDG_TEST_CHECK(GraphBuilder.RegisterExternalTexture(History) == HistoryTexture, "registering twice must return the same RDG texture");
		FRDGTextureRef Out = GraphBuilder.CreateTexture(MakeR32Desc(), "Out");
		AddAddValuePass(GraphBuilder, Env, HistoryTexture, Out, 1.0f);
		GraphBuilder.QueueTextureExtraction(Out, &Output);
		GraphBuilder.Execute();
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Output), 8.0f, kTestSize * kTestSize, 1e-4f, &Error), "history (7) + 1 != 8: %s", Error);
}

void TestBufferUploadAndUAV(FTestEnv &Env, FTestResult &Result) {
	constexpr uint32 NumElements = 100;
	LocalVector<float> InitialData;
	for (uint32 Index = 0; Index < NumElements; Index++) InitialData.push_back((float)Index);
	TRefCountPtr<FRDGPooledBuffer> Output;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestBuffers"));
		FRDGBufferRef Input = CreateStructuredBuffer(GraphBuilder, "Input", InitialData);
		FRDGBufferRef Out = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumElements), "Output");
		TShaderMapRef<FTestBufferMadCS> ComputeShader(Env.ShaderMap);
		FTestBufferMadCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestBufferMadCS::FParameters>();
		PassParameters->NumElements = NumElements;
		PassParameters->Scale = 2.0f;
		PassParameters->Bias = 1.0f;
		PassParameters->InputBuffer = GraphBuilder.CreateSRV(Input);
		PassParameters->OutputBuffer = GraphBuilder.CreateUAV(Out);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BufferMad"), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount((int32)NumElements, 64));
		GraphBuilder.QueueBufferExtraction(Out, &Output);
		GraphBuilder.Execute();
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	const Vector<float> Values = ReadBufferFloats(Env, Output);
	RDG_TEST_CHECK((uint32)Values.size() >= NumElements, "buffer readback has %d floats", Values.size());
	for (uint32 Index = 0; Index < NumElements; Index++) RDG_TEST_CHECK(Math::abs(Values[Index] - (2.0f * Index + 1.0f)) < 1e-4f, "element %d = %f, expected %f", Index, Values[Index], 2.0f * Index + 1.0f);
}

void TestBufferClearAndReadback(FTestEnv &Env, FTestResult &Result) {
	FRHIGPUBufferReadback Readback("TestReadback");
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestBufferClear"));
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(4, 256), "Cleared");
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer), 0xABCD1234u);
		AddEnqueueCopyPass(GraphBuilder, &Readback, Buffer, 256 * 4);
		GraphBuilder.Execute();
	}
	for (int32 Attempt = 0; Attempt < 3 && !Readback.IsReady(); Attempt++) Env.RHICmdList->SubmitCommandsAndFlushGPU();
	RDG_TEST_CHECK(Readback.IsReady(), "async buffer readback never completed");
	const uint32 *Data = reinterpret_cast<const uint32 *>(Readback.Lock(256 * 4));
	RDG_TEST_CHECK(Data != nullptr, "readback Lock() returned null");
	for (uint32 Index = 0; Index < 256; Index++) RDG_TEST_CHECK(Data[Index] == 0xABCD1234u, "element %d = 0x%x", Index, Data[Index]);
	Readback.Unlock();
}

void TestUniformBuffer(FTestEnv &Env, FTestResult &Result) {
	TRefCountPtr<IPooledRenderTarget> Output;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestUniformBuffer"));
		FRDGTextureRef Source = GraphBuilder.CreateTexture(MakeR32Desc(), "Source");
		FRDGTextureRef Out = GraphBuilder.CreateTexture(MakeR32Desc(), "Out");
		AddFillPass(GraphBuilder, Env, Source, 3.0f);

		FTestUniformBuffer *UniformParameters = GraphBuilder.AllocParameters<FTestUniformBuffer>();
		UniformParameters->Scale = FVector4f(2.0f, 0.0f, 0.0f, 0.0f);
		UniformParameters->Offset = 0.5f;
		UniformParameters->Mode = 1;
		UniformParameters->SourceTexture = Source;
		TRDGUniformBufferRef<FTestUniformBuffer> UniformBuffer = GraphBuilder.CreateUniformBuffer(UniformParameters);

		TShaderMapRef<FTestUniformBufferCS> ComputeShader(Env.ShaderMap);
		FTestUniformBufferCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestUniformBufferCS::FParameters>();
		PassParameters->Size = Out->Desc.Extent;
		PassParameters->TestUB = UniformBuffer;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(Out);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("UniformBufferCS"), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(Out->Desc.Extent, 8));
		GraphBuilder.QueueTextureExtraction(Out, &Output);
		GraphBuilder.Execute();
		RDG_TEST_CHECK(GraphBuilder.GetStats().NumBindingErrors == 0, "binding errors: %d", GraphBuilder.GetStats().NumBindingErrors);
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Output), 7.5f, kTestSize * kTestSize, 1e-4f, &Error), "3 * 2 + 0.5 + 1 != 7.5: %s", Error);
}

void TestShaderPermutations(FTestEnv &Env, FTestResult &Result) {
	TRefCountPtr<IPooledRenderTarget> Plain;
	TRefCountPtr<IPooledRenderTarget> Offset;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestPermutations"));
		FRDGTextureRef A = GraphBuilder.CreateTexture(MakeR32Desc(), "Plain");
		FRDGTextureRef B = GraphBuilder.CreateTexture(MakeR32Desc(), "WithOffset");
		AddFillPass(GraphBuilder, Env, A, 5.0f, false);
		AddFillPass(GraphBuilder, Env, B, 5.0f, true);
		GraphBuilder.QueueTextureExtraction(A, &Plain);
		GraphBuilder.QueueTextureExtraction(B, &Offset);
		GraphBuilder.Execute();
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Plain), 5.0f, kTestSize * kTestSize, 1e-4f, &Error), "USE_OFFSET=0 wrong: %s", Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Offset), 105.0f, kTestSize * kTestSize, 1e-4f, &Error), "USE_OFFSET=1 wrong: %s", Error);
	RDG_TEST_CHECK(Env.ShaderMap->GetShader<FTestFillCS>(0).GetShader() != Env.ShaderMap->GetShader<FTestFillCS>(1).GetShader(), "permutations should be distinct shaders");
}

void TestMipViews(FTestEnv &Env, FTestResult &Result) {
	TRefCountPtr<IPooledRenderTarget> Output;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestMipViews"));
		FRDGTextureRef MipChain = GraphBuilder.CreateTexture(MakeR32Desc(16, 5), "MipChain");
		FRDGTextureRef Final = GraphBuilder.CreateTexture(MakeR32Desc(1), "LastMip");
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(MipChain, 0)), 4.0f);
		TShaderMapRef<FTestDownsampleCS> ComputeShader(Env.ShaderMap);
		for (uint8 Mip = 1; Mip < 5; Mip++) {
			const FIntPoint DestSize = MipChain->Desc.GetMipExtent(Mip);
			FTestDownsampleCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestDownsampleCS::FParameters>();
			PassParameters->DestSize = DestSize;
			PassParameters->SourceMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(MipChain, Mip - 1));
			PassParameters->DestMip = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(MipChain, Mip));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Downsample Mip%d", Mip), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(DestSize, 8));
		}
		FRHICopyTextureInfo CopyInfo;
		CopyInfo.SourceMipIndex = 4;
		CopyInfo.Size = FIntVector(1, 1, 1);
		AddCopyTexturePass(GraphBuilder, MipChain, Final, CopyInfo);
		GraphBuilder.QueueTextureExtraction(Final, &Output);
		GraphBuilder.Execute();
		RDG_TEST_CHECK(GraphBuilder.GetStats().NumBindingErrors == 0, "binding errors: %d", GraphBuilder.GetStats().NumBindingErrors);
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Output), 4.0f, 1, 1e-4f, &Error), "mip 4 of a constant 4.0 chain: %s", Error);
}

void TestIndirectDispatch(FTestEnv &Env, FTestResult &Result) {
	TRefCountPtr<FRDGPooledBuffer> Counter;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestIndirectDispatch"));
		FRDGBufferRef Args = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), "IndirectArgs");
		FRDGBufferRef CounterBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(4, 1), "Counter");
		{
			TShaderMapRef<FTestWriteIndirectArgsCS> ComputeShader(Env.ShaderMap);
			FTestWriteIndirectArgsCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestWriteIndirectArgsCS::FParameters>();
			PassParameters->GroupCountX = 3;
			PassParameters->IndirectArgsBuffer = GraphBuilder.CreateUAV(Args);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("WriteIndirectArgs"), ComputeShader, PassParameters, FIntVector(1, 1, 1));
		}
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CounterBuffer), 0u);
		{
			TShaderMapRef<FTestCountThreadsCS> ComputeShader(Env.ShaderMap);
			FTestCountThreadsCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestCountThreadsCS::FParameters>();
			PassParameters->CounterBuffer = GraphBuilder.CreateUAV(CounterBuffer);
			PassParameters->IndirectArgs = Args;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CountThreads (indirect)"), ComputeShader, PassParameters, Args, 0);
		}
		GraphBuilder.QueueBufferExtraction(CounterBuffer, &Counter);
		GraphBuilder.Execute();
		RDG_TEST_CHECK(GraphBuilder.GetStats().NumBindingErrors == 0, "binding errors: %d", GraphBuilder.GetStats().NumBindingErrors);
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	const Vector<uint8_t> Data = Env.RD->buffer_get_data(Counter->GetRHI()->GetRID());
	RDG_TEST_CHECK(Data.size() >= 4, "counter readback empty");
	const uint32 Count = *reinterpret_cast<const uint32 *>(Data.ptr());
	RDG_TEST_CHECK(Count == 3 * 64, "indirect dispatch of 3 groups x 64 threads counted %d", Count);
}

void TestRasterAndRenderPassMerging(FTestEnv &Env, FTestResult &Result) {
	for (int32 bMerge = 1; bMerge >= 0; bMerge--) {
		FScopedCVar Merge("r.RDG.MergeRenderPasses", bMerge);
		TRefCountPtr<IPooledRenderTarget> Output;
		uint32 NumMerged = 0;
		{
			FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestRaster"));
			const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(8, 8), PF_FloatRGBA, FClearValueBinding(FLinearColor(1.0f, 0.0f, 0.0f, 1.0f)), TexCreate_RenderTargetable | TexCreate_ShaderResource);
			FRDGTextureRef Target = GraphBuilder.CreateTexture(Desc, "RasterTarget");
			TShaderMapRef<FTestSolidColorPS> PixelShader(Env.ShaderMap);
			const FIntRect Viewport(0, 0, 8, 8);

			FTestSolidColorPS::FParameters *First = GraphBuilder.AllocParameters<FTestSolidColorPS::FParameters>();
			First->Color = FLinearColor(0.25f, 0.5f, 0.0f, 1.0f);
			First->RenderTargets[0] = FRenderTargetBinding(Target, ERenderTargetLoadAction::EClear);
			FPixelShaderUtils::AddFullscreenPass(GraphBuilder, Env.ShaderMap, RDG_EVENT_NAME("SolidColor"), PixelShader, First, Viewport);

			FTestSolidColorPS::FParameters *Second = GraphBuilder.AllocParameters<FTestSolidColorPS::FParameters>();
			Second->Color = FLinearColor(0.25f, 0.0f, 0.5f, 0.0f);
			Second->RenderTargets[0] = FRenderTargetBinding(Target, ERenderTargetLoadAction::ELoad);
			FPixelShaderUtils::AddFullscreenPass(GraphBuilder, Env.ShaderMap, RDG_EVENT_NAME("AdditiveColor"), PixelShader, Second, Viewport, TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI());

			GraphBuilder.QueueTextureExtraction(Target, &Output);
			GraphBuilder.Execute();
			NumMerged = GraphBuilder.GetStats().NumMergedRenderPasses;
			RDG_TEST_CHECK(GraphBuilder.GetStats().NumBindingErrors == 0, "binding errors: %d", GraphBuilder.GetStats().NumBindingErrors);
		}
		Env.RHICmdList->SubmitCommandsAndFlushGPU();
		RDG_TEST_CHECK(NumMerged == (bMerge ? 2u : 0u), "merged render passes: %d (r.RDG.MergeRenderPasses=%d)", NumMerged, bMerge);
		const Vector<uint8_t> Data = Env.RD->texture_get_data(Output->GetRHI()->GetRID(), 0);
		RDG_TEST_CHECK(Data.size() >= 8 * 8 * 8, "raster readback has %d bytes", Data.size());
		const uint16 *Halves = reinterpret_cast<const uint16 *>(Data.ptr());
		const float Expected[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
		for (uint32 Pixel = 0; Pixel < 64; Pixel++) {
			for (uint32 Channel = 0; Channel < 4; Channel++) {
				const float Value = Math::half_to_float(Halves[Pixel * 4 + Channel]);
				RDG_TEST_CHECK(Math::abs(Value - Expected[Channel]) < 1e-3f, "pixel %d channel %d = %f, expected %f (merge=%d)", Pixel, Channel, Value, Expected[Channel], bMerge);
			}
		}
	}
	Result.Message = "clear + opaque draw + additive draw, merged into one RD draw list";
}

struct FTestMeshVertex {
	float Position[2];
	uint8 Color[4];
};

struct FTestMeshInstance {
	float Offset[2];
	float Blue;
};

enum class ETestMeshDraw : uint8 {
	Indexed,
	NonIndexed,
	IndexedIndirect,
	Num
};

void TestVertexAndIndexBuffers(FTestEnv &Env, FTestResult &Result) {
	// A quad over the left half of the viewport; instance 1 moves it to the right half and changes its blue channel.
	const FTestMeshVertex Quad[4] = {
		{ { -1.0f, -1.0f }, { 64, 128, 0, 255 } },
		{ { 0.0f, -1.0f }, { 64, 128, 0, 255 } },
		{ { -1.0f, 1.0f }, { 64, 128, 0, 255 } },
		{ { 0.0f, 1.0f }, { 64, 128, 0, 255 } },
	};
	const FTestMeshVertex Junk = { { 0.5f, 0.5f }, { 255, 0, 255, 255 } };
	// [0, 2): junk, skipped by BaseVertexIndex = 2 of the indexed draws. [2, 6): the quad. [6, 12): the quad as a triangle list (non-indexed draw, BaseVertexIndex = 6).
	LocalVector<FTestMeshVertex> Vertices;
	Vertices.push_back(Junk);
	Vertices.push_back(Junk);
	for (const FTestMeshVertex &Vertex : Quad) Vertices.push_back(Vertex);
	for (uint32 Index : { 0u, 1u, 2u, 2u, 1u, 3u }) Vertices.push_back(Quad[Index]);
	// [0, 3): degenerate triangle, skipped by StartIndex = 3. [3, 9): the quad. [9, 12): padding.
	const LocalVector<uint16> Indices = { 0, 0, 0, 0, 1, 2, 2, 1, 3, 0, 0, 0 };
	const LocalVector<FTestMeshInstance> Instances = { { { 0.0f, 0.0f }, 0.25f }, { { 1.0f, 0.0f }, 0.75f } };

	FVertexDeclarationElementList Elements;
	Elements.push_back(FVertexElement(0, offsetof(FTestMeshVertex, Position), VET_Float2, 0, sizeof(FTestMeshVertex)));
	Elements.push_back(FVertexElement(0, offsetof(FTestMeshVertex, Color), VET_UByte4N, 1, sizeof(FTestMeshVertex)));
	Elements.push_back(FVertexElement(1, offsetof(FTestMeshInstance, Offset), VET_Float2, 2, sizeof(FTestMeshInstance), true));
	Elements.push_back(FVertexElement(1, offsetof(FTestMeshInstance, Blue), VET_Float1, 3, sizeof(FTestMeshInstance), true));
	FRHIVertexDeclaration *VertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	RDG_TEST_CHECK(VertexDeclaration == PipelineStateCache::GetOrCreateVertexDeclaration(Elements), "equal element lists created two vertex declarations");

	FScopedCVar Parallel("r.RDG.ParallelExecute", 1);
	const char *DrawNames[] = { "indexed", "non-indexed", "indexed indirect" };
	for (int32 bParallel = 0; bParallel <= 1; bParallel++) {
		TRefCountPtr<IPooledRenderTarget> Outputs[(uint32)ETestMeshDraw::Num];
		TRefCountPtr<FRDGPooledBuffer> ExtractedVertices, ExtractedIndices;
		{
			FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestVertexAndIndexBuffers"), bParallel ? ERDGBuilderFlags::Parallel : ERDGBuilderFlags::None);
			FRDGBufferRef VertexBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FTestMeshVertex), Vertices.size()), "MeshVertices");
			GraphBuilder.QueueBufferUpload(VertexBuffer, Vertices);
			// The instance stream is written by a compute pass (a vertex buffer with storage usage).
			FRDGBufferRef InstanceBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FTestMeshInstance), Instances.size()), "MeshInstances");
			const uint32 NumInstanceFloats = Instances.size() * sizeof(FTestMeshInstance) / sizeof(float);
			FRDGBufferRef InstanceSource = CreateStructuredBuffer(GraphBuilder, "MeshInstanceSource", sizeof(float), NumInstanceFloats, Instances.ptr(), Instances.size() * sizeof(FTestMeshInstance));
			AddBufferMadPass(GraphBuilder, Env, InstanceSource, InstanceBuffer, NumInstanceFloats, 1.0f, 0.0f);
			FRDGBufferDesc IndexDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint16), Indices.size());
			IndexDesc.Usage |= BUF_IndexBuffer;
			FRDGBufferRef IndexBuffer = GraphBuilder.CreateBuffer(IndexDesc, "MeshIndices");
			GraphBuilder.QueueBufferUpload(IndexBuffer, Indices);
			FRHIDrawIndexedIndirectParameters IndirectParameters;
			IndirectParameters.IndexCountPerInstance = 6;
			IndirectParameters.InstanceCount = 2;
			IndirectParameters.StartIndexLocation = 3;
			IndirectParameters.BaseVertexLocation = 2;
			FRDGBufferRef IndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndexedIndirectParameters>(1), "MeshIndirectArgs");
			GraphBuilder.QueueBufferUpload(IndirectArgs, &IndirectParameters, sizeof(IndirectParameters));

			const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(8, 8), PF_FloatRGBA, FClearValueBinding(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)), TexCreate_RenderTargetable | TexCreate_ShaderResource);
			TShaderMapRef<FTestMeshVS> VertexShader(Env.ShaderMap);
			TShaderMapRef<FTestVertexColorPS> PixelShader(Env.ShaderMap);
			for (uint32 Draw = 0; Draw < (uint32)ETestMeshDraw::Num; Draw++) {
				FRDGTextureRef Target = GraphBuilder.CreateTexture(Desc, "MeshTarget");
				FTestMeshPassParameters *PassParameters = GraphBuilder.AllocParameters<FTestMeshPassParameters>();
				PassParameters->VertexBuffer = VertexBuffer;
				PassParameters->InstanceBuffer = InstanceBuffer;
				if (Draw != (uint32)ETestMeshDraw::NonIndexed) PassParameters->IndexBuffer = IndexBuffer;
				if (Draw == (uint32)ETestMeshDraw::IndexedIndirect) PassParameters->IndirectArgs = IndirectArgs;
				PassParameters->RenderTargets[0] = FRenderTargetBinding(Target, ERenderTargetLoadAction::EClear);
				GraphBuilder.AddPass(RDG_EVENT_NAME("MeshDraw (%s)", DrawNames[Draw]), PassParameters, ERDGPassFlags::Raster, [PassParameters, VertexShader, PixelShader, VertexDeclaration, Draw](FRHICommandList &RHICmdList) {
					RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, 8.0f, 8.0f, 1.0f);
					FGraphicsPipelineStateInitializer GraphicsPSOInit;
					GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
					GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
					GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
					GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = VertexDeclaration;
					GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
					GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
					GraphicsPSOInit.PrimitiveType = PT_TriangleList;
					SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
					RHICmdList.SetStreamSource(0, PassParameters->VertexBuffer->GetRHI(), 0);
					RHICmdList.SetStreamSource(1, PassParameters->InstanceBuffer->GetRHI(), 0);
					switch ((ETestMeshDraw)Draw) {
						case ETestMeshDraw::Indexed:
							RHICmdList.DrawIndexedPrimitive(PassParameters->IndexBuffer->GetRHI(), 2, 0, 4, 3, 2, 2);
							break;
						case ETestMeshDraw::NonIndexed:
							RHICmdList.DrawPrimitive(6, 2, 2);
							break;
						default:
							RHICmdList.DrawIndexedPrimitiveIndirect(PassParameters->IndexBuffer->GetRHI(), PassParameters->IndirectArgs->GetRHI(), 0);
							break;
					}
				});
				GraphBuilder.QueueTextureExtraction(Target, &Outputs[Draw]);
			}
			GraphBuilder.QueueBufferExtraction(VertexBuffer, &ExtractedVertices);
			GraphBuilder.QueueBufferExtraction(IndexBuffer, &ExtractedIndices);
			GraphBuilder.Execute();
			const FRDGBuilderStats &Stats = GraphBuilder.GetStats();
			RDG_TEST_CHECK(Stats.NumBindingErrors == 0, "binding errors: %d (parallel=%d)", Stats.NumBindingErrors, bParallel);
			if (bParallel) RDG_TEST_CHECK(Stats.NumParallelPasses >= (uint32)ETestMeshDraw::Num, "only %d passes were recorded in parallel tasks", Stats.NumParallelPasses);
		}
		RDG_TEST_CHECK(ExtractedVertices->GetRHI()->GetRDType() == ERDBufferType::Vertex, "vertex buffer was not allocated as an RD vertex buffer");
		RDG_TEST_CHECK(ExtractedIndices->GetRHI()->GetRDType() == ERDBufferType::Index, "index buffer was not allocated as an RD index buffer");
		Env.RHICmdList->SubmitCommandsAndFlushGPU();
		for (uint32 Draw = 0; Draw < (uint32)ETestMeshDraw::Num; Draw++) {
			const Vector<uint8_t> Data = Env.RD->texture_get_data(Outputs[Draw]->GetRHI()->GetRID(), 0);
			RDG_TEST_CHECK(Data.size() >= 8 * 8 * 8, "%s draw readback has %d bytes", DrawNames[Draw], Data.size());
			const uint16 *Halves = reinterpret_cast<const uint16 *>(Data.ptr());
			for (uint32 Pixel = 0; Pixel < 64; Pixel++) {
				// Left half: instance 0, right half: instance 1.
				const float Expected[4] = { 64.0f / 255.0f, 128.0f / 255.0f, (Pixel % 8) < 4 ? 0.25f : 0.75f, 1.0f };
				for (uint32 Channel = 0; Channel < 4; Channel++) {
					const float Value = Math::half_to_float(Halves[Pixel * 4 + Channel]);
					RDG_TEST_CHECK(Math::abs(Value - Expected[Channel]) < 2e-3f, "%s draw: pixel (%d, %d) channel %d = %f, expected %f (parallel=%d)", DrawNames[Draw], Pixel % 8, Pixel / 8, Channel, Value, Expected[Channel], bParallel);
				}
			}
		}
	}
	Result.Message = "vertex + instance streams (instances written by a compute pass) with base vertex, start index, indexed / non-indexed / indexed indirect draws, serial and parallel";
}

uint32 CountExternalAccessPasses(const FRDGBuilder &GraphBuilder, ERHIPipeline Pipeline) {
	uint32 Count = 0;
	for (const FRDGPass *Pass : GraphBuilder.GetPasses()) {
		if (Pass->IsExternalAccessPass() && Pass->GetPipeline() == Pipeline && !Pass->IsCulled()) Count++;
	}
	return Count;
}

void RunExternalAccessMode(FTestEnv &Env, FTestResult &Result, ERDGBuilderFlags Flags) {
	TRefCountPtr<IPooledRenderTarget> Out1, Out2, OutAsync;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestExternalAccessMode"), Flags);
		FRDGTextureRef A = GraphBuilder.CreateTexture(MakeR32Desc(), "A");
		FRDGPassRef FillA = nullptr;
		AddFillPass(GraphBuilder, Env, A, 1.0f, false, &FillA);
		RDG_TEST_CHECK(GraphBuilder.ConvertToExternalTexture(A).IsValid() && GraphBuilder.GetTransientAllocation(A).Heap == nullptr, "ConvertToExternalTexture must return a pooled (non transient) texture");

		// Back-to-back external / internal before any pass: backed out, no access mode pass.
		GraphBuilder.UseExternalAccessMode(A, ERHIAccess::SRVCompute);
		GraphBuilder.UseInternalAccessMode(A);
		RDG_TEST_CHECK(CountExternalAccessPasses(GraphBuilder, ERHIPipeline::Graphics) == 0, "a backed out access mode switch added a pass");

		const ERHIPipeline Pipelines = GraphBuilder.IsAsyncComputeEnabled() ? ERHIPipeline::All : ERHIPipeline::Graphics;
		GraphBuilder.UseExternalAccessMode(A, ERHIAccess::SRVCompute, Pipelines);
		GraphBuilder.UseExternalAccessMode(A, ERHIAccess::SRVCompute, Pipelines);
		FRDGTextureRef B = GraphBuilder.CreateTexture(MakeR32Desc(), "B");
		FRDGPassRef ReadExternal = AddAddValuePass(GraphBuilder, Env, A, B, 1.0f);
		FRDGTextureRef C = GraphBuilder.CreateTexture(MakeR32Desc(), "C");
		FRDGPassRef ReadExternalAsync = AddAddValuePass(GraphBuilder, Env, A, C, 2.0f, ERDGPassFlags::AsyncCompute);
		RDG_TEST_CHECK(CountExternalAccessPasses(GraphBuilder, ERHIPipeline::Graphics) == 1, "expected one graphics access mode pass after UseExternalAccessMode");

		GraphBuilder.UseInternalAccessMode(A);
		FRDGPassRef FillAgain = nullptr;
		AddFillPass(GraphBuilder, Env, A, 5.0f, false, &FillAgain);
		FRDGTextureRef D = GraphBuilder.CreateTexture(MakeR32Desc(), "D");
		FRDGPassRef ReadInternal = AddAddValuePass(GraphBuilder, Env, A, D, 1.0f);
		RDG_TEST_CHECK(CountExternalAccessPasses(GraphBuilder, ERHIPipeline::Graphics) == 2, "expected the 'end' access mode pass after UseInternalAccessMode");

		GraphBuilder.QueueTextureExtraction(B, &Out1);
		GraphBuilder.QueueTextureExtraction(D, &Out2);
		GraphBuilder.QueueTextureExtraction(C, &OutAsync);
		GraphBuilder.Execute();
		RDG_TEST_CHECK(GraphBuilder.GetStats().NumValidationErrors == 0, "%d validation errors", GraphBuilder.GetStats().NumValidationErrors);

		// Passes in external access mode don't depend on the producer of A, the access mode pass does.
		RDG_TEST_CHECK(!ReadExternal->GetProducers().has(FillA), "the read in external access mode depends on the producer of A");
		RDG_TEST_CHECK(!ReadExternalAsync->GetProducers().has(FillA) && ReadExternalAsync->GetCrossPipelineProducer().IsNull(), "the async compute read in external access mode depends on the producer of A");
		const FRDGPass *BeginPass = nullptr;
		for (const FRDGPass *Pass : GraphBuilder.GetPasses()) {
			if (Pass->IsExternalAccessPass() && Pass->GetPipeline() == ERHIPipeline::Graphics) {
				BeginPass = Pass;
				break;
			}
		}
		RDG_TEST_CHECK(BeginPass && !BeginPass->IsCulled() && BeginPass->GetProducers().has(FillA), "the access mode pass must read A after its producer");
		if (GraphBuilder.IsAsyncComputeEnabled()) RDG_TEST_CHECK(CountExternalAccessPasses(GraphBuilder, ERHIPipeline::AsyncCompute) == 2, "expected async compute access mode passes for the async compute pipe");
		// Back in internal access mode, the reads depend on the producer again.
		RDG_TEST_CHECK(ReadInternal->GetProducers().has(FillAgain), "the read after UseInternalAccessMode doesn't depend on the producer of A");
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out1), 2.0f, kTestSize * kTestSize, 1e-4f, &Error), "external access read: A (1) + 1 != 2: %s", Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, OutAsync), 3.0f, kTestSize * kTestSize, 1e-4f, &Error), "async compute external access read: A (1) + 2 != 3: %s", Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, Out2), 6.0f, kTestSize * kTestSize, 1e-4f, &Error), "internal access read after the rewrite: A (5) + 1 != 6: %s", Error);

	// Validation: transient resource, writable access, write in external access mode.
	const uint32 ErrorsBefore = RDGPrivate::GetValidationErrorCount();
	print_line("[RDG][TestExternalAccessMode] The following 3 errors are expected:");
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestExternalAccessModeValidation"), Flags);
		FRDGTextureRef Transient = GraphBuilder.CreateTexture(MakeR32Desc(), "Transient");
		AddFillPass(GraphBuilder, Env, Transient, 1.0f);
		GraphBuilder.UseExternalAccessMode(Transient, ERHIAccess::SRVCompute);
		FRDGTextureRef External = GraphBuilder.CreateTexture(MakeR32Desc(), "External");
		AddFillPass(GraphBuilder, Env, External, 1.0f);
		GraphBuilder.ConvertToExternalTexture(External);
		GraphBuilder.UseExternalAccessMode(External, ERHIAccess::UAVCompute);
		GraphBuilder.UseExternalAccessMode(External, ERHIAccess::SRVCompute);
		AddFillPass(GraphBuilder, Env, External, 2.0f);
		GraphBuilder.Execute();
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	const uint32 NumErrors = RDGPrivate::GetValidationErrorCount() - ErrorsBefore;
	RDG_TEST_CHECK(NumErrors == 3, "expected 3 validation errors, got %d (parallel setup=%d)", NumErrors, (int)EnumHasAnyFlags(Flags, ERDGBuilderFlags::ParallelSetup));
}

void TestExternalAccessMode(FTestEnv &Env, FTestResult &Result) {
	// With ParallelSetup the passes are set up by the async setup queue: the access mode switches are replayed there.
	for (const ERDGBuilderFlags Flags : { ERDGBuilderFlags::None, ERDGBuilderFlags::ParallelSetup }) {
		RunExternalAccessMode(Env, Result, Flags);
		if (!Result.bPassed) return;
	}
	Result.Message = "access mode passes, untracked external reads on both pipes, back to internal tracking, validation; serial and async pass setup";
}

FRDGPassRef AddBufferMadPass(FRDGBuilder &GraphBuilder, FTestEnv &Env, FRDGBufferRef Input, FRDGBufferRef Output, uint32 NumElements, float Scale, float Bias) {
	TShaderMapRef<FTestBufferMadCS> ComputeShader(Env.ShaderMap);
	FTestBufferMadCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestBufferMadCS::FParameters>();
	PassParameters->NumElements = NumElements;
	PassParameters->Scale = Scale;
	PassParameters->Bias = Bias;
	PassParameters->InputBuffer = GraphBuilder.CreateSRV(Input);
	PassParameters->OutputBuffer = GraphBuilder.CreateUAV(Output);
	return FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BufferMad(%s -> %s)", Input->Name, Output->Name), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount((int32)NumElements, 64));
}

void TestParallelSetup(FTestEnv &Env, FTestResult &Result) {
	constexpr uint32 NumElements = 1000;
	constexpr int32 NumChainPasses = 64;
	const Thread::ID RenderThreadID = Thread::get_caller_id();
	String Summary;
	for (const ERDGBuilderFlags Flags : { ERDGBuilderFlags::None, ERDGBuilderFlags::ParallelSetup | ERDGBuilderFlags::ParallelCompile }) {
		const bool bParallel = Flags != ERDGBuilderFlags::None;
		// Filled by setup tasks, read by the buffer callbacks when the graph compiles.
		LocalVector<float> SetupData;
		LocalVector<float> DerivedData;
		uint32 SetupNumElements = 0;
		std::atomic<int32> ExecuteValue{ 0 };
		int32 SeenExecuteValue = -1;
		Thread::ID SetupTaskThread = RenderThreadID;
		Thread::ID FillCallbackThread = RenderThreadID;
		bool bFreed = false;
		bool bCulledFreed = false;
		TRefCountPtr<FRDGPooledBuffer> MadOut, FillOut, DerivedOut, OwnedOut;
		TRefCountPtr<IPooledRenderTarget> ChainOut;
		FRDGBuilderStats Stats;
		{
			FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestParallelSetup"), Flags);
			RDG_TEST_CHECK(GraphBuilder.IsParallelSetupEnabled() == bParallel, "IsParallelSetupEnabled() = %d", (int)GraphBuilder.IsParallelSetupEnabled());

			// Setup task (Compile wait point) + a task depending on it.
			UE::Tasks::FTask FillTask = GraphBuilder.AddSetupTask([&]() {
				// Still running when the next task is launched: it really waits for its prerequisite.
				OS::get_singleton()->delay_usec(5000);
				SetupTaskThread = Thread::get_caller_id();
				SetupData.resize(NumElements);
				for (uint32 Index = 0; Index < NumElements; Index++) SetupData[Index] = 0.5f * Index;
				SetupNumElements = NumElements;
			});
			RDG_TEST_CHECK(FillTask.IsValid() == bParallel, "AddSetupTask launched a task: %d", (int)FillTask.IsValid());
			GraphBuilder.AddSetupTask([&]() {
				DerivedData.resize(SetupData.size());
				for (uint32 Index = 0; Index < SetupData.size(); Index++) DerivedData[Index] = SetupData[Index] + 3.0f;
			}, FillTask);

			// Buffers sized and filled by callbacks.
			FRDGBufferRef Input = CreateStructuredBuffer(GraphBuilder, "SetupTaskInput", sizeof(float), [&]() { return SetupNumElements; }, [&]() -> const void * { return SetupData.ptr(); }, [&]() { return (uint64)SetupData.size() * sizeof(float); });
			FRDGBufferRef Output = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 1), "SetupTaskOutput", [&]() { return SetupNumElements; });
			AddBufferMadPass(GraphBuilder, Env, Input, Output, NumElements, 2.0f, 1.0f);
			GraphBuilder.QueueBufferExtraction(Output, &MadOut);

			FRDGBufferRef Derived = CreateStructuredBuffer(GraphBuilder, "DerivedInput", sizeof(float), [&]() { return (uint32)DerivedData.size(); }, [&]() -> const void * { return DerivedData.ptr(); }, [&]() { return (uint64)DerivedData.size() * sizeof(float); });
			FRDGBufferRef DerivedOutput = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumElements), "DerivedOutput");
			AddBufferMadPass(GraphBuilder, Env, Derived, DerivedOutput, NumElements, 1.0f, 0.0f);
			GraphBuilder.QueueBufferExtraction(DerivedOutput, &DerivedOut);

			// Fill callback, and a caller-owned upload with a free callback.
			FRDGBufferRef Filled = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumElements), "Filled");
			GraphBuilder.QueueBufferUpload(Filled, [&](void *Data, uint32 Size) {
				FillCallbackThread = Thread::get_caller_id();
				float *Floats = reinterpret_cast<float *>(Data);
				for (uint32 Index = 0; Index < Size / sizeof(float); Index++) Floats[Index] = 7.0f;
			});
			static const float Owned[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
			FRDGBufferRef OwnedBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 4), "Owned");
			GraphBuilder.QueueBufferUpload(OwnedBuffer, Owned, sizeof(Owned), [&](const void *Data) { bFreed = Data == Owned; });
			FRDGBufferRef FillOutput = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumElements), "FilledOutput");
			AddBufferMadPass(GraphBuilder, Env, Filled, FillOutput, NumElements, 2.0f, 1.0f);
			FRDGBufferRef OwnedOutput = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 4), "OwnedOutput");
			AddBufferMadPass(GraphBuilder, Env, OwnedBuffer, OwnedOutput, 4, 1.0f, 0.0f);
			GraphBuilder.QueueBufferExtraction(FillOutput, &FillOut);
			GraphBuilder.QueueBufferExtraction(OwnedOutput, &OwnedOut);
			// Culled: its upload never happens, the free callback is still called.
			FRDGBufferRef Culled = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 4), "Culled");
			GraphBuilder.QueueBufferUpload(Culled, Owned, sizeof(Owned), [&](const void *) { bCulledFreed = true; });
			AddBufferMadPass(GraphBuilder, Env, Culled, GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 4), "CulledOutput"), 4, 1.0f, 0.0f);

			// Execute wait point: synced before the passes run (still running when Execute() starts).
			GraphBuilder.AddSetupTask([&]() {
				OS::get_singleton()->delay_usec(50000);
				ExecuteValue = 42;
			}, true, ERDGSetupTaskWaitPoint::Execute);
			GraphBuilder.AddPass(RDG_EVENT_NAME("ReadExecuteSetupTask"), ERDGPassFlags::NeverCull, [&](FRHICommandList &) { SeenExecuteValue = ExecuteValue.load(); });

			// Async setup queue: a chain of passes, flushed every 8 passes.
			FRDGTextureRef Texture = GraphBuilder.CreateTexture(MakeR32Desc(), "ChainStart");
			AddFillPass(GraphBuilder, Env, Texture, 0.0f);
			for (int32 Step = 0; Step < NumChainPasses; Step++) {
				FRDGTextureRef Next = GraphBuilder.CreateTexture(MakeR32Desc(), "ChainStep");
				AddAddValuePass(GraphBuilder, Env, Texture, Next, 1.0f);
				Texture = Next;
				if (Step % 8 == 7) GraphBuilder.FlushSetupQueue();
			}
			GraphBuilder.QueueTextureExtraction(Texture, &ChainOut);
			// Gives the queue task the time to run before Execute() sets up what is left.
			if (bParallel) OS::get_singleton()->delay_usec(20000);
			GraphBuilder.Execute();
			Stats = GraphBuilder.GetStats();
		}
		Env.RHICmdList->SubmitCommandsAndFlushGPU();
		const char *Mode = bParallel ? "parallel" : "serial";
		RDG_TEST_CHECK(Stats.NumBindingErrors == 0 && Stats.NumValidationErrors == 0, "[%s] %d binding / %d validation errors", Mode, Stats.NumBindingErrors, Stats.NumValidationErrors);
		RDG_TEST_CHECK(SeenExecuteValue == 42, "[%s] the pass ran before the Execute setup task (saw %d)", Mode, SeenExecuteValue);
		RDG_TEST_CHECK(bFreed && bCulledFreed, "[%s] free callbacks: uploaded %d, culled %d", Mode, (int)bFreed, (int)bCulledFreed);
		if (bParallel) {
			RDG_TEST_CHECK(SetupTaskThread != RenderThreadID, "[%s] the setup task ran on the render thread", Mode);
			RDG_TEST_CHECK(FillCallbackThread != RenderThreadID, "[%s] the fill callback ran on the render thread (no parallel compile task)", Mode);
			RDG_TEST_CHECK(Stats.NumSetupTasks >= 4, "[%s] %d setup tasks launched", Mode, Stats.NumSetupTasks);
			RDG_TEST_CHECK(Stats.NumAsyncSetupPasses > 0, "[%s] no pass was set up by the async setup queue task", Mode);
		} else {
			RDG_TEST_CHECK(SetupTaskThread == RenderThreadID && FillCallbackThread == RenderThreadID && Stats.NumSetupTasks == 0 && Stats.NumAsyncSetupPasses == 0, "[%s] tasks were launched", Mode);
		}
		const Vector<float> Mad = ReadBufferFloats(Env, MadOut);
		RDG_TEST_CHECK((uint32)Mad.size() >= NumElements, "[%s] callback sized buffer has %d floats", Mode, Mad.size());
		for (uint32 Index = 0; Index < NumElements; Index++) RDG_TEST_CHECK(Math::abs(Mad[Index] - (Index + 1.0f)) < 1e-4f, "[%s] callback buffer element %d = %f, expected %f", Mode, Index, Mad[Index], Index + 1.0f);
		const Vector<float> DerivedValues = ReadBufferFloats(Env, DerivedOut);
		for (uint32 Index = 0; Index < NumElements; Index++) RDG_TEST_CHECK(Math::abs(DerivedValues[Index] - (0.5f * Index + 3.0f)) < 1e-4f, "[%s] prerequisite task data element %d = %f, expected %f", Mode, Index, DerivedValues[Index], 0.5f * Index + 3.0f);
		const Vector<float> Filled = ReadBufferFloats(Env, FillOut);
		for (uint32 Index = 0; Index < NumElements; Index++) RDG_TEST_CHECK(Math::abs(Filled[Index] - 15.0f) < 1e-4f, "[%s] fill callback element %d = %f, expected 15", Mode, Index, Filled[Index]);
		const Vector<float> OwnedValues = ReadBufferFloats(Env, OwnedOut);
		for (uint32 Index = 0; Index < 4; Index++) RDG_TEST_CHECK(Math::abs(OwnedValues[Index] - (Index + 1.0f)) < 1e-4f, "[%s] free callback upload element %d = %f, expected %f", Mode, Index, OwnedValues[Index], Index + 1.0f);
		String Error;
		RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, ChainOut), (float)NumChainPasses, kTestSize * kTestSize, 1e-4f, &Error), "[%s] chain of %d passes: %s", Mode, NumChainPasses, Error);
		if (bParallel) Summary = vformat("%d setup tasks, %d of %d passes set up by the async setup queue task", Stats.NumSetupTasks, Stats.NumAsyncSetupPasses, Stats.NumPasses);
	}
	Result.Message = Summary;
}

void TestEventScopes(FTestEnv &Env, FTestResult &Result) {
	FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestEventScopes"));
	FRDGPassRef Pass = nullptr;
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Outer %d", 1);
		{
			RDG_EVENT_SCOPE(GraphBuilder, "Inner");
			Pass = GraphBuilder.AddPass(RDG_EVENT_NAME("Leaf"), ERDGPassFlags::NeverCull, [](FRHICommandList &) {});
		}
	}
	FRDGPassRef Outside = GraphBuilder.AddPass(RDG_EVENT_NAME("Outside"), ERDGPassFlags::NeverCull, [](FRHICommandList &) {});
	GraphBuilder.Execute();
	RDG_TEST_CHECK(Pass->GetScope() && Pass->GetScope()->GetPath() == "Outer 1/Inner", "scope path is '%s'", Pass->GetScope() ? Pass->GetScope()->GetPath() : String("<none>"));
	RDG_TEST_CHECK(Outside->GetScope() == nullptr, "scope leaked out of RDG_EVENT_SCOPE");
}

void RunValidation(FTestEnv &Env, FTestResult &Result, ERDGBuilderFlags Flags) {
	const uint32 ErrorsBefore = RDGPrivate::GetValidationErrorCount();
	print_line("[RDG][TestValidation] The following 3 errors are expected:");
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestValidation"), Flags);
		FRDGTextureRef NeverWritten = GraphBuilder.CreateTexture(MakeR32Desc(), "NeverWritten");
		FRDGTextureRef Out = GraphBuilder.CreateTexture(MakeR32Desc(), "Out");
		FRDGTextureRef Undeclared = GraphBuilder.CreateTexture(MakeR32Desc(), "Undeclared");
		AddFillPass(GraphBuilder, Env, Undeclared, 1.0f);
		// 1. Read dependency on a texture no pass wrote.
		AddAddValuePass(GraphBuilder, Env, NeverWritten, Out, 1.0f);
		// 2. Lambda touches a texture that is not in its parameter struct.
		FTestFillCS::FParameters *PassParameters = GraphBuilder.AllocParameters<FTestFillCS::FParameters>();
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(Out);
		GraphBuilder.AddPass(RDG_EVENT_NAME("UndeclaredAccess"), PassParameters, ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, [Undeclared](FRHIComputeCommandList &) { (void)Undeclared->GetRHI(); });
		// 3. Extraction of a texture that was never produced.
		FRDGTextureRef Orphan = GraphBuilder.CreateTexture(MakeR32Desc(), "Orphan");
		TRefCountPtr<IPooledRenderTarget> OrphanOut;
		GraphBuilder.QueueTextureExtraction(Orphan, &OrphanOut);
		TRefCountPtr<IPooledRenderTarget> Keep;
		GraphBuilder.QueueTextureExtraction(Undeclared, &Keep);
		GraphBuilder.Execute();
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	const uint32 NumErrors = RDGPrivate::GetValidationErrorCount() - ErrorsBefore;
	RDG_TEST_CHECK(NumErrors == 3, "expected 3 validation errors, got %d (parallel setup=%d)", NumErrors, (int)EnumHasAnyFlags(Flags, ERDGBuilderFlags::ParallelSetup));
}

void TestValidation(FTestEnv &Env, FTestResult &Result) {
	// With ParallelSetup the read dependencies are validated on the setup timeline of the user (MarkResourcesAsProduced).
	for (const ERDGBuilderFlags Flags : { ERDGBuilderFlags::None, ERDGBuilderFlags::ParallelSetup }) {
		RunValidation(Env, Result, Flags);
		if (!Result.bPassed) return;
	}
	Result.Message = "read-before-write, undeclared RHI access and unproduced extraction detected; serial and async pass setup";
}

void TestDumpGraph(FTestEnv &Env, FTestResult &Result) {
	{
		FScopedCVar Dump("r.RDG.DumpGraph", 2);
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestDumpGraph"));
		FRDGTextureRef Kept = GraphBuilder.CreateTexture(MakeR32Desc(), "Kept");
		FRDGTextureRef Dropped = GraphBuilder.CreateTexture(MakeR32Desc(), "Dropped");
		AddFillPass(GraphBuilder, Env, Kept, 1.0f);
		AddFillPass(GraphBuilder, Env, Dropped, 2.0f);
		TRefCountPtr<IPooledRenderTarget> Output;
		GraphBuilder.QueueTextureExtraction(Kept, &Output);
		GraphBuilder.Execute();
		RDG_TEST_CHECK(IConsoleManager::Get().FindConsoleVariable("r.RDG.DumpGraph")->GetInt() == 0, "r.RDG.DumpGraph=2 should reset itself after one dump");
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	const String Path = FRDGBuilder::GetLastDumpPath();
	RDG_TEST_CHECK(!Path.is_empty() && FileAccess::exists(Path), "no graph dump written");
	const String Dot = FileAccess::get_file_as_string(Path);
	RDG_TEST_CHECK(Dot.contains("digraph RDG") && Dot.contains("CULLED") && Dot.contains("Fill(Kept"), "dump content unexpected");
	Result.Message = Path;
}

int32 FindSubmission(const Vector<RenderingDevice::AsyncComputeSubmissionInfo> &Submissions, const String &PassName) {
	for (int32 Index = 0; Index < Submissions.size(); Index++) {
		for (const String &Label : Submissions[Index].labels) {
			if (Label.ends_with(PassName)) return Index;
		}
	}
	return -1;
}

String DescribeSubmissions(const Vector<RenderingDevice::AsyncComputeSubmissionInfo> &Submissions, const LocalVector<String> &PassNames) {
	String Description;
	for (int32 Index = 0; Index < Submissions.size(); Index++) {
		const RenderingDevice::AsyncComputeSubmissionInfo &Submission = Submissions[Index];
		String Passes;
		for (uint32 PassIndex = 0; PassIndex < PassNames.size(); PassIndex++) {
			if (FindSubmission(Submissions, PassNames[PassIndex]) == Index) Passes += vformat("%sP%d", Passes.is_empty() ? "" : ",", PassIndex);
		}
		Description += vformat("%s#%d %s[%s]", Description.is_empty() ? "" : " ", Index, Submission.async_compute ? "async" : "main", Passes);
		if (Submission.wait_submission >= 0) Description += vformat("<-#%d", Submission.wait_submission);
	}
	return Description;
}

void TestAsyncComputeForkJoin(FTestEnv &Env, FTestResult &Result) {
	// P0 (graphics) produces A, P1 (async compute) reads A and produces B, P2 (graphics) is independent, P3 (graphics)
	// reads B. Like UE, P1 forks from P0 and joins at P3, and P2 runs in parallel with P1. On the RenderingDevice, P1 must
	// run in an async compute submission waiting for P0's, P2 in a main submission that neither waits for P1 nor is waited
	// by it, and P3 in a main submission that waits for P1. With r.RDG.AsyncCompute=0 the same graph runs on graphics.
	constexpr int32 Size = 256;
	const int32 Pixels = Size * Size;
	String Error;
	for (const int32 AsyncComputeMode : { 0, 1 }) {
		FScopedCVar AsyncCompute("r.RDG.AsyncCompute", AsyncComputeMode);
		TRefCountPtr<IPooledRenderTarget> OutC;
		TRefCountPtr<IPooledRenderTarget> OutD;
		LocalVector<String> PassNames;
		bool bAsyncComputeEnabled = false;
		{
			FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestAsyncComputeForkJoin"));
			bAsyncComputeEnabled = GraphBuilder.IsAsyncComputeEnabled();
			FRDGTextureRef A = GraphBuilder.CreateTexture(MakeR32Desc(Size), "A");
			FRDGTextureRef B = GraphBuilder.CreateTexture(MakeR32Desc(Size), "B");
			FRDGTextureRef C = GraphBuilder.CreateTexture(MakeR32Desc(Size), "C");
			FRDGTextureRef D = GraphBuilder.CreateTexture(MakeR32Desc(Size), "D");
			FRDGPassRef P0 = nullptr;
			FRDGPassRef P2 = nullptr;
			AddFillPass(GraphBuilder, Env, A, 3.0f, false, &P0);
			FRDGPassRef P1 = AddAddValuePass(GraphBuilder, Env, A, B, 2.0f, ERDGPassFlags::AsyncCompute);
			AddFillPass(GraphBuilder, Env, C, 7.0f, false, &P2);
			FRDGPassRef P3 = AddAddValuePass(GraphBuilder, Env, B, D, 1.0f);
			GraphBuilder.QueueTextureExtraction(C, &OutC);
			GraphBuilder.QueueTextureExtraction(D, &OutD);
			GraphBuilder.Execute();

			for (const FRDGPass *Pass : { P0, P1, P2, P3 }) PassNames.push_back(String::utf8(Pass->GetName()));
			const FRDGBuilderStats &Stats = GraphBuilder.GetStats();
			if (bAsyncComputeEnabled) {
				RDG_TEST_CHECK(P1->IsAsyncCompute() && P1->GetPipeline() == ERHIPipeline::AsyncCompute, "P1 should run on the async compute pipe");
				RDG_TEST_CHECK(P1->GetGraphicsForkPass() == P0->GetHandle() && P1->IsAsyncComputeBegin() && P0->IsGraphicsFork(), "P1 should fork from P0 (its graphics producer), fork pass #%d", (int64)P1->GetGraphicsForkPass().GetIndex());
				RDG_TEST_CHECK(P1->GetGraphicsJoinPass() == P3->GetHandle() && P1->IsAsyncComputeEnd() && P3->IsGraphicsJoin(), "P1 should join at P3 (its graphics consumer), join pass #%d", (int64)P1->GetGraphicsJoinPass().GetIndex());
				RDG_TEST_CHECK(!P2->IsGraphicsFork() && !P2->IsGraphicsJoin(), "P2 is independent and should be neither a fork nor a join");
				RDG_TEST_CHECK(Stats.NumAsyncComputePasses == 1 && Stats.NumAsyncComputeForks == 1 && Stats.NumAsyncComputeJoins == 1, "expected 1 async compute pass / fork / join, got %d / %d / %d", Stats.NumAsyncComputePasses, Stats.NumAsyncComputeForks, Stats.NumAsyncComputeJoins);
			} else {
				RDG_TEST_CHECK(!P1->IsAsyncCompute() && EnumHasAnyFlags(P1->GetFlags(), ERDGPassFlags::Compute) && Stats.NumAsyncComputePasses == 0, "without async compute, P1 should become a Compute pass on the graphics pipe");
			}
		}
		Env.RHICmdList->SubmitCommandsAndFlushGPU();
		const Vector<RenderingDevice::AsyncComputeSubmissionInfo> Submissions = Env.RD->async_compute_get_last_frame_submissions();

		RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, OutD), 6.0f, Pixels, 1e-4f, &Error), "r.RDG.AsyncCompute=%d: D = A (3) + 2 + 1: %s", AsyncComputeMode, Error);
		RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, OutC), 7.0f, Pixels, 1e-4f, &Error), "r.RDG.AsyncCompute=%d: C = 7: %s", AsyncComputeMode, Error);
		if (AsyncComputeMode == 0) continue;

		if (!bAsyncComputeEnabled) {
			Result.bSkipped = true;
			Result.Message = "the RenderingDevice has no async compute queue: AsyncCompute passes ran on the graphics queue (results checked)";
			return;
		}

		int32 S0 = FindSubmission(Submissions, PassNames[0]);
		int32 S1 = FindSubmission(Submissions, PassNames[1]);
		int32 S2 = FindSubmission(Submissions, PassNames[2]);
		int32 S3 = FindSubmission(Submissions, PassNames[3]);
		String Description = DescribeSubmissions(Submissions, PassNames);
		bool bHasLabels = false;
		for (const RenderingDevice::AsyncComputeSubmissionInfo &Submission : Submissions) bHasLabels |= !Submission.labels.is_empty();
		if (!bHasLabels) {
			// RD only records labels with Vulkan debug utils (--verbose or validation layers). Every pass records one compute
			// list in this frame, so the passes are identified by the order of the submissions instead.
			RDG_TEST_CHECK(Submissions.size() == 4, "expected 4 submissions (one per pass), got %d", Submissions.size());
			for (const RenderingDevice::AsyncComputeSubmissionInfo &Submission : Submissions) RDG_TEST_CHECK(Submission.command_count == 1, "expected one command per submission");
			S0 = 0;
			S1 = 1;
			S2 = 2;
			S3 = 3;
			Description = "(no debug labels, passes identified by submission order) ";
			for (int32 Index = 0; Index < Submissions.size(); Index++) Description += vformat("%s#%d %s[P%d]%s", Index == 0 ? "" : " ", Index, Submissions[Index].async_compute ? "async" : "main", Index, Submissions[Index].wait_submission >= 0 ? vformat("<-#%d", Submissions[Index].wait_submission) : String());
		}
		RDG_TEST_CHECK(S0 >= 0 && S1 >= 0 && S2 >= 0 && S3 >= 0, "every pass should be in a submission of the last async compute frame: %s", Description);
		RDG_TEST_CHECK(Submissions[S1].async_compute && !Submissions[S0].async_compute && !Submissions[S2].async_compute && !Submissions[S3].async_compute, "only P1 should be on the async compute queue: %s", Description);
		RDG_TEST_CHECK(Submissions[S1].wait_submission == S0, "fork: P1 should wait for P0's submission: %s", Description);
		RDG_TEST_CHECK(S2 != S0 && Submissions[S2].wait_submission != S1 && S2 < S3, "P2 should run in parallel with P1 (neither waited by it nor waiting for it): %s", Description);
		RDG_TEST_CHECK(Submissions[S3].wait_submission == S1, "join: P3 should wait for P1's submission: %s", Description);
		Result.Message = Description;
	}
}

void TestAsyncComputeTransientFences(FTestEnv &Env, FTestResult &Result) {
	// X is last used by the async compute pass P1. Z is first used by P2, a graphics pass between P1's fork (P0) and join
	// (P3): both pipes may use the memory at the same time, so Z must not alias X (UE's FRHITransientAllocationFences).
	// V is first used by P4, after the join, and can take X's memory again.
	FScopedCVar AsyncCompute("r.RDG.AsyncCompute", 1);
	FScopedCVar Transient("r.RDG.TransientAllocator", 1);
	constexpr int32 Size = 256;
	const int32 Pixels = Size * Size;
	TRefCountPtr<IPooledRenderTarget> OutW;
	TRefCountPtr<IPooledRenderTarget> OutZ;
	TRefCountPtr<IPooledRenderTarget> OutV;
	FRHITransientHeapAllocation AllocationX;
	FRHITransientHeapAllocation AllocationZ;
	FRHITransientHeapAllocation AllocationV;
	bool bAsyncComputeEnabled = false;
	FRHITransientResourceAllocator &Allocator = FRDGDeviceContext::Get(Env.RD)->TransientAllocator;
	const uint32 SkipsBefore = Allocator.GetStats().NumAsyncComputeAliasingSkips;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestAsyncComputeTransientFences"));
		bAsyncComputeEnabled = GraphBuilder.IsAsyncComputeEnabled();
		FRDGTextureRef X = GraphBuilder.CreateTexture(MakeR32Desc(Size), "X");
		FRDGTextureRef Y = GraphBuilder.CreateTexture(MakeR32Desc(Size), "Y");
		FRDGTextureRef Z = GraphBuilder.CreateTexture(MakeR32Desc(Size), "Z");
		FRDGTextureRef V = GraphBuilder.CreateTexture(MakeR32Desc(Size), "V");
		FRDGTextureRef W = GraphBuilder.CreateTexture(MakeR32Desc(Size), "W");
		FRDGTextureRef ZCopy = GraphBuilder.CreateTexture(MakeR32Desc(Size), "ZCopy");
		FRDGTextureRef VCopy = GraphBuilder.CreateTexture(MakeR32Desc(Size), "VCopy");
		AddFillPass(GraphBuilder, Env, X, 1.0f);
		AddAddValuePass(GraphBuilder, Env, X, Y, 1.0f, ERDGPassFlags::AsyncCompute);
		AddFillPass(GraphBuilder, Env, Z, 5.0f);
		AddAddValuePass(GraphBuilder, Env, Y, W, 1.0f);
		AddFillPass(GraphBuilder, Env, V, 9.0f);
		AddAddValuePass(GraphBuilder, Env, Z, ZCopy, 0.0f);
		AddAddValuePass(GraphBuilder, Env, V, VCopy, 0.0f);
		GraphBuilder.QueueTextureExtraction(W, &OutW);
		GraphBuilder.QueueTextureExtraction(ZCopy, &OutZ);
		GraphBuilder.QueueTextureExtraction(VCopy, &OutV);
		GraphBuilder.Execute();
		AllocationX = GraphBuilder.GetTransientAllocation(X);
		AllocationZ = GraphBuilder.GetTransientAllocation(Z);
		AllocationV = GraphBuilder.GetTransientAllocation(V);
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();

	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, OutW), 3.0f, Pixels, 1e-4f, &Error), "W = X (1) + 1 + 1: %s", Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, OutZ), 5.0f, Pixels, 1e-4f, &Error), "Z = 5: %s", Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, OutV), 9.0f, Pixels, 1e-4f, &Error), "V = 9: %s", Error);
	if (!IsTransientHeapSupported(Env, Result)) return;
	if (!bAsyncComputeEnabled) {
		Result.bSkipped = true;
		Result.Message = "the RenderingDevice has no async compute queue (results checked)";
		return;
	}

	RDG_TEST_CHECK(AllocationX.IsValid() && AllocationZ.IsValid() && AllocationV.IsValid(), "X, Z and V should be placed in a transient heap");
	RDG_TEST_CHECK(!AllocationZ.Overlaps(AllocationX), "Z [%d, +%d) is used by graphics inside X's async compute interval and must not alias X [%d, +%d)", (int64)AllocationZ.Offset, (int64)AllocationZ.Size, (int64)AllocationX.Offset, (int64)AllocationX.Size);
	RDG_TEST_CHECK(AllocationV.Overlaps(AllocationX), "V [%d, +%d) comes after the join and should alias X [%d, +%d)", (int64)AllocationV.Offset, (int64)AllocationV.Size, (int64)AllocationX.Offset, (int64)AllocationX.Size);
	const uint32 Skips = Allocator.GetStats().NumAsyncComputeAliasingSkips - SkipsBefore;
	RDG_TEST_CHECK(Skips > 0, "the allocator should have skipped X's memory for Z");
	Result.Message = vformat("X at %d, Z at %d (%d placements skipped), V at %d after the join", (int64)AllocationX.Offset, (int64)AllocationZ.Offset, Skips, (int64)AllocationV.Offset);
}

void TestParallelTranslate(FTestEnv &Env, FTestResult &Result) {
	// The RenderingDevice translates the frame's sorted commands into several command buffers recorded by worker tasks and
	// submits them in order (UE's parallel RHI translate). With r.RHICmd.ParallelTranslate.MaxCommandsPerTranslate=-1
	// (always split) the commands are cut into as many chunks as there are tasks, so levels (and their barriers), compute
	// chains, clears, copies and render passes are spread over the chunks. The results must not change, also in a frame
	// with async compute, where the main queue batches around the async compute submissions are translated in parallel.
	if (WorkerThreadPool::get_singleton()->get_thread_count() == 0) {
		Result.bSkipped = true;
		Result.Message = "no worker threads";
		return;
	}
	FScopedCVar Enable("r.RHICmd.ParallelTranslate.Enable", 1);
	struct FVariant {
		int32 MaxCommands;
		const char *Name;
	};
	const FVariant Variants[] = { { -1, "always split" }, { 8, "8 commands per translate" }, { 0, "no limit" } };
	String Summary;
	for (const FVariant &Variant : Variants) {
		FScopedCVar MaxCommands("r.RHICmd.ParallelTranslate.MaxCommandsPerTranslate", Variant.MaxCommands);
		for (const ERDGBuilderFlags Flags : { ERDGBuilderFlags::None, ERDGBuilderFlags::ParallelExecute }) {
			FParallelGraphOutputs Out;
			BuildParallelTestGraph(Env, Flags, Out);
			Env.RHICmdList->SubmitCommandsAndFlushGPU();
			const RenderingDevice::TranslateStats Stats = Env.RD->parallel_translate_get_last_frame_stats();
			const String Name = vformat("%s, %s", Variant.Name, Flags == ERDGBuilderFlags::None ? "serial execute" : "parallel execute");
			RDG_TEST_CHECK((Stats.task_count > 0) == (Variant.MaxCommands != 0), "[%s] %d commands translated with %d worker tasks", Name, Stats.command_count, Stats.task_count);
			CheckParallelTestGraph(Env, Result, Out, Name.utf8().get_data());
			if (!Result.bPassed) return;
			if (Flags == ERDGBuilderFlags::None) Summary += vformat("%s%s: %d commands, %d tasks", Summary.is_empty() ? "" : "; ", Variant.Name, Stats.command_count, Stats.task_count);
		}
	}

	// Fork / join: A -> B on async compute, C in parallel on graphics, D = B + 1 at the join, then a graphics chain.
	FScopedCVar MaxCommands("r.RHICmd.ParallelTranslate.MaxCommandsPerTranslate", -1);
	FScopedCVar AsyncCompute("r.RDG.AsyncCompute", 1);
	constexpr int32 Size = 256;
	constexpr int32 ChainLength = 8;
	TRefCountPtr<IPooledRenderTarget> OutC;
	TRefCountPtr<IPooledRenderTarget> OutE;
	bool bAsyncComputeEnabled = false;
	{
		FRDGBuilder GraphBuilder(*Env.RHICmdList, RDG_EVENT_NAME("TestParallelTranslate"));
		bAsyncComputeEnabled = GraphBuilder.IsAsyncComputeEnabled();
		FRDGTextureRef A = GraphBuilder.CreateTexture(MakeR32Desc(Size), "A");
		FRDGTextureRef B = GraphBuilder.CreateTexture(MakeR32Desc(Size), "B");
		FRDGTextureRef C = GraphBuilder.CreateTexture(MakeR32Desc(Size), "C");
		FRDGTextureRef E = GraphBuilder.CreateTexture(MakeR32Desc(Size), "D");
		AddFillPass(GraphBuilder, Env, A, 3.0f);
		AddAddValuePass(GraphBuilder, Env, A, B, 2.0f, ERDGPassFlags::AsyncCompute);
		AddFillPass(GraphBuilder, Env, C, 7.0f);
		AddAddValuePass(GraphBuilder, Env, B, E, 1.0f);
		for (int32 Step = 0; Step < ChainLength; Step++) {
			FRDGTextureRef Next = GraphBuilder.CreateTexture(MakeR32Desc(Size), "E");
			AddAddValuePass(GraphBuilder, Env, E, Next, 1.0f);
			E = Next;
		}
		GraphBuilder.QueueTextureExtraction(C, &OutC);
		GraphBuilder.QueueTextureExtraction(E, &OutE);
		GraphBuilder.Execute();
	}
	Env.RHICmdList->SubmitCommandsAndFlushGPU();
	const RenderingDevice::TranslateStats Stats = Env.RD->parallel_translate_get_last_frame_stats();
	const int32 NumSubmissions = Env.RD->async_compute_get_last_frame_submissions().size();
	String Error;
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, OutE), 6.0f + ChainLength, Size * Size, 1e-4f, &Error), "async compute frame: E = A (3) + 2 + 1 + %d: %s", ChainLength, Error);
	RDG_TEST_CHECK(AllNear(ReadTextureFloats(Env, OutC), 7.0f, Size * Size, 1e-4f, &Error), "async compute frame: C = 7: %s", Error);
	RDG_TEST_CHECK(Stats.task_count > 0, "async compute frame: %d commands translated with %d worker tasks", Stats.command_count, Stats.task_count);
	RDG_TEST_CHECK(!bAsyncComputeEnabled || NumSubmissions >= 3, "async compute frame: expected main / async / main submissions, got %d", NumSubmissions);
	Result.Message = vformat("%s; async compute frame (%s): %d commands, %d tasks, %d submissions", Summary, bAsyncComputeEnabled ? "async compute queue" : "no async compute queue", Stats.command_count, Stats.task_count, NumSubmissions);
}

const FTestEntry GTests[] = {
	{ "ShaderParameterMetadata", &TestShaderParameterMetadata, false },
	{ "Blackboard", &TestBlackboard, false },
	{ "PermutationDomain", &TestPermutationDomain, false },
	{ "ComputeChain", &TestComputeChain, true },
	{ "ImmediateMode", &TestImmediateMode, true },
	{ "PassCulling", &TestPassCulling, true },
	{ "PassCullingDisabled", &TestPassCullingDisabled, true },
	{ "ClearUnusedGraphResources", &TestClearUnusedGraphResources, true },
	{ "TransientAliasing", &TestTransientAliasing, true },
	{ "TransientHeapAliasing", &TestTransientHeapAliasing, true },
	{ "ParallelExecute", &TestParallelExecute, true },
	{ "ParallelExecuteAliasing", &TestParallelExecuteAliasing, true },
	{ "ParallelExecuteTiming", &TestParallelExecuteTiming, true },
	{ "TransientBufferAliasing", &TestTransientBufferAliasing, true },
	{ "TransientHeapCache", &TestTransientHeapCache, true },
	{ "CrossFramePooling", &TestCrossFramePooling, true },
	{ "ExtractionAndExternal", &TestExtractionAndExternal, true },
	{ "BufferUploadAndUAV", &TestBufferUploadAndUAV, true },
	{ "BufferClearAndReadback", &TestBufferClearAndReadback, true },
	{ "UniformBuffer", &TestUniformBuffer, true },
	{ "ShaderPermutations", &TestShaderPermutations, true },
	{ "MipViews", &TestMipViews, true },
	{ "IndirectDispatch", &TestIndirectDispatch, true },
	{ "RasterAndRenderPassMerging", &TestRasterAndRenderPassMerging, true },
	{ "VertexAndIndexBuffers", &TestVertexAndIndexBuffers, true },
	{ "ExternalAccessMode", &TestExternalAccessMode, true },
	{ "ParallelSetup", &TestParallelSetup, true },
	{ "ParallelSetupTiming", &TestParallelSetupTiming, true },
	{ "EventScopes", &TestEventScopes, true },
	{ "Validation", &TestValidation, true },
	{ "DumpGraph", &TestDumpGraph, true },
	{ "AsyncComputeForkJoin", &TestAsyncComputeForkJoin, true },
	{ "AsyncComputeTransientFences", &TestAsyncComputeTransientFences, true },
	{ "ParallelTranslate", &TestParallelTranslate, true },
};

} //namespace

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Script API.

void RenderGraphTests::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_test_names"), &RenderGraphTests::get_test_names);
	ClassDB::bind_method(D_METHOD("run_all"), &RenderGraphTests::run_all);
	ClassDB::bind_method(D_METHOD("run_test", "name"), &RenderGraphTests::run_test);
}

PackedStringArray RenderGraphTests::get_test_names() const {
	PackedStringArray Names;
	for (const FTestEntry &Entry : GTests) Names.push_back(Entry.Name);
	return Names;
}

static Dictionary RunTests(const String &p_filter) {
	Dictionary Summary;
	Array Results;
	int32 NumPassed = 0;
	int32 NumFailed = 0;
	int32 NumSkipped = 0;

	RenderingServer *RS = RenderingServer::get_singleton();
	RenderingDevice *RD = RS ? RS->create_local_rendering_device() : nullptr;
	FRHICommandListImmediate *RHICmdList = RD ? memnew(FRHICommandListImmediate(RD)) : nullptr;

	FTestEnv Env;
	Env.RD = RD;
	Env.RHICmdList = RHICmdList;
	Env.ShaderMap = RD ? GetGlobalShaderMap(RD) : nullptr;

	for (const FTestEntry &Entry : GTests) {
		if (!p_filter.is_empty() && p_filter != Entry.Name) continue;
		FTestResult TestResult;
		const uint64 Start = OS::get_singleton()->get_ticks_usec();
		if (Entry.bNeedsGPU && !RD) {
			TestResult.bSkipped = true;
			TestResult.Message = "no RenderingDevice (headless); run without --headless and with --rendering-driver vulkan";
		} else {
			Entry.Function(Env, TestResult);
		}
		const double TimeMs = (OS::get_singleton()->get_ticks_usec() - Start) / 1000.0;

		Dictionary Item;
		Item["name"] = Entry.Name;
		Item["status"] = TestResult.bSkipped ? "SKIP" : (TestResult.bPassed ? "PASS" : "FAIL");
		Item["message"] = TestResult.Message;
		Item["time_ms"] = TimeMs;
		Results.push_back(Item);
		if (TestResult.bSkipped) NumSkipped++;
		else if (TestResult.bPassed) NumPassed++;
		else NumFailed++;
		print_line(vformat("[RDG][%s] %s (%.1f ms) %s", Item["status"], Entry.Name, TimeMs, TestResult.Message));
	}

	if (RD) {
		RHICmdList->SubmitCommandsAndFlushGPU();
		memdelete(RHICmdList);
		FRDGDeviceContext::Release(RD);
		memdelete(RD);
	}

	Summary["passed"] = NumPassed;
	Summary["failed"] = NumFailed;
	Summary["skipped"] = NumSkipped;
	Summary["results"] = Results;
	Summary["device"] = RD ? String("local RenderingDevice") : String("none");
	return Summary;
}

Dictionary RenderGraphTests::run_all() {
	return RunTests(String());
}

Dictionary RenderGraphTests::run_test(const String &p_name) {
	return RunTests(p_name);
}

// Compute shaders used by RenderGraphTests. Several entry points share one file like a UE .usf;
// each entry point's resources are guarded by `#ifdef <EntryPoint>` (the entry point name is #defined to main).

#include "/Engine/Private/Common.glsl"

#ifdef AddValueCS
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2D InputTexture;
layout(set = 0, binding = 1, r32f) uniform restrict writeonly image2D OutputTexture;
layout(push_constant, std430) uniform FParameters {
	ivec2 Size;
	float Value;
};
void AddValueCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, Size))) return;
	imageStore(OutputTexture, Pixel, vec4(texelFetch(InputTexture, Pixel, 0).r + Value));
}
#endif

#ifdef FillCS
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, r32f) uniform restrict writeonly image2D OutputTexture;
layout(push_constant, std430) uniform FParameters {
	ivec2 Size;
	float Value;
};
void FillCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, Size))) return;
	float Result = Value;
#if USE_OFFSET
	Result += 100.0;
#endif
	imageStore(OutputTexture, Pixel, vec4(Result));
}
#endif

#ifdef BufferMadCS
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) restrict readonly buffer FInputBuffer {
	float Data[];
} InputBuffer;
layout(set = 0, binding = 1, std430) restrict writeonly buffer FOutputBuffer {
	float Data[];
} OutputBuffer;
layout(push_constant, std430) uniform FParameters {
	uint NumElements;
	float Scale;
	float Bias;
};
void BufferMadCS() {
	const uint Index = gl_GlobalInvocationID.x;
	if (Index >= NumElements) return;
	OutputBuffer.Data[Index] = InputBuffer.Data[Index] * Scale + Bias;
}
#endif

#ifdef DownsampleCS
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2D SourceMip;
layout(set = 0, binding = 1, r32f) uniform restrict writeonly image2D DestMip;
layout(push_constant, std430) uniform FParameters {
	ivec2 DestSize;
};
void DownsampleCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, DestSize))) return;
	const ivec2 Source = Pixel * 2;
	const float Sum = texelFetch(SourceMip, Source, 0).r + texelFetch(SourceMip, Source + ivec2(1, 0), 0).r + texelFetch(SourceMip, Source + ivec2(0, 1), 0).r + texelFetch(SourceMip, Source + ivec2(1, 1), 0).r;
	imageStore(DestMip, Pixel, vec4(Sum * 0.25));
}
#endif

#ifdef WriteIndirectArgsCS
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) restrict writeonly buffer FIndirectArgs {
	uint Data[];
} IndirectArgsBuffer;
layout(push_constant, std430) uniform FParameters {
	uint GroupCountX;
};
void WriteIndirectArgsCS() {
	IndirectArgsBuffer.Data[0] = GroupCountX;
	IndirectArgsBuffer.Data[1] = 1;
	IndirectArgsBuffer.Data[2] = 1;
}
#endif

#ifdef CountThreadsCS
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) restrict buffer FCounter {
	uint Count;
} CounterBuffer;
void CountThreadsCS() {
	atomicAdd(CounterBuffer.Count, 1u);
}
#endif

#ifdef UniformBufferCS
layout(local_size_x = 8, local_size_y = 8) in;
// Uniform buffer created with GraphBuilder.CreateUniformBuffer(); shader variable name "TestUB".
layout(set = 0, binding = 0, std140) uniform FTestUniformBufferBlock {
	vec4 Scale;
	float Offset;
	int Mode;
} TestUB;
// Resource members of a uniform buffer are separate descriptors named "<UB>_<Member>".
layout(set = 0, binding = 1) uniform texture2D TestUB_SourceTexture;
layout(set = 0, binding = 2, r32f) uniform restrict writeonly image2D OutputTexture;
layout(push_constant, std430) uniform FParameters {
	ivec2 Size;
};
void UniformBufferCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, Size))) return;
	const float Source = texelFetch(TestUB_SourceTexture, Pixel, 0).r;
	imageStore(OutputTexture, Pixel, vec4(Source * TestUB.Scale.x + TestUB.Offset + float(TestUB.Mode)));
}
#endif

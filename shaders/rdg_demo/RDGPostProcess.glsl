// Post process shaders of RDGCompositorEffect, addressed from C++ as "/Project/rdg_demo/RDGPostProcess.glsl"
// (UE-style virtual path, maps to res://shaders/). One file, several entry points selected with
// IMPLEMENT_GLOBAL_SHADER(..., "EntryPoint", SF_xxx); `#ifdef EntryPoint` guards each entry point's resources.
// Parameters bind by name to the C++ BEGIN_SHADER_PARAMETER_STRUCT members (descriptors and push constants).

#include "/Engine/Private/Common.glsl"

#ifdef BloomSetupCS
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2D SceneColorTexture;
layout(set = 0, binding = 1) uniform sampler SceneColorSampler;
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D OutputTexture;
layout(push_constant, std430) uniform FParameters {
	vec2 OutputInvSize;
	float BloomThreshold;
};

void BloomSetupCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, imageSize(OutputTexture)))) return;
	const vec2 UV = (vec2(Pixel) + 0.5) * OutputInvSize;
	const vec3 Color = texture(sampler2D(SceneColorTexture, SceneColorSampler), UV).rgb;
	const float Lum = Luminance(Color);
	const float Scale = max(Lum - BloomThreshold, 0.0) / max(Lum, 1e-4);
	imageStore(OutputTexture, Pixel, vec4(min(Color * Scale, vec3(64.0)), 1.0));
}
#endif

#ifdef BloomDownsampleCS
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2D InputTexture;
layout(set = 0, binding = 1) uniform sampler InputSampler;
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D OutputTexture;
layout(push_constant, std430) uniform FParameters {
	vec2 OutputInvSize;
	vec2 InputInvSize;
};

void BloomDownsampleCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, imageSize(OutputTexture)))) return;
	const vec2 UV = (vec2(Pixel) + 0.5) * OutputInvSize;
	const vec2 Offset = InputInvSize;
	// 4 bilinear taps = 4x4 box filter.
	vec3 Sum = texture(sampler2D(InputTexture, InputSampler), UV + vec2(-Offset.x, -Offset.y)).rgb;
	Sum += texture(sampler2D(InputTexture, InputSampler), UV + vec2(Offset.x, -Offset.y)).rgb;
	Sum += texture(sampler2D(InputTexture, InputSampler), UV + vec2(-Offset.x, Offset.y)).rgb;
	Sum += texture(sampler2D(InputTexture, InputSampler), UV + vec2(Offset.x, Offset.y)).rgb;
	imageStore(OutputTexture, Pixel, vec4(Sum * 0.25, 1.0));
}
#endif

#if PIXELSHADER
#ifdef BloomUpsamplePS
layout(location = 0) in vec2 UV;
layout(location = 0) out vec4 OutColor;
layout(set = 0, binding = 0) uniform texture2D BloomDownTexture;
layout(set = 0, binding = 1) uniform sampler BloomSampler;
#if HAS_PREVIOUS
layout(set = 0, binding = 2) uniform texture2D PreviousUpTexture;
#endif
layout(push_constant, std430) uniform FParameters {
	vec2 PreviousInvSize;
	float Radius;
};

void BloomUpsamplePS() {
	vec3 Color = texture(sampler2D(BloomDownTexture, BloomSampler), UV).rgb;
#if HAS_PREVIOUS
	// 3x3 tent filter on the lower resolution level.
	const vec2 Offset = PreviousInvSize * Radius;
	vec3 Up = texture(sampler2D(PreviousUpTexture, BloomSampler), UV).rgb * 4.0;
	Up += texture(sampler2D(PreviousUpTexture, BloomSampler), UV + vec2(Offset.x, 0.0)).rgb * 2.0;
	Up += texture(sampler2D(PreviousUpTexture, BloomSampler), UV - vec2(Offset.x, 0.0)).rgb * 2.0;
	Up += texture(sampler2D(PreviousUpTexture, BloomSampler), UV + vec2(0.0, Offset.y)).rgb * 2.0;
	Up += texture(sampler2D(PreviousUpTexture, BloomSampler), UV - vec2(0.0, Offset.y)).rgb * 2.0;
	Up += texture(sampler2D(PreviousUpTexture, BloomSampler), UV + Offset).rgb;
	Up += texture(sampler2D(PreviousUpTexture, BloomSampler), UV - Offset).rgb;
	Up += texture(sampler2D(PreviousUpTexture, BloomSampler), UV + vec2(Offset.x, -Offset.y)).rgb;
	Up += texture(sampler2D(PreviousUpTexture, BloomSampler), UV + vec2(-Offset.x, Offset.y)).rgb;
	Color += Up / 16.0;
#endif
	OutColor = vec4(Color, 1.0);
}
#endif
#endif

#ifdef TrailCS
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2D SceneColorTexture;
layout(set = 0, binding = 1) uniform texture2D HistoryTexture;
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D OutputTexture;
layout(push_constant, std430) uniform FParameters {
	float HistoryWeight;
	int bHasHistory;
};

void TrailCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, imageSize(OutputTexture)))) return;
	const vec4 Scene = texelFetch(SceneColorTexture, Pixel, 0);
	vec3 Result = Scene.rgb;
	if (bHasHistory != 0) {
		const vec3 History = texelFetch(HistoryTexture, Pixel, 0).rgb;
		// Keep bright history around longer than dark history: light streaks.
		Result = max(Scene.rgb, History * HistoryWeight);
	}
	imageStore(OutputTexture, Pixel, vec4(Result, Scene.a));
}
#endif

#ifdef CompositeCS
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba16f) uniform restrict image2D SceneColorOutput;
layout(set = 0, binding = 1) uniform texture2D BloomTexture;
layout(set = 0, binding = 2) uniform sampler BloomSampler;
#if USE_TRAIL
layout(set = 0, binding = 3) uniform texture2D TrailTexture;
#endif
layout(push_constant, std430) uniform FParameters {
	vec2 InvSize;
	float BloomIntensity;
	int DebugView;
};

void CompositeCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, imageSize(SceneColorOutput)))) return;
	const vec2 UV = (vec2(Pixel) + 0.5) * InvSize;
	vec4 Scene = imageLoad(SceneColorOutput, Pixel);
#if USE_TRAIL
	Scene.rgb = texelFetch(TrailTexture, Pixel, 0).rgb;
#endif
	const vec3 Bloom = texture(sampler2D(BloomTexture, BloomSampler), UV).rgb;
	vec3 Result = Scene.rgb + Bloom * BloomIntensity;
	if (DebugView != 0) Result = Bloom;
	imageStore(SceneColorOutput, Pixel, vec4(Result, Scene.a));
}
#endif

#ifdef LuminanceHistogramCS
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform texture2D SceneColorTexture;
layout(set = 0, binding = 1, std430) restrict buffer FHistogram {
	uint Bins[];
} HistogramBuffer;

void LuminanceHistogramCS() {
	const ivec2 Pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(Pixel, textureSize(SceneColorTexture, 0)))) return;
	const float Lum = Luminance(texelFetch(SceneColorTexture, Pixel, 0).rgb);
	const uint Bin = uint(clamp(log2(max(Lum, 1e-5)) * 4.0 + 32.0, 0.0, 63.0));
	atomicAdd(HistogramBuffer.Bins[Bin], 1u);
}
#endif

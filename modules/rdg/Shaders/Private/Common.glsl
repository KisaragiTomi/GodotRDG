#pragma once
// Small subset of UE's Common.ush for RDG shaders: #include "/Engine/Private/Common.glsl"

#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define lerp mix
#define saturate(x) clamp((x), 0.0, 1.0)
#define frac fract
#define rsqrt inversesqrt

float Luminance(vec3 LinearColor) {
	return dot(LinearColor, vec3(0.3, 0.59, 0.11));
}

float Pow2(float X) {
	return X * X;
}

vec3 Pow2(vec3 X) {
	return X * X;
}

float Square(float X) {
	return X * X;
}

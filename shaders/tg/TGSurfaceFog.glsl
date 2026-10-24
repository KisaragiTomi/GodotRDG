// Native volumetric fog composited onto the unfogged opaque surfaces for TGSurfaceFog (modules/tg_gi), addressed from
// C++ as "/Project/tg/TGSurfaceFog.glsl". Drawn with FPixelShaderUtils::AddFullscreenPass into Godot's (MSAA) color
// target, before the native sky and transparency. Fixed-function blend: scattering + surface_radiance * transmittance.

#if PIXELSHADER
#ifdef TGSurfaceFogPS
layout(location = 0) out vec4 OutColor;
layout(set = 0, binding = 0) uniform sampler3D FogTexture;
#if FOG_MSAA
layout(set = 0, binding = 1) uniform sampler2DMS DepthTexture;
#else
layout(set = 0, binding = 1) uniform sampler2D DepthTexture;
#endif
layout(push_constant, std430) uniform FParameters {
	mat4 InverseProjection;
	vec4 ViewportAndFog; // width, height, native fog length, detail spread
};

void TGSurfaceFogPS() {
	ivec2 xy = ivec2(gl_FragCoord.xy);
#if FOG_MSAA
	// gl_SampleID makes the shader run per sample.
	float depth = texelFetch(DepthTexture, xy, gl_SampleID).r;
#else
	float depth = texelFetch(DepthTexture, xy, 0).r;
#endif
	// The native sky pass runs afterwards and owns uncovered samples.
	if (depth <= 0.000001) discard;
	vec2 uv = gl_FragCoord.xy / ViewportAndFog.xy;
	vec4 view = InverseProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	float z = max(-view.z / view.w, 0.0) / ViewportAndFog.z;
	if (z < 1.0) z = pow(z, 1.0 / ViewportAndFog.w);
	vec4 fog = textureLod(FogTexture, vec3(uv, z), 0.0);
	// Match Forward+'s half-precision fog packing before the final blend.
	OutColor = vec4(unpackHalf2x16(packHalf2x16(fog.rg)), unpackHalf2x16(packHalf2x16(fog.ba)));
}
#endif
#endif

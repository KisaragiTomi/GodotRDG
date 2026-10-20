// Raster shaders used by RenderGraphTests. SolidColorPS / UVGradientPS pair with FScreenVertexShaderVS,
// VertexColorPS with MeshVS (vertex streams).

#if VERTEXSHADER

#ifdef MeshVS
layout(location = 0) in vec2 InPosition; // Stream 0, per vertex.
layout(location = 1) in vec4 InColor; // Stream 0, per vertex (UByte4N).
layout(location = 2) in vec2 InInstanceOffset; // Stream 1, per instance.
layout(location = 3) in float InInstanceBlue; // Stream 1, per instance.
layout(location = 0) out vec4 Color;
void MeshVS() {
	gl_Position = vec4(InPosition + InInstanceOffset, 0.0, 1.0);
	Color = vec4(InColor.rg, InInstanceBlue, InColor.a);
}
#endif

#endif

#if PIXELSHADER

layout(location = 0) out vec4 OutColor;

#ifdef SolidColorPS
layout(location = 0) in vec2 UV;
layout(push_constant, std430) uniform FParameters {
	vec4 Color;
};
void SolidColorPS() {
	OutColor = Color;
}
#endif

#ifdef UVGradientPS
layout(location = 0) in vec2 UV;
void UVGradientPS() {
	OutColor = vec4(UV, 0.0, 1.0);
}
#endif

#ifdef VertexColorPS
layout(location = 0) in vec4 Color;
void VertexColorPS() {
	OutColor = Color;
}
#endif

#endif

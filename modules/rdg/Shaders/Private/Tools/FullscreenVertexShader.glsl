// Fullscreen triangle vertex shader (UE: FullscreenVertexShader.usf / FScreenVertexShaderVS).
// Pixel shaders receive the UV at location 0:  layout(location = 0) in vec2 UV;
// UV (0, 0) is the top-left corner, like UE.

#if VERTEXSHADER

layout(location = 0) out vec2 OutUV;

void MainVS() {
	const vec2 Positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
	const vec2 Position = Positions[gl_VertexIndex % 3];
	OutUV = Position * 0.5 + 0.5;
	gl_Position = vec4(Position, 0.0, 1.0);
}

#endif

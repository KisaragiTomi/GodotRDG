// Clear helpers used by AddClearUAVPass (UE: ClearReplacementShaders.usf).

#if COMPUTESHADER

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict writeonly buffer FClearUAVBuffer {
	uint Data[];
} ClearUAV;

layout(push_constant, std430) uniform FClearParameters {
	uint ClearValue;
	uint NumEntries;
};

void ClearBufferCS() {
	const uint Index = gl_GlobalInvocationID.x;
	if (Index >= NumEntries) return;
	ClearUAV.Data[Index] = ClearValue;
}

#endif

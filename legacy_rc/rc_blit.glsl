#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D src_tex;
layout(rgba16f, set = 0, binding = 1) uniform writeonly image2D dst_tex;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 _pad;
} params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.size.x || pixel.y >= params.size.y)
		return;
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.size);
	imageStore(dst_tex, pixel, texture(src_tex, uv));
}

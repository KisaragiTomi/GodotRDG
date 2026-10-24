#[compute]
#version 450

#include "rc_probe.glslinc"

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform image2D cascade_atlas;

layout(push_constant, std430) uniform Params {
	int dst_offset_x;
	int dst_offset_y;
	int src_offset_x;
	int src_offset_y;
	int dst_probes_x;
	int dst_probes_y;
	int src_probes_x;
	int src_probes_y;
	int dst_dirs;
	int src_dirs;
	int dst_spacing;
	int src_spacing;
	ivec2 screen_size;
	int dst_cascade;
	int src_cascade;
} params;

void main() {
	int dst_probe_x = int(gl_GlobalInvocationID.x) / params.dst_dirs;
	int dst_dir = int(gl_GlobalInvocationID.x) % params.dst_dirs;
	int dst_probe_y = int(gl_GlobalInvocationID.y);

	if (dst_probe_x >= params.dst_probes_x || dst_probe_y >= params.dst_probes_y)
		return;

	vec2 dst_position = legacy_probe_position(ivec2(dst_probe_x, dst_probe_y), params.dst_cascade,
			params.dst_spacing, params.screen_size);
	ivec2 src_center = ivec2(floor(dst_position / float(params.src_spacing)));

	int src_dir_base = dst_dir * (params.src_dirs / params.dst_dirs);
	int dirs_to_avg = params.src_dirs / params.dst_dirs;

	vec4 far_interval = vec4(0.0);
	float weight_total = 0.0;

	// 3x3 covers the tent support for the shared ±0.15-spacing jitter.
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 sp = src_center + ivec2(dx, dy);
			if (sp.x < 0 || sp.y < 0 || sp.x >= params.src_probes_x || sp.y >= params.src_probes_y)
				continue;

			vec2 src_position = legacy_probe_position(sp, params.src_cascade,
					params.src_spacing, params.screen_size);
			float w = legacy_probe_weight(dst_position, src_position, params.src_spacing);
			if (w <= 0.0) continue;

			vec4 dir_accum = vec4(0.0);
			for (int d = 0; d < dirs_to_avg; d++) {
				ivec2 src_pos = ivec2(
					params.src_offset_x + sp.x * params.src_dirs + src_dir_base + d,
					params.src_offset_y + sp.y
				);
				dir_accum += imageLoad(cascade_atlas, src_pos);
			}
			dir_accum /= float(dirs_to_avg);

			far_interval += w * dir_accum;
			weight_total += w;
		}
	}

	if (weight_total > 0.001)
		far_interval /= weight_total;
	else
		far_interval = vec4(0.0, 0.0, 0.0, 1.0); // No sampled light; preserve transmittance.

	ivec2 dst_pos = ivec2(
		params.dst_offset_x + dst_probe_x * params.dst_dirs + dst_dir,
		params.dst_offset_y + dst_probe_y
	);
	vec4 near_interval = imageLoad(cascade_atlas, dst_pos);

	vec4 merged;
	merged.rgb = near_interval.rgb + near_interval.a * far_interval.rgb;
	merged.a = near_interval.a * far_interval.a;

	imageStore(cascade_atlas, dst_pos, merged);
}

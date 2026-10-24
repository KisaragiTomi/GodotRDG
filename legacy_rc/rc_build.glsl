#[compute]
#version 450

#include "rc_probe.glslinc"

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D color_tex;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(rgba16f, set = 0, binding = 2) uniform writeonly image2D cascade_out;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	int cascade_index;
	int num_angular_dirs;
	int probe_spacing;
	float interval_start;
	float interval_end;
	float z_near;
	float z_far;
	int atlas_offset_x;
	int atlas_offset_y;
	int probes_x;
	int probes_y;
	float depth_precision;
	float z_thickness;
	int _pad0;
} params;

const float PI = 3.14159265358979;
const float DEPTH_BIAS = 0.0005;

float linearize_depth(float d) {
	return params.z_near * params.z_far / (params.z_near + d * (params.z_far - params.z_near));
}

void main() {
	int probe_x = int(gl_GlobalInvocationID.x) / params.num_angular_dirs;
	int dir_idx = int(gl_GlobalInvocationID.x) % params.num_angular_dirs;
	int probe_y = int(gl_GlobalInvocationID.y);

	if (probe_x >= params.probes_x || probe_y >= params.probes_y)
		return;

	vec2 probe_pos = legacy_probe_position(ivec2(probe_x, probe_y), params.cascade_index,
			params.probe_spacing, params.screen_size);

	// Fixed angular bin centers match the merge/apply direction indexing.
	float angle = (float(dir_idx) + 0.5) / float(params.num_angular_dirs) * 2.0 * PI;
	vec2 ray_dir = vec2(cos(angle), sin(angle));

	ivec2 probe_texel = clamp(ivec2(probe_pos), ivec2(0), params.screen_size - 1);
	float probe_raw = texelFetch(depth_tex, probe_texel, 0).r;
	float probe_z = linearize_depth(probe_raw);

	vec3 accum_rad = vec3(0.0);
	float transparency = 1.0;

	float t = max(params.interval_start, 1.0);
	float cascade_scale = float(1 << params.cascade_index);
	float base_step = max(1.0, cascade_scale * 0.5);
	float step_sz = base_step;
	int max_steps = int(min(128.0, (params.interval_end - t) / base_step + 16.0));
	int steps = 0;

	while (t < params.interval_end && transparency > 0.02 && steps < max_steps) {
		vec2 sp = probe_pos + ray_dir * t;

		if (sp.x < 0.0 || sp.y < 0.0 ||
			sp.x >= float(params.screen_size.x) ||
			sp.y >= float(params.screen_size.y))
			break;

		vec2 uv = sp / vec2(params.screen_size);
		float raw_d = texture(depth_tex, uv).r;

		if (raw_d < DEPTH_BIAS) {
			t += step_sz;
			step_sz = min(step_sz * 1.08, base_step * 4.0);
			steps++;
			continue;
		}

		float sample_z = linearize_depth(raw_d);

		float inv_precision = 1.0 / max(params.depth_precision, 0.1);
		float depth_ratio = clamp(sample_z / params.z_far, 0.0, 1.0);
		float thickness = params.z_thickness * mix(0.2, 1.5, depth_ratio) * inv_precision
						+ step_sz * 0.2 * inv_precision;

		float z_front = sample_z - thickness * 0.2;
		float z_back = sample_z + thickness;

		if (probe_z >= z_front && probe_z <= z_back) {
			vec3 color = texture(color_tex, uv).rgb;
			float dist_norm = abs(probe_z - sample_z) / max(thickness, 0.001);
			float hit_opacity = clamp(1.0 - dist_norm * dist_norm, 0.1, 1.0);
			accum_rad += transparency * hit_opacity * color;
			transparency *= (1.0 - hit_opacity);
			if (transparency < 0.02) break;
		} else if (sample_z < probe_z - thickness) {
			float gap = probe_z - sample_z - thickness;
			float occlude = clamp(1.0 - gap / (thickness * 4.0), 0.0, 0.08);
			transparency *= (1.0 - occlude);
		}

		t += step_sz;
		step_sz = min(step_sz * 1.08, base_step * 4.0);
		steps++;
	}

	ivec2 out_pos = ivec2(
		params.atlas_offset_x + probe_x * params.num_angular_dirs + dir_idx,
		params.atlas_offset_y + probe_y
	);
	imageStore(cascade_out, out_pos, vec4(accum_rad, transparency));
}

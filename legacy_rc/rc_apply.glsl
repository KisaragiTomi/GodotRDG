#[compute]
#version 450

#include "rc_probe.glslinc"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(rgba16f, set = 0, binding = 1) uniform readonly image2D cascade_atlas;
layout(set = 0, binding = 2) uniform sampler2D color_tex;
layout(rgba16f, set = 0, binding = 3) uniform writeonly image2D output_tex;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	int atlas_offset_x;
	int atlas_offset_y;
	int probes_x;
	int probes_y;
	int probe_spacing;
	int num_dirs;
	float gi_intensity;
	float z_near;
	float z_far;
	float z_thickness;
} params;

const float PI = 3.14159265358979;

float linearize_depth(float d) {
	return params.z_near * params.z_far / (params.z_near + d * (params.z_far - params.z_near));
}

vec3 reconstruct_normal(ivec2 pixel) {
	float d_c = linearize_depth(texelFetch(depth_tex, pixel, 0).r);
	ivec2 last_pixel = params.screen_size - 1;
	float d_r = linearize_depth(texelFetch(depth_tex, clamp(pixel + ivec2(1, 0), ivec2(0), last_pixel), 0).r);
	float d_l = linearize_depth(texelFetch(depth_tex, clamp(pixel - ivec2(1, 0), ivec2(0), last_pixel), 0).r);
	float d_u = linearize_depth(texelFetch(depth_tex, clamp(pixel + ivec2(0, 1), ivec2(0), last_pixel), 0).r);
	float d_d = linearize_depth(texelFetch(depth_tex, clamp(pixel - ivec2(0, 1), ivec2(0), last_pixel), 0).r);

	float dx = (abs(d_r - d_c) < abs(d_c - d_l)) ? (d_r - d_c) : (d_c - d_l);
	float dy = (abs(d_u - d_c) < abs(d_c - d_d)) ? (d_u - d_c) : (d_c - d_d);

	float depth_range = max(abs(dx), abs(dy));
	float z_scale = max(1.0, d_c * 0.1);
	vec3 normal = normalize(vec3(-dx, -dy, z_scale));
	return normal;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y)
		return;

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 scene_color = texture(color_tex, uv);

	if (params.gi_intensity < -1.5) {
		ivec2 atlas_size = imageSize(cascade_atlas);
		ivec2 atlas_px = clamp(ivec2(uv * vec2(atlas_size)), ivec2(0), atlas_size - 1);
		vec4 atlas_val = imageLoad(cascade_atlas, atlas_px);
		imageStore(output_tex, pixel, vec4(atlas_val.rgb * 3.0, 1.0));
		return;
	}

	float raw_depth = texelFetch(depth_tex, pixel, 0).r;
	if (raw_depth < 0.0005) {
		imageStore(output_tex, pixel, params.gi_intensity < -0.5 ? vec4(0.0, 0.0, 0.0, 1.0) : scene_color);
		return;
	}

	vec3 normal = reconstruct_normal(pixel);

	vec2 pixel_position = vec2(pixel) + 0.5;
	ivec2 probe_center = ivec2(floor(pixel_position / float(params.probe_spacing)));

	float pixel_depth = linearize_depth(raw_depth);

	vec3 gi_color = vec3(0.0);
	float weight_sum = 0.0;

	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 pp = probe_center + ivec2(dx, dy);
			if (pp.x < 0 || pp.y < 0 || pp.x >= params.probes_x || pp.y >= params.probes_y)
				continue;

			vec2 probe_position = legacy_probe_position(pp, 0, params.probe_spacing, params.screen_size);
			float spatial_w = legacy_probe_weight(pixel_position, probe_position, params.probe_spacing);
			if (spatial_w <= 0.0) continue;

			ivec2 probe_pixel = ivec2(probe_position);
			float probe_raw = texelFetch(depth_tex, probe_pixel, 0).r;
			if (probe_raw < 0.0005) continue;
			float probe_depth = linearize_depth(probe_raw);
			float depth_diff = abs(pixel_depth - probe_depth) / max(pixel_depth * params.z_thickness, 0.01);
			float depth_w = exp(-depth_diff * depth_diff * 10.0);
			// Rejected probes contribute exactly zero. Do not normalize a weight floor.
			if (depth_w < 0.05) continue;
			float w = spatial_w * depth_w;

			vec3 probe_irradiance = vec3(0.0);
			float cosine_sum = 0.0;
			for (int d = 0; d < params.num_dirs; d++) {
				float angle = (float(d) + 0.5) / float(params.num_dirs) * 2.0 * PI;
				vec2 dir_2d = vec2(cos(angle), sin(angle));
				float cosine_weight = max(0.0, dot(normal.xy, dir_2d) * 0.5 + 0.5);
				cosine_weight = cosine_weight * cosine_weight;

				ivec2 atlas_pos = ivec2(
					params.atlas_offset_x + pp.x * params.num_dirs + d,
					params.atlas_offset_y + pp.y
				);
				vec4 ri = imageLoad(cascade_atlas, atlas_pos);
				probe_irradiance += ri.rgb * cosine_weight;
				cosine_sum += cosine_weight;
			}
			if (cosine_sum > 0.001)
				probe_irradiance /= cosine_sum;

			gi_color += w * probe_irradiance;
			weight_sum += w;
		}
	}

	if (weight_sum > 0.00001)
		gi_color /= weight_sum;
	else
		gi_color = vec3(0.0); // Conservative zero-valid fallback: no indirect light.

	float intensity = abs(params.gi_intensity);
	gi_color *= intensity;

	if (params.gi_intensity < -0.5) {
		imageStore(output_tex, pixel, vec4(gi_color, 1.0));
	} else {
		imageStore(output_tex, pixel, vec4(scene_color.rgb + gi_color, scene_color.a));
	}
}

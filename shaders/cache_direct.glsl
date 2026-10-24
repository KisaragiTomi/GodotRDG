#[compute]
#version 450

// L0 is outgoing Lambertian radiance from direct point lighting. Self emission
// is sampled explicitly from cached area form factors in the first bounce;
// adding it here as well would count the same energy twice.
// Uses Godot's NONPHYSICAL point-light energy convention: the renderer's PI
// energy multiplier cancels Lambert's 1/PI. No camera exposure normalization.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Face {
	vec4 origin;
	vec4 axis_u;
	vec4 axis_v;
	vec4 normal;
	vec4 albedo;
	vec4 emission;
	vec4 rect;
};
struct Surfel { vec4 position; vec4 atlas; };
struct Box { vec4 lo; vec4 hi; };
struct Light { vec4 position; vec4 color; vec4 unused; };

layout(set = 0, binding = 0, std430) readonly buffer Faces { Face faces[]; };
layout(set = 0, binding = 1, std430) readonly buffer Surfels { Surfel surfels[]; };
layout(set = 0, binding = 2, std430) readonly buffer Boxes { Box boxes[]; };
layout(set = 0, binding = 3, std430) readonly buffer Lights { Light lights[]; };
layout(rgba16f, set = 0, binding = 4) uniform writeonly image2D outgoing;
layout(push_constant, std430) uniform Parameters {
	uint surfel_count;
	uint box_count;
	uint light_count;
	uint ray_count;
	uint atlas_width;
	uint atlas_height;
	uint bounce_index;
	uint unused;
} params;

const float RAY_BIAS = 0.005;

bool blocks_segment(vec3 origin, vec3 direction, float segment_length, Box box) {
	float near_t = 0.0;
	float far_t = segment_length;
	for (int axis = 0; axis < 3; ++axis) {
		if (abs(direction[axis]) < 1e-8) {
			if (origin[axis] < box.lo[axis] || origin[axis] > box.hi[axis]) return false;
			continue;
		}
		float t0 = (box.lo[axis] - origin[axis]) / direction[axis];
		float t1 = (box.hi[axis] - origin[axis]) / direction[axis];
		near_t = max(near_t, min(t0, t1));
		far_t = min(far_t, max(t0, t1));
		if (far_t < near_t) return false;
	}
	return far_t > 0.0 && near_t < segment_length;
}

float omni_attenuation(float distance, float range, float decay) {
	// Matches scene_forward_lights_inc.glsl::get_omni_attenuation.
	float normalized_distance = distance / max(range, 0.001);
	normalized_distance *= normalized_distance;
	normalized_distance *= normalized_distance;
	float window = max(1.0 - normalized_distance, 0.0);
	return window * window * pow(max(distance, 0.0001), -decay);
}

void main() {
	uint surfel_id = gl_GlobalInvocationID.x;
	if (surfel_id >= params.surfel_count) return;
	Surfel surfel = surfels[surfel_id];
	Face face = faces[uint(surfel.position.w + 0.5)];
	vec3 normal = normalize(face.normal.xyz);
	vec3 direct = vec3(0.0);
	vec3 origin = surfel.position.xyz + normal * RAY_BIAS;
	for (uint light_id = 0u; light_id < params.light_count; ++light_id) {
		Light light = lights[light_id];
		vec3 to_light = light.position.xyz - surfel.position.xyz;
		float distance = length(to_light);
		if (distance <= 0.0001 || distance >= light.position.w) continue;
		vec3 direction = to_light / distance;
		float cosine = max(dot(normal, direction), 0.0);
		if (cosine <= 0.0) continue;
		// Bias visibility only; preserve the true surface point for falloff/cosine.
		vec3 shadow_segment = light.position.xyz - origin;
		float shadow_length = length(shadow_segment);
		vec3 shadow_direction = shadow_segment / max(shadow_length, 0.0001);
		bool blocked = false;
		for (uint box_id = 0u; box_id < params.box_count; ++box_id) {
			if (blocks_segment(origin, shadow_direction, max(shadow_length - 0.0001, 0.0), boxes[box_id])) {
				blocked = true;
				break;
			}
		}
		if (!blocked) direct += light.color.rgb * cosine * omni_attenuation(distance, light.position.w, light.color.w);
	}
	vec3 legacy_emission = params.unused == 0u ? face.emission.rgb : vec3(0.0);
	imageStore(outgoing, ivec2(surfel.atlas.xy), vec4(legacy_emission + face.albedo.rgb * direct, 1.0));
}

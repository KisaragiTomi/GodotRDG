#[compute]
#version 450
// Moving, uniformly emissive sphere. Only source visibility is recomputed;
// static receiver-to-receiver transport remains reusable.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
struct Face { vec4 origin; vec4 axis_u; vec4 axis_v; vec4 normal; vec4 albedo; vec4 emission; vec4 rect; };
struct Surfel { vec4 position; vec4 atlas; };
struct Box { vec4 lo; vec4 hi; };
layout(set = 0, binding = 0, std430) readonly buffer Faces { Face faces[]; };
layout(set = 0, binding = 1, std430) readonly buffer Surfels { Surfel surfels[]; };
layout(set = 0, binding = 2, std430) readonly buffer Boxes { Box boxes[]; };
layout(set = 0, binding = 3, std430) readonly buffer Sphere { vec4 center_radius; vec4 radiance; } source;
layout(rgba16f, set = 0, binding = 4) uniform writeonly image2D sphere_incoming;
layout(push_constant, std430) uniform Parameters {
	uint surfel_count; uint box_count; uint light_count; uint ray_count;
	uint atlas_width; uint atlas_height; uint bounce_index; uint unused;
} params;
const float PI = 3.141592653589793;
const uint SAMPLES = 64u;

bool visible(vec3 origin, vec3 target) {
	vec3 delta = target - origin;
	float length = length(delta);
	vec3 direction = delta / max(length, 1e-7);
	for (uint box_id = 0u; box_id < params.box_count; ++box_id) {
		float near_t = 0.0;
		float far_t = length;
		bool miss = false;
		for (int axis = 0; axis < 3; ++axis) {
			if (abs(direction[axis]) < 1e-8) {
				if (origin[axis] < boxes[box_id].lo[axis] || origin[axis] > boxes[box_id].hi[axis]) miss = true;
			} else {
				float a = (boxes[box_id].lo[axis] - origin[axis]) / direction[axis];
				float b = (boxes[box_id].hi[axis] - origin[axis]) / direction[axis];
				near_t = max(near_t, min(a, b));
				far_t = min(far_t, max(a, b));
			}
		}
		if (!miss && far_t >= near_t && far_t > 0.0 && near_t < length) return false;
	}
	return true;
}

vec3 irradiance_over_pi(vec3 receiver, vec3 normal) {
	float radius = source.center_radius.w;
	if (radius <= 0.0 || !any(greaterThan(source.radiance.rgb, vec3(0.0)))) return vec3(0.0);
	vec3 delta = source.center_radius.xyz - receiver;
	float distance_squared = dot(delta, delta);
	float height = dot(normal, delta);
	if (height <= -radius || distance_squared <= radius * radius) return vec3(0.0);
	vec3 axis = delta * inversesqrt(distance_squared);
	vec3 tangent = normalize(cross(abs(axis.y) < 0.95 ? vec3(0, 1, 0) : vec3(1, 0, 0), axis));
	vec3 bitangent = cross(axis, tangent);
	float sin_squared = radius * radius / distance_squared;
	float cap_cosine = sqrt(max(1.0 - sin_squared, 0.0));
	// Stable even when the solid angle is tiny at long distances.
	float cap_height = sin_squared / (1.0 + cap_cosine);
	float full_weight = 0.0;
	float visible_weight = 0.0;
	for (uint sample_id = 0u; sample_id < SAMPLES; ++sample_id) {
		float cosine = 1.0 - cap_height * (float(sample_id) + 0.5) / float(SAMPLES);
		float sine = sqrt(max(1.0 - cosine * cosine, 0.0));
		float phi = 2.0 * PI * float(bitfieldReverse(sample_id)) * 2.3283064365386963e-10;
		vec3 direction = axis * cosine + (tangent * cos(phi) + bitangent * sin(phi)) * sine;
		float weight = max(dot(normal, direction), 0.0);
		if (weight <= 0.0) continue;
		full_weight += weight;
		float projected = dot(delta, direction);
		float hit_distance = projected - sqrt(max(projected * projected - distance_squared + radius * radius, 0.0));
		if (visible(receiver + normal * 0.005, receiver + direction * hit_distance)) visible_weight += weight;
	}
	// The fully above-horizon sphere has exact projected solid angle. Only
	// horizon clipping and partial visibility use the finite cone samples.
	float factor = height >= radius ? sin_squared * dot(normal, axis) : 2.0 * cap_height * full_weight / float(SAMPLES);
	return full_weight > 0.0 ? source.radiance.rgb * factor * (visible_weight / full_weight) : vec3(0.0);
}

void main() {
	uint id = gl_GlobalInvocationID.x;
	if (id >= params.surfel_count) return;
	Surfel surfel = surfels[id];
	vec3 normal = normalize(faces[uint(surfel.position.w + 0.5)].normal.xyz);
	imageStore(sphere_incoming, ivec2(surfel.atlas.xy), vec4(irradiance_over_pi(surfel.position.xyz, normal), 1.0));
}

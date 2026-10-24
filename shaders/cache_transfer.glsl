#[compute]
#version 450

// Static world-space transfer. Dispatch ceil(surfel_count / 64) groups.
// Each entry stores a hit in the surface atlas, not a screen-space sample.
// Misses remain black sky and are INCLUDED in the hemisphere normalization.
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

layout(set = 0, binding = 0, std430) readonly buffer Faces { Face faces[]; };
layout(set = 0, binding = 1, std430) readonly buffer Surfels { Surfel surfels[]; };
layout(set = 0, binding = 2, std430) readonly buffer Boxes { Box boxes[]; };
layout(set = 0, binding = 3, std430) writeonly buffer Transfer { vec2 hits[]; };
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

const float PI = 3.141592653589793;
const float RAY_BIAS = 0.005;

uint hash_uint(uint value) {
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

// Slab intersection preserves the entered face. Face order matches the CPU:
// +X, -X, +Y, -Y, +Z, -Z. Ray origins are outside the surface by RAY_BIAS.
bool intersect_box(vec3 origin, vec3 direction, Box box, out float distance, out uint face_id) {
	float near_t = -1e30;
	float far_t = 1e30;
	uint entering_face = 0u;
	for (int axis = 0; axis < 3; ++axis) {
		if (abs(direction[axis]) < 1e-8) {
			if (origin[axis] < box.lo[axis] || origin[axis] > box.hi[axis]) return false;
			continue;
		}
		float t0 = (box.lo[axis] - origin[axis]) / direction[axis];
		float t1 = (box.hi[axis] - origin[axis]) / direction[axis];
		float axis_near = min(t0, t1);
		if (axis_near > near_t) {
			near_t = axis_near;
			entering_face = uint(axis) * 2u + (direction[axis] > 0.0 ? 1u : 0u);
		}
		far_t = min(far_t, max(t0, t1));
		if (far_t < near_t) return false;
	}
	if (far_t <= 0.0 || near_t < 0.0) return false;
	distance = near_t;
	face_id = uint(box.lo.w + 0.5) + entering_face;
	return true;
}

bool point_inside_box(vec3 point, Box box) {
	return all(greaterThan(point, box.lo.xyz)) && all(lessThan(point, box.hi.xyz));
}

bool visible_receiver_center(vec3 hit_point, vec3 center) {
	vec3 segment = center - hit_point;
	float length = length(segment);
	vec3 direction = segment / max(length, 0.000001);
	for (uint box_id = 0u; box_id < params.box_count; ++box_id) {
		Box box = boxes[box_id];
		// Match CPU buried-surfel omission; an unwritten atlas texel is never a
		// valid receiver. Also reject a numerically buried hit after normal bias.
		if (point_inside_box(center, box) || point_inside_box(hit_point, box)) return false;
		if (length <= 0.000001) continue;
		float distance;
		uint face_id;
		if (intersect_box(hit_point, direction, box, distance, face_id) && distance < length) return false;
	}
	return true;
}

vec2 select_visible_center(Face target, vec3 hit_point, vec2 uv, vec2 atlas_size) {
	// Merely using nearest filtering is insufficient: a thin, off-grid wall can
	// separate the hit from its nearest sample center. Choose a visible center
	// among the local four and store that exact texel center (no interpolation).
	vec2 lattice = clamp(uv * target.rect.zw - vec2(0.5), vec2(0.0), target.rect.zw - vec2(1.0));
	vec2 base = floor(lattice);
	vec3 biased_hit = hit_point + target.normal.xyz * RAY_BIAS;
	float closest_squared = 1e30;
	vec2 best_uv = vec2(-1.0);
	for (int tap = 0; tap < 4; ++tap) {
		vec2 corner = vec2(float(tap % 2), float(tap / 2));
		vec2 sample_pixel = clamp(base + corner, vec2(0.0), target.rect.zw - vec2(1.0));
		vec2 sample_uv = (sample_pixel + vec2(0.5)) / target.rect.zw;
		vec3 center = target.origin.xyz + target.axis_u.xyz * sample_uv.x + target.axis_v.xyz * sample_uv.y;
		float squared_distance = dot(center - hit_point, center - hit_point);
		if (squared_distance >= closest_squared) continue;
		if (!visible_receiver_center(biased_hit, center + target.normal.xyz * RAY_BIAS)) continue;
		closest_squared = squared_distance;
		best_uv = (target.rect.xy + sample_pixel + vec2(0.5)) / atlas_size;
	}
	// If every local center is buried/across a blocker, the hit contributes zero.
	// This loses energy at undersampled corners but cannot transmit through walls.
	return best_uv;
}

void main() {
	uint surfel_id = gl_GlobalInvocationID.x;
	if (surfel_id >= params.surfel_count) return;
	Surfel surfel = surfels[surfel_id];
	Face face = faces[uint(surfel.position.w + 0.5)];
	vec3 normal = normalize(face.normal.xyz);
	vec3 tangent = normalize(face.axis_u.xyz);
	vec3 bitangent = normalize(cross(normal, tangent));
	vec3 ray_origin = surfel.position.xyz + normal * RAY_BIAS;
	float rotation = float(hash_uint(surfel_id) & 0x00ffffffu) / 16777216.0;
	vec2 atlas_size = vec2(params.atlas_width, params.atlas_height);
	for (uint ray = 0u; ray < params.ray_count; ++ray) {
		// Deterministic cosine-weighted Hammersley hemisphere. Rotation decorrelates
		// neighboring surfels without introducing camera or temporal dependence.
		float radial_squared = (float(ray) + 0.5) / float(params.ray_count);
		float azimuth = 2.0 * PI * fract(float(bitfieldReverse(ray)) * 2.3283064365386963e-10 + rotation);
		float radial = sqrt(radial_squared);
		vec3 direction = tangent * (radial * cos(azimuth))
				+ bitangent * (radial * sin(azimuth)) + normal * sqrt(1.0 - radial_squared);
		float closest = 1e30;
		uint hit_face = 0u;
		bool found = false;
		for (uint box_id = 0u; box_id < params.box_count; ++box_id) {
			float distance;
			uint face_id;
			if (intersect_box(ray_origin, direction, boxes[box_id], distance, face_id) && distance < closest) {
				closest = distance;
				hit_face = face_id;
				found = true;
			}
		}
		vec2 atlas_uv = vec2(-1.0);
		if (found) {
			Face target = faces[hit_face];
			if (target.rect.z > 0.0 && target.rect.w > 0.0) {
				vec3 hit_point = ray_origin + direction * closest;
				vec3 relative = hit_point - target.origin.xyz;
				vec2 uv = vec2(dot(relative, target.axis_u.xyz) / dot(target.axis_u.xyz, target.axis_u.xyz),
						dot(relative, target.axis_v.xyz) / dot(target.axis_v.xyz, target.axis_v.xyz));
				atlas_uv = select_visible_center(target, hit_point, uv, atlas_size);
			}
		}
		hits[surfel_id * params.ray_count + ray] = atlas_uv;
	}
}

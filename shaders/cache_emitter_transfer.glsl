#[compute]
#version 450

// Static form factors for every receiver/box face, independent of emission.
// Dispatch ceil(surfel_count / 64) groups. Dynamic emitter edits only multiply
// these cached factors; they never trace new rays or depend on the camera.
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
layout(set = 0, binding = 3, std430) writeonly buffer EmitterTransfer { float factors[]; };
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
// Eight-point Gauss-Legendre nodes and weights mapped from [-1, 1] to [0, 1].
const float QUAD_X[8] = float[8](
	0.0198550718, 0.1016667613, 0.2372337950, 0.4082826788,
	0.5917173212, 0.7627662050, 0.8983332387, 0.9801449282);
const float QUAD_W[8] = float[8](
	0.0506142681, 0.1111905172, 0.1568533229, 0.1813418917,
	0.1813418917, 0.1568533229, 0.1111905172, 0.0506142681);

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

bool visible(vec3 origin, vec3 target) {
	vec3 segment = target - origin;
	float segment_length = length(segment);
	if (segment_length <= 0.000001) return true;
	vec3 direction = segment / segment_length;
	for (uint box_id = 0u; box_id < params.box_count; ++box_id) {
		if (blocks_segment(origin, direction, segment_length, boxes[box_id])) return false;
	}
	return true;
}

float unoccluded_form_factor(vec3 receiver, vec3 normal, Face emitter) {
	// Clip the rectangle against the receiver's positive hemisphere before
	// evaluating the exact projected solid angle. Axis U cross axis V follows
	// the emitter normal, so its contour is clockwise from a front receiver.
	vec3 corners[4];
	corners[0] = emitter.origin.xyz - receiver;
	corners[1] = corners[0] + emitter.axis_u.xyz;
	corners[2] = corners[1] + emitter.axis_v.xyz;
	corners[3] = corners[0] + emitter.axis_v.xyz;
	vec3 polygon[6];
	int count = 0;
	vec3 previous = corners[3];
	float previous_height = dot(normal, previous);
	for (int corner = 0; corner < 4; ++corner) {
		vec3 current = corners[corner];
		float height = dot(normal, current);
		if ((height > 0.0) != (previous_height > 0.0)) {
			polygon[count++] = mix(previous, current, previous_height / (previous_height - height));
		}
		if (height > 0.0) polygon[count++] = current;
		previous = current;
		previous_height = height;
	}
	if (count < 3) return 0.0;
	vec3 vector_area = vec3(0.0);
	previous = normalize(polygon[count - 1]);
	for (int vertex = 0; vertex < count; ++vertex) {
		vec3 current = normalize(polygon[vertex]);
		vec3 edge = cross(previous, current);
		float sine = length(edge);
		if (sine > 1e-8) vector_area += edge * (atan(sine, dot(previous, current)) / sine);
		previous = current;
	}
	return max(-dot(normal, vector_area) / (2.0 * PI), 0.0);
}

float emitter_form_factor(vec3 receiver, vec3 normal, Face emitter) {
	// Every point on a planar face shares this emission-side test. It also
	// rejects a receiver's own face without a singular near-field integral.
	vec3 emitter_normal = normalize(emitter.normal.xyz);
	if (dot(emitter_normal, receiver - emitter.origin.xyz) <= 0.000001) return 0.0;
	float exact_factor = unoccluded_form_factor(receiver, normal, emitter);
	if (exact_factor <= 0.0) return 0.0;
	// Box charts and receiver normals are axis aligned. Their hemisphere cut
	// therefore clips at most one chart axis. Sample the surviving interval so
	// even a very narrow visible strip has visibility samples above the horizon.
	vec2 lower = vec2(0.0);
	vec2 upper = vec2(1.0);
	float height = dot(normal, emitter.origin.xyz - receiver);
	float height_u = dot(normal, emitter.axis_u.xyz);
	float height_v = dot(normal, emitter.axis_v.xyz);
	if (abs(height_u) > 1e-8) {
		float cut = clamp(-height / height_u, 0.0, 1.0);
		if (height_u > 0.0) lower.x = cut;
		else upper.x = cut;
	} else if (abs(height_v) > 1e-8) {
		float cut = clamp(-height / height_v, 0.0, 1.0);
		if (height_v > 0.0) lower.y = cut;
		else upper.y = cut;
	}
	vec3 origin = receiver + normal * RAY_BIAS;
	float full_weight = 0.0;
	float visible_weight = 0.0;
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			vec2 uv = mix(lower, upper, vec2(QUAD_X[x], QUAD_X[y]));
			vec3 point = emitter.origin.xyz + emitter.axis_u.xyz * uv.x + emitter.axis_v.xyz * uv.y;
			vec3 delta = point - receiver;
			float distance_squared = dot(delta, delta);
			float receiver_cosine_distance = max(dot(normal, delta), 0.0);
			float emitter_cosine_distance = max(-dot(emitter_normal, delta), 0.0);
			float weight = QUAD_W[x] * QUAD_W[y] * receiver_cosine_distance * emitter_cosine_distance
					/ max(distance_squared * distance_squared, 1e-20);
			full_weight += weight;
			if (weight > 0.0 && visible(origin, point + emitter_normal * RAY_BIAS)) visible_weight += weight;
		}
	}
	// Quadrature estimates the *weighted visibility*, whose range is [0,1].
	// Exact projected solid angle supplies the unoccluded energy: even a very
	// close, thin emitter remains correct without an arbitrary energy clamp.
	return full_weight > 0.0 ? exact_factor * (visible_weight / full_weight) : 0.0;
}

void main() {
	uint surfel_id = gl_GlobalInvocationID.x;
	if (surfel_id >= params.surfel_count) return;
	Surfel surfel = surfels[surfel_id];
	Face receiver = faces[uint(surfel.position.w + 0.5)];
	vec3 normal = normalize(receiver.normal.xyz);
	uint face_count = params.box_count * 6u;
	for (uint face_id = 0u; face_id < face_count; ++face_id) {
		factors[surfel_id * face_count + face_id] = emitter_form_factor(surfel.position.xyz, normal, faces[face_id]);
	}
}

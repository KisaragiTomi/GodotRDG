#[compute]
#version 450

// Finite-bounce transport: each pass transports ONLY the preceding bounce.
// totalIncoming stores sum(E_indirect / PI) and is reset by bounce_index == 0.
// Bind distinct previous/next outgoing images and issue a compute barrier
// between dependent passes. Never re-add L0 to subsequent outgoing images.
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

layout(set = 0, binding = 0, std430) readonly buffer Faces { Face faces[]; };
layout(set = 0, binding = 1, std430) readonly buffer Surfels { Surfel surfels[]; };
layout(set = 0, binding = 2, std430) readonly buffer Transfer { vec2 hits[]; };
layout(set = 0, binding = 3) uniform sampler2D previous_outgoing;
layout(rgba16f, set = 0, binding = 4) uniform writeonly image2D next_outgoing;
layout(rgba16f, set = 0, binding = 5) uniform image2D total_incoming;
layout(set = 0, binding = 6, std430) readonly buffer EmitterTransfer { float emitter_factors[]; };
layout(rgba16f, set = 0, binding = 7) uniform readonly image2D sphere_incoming;
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

void main() {
	uint surfel_id = gl_GlobalInvocationID.x;
	if (surfel_id >= params.surfel_count) return;
	Surfel surfel = surfels[surfel_id];
	Face face = faces[uint(surfel.position.w + 0.5)];
	vec3 incoming = vec3(0.0);
	for (uint ray = 0u; ray < params.ray_count; ++ray) {
		vec2 uv = hits[surfel_id * params.ray_count + ray];
		if (uv.x >= 0.0) incoming += textureLod(previous_outgoing, uv, 0.0).rgb;
	}
	// Cosine PDF = N.L/PI, so the sample mean is precisely E/PI. Do not divide
	// by the number of hits: doing so would brighten open surfaces artificially.
	incoming /= float(max(params.ray_count, 1u));
	// Integrate emission explicitly once. Hemispherical transport then handles
	// reflected radiance, avoiding rare, extremely bright random emitter hits.
	if (params.bounce_index == 0u && params.unused != 0u) {
		uint face_count = params.box_count * 6u;
		for (uint source = 0u; source < face_count; ++source) {
			vec3 emission = faces[source].emission.rgb;
			if (any(greaterThan(emission, vec3(0.0)))) incoming += emission * emitter_factors[surfel_id * face_count + source];
		}
	}
	ivec2 pixel = ivec2(surfel.atlas.xy);
	if (params.bounce_index == 0u) incoming += imageLoad(sphere_incoming, pixel).rgb;
	vec3 accumulated = params.bounce_index == 0u ? vec3(0.0) : imageLoad(total_incoming, pixel).rgb;
	imageStore(total_incoming, pixel, vec4(accumulated + incoming, 1.0));
	imageStore(next_outgoing, pixel, vec4(face.albedo.rgb * incoming, 1.0));
}

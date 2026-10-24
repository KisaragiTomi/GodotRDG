shader_type spatial;
render_mode diffuse_lambert, specular_disabled;

uniform vec4 surface_color : source_color = vec4(0.6, 0.6, 0.6, 1.0);
uniform vec4 surface_emission : source_color = vec4(0.0, 0.0, 0.0, 1.0);
uniform float emission_energy = 0.0;
uniform sampler2D indirect_atlas : filter_linear, repeat_disable;
uniform vec4 atlas_rect;
uniform vec2 atlas_size = vec2(256.0);
// Face-local projected box footprints: minimum UV, maximum UV. They prevent
// adjacent atlas texels on opposite sides of a sub-texel wall from mixing.
uniform vec4 blockers[32];
uniform int blocker_count = 0;
uniform bool blocker_overflow = false;
// 0: no GI, 1: cached transport, 2: indirect only, 3: native GI.
uniform int gi_mode = 1;
uniform float indirect_energy = 1.0;

bool overlaps_footprint(vec4 rectangle, vec2 minimum, vec2 maximum) {
	return all(lessThanEqual(rectangle.xy, maximum)) && all(greaterThanEqual(rectangle.zw, minimum));
}

bool blocked_segment(vec2 origin, vec2 target, vec4 rectangle) {
	vec2 direction = target - origin;
	float near_t = 0.0;
	float far_t = 1.0;
	for (int axis = 0; axis < 2; axis++) {
		if (abs(direction[axis]) < 0.0000001) {
			if (origin[axis] < rectangle[axis] || origin[axis] > rectangle[axis + 2]) return false;
		} else {
			float t0 = (rectangle[axis] - origin[axis]) / direction[axis];
			float t1 = (rectangle[axis + 2] - origin[axis]) / direction[axis];
			near_t = max(near_t, min(t0, t1));
			far_t = min(far_t, max(t0, t1));
			if (far_t < near_t) return false;
		}
	}
	return far_t >= near_t;
}

vec3 sample_indirect(vec2 face_uv) {
	if (blocker_overflow) return vec3(0.0);
	vec2 pixel = clamp(atlas_rect.xy + face_uv * atlas_rect.zw,
		atlas_rect.xy + vec2(0.5), atlas_rect.xy + atlas_rect.zw - vec2(0.5));
	if (blocker_count == 0) return textureLod(indirect_atlas, pixel / atlas_size, 0.0).rgb;
	vec2 base = floor(pixel - vec2(0.5));
	vec2 fraction = fract(pixel - vec2(0.5));
	vec2 chart_minimum = vec2(0.5) / atlas_rect.zw;
	vec2 chart_maximum = vec2(1.0) - chart_minimum;
	vec2 footprint_minimum = min(face_uv, clamp((base + vec2(0.5) - atlas_rect.xy) / atlas_rect.zw, chart_minimum, chart_maximum));
	vec2 footprint_maximum = max(face_uv, clamp((base + vec2(1.5) - atlas_rect.xy) / atlas_rect.zw, chart_minimum, chart_maximum));
	bool needs_visibility = false;
	for (int blocker = 0; blocker < 32; blocker++) {
		if (blocker >= blocker_count) break;
		if (overlaps_footprint(blockers[blocker], footprint_minimum, footprint_maximum)) {
			needs_visibility = true;
			break;
		}
	}
	if (!needs_visibility) return textureLod(indirect_atlas, pixel / atlas_size, 0.0).rgb;
	vec3 accumulated = vec3(0.0);
	float weight_sum = 0.0;
	for (int tap = 0; tap < 4; tap++) {
		vec2 corner = vec2(float(tap % 2), float(tap / 2));
		vec2 axis_weight = mix(vec2(1.0) - fraction, fraction, corner);
		float weight = axis_weight.x * axis_weight.y;
		if (weight <= 0.0) continue;
		vec2 coordinate = clamp(base + corner, atlas_rect.xy, atlas_rect.xy + atlas_rect.zw - vec2(1.0));
		vec4 value = texelFetch(indirect_atlas, ivec2(coordinate), 0);
		if (value.a < 0.5) continue;
		vec2 tap_uv = (coordinate + vec2(0.5) - atlas_rect.xy) / atlas_rect.zw;
		bool visible = true;
		for (int blocker = 0; blocker < 32; blocker++) {
			if (blocker >= blocker_count) break;
			if (!overlaps_footprint(blockers[blocker], footprint_minimum, footprint_maximum)) continue;
			if (blocked_segment(face_uv, tap_uv, blockers[blocker])) {
				visible = false;
				break;
			}
		}
		if (visible) {
			accumulated += value.rgb * weight;
			weight_sum += weight;
		}
	}
	// Every invalid tap is really rejected. There is no weight floor that could
	// cancel during normalization and resurrect an across-wall sample.
	return weight_sum > 0.0 ? accumulated / weight_sum : vec3(0.0);
}

void fragment() {
	vec3 incoming = vec3(0.0);
	if (gi_mode == 1 || gi_mode == 2) incoming = sample_indirect(clamp(UV, vec2(0.0), vec2(1.0))) * indirect_energy;
	ALBEDO = surface_color.rgb;
	ROUGHNESS = 1.0;
	METALLIC = 0.0;
	EMISSION = surface_emission.rgb * max(emission_energy, 0.0);
	IRRADIANCE = vec4(gi_mode == 1 ? incoming : vec3(0.0), gi_mode == 3 ? 0.0 : 1.0);
	if (gi_mode == 2) {
		ALBEDO = vec3(0.0);
		EMISSION = surface_color.rgb * incoming;
	}
}

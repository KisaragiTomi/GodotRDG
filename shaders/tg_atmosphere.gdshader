shader_type fog;

// Reimplemented from the audited density and source terms, not extracted code.
// Godot integrates extinction and shadowed HG scattering in its froxel volume.
uniform vec3 center = vec3(0.0);
uniform float distance_scale = 0.2;
uniform float extinction = 0.075;
uniform vec3 tint : source_color = vec3(0.94, 0.97, 1.0);
uniform vec3 sky_color : source_color = vec3(0.24, 0.32, 0.45);
uniform float sky_energy = 1.0;
uniform float solar_transmittance = 1.0;
uniform int theme = 0;

void fog() {
	vec3 p = (WORLD_POSITION - center) / max(distance_scale, 0.001);
	float radius = length(p.xz);
	vec3 q = p / vec3(300.0, 30.0, 300.0);
	float profile = 0.6 * exp2(-dot(q, q));
	if (theme == 2) {
		profile = min(profile * mix(1.0, 3.0, smoothstep(60.0, 200.0, radius)), 0.8);
	} else if (theme == 3) {
		profile *= mix(1.0, 8.0, smoothstep(30.0, 200.0, radius));
	} else {
		profile *= mix(1.0, theme == 4 ? 4.0 : 2.0, smoothstep(100.0, 300.0, radius));
	}
	DENSITY = extinction * profile;
	// sigma_s / sigma_t = 0.4 / 1.2 in the reference shader.
	ALBEDO = tint / 3.0;
	float sky_lobe = mix(4.0, 8.0, 1.0 - exp(-0.01 * radius));
	EMISSION = sky_color * tint * (sky_energy * sky_lobe * solar_transmittance / 3.0);
}

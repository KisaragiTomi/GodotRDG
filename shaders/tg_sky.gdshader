shader_type sky;
// Linear source radiance, shared with GI misses and the atmosphere's sky source.
// No ground environment: downward misses cannot inject light through the floor.
uniform vec3 radiance = vec3(0.0);

void sky() {
	COLOR = EYEDIR.y > 0.0 ? radiance : vec3(0.0);
}

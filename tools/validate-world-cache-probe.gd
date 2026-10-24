extends SceneTree
## Query the same GPU cache helper as production, with controlled surface records.
func _initialize() -> void:
	_run.call_deferred()

func _run() -> void:
	var output := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--world-cache-probe="): output = arg.trim_prefix("--world-cache-probe=")
	if output.is_empty():
		quit(2)
		return
	var rd := RenderingServer.create_local_rendering_device()
	var source: RDShaderFile = load("res://shaders/tests/tg_world_cache_probe.glsl")
	var spirv := source.get_spirv()
	var error := spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE)
	if not error.is_empty():
		push_error(error)
		quit(1)
		return
	var shader := rd.shader_create_from_spirv(spirv)
	var pipeline := rd.compute_pipeline_create(shader)
	var query_bytes := FileAccess.get_file_as_bytes(output.path_join("queries.bin"))
	var cache_bytes := FileAccess.get_file_as_bytes(output.path_join("entries.bin"))
	var count := query_bytes.size() / 48
	var queries := rd.storage_buffer_create(query_bytes.size(), query_bytes)
	var cache := rd.storage_buffer_create(cache_bytes.size(), cache_bytes)
	var results := rd.storage_buffer_create(count * 16)
	var uniforms: Array[RDUniform] = []
	for pair in [[0, queries], [1, results], [35, cache]]:
		var uniform := RDUniform.new()
		uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		uniform.binding = pair[0]
		uniform.add_id(pair[1])
		uniforms.append(uniform)
	var set := rd.uniform_set_create(uniforms, shader, 0)
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, set, 0)
	var push := PackedInt32Array([count, 0, 0, 0]).to_byte_array()
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, ceili(count / 64.0), 1, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()
	FileAccess.open(output.path_join("actual.bin"), FileAccess.WRITE).store_buffer(rd.buffer_get_data(results))
	for rid in [set, pipeline, shader, queries, cache, results]: rd.free_rid(rid)
	rd.free()
	print("WORLD_CACHE_PROBE completed ", count, " queries")
	quit()

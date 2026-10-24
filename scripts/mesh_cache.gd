class_name MeshSurfaceCache
extends RefCounted

signal ready_for_use
signal update_complete
signal failed(message: String)
const ROOT := "res://generated/sponza/"
var texture := Texture2DRD.new()
var stats: Dictionary = {"ready": false, "transport_builds": 0, "lighting_updates": 0}
var metadata: Dictionary
var rays := 256
var pending := false
var _closed := false
var _rd: RenderingDevice
var _resources: Array[RID] = []
var _sets: Array[RID] = []
var _pipeline: RID
var _light_buffer: RID
var _atlas: RID
var _shader: RDShaderFile
var _last_timing_frame := -1

func initialize(lights: PackedFloat32Array, sample_count: int = 256) -> void:
	metadata = JSON.parse_string(FileAccess.get_file_as_string(ROOT + "cache.json"))
	rays = sample_count
	stats.surfel_count = int(metadata.surfel_count)
	stats.rays = rays
	stats.atlas = metadata.atlas
	stats.triangles = metadata.triangles
	stats.transfer_bytes = int(metadata.surfel_count) * rays * 4
	_shader = load("res://shaders/mesh_cache.glsl")
	pending = true
	RenderingServer.call_on_render_thread(_initialize_gpu.bind(lights.duplicate()))

func update_lights(lights: PackedFloat32Array) -> bool:
	if _closed or pending or not stats.ready: return false
	pending = true
	RenderingServer.call_on_render_thread(_update_gpu.bind(lights.duplicate(), false))
	return true

func _keep(rid: RID) -> RID:
	_resources.append(rid)
	return rid

func _buffer_file(name: String) -> RID:
	var bytes := FileAccess.get_file_as_bytes(ROOT + name)
	return _keep(_rd.storage_buffer_create(bytes.size(), bytes))

func _uniform(binding: int, kind: int, ids: Array[RID]) -> RDUniform:
	var result := RDUniform.new()
	result.binding = binding
	result.uniform_type = kind
	for rid in ids: result.add_id(rid)
	return result

func _image() -> RID:
	var format := RDTextureFormat.new()
	format.width = int(metadata.atlas[0])
	format.height = int(metadata.atlas[1])
	format.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	var bytes := PackedByteArray()
	bytes.resize(format.width * format.height * 8)
	return _keep(_rd.texture_create(format, RDTextureView.new(), [bytes]))

func _initialize_gpu(lights: PackedFloat32Array) -> void:
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		_error.call_deferred("Mesh GI needs Forward+ and a GPU")
		return
	var spirv := _shader.get_spirv()
	var error := spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE)
	if not error.is_empty():
		_error.call_deferred(error)
		return
	var shader := _keep(_rd.shader_create_from_spirv(spirv))
	_pipeline = _keep(_rd.compute_pipeline_create(shader))
	var buffers: Array[RID] = [_buffer_file("bvh.bin"), _buffer_file("triangles.bin"), _buffer_file("surfels.bin"), _keep(_rd.storage_buffer_create(int(stats.transfer_bytes))), _buffer_file("padding.bin")]
	var materials := PackedFloat32Array()
	var images: Array[PackedByteArray] = []
	for id in metadata.materials.size():
		var material: Dictionary = metadata.materials[id]
		materials.append_array(PackedFloat32Array(material.color))
		materials.append_array(PackedFloat32Array([material.alpha_cutoff, 0, 0, 0]))
		var image := Image.new()
		image.load_png_from_buffer(FileAccess.get_file_as_bytes(ROOT + "albedo_%d.png" % id))
		image.convert(Image.FORMAT_RGBA8)
		images.append(image.get_data())
	var material_bytes := materials.to_byte_array()
	buffers.append(_keep(_rd.storage_buffer_create(material_bytes.size(), material_bytes)))
	var format := RDTextureFormat.new()
	format.texture_type = RenderingDevice.TEXTURE_TYPE_2D_ARRAY
	format.width = 512
	format.height = 512
	format.array_layers = images.size()
	format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	var albedo := _keep(_rd.texture_create(format, RDTextureView.new(), images))
	var sampler_state := RDSamplerState.new()
	sampler_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	sampler_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	var sampler := _keep(_rd.sampler_create(sampler_state))
	_light_buffer = _keep(_rd.storage_buffer_create(64))
	var a := _image()
	var b := _image()
	var total := _image()
	_atlas = a
	texture.texture_rd_rid = a
	for pair in [[a, b], [b, a]]:
		var uniforms: Array[RDUniform] = []
		for index in buffers.size(): uniforms.append(_uniform(index, RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, [buffers[index]]))
		uniforms.append(_uniform(6, RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, [sampler, albedo]))
		uniforms.append(_uniform(7, RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, [_light_buffer]))
		uniforms.append(_uniform(8, RenderingDevice.UNIFORM_TYPE_IMAGE, [pair[0]]))
		uniforms.append(_uniform(9, RenderingDevice.UNIFORM_TYPE_IMAGE, [pair[1]]))
		uniforms.append(_uniform(10, RenderingDevice.UNIFORM_TYPE_IMAGE, [total]))
		_sets.append(_keep(_rd.uniform_set_create(uniforms, shader, 0)))
	_update_gpu(lights, true)
	_initialized.call_deferred()

func _dispatch(list: int, stage: int, bounce: int, set_id: int, offset: int = 0, amount: int = -1) -> void:
	var push := PackedInt32Array([stage, bounce, int(metadata.surfel_count), rays, int(metadata.atlas[0]), int(metadata.atlas[1]), int(metadata.bvh_nodes), offset]).to_byte_array()
	_rd.compute_list_bind_compute_pipeline(list, _pipeline)
	_rd.compute_list_bind_uniform_set(list, _sets[set_id], 0)
	_rd.compute_list_set_push_constant(list, push, push.size())
	var count := int(metadata.atlas[0]) * int(metadata.atlas[1]) if stage == 3 else int(metadata.surfel_count)
	if amount > 0: count = amount
	_rd.compute_list_dispatch(list, ceili(float(count) / 64.0), 1, 1)

func _update_gpu(lights: PackedFloat32Array, build: bool) -> void:
	if _closed: return
	_read_timing_gpu()
	var bytes := lights.to_byte_array()
	_rd.buffer_update(_light_buffer, 0, bytes.size(), bytes)
	if build:
		_rd.capture_timestamp("MeshGI transport begin")
		# Bound individual dispatches; no giant 262k-triangle brute-force kernel.
		for offset in range(0, int(metadata.surfel_count), 4096):
			var list := _rd.compute_list_begin()
			_dispatch(list, 0, 0, 0, offset, mini(4096, int(metadata.surfel_count) - offset))
			_rd.compute_list_end()
		_rd.capture_timestamp("MeshGI transport end")
	_rd.capture_timestamp("MeshGI lighting begin")
	var list := _rd.compute_list_begin()
	_dispatch(list, 1, 0, 1)
	_rd.compute_list_add_barrier(list)
	for bounce in 2:
		_dispatch(list, 2, bounce, bounce % 2)
		_rd.compute_list_add_barrier(list)
	_dispatch(list, 3, 0, 1)
	_rd.compute_list_end()
	_rd.capture_timestamp("MeshGI lighting end")
	_did_update.call_deferred(build)

func _read_timing_gpu() -> void:
	var frame := _rd.get_captured_timestamps_frame()
	if frame == _last_timing_frame: return
	_last_timing_frame = frame
	var starts: Dictionary = {}
	for index in _rd.get_captured_timestamps_count():
		var label := _rd.get_captured_timestamp_name(index)
		var time := _rd.get_captured_timestamp_gpu_time(index)
		for kind in ["transport", "lighting"]:
			if label == "MeshGI %s begin" % kind: starts[kind] = time
			if label == "MeshGI %s end" % kind and starts.has(kind):
				_set_time.call_deferred(kind, float(time - int(starts[kind])) / 1000000.0)

func _set_time(kind: String, value: float) -> void:
	stats[kind + "_gpu_ms"] = value

func _did_update(build: bool) -> void:
	pending = false
	stats.lighting_updates += 1
	if build: stats.transport_builds += 1
	update_complete.emit()

func _initialized() -> void:
	stats.ready = true
	ready_for_use.emit()

func _error(message: String) -> void:
	push_error(message)
	failed.emit(message)

func read_atlas(callback: Callable) -> void:
	RenderingServer.call_on_render_thread(_read_gpu.bind(callback))

func poll_timing() -> void:
	if stats.ready and not _closed: RenderingServer.call_on_render_thread(_read_timing_gpu)

func _read_gpu(callback: Callable) -> void:
	_read_timing_gpu()
	var data := _rd.texture_get_data(_atlas, 0)
	callback.call_deferred(Image.create_from_data(int(metadata.atlas[0]), int(metadata.atlas[1]), false, Image.FORMAT_RGBAH, data))

func shutdown() -> void:
	_closed = true
	RenderingServer.call_on_render_thread(_cleanup)

func _cleanup() -> void:
	texture.texture_rd_rid = RID()
	_resources.reverse()
	for resource in _resources:
		if resource.is_valid(): _rd.free_rid(resource)
	_resources.clear()

class_name SurfaceCache
extends RefCounted
## RenderingDevice work lives on the render thread. Static transport is built once;
## camera movement and unchanged lights submit no compute dispatches.

signal ready_for_use
signal update_complete
signal failed(message: String)

var texture := Texture2DRD.new()
var stats: Dictionary = {"ready": false, "surfel_count": 0, "transport_builds": 0, "lighting_updates": 0, "gpu_ms": null, "gpu_samples_ms": []}
var rays := 256
var bounces := 2
var emitter_sampling := true
var _rd: RenderingDevice
var _resources: Array[RID] = []
var _pipelines: Array[RID] = []
var _sets: Array[RID] = []
var _light_buffer := RID()
var _face_buffer := RID()
var _face_buffer_size := 0
var _total := RID()
var _layout: SurfaceLayout
var _closed := false
var _pending := false
var _shader_files: Array[RDShaderFile] = []
var _last_timestamp_frame := -1
var _emitter_pipeline := RID()
var _emitter_set := RID()
var _emitter_factors := RID()
var _emitter_transfer_ready := false
var _emission_present := false
var _sphere_buffer := RID()
var _sphere_set := RID()
var _sphere_dirty := false

func initialize(layout: SurfaceLayout, lights: PackedFloat32Array, sample_count: int = 256) -> void:
	_layout = layout
	_face_buffer_size = layout.face_data.size() * 4
	rays = sample_count
	stats.surfel_count = layout.surfels.size() / 8
	stats.atlas_size = [layout.atlas_size.x, layout.atlas_size.y]
	stats.rays = rays
	stats.bounces = bounces
	stats.emitter_refined_faces = layout.faces.filter(func(face): return face.emitter_refined).size()
	stats.emitter_sampling = emitter_sampling
	stats.emitter_transfer_builds = 0
	stats.sphere_updates = 0
	stats.emitter_transfer_bytes = int(stats.surfel_count) * layout.faces.size() * 4
	_emission_present = _has_emission(layout.face_data)
	stats.transfer_bytes = int(stats.surfel_count) * rays * 8
	for path in ["res://shaders/cache_transfer.glsl", "res://shaders/cache_direct.glsl", "res://shaders/cache_bounce.glsl", "res://shaders/cache_emitter_transfer.glsl", "res://shaders/cache_sphere.glsl"]:
		_shader_files.append(load(path) as RDShaderFile)
	_pending = true
	RenderingServer.call_on_render_thread(_initialize_gpu.bind(lights))

func update_lights(lights: PackedFloat32Array) -> bool:
	if _closed or _pending or not stats.ready:
		return false
	_pending = true
	RenderingServer.call_on_render_thread(_update_gpu.bind(lights, false))
	return true

func update_materials(face_data: PackedFloat32Array, lights: PackedFloat32Array) -> bool:
	if _closed or _pending or not stats.ready:
		return false
	if face_data.size() * 4 != _face_buffer_size:
		push_error("Surface material update must preserve the face buffer size; geometry changes require a new cache.")
		return false
	# Capture mutable CPU material data before scheduling render-thread work.
	var bytes := face_data.to_byte_array()
	var light_snapshot := lights.duplicate()
	_pending = true
	RenderingServer.call_on_render_thread(_update_materials_gpu.bind(bytes, light_snapshot, _has_emission(face_data)))
	return true

func update_sphere(source: PackedFloat32Array, lights: PackedFloat32Array) -> bool:
	# Two vec4s: world center/radius, linear emission RGB/padding.
	if _closed or _pending or not stats.ready:
		return false
	if source.size() != 8 or source[3] <= 0.0:
		push_error("Sphere source requires 8 floats and a positive radius.")
		return false
	for value in source:
		if not is_finite(value):
			push_error("Sphere source must be finite.")
			return false
	_pending = true
	RenderingServer.call_on_render_thread(_update_sphere_gpu.bind(source.to_byte_array(), lights.duplicate()))
	return true

static func _has_emission(data: PackedFloat32Array) -> bool:
	for offset in range(SurfaceLayout.FACE_EMISSION_OFFSET, data.size(), SurfaceLayout.FACE_FLOAT_COUNT):
		if data[offset] > 0.0 or data[offset + 1] > 0.0 or data[offset + 2] > 0.0:
			return true
	return false

func read_atlas(callback: Callable) -> void:
	RenderingServer.call_on_render_thread(_read_atlas_gpu.bind(callback))

func poll_gpu_timing() -> void:
	if not _closed and stats.ready:
		RenderingServer.call_on_render_thread(_read_timing_gpu)

func shutdown() -> void:
	if _closed:
		return
	_closed = true
	RenderingServer.call_on_render_thread(_cleanup_gpu)

func _keep(rid: RID) -> RID:
	_resources.append(rid)
	return rid

func _storage(data: PackedFloat32Array) -> RID:
	var bytes := data.to_byte_array()
	return _keep(_rd.storage_buffer_create(bytes.size(), bytes))

func _uniform(binding: int, kind: int, ids: Array[RID]) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.binding = binding
	uniform.uniform_type = kind
	for id in ids:
		uniform.add_id(id)
	return uniform

func _buffer(binding: int, rid: RID) -> RDUniform:
	return _uniform(binding, RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, [rid])

func _image(binding: int, rid: RID) -> RDUniform:
	return _uniform(binding, RenderingDevice.UNIFORM_TYPE_IMAGE, [rid])

func _create_texture() -> RID:
	var format := RDTextureFormat.new()
	format.width = _layout.atlas_size.x
	format.height = _layout.atlas_size.y
	format.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT
	var zero := PackedByteArray()
	zero.resize(format.width * format.height * 8)
	return _keep(_rd.texture_create(format, RDTextureView.new(), [zero]))

func _initialize_gpu(lights: PackedFloat32Array) -> void:
	if _closed:
		return
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		_fail.call_deferred("Forward+ / RenderingDevice is required. Headless mode cannot validate this GPU implementation.")
		return
	var shaders: Array[RID] = []
	for file in _shader_files:
		if file == null:
			_fail.call_deferred("Missing compute shader import; open/import the project first.")
			return
		var spirv := file.get_spirv()
		var error := spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE)
		if not error.is_empty():
			_fail.call_deferred(error)
			return
		var shader := _keep(_rd.shader_create_from_spirv(spirv))
		if not shader.is_valid():
			_fail.call_deferred("Compute shader creation failed")
			return
		shaders.append(shader)
		_pipelines.append(_keep(_rd.compute_pipeline_create(shader)))
	_face_buffer = _storage(_layout.face_data)
	var surfels := _storage(_layout.surfels)
	var boxes := _storage(_layout.box_data)
	var transfer := _keep(_rd.storage_buffer_create(int(stats.transfer_bytes)))
	_emitter_factors = _keep(_rd.storage_buffer_create(int(stats.emitter_transfer_bytes)))
	# Fixed capacity avoids descriptor recreation for lamp movement / toggles.
	_light_buffer = _keep(_rd.storage_buffer_create(16 * 12 * 4))
	var outgoing_a := _create_texture()
	var outgoing_b := _create_texture()
	_total = _create_texture()
	var sphere_incoming := _create_texture()
	_sphere_buffer = _keep(_rd.storage_buffer_create(32))
	var state := RDSamplerState.new()
	# Transfer sampling must not interpolate radiance across a thin wall on one
	# large floor chart. Receiver filtering separately checks chart visibility.
	state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	var sampler := _keep(_rd.sampler_create(state))
	_sets.append(_keep(_rd.uniform_set_create([_buffer(0, _face_buffer), _buffer(1, surfels), _buffer(2, boxes), _buffer(3, transfer)], shaders[0], 0)))
	_sets.append(_keep(_rd.uniform_set_create([_buffer(0, _face_buffer), _buffer(1, surfels), _buffer(2, boxes), _buffer(3, _light_buffer), _image(4, outgoing_a)], shaders[1], 0)))
	for pair in [[outgoing_a, outgoing_b], [outgoing_b, outgoing_a]]:
		_sets.append(_keep(_rd.uniform_set_create([_buffer(0, _face_buffer), _buffer(1, surfels), _buffer(2, transfer), _uniform(3, RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, [sampler, pair[0]]), _image(4, pair[1]), _image(5, _total), _buffer(6, _emitter_factors), _image(7, sphere_incoming)], shaders[2], 0)))
	_sphere_set = _keep(_rd.uniform_set_create([_buffer(0, _face_buffer), _buffer(1, surfels), _buffer(2, boxes), _buffer(3, _sphere_buffer), _image(4, sphere_incoming)], shaders[4], 0))
	if not _sphere_set.is_valid():
		_fail.call_deferred("Sphere source descriptor creation failed")
		return
	_emitter_pipeline = _pipelines[3]
	_emitter_set = _keep(_rd.uniform_set_create([_buffer(0, _face_buffer), _buffer(1, surfels), _buffer(2, boxes), _buffer(3, _emitter_factors)], shaders[3], 0))
	if not _emitter_set.is_valid():
		_fail.call_deferred("Emitter transfer descriptor creation failed")
		return
	for uniform_set in _sets:
		if not uniform_set.is_valid():
			_fail.call_deferred("Compute descriptor creation failed")
			return
	texture.texture_rd_rid = _total
	_update_gpu(lights, true)
	_initialized.call_deferred()

func _push(light_count: int, bounce: int = 0) -> PackedByteArray:
	return PackedInt32Array([int(stats.surfel_count), _layout.boxes.size(), light_count, rays, _layout.atlas_size.x, _layout.atlas_size.y, bounce, 1 if emitter_sampling else 0]).to_byte_array()

func _dispatch(list: int, pipeline: RID, uniform_set: RID, push: PackedByteArray) -> void:
	_rd.compute_list_bind_compute_pipeline(list, pipeline)
	_rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	_rd.compute_list_set_push_constant(list, push, push.size())
	_rd.compute_list_dispatch(list, ceili(float(stats.surfel_count) / 64.0), 1, 1)

func _update_materials_gpu(bytes: PackedByteArray, lights: PackedFloat32Array, has_emission: bool) -> void:
	if _closed:
		return
	_rd.buffer_update(_face_buffer, 0, bytes.size(), bytes)
	_emission_present = has_emission
	_update_gpu(lights, false)

func _update_sphere_gpu(bytes: PackedByteArray, lights: PackedFloat32Array) -> void:
	if _closed:
		return
	_rd.buffer_update(_sphere_buffer, 0, bytes.size(), bytes)
	_sphere_dirty = true
	_update_gpu(lights, false)

func _update_gpu(lights: PackedFloat32Array, build_transfer: bool) -> void:
	if _closed:
		return
	_read_timing_gpu()
	var bytes := lights.to_byte_array()
	if bytes.size() > 16 * 12 * 4:
		_fail.call_deferred("Surface cache supports at most 16 point lights")
		return
	if not bytes.is_empty():
		_rd.buffer_update(_light_buffer, 0, bytes.size(), bytes)
	var light_count := lights.size() / 12
	if build_transfer:
		_rd.capture_timestamp("TopDownGI transport begin")
		var transfer_list := _rd.compute_list_begin()
		_dispatch(transfer_list, _pipelines[0], _sets[0], _push(light_count))
		_rd.compute_list_end()
		_rd.capture_timestamp("TopDownGI transport end")
	if emitter_sampling and _emission_present and not _emitter_transfer_ready:
		_rd.capture_timestamp("TopDownGI emitters begin")
		var emitter_list := _rd.compute_list_begin()
		_dispatch(emitter_list, _emitter_pipeline, _emitter_set, _push(light_count))
		_rd.compute_list_end()
		_rd.capture_timestamp("TopDownGI emitters end")
		_emitter_transfer_ready = true
		_did_build_emitters.call_deferred()
	_rd.capture_timestamp("TopDownGI lighting begin")
	var list := _rd.compute_list_begin()
	if _sphere_dirty:
		_dispatch(list, _pipelines[4], _sphere_set, _push(light_count))
		_rd.compute_list_add_barrier(list)
		_sphere_dirty = false
		_did_update_sphere.call_deferred()
	_dispatch(list, _pipelines[1], _sets[1], _push(light_count))
	_rd.compute_list_add_barrier(list)
	for bounce in bounces:
		_dispatch(list, _pipelines[2], _sets[2 + bounce % 2], _push(light_count, bounce))
		if bounce + 1 < bounces:
			_rd.compute_list_add_barrier(list)
	_rd.compute_list_end()
	_rd.capture_timestamp("TopDownGI lighting end")
	_did_update.call_deferred(build_transfer)

func _read_timing_gpu() -> void:
	if _rd == null:
		return
	var frame := _rd.get_captured_timestamps_frame()
	if frame == _last_timestamp_frame:
		return
	_last_timestamp_frame = frame
	var start := -1
	var build_start := -1
	var emitter_start := -1
	for i in _rd.get_captured_timestamps_count():
		var label := _rd.get_captured_timestamp_name(i)
		if label == "TopDownGI emitters begin":
			emitter_start = _rd.get_captured_timestamp_gpu_time(i)
		elif label == "TopDownGI emitters end" and emitter_start >= 0:
			_set_emitter_time.call_deferred(float(_rd.get_captured_timestamp_gpu_time(i) - emitter_start) / 1000000.0)
		elif label == "TopDownGI transport begin":
			build_start = _rd.get_captured_timestamp_gpu_time(i)
		elif label == "TopDownGI transport end" and build_start >= 0:
			_set_build_time.call_deferred(float(_rd.get_captured_timestamp_gpu_time(i) - build_start) / 1000000.0)
		elif label == "TopDownGI lighting begin":
			start = _rd.get_captured_timestamp_gpu_time(i)
		elif label == "TopDownGI lighting end" and start >= 0:
			_set_gpu_time.call_deferred(float(_rd.get_captured_timestamp_gpu_time(i) - start) / 1000000.0)

func _read_atlas_gpu(callback: Callable) -> void:
	if _closed or not _total.is_valid():
		return
	_read_timing_gpu()
	var bytes := _rd.texture_get_data(_total, 0)
	callback.call_deferred(Image.create_from_data(_layout.atlas_size.x, _layout.atlas_size.y, false, Image.FORMAT_RGBAH, bytes))

func _set_gpu_time(value: float) -> void:
	stats.gpu_ms = value
	stats.gpu_samples_ms.append(value)
	if stats.gpu_samples_ms.size() > 240:
		stats.gpu_samples_ms.pop_front()

func _set_build_time(value: float) -> void:
	stats.transport_gpu_ms = value

func _set_emitter_time(value: float) -> void:
	stats.emitter_transfer_gpu_ms = value

func _did_build_emitters() -> void:
	stats.emitter_transfer_builds += 1

func _did_update_sphere() -> void:
	stats.sphere_updates += 1

func _initialized() -> void:
	stats.ready = true
	ready_for_use.emit()

func _did_update(built: bool) -> void:
	_pending = false
	stats.lighting_updates += 1
	if built:
		stats.transport_builds += 1
	update_complete.emit()

func _fail(message: String) -> void:
	_pending = false
	push_error(message)
	failed.emit(message)

func _cleanup_gpu() -> void:
	texture.texture_rd_rid = RID()
	if _rd == null:
		return
	# Free dependants (sets, pipelines) before their underlying buffers/textures.
	_resources.reverse()
	for rid in _resources:
		if rid.is_valid():
			_rd.free_rid(rid)
	_resources.clear()

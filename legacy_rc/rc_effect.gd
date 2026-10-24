@tool
class_name LegacyRadianceCascadesEffect
extends CompositorEffect

# Regression reference only: screen-space color transport, not the topdown GI path.
# This preserves the legacy perspective-depth approximation and material limitations.

const NUM_CASCADES := 5
const BASE_SPACING := 4
const BASE_DIRS := 8
const BASE_INTERVAL := 8.0

enum DebugView { NONE, INDIRECT_ONLY, CASCADE_ATLAS }

@export var gi_intensity: float = 1.5
@export var z_near: float = 0.1
@export var z_far: float = 100.0
@export_range(0.1, 5.0, 0.1) var depth_precision: float = 1.0
@export_range(0.1, 4.0, 0.05) var z_thickness: float = 1.0
@export var debug_view: DebugView = DebugView.NONE

var rd: RenderingDevice
var build_shader: RID
var merge_shader: RID
var apply_shader: RID
var blit_shader: RID
var build_pipeline: RID
var merge_pipeline: RID
var apply_pipeline: RID
var blit_pipeline: RID

var atlas_texture: RID
var atlas_size: Vector2i
var intermediate_texture: RID
var intermediate_size: Vector2i
var sampler_linear: RID

var _cached_build_uniform_set: RID
var _cached_merge_uniform_set: RID
var _cached_apply_uniform_set: RID
var _cached_blit_uniform_set: RID
var _cached_color: RID
var _cached_depth: RID

var _initialized := false
var _init_attempted := false

func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE
	access_resolved_color = true
	access_resolved_depth = true
	RenderingServer.call_on_render_thread(_init_gpu)

func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE and _initialized:
		_cleanup()

func _init_gpu() -> void:
	if _init_attempted:
		return
	_init_attempted = true

	rd = RenderingServer.get_rendering_device()
	if rd == null:
		push_error("RC: RenderingDevice is null")
		return

	build_shader = _load_shader("res://legacy_rc/rc_build.glsl")
	merge_shader = _load_shader("res://legacy_rc/rc_merge.glsl")
	apply_shader = _load_shader("res://legacy_rc/rc_apply.glsl")
	blit_shader = _load_shader("res://legacy_rc/rc_blit.glsl")

	for s_name in ["build", "merge", "apply", "blit"]:
		var s: RID = get("" + s_name + "_shader")
		if not s.is_valid():
			push_error("RC: %s shader failed to compile" % s_name)
			return

	build_pipeline = rd.compute_pipeline_create(build_shader)
	merge_pipeline = rd.compute_pipeline_create(merge_shader)
	apply_pipeline = rd.compute_pipeline_create(apply_shader)
	blit_pipeline = rd.compute_pipeline_create(blit_shader)

	var sampler_state := RDSamplerState.new()
	sampler_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_linear = rd.sampler_create(sampler_state)

	_initialized = build_pipeline.is_valid() and merge_pipeline.is_valid() and apply_pipeline.is_valid() and blit_pipeline.is_valid()
	if _initialized:
		print("Legacy screen-space RC initialized (%d cascades, base_dirs=%d, base_spacing=%d)" % [NUM_CASCADES, BASE_DIRS, BASE_SPACING])
	else:
		push_error("RC: pipeline creation failed")

func _load_shader(path: String) -> RID:
	var shader_file := load(path) as RDShaderFile
	if shader_file == null:
		push_error("RC: Failed to load shader file: %s" % path)
		return RID()
	var spirv := shader_file.get_spirv()
	if spirv == null:
		push_error("RC: Failed to get SPIR-V from: %s" % path)
		return RID()
	var shader_rid := rd.shader_create_from_spirv(spirv)
	if not shader_rid.is_valid():
		push_error("RC: SPIR-V compilation failed for: %s" % path)
	return shader_rid

func _cleanup() -> void:
	if rd == null:
		return
	_invalidate_uniform_sets()
	for tex in [atlas_texture, intermediate_texture]:
		if tex is RID and tex.is_valid():
			rd.free_rid(tex)
	if sampler_linear.is_valid():
		rd.free_rid(sampler_linear)
	for s in [build_shader, merge_shader, apply_shader, blit_shader]:
		if s.is_valid():
			rd.free_rid(s)
	for p in [build_pipeline, merge_pipeline, apply_pipeline, blit_pipeline]:
		if p.is_valid():
			rd.free_rid(p)

func _invalidate_uniform_sets() -> void:
	for us in [_cached_build_uniform_set, _cached_merge_uniform_set, _cached_apply_uniform_set, _cached_blit_uniform_set]:
		if us is RID and us.is_valid():
			rd.free_rid(us)
	_cached_build_uniform_set = RID()
	_cached_merge_uniform_set = RID()
	_cached_apply_uniform_set = RID()
	_cached_blit_uniform_set = RID()
	_cached_color = RID()
	_cached_depth = RID()

func _compute_cascade_layout(screen: Vector2i) -> Array:
	var cascades: Array = []
	var offset_x := 0

	for i in NUM_CASCADES:
		var info := {}
		info["spacing"] = BASE_SPACING << i
		info["dirs"] = BASE_DIRS << i
		info["probes_x"] = ceili(float(screen.x) / float(info["spacing"]))
		info["probes_y"] = ceili(float(screen.y) / float(info["spacing"]))
		var interval_offset: float = BASE_INTERVAL * (1.0 - pow(4.0, float(i))) / (1.0 - 4.0) if i > 0 else 0.0
		var interval_range := BASE_INTERVAL * pow(4.0, float(i))
		var next_spacing := float(BASE_SPACING << (i + 1))
		interval_range += next_spacing * 1.5
		info["interval_start"] = interval_offset
		info["interval_end"] = interval_offset + interval_range
		info["atlas_offset_x"] = offset_x
		info["atlas_offset_y"] = 0

		offset_x += info["probes_x"] * info["dirs"]
		cascades.append(info)

	return cascades

func _ensure_atlas(cascades: Array) -> void:
	var total_w := 0
	var total_h := 0
	for c in cascades:
		total_w = maxi(total_w, c["atlas_offset_x"] + c["probes_x"] * c["dirs"])
		total_h = maxi(total_h, c["probes_y"])

	var needed := Vector2i(total_w, total_h)
	if atlas_texture.is_valid() and atlas_size == needed:
		return

	if atlas_texture.is_valid():
		rd.free_rid(atlas_texture)
	_invalidate_uniform_sets()

	var fmt := RDTextureFormat.new()
	fmt.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	fmt.width = needed.x
	fmt.height = needed.y
	fmt.usage_bits = (
		RenderingDevice.TEXTURE_USAGE_STORAGE_BIT |
		RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT |
		RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT |
		RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT
	)
	atlas_texture = rd.texture_create(fmt, RDTextureView.new())
	atlas_size = needed

func _ensure_intermediate(screen: Vector2i) -> void:
	if intermediate_texture.is_valid() and intermediate_size == screen:
		return
	if intermediate_texture.is_valid():
		rd.free_rid(intermediate_texture)
	_invalidate_uniform_sets()

	var fmt := RDTextureFormat.new()
	fmt.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	fmt.width = screen.x
	fmt.height = screen.y
	fmt.usage_bits = (
		RenderingDevice.TEXTURE_USAGE_STORAGE_BIT |
		RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT |
		RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT |
		RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT
	)
	intermediate_texture = rd.texture_create(fmt, RDTextureView.new())
	intermediate_size = screen

func _render_callback(p_effect_callback_type: int, p_render_data: RenderData) -> void:
	if not _initialized or rd == null:
		return

	var scene_buffers := p_render_data.get_render_scene_buffers()
	if scene_buffers == null:
		return

	var render_scene_buffers_rd := scene_buffers as RenderSceneBuffersRD
	if render_scene_buffers_rd == null:
		return

	var render_size: Vector2i = render_scene_buffers_rd.get_internal_size()
	if render_size.x <= 0 or render_size.y <= 0:
		return

	var color_image: RID = render_scene_buffers_rd.get_color_layer(0)
	var depth_image: RID = render_scene_buffers_rd.get_depth_layer(0)
	if not color_image.is_valid() or not depth_image.is_valid():
		return

	var cascades := _compute_cascade_layout(render_size)
	_ensure_atlas(cascades)
	_ensure_intermediate(render_size)
	if not atlas_texture.is_valid() or not intermediate_texture.is_valid():
		return

	if color_image != _cached_color or depth_image != _cached_depth:
		_invalidate_uniform_sets()
		_cached_color = color_image
		_cached_depth = depth_image

	_clear_atlas()
	_pass_build(color_image, depth_image, render_size, cascades)
	_pass_merge(cascades, render_size)

	var intensity := gi_intensity
	if debug_view == DebugView.INDIRECT_ONLY:
		intensity = -1.0
	elif debug_view == DebugView.CASCADE_ATLAS:
		intensity = -2.0

	_pass_apply(depth_image, color_image, render_size, cascades[0], intensity)
	_pass_blit(intermediate_texture, color_image, render_size)

# --- Clear atlas to prevent stale data bleed ---

func _clear_atlas() -> void:
	if not atlas_texture.is_valid():
		return
	rd.texture_clear(atlas_texture, Color(0, 0, 0, 1), 0, 1, 0, 1)

# --- Build pass: ray march each cascade (batched into one compute list) ---

func _pass_build(color: RID, depth: RID, screen: Vector2i, cascades: Array) -> void:
	if not _cached_build_uniform_set.is_valid():
		var color_uniform := RDUniform.new()
		color_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		color_uniform.binding = 0
		color_uniform.add_id(sampler_linear)
		color_uniform.add_id(color)

		var depth_uniform := RDUniform.new()
		depth_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		depth_uniform.binding = 1
		depth_uniform.add_id(sampler_linear)
		depth_uniform.add_id(depth)

		var atlas_uniform := RDUniform.new()
		atlas_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		atlas_uniform.binding = 2
		atlas_uniform.add_id(atlas_texture)

		_cached_build_uniform_set = rd.uniform_set_create(
			[color_uniform, depth_uniform, atlas_uniform], build_shader, 0)

	var cl := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(cl, build_pipeline)
	rd.compute_list_bind_uniform_set(cl, _cached_build_uniform_set, 0)

	for i in cascades.size():
		var c: Dictionary = cascades[i]

		var push_ints := PackedInt32Array()
		push_ints.append(screen.x)
		push_ints.append(screen.y)
		push_ints.append(i)
		push_ints.append(c["dirs"])
		push_ints.append(c["spacing"])

		var push_floats := PackedFloat32Array()
		push_floats.append(c["interval_start"])
		push_floats.append(c["interval_end"])
		push_floats.append(z_near)
		push_floats.append(z_far)

		var push_ints2 := PackedInt32Array()
		push_ints2.append(c["atlas_offset_x"])
		push_ints2.append(c["atlas_offset_y"])
		push_ints2.append(c["probes_x"])
		push_ints2.append(c["probes_y"])

		var push_floats2 := PackedFloat32Array()
		push_floats2.append(depth_precision)
		push_floats2.append(z_thickness)

		var push_pad := PackedInt32Array()
		push_pad.append(0)

		var push_bytes := push_ints.to_byte_array()
		push_bytes.append_array(push_floats.to_byte_array())
		push_bytes.append_array(push_ints2.to_byte_array())
		push_bytes.append_array(push_floats2.to_byte_array())
		push_bytes.append_array(push_pad.to_byte_array())

		var groups_x := ceili(float(c["probes_x"] * c["dirs"]) / 16.0)
		var groups_y := ceili(float(c["probes_y"]) / 16.0)

		rd.compute_list_set_push_constant(cl, push_bytes, push_bytes.size())
		rd.compute_list_dispatch(cl, groups_x, groups_y, 1)

	rd.compute_list_end()

# --- Merge pass: combine cascades from far to near (batched) ---

func _pass_merge(cascades: Array, screen: Vector2i) -> void:
	if not _cached_merge_uniform_set.is_valid():
		var atlas_uniform := RDUniform.new()
		atlas_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		atlas_uniform.binding = 0
		atlas_uniform.add_id(atlas_texture)

		_cached_merge_uniform_set = rd.uniform_set_create(
			[atlas_uniform], merge_shader, 0)

	var cl := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(cl, merge_pipeline)
	rd.compute_list_bind_uniform_set(cl, _cached_merge_uniform_set, 0)

	for i in range(cascades.size() - 2, -1, -1):
		var dst: Dictionary = cascades[i]
		var src: Dictionary = cascades[i + 1]

		var push := PackedInt32Array()
		push.append(dst["atlas_offset_x"])
		push.append(dst["atlas_offset_y"])
		push.append(src["atlas_offset_x"])
		push.append(src["atlas_offset_y"])
		push.append(dst["probes_x"])
		push.append(dst["probes_y"])
		push.append(src["probes_x"])
		push.append(src["probes_y"])
		push.append(dst["dirs"])
		push.append(src["dirs"])
		push.append(dst["spacing"])
		push.append(src["spacing"])
		push.append(screen.x)
		push.append(screen.y)
		push.append(i)
		push.append(i + 1)

		var push_bytes := push.to_byte_array()

		var groups_x := ceili(float(dst["probes_x"] * dst["dirs"]) / 16.0)
		var groups_y := ceili(float(dst["probes_y"]) / 16.0)

		rd.compute_list_set_push_constant(cl, push_bytes, push_bytes.size())
		rd.compute_list_dispatch(cl, groups_x, groups_y, 1)
		# The next dispatch reads the cascade just written by this dispatch.
		# CPU dispatch order alone does not declare this GPU dependency.
		if i > 0:
			rd.compute_list_add_barrier(cl)

	rd.compute_list_end()

# --- Apply pass: add indirect lighting to scene ---

func _pass_apply(depth: RID, color: RID, screen: Vector2i, c0: Dictionary, intensity: float) -> void:
	if not _cached_apply_uniform_set.is_valid():
		var depth_uniform := RDUniform.new()
		depth_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		depth_uniform.binding = 0
		depth_uniform.add_id(sampler_linear)
		depth_uniform.add_id(depth)

		var atlas_uniform := RDUniform.new()
		atlas_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		atlas_uniform.binding = 1
		atlas_uniform.add_id(atlas_texture)

		var color_uniform := RDUniform.new()
		color_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		color_uniform.binding = 2
		color_uniform.add_id(sampler_linear)
		color_uniform.add_id(color)

		var output_uniform := RDUniform.new()
		output_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		output_uniform.binding = 3
		output_uniform.add_id(intermediate_texture)

		_cached_apply_uniform_set = rd.uniform_set_create(
			[depth_uniform, atlas_uniform, color_uniform, output_uniform], apply_shader, 0)

	var push_ints := PackedInt32Array()
	push_ints.append(screen.x)
	push_ints.append(screen.y)
	push_ints.append(c0["atlas_offset_x"])
	push_ints.append(c0["atlas_offset_y"])
	push_ints.append(c0["probes_x"])
	push_ints.append(c0["probes_y"])
	push_ints.append(c0["spacing"])
	push_ints.append(c0["dirs"])

	var push_floats := PackedFloat32Array()
	push_floats.append(intensity)
	push_floats.append(z_near)
	push_floats.append(z_far)
	push_floats.append(z_thickness)

	var push_bytes := push_ints.to_byte_array()
	push_bytes.append_array(push_floats.to_byte_array())

	var groups_x := ceili(float(screen.x) / 8.0)
	var groups_y := ceili(float(screen.y) / 8.0)

	var cl := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(cl, apply_pipeline)
	rd.compute_list_bind_uniform_set(cl, _cached_apply_uniform_set, 0)
	rd.compute_list_set_push_constant(cl, push_bytes, push_bytes.size())
	rd.compute_list_dispatch(cl, groups_x, groups_y, 1)
	rd.compute_list_end()

func _pass_blit(src: RID, dst: RID, screen: Vector2i) -> void:
	if not _cached_blit_uniform_set.is_valid():
		var src_uniform := RDUniform.new()
		src_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		src_uniform.binding = 0
		src_uniform.add_id(sampler_linear)
		src_uniform.add_id(src)

		var dst_uniform := RDUniform.new()
		dst_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		dst_uniform.binding = 1
		dst_uniform.add_id(dst)

		_cached_blit_uniform_set = rd.uniform_set_create(
			[src_uniform, dst_uniform], blit_shader, 0)

	var push := PackedInt32Array()
	push.append(screen.x)
	push.append(screen.y)
	push.append(0)
	push.append(0)
	var push_bytes := push.to_byte_array()

	var groups_x := ceili(float(screen.x) / 8.0)
	var groups_y := ceili(float(screen.y) / 8.0)

	var cl := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(cl, blit_pipeline)
	rd.compute_list_bind_uniform_set(cl, _cached_blit_uniform_set, 0)
	rd.compute_list_set_push_constant(cl, push_bytes, push_bytes.size())
	rd.compute_list_dispatch(cl, groups_x, groups_y, 1)
	rd.compute_list_end()

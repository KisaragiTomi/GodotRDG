extends Node3D
## Sponza with the TG-style live GI (TGScreenGI) and the optional sunlit atmosphere. No lighting is baked: only the
## triangle BVH and material data from tools/prepare-sponza.ps1 are precomputed.

var geometry: Node3D
var camera: Camera3D
var camera_controls: FreeFlyCamera
var sun: DirectionalLight3D
var lamp: OmniLight3D
var marker: MeshInstance3D
var status: Label
var ready_for_input := false
var overview := false
var capture_path := ""
var materials: Array[ShaderMaterial] = []
var mode := 1
var tg: TGScreenGI
var blocker: MeshInstance3D
var _blocker_state := 0
var _tg_debug: Dictionary = {}
var tg_stable_denoise := true
var noise_capture_path := ""
var camera_capture_path := ""
var tg_sampling := 0
var tg_post_denoise := true
var tg_world_cache := false
var _timing_values: Dictionary = {}
var _timing_title: Label
var _timing_clock := 0.0
var _viewport_timing_window: Array[float] = []
@export var atmosphere_requested := false
var atmosphere: TGAtmosphere
var atmosphere_capture_path := ""
var surface_fog: TGSurfaceFog
var separate_surface_fog := false
var _sky_material: ShaderMaterial
var _sky_source := Vector3(-1, -1, -1)

func _ready() -> void:
	var native_fog_only := "--tg-native-fog" in OS.get_cmdline_user_args()
	for arg in OS.get_cmdline_user_args():
		if arg == "--atmosphere": atmosphere_requested = true
		if arg.begins_with("--atmosphere-capture="):
			atmosphere_requested = true
			atmosphere_capture_path = arg.trim_prefix("--atmosphere-capture=")
		if arg == "--tg-legacy-denoise":
			tg_stable_denoise = false
			tg_post_denoise = false
		if arg == "--tg-basic-filter": tg_post_denoise = false
		if arg == "--tg-world-cache": tg_world_cache = true
		if arg.begins_with("--tg-noise-capture="): noise_capture_path = arg.trim_prefix("--tg-noise-capture=")
		if arg.begins_with("--tg-camera-capture="): camera_capture_path = arg.trim_prefix("--tg-camera-capture=")
		if arg.begins_with("--tg-sampling="): tg_sampling = clampi(arg.trim_prefix("--tg-sampling=").to_int(), 0, 3)
		if arg.begins_with("--sponza-capture="):
			capture_path = arg.trim_prefix("--sponza-capture=")
	separate_surface_fog = atmosphere_requested and TGSurfaceFog.supported() and not native_fog_only
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	_create_environment()
	_create_ui()
	geometry = (load("res://assets/sponza/glTF/Sponza.gltf") as PackedScene).instantiate()
	geometry.name = "Sponza"
	add_child(geometry)
	_create_lights()
	if atmosphere_requested:
		sun.look_at_from_position(Vector3(-0.75, 0.65, -0.2) * 30.0, Vector3.ZERO)
		sun.light_color = Color(1.0, 0.88, 0.69)
		sun.light_energy = 4.0
		lamp.visible = false
		atmosphere = TGAtmosphere.new()
		atmosphere.distance_scale = 0.28
		atmosphere.thickness = 1.1
		atmosphere.fog_length = 55.0
		atmosphere.sky_enabled = "--tg-no-sky" not in OS.get_cmdline_user_args()
		add_child(atmosphere)
		atmosphere.setup(get_world_3d().environment, sun)
		_create_sky()
		lamp.light_volumetric_fog_energy = 0.0
	camera = Camera3D.new()
	camera.near = 0.08
	camera.far = 100.0
	camera.fov = 72.0
	add_child(camera)
	_reset_camera()
	camera.current = true
	camera_controls = FreeFlyCamera.new()
	camera_controls.camera = camera
	add_child(camera_controls)
	get_viewport().msaa_3d = Viewport.MSAA_2X
	get_viewport().use_taa = true
	await _start_tg()

func _create_environment() -> void:
	var world := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.055, 0.075, 0.11)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color.BLACK
	env.ambient_light_energy = 0.0
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.glow_enabled = false
	env.ssao_enabled = false
	env.ssil_enabled = false
	world.environment = env
	add_child(world)

func _create_sky() -> void:
	_sky_material = ShaderMaterial.new()
	_sky_material.shader = preload("res://shaders/tg_sky.gdshader")
	var sky := Sky.new()
	sky.sky_material = _sky_material
	var env := get_world_3d().environment
	env.sky = sky
	env.background_mode = Environment.BG_SKY
	# Ambient injection stays disabled: only unoccluded rays receive sky light.

func _set_sky_enabled(value: bool) -> void:
	if atmosphere == null: return
	atmosphere.sky_enabled = value
	atmosphere.apply()

func _create_lights() -> void:
	sun = DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-58, -35, 0)
	sun.light_color = Color(1.0, 0.95, 0.86)
	sun.light_energy = 2.0
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 60.0
	sun.shadow_normal_bias = 0.4
	add_child(sun)
	lamp = OmniLight3D.new()
	lamp.position = Vector3(-2.0, 2.0, 0.0)
	lamp.light_color = Color(0.25, 0.6, 1.0)
	lamp.light_energy = 16.0
	lamp.omni_range = 8.0
	lamp.shadow_enabled = true
	lamp.shadow_normal_bias = 0.15
	add_child(lamp)
	marker = MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 0.2
	sphere.height = 0.4
	marker.mesh = sphere
	marker.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	marker.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
	var material := StandardMaterial3D.new()
	material.albedo_color = lamp.light_color
	material.emission_enabled = true
	material.emission = lamp.light_color
	material.emission_energy_multiplier = 4.0
	marker.material_override = material
	material.set_flag(BaseMaterial3D.FLAG_DISABLE_FOG, separate_surface_fog)
	lamp.add_child(marker)

func _create_ui() -> void:
	var canvas := CanvasLayer.new()
	add_child(canvas)
	var panel := PanelContainer.new()
	panel.position = Vector2(12, 12)
	canvas.add_child(panel)
	status = Label.new()
	status.add_theme_font_size_override("font_size", 17)
	panel.add_child(status)
	status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	panel.set_anchors_and_offsets_preset(Control.PRESET_TOP_WIDE)
	panel.offset_left = 12
	panel.offset_right = -300
	panel.offset_top = 12
	_create_timing_ui(canvas)

func _create_timing_ui(canvas: CanvasLayer) -> void:
	var panel := PanelContainer.new()
	canvas.add_child(panel)
	panel.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	panel.offset_left = -288
	panel.offset_right = -12
	panel.offset_top = 12
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.018, 0.026, 0.04, 0.92)
	style.set_content_margin_all(12)
	style.set_corner_radius_all(5)
	panel.add_theme_stylebox_override("panel", style)
	var font := SystemFont.new()
	font.font_names = PackedStringArray(["Microsoft YaHei UI", "Microsoft YaHei", "Consolas"])
	panel.theme = Theme.new()
	panel.theme.default_font = font
	panel.theme.default_font_size = 15
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 7)
	column.mouse_filter = Control.MOUSE_FILTER_IGNORE
	panel.add_child(column)
	_timing_title = Label.new()
	_timing_title.text = "GI 阶段耗时 · GPU"
	column.add_child(_timing_title)
	var caption := Label.new()
	caption.text = "毫秒 · 最近 16 帧均值"
	caption.modulate = Color(0.65, 0.73, 0.82)
	caption.add_theme_font_size_override("font_size", 12)
	column.add_child(caption)
	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 18)
	grid.add_theme_constant_override("v_separation", 4)
	column.add_child(grid)
	for pair in [["guide", "深度 / 几何引导"], ["budget", "自适应射线分配"], ["trace", "射线追踪（屏幕/BVH）"], ["project", "候选 SH 归约"], ["reconstruct", "邻域重建 / 历史累积"], ["filter", "降噪 · 细尺度"], ["filter_medium", "降噪 · 中尺度"], ["filter_wide", "降噪 · 粗尺度"], ["filter_large", "降噪 · 大范围"], ["cache_select", "世界缓存 · 选点"], ["cache_resolve", "世界缓存 · 更新"], ["history", "历史颜色 / 深度复制"]]:
		_add_timing_row(grid, pair[0], pair[1])
	column.add_child(HSeparator.new())
	var totals := GridContainer.new()
	totals.columns = 2
	totals.add_theme_constant_override("h_separation", 18)
	column.add_child(totals)
	_add_timing_row(totals, "total", "GI 计算合计")
	_add_timing_row(totals, "viewport", "视口 GPU")
	# Keep changing debug text out of the image-noise comparison captures.
	panel.visible = noise_capture_path.is_empty() and camera_capture_path.is_empty()

func _add_timing_row(grid: GridContainer, key: String, text: String) -> void:
	var label := Label.new()
	label.text = text
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(label)
	var value := Label.new()
	value.text = "--"
	value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value.custom_minimum_size.x = 60
	grid.add_child(value)
	_timing_values[key] = value

func _update_timing_ui(delta: float) -> void:
	if tg == null or _timing_values.is_empty(): return
	_viewport_timing_window.append(RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid()))
	if _viewport_timing_window.size() > 16: _viewport_timing_window.pop_front()
	_timing_clock += delta
	if _timing_clock < 0.25: return
	_timing_clock = 0.0
	_timing_title.text = "GI 阶段耗时 · GPU" if mode != 0 else "GI 阶段耗时 · 已关闭"
	var stages: Dictionary = tg.stats.get("stage_gpu_ms", {})
	for key in _timing_values:
		var label: Label = _timing_values[key]
		if key == "viewport":
			var sum := 0.0
			for sample in _viewport_timing_window: sum += sample
			label.text = "%.3f" % (sum / _viewport_timing_window.size())
		elif mode == 0:
			label.text = "0.000" if key == "total" else "--"
		else:
			var skipped: bool = (key == "project" and tg_sampling in [0, 3]) or (key == "budget" and tg_sampling != 3) or (key in ["filter_medium", "filter_wide", "filter_large"] and not tg_post_denoise)
			if key in ["cache_select", "cache_resolve"]: skipped = not stages.has(key)
			label.text = "跳过" if skipped else ("%.3f" % stages[key] if stages.has(key) else "--")
		label.modulate = Color(1.0, 0.78, 0.43) if key == "trace" else Color(0.9, 0.95, 1.0)

func _update_status() -> void:
	if tg == null: return
	var names := ["DIRECT / NO GI", "TG-STYLE / SCREEN + BVH + SH", "TG-STYLE / BVH ONLY + SH", "TG-STYLE / INDIRECT (BVH)", "TG-STYLE / NO TEMPORAL HISTORY"]
	if atmosphere != null and atmosphere.environment.volumetric_fog_enabled: names[1] = "TG-STYLE / CLEAN SCREEN + BVH + ATMOSPHERE" if separate_surface_fog else "TG-STYLE / BVH + ATMOSPHERE"
	status.text = "Sponza | %s | %s | %s\n1 Direct  2 TG  3 BVH  4 Indirect  5 No history  6 Sampling  7 Denoise  B Blocker\nHold RMB Look  WASD Fly  E/Space Up  Q Down  Shift Fast  Wheel Speed\nArrows/PgUp/PgDn Move lamp  L Lamp  R Reset  P Preset  Esc Release/Close" % [names[mode], ["SPARSE 4x4 / 1 RAY", "QUALITY 2/4/8 RAYS", "REFERENCE 32 RAYS", "ADAPTIVE 4x4 / MAX +50%"][tg_sampling], "POST DENOISE" if tg_post_denoise else "BASIC FILTER"]
	if atmosphere != null: status.text += "\nF Fog  [ / ] Density  J/K Sun azimuth  U/I Sun elevation  8 Sky %s" % ("ON" if atmosphere.sky_enabled else "OFF")
	status.text += "\n9 World cache %s (experimental)" % ("ON" if tg_world_cache else "OFF")

func _reset_camera() -> void:
	if camera_controls != null: camera_controls.release_mouse()
	camera.projection = Camera3D.PROJECTION_PERSPECTIVE
	if overview:
		camera.position = Vector3(20, 23, 17)
		camera.look_at(Vector3(0, 2, 0))
	else:
		camera.position = Vector3(5.8, 6.2, 0.3)
		camera.look_at(Vector3(-5.0, 1.6, -0.3))
		if atmosphere_requested:
			camera.position = Vector3(5.7, 2.1, 0.3)
			camera.look_at(Vector3(-8.0, 4.0, 0.0))

func _process(delta: float) -> void:
	var interactive := ready_for_input and capture_path.is_empty() and noise_capture_path.is_empty() and camera_capture_path.is_empty() and atmosphere_capture_path.is_empty()
	if camera_controls != null: camera_controls.enabled = interactive
	if tg != null: _push_tg_inputs()
	_update_timing_ui(delta)
	if not interactive:
		return
	var right := camera.global_basis.x
	right.y = 0
	right = right.normalized()
	var forward := -camera.global_basis.z
	forward.y = 0
	forward = forward.normalized()
	var move := Vector3.ZERO
	if Input.is_physical_key_pressed(KEY_UP): move += forward
	if Input.is_physical_key_pressed(KEY_DOWN): move -= forward
	if Input.is_physical_key_pressed(KEY_RIGHT): move += right
	if Input.is_physical_key_pressed(KEY_LEFT): move -= right
	# Q / E / Space belong to the fly camera.
	if Input.is_physical_key_pressed(KEY_PAGEUP): move += Vector3.UP
	if Input.is_physical_key_pressed(KEY_PAGEDOWN): move -= Vector3.UP
	lamp.global_position += move.limit_length() * delta * 3.0
	lamp.global_position = lamp.global_position.clamp(Vector3(-11, 0.4, -3.2), Vector3(11, 9.0, 3.2))

func _unhandled_input(event: InputEvent) -> void:
	if not ready_for_input or not capture_path.is_empty() or not noise_capture_path.is_empty() or not camera_capture_path.is_empty() or not atmosphere_capture_path.is_empty():
		return
	if event is InputEventKey and event.pressed and not event.echo:
		match event.physical_keycode:
			KEY_F:
				if atmosphere != null:
					atmosphere.active = not atmosphere.active
					atmosphere.apply()
					_update_status()
			KEY_BRACKETLEFT, KEY_BRACKETRIGHT:
				if atmosphere != null:
					atmosphere.thickness = clampf(atmosphere.thickness + (0.1 if event.physical_keycode == KEY_BRACKETRIGHT else -0.1), 0.0, 4.0)
					atmosphere.apply()
					_update_status()
			KEY_J, KEY_K:
				if atmosphere != null:
					sun.rotate_y(0.1 if event.physical_keycode == KEY_K else -0.1)
			KEY_U, KEY_I:
				if atmosphere != null:
					sun.rotation.x = clampf(sun.rotation.x + (0.08 if event.physical_keycode == KEY_I else -0.08), -1.4, -0.1)
			KEY_1, KEY_2, KEY_3, KEY_4, KEY_5:
				if event.physical_keycode == KEY_5 and tg == null: return
				_select_mode(event.physical_keycode - KEY_1)
			KEY_B:
				if tg != null: _move_blocker()
			KEY_7:
				if tg != null:
					tg_post_denoise = not tg_post_denoise
					tg.set_post_denoise(tg_post_denoise)
					_update_status()
			KEY_8:
				if atmosphere != null and tg != null:
					_set_sky_enabled(not atmosphere.sky_enabled)
					_update_status()
			KEY_9:
				if tg != null:
					tg_world_cache = not tg_world_cache
					tg.set_world_cache(tg_world_cache)
					_update_status()
			KEY_6:
				if tg != null:
					tg_sampling = {0: 3, 3: 1, 1: 0, 2: 0}[tg_sampling]
					tg.set_sampling(tg_sampling)
					_update_status()
			KEY_P:
				overview = not overview
				_reset_camera()
			KEY_R:
				_reset_camera()
				lamp.global_position = Vector3(-2, 2, 0)
				if atmosphere != null:
					sun.look_at_from_position(Vector3(-0.75, 0.65, -0.2) * 30.0, Vector3.ZERO)
					atmosphere.thickness = 1.1
					atmosphere.active = true
					atmosphere.sky_enabled = true
					atmosphere.apply()
					_update_status()
			KEY_L: lamp.visible = not lamp.visible
			KEY_ESCAPE: get_tree().quit()

func _settle(frames: int = 45) -> void:
	for frame in frames:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw

func _light_data() -> PackedFloat32Array:
	var direction := sun.global_basis.z.normalized()
	var sun_rgb := sun.light_color.srgb_to_linear() * (sun.light_energy if sun.visible else 0.0)
	var lamp_rgb := lamp.light_color.srgb_to_linear() * (lamp.light_energy if lamp.visible else 0.0)
	var point := lamp.global_position
	return PackedFloat32Array([direction.x, direction.y, direction.z, 0, sun_rgb.r, sun_rgb.g, sun_rgb.b, 0, point.x, point.y, point.z, lamp.omni_range, lamp_rgb.r, lamp_rgb.g, lamp_rgb.b, lamp.omni_attenuation])

func _create_materials() -> void:
	var shader := load("res://shaders/mesh_surface.gdshader") as Shader
	var two_sided := Shader.new()
	two_sided.code = shader.code.replace("render_mode diffuse_lambert;", "render_mode diffuse_lambert, cull_disabled;")
	if separate_surface_fog:
		shader = shader.duplicate()
		shader.code = shader.code.replace("render_mode diffuse_lambert;", "render_mode diffuse_lambert, fog_disabled;")
		two_sided.code = two_sided.code.replace("render_mode diffuse_lambert, cull_disabled;", "render_mode diffuse_lambert, cull_disabled, fog_disabled;")
	var instances := geometry.find_children("*", "MeshInstance3D", true, false)
	if blocker != null: instances.append(blocker)
	for child in instances:
		var instance := child as MeshInstance3D
		for index in instance.mesh.get_surface_count():
			var source := instance.mesh.surface_get_material(index) as StandardMaterial3D
			var material := ShaderMaterial.new()
			material.shader = two_sided if source.cull_mode == BaseMaterial3D.CULL_DISABLED else shader
			material.set_shader_parameter("base_color", source.albedo_color)
			material.set_shader_parameter("albedo_texture", source.albedo_texture)
			material.set_shader_parameter("normal_texture", source.normal_texture)
			material.set_shader_parameter("has_normal", source.normal_enabled and source.normal_texture != null)
			material.set_shader_parameter("normal_scale", source.normal_scale)
			material.set_shader_parameter("metallic_texture", source.metallic_texture)
			material.set_shader_parameter("roughness_texture", source.roughness_texture)
			material.set_shader_parameter("has_metallic", source.metallic_texture != null)
			material.set_shader_parameter("has_roughness", source.roughness_texture != null)
			material.set_shader_parameter("metallic_factor", source.metallic)
			material.set_shader_parameter("roughness_factor", source.roughness)
			material.set_shader_parameter("metallic_channel", source.metallic_texture_channel)
			material.set_shader_parameter("roughness_channel", source.roughness_texture_channel)
			material.set_shader_parameter("alpha_cutoff", source.alpha_scissor_threshold if source.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR else -1.0)
			material.set_shader_parameter("tg_sh", tg.sh_texture)
			material.set_shader_parameter("tg_position", tg.position_texture)
			material.set_shader_parameter("tg_normal", tg.normal_texture)
			instance.set_surface_override_material(index, material)
			materials.append(material)

func _select_mode(value: int) -> void:
	mode = value
	marker.visible = mode != 3
	for material in materials: material.set_shader_parameter("gi_mode", 0 if mode == 0 else (5 if mode == 3 else 4))
	_push_tg_inputs()
	_update_status()

func _exit_tree() -> void:
	if tg != null: tg.shutdown()
	if surface_fog != null: surface_fog.shutdown()

func _start_tg() -> void:
	status.text = "Sponza: loading TG-style live GI (no light transport bake)..."
	blocker = MeshInstance3D.new()
	blocker.name = "LiveBlocker"
	var box := BoxMesh.new()
	box.size = Vector3(0.3, 3.0, 3.0)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.6, 0.6, 0.6)
	var white := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	white.fill(Color.WHITE)
	material.albedo_texture = ImageTexture.create_from_image(white)
	box.material = material
	blocker.mesh = box
	blocker.position = Vector3(-1, 1.5, 0)
	blocker.visible = false
	add_child(blocker)
	tg = TGScreenGI.new()
	tg.stable_denoise = tg_stable_denoise
	tg.post_denoise = tg_post_denoise
	tg.sampling_profile = tg_sampling
	tg.surface_history = separate_surface_fog
	tg.world_cache = tg_world_cache
	# Captures and validation scripts read the GI debug state every few frames: keep it ready instead of costing a frame.
	tg.debug_readback = not capture_path.is_empty() or not noise_capture_path.is_empty() or not camera_capture_path.is_empty() or not atmosphere_capture_path.is_empty() or Array(OS.get_cmdline_user_args()).any(func(arg): return arg.contains("-output=") or arg.contains("capture") or arg.begins_with("--validate") or arg.begins_with("--benchmark"))
	tg.failed.connect(func(message): push_error(message); get_tree().quit(1))
	tg.resize_requested.connect(func(size): tg.prepare(size))
	tg.prepare(Vector2i(get_viewport().get_visible_rect().size))
	await tg.buffers_ready
	# Post-opaque companion of the GI effect: keeps this frame's radiance for the next frame's screen hits.
	var history: CompositorEffect = tg.history_effect
	var compositor := Compositor.new()
	compositor.compositor_effects = [tg, history]
	if separate_surface_fog:
		surface_fog = TGSurfaceFog.new()
		compositor.compositor_effects = [tg, history, surface_fog]
	camera.compositor = compositor
	_push_tg_inputs()
	_create_materials()
	for shader_material in materials: shader_material.set_shader_parameter("tg_stable_denoise", tg_stable_denoise)
	_select_mode(1)
	ready_for_input = true
	print("TG_READY: live screen/BVH tracing, no transport precomputation")
	DemoViewer.configure(self, "Sponza | TG GI + atmosphere", atmosphere, tg)
	if not atmosphere_capture_path.is_empty():
		await _capture_atmosphere()
		return
	if not camera_capture_path.is_empty():
		await _capture_tg_camera()
		return
	if not noise_capture_path.is_empty():
		await _capture_tg_noise()
		return
	if not capture_path.is_empty(): await _capture_tg()

func _push_tg_inputs() -> void:
	var box: AABB = blocker.global_transform * blocker.mesh.get_aabb()
	var color := Color(0.6, 0.6, 0.6).srgb_to_linear()
	var descriptor := PackedFloat32Array([box.position.x, box.position.y, box.position.z, float(blocker.visible), box.end.x, box.end.y, box.end.z, 0, color.r, color.g, color.b, 0])
	# Stock engines retain the safe BVH fallback. With the native fog interface,
	# capture clean opaque radiance first, then composite fog into the MSAA target.
	var fog_active := atmosphere != null and atmosphere.environment.volumetric_fog_enabled
	var sky := atmosphere.sky_radiance() if atmosphere != null else Vector3.ZERO
	if _sky_material != null and sky != _sky_source:
		_sky_material.set_shader_parameter("radiance", sky)
		_sky_source = sky
	tg.set_inputs(_light_data(), descriptor, mode != 0, mode == 1 and (separate_surface_fog or not fog_active), 1 if mode == 4 else 48, sky)

func _move_blocker() -> void:
	_blocker_state = (_blocker_state + 1) % 3
	blocker.visible = _blocker_state != 0
	blocker.position.x = -1.0 if _blocker_state == 1 else 1.0
	if atmosphere != null: atmosphere.invalidate_history()
	_push_tg_inputs()

func _capture_atmosphere() -> void:
	DirAccess.make_dir_recursive_absolute(atmosphere_capture_path)
	for canvas in find_children("*", "CanvasLayer", true, false): canvas.visible = false
	# Keep the transport path equal for the two visual reference images.
	_select_mode(2)
	atmosphere.active = false
	atmosphere.apply()
	await _settle(90)
	var clear := get_viewport().get_texture().get_image()
	clear.save_png(atmosphere_capture_path.path_join("sponza_clear.png"))
	atmosphere.active = true
	atmosphere.apply()
	await _settle(90)
	var fog := get_viewport().get_texture().get_image()
	fog.save_png(atmosphere_capture_path.path_join("sponza_fog.png"))
	_select_mode(1)
	await _settle(60)
	var debug := await _read_tg_debug()
	var sh_metrics := _tg_metrics(debug.sh)
	var result := {"gpu": RenderingServer.get_video_adapter_name(), "gi_frames": debug.frame, "surface_history": debug.surface_history, "screen_hits_with_fog": debug.counters[1], "bvh_rays": debug.counters[2], "sh_finite": sh_metrics.finite, "fog_enabled": atmosphere.environment.volumetric_fog_enabled, "fog_difference": _image_difference(clear, fog, 0)}
	var screen_valid: bool = debug.counters[1] > 0 if separate_surface_fog else debug.counters[1] == 0
	result["passed"] = debug.frame > 0 and screen_valid and debug.counters[2] > 0 and sh_metrics.finite and result.fog_enabled and result.fog_difference > 0.005
	sun.rotate_y(0.5)
	await _settle(90)
	get_viewport().get_texture().get_image().save_png(atmosphere_capture_path.path_join("sponza_sun_rotated.png"))
	FileAccess.open(atmosphere_capture_path.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("SPONZA_ATMOSPHERE_RESULT: ", JSON.stringify(result))
	get_tree().quit(0 if result.passed else 1)

func _read_tg_debug() -> Dictionary:
	_tg_debug = {}
	tg.read_debug(func(value): _tg_debug = value)
	while _tg_debug.is_empty(): await get_tree().process_frame
	return _tg_debug

func _capture_tg() -> void:
	DirAccess.make_dir_recursive_absolute(capture_path)
	_select_mode(0)
	await _settle()
	var direct := get_viewport().get_texture().get_image()
	direct.save_png(capture_path.path_join("tg_direct.png"))
	_select_mode(1)
	await _settle(150)
	var lit := get_viewport().get_texture().get_image()
	lit.save_png(capture_path.path_join("tg_gi.png"))
	var debug := await _read_tg_debug()
	(debug.sh as Image).save_exr(capture_path.path_join("tg_sh.exr"), false)
	(debug.position as Image).save_exr(capture_path.path_join("tg_position.exr"), false)
	(debug.age as Image).save_exr(capture_path.path_join("tg_age.exr"), false)
	var counters: Array = debug.counters
	var result := {"initial_counters": counters, "no_transport_precompute": tg.stats.transport_builds == 0 and tg.stats.transfer_bytes == 0}
	result["screen_and_bvh_active"] = counters[1] > 0 and counters[2] > 0 and counters[3] > 0
	result["temporal_reprojection_active"] = counters[4] > counters[5]
	result["initial_sh"] = _tg_metrics(debug.sh)
	result["taa_history"] = debug.taa_history
	result["initial_age"] = _tg_metrics(debug.age)
	result["gi_image_difference"] = _image_difference(direct, lit)
	var timing: Array = tg.stats.gpu_samples_ms.duplicate()
	timing.sort()
	result["gi_gpu_ms_median"] = timing[timing.size() / 2] if not timing.is_empty() else null
	var frame_times: Array[float] = []
	for index in 45:
		await get_tree().process_frame
		frame_times.append(RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid()))
	frame_times.sort()
	result["viewport_gpu_ms_median"] = frame_times[frame_times.size() / 2]
	_select_mode(2)
	await _settle(100)
	var stable_a := await _read_tg_debug()
	await _settle(2)
	var stable_b := await _read_tg_debug()
	result["stable_sh_delta"] = _image_difference(stable_a.sh, stable_b.sh, 0)
	result["bvh_only_active"] = stable_b.counters[1] == 0 and stable_b.counters[2] > 0 and stable_b.counters[3] > 0
	get_viewport().get_texture().get_image().save_png(capture_path.path_join("tg_bvh_only.png"))
	_select_mode(4)
	await _settle(8)
	var raw_a := await _read_tg_debug()
	await _settle(2)
	var raw_b := await _read_tg_debug()
	result["raw_sh_delta"] = _image_difference(raw_a.sh, raw_b.sh, 0)
	result["temporal_reduces_noise"] = result.stable_sh_delta < result.raw_sh_delta * 0.8
	get_viewport().get_texture().get_image().save_png(capture_path.path_join("tg_no_history.png"))
	_select_mode(1)
	await _settle(60)
	var resets_before: int = tg.stats.history_resets
	lamp.position += Vector3(3, 0, 1)
	await _settle(60)
	get_viewport().get_texture().get_image().save_png(capture_path.path_join("tg_moved_light.png"))
	result["moving_light_resets_history"] = tg.stats.history_resets > resets_before
	_move_blocker()
	await _settle(60)
	var blocked := await _read_tg_debug()
	result["live_blocker_hit_count"] = blocked.counters[6]
	get_viewport().get_texture().get_image().save_png(capture_path.path_join("tg_live_blocker.png"))
	_move_blocker()
	await _settle(12)
	result["no_geometry_rebake"] = tg.stats.transport_builds == 0
	_move_blocker()
	await _settle(60)
	camera.position += Vector3(-0.7, 0.0, 0.5)
	await _settle(2)
	var camera_change := await _read_tg_debug()
	result["camera_reprojection_counters"] = camera_change.counters
	result["camera_sh_finite"] = _tg_metrics(camera_change.sh).finite
	get_viewport().get_texture().get_image().save_png(capture_path.path_join("tg_camera_move.png"))
	_reset_camera()
	sun.visible = false
	lamp.visible = false
	var restore_sky := atmosphere != null and atmosphere.sky_enabled
	_set_sky_enabled(false)
	await _settle(3)
	var dark := await _read_tg_debug()
	result["lights_off"] = _tg_metrics(dark.sh)
	result["lights_off_inputs"] = dark.lights
	result["lights_off_counters"] = dark.counters
	result["lights_off_cooldown"] = dark.cooldown
	(dark.candidate as Image).save_exr(capture_path.path_join("tg_off_candidate.exr"), false)
	(dark.sh as Image).save_exr(capture_path.path_join("tg_off_sh.exr"), false)
	await _settle(60)
	get_viewport().get_texture().get_image().save_png(capture_path.path_join("tg_lights_off.png"))
	sun.visible = true
	lamp.visible = true
	_set_sky_enabled(restore_sky)
	lamp.position = Vector3(-2, 2, 0)
	_select_mode(3)
	await _settle(100)
	get_viewport().get_texture().get_image().save_png(capture_path.path_join("tg_indirect.png"))
	_select_mode(1)
	await _settle(100)
	var restored := await _read_tg_debug()
	result["restored_sh"] = _tg_metrics(restored.sh)
	# Exercise actual render-target recreation as well as history invalidation.
	var original_size := get_window().size
	get_window().size = Vector2i(960, 600)
	await _settle(45)
	var resized := await _read_tg_debug()
	result["resize_ok"] = (resized.position as Image).get_width() == 480 and _tg_metrics(resized.sh).finite
	get_window().size = original_size
	await _settle(90)
	var final_image := get_viewport().get_texture().get_image()
	final_image.save_png(capture_path.path_join("tg_gi.png"))
	result["resize_image_difference"] = _image_difference(direct, final_image)
	result["resize_ok"] = result.resize_ok and result.resize_image_difference > result.gi_image_difference * 0.5
	result["frames"] = tg.stats.frames
	result["passed"] = result.no_transport_precompute and result.no_geometry_rebake and result.screen_and_bvh_active and result.temporal_reprojection_active and result.initial_sh.finite and result.initial_sh.maximum > 0.0 and result.gi_image_difference > 0.001 and result.bvh_only_active and result.temporal_reduces_noise and result.moving_light_resets_history and result.live_blocker_hit_count > 0 and result.camera_sh_finite and result.lights_off.maximum == 0.0 and result.restored_sh.finite and result.restored_sh.maximum > 0.0 and result.resize_ok
	var file := FileAccess.open(capture_path.path_join("results.json"), FileAccess.WRITE)
	file.store_string(JSON.stringify(result, "\t"))
	print("TG_RESULT ", JSON.stringify(result))
	get_tree().quit(0 if result.passed else 1)

func _capture_tg_camera() -> void:
	DirAccess.make_dir_recursive_absolute(camera_capture_path)
	var result := {"profile": tg_sampling, "post_denoise": tg_post_denoise, "adaptive_priority": tg.adaptive_priority, "samples": []}
	# Hold denoising, materials, lights and camera poses fixed across profiles.
	# World tracing isolates sampling error from screen-color feedback changes.
	_select_mode(0)
	await _settle(4)
	_select_mode(2)
	await _settle(1)
	result.samples.append(await _save_tg_camera_sample("cold"))
	await _settle(150)
	result.samples.append(await _save_tg_camera_sample("warm"))
	var timing: Array = tg.stats.gpu_samples_ms.duplicate()
	timing.sort()
	result["static_gpu_ms_median"] = timing[timing.size() / 2]
	var origin := camera.position
	for step in range(1, 49):
		camera.position = origin + Vector3(0.018 * step, 0.0, 0.04 * step)
		await _settle(1)
		if step % 8 == 0: result.samples.append(await _save_tg_camera_sample("pan_%02d" % step))
	await _settle(90)
	result.samples.append(await _save_tg_camera_sample("settled"))
	# Exercise the normal screen + BVH mode while returning along the path.
	_select_mode(1)
	await _settle(60)
	for step in range(24, -1, -1):
		camera.position = origin + Vector3(0.036 * step, 0.0, 0.08 * step)
		await _settle(1)
	var integrated := await _read_tg_debug()
	result["integrated_counters"] = integrated.counters
	result["screen_and_bvh_active"] = integrated.counters[1] > 0 and integrated.counters[2] > 0
	get_viewport().get_texture().get_image().save_png(camera_capture_path.path_join("tg_camera_motion.png"))
	result["profile_switch_ok"] = true
	if tg_sampling in [1, 3]:
		tg.set_sampling(0)
		await _settle(6)
		var sparse := await _read_tg_debug()
		tg.set_sampling(tg_sampling)
		await _settle(30)
		var quality := await _read_tg_debug()
		result.profile_switch_ok = sparse.sampling_profile == 0 and quality.sampling_profile == tg_sampling and quality.counters[0] >= sparse.counters[0] and _tg_metrics(quality.sh).maximum > 0.0
	sun.visible = false
	lamp.visible = false
	_set_sky_enabled(false)
	await _settle(3)
	result["lights_off"] = _tg_metrics((await _read_tg_debug()).sh)
	result["passed"] = result.screen_and_bvh_active and result.profile_switch_ok and result.lights_off.maximum == 0.0
	for sample in result.samples: result.passed = result.passed and sample.finite
	FileAccess.open(camera_capture_path.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("TG_CAMERA_RESULT ", JSON.stringify(result))
	get_tree().quit(0 if result.passed else 1)

func _save_tg_camera_sample(label: String) -> Dictionary:
	var debug := await _read_tg_debug()
	for field in ["sh", "position", "normal", "age"]:
		FileAccess.open(camera_capture_path.path_join(label + "_" + field + ".bin"), FileAccess.WRITE).store_buffer((debug[field] as Image).get_data())
	if not debug.ray_budget.is_empty(): FileAccess.open(camera_capture_path.path_join(label + "_budget.bin"), FileAccess.WRITE).store_buffer(debug.ray_budget)
	get_viewport().get_texture().get_image().save_png(camera_capture_path.path_join(label + ".png"))
	var times: Array = tg.stats.gpu_samples_ms.slice(-8)
	times.sort()
	return {"name": label, "frame": debug.frame, "width": (debug.position as Image).get_width(), "height": (debug.position as Image).get_height(), "trace_width": debug.trace_size.x, "trace_height": debug.trace_size.y, "counters": debug.counters, "camera_moving": debug.camera_moving, "age": _tg_age_stats(debug), "finite": _tg_metrics(debug.sh).finite, "recent_gpu_ms_median": times[times.size() / 2] if not times.is_empty() else 0.0}

func _capture_tg_noise() -> void:
	DirAccess.make_dir_recursive_absolute(noise_capture_path)
	var result := {"stable_denoise": tg_stable_denoise, "post_denoise": tg_post_denoise}
	_select_mode(1)
	await _settle(180)
	result["static"] = await _tg_noise_series("static")
	var resets: int = tg.stats.history_resets
	var previous := await _read_tg_debug()
	var moving_deltas: Array[float] = []
	var moving_age := {}
	for step in 60:
		lamp.position.x += 0.035
		await _settle(1)
		if step % 6 == 5:
			var current := await _read_tg_debug()
			moving_deltas.append(_image_difference(previous.sh, current.sh, 0))
			previous = current
			moving_age = _tg_age_stats(current)
		if step == 29: get_viewport().get_texture().get_image().save_png(noise_capture_path.path_join("moving.png"))
	result["moving"] = {"sh_delta": _mean_values(moving_deltas), "hard_resets": tg.stats.history_resets - resets, "age": moving_age}
	await _settle(120)
	result["settled_after_move"] = await _tg_noise_series("settled")
	sun.visible = false
	lamp.visible = false
	_set_sky_enabled(false)
	await _settle(3)
	result["lights_off"] = _tg_metrics((await _read_tg_debug()).sh)
	var timing: Array = tg.stats.gpu_samples_ms.duplicate()
	timing.sort()
	result["gpu_ms_median"] = timing[timing.size() / 2]
	result["passed"] = result.lights_off.maximum == 0.0 and result.static.finite and result.settled_after_move.finite
	if tg_stable_denoise: result.passed = result.passed and result.static.taa_history and result.moving.hard_resets == 0 and result.moving.age.near_lamp_mean > 2.0
	FileAccess.open(noise_capture_path.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("TG_NOISE_RESULT ", JSON.stringify(result))
	get_tree().quit(0 if result.passed else 1)

func _tg_noise_series(label: String) -> Dictionary:
	var previous := await _read_tg_debug()
	var before := get_viewport().get_texture().get_image()
	before.save_png(noise_capture_path.path_join(label + "_first.png"))
	var deltas: Array[float] = []
	var frame_deltas: Array[float] = []
	var finite := true
	for index in 8:
		await _settle(1)
		var current := await _read_tg_debug()
		var frame := get_viewport().get_texture().get_image()
		deltas.append(_image_difference(previous.sh, current.sh, 0))
		frame_deltas.append(_image_difference(before, frame))
		finite = finite and _tg_metrics(current.sh).finite
		previous = current
		before = frame
	before.save_png(noise_capture_path.path_join(label + "_last.png"))
	return {"sh_delta": _mean_values(deltas), "frame_delta": _mean_values(frame_deltas), "age": _tg_age_stats(previous), "finite": finite, "taa_history": previous.taa_history}

func _mean_values(values: Array[float]) -> float:
	var sum := 0.0
	for value in values: sum += value
	return sum / maxi(values.size(), 1)

func _tg_age_stats(debug: Dictionary) -> Dictionary:
	var age := (debug.age as Image).duplicate() as Image
	age.convert(Image.FORMAT_RF)
	var ages := age.get_data().to_float32_array()
	var positions := (debug.position as Image).get_data().to_float32_array()
	var total := 0.0
	var count := 0
	var near_total := 0.0
	var near_count := 0
	for index in range(0, ages.size(), 17):
		if positions[index * 4 + 3] <= 0.0: continue
		total += ages[index]
		count += 1
		var point := Vector3(positions[index * 4], positions[index * 4 + 1], positions[index * 4 + 2])
		if point.distance_to(lamp.position) < lamp.omni_range:
			near_total += ages[index]
			near_count += 1
	return {"mean": total / maxi(count, 1), "near_lamp_mean": near_total / maxi(near_count, 1)}

func _tg_metrics(image: Image) -> Dictionary:
	var copy := image.duplicate() as Image
	copy.convert(Image.FORMAT_RGBAF)
	var finite := true
	var maximum := 0.0
	var sum := 0.0
	var count := 0
	for value in copy.get_data().to_float32_array():
		finite = finite and is_finite(value)
		maximum = maxf(maximum, absf(value))
		sum += absf(value)
		count += 1
	return {"finite": finite, "maximum": maximum, "mean_absolute": sum / maxi(count, 1)}

func _image_difference(a: Image, b: Image, top: int = 150) -> float:
	var sum := 0.0
	var count := 0
	for y in range(top, mini(a.get_height(), b.get_height()), 4):
		for x in range(0, mini(a.get_width(), b.get_width()), 4):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			sum += absf(ca.r-cb.r) + absf(ca.g-cb.g) + absf(ca.b-cb.b)
			count += 3
	return sum / maxi(count, 1)

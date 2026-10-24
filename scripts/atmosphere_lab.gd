extends Node3D
## Self-contained courtyard; no extracted game assets or Sponza download needed.

var camera: Camera3D
var controls: FreeFlyCamera
var sun: DirectionalLight3D
var atmosphere: TGAtmosphere
var environment: Environment
var shutter: MeshInstance3D
var hud: CanvasLayer
var status: Label
var capture_path := ""
var preset := 0
var _moving_shutter := false
var _clock := 0.0
var _stone: StandardMaterial3D
var _dark_stone: StandardMaterial3D

func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--atmosphere-capture="): capture_path = arg.trim_prefix("--atmosphere-capture=")
	if RenderingServer.get_current_rendering_method() != "gl_compatibility":
		RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	_build_world()
	_build_courtyard()
	camera = Camera3D.new()
	camera.near = 0.08
	camera.far = 140.0
	camera.fov = 68.0
	add_child(camera)
	camera.current = true
	_reset_camera()
	controls = FreeFlyCamera.new()
	controls.camera = camera
	add_child(controls)
	controls.enabled = capture_path.is_empty()
	get_viewport().use_taa = true
	get_viewport().msaa_3d = Viewport.MSAA_2X
	atmosphere = TGAtmosphere.new()
	atmosphere.thickness = 1.1
	atmosphere.center = Vector3(0, 0, -7)
	add_child(atmosphere)
	atmosphere.setup(environment, sun)
	_build_ui()
	DemoViewer.configure(self, "Courtyard | atmosphere", atmosphere)
	print("ATMOSPHERE_READY: TG Gaussian density, HG g=0.65, native shadowed volume")
	if not capture_path.is_empty(): await _capture()

func _build_world() -> void:
	environment = Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.22, 0.34, 0.49)
	sky_material.sky_horizon_color = Color(0.67, 0.69, 0.67)
	sky_material.ground_horizon_color = sky_material.sky_horizon_color
	sky_material.ground_bottom_color = Color(0.19, 0.23, 0.21)
	sky_material.sun_angle_max = 3.0
	sky.sky_material = sky_material
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.48, 0.59, 0.72)
	environment.ambient_light_energy = 0.22
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	environment.ssao_enabled = true
	environment.ssao_radius = 1.1
	environment.ssao_intensity = 1.4
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	sun = DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_color = Color(1.0, 0.86, 0.62)
	sun.light_energy = 3.5
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 90.0
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	sun.directional_shadow_blend_splits = true
	sun.shadow_bias = 0.03
	sun.shadow_normal_bias = 0.3
	add_child(sun)
	_reset_sun()

func _material(color: Color) -> StandardMaterial3D:
	var result := StandardMaterial3D.new()
	result.albedo_color = color
	result.roughness = 0.92
	return result

func _box(label: String, position_value: Vector3, size: Vector3, material_value: Material) -> MeshInstance3D:
	var result := MeshInstance3D.new()
	result.name = label
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh.material = material_value
	result.mesh = mesh
	result.position = position_value
	add_child(result)
	return result

func _build_courtyard() -> void:
	_stone = _material(Color(0.62, 0.57, 0.46))
	_dark_stone = _material(Color(0.32, 0.36, 0.32))
	var grass := _material(Color(0.20, 0.28, 0.15))
	_box("Meadow", Vector3(0, -0.24, -8), Vector3(100, 0.4, 100), grass)
	_box("Courtyard", Vector3(0, -0.05, -7), Vector3(13, 0.3, 34), _dark_stone)
	var rng := RandomNumberGenerator.new()
	rng.seed = 817
	for x in range(-4, 5):
		for z in range(-11, 5):
			var color := Color(0.42, 0.43, 0.38) * rng.randf_range(0.88, 1.1)
			_box("Paving", Vector3(x * 1.1, 0.13, z * 2.0), Vector3(1.06, 0.08, 1.96), _material(color))
	for side in [-1.0, 1.0]:
		for index in 8:
			var z := 10.0 - index * 4.4
			_box("Pillar", Vector3(side * 5.3, 2.2, z), Vector3(0.9, 4.4, 0.75), _stone)
			_box("Footing", Vector3(side * 5.3, 0.4, z), Vector3(1.24, 0.7, 1.08), _dark_stone)
			_box("Capital", Vector3(side * 5.3, 4.05, z), Vector3(1.16, 0.32, 1.08), _stone)
			if index < 7: _arch(side * 5.3, z - 2.2)
		_box("Cornice", Vector3(side * 5.3, 6.5, -5.4), Vector3(1.3, 0.5, 32.4), _stone)
		_box("WindowSillWall", Vector3(side * 5.3, 0.95, -5.4), Vector3(0.72, 1.9, 32.4), _stone)
	# Roof slats cast long, world-space light shafts across the gallery.
	for index in 12:
		_box("RoofBeam", Vector3(0, 6.45, 9.0 - index * 2.7), Vector3(11.4, 0.35, 2.1), _dark_stone)
	_box("EndWall", Vector3(0, 3.1, -22), Vector3(11.5, 6.2, 0.8), _stone)
	for side in [-1.0, 1.0]:
		_box("Bench", Vector3(side * 3.8, 0.9, -10.5), Vector3(1.1, 0.28, 5.0), _stone)
		for z in [-12.3, -8.7]: _box("BenchLeg", Vector3(side * 3.8, 0.45, z), Vector3(0.8, 0.8, 0.35), _dark_stone)
	shutter = _box("MovingShutter", Vector3(-5.2, 4.0, 1.2), Vector3(0.3, 5.6, 4.0), _dark_stone)
	shutter.visible = false
	for index in 16:
		var x := rng.randf_range(9, 27) * (-1.0 if index % 2 else 1.0)
		var z := rng.randf_range(-35, 0)
		var height := rng.randf_range(5, 10)
		_box("DistantTrunk", Vector3(x, height * 0.3, z), Vector3(0.5, height * 0.6, 0.5), _dark_stone)
		var canopy := MeshInstance3D.new()
		var sphere := SphereMesh.new()
		sphere.radius = height * 0.32
		sphere.height = height * 0.8
		sphere.radial_segments = 12
		sphere.rings = 6
		sphere.material = _material(Color(0.15, 0.24, 0.14) * rng.randf_range(0.8, 1.1))
		canopy.mesh = sphere
		canopy.position = Vector3(x, height * 0.72, z)
		add_child(canopy)

func _arch(x: float, z: float) -> void:
	# Individual voussoirs form an opening; the opening remains real geometry.
	for index in 13:
		var angle := (index + 0.5) * PI / 13.0
		var stone := _box("ArchStone", Vector3(x, 4.0 + sin(angle) * 1.8, z + cos(angle) * 1.8), Vector3(0.94, 0.48, 0.49), _stone)
		stone.rotation.x = angle

func _reset_sun() -> void:
	sun.look_at_from_position(Vector3(-0.85, 0.58, -0.4) * 30.0, Vector3.ZERO)

func _reset_camera() -> void:
	if preset == 0:
		camera.position = Vector3(3.1, 2.7, 10.5)
		camera.look_at(Vector3(-2.1, 2.8, -9.0))
	elif preset == 1:
		camera.position = Vector3(13.7, 8.0, 17.0)
		camera.look_at(Vector3(0, 2.0, -7))
	else:
		camera.position = Vector3(-3.2, 2.3, -17.0)
		camera.look_at(Vector3(1.5, 3.1, 8.0))

func _build_ui() -> void:
	hud = CanvasLayer.new()
	add_child(hud)
	var panel := PanelContainer.new()
	panel.set_anchors_and_offsets_preset(Control.PRESET_TOP_WIDE)
	panel.offset_left = 20
	panel.offset_right = -20
	panel.offset_top = 18
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	hud.add_child(panel)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.025, 0.04, 0.05, 0.88)
	style.set_content_margin_all(14)
	style.set_corner_radius_all(7)
	panel.add_theme_stylebox_override("panel", style)
	status = Label.new()
	status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	status.add_theme_font_size_override("font_size", 16)
	panel.add_child(status)
	_update_status()

func _update_status() -> void:
	status.text = "SUNLIT ATMOSPHERE  /  TG scattering study\nFog %s   Thickness %.2f   Anisotropy %.2f   Sun %s\nF fog   [ / ] density   G forward / isotropic   L sunlight   B moving shutter   Arrows sun angle\nR reset   P camera presets   RMB + WASD fly   Space / Ctrl height   Shift fast   H hide panel" % ["ON" if atmosphere.active else "OFF", atmosphere.thickness, atmosphere.anisotropy, "ON" if sun.visible else "OFF"]

func _process(delta: float) -> void:
	if atmosphere == null or not capture_path.is_empty(): return
	_clock += delta
	if _moving_shutter:
		shutter.position.z = 1.2 + sin(_clock * 0.8) * 5.5
		atmosphere.invalidate_history()
	var azimuth := float(Input.is_physical_key_pressed(KEY_RIGHT)) - float(Input.is_physical_key_pressed(KEY_LEFT))
	var elevation := float(Input.is_physical_key_pressed(KEY_UP)) - float(Input.is_physical_key_pressed(KEY_DOWN))
	if azimuth != 0 or elevation != 0:
		sun.rotation.y += azimuth * delta * 0.4
		sun.rotation.x = clampf(sun.rotation.x + elevation * delta * 0.3, -1.4, -0.1)

func _unhandled_input(event: InputEvent) -> void:
	if not capture_path.is_empty() or not event is InputEventKey or not event.pressed or event.echo: return
	match event.physical_keycode:
		KEY_F: atmosphere.active = not atmosphere.active
		KEY_BRACKETLEFT: atmosphere.thickness = maxf(0.0, atmosphere.thickness - 0.15)
		KEY_BRACKETRIGHT: atmosphere.thickness = minf(4.0, atmosphere.thickness + 0.15)
		KEY_G: atmosphere.anisotropy = 0.0 if atmosphere.anisotropy > 0.0 else 0.65
		KEY_L: sun.visible = not sun.visible
		KEY_B:
			_moving_shutter = not _moving_shutter
			shutter.visible = _moving_shutter
		KEY_P:
			preset = (preset + 1) % 3
			_reset_camera()
		KEY_R:
			preset = 0
			_reset_camera()
			_reset_sun()
			sun.visible = true
			shutter.visible = false
			_moving_shutter = false
			atmosphere.active = true
			atmosphere.thickness = 1.1
			atmosphere.anisotropy = 0.65
		KEY_H: hud.visible = not hud.visible
		KEY_ESCAPE: get_tree().quit()
	atmosphere.apply()
	_update_status()

func _settle(frames := 72) -> void:
	for index in frames: await get_tree().process_frame
	await RenderingServer.frame_post_draw

func _save(label: String) -> Image:
	await _settle()
	var image := get_viewport().get_texture().get_image()
	image.save_png(capture_path.path_join(label + ".png"))
	return image

func _difference(a: Image, b: Image) -> float:
	var total := 0.0
	var count := 0
	for y in range(150, a.get_height() - 30, 4):
		for x in range(30, a.get_width() - 30, 4):
			var delta := a.get_pixel(x, y) - b.get_pixel(x, y)
			total += absf(delta.r) + absf(delta.g) + absf(delta.b)
			count += 3
	return total / maxf(count, 1)

func _capture() -> void:
	DirAccess.make_dir_recursive_absolute(capture_path)
	if RenderingServer.get_rendering_device() == null:
		FileAccess.open(capture_path.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify({"passed": false, "gpu": "SKIP: no RenderingDevice; run with Vulkan"}))
		get_tree().quit(2)
		return
	hud.visible = false
	atmosphere.active = false
	atmosphere.apply()
	var clear := await _save("01_clear")
	atmosphere.active = true
	atmosphere.apply()
	var fog := await _save("02_sunlit_fog")
	atmosphere.sunlight = 0.0
	atmosphere.apply()
	var no_scatter := await _save("03_no_sun_scattering")
	atmosphere.sunlight = 4.0
	atmosphere.anisotropy = 0.0
	atmosphere.apply()
	var isotropic := await _save("04_isotropic")
	atmosphere.anisotropy = 0.65
	atmosphere.apply()
	shutter.visible = true
	atmosphere.invalidate_history()
	var blocked := await _save("05_shutter")
	shutter.position.z -= 5.5
	atmosphere.invalidate_history()
	var moved := await _save("06_moved_shutter")
	shutter.visible = false
	sun.rotate_y(0.6)
	var rotated := await _save("07_rotated_sun")
	_reset_sun()
	sun.visible = false
	atmosphere.invalidate_history()
	await _save("08_sun_off")
	sun.visible = true
	atmosphere.thickness = 0.0
	atmosphere.apply()
	var zero := await _save("09_zero_density")
	atmosphere.thickness = 1.1
	atmosphere.apply()
	preset = 1
	_reset_camera()
	await _save("10_overview")
	preset = 2
	_reset_camera()
	await _save("11_reverse")
	preset = 0
	_reset_camera()
	await _settle()
	var restored := await _save("12_restored")
	hud.visible = true
	await _save("13_controls")
	var result := {
		"gpu": RenderingServer.get_video_adapter_name(),
		"renderer": RenderingServer.get_current_rendering_method(),
		"fog_difference": _difference(clear, fog),
		"sun_scatter_difference": _difference(fog, no_scatter),
		"anisotropy_difference": _difference(fog, isotropic),
		"shutter_difference": _difference(fog, blocked),
		"moving_shutter_difference": _difference(blocked, moved),
		"rotated_sun_difference": _difference(fog, rotated),
		"zero_density_error": _difference(clear, zero),
		"restored_error": _difference(fog, restored),
		"viewport_gpu_ms": RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid())
	}
	result["passed"] = result.fog_difference > 0.005 and result.sun_scatter_difference > 0.002 and result.anisotropy_difference > 0.001 and result.moving_shutter_difference > 0.001 and result.zero_density_error < 0.005 and result.restored_error < 0.005
	FileAccess.open(capture_path.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("ATMOSPHERE_RESULT: ", JSON.stringify(result))
	get_tree().quit(0 if result.passed else 1)

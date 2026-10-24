extends Node3D
## Kenney city: native direct/ambient lighting and the shared TG atmosphere.

const VIEW_NAMES := ["Skyline", "Sunlit avenue", "Central square"]
const VIEW_POSITIONS := [Vector3(340, 260, 370), Vector3(33, 5.5, 166), Vector3(13, 8, 46)]
const VIEW_TARGETS := [Vector3.ZERO, Vector3(-12, 19, -140), Vector3(-75, 26, -98)]
const FOG_THICKNESS := 0.8
const FOG_SCALE := 1.4
const FOG_LENGTH := 480.0
const EDITOR_FOG_LENGTH := 1800.0
const BALANCED_FOG_GRID := Vector2i(224, 192)
const FOG_RANGE_STEP := 128.0

var camera: Camera3D
var sun: DirectionalLight3D
var atmosphere: TGAtmosphere
var controls: FreeFlyCamera
var hud: CanvasLayer
var status: Label
var preset := 0
var capture_path := ""
var _initial_sun_rotation: Vector3
var balanced_fog := true
var _project_fog_grid := Vector2i(256, 192)
var _city_bounds := AABB(Vector3(-240, -2, -240), Vector3(480, 2, 480))

func _ready() -> void:
	_project_fog_grid = Vector2i(ProjectSettings.get_setting("rendering/environment/volumetric_fog/volume_size", 256), ProjectSettings.get_setting("rendering/environment/volumetric_fog/volume_depth", 192))
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--city-capture="): capture_path = arg.trim_prefix("--city-capture=")
		if arg == "--city-full-fog": balanced_fog = false
	camera = $Camera3D
	sun = $Sun
	_initial_sun_rotation = sun.rotation
	for instance in $CityBlocks.find_children("*", "MeshInstance3D", true, false):
		if instance.mesh != null: _city_bounds = _city_bounds.merge(instance.global_transform * instance.mesh.get_aabb())
	# The saved volume gives the editor the same atmosphere as the running scene.
	$EditorFog.free()
	atmosphere = TGAtmosphere.new()
	atmosphere.name = "Atmosphere"
	atmosphere.thickness = FOG_THICKNESS
	atmosphere.distance_scale = FOG_SCALE
	atmosphere.fog_length = FOG_LENGTH
	add_child(atmosphere)
	atmosphere.setup($WorldEnvironment.environment, sun)
	get_viewport().use_taa = true
	get_viewport().msaa_3d = Viewport.MSAA_2X
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	controls = FreeFlyCamera.new()
	controls.camera = camera
	controls.speed = 22.0
	add_child(controls)
	controls.enabled = capture_path.is_empty()
	_set_view(0)
	_build_ui()
	DemoViewer.configure(self, "City | atmosphere", atmosphere)
	print("CITY_READY: ", get_meta("building_count"), " buildings / 464 x 464 m / Kenney CC0")
	if not capture_path.is_empty(): await _capture()

func _set_view(index: int) -> void:
	preset = index
	camera.position = VIEW_POSITIONS[index]
	camera.look_at(VIEW_TARGETS[index])
	controls.speed = 22.0 if index == 0 else 10.0
	if atmosphere != null: _apply_fog_profile()
	if status != null: _update_status()

func _apply_fog_profile() -> void:
	_update_fog_coverage(true)
	var grid := BALANCED_FOG_GRID if balanced_fog else _project_fog_grid
	RenderingServer.environment_set_volumetric_fog_volume_size(grid.x, grid.y)
	atmosphere.apply()

func _required_fog_length() -> float:
	# The fog is anchored in world space. Its camera volume must reach the
	# city even after flying outside it; a fixed near range misses it entirely.
	var farthest := 0.0
	for corner in 8:
		farthest = maxf(farthest, -camera.global_basis.z.dot(_city_bounds.get_endpoint(corner) - camera.global_position))
	return minf(camera.far, maxf(FOG_LENGTH, ceilf((farthest + FOG_RANGE_STEP) / FOG_RANGE_STEP) * FOG_RANGE_STEP))

func _update_fog_coverage(force := false) -> void:
	var required := _required_fog_length()
	# Expand before any city surface leaves the volume. Padding and hysteresis
	# prevent tiny camera movements from repeatedly redistributing the grid.
	if not force and required <= atmosphere.fog_length and atmosphere.fog_length - required < FOG_RANGE_STEP * 2.0: return
	if is_equal_approx(required, atmosphere.fog_length): return
	atmosphere.fog_length = required
	atmosphere.environment.volumetric_fog_length = required
	atmosphere.invalidate_history()
	if status != null: _update_status()

func _exit_tree() -> void:
	# The froxel resolution is global; restore it before entering another demo.
	RenderingServer.environment_set_volumetric_fog_volume_size(_project_fog_grid.x, _project_fog_grid.y)

func _build_ui() -> void:
	hud = CanvasLayer.new()
	add_child(hud)
	var panel := PanelContainer.new()
	panel.position = Vector2(18, 18)
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	hud.add_child(panel)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.025, 0.035, 0.055, 0.88)
	style.set_content_margin_all(12)
	style.set_corner_radius_all(6)
	panel.add_theme_stylebox_override("panel", style)
	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 8)
	panel.add_child(content)
	status = Label.new()
	status.add_theme_font_size_override("font_size", 16)
	content.add_child(status)
	_update_status()

func _update_status() -> void:
	status.text = "CITY ATMOSPHERE  /  %s\n464 x 464 m   %d buildings   Kenney CC0\nP / 1-3 views   F fog %s   [ / ] density %.2f   Arrows sun\nR reset   RMB + WASD fly   E / Space up   Q down   Wheel speed   Shift fast   H hide" % [VIEW_NAMES[preset], get_meta("building_count"), "ON" if atmosphere.active else "OFF", atmosphere.thickness]
	status.text += "\nV fog quality: %s | coverage %.0f m" % ["BALANCED" if balanced_fog else "HIGH", atmosphere.fog_length]

func _process(delta: float) -> void:
	if atmosphere == null: return
	_update_fog_coverage()
	if not capture_path.is_empty(): return
	var azimuth := float(Input.is_physical_key_pressed(KEY_RIGHT)) - float(Input.is_physical_key_pressed(KEY_LEFT))
	var elevation := float(Input.is_physical_key_pressed(KEY_UP)) - float(Input.is_physical_key_pressed(KEY_DOWN))
	if azimuth != 0 or elevation != 0:
		sun.rotation.y += azimuth * delta * 0.4
		sun.rotation.x = clampf(sun.rotation.x + elevation * delta * 0.3, -1.4, -0.1)

func _unhandled_input(event: InputEvent) -> void:
	if not capture_path.is_empty() or not event is InputEventKey or not event.pressed or event.echo: return
	match event.physical_keycode:
		KEY_P: _set_view((preset + 1) % VIEW_NAMES.size())
		KEY_1, KEY_2, KEY_3: _set_view(event.physical_keycode - KEY_1)
		KEY_F: atmosphere.active = not atmosphere.active
		KEY_BRACKETLEFT: atmosphere.thickness = maxf(0.0, atmosphere.thickness - 0.1)
		KEY_BRACKETRIGHT: atmosphere.thickness = minf(3.0, atmosphere.thickness + 0.1)
		KEY_L: sun.visible = not sun.visible
		KEY_G: atmosphere.anisotropy = 0.0 if atmosphere.anisotropy > 0.0 else 0.65
		KEY_V:
			balanced_fog = not balanced_fog
			_apply_fog_profile()
		KEY_R:
			_set_view(0)
			sun.rotation = _initial_sun_rotation
			sun.visible = true
			atmosphere.active = true
			atmosphere.thickness = FOG_THICKNESS
			atmosphere.anisotropy = 0.65
		KEY_H: hud.visible = not hud.visible
		KEY_ESCAPE: get_tree().quit()
		_: return
	atmosphere.apply()
	_update_status()

func _save(label: String) -> void:
	for frame in 90: await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var result := get_viewport().get_texture().get_image().save_png(capture_path.path_join(label + ".png"))
	if result != OK:
		push_error("Could not write city capture: " + label)
		get_tree().quit(1)

func _capture() -> void:
	if RenderingServer.get_rendering_device() == null:
		push_error("City captures require a real GPU rendering device.")
		get_tree().quit(2)
		return
	DirAccess.make_dir_recursive_absolute(capture_path)
	hud.visible = false
	await _save("01_skyline")
	_set_view(1)
	await _save("02_avenue")
	atmosphere.active = false
	atmosphere.apply()
	await _save("03_avenue_clear")
	atmosphere.active = true
	atmosphere.apply()
	_set_view(2)
	await _save("04_square")
	_set_view(0)
	hud.visible = true
	DemoViewer.force_visible = true
	_update_status()
	await _save("05_controls")
	var result := {"gpu": RenderingServer.get_video_adapter_name(), "buildings": get_meta("building_count"), "city_width_m": 464, "viewport_gpu_ms": RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid()), "fog_reprojection": atmosphere.environment.volumetric_fog_temporal_reprojection_enabled, "timing_hud": DemoViewer.timing.text}
	FileAccess.open(capture_path.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("CITY_CAPTURE: ", JSON.stringify(result))
	get_tree().quit()

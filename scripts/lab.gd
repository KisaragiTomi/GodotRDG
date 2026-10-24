extends Node3D

const LayoutScript = preload("res://scripts/surface_layout.gd")
const CacheScript = preload("res://scripts/surface_cache.gd")
const SURFACE_SHADER = preload("res://shaders/surface.gdshader")
const MODE_NAMES := ["DIRECT / no GI", "SURFACE CACHE / two diffuse bounces", "INDIRECT ONLY", "VOXEL GI / native baseline", "LIGHTMAP GI / baked reference"]

var layout: SurfaceLayout
var cache: SurfaceCache
var geometry: Node3D
var camera: Camera3D
var camera_controls: FreeFlyCamera
var lamps: Array[OmniLight3D] = []
var meshes: Array[MeshInstance3D] = []
var native: Node
var mode := 1
var door_closed := false
var animate_light := false
var light_time := 0.0
var update_clock := 0.0
var _status: Label
var _hint: Label
var _busy := true
var _report: Dictionary = {}
var _args: Dictionary = {}
var _atlas_image: Image
var _last_light_data := PackedFloat32Array()
var _reference_light_data := PackedFloat32Array()
var _rays := 256
var emissive_demo := false
var emitters_enabled := true
var emission_strength := 7.5
var _emission_dirty := false
var _environment: Environment
var ball_demo := false
var ball: EmissiveBall
var _last_sphere_data := PackedFloat32Array()

func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		var parts := arg.trim_prefix("--").split("=", true, 1)
		_args[parts[0]] = parts[1] if parts.size() > 1 else "true"
	_rays = clampi(int(_args.get("rays", 256)), 16, 512)
	ball_demo = _args.has("ball") or _args.has("ball-capture")
	emissive_demo = ball_demo or _args.has("emissive") or _args.has("emissive-capture")
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	_create_environment()
	_create_camera()
	_create_lights()
	_reference_light_data = _light_data()
	_create_ui()
	await _build_scene()
	if _args.has("ball-capture"):
		await _capture_ball(str(_args["ball-capture"]))
		return
	if _args.has("emissive-capture"):
		await _capture_emissive(str(_args["emissive-capture"]))
		return
	if _args.has("export-native-scene"):
		await _export_native_scene(str(_args["export-native-scene"]))
		get_tree().quit()
		return
	if _args.has("validate"):
		await _validate()
	elif _args.has("capture"):
		await _capture_all(str(_args.capture))
		get_tree().quit()

func _create_environment() -> void:
	var world := WorldEnvironment.new()
	world.name = "WorldEnvironment"
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.028, 0.032, 0.042)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color.BLACK
	env.ambient_light_energy = 0.0
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.glow_enabled = false
	env.ssao_enabled = false
	env.ssil_enabled = false
	env.fog_enabled = false
	world.environment = env
	_environment = env
	add_child(world)

func _create_camera() -> void:
	camera = Camera3D.new()
	camera.name = "InspectionCamera"
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 21.0
	camera.near = 0.1
	camera.far = 80.0
	add_child(camera)
	camera.position = Vector3(13.5, 19.5, 18.0)
	camera.look_at(Vector3(0, 0, 0))
	camera.current = true
	camera_controls = FreeFlyCamera.new()
	camera_controls.camera = camera
	add_child(camera_controls)

func _create_lights() -> void:
	for config in [[Vector3(-4.0, 2.5, -2.0), Color(1.0, 0.93, 0.82), 10.0, 10.0], [Vector3(4.5, 2.3, 1.5), Color(0.85, 0.93, 1.0), 8.0, 9.0]]:
		var light := OmniLight3D.new()
		light.name = "Lamp%d" % lamps.size()
		light.position = config[0]
		light.light_color = config[1]
		light.light_energy = config[2]
		light.omni_range = config[3]
		light.omni_attenuation = 1.5
		light.light_bake_mode = Light3D.BAKE_DYNAMIC
		light.shadow_enabled = true
		light.shadow_bias = 0.02
		light.shadow_normal_bias = 0.3
		light.visible = not emissive_demo
		add_child(light)
		lamps.append(light)

func _boxes() -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	result.append(_box("Ground", Vector3(0, -0.25, 0), Vector3(16, 0.5, 12), Color(0.62, 0.62, 0.6)))
	result.append(_box("RedWall", Vector3(-8, 1.5, 0), Vector3(0.3, 3, 12), Color(0.68, 0.22, 0.15)))
	result.append(_box("BackWall", Vector3(0, 1.5, -6), Vector3(16, 3, 0.3), Color(0.65, 0.65, 0.63)))
	result.append(_box("BlueWall", Vector3(8, 1.5, 0), Vector3(0.3, 3, 12), Color(0.18, 0.44, 0.62)))
	result.append(_box("ThinPartitionA", Vector3(0, 1.5, -3.6), Vector3(0.18, 3, 4.8), Color(0.58, 0.58, 0.56)))
	result.append(_box("ThinPartitionB", Vector3(0, 1.5, 3.6), Vector3(0.18, 3, 4.8), Color(0.58, 0.58, 0.56)))
	result.append(_box("WhiteReceiver", Vector3(-6, 0.65, 2.5), Vector3(1.3, 1.3, 1.3), Color(0.88, 0.88, 0.88)))
	result.append(_box("GrayReceiver", Vector3(-4, 0.65, 2.5), Vector3(1.3, 1.3, 1.3), Color(0.5, 0.5, 0.5)))
	result.append(_box("BlackReceiver", Vector3(-2, 0.65, 2.5), Vector3(1.3, 1.3, 1.3), Color(0.035, 0.035, 0.035)))
	for step in 4:
		var height := float(step + 1) * 0.35
		result.append(_box("Step%d" % step, Vector3(4.5, height * 0.5, -1.0 - step * 0.8), Vector3(2.5, height, 0.8), Color(0.62, 0.6, 0.54)))
	# A bridge is genuine stacked geometry, with an open region beneath it.
	result.append(_box("Bridge", Vector3(3.6, 1.9, 3.5), Vector3(3.4, 0.25, 1.3), Color(0.65, 0.53, 0.35)))
	result.append(_box("BridgePierA", Vector3(2.1, 0.9, 3.5), Vector3(0.3, 1.8, 1.3), Color(0.5, 0.5, 0.48)))
	result.append(_box("BridgePierB", Vector3(5.1, 0.9, 3.5), Vector3(0.3, 1.8, 1.3), Color(0.5, 0.5, 0.48)))
	if ball_demo:
		# Neutral walls make the moving source's color and corner response clear.
		for index in [1, 2, 3]:
			result[index]["color"] = Color(0.65, 0.65, 0.65)
	if emissive_demo and not ball_demo:
		var panel := _box("WarmEmitter", Vector3(-4.5, 1.2, -5.66), Vector3(2.6, 1.8, 0.16), Color(0.16, 0.16, 0.16))
		panel["emission_color"] = Color(1.0, 0.48, 0.15)
		panel["emission_energy"] = emission_strength if emitters_enabled else 0.0
		result.append(panel)
		var cube := _box("CoolEmitter", Vector3(5.8, 0.65, 0.35), Vector3(1.15, 1.3, 1.15), Color(0.16, 0.16, 0.16))
		cube["emission_color"] = Color(0.12, 0.65, 1.0)
		cube["emission_energy"] = emission_strength if emitters_enabled else 0.0
		result.append(cube)
	if door_closed:
		result.append(_box("Door", Vector3(0, 1.5, 0), Vector3(0.18, 3, 2.4), Color(0.5, 0.4, 0.28)))
	return result

func _box(label: String, center: Vector3, size: Vector3, color: Color) -> Dictionary:
	return {"name": label, "center": center, "size": size, "color": color}

func _build_scene() -> void:
	_busy = true
	if is_instance_valid(geometry):
		for light in lamps:
			light.reparent(self)
		geometry.queue_free()
		await get_tree().process_frame
	if cache != null:
		cache.shutdown()
	layout = LayoutScript.new()
	layout.refine_emitter_receivers = not _args.has("coarse-emitter-receivers")
	layout.build(_boxes(), clampf(float(_args.get("floor-spacing", 0.25)), 0.05, 2.0), clampf(float(_args.get("wall-spacing", 0.25 if ball_demo else 0.5)), 0.05, 2.0))
	cache = CacheScript.new()
	cache.emitter_sampling = not _args.has("legacy-emitter-sampling")
	cache.failed.connect(_on_cache_failed)
	geometry = Node3D.new()
	geometry.name = "Geometry"
	add_child(geometry)
	for light in lamps:
		light.reparent(geometry)
	meshes.clear()
	for i in layout.boxes.size():
		var mesh := MeshInstance3D.new()
		mesh.name = layout.boxes[i].name
		mesh.mesh = layout.make_mesh(i, SURFACE_SHADER, cache.texture)
		mesh.position = layout.boxes[i].center
		mesh.gi_mode = GeometryInstance3D.GI_MODE_STATIC
		geometry.add_child(mesh)
		meshes.append(mesh)
	if ResourceLoader.exists("res://scripts/native_baselines.gd"):
		native = load("res://scripts/native_baselines.gd").new()
		geometry.add_child(native)
		native.setup(geometry, AABB(Vector3(-8.5, -0.6, -6.5), Vector3(17, 4.0, 13)), lamps)
	_last_light_data = _light_data()
	cache.initialize(layout, _last_light_data, _rays)
	await cache.ready_for_use
	if ball_demo:
		if not is_instance_valid(ball):
			ball = EmissiveBall.new()
			ball.name = "ControlledEmitter"
			add_child(ball)
		if not ball.can_place(ball.position, layout.boxes):
			ball.position = EmissiveBall.START
		_last_sphere_data = ball.source_data()
		cache.update_sphere(_last_sphere_data, _last_light_data)
		await cache.update_complete
	_emission_dirty = false
	_set_mode(mode if mode < 3 else 1)
	_busy = false
	print("TOPDOWN_GI_READY ", JSON.stringify(cache.stats))

func _light_data() -> PackedFloat32Array:
	var data := PackedFloat32Array()
	for light in lamps:
		var color := light.light_color.srgb_to_linear() * light.light_energy
		if not light.visible:
			color = Color.BLACK
		data.append_array(PackedFloat32Array([light.position.x, light.position.y, light.position.z, light.omni_range, color.r, color.g, color.b, light.omni_attenuation, 0, 0, 0, 0]))
	return data

func _create_ui() -> void:
	var canvas := CanvasLayer.new()
	canvas.name = "HUD"
	add_child(canvas)
	var panel := PanelContainer.new()
	panel.position = Vector2(18, 18)
	canvas.add_child(panel)
	var stack := VBoxContainer.new()
	panel.add_child(stack)
	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 20)
	stack.add_child(_status)
	_hint = Label.new()
	_hint.text = "1 Direct  2 Surface GI  3 Indirect  4 VoxelGI  5 LightmapGI\nL Move lamp  O Door  P Top-down preset / projection\nHold RMB Look  WASD Fly  Space/Ctrl Up/Down  Shift Fast  Wheel Dolly"
	_hint.add_theme_font_size_override("font_size", 14)
	if emissive_demo:
		_hint.text = "1 Source only  2 GI  3 Indirect  E Emitters  G Glow  + / - Strength\nHold RMB Look  WASD Fly  Space/Ctrl Up/Down  Shift Fast  Wheel Dolly\nP Top-down preset  Esc Release/Close | Point lights OFF"
	if ball_demo:
		_hint.text = "Arrows Move ball  Q/Z Ball height  R Reset ball  C Color  E Light\n+ / - Brightness  1 / 2 / 3 Compare  G Glow  P Top-down preset\nHold RMB Look  WASD Fly  Space/Ctrl Up/Down  Shift Fast  Esc Release/Close"
	stack.add_child(_hint)
	var footer := Label.new()
	footer.position = Vector2(18, 740)
	footer.text = "COLOR BOUNCE          THIN WALL / DOOR          BLACK / GRAY / WHITE          STAIRS / BRIDGE"
	if emissive_demo:
		footer.text = "WARM EMISSIVE PANEL                     COOL EMISSIVE BLOCK                     SURFACE COLOR BOUNCE"
	if ball_demo:
		footer.text = "MOVE THE GLOWING BALL THROUGH THE DOORWAY | NEUTRAL WALLS | LIVE LIGHT BOUNCE"
		DisplayServer.window_set_title("Godot Sparse GI - Movable Emissive Ball")
	footer.add_theme_font_size_override("font_size", 14)
	canvas.add_child(footer)

func _process(delta: float) -> void:
	if camera_controls != null:
		camera_controls.enabled = not _busy and not _args.has("validate") and not _args.has("capture") and not _args.has("ball-capture") and not _args.has("emissive-capture") and not _args.has("export-native-scene")
	if cache == null:
		return
	cache.poll_gpu_timing()
	_status.text = "%s%s\n%d surface samples | %d rays | %d bounces | %d lighting / %d transport updates" % [MODE_NAMES[mode], "  [building]" if _busy else "", int(cache.stats.surfel_count), _rays, cache.bounces, int(cache.stats.lighting_updates), int(cache.stats.transport_builds)]
	if emissive_demo:
		var view := "SELF-LIT SURFACES" if mode == 0 else ("INDIRECT ONLY" if mode == 2 else "EMISSIVE + GI")
		_status.text = "%s | strength %.2f | emitters %s | glow %s" % [view, emission_strength, "ON" if emitters_enabled else "OFF", "ON" if _environment.glow_enabled else "OFF"]
	if ball_demo and is_instance_valid(ball):
		_status.text = "MOVABLE EMISSIVE BALL | %s | %.1f | %s | height %.2f m" % [EmissiveBall.COLOR_NAMES[ball.color_index], ball.energy, "ON" if ball.enabled else "OFF", ball.position.y]
	if _busy:
		return
	if ball_demo and not _args.has("ball-capture"):
		ball.move_with_input(delta, camera, layout.boxes)
	if animate_light:
		light_time += delta
		lamps[0].position = Vector3(-4 + sin(light_time * 0.7) * 2.2, 2.5, -2 + cos(light_time * 0.7) * 1.5)
	if mode == 4 and _light_data() != _reference_light_data:
		_set_mode(1)
	update_clock += delta
	if update_clock >= 1.0 / 30.0:
		var data := _light_data()
		if ball_demo:
			var source := ball.source_data()
			if (source != _last_sphere_data or data != _last_light_data) and cache.update_sphere(source, data):
				_last_sphere_data = source
				_last_light_data = data
		elif _emission_dirty:
			if cache.update_materials(layout.face_data, data):
				_emission_dirty = false
				_last_light_data = data
		elif data != _last_light_data and cache.update_lights(data):
			_last_light_data = data
		update_clock = 0.0

func _unhandled_input(event: InputEvent) -> void:
	if _busy:
		return
	if event is InputEventKey and event.pressed and not event.echo:
		match event.physical_keycode:
			KEY_1: _set_mode(0)
			KEY_2: _set_mode(1)
			KEY_3: _set_mode(2)
			KEY_4:
				if not ball_demo: _enable_voxel()
			KEY_5:
				if not ball_demo: _enable_lightmap()
			KEY_R:
				if ball_demo: ball.position = EmissiveBall.START
			KEY_C:
				if ball_demo:
					ball.color_index = (ball.color_index + 1) % EmissiveBall.COLORS.size()
					ball.refresh_visual()
			KEY_L:
				if not emissive_demo: animate_light = not animate_light
			KEY_E:
				if ball_demo:
					ball.enabled = not ball.enabled
					ball.refresh_visual()
				elif emissive_demo:
					emitters_enabled = not emitters_enabled
					_refresh_emission()
			KEY_G:
				if emissive_demo: _environment.glow_enabled = not _environment.glow_enabled
			KEY_EQUAL, KEY_PLUS, KEY_KP_ADD:
				if ball_demo:
					ball.energy = minf(96.0, ball.energy * 1.25)
					ball.refresh_visual()
				elif emissive_demo:
					emission_strength = minf(30.0, emission_strength * 1.25)
					_refresh_emission()
			KEY_MINUS, KEY_KP_SUBTRACT:
				if ball_demo:
					ball.energy = maxf(1.0, ball.energy / 1.25)
					ball.refresh_visual()
				elif emissive_demo:
					emission_strength = maxf(0.25, emission_strength / 1.25)
					_refresh_emission()
			KEY_O:
				door_closed = not door_closed
				_build_scene()
			KEY_P:
				camera_controls.release_mouse()
				camera.position = Vector3(13.5, 19.5, 18.0)
				camera.look_at(Vector3.ZERO)
				camera.size = 21.0
				camera.projection = Camera3D.PROJECTION_PERSPECTIVE if camera.projection == Camera3D.PROJECTION_ORTHOGONAL else Camera3D.PROJECTION_ORTHOGONAL
			KEY_ESCAPE: get_tree().quit()

func _set_mode(value: int) -> void:
	mode = value
	if ball_demo and is_instance_valid(ball):
		ball.visual.visible = mode != 2
	if is_instance_valid(native):
		native.select_mode("voxel" if mode == 3 else ("lightmap" if mode == 4 else "direct"))
	for instance in meshes:
		for side in instance.mesh.get_surface_count():
			var material := instance.mesh.surface_get_material(side) as ShaderMaterial
			material.set_shader_parameter("gi_mode", 3 if mode >= 3 else mode)

func _enable_voxel() -> void:
	if not is_instance_valid(native):
		return
	_busy = true
	# Godot's material baker must see the base albedo, without custom indirect output.
	_set_mode(0)
	var result: Dictionary = await native.bake_voxel()
	print("VOXEL_BASELINE ", JSON.stringify(result))
	_set_mode(3 if result.get("ok", false) else 1)
	_busy = false

func _enable_lightmap() -> void:
	if emissive_demo:
		_hint.text = "The saved lightmap is for the point-light scene. Use 1 / 2 / 3 to compare emission and GI."
		return
	if door_closed or animate_light or _light_data() != _reference_light_data:
		_hint.text = "Lightmap reference requires the original static scene: open door and stationary lights."
		return
	# Reuse the exact unwrapped mesh resources that were actually baked.
	if ResourceLoader.exists("res://generated/native_reference.tscn"):
		var reference := (load("res://generated/native_reference.tscn") as PackedScene).instantiate()
		for instance in meshes:
			var baked_mesh := reference.get_node_or_null("Geometry/" + str(instance.name)) as MeshInstance3D
			if baked_mesh == null:
				continue
			var replacement := baked_mesh.mesh.duplicate() as ArrayMesh
			for side in replacement.get_surface_count():
				replacement.surface_set_material(side, instance.mesh.surface_get_material(side))
			instance.mesh = replacement
		reference.free()
	if is_instance_valid(native) and native.load_lightmap("res://generated/native_reference.lmbake"):
		_set_mode(4)
	else:
		_hint.text = "Lightmap reference is not baked. Run tools/validate.ps1 -BakeLightmap.\n" + _hint.text

func _wait_frames(count: int = 8) -> void:
	for i in count:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw

func _capture(path: String) -> void:
	await _wait_frames()
	var image := get_viewport().get_texture().get_image()
	var error := image.save_png(path)
	if error != OK:
		push_error("Cannot save capture: " + path)
	print("CAPTURE ", path)

func _capture_all(folder: String) -> void:
	DirAccess.make_dir_recursive_absolute(folder)
	for value in [0, 1, 2]:
		_set_mode(value)
		await _capture(folder.path_join(["direct.png", "topdown.png", "indirect.png"][value]))
	_set_mode(1)

func _read_atlas() -> Image:
	_atlas_image = null
	cache.read_atlas(func(image: Image): _atlas_image = image)
	while _atlas_image == null:
		await get_tree().process_frame
	return _atlas_image

func _refresh_emission() -> void:
	# Changing radiance reuses geometric transport. No ray recast or mesh rebuild.
	if mode >= 3:
		_set_mode(1)
	for index in layout.boxes.size():
		var box: Dictionary = layout.boxes[index]
		if not box.has("emission_color"):
			continue
		var energy := emission_strength if emitters_enabled else 0.0
		layout.set_emission(index, box.emission_color, energy)
		for side in meshes[index].mesh.get_surface_count():
			var material := meshes[index].mesh.surface_get_material(side) as ShaderMaterial
			material.set_shader_parameter("emission_energy", energy)
	_emission_dirty = true

func _wait_emission() -> void:
	while _emission_dirty:
		await get_tree().process_frame
	await _wait_frames()

func _wait_ball() -> void:
	while ball.source_data() != _last_sphere_data:
		await get_tree().process_frame
	await _wait_frames()

func _ball_key(code: Key) -> void:
	var event := InputEventKey.new()
	event.physical_keycode = code
	event.pressed = true
	_unhandled_input(event)

func _capture_ball(folder: String) -> void:
	DirAccess.make_dir_recursive_absolute(folder)
	await _wait_ball()
	var initial := await _read_atlas()
	var result := {"point_lights_disabled": lamps.all(func(light): return not light.visible), "sphere_lights_room": _image_energy(initial) > 0.001}
	await _capture(folder.path_join("ball_start.png"))
	var screen := get_viewport().get_texture().get_image()
	var ball_pixel := screen.get_pixelv(Vector2i(camera.unproject_position(ball.global_position)))
	result["ball_visibly_emissive"] = maxf(ball_pixel.r, maxf(ball_pixel.g, ball_pixel.b)) > 0.8
	var camera_before := camera.transform
	Input.action_press("ball_right")
	for index in 6:
		ball.move_with_input(0.1, camera, layout.boxes)
	Input.action_release("ball_right")
	await _wait_ball()
	var moved := await _read_atlas()
	result["arrow_input_moves_ball"] = ball.position.distance_to(EmissiveBall.START) > 1.0
	result["ball_input_preserves_camera"] = camera.transform == camera_before
	result["motion_changes_light"] = moved.get_data() != initial.get_data() and _finite_image(moved)
	await _capture(folder.path_join("ball_moved.png"))
	ball.position = Vector3(-1.0, 0.9, -2.0)
	ball.move_by(Vector3(3, 0, 0), layout.boxes)
	result["thin_partition_blocks_ball"] = ball.position.x < -0.39 and ball.can_place(ball.position, layout.boxes)
	await _wait_ball()
	await _capture(folder.path_join("ball_at_wall.png"))
	ball.position = Vector3(-1.0, 0.9, 0.0)
	ball.move_by(Vector3(2, 0, 0), layout.boxes)
	result["doorway_is_traversable"] = ball.position.x > 0.9
	Input.action_press("ball_raise")
	for index in 6:
		ball.move_with_input(0.1, camera, layout.boxes)
	Input.action_release("ball_raise")
	result["height_input_works"] = ball.position.y > 2.0 and ball.can_place(ball.position, layout.boxes)
	_ball_key(KEY_C)
	await _wait_ball()
	var blue := CacheValidation.summarize(layout, await _read_atlas())
	result["color_changes_illumination"] = blue.rgb_mean[2] > blue.rgb_mean[0] * 2.0
	await _capture(folder.path_join("ball_blue.png"))
	_ball_key(KEY_E)
	await _wait_ball()
	result["switch_off_clears_light"] = _image_energy(await _read_atlas()) == 0.0
	_ball_key(KEY_E)
	_ball_key(KEY_R)
	ball.color_index = 0
	ball.refresh_visual()
	await _wait_ball()
	result["deterministic_restore"] = initial.get_data() == (await _read_atlas()).get_data()
	var updates := int(cache.stats.lighting_updates)
	await _wait_frames(30)
	result["idle_does_not_dispatch"] = int(cache.stats.lighting_updates) == updates
	result["motion_reuses_transport"] = int(cache.stats.transport_builds) == 1 and int(cache.stats.emitter_transfer_builds) == 0
	result["gpu"] = cache.stats.duplicate(true)
	result["passed"] = true
	for key in result:
		if result[key] is bool and not result[key]: result.passed = false
	FileAccess.open(folder.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("BALL_TEST ", JSON.stringify(result))
	get_tree().quit(0 if result.passed else 1)

func _capture_emissive(folder: String) -> void:
	DirAccess.make_dir_recursive_absolute(folder)
	_set_mode(0)
	await _capture(folder.path_join("source_only.png"))
	_set_mode(1)
	await _capture(folder.path_join("emissive_gi.png"))
	# Fixed close-up exposes chart reconstruction artifacts hidden at overview scale.
	var overview_transform := camera.transform
	var overview_size := camera.size
	camera.position += Vector3(-4.5, 1.1, -5.65)
	camera.size = 7.0
	get_node("HUD").visible = false
	await _capture(folder.path_join("emissive_detail.png"))
	camera.transform = overview_transform
	camera.size = overview_size
	get_node("HUD").visible = true
	var lit := await _read_atlas()
	var lit_energy := _image_energy(lit)
	var result := {"point_lights_disabled": lamps.all(func(light): return not light.visible), "emission_lights_receivers": lit_energy > 0.001, "finite_nonnegative": _finite_image(lit)}
	result["lit_atlas"] = CacheValidation.summarize(layout, lit)
	result["gpu"] = cache.stats.duplicate(true)
	lit.save_exr(folder.path_join("indirect_atlas.exr"))
	_set_mode(2)
	await _capture(folder.path_join("indirect_only.png"))
	_set_mode(1)
	_environment.glow_enabled = true
	await _capture(folder.path_join("emissive_glow.png"))
	result["glow_is_only_postprocess"] = lit.get_data() == (await _read_atlas()).get_data()
	_environment.glow_enabled = false
	emission_strength *= 0.5
	_refresh_emission()
	await _wait_emission()
	var half := await _read_atlas()
	var half_ratio := _image_energy(half) / lit_energy
	result["half_intensity_ratio"] = half_ratio
	result["linear_emission_response"] = absf(half_ratio - 0.5) < 0.005
	emitters_enabled = false
	_refresh_emission()
	await _wait_emission()
	var dark := await _read_atlas()
	result["emitters_off_no_history"] = _image_energy(dark) == 0.0
	await _capture(folder.path_join("emitters_off.png"))
	emission_strength *= 2.0
	emitters_enabled = true
	_refresh_emission()
	await _wait_emission()
	result["deterministic_restore"] = lit.get_data() == (await _read_atlas()).get_data()
	result["emission_reuses_transport"] = int(cache.stats.transport_builds) == 1
	result["emission_reuses_area_visibility"] = int(cache.stats.emitter_transfer_builds) == (1 if cache.emitter_sampling else 0)
	result["passed"] = true
	for key in result:
		if result[key] is bool and not result[key]: result.passed = false
	FileAccess.open(folder.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("EMISSIVE_TEST ", JSON.stringify({"passed": result.passed, "half_ratio": half_ratio, "transport_builds": cache.stats.transport_builds}))
	get_tree().quit(0 if result.passed else 1)

func _validate() -> void:
	var folder: String = _args.get("output", ProjectSettings.globalize_path("res://artifacts/validation"))
	DirAccess.make_dir_recursive_absolute(folder)
	await _capture_all(folder)
	var initial := await _read_atlas()
	initial.save_exr(folder.path_join("indirect_atlas.exr"))
	_report["initial"] = cache.stats.duplicate(true)
	_report["atlas"] = CacheValidation.summarize(layout, initial)
	_report["atlas_finite_nonnegative"] = _finite_image(initial)
	_report["atlas_has_energy"] = _image_energy(initial) > 0.001
	var before := int(cache.stats.lighting_updates)
	var original_camera := camera.transform
	camera.position += Vector3(2, 0, -2)
	camera.size = 16
	await _capture(folder.path_join("pan_zoom.png"))
	camera.projection = Camera3D.PROJECTION_PERSPECTIVE
	await _capture(folder.path_join("perspective_topdown.png"))
	var after_camera := await _read_atlas()
	_report["camera_does_not_dispatch"] = int(cache.stats.lighting_updates) == before
	_report["camera_invariant_atlas"] = initial.get_data() == after_camera.get_data()
	_report["camera_comparison"] = CacheValidation.compare(layout, initial, after_camera)
	camera.transform = original_camera
	camera.position.x += 10.0
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 10.0
	await _capture(folder.path_join("offscreen_light.png"))
	_report["source_is_offscreen"] = not camera.is_position_in_frustum(lamps[0].global_position)
	var offscreen := await _read_atlas()
	_report["offscreen_light_preserves_gi"] = initial.get_data() == offscreen.get_data()
	camera.transform = original_camera
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 21
	var original_position := lamps[0].position
	lamps[0].position.x -= 2.0
	await _wait_frames(20)
	var moved := await _read_atlas()
	_report["moving_light_updates_gi"] = moved.get_data() != initial.get_data()
	_report["moving_light_comparison"] = CacheValidation.compare(layout, initial, moved)
	_report["moving_light_reuses_transport"] = int(cache.stats.transport_builds) == 1
	await _capture(folder.path_join("moved_light.png"))
	for light in lamps:
		light.visible = false
	await _wait_frames(20)
	var dark := await _read_atlas()
	_report["lights_off_no_history"] = _image_energy(dark) < 0.000001
	await _capture(folder.path_join("lights_off.png"))
	for light in lamps:
		light.visible = true
	lamps[0].position = original_position
	await _wait_frames(20)
	var restored := await _read_atlas()
	_report["deterministic_restore"] = initial.get_data() == restored.get_data()
	if _args.has("voxel"):
		await _enable_voxel()
		await _capture(folder.path_join("voxel.png"))
		_report["native"] = native.status()
		_set_mode(1)
	if _args.has("lightmap"):
		_enable_lightmap()
		await _capture(folder.path_join("lightmap.png"))
		_report["lightmap_loaded"] = mode == 4
		_set_mode(1)
	# Fixed-length warm dynamic update sample, with camera and resolution held fixed.
	var gpu_frames: Array[float] = []
	var cpu_frames: Array[float] = []
	for frame in 90:
		lamps[0].position.x = original_position.x + sin(float(frame) * 0.1)
		cache.update_lights(_light_data())
		await _wait_frames(1)
		if frame >= 10:
			gpu_frames.append(RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid()))
			# Performance.TIME_PROCESS refreshes too slowly for this short burst and
			# can still report the preceding bake/readback stall. Record each frame.
			cpu_frames.append(get_process_delta_time() * 1000.0)
	_report["dynamic_gpu"] = cache.stats.duplicate(true)
	_report["viewport_gpu_samples_ms"] = gpu_frames
	_report["frame_cpu_samples_ms"] = cpu_frames
	_report["timing_scope"] = "Synthetic 90-frame moving-light burst; compute GPU excludes material rendering. Frame delta includes harness/OS overhead; not a product frame-budget guarantee."
	lamps[0].position = original_position
	await _wait_frames(20)
	door_closed = true
	await _build_scene()
	await _capture(folder.path_join("door_closed.png"))
	_report["door_rebuilt_transport"] = layout.boxes.back().name == "Door" and int(cache.stats.transport_builds) == 1
	_report["device"] = RenderingServer.get_video_adapter_name()
	_report["resolution"] = [get_viewport().size.x, get_viewport().size.y]
	_report["final"] = cache.stats.duplicate(true)
	_report["passed"] = true
	for key in _report:
		if _report[key] is bool and not _report[key]:
			_report.passed = false
	var file := FileAccess.open(folder.path_join("results.json"), FileAccess.WRITE)
	file.store_string(JSON.stringify(_report, "\t"))
	print("TOPDOWN_VALIDATION ", JSON.stringify({"passed": _report.passed, "rays": _rays, "report": folder.path_join("results.json")}))
	get_tree().quit(0 if _report.passed else 1)

func _finite_image(image: Image) -> bool:
	for y in image.get_height():
		for x in image.get_width():
			var c := image.get_pixel(x, y)
			if not is_finite(c.r) or not is_finite(c.g) or not is_finite(c.b) or minf(c.r, minf(c.g, c.b)) < 0.0:
				return false
	return true

func _image_energy(image: Image) -> float:
	var total := 0.0
	for y in image.get_height():
		for x in image.get_width():
			var c := image.get_pixel(x, y)
			total += c.r + c.g + c.b
	return total

func _export_native_scene(path: String) -> void:
	_set_mode(0)
	for instance in meshes:
		for side in instance.mesh.get_surface_count():
			var material := StandardMaterial3D.new()
			material.albedo_color = layout.boxes[meshes.find(instance)].color
			var box: Dictionary = layout.boxes[meshes.find(instance)]
			material.emission_enabled = float(box.get("emission_energy", 0.0)) > 0.0
			material.emission = box.get("emission_color", Color.BLACK)
			material.emission_energy_multiplier = float(box.get("emission_energy", 0.0))
			material.roughness = 1.0
			material.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
			material.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
			instance.mesh.surface_set_material(side, material)
		instance.mesh.lightmap_unwrap(Transform3D.IDENTITY, 0.2)
	for light in lamps:
		light.light_bake_mode = Light3D.BAKE_DYNAMIC
	if is_instance_valid(native):
		native.prepare_lightmap("res://generated/native_reference.lmbake")
	var duplicate := duplicate() as Node3D
	duplicate.set_script(null)
	var hud := duplicate.get_node_or_null("HUD")
	if hud:
		hud.free()
	_set_owner_recursive(duplicate, duplicate)
	var packed := PackedScene.new()
	var error := packed.pack(duplicate)
	if error == OK:
		DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(path.get_base_dir()))
		error = ResourceSaver.save(packed, path)
	duplicate.free()
	print("NATIVE_SCENE_EXPORT ", error, " ", path)

func _set_owner_recursive(node: Node, root: Node) -> void:
	for child in node.get_children():
		child.owner = root
		_set_owner_recursive(child, root)

func _on_cache_failed(message: String) -> void:
	push_error("Sparse GI failed: " + message)
	get_tree().quit(1)

func _exit_tree() -> void:
	if cache != null:
		cache.shutdown()

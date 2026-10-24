extends SceneTree
## Real input dispatch plus GPU GI checks at non-top-down camera orientations.

var checks := {}

func _initialize() -> void:
	_run.call_deferred()

func _settle(frames: int) -> void:
	for frame in frames: await process_frame
	await RenderingServer.frame_post_draw

func _key(code: Key, pressed: bool) -> void:
	var event := InputEventKey.new()
	event.physical_keycode = code
	event.pressed = pressed
	Input.parse_input_event(event)
	Input.flush_buffered_events()

func _button(code: MouseButton, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = code
	event.pressed = pressed
	Input.parse_input_event(event)
	Input.flush_buffered_events()

func _motion(amount: Vector2) -> void:
	var event := InputEventMouseMotion.new()
	event.screen_relative = amount
	event.relative = amount
	Input.parse_input_event(event)
	Input.flush_buffered_events()

func _run() -> void:
	var scene = load("res://scenes/sponza.tscn").instantiate()
	root.add_child(scene)
	while not scene.ready_for_input: await process_frame
	root.grab_focus()
	await _settle(60)
	var camera: Camera3D = scene.camera
	var controls: FreeFlyCamera = scene.camera_controls
	# Manually step only the controller, so keyboard distances are deterministic.
	controls.set_process(false)
	var initial := camera.transform
	_motion(Vector2(50, 30))
	checks["mouse_requires_rmb"] = camera.transform == initial
	_button(MOUSE_BUTTON_RIGHT, true)
	_motion(Vector2(140, -60))
	checks["free_yaw_pitch"] = controls.looking and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED and not camera.basis.is_equal_approx(initial.basis)
	_motion(Vector2(0, -100000))
	checks["pitch_clamped_no_roll"] = absf(camera.rotation_degrees.x - 89.0) < 0.01 and absf(camera.rotation.z) < 0.0001
	_button(MOUSE_BUTTON_RIGHT, false)
	checks["rmb_release"] = not controls.looking and Input.mouse_mode == Input.MOUSE_MODE_VISIBLE
	scene._reset_camera()
	var origin := camera.position
	var forward := -camera.basis.z
	_key(KEY_W, true)
	controls._process(0.1)
	_key(KEY_W, false)
	var regular_distance := camera.position.distance_to(origin)
	checks["fly_along_view"] = (camera.position - origin).is_equal_approx(forward * 0.5) and absf(camera.position.y - origin.y) > 0.01
	origin = camera.position
	_key(KEY_W, true)
	_key(KEY_SHIFT, true)
	controls._process(0.1)
	_key(KEY_W, false)
	_key(KEY_SHIFT, false)
	checks["shift_boost"] = absf(camera.position.distance_to(origin) - regular_distance * 3.0) < 0.001
	origin = camera.position
	_key(KEY_SPACE, true)
	controls._process(0.1)
	_key(KEY_SPACE, false)
	checks["world_up"] = (camera.position - origin).is_equal_approx(Vector3.UP * 0.5)
	_key(KEY_CTRL, true)
	controls._process(0.1)
	_key(KEY_CTRL, false)
	checks["world_down"] = camera.position.is_equal_approx(origin)
	_key(KEY_Q, true)
	controls._process(0.1)
	_key(KEY_Q, false)
	checks["lamp_keys_do_not_move_camera"] = camera.position.is_equal_approx(origin)
	_button(MOUSE_BUTTON_WHEEL_UP, true)
	checks["wheel_dolly"] = (camera.position - origin).is_equal_approx(forward * 0.6)
	_button(MOUSE_BUTTON_RIGHT, true)
	root.focus_exited.emit()
	checks["focus_loss_releases_mouse"] = not controls.looking and Input.mouse_mode == Input.MOUSE_MODE_VISIBLE
	_button(MOUSE_BUTTON_RIGHT, true)
	_key(KEY_ESCAPE, true)
	_key(KEY_ESCAPE, false)
	checks["escape_releases_without_quitting"] = not controls.looking and Input.mouse_mode == Input.MOUSE_MODE_VISIBLE
	_key(KEY_P, true)
	_key(KEY_P, false)
	checks["overview_preset"] = camera.position.is_equal_approx(Vector3(20, 23, 17))
	_key(KEY_P, true)
	_key(KEY_P, false)
	checks["original_preset_restored"] = camera.transform.is_equal_approx(initial)
	var output := ProjectSettings.globalize_path("res://artifacts/free-camera")
	DirAccess.make_dir_recursive_absolute(output)
	var poses := []
	_button(MOUSE_BUTTON_RIGHT, true)
	for step in 30:
		_motion(Vector2(12, -2))
		await _settle(1)
	_button(MOUSE_BUTTON_RIGHT, false)
	poses.append(await _capture_pose(scene, output, "rotating"))
	camera.position = Vector3(5.8, 1.7, 0.3)
	camera.look_at(Vector3(-5.0, 1.7, -0.3))
	await _settle(60)
	poses.append(await _capture_pose(scene, output, "eye_level"))
	camera.look_at(Vector3(-2.0, 8.0, 0.0))
	await _settle(45)
	poses.append(await _capture_pose(scene, output, "upward"))
	camera.look_at(Vector3(10.0, 2.0, 0.0))
	await _settle(45)
	poses.append(await _capture_pose(scene, output, "reverse"))
	scene._reset_camera()
	await _settle(60)
	poses.append(await _capture_pose(scene, output, "restored"))
	checks["gi_finite_at_free_angles"] = poses.all(func(pose): return pose.finite and pose.counters[0] > 0)
	checks["no_geometry_rebake"] = scene.tg.stats.transport_builds == 0
	var result := {"passed": checks.values().all(func(value): return value), "checks": checks, "poses": poses}
	FileAccess.open(output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("FREE_CAMERA_RESULT ", JSON.stringify(result))
	quit(0 if result.passed else 1)

func _capture_pose(scene: Node3D, output: String, name: String) -> Dictionary:
	var debug: Dictionary = await scene._read_tg_debug()
	root.get_texture().get_image().save_png(output.path_join(name + ".png"))
	return {"name": name, "finite": scene._tg_metrics(debug.sh).finite, "counters": debug.counters, "age": scene._tg_age_stats(debug)}

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

## Holds the keys for one 0.1 s controller step and returns how far the camera moved.
func _step(controls: FreeFlyCamera, keys: Array) -> Vector3:
	var origin := controls.camera.position
	for code in keys: _key(code, true)
	controls._process(0.1)
	for code in keys: _key(code, false)
	return controls.camera.position - origin

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
	# Key bindings of the general fly camera: WASD, E / Space up, Q down, Shift x2.5.
	var forward := -camera.basis.z
	var speed := controls.speed
	var moved := _step(controls, [KEY_W])
	checks["fly_along_view"] = moved.is_equal_approx(forward * speed * 0.1) and absf(moved.y) > 0.01
	checks["shift_boost"] = _step(controls, [KEY_W, KEY_SHIFT]).is_equal_approx(forward * speed * 2.5 * 0.1)
	checks["e_space_up"] = _step(controls, [KEY_E]).is_equal_approx(Vector3.UP * speed * 0.1) and _step(controls, [KEY_SPACE]).is_equal_approx(Vector3.UP * speed * 0.1)
	checks["q_down"] = _step(controls, [KEY_Q]).is_equal_approx(Vector3.DOWN * speed * 0.1)
	checks["lamp_keys_do_not_move_camera"] = _step(controls, [KEY_PAGEUP]).is_zero_approx() and _step(controls, [KEY_PAGEDOWN]).is_zero_approx()
	# The wheel changes the speed and leaves the camera where it is.
	var position := camera.position
	_button(MOUSE_BUTTON_WHEEL_UP, true)
	var faster := controls.speed
	_button(MOUSE_BUTTON_WHEEL_DOWN, true)
	checks["wheel_speed"] = is_equal_approx(faster, speed + controls.speed_scroll_step) and is_equal_approx(controls.speed, speed) and camera.position == position
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

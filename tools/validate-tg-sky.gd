extends SceneTree
## Real Vulkan: sky-only transport, finite-range occlusion, resets and display A/B.
var _scene
var _output := ""

func _initialize() -> void:
	_run.call_deferred()

func _wait(count: int) -> void:
	for frame in count: await process_frame
	await RenderingServer.frame_post_draw

func _reset_gi() -> void:
	RenderingServer.call_on_render_thread(func(): _scene.tg.reset_history())

func _save(label: String) -> Dictionary:
	var debug: Dictionary = await _scene._read_tg_debug()
	for field in ["candidate", "direction", "position", "normal", "sh"]:
		FileAccess.open(_output.path_join(label + "_" + field + ".bin"), FileAccess.WRITE).store_buffer(debug[field].get_data())
	root.get_texture().get_image().save_png(_output.path_join(label + ".png"))
	return {"name": label, "frame": debug.frame, "width": debug.position.get_width(), "height": debug.position.get_height(), "candidate_width": debug.candidate.get_width(), "candidate_height": debug.candidate.get_height(), "counters": debug.counters, "sky": debug.sky, "sh": _scene._tg_metrics(debug.sh), "profile": debug.sampling_profile, "surface_history": debug.surface_history}

func _run() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--sky-output="): _output = arg.trim_prefix("--sky-output=")
	if _output.is_empty():
		quit(2)
		return
	DirAccess.make_dir_recursive_absolute(_output)
	_scene = load("res://scenes/sponza.tscn").instantiate()
	root.add_child(_scene)
	while not _scene.ready_for_input: await process_frame
	_scene.ready_for_input = false
	_scene.camera_controls.enabled = false
	root.get_node("DemoViewer").capture_mode = true
	for canvas in _scene.find_children("*", "CanvasLayer", true, false): canvas.visible = false
	root.use_taa = false
	_scene.sun.visible = false
	_scene.lamp.visible = false
	_scene.atmosphere.active = false
	_scene.atmosphere.apply()
	_scene._select_mode(2)
	var sky_color: Color = _scene.atmosphere.sky_color
	var blocker_mesh: Mesh = _scene.blocker.mesh
	var blocker_transform: Transform3D = _scene.blocker.transform
	var roof := BoxMesh.new()
	roof.size = Vector3(2000000, 1, 2000000)
	var result := {"cases": [], "passed": true}
	for name in ["dark", "sky", "half_energy", "warm", "far_roof", "restored", "off_after3", "full_without_sky", "full_with_sky", "adaptive", "resize"]:
		_scene.atmosphere.sky_color = Color(0.45, 0.3, 0.15) if name == "warm" else sky_color
		_scene.atmosphere.sky_energy = 0.5 if name == "half_energy" else 1.0
		_scene._set_sky_enabled(name not in ["dark", "off_after3", "full_without_sky"])
		if name == "far_roof":
			# Invisible to rasterization; intentionally beyond the 80 m GI range.
			# It must still block all upward BVH rays, including near-horizontal ones.
			_scene.blocker.mesh = roof
			_scene.blocker.position = Vector3(0, 150, 0)
			_scene.blocker.layers = 0
			_scene.blocker.visible = true
		if name == "restored":
			_scene.blocker.visible = false
			_scene.blocker.layers = 1
			_scene.blocker.mesh = blocker_mesh
			_scene.blocker.transform = blocker_transform
		if name == "full_without_sky":
			_scene.sun.visible = true
			_scene._select_mode(1)
		if name == "adaptive":
			root.use_taa = true
			_scene.tg_sampling = 3
			_scene.tg.set_sampling(3)
		if name == "resize": root.size = Vector2i(997, 613)
		if name == "off_after3":
			# Exercise the normal setter's invalidation, without a test-side reset.
			await _wait(3)
		else:
			await _wait(4)
			_reset_gi()
			await _wait(120)
		var sample := await _save(name)
		result.cases.append(sample)
		result.passed = result.passed and sample.sh.finite
		if name in ["dark", "far_roof", "off_after3"]:
			result.passed = result.passed and sample.sh.maximum == 0.0 and sample.counters[12] == 0
		else: result.passed = result.passed and sample.sh.maximum > 0.0
		if name in ["sky", "half_energy", "warm", "restored", "full_with_sky", "adaptive", "resize"]: result.passed = result.passed and sample.counters[12] > 0
	# Exercise the actual key binding and verify it updates the source next frame.
	_scene.ready_for_input = true
	var event := InputEventKey.new()
	event.physical_keycode = KEY_8
	event.pressed = true
	_scene._unhandled_input(event)
	_scene.ready_for_input = false
	await _wait(3)
	var toggled: Dictionary = await _scene._read_tg_debug()
	result["key_toggle_passed"] = not _scene.atmosphere.sky_enabled and toggled.sky == [0.0, 0.0, 0.0, 0.0] and toggled.counters[12] == 0
	result.passed = result.passed and result.key_toggle_passed
	FileAccess.open(_output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("TG_SKY_RESULT ", JSON.stringify(result))
	quit(0 if result.passed else 1)

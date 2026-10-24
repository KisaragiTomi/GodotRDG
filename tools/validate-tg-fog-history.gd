extends SceneTree
## Native composition A/B plus matching fogged/clear surface-history sequences.
var _output := ""
var _scene
var _control := false

func _initialize() -> void:
	_run.call_deferred()

func _wait(count: int) -> void:
	for frame in count: await process_frame
	await RenderingServer.frame_post_draw

func _reset_gi() -> void:
	RenderingServer.call_on_render_thread(func(): _scene.tg.reset_history())

func _save(label: String) -> Dictionary:
	var debug: Dictionary = await _scene._read_tg_debug()
	for field in ["sh", "position", "normal", "age", "radiance"]:
		FileAccess.open(_output.path_join(label + "_" + field + ".bin"), FileAccess.WRITE).store_buffer(debug[field].get_data())
	root.get_texture().get_image().save_png(_output.path_join(label + ".png"))
	var rgb: Image = debug.radiance.duplicate()
	rgb.convert(Image.FORMAT_RGBF)
	var maximum := 0.0
	var finite := true
	for value in rgb.get_data().to_float32_array():
		maximum = maxf(maximum, absf(value))
		finite = finite and is_finite(value)
	return {"name": label, "frame": debug.frame, "width": debug.position.get_width(), "height": debug.position.get_height(), "radiance_width": debug.radiance.get_width(), "radiance_height": debug.radiance.get_height(), "counters": debug.counters, "surface_history": debug.surface_history, "taa_history": debug.taa_history, "sh": _scene._tg_metrics(debug.sh), "radiance": {"maximum": maximum, "finite": finite}, "stages": _scene.tg.stats.get("stage_gpu_ms", {}).duplicate()}

func _run() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--fog-history-output="): _output = arg.trim_prefix("--fog-history-output=")
		if arg == "--fog-history-control": _control = true
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
	var result := {"separated": _scene.separate_surface_fog, "composition": [], "integration": [], "passed": true}
	root.use_taa = false
	_scene.atmosphere.temporal_reprojection = false
	var pose: Transform3D = _scene.camera.global_transform
	# No temporal jitter or GI feedback: only the fog composition order differs.
	for name in ["msaa_off", "msaa_2", "msaa_4", "msaa_8", "dense", "sky_edge", "gi_off"]:
		root.msaa_3d = {"msaa_off": Viewport.MSAA_DISABLED, "msaa_2": Viewport.MSAA_2X, "msaa_4": Viewport.MSAA_4X, "msaa_8": Viewport.MSAA_8X}.get(name, Viewport.MSAA_2X)
		_scene.atmosphere.thickness = 4.0 if name == "dense" else 1.1
		_scene.atmosphere.apply()
		_scene.camera.global_transform = pose
		if name == "sky_edge": _scene.camera.look_at_from_position(Vector3(1, 5, 0), Vector3(-5, 11, 0))
		_scene._select_mode(0 if name == "gi_off" else 4)
		await _wait(4)
		_reset_gi()
		await _wait(64)
		var sample := await _save(name)
		result.composition.append(sample)
		result.passed = result.passed and sample.sh.finite
	# Matching deterministic sequences: fog changes only the displayed image.
	_scene.camera.global_transform = pose
	root.use_taa = true
	_scene.atmosphere.temporal_reprojection = true
	_scene._select_mode(1)
	_scene.atmosphere.active = not _control
	_scene.atmosphere.apply()
	await _wait(4)
	_reset_gi()
	for name in ["screen", "thick", "zero_density", "restored_fog", "camera", "blocker", "lamp", "lights_off", "lights_restored", "resize"]:
		_scene.atmosphere.thickness = 4.0 if name == "thick" else (0.0 if name == "zero_density" else 1.1)
		_scene.atmosphere.apply()
		if name == "camera": _scene.camera.position += Vector3(-0.4, 0.2, 0.3)
		if name == "blocker": _scene._move_blocker()
		if name == "lamp": _scene.lamp.visible = true
		if name == "lights_off":
			_scene.sun.visible = false
			_scene.lamp.visible = false
			_scene._set_sky_enabled(false)
		if name == "lights_restored":
			_scene.sun.visible = true
			_scene._set_sky_enabled(true)
		if name == "resize": root.size = Vector2i(997, 613)
		await _wait(3 if name == "lights_off" else 80)
		var sample := await _save(name)
		result.integration.append(sample)
		result.passed = result.passed and sample.sh.finite and sample.radiance.finite
		if _scene.separate_surface_fog:
			result.passed = result.passed and sample.surface_history and not sample.taa_history
			if name != "lights_off": result.passed = result.passed and sample.counters[1] > 0 and sample.counters[2] > 0
		if name == "lights_off":
			result.passed = result.passed and sample.sh.maximum == 0.0
			if _scene.separate_surface_fog: result.passed = result.passed and sample.radiance.maximum == 0.0
	FileAccess.open(_output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("TG_FOG_HISTORY_RESULT ", JSON.stringify(result))
	quit(0 if result.passed else 1)

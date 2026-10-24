extends SceneTree
## Fixed-input world-cache retention, invalidation and clean-fog integration.
var _scene
var _output := ""
var _fog := false
var _debug: Dictionary = {}

func _initialize() -> void:
	_run.call_deferred()

func _wait(frames: int) -> void:
	for frame in frames: await process_frame
	await RenderingServer.frame_post_draw

func _reset_screen(reset_world: bool = false) -> void:
	RenderingServer.call_on_render_thread(func(): _scene.tg.reset_history(reset_world))

func _freeze(value: bool) -> void:
	RenderingServer.call_on_render_thread(func(): _scene.tg.world_cache_updates = not value)

func _save(label: String) -> Dictionary:
	_debug = {}
	_scene.tg.read_debug(func(value): _debug = value, true)
	while _debug.is_empty(): await process_frame
	var debug := _debug
	for field in ["candidate", "position", "normal", "sh", "age", "radiance"]:
		FileAccess.open(_output.path_join(label + "_" + field + ".bin"), FileAccess.WRITE).store_buffer(debug[field].get_data())
	root.get_texture().get_image().save_png(_output.path_join(label + ".png"))
	FileAccess.open(_output.path_join(label + "_cache.bin"), FileAccess.WRITE).store_buffer(debug.world_entries)
	return {"name": label, "frame": debug.frame, "epoch": debug.world_epoch, "tick": debug.world_tick, "cooldown": debug.world_cooldown, "width": debug.position.get_width(), "height": debug.position.get_height(), "candidate_width": debug.candidate.get_width(), "candidate_height": debug.candidate.get_height(), "radiance_width": debug.radiance.get_width(), "radiance_height": debug.radiance.get_height(), "counters": debug.counters, "sh": _scene._tg_metrics(debug.sh), "world_cache": debug.world_cache, "stages": _scene.tg.stats.get("stage_gpu_ms", {}).duplicate()}

func _run() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--world-cache-output="): _output = arg.trim_prefix("--world-cache-output=")
		if arg == "--world-cache-fog": _fog = true
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
	_scene.atmosphere.active = _fog
	_scene.atmosphere.apply()
	_scene._select_mode(2)
	var pose: Transform3D = _scene.camera.transform
	var enabled: bool = _scene.tg_world_cache
	var result := {"enabled": enabled, "fog": _fog, "cases": [], "passed": true}
	for name in ["warm_a", "turned_frozen", "expired", "rewarmed", "lights_off", "sky_restored", "blocker_reset", "blocker_settled", "lamp_motion", "screen", "resized", "basic_filter", "no_history"]:
		var frames := 80
		if name == "warm_a": frames = 160
		if name == "turned_frozen":
			_freeze(true)
			_scene.camera.rotate_y(0.9)
			frames = 12
		if name == "expired": frames = 125
		if name == "rewarmed":
			_scene.camera.transform = pose
			_freeze(false)
		if name == "lights_off":
			_scene._set_sky_enabled(false)
			frames = 3
		if name == "sky_restored": _scene._set_sky_enabled(true)
		if name == "blocker_reset":
			_scene._move_blocker()
			frames = 1
		if name == "lamp_motion":
			_scene.lamp.visible = true
			await _wait(60)
			_scene.lamp.position.x += 0.05
			frames = 2
		if name == "screen": _scene._select_mode(1)
		if name == "resized": root.size = Vector2i(997, 613)
		if name == "basic_filter": _scene.tg.set_post_denoise(false)
		if name == "no_history": _scene._select_mode(4)
		if name not in ["lights_off", "blocker_reset", "lamp_motion"]:
			await _wait(4)
			_reset_screen(name == "warm_a")
		await _wait(frames)
		var sample := await _save(name)
		var passed: bool = sample.sh.finite
		if name == "lights_off": passed = passed and sample.sh.maximum == 0.0
		if name in ["lights_off", "blocker_reset", "lamp_motion", "expired", "no_history"] or not enabled: passed = passed and sample.counters[14] == 0
		if enabled and name in ["warm_a", "rewarmed", "sky_restored", "blocker_settled", "screen", "resized", "basic_filter"]: passed = passed and sample.counters[14] > 0 and sample.counters[16] > 0
		if enabled and name == "turned_frozen": passed = passed and sample.counters[15] > 0 and sample.counters[16] == 0
		if enabled and name == "lamp_motion": passed = passed and sample.cooldown > 40 and sample.counters[16] == 0
		if name == "screen": passed = passed and sample.counters[1] > 0
		sample["passed"] = passed
		result.cases.append(sample)
		result.passed = result.passed and passed
		print("WORLD_CACHE_CASE ", name, " ", passed, " counters=", sample.counters)
	# Test the actual interactive toggle, including allocation on first enable.
	_scene.ready_for_input = true
	var event := InputEventKey.new()
	event.physical_keycode = KEY_9
	event.pressed = true
	_scene._unhandled_input(event)
	_scene.ready_for_input = false
	_scene._select_mode(2)
	await _wait(30)
	var toggled: Dictionary = await _scene._read_tg_debug()
	result["key_toggle_passed"] = toggled.world_cache != enabled and _scene.tg_world_cache != enabled
	if not enabled: result.key_toggle_passed = result.key_toggle_passed and toggled.counters[14] > 0
	else: result.key_toggle_passed = result.key_toggle_passed and toggled.counters[14] == 0
	result.passed = result.passed and result.key_toggle_passed
	FileAccess.open(_output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("WORLD_CACHE_RESULT passed=", result.passed)
	quit(0 if result.passed else 1)

extends SceneTree
## Exercise reactive lighting, partial workgroups, empty tiles and live switching.

func _initialize() -> void:
	_run.call_deferred()

func _wait(count: int) -> void:
	for frame in count: await process_frame
	await RenderingServer.frame_post_draw

func _budget_valid(debug: Dictionary) -> bool:
	var size: Vector2i = debug.trace_size
	var bytes: PackedByteArray = debug.ray_budget
	if bytes.size() != size.x * size.y: return false
	var total := 0
	var base := 0
	for y in range(0, size.y, 8):
		for x in range(0, size.x, 8):
			var receivers := 0
			var extra := 0
			for j in range(y, mini(y + 8, size.y)):
				for i in range(x, mini(x + 8, size.x)):
					var rays := bytes[j * size.x + i] & 3
					if rays > 2: return false
					var visible: bool = debug.position.get_pixel(i * 2, j * 2).a > 0.0
					if (rays > 0) != visible: return false
					receivers += int(rays > 0)
					extra += int(rays == 2)
					total += rays
			base += receivers
			if extra > receivers / 2: return false
	return total == debug.counters[0] and base == debug.counters[8] and total - base == debug.counters[7]

func _run() -> void:
	var output := ProjectSettings.globalize_path("res://artifacts/tg-adaptive/runtime")
	DirAccess.make_dir_recursive_absolute(output)
	var scene = load("res://scenes/sponza.tscn").instantiate()
	root.add_child(scene)
	while not scene.ready_for_input: await process_frame
	scene.ready_for_input = false
	scene.camera_controls.enabled = false
	root.get_node("DemoViewer").capture_mode = true
	scene.tg_sampling = 3
	scene.tg.set_sampling(3)
	scene._select_mode(2)
	await _wait(120)
	var result := {"cases": {}, "passed": true}
	var pose: Transform3D = scene.camera.global_transform
	var reset_count: int = scene.tg.stats.history_resets
	for name in ["static", "lamp", "no_history", "odd_size", "partial_sky", "sky"]:
		if name == "lamp": scene.lamp.position.x += 0.1
		if name == "no_history": scene._select_mode(4)
		if name == "odd_size":
			scene._select_mode(2)
			root.size = Vector2i(997, 613)
		if name == "partial_sky": scene.camera.look_at_from_position(Vector3(0, 6, 0), Vector3(4, 9, 0))
		if name == "sky": scene.camera.look_at_from_position(Vector3(0, 100, 0), Vector3(1, 120, 0))
		await _wait(2 if name == "lamp" else 60)
		var debug: Dictionary = await scene._read_tg_debug()
		var passed: bool = _budget_valid(debug) and scene._tg_metrics(debug.sh).finite
		if name == "lamp": passed = passed and debug.counters[11] > 0 and scene.tg.stats.history_resets == reset_count
		if name == "no_history": passed = passed and debug.counters[7] > 0 and scene._tg_age_stats(debug).mean == 1.0
		if name == "partial_sky": passed = passed and debug.counters[8] > 0 and debug.counters[8] < debug.trace_size.x * debug.trace_size.y
		if name == "sky": passed = passed and debug.counters[0] == 0 and scene._tg_metrics(debug.sh).maximum == 0.0
		result.cases[name] = {"passed": passed, "counters": debug.counters, "trace_size": [debug.trace_size.x, debug.trace_size.y], "stages": scene.tg.stats.stage_gpu_ms.duplicate()}
		result.passed = result.passed and passed
		root.get_texture().get_image().save_png(output.path_join(name + ".png"))
	scene.camera.global_transform = pose
	root.size = Vector2i(1280, 800)
	scene.tg_sampling = 0
	scene.tg.set_sampling(0)
	await _wait(10)
	for profile in [3, 1, 0]:
		scene.ready_for_input = true
		var event := InputEventKey.new()
		event.physical_keycode = KEY_6
		event.pressed = true
		scene._unhandled_input(event)
		scene.ready_for_input = false
		await _wait(30)
		var debug: Dictionary = await scene._read_tg_debug()
		var stages: Dictionary = scene.tg.stats.stage_gpu_ms
		var passed: bool = debug.sampling_profile == profile and scene.tg_sampling == profile and scene._tg_metrics(debug.sh).maximum > 0.0
		passed = passed and stages.has("budget") == (profile == 3) and stages.has("project") == (profile == 1)
		passed = passed and scene._timing_values.budget.text != "--" and scene._timing_values.project.text != "--"
		if profile == 3: passed = passed and _budget_valid(debug)
		result.cases["switch_%d" % profile] = {"passed": passed, "counters": debug.counters}
		result.passed = result.passed and passed
	FileAccess.open(output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("TG_ADAPTIVE_RUNTIME ", JSON.stringify(result))
	quit(0 if result.passed else 1)

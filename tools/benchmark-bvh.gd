extends SceneTree
## Fixed rays/cameras/materials; only the geometry acceleration directory changes.

func _initialize() -> void:
	_run.call_deferred()

func _wait(count: int) -> void:
	for frame in count: await process_frame
	await RenderingServer.frame_post_draw

func _median(values: Array) -> float:
	values.sort()
	return float(values[values.size() / 2])

func _run() -> void:
	if RenderingServer.get_rendering_device() == null:
		quit(2)
		return
	var output := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--benchmark-output="): output = arg.trim_prefix("--benchmark-output=")
	if output.is_empty():
		quit(2)
		return
	DirAccess.make_dir_recursive_absolute(output)
	var scene = load("res://scenes/sponza.tscn").instantiate()
	root.add_child(scene)
	while not scene.ready_for_input: await process_frame
	scene.ready_for_input = false
	scene.camera_controls.enabled = false
	root.use_taa = false
	for canvas in scene.find_children("*", "CanvasLayer", true, false): canvas.visible = false
	root.get_node("DemoViewer").capture_mode = true
	var pose: Transform3D = scene.camera.global_transform
	var result := {"gpu": RenderingServer.get_video_adapter_name(), "cases": {}, "passed": true}
	for name in ["bvh", "bvh_reverse", "fog", "screen"]:
		scene.camera.global_transform = pose
		if name == "bvh_reverse": scene.camera.look_at_from_position(Vector3(7, 2, 0), Vector3(-5, 3, 0))
		scene.atmosphere.active = name == "fog"
		scene.atmosphere.apply()
		scene._select_mode(1 if name == "screen" else 2)
		RenderingServer.call_on_render_thread(func(): scene.tg.reset_history())
		await _wait(120)
		var trace: Array = []
		var total: Array = []
		var viewport: Array = []
		var last_frame := -1
		for sample in 150:
			await process_frame
			var timing: Dictionary = scene.tg.stats
			if timing.get("timing_frame", -1) == last_frame: continue
			last_frame = timing.timing_frame
			trace.append(timing.stage_gpu_ms.trace)
			total.append(timing.stage_gpu_ms.total)
			viewport.append(RenderingServer.viewport_get_measured_render_time_gpu(root.get_viewport_rid()))
		# Lock the stochastic sample index and allow history to converge for image A/B.
		scene._select_mode(4)
		for frame in 12:
			RenderingServer.call_on_render_thread(func(): scene.tg.frame_index = 512)
			await _wait(1)
		var debug: Dictionary = await scene._read_tg_debug()
		var metrics: Dictionary = scene._tg_metrics(debug.sh)
		FileAccess.open(output.path_join(name + ".sh"), FileAccess.WRITE).store_buffer(debug.sh.get_data())
		root.get_texture().get_image().save_png(output.path_join(name + ".png"))
		result.cases[name] = {"trace_ms": _median(trace), "total_ms": _median(total), "viewport_ms": _median(viewport), "samples": trace.size(), "sh_finite": metrics.finite}
		result.passed = result.passed and metrics.finite and trace.size() > 100
	FileAccess.open(output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("BVH_BENCHMARK: ", JSON.stringify(result))
	quit(0 if result.passed else 1)

extends SceneTree
## Check that repeated diffuse feedback settles under an unchanged sky source.
func _initialize() -> void:
	_run.call_deferred()

func _run() -> void:
	var output := ProjectSettings.globalize_path("res://artifacts/tg-world-cache/stability")
	DirAccess.make_dir_recursive_absolute(output)
	var scene = load("res://scenes/sponza.tscn").instantiate()
	root.add_child(scene)
	while not scene.ready_for_input: await process_frame
	scene.ready_for_input = false
	scene.camera_controls.enabled = false
	root.get_node("DemoViewer").capture_mode = true
	root.use_taa = false
	scene.sun.visible = false
	scene.lamp.visible = false
	scene.atmosphere.active = false
	scene.atmosphere.apply()
	scene._select_mode(2)
	var result := {"samples": [], "passed": true}
	for frames in [160, 320, 480]:
		for frame in frames: await process_frame
		await RenderingServer.frame_post_draw
		var debug: Dictionary = await scene._read_tg_debug()
		var metrics: Dictionary = scene._tg_metrics(debug.sh)
		result.samples.append({"frame": debug.frame, "sh": metrics, "counters": debug.counters})
		result.passed = result.passed and debug.world_cache and debug.counters[14] > 0 and metrics.finite and metrics.maximum < 0.6
	var a: float = result.samples[1].sh.mean_absolute
	var b: float = result.samples[2].sh.mean_absolute
	result["late_relative_change"] = absf(b - a) / maxf(a, 0.000001)
	result.passed = result.passed and result.late_relative_change < 0.05
	FileAccess.open(output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("WORLD_CACHE_STABILITY ", JSON.stringify(result))
	quit(0 if result.passed else 1)

extends SceneTree
## Run with --path . --script tools/validate-tg-timing.gd -- --tg.

func _initialize() -> void:
	_run.call_deferred()

func _settle(frames: int) -> void:
	for frame in frames: await process_frame
	await RenderingServer.frame_post_draw

func _run() -> void:
	var scene = load("res://scenes/sponza.tscn").instantiate()
	root.add_child(scene)
	while not scene.ready_for_input: await process_frame
	# Drive the scene deterministically while retaining normal HUD updates.
	scene.ready_for_input = false
	var output := ProjectSettings.globalize_path("res://artifacts/tg-timing")
	DirAccess.make_dir_recursive_absolute(output)
	var result := {"passed": true, "cases": {}}
	for name in ["quality", "sparse", "basic", "post_again", "off", "small"]:
		scene.tg_sampling = 1 if name == "quality" else 0
		scene.tg.set_sampling(scene.tg_sampling)
		scene.tg_post_denoise = name != "basic"
		scene.tg.set_post_denoise(scene.tg_post_denoise)
		scene._select_mode(0 if name == "off" else 1)
		if name == "small": root.size = Vector2i(960, 600)
		await _settle(100)
		var stages: Dictionary = scene.tg.stats.get("stage_gpu_ms", {}).duplicate()
		var values := {}
		for key in scene._timing_values: values[key] = scene._timing_values[key].text
		var valid := true
		if name == "off":
			valid = values.total == "0.000" and values.trace == "--"
		else:
			var sum := 0.0
			for stage in ["guide", "trace", "reconstruct", "filter", "filter_medium", "filter_wide", "filter_large", "history"]:
				if name == "basic" and stage in ["filter_medium", "filter_wide", "filter_large"]:
					valid = valid and values[stage] == "跳过" and not stages.has(stage)
					continue
				var ms: float = stages.get(stage, -1.0)
				valid = valid and is_finite(ms) and ms > 0.0
				sum += ms
			sum += float(stages.get("project", 0.0))
			var total: float = stages.get("total", -1.0)
			valid = valid and is_finite(total) and total >= sum - 0.01 and total - sum < 0.5
			valid = valid and (values.project == "跳过" if scene.tg_sampling == 0 else float(stages.get("project", -1.0)) > 0.0)
		result.cases[name] = {"passed": valid, "gpu_ms": stages, "hud": values}
		result.passed = result.passed and valid
		root.get_texture().get_image().save_png(output.path_join(name + ".png"))
	FileAccess.open(output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("TG_TIMING_RESULT ", JSON.stringify(result))
	quit(0 if result.passed else 1)

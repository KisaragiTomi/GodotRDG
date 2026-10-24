extends SceneTree
## Exercise views outside the old camera presets: distant city must stay in fog.

var output := ""
var scene: Node3D
var failures: Array[String] = []

func _initialize() -> void:
	_run.call_deferred()

func _wait(count := 100) -> void:
	for frame in count: await process_frame
	await RenderingServer.frame_post_draw

func _shot(label: String) -> void:
	await _wait()
	root.get_texture().get_image().save_png(output.path_join(label + ".png"))

func _run() -> void:
	if RenderingServer.get_rendering_device() == null:
		quit(2)
		return
	output = ProjectSettings.globalize_path("res://artifacts/city-coverage/after")
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--coverage-output="): output = arg.trim_prefix("--coverage-output=")
	DirAccess.make_dir_recursive_absolute(output)
	scene = load("res://scenes/city.tscn").instantiate()
	root.add_child(scene)
	scene.capture_path = "validation"
	scene.controls.enabled = false
	scene.hud.visible = false
	scene.set_process(false)
	root.get_node("DemoViewer").capture_mode = true
	var fog: TGAtmosphere = scene.atmosphere
	var result := {"gpu": RenderingServer.get_video_adapter_name(), "views": {}}
	var positions := [Vector3(340, 260, 370), Vector3(540, 260, 640), Vector3(850, 400, 900), Vector3(33, 12, 720)]
	for index in positions.size():
		scene._set_view(1 if index == 3 else 0)
		scene.camera.look_at_from_position(positions[index], Vector3(0, 20, 0))
		scene._apply_fog_profile()
		var label := "view_%d" % index
		await _shot(label)
		result.views[label] = {"camera_distance": positions[index].length(), "fog_length": fog.fog_length}
		# Independently cover every visible surface; keep native shadowed density.
		fog.fog_length = scene.camera.far
		fog.apply()
		fog.environment.fog_enabled = false
		RenderingServer.environment_set_volumetric_fog_volume_size(384, 384)
		fog.invalidate_history()
		await _shot(label + "_reference")
		fog.active = false
		fog.apply()
		fog.environment.fog_enabled = false
		await _shot(label + "_clear")
		fog.active = true
		RenderingServer.environment_set_volumetric_fog_volume_size(256, 192)
	FileAccess.open(output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("CITY_COVERAGE: ", JSON.stringify(result))
	quit()

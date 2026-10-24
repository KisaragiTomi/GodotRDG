extends SceneTree
## Both quality budgets must preserve full city coverage, including free flight.

var scene: Node3D
var output := ""
var failures: Array[String] = []
var cases := {}

func _initialize() -> void:
	_run.call_deferred()

func _wait(count: int) -> void:
	for frame in count: await process_frame
	await RenderingServer.frame_post_draw

func _check(condition: bool, message: String) -> void:
	if not condition: failures.append(message)

func _shot(label: String, frames := 100) -> void:
	await _wait(frames)
	_check(root.get_texture().get_image().save_png(output.path_join(label + ".png")) == OK, label + ": screenshot")

func _sample(label: String) -> void:
	await _wait(100)
	var gpu: Array[float] = []
	for sample in 100:
		await process_frame
		gpu.append(RenderingServer.viewport_get_measured_render_time_gpu(root.get_viewport_rid()))
	gpu.sort()
	var viewer := root.get_node("DemoViewer")
	cases[label] = {"viewport_median_ms": gpu[50], "fog_ms": viewer.latest.get("fog_ms", -1.0), "coverage_length": scene.atmosphere.fog_length}
	await _shot(label, 0)

func _run() -> void:
	if RenderingServer.get_rendering_device() == null:
		quit(2)
		return
	output = ProjectSettings.globalize_path("res://artifacts/city-fog")
	DirAccess.make_dir_recursive_absolute(output)
	scene = load("res://scenes/city.tscn").instantiate()
	root.add_child(scene)
	scene.capture_path = "validation"
	scene.controls.enabled = false
	scene.hud.visible = false
	root.get_node("DemoViewer").capture_mode = true
	var fog: TGAtmosphere = scene.atmosphere
	for view in [0, 1, 2]:
		scene._set_view(view)
		for balanced in [false, true]:
			scene.balanced_fog = balanced
			scene._apply_fog_profile()
			var label := "%d_%s" % [view, "balanced" if balanced else "high"]
			await _sample(label)
			_check(fog.environment.volumetric_fog_temporal_reprojection_amount > 0.0, label + ": history")
			_check(fog.fog_length >= scene._required_fog_length(), label + ": full city coverage")
			RenderingServer.environment_set_volumetric_fog_volume_size(512, 384)
			fog.invalidate_history()
			await _shot(label + "_reference", 130)
			var grid: Vector2i = scene.BALANCED_FOG_GRID if balanced else scene._project_fog_grid
			RenderingServer.environment_set_volumetric_fog_volume_size(grid.x, grid.y)
			fog.invalidate_history()
	scene._set_view(1)
	var pose: Transform3D = scene.camera.global_transform
	await _shot("restored")
	var reprojected := true
	for step in 48:
		scene.camera.position += Vector3(0.04, 0, -0.05)
		await _wait(1)
		reprojected = reprojected and fog.environment.volumetric_fog_temporal_reprojection_amount > 0.0
	_check(reprojected, "ordinary motion retains reprojection")
	await _shot("moving", 0)
	await _shot("moving_settled")
	scene.camera.global_transform = pose
	fog.invalidate_history()
	await _shot("returned")
	fog.thickness = 0.0
	fog.apply()
	_check(not fog.environment.fog_enabled and not fog.environment.volumetric_fog_enabled, "zero density disables fog")
	await _shot("zero")
	fog.thickness = scene.FOG_THICKNESS
	fog.active = false
	fog.apply()
	_check(not fog.environment.fog_enabled and not fog.environment.volumetric_fog_enabled, "toggle disables fog")
	await _shot("off")
	fog.active = true
	fog.apply()
	# Move from a street preset to a remote viewpoint WITHOUT choosing a preset.
	scene.camera.look_at_from_position(Vector3(850, 400, 900), Vector3(0, 20, 0))
	await _wait(8)
	_check(fog.fog_length >= scene._required_fog_length() and fog.fog_length > 1200.0, "free flight expands coverage")
	var previous_range: float = fog.fog_length
	for step in 16:
		scene.camera.position += Vector3(0.005, 0, 0)
		await _wait(1)
	_check(fog.fog_length == previous_range and fog.environment.volumetric_fog_temporal_reprojection_amount > 0.0, "range hysteresis preserves settled history")
	scene.camera.fov = 100.0
	scene.camera.look_at(Vector3(0, 35, 0))
	await _wait(8)
	_check(fog.fog_length >= scene._required_fog_length(), "wide FOV retains city coverage")
	# Restore all pre-existing environment settings when the helper is removed.
	var saved: Dictionary = fog._saved_environment.duplicate()
	var env: Environment = fog.environment
	scene.remove_child(fog)
	for property in saved: _check(env.get(property) == saved[property], "environment restore: " + property)
	fog.free()
	scene.atmosphere = null
	var result := {"gpu": RenderingServer.get_video_adapter_name(), "cases": cases, "passed": failures.is_empty(), "failures": failures}
	FileAccess.open(output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("CITY_FOG_RESULT: ", JSON.stringify(result))
	quit(0 if failures.is_empty() else 1)

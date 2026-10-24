extends SceneTree
## Identical-camera fog quality comparison and motion/light-history checks.

var scene: Node3D
var output: String
var fog: TGAtmosphere

func _initialize() -> void:
	_run.call_deferred()

func _wait_frames(count: int) -> void:
	for index in count: await process_frame
	await RenderingServer.frame_post_draw

func _shot(label: String, frames := 96) -> void:
	await _wait_frames(frames)
	root.get_texture().get_image().save_png(output.path_join(label + ".png"))

func _run() -> void:
	if RenderingServer.get_rendering_device() == null:
		push_error("GPU quality validation requires non-headless Vulkan.")
		quit(2)
		return
	var sponza := OS.get_cmdline_user_args().has("--tg")
	output = ProjectSettings.globalize_path("res://artifacts/atmosphere-quality/" + ("sponza" if sponza else "courtyard"))
	DirAccess.make_dir_recursive_absolute(output)
	scene = load("res://scenes/sponza.tscn" if sponza else "res://scenes/atmosphere.tscn").instantiate()
	root.add_child(scene)
	if sponza:
		while not scene.ready_for_input: await process_frame
		scene.camera_capture_path = "quality-validation"
		scene._select_mode(2)
	else:
		scene.capture_path = "quality-validation"
		scene.controls.enabled = false
	for canvas in scene.find_children("*", "CanvasLayer", true, false): canvas.visible = false
	fog = scene.atmosphere
	var pose: Transform3D = scene.camera.global_transform
	fog.active = false
	fog.apply()
	await _shot("clear")
	fog.active = true
	fog.detail_spread = 0.65
	fog.temporal_reprojection = false
	fog.apply()
	await _shot("before")
	fog.detail_spread = 2.0
	fog.temporal_reprojection = true
	fog.apply()
	await _shot("after")
	# An independently higher sampling budget reveals fixed-grid edge errors.
	RenderingServer.environment_set_volumetric_fog_volume_size(512, 384)
	fog.invalidate_history()
	await _shot("reference", 160)
	RenderingServer.environment_set_volumetric_fog_volume_size(256, 192)
	fog.invalidate_history()
	await _wait_frames(60)
	var motion_reprojected := true
	for index in 48:
		scene.camera.position += Vector3(0.006, 0, -0.01)
		await _wait_frames(1)
		motion_reprojected = motion_reprojected and fog.environment.volumetric_fog_temporal_reprojection_enabled and fog.environment.volumetric_fog_temporal_reprojection_amount > 0.0
	await _shot("moving", 0)
	await _shot("motion_settled")
	scene.camera.global_transform = pose
	fog.invalidate_history()
	await _shot("restored")
	var sun_energy: float = fog.sunlight
	fog.sunlight = 0.0
	fog.apply()
	await _shot("light_off_fast", 6)
	await _shot("light_off_settled")
	fog.sunlight = sun_energy
	fog.apply()
	var result := {"motion_reprojected": motion_reprojected, "gpu": RenderingServer.get_video_adapter_name(), "passed": motion_reprojected}
	FileAccess.open(output.path_join("runtime.json"), FileAccess.WRITE).store_string(JSON.stringify(result, "\t"))
	print("FOG_QUALITY_RUNTIME: ", JSON.stringify(result))
	quit(0 if result.passed else 1)

extends SceneTree
## Exercise real hotkeys, GPU scope measurements, and scene/resource round trips.

var viewer: Node
var output := ""
var failures: Array[String] = []
var samples: Array[Dictionary] = []

func _initialize() -> void:
	output = ProjectSettings.globalize_path("res://artifacts/viewer")
	call_deferred("_run")

func _key(code: Key) -> void:
	var event := InputEventKey.new()
	event.physical_keycode = code
	event.pressed = true
	Input.parse_input_event(event)
	event = InputEventKey.new()
	event.physical_keycode = code
	event.pressed = false
	Input.parse_input_event(event)

func _settle(frames := 90) -> void:
	for frame in frames: await process_frame
	await RenderingServer.frame_post_draw

func _switch(code: Key, scene_path: String) -> bool:
	_key(code)
	for frame in 900:
		await process_frame
		if current_scene != null and current_scene.scene_file_path == scene_path and not viewer.busy:
			await _settle()
			return true
	failures.append("Scene key did not reach " + scene_path)
	return false

func _check(condition: bool, message: String) -> void:
	if not condition: failures.append(message)

func _snapshot(label: String, fog_on: bool, gi_state: int) -> void:
	var data: Dictionary = viewer.latest.duplicate(true)
	data["label"] = label
	data["scene"] = current_scene.scene_file_path
	data["hud"] = viewer.timing.text
	samples.append(data)
	_check(data.get("fog_state", -1) == int(fog_on), label + ": fog state")
	_check(data.get("gi_state", -2) == gi_state, label + ": GI state")
	if fog_on: _check(data.get("fog_ms", -1.0) > 0.0, label + ": missing native fog GPU timestamp")
	else: _check(data.get("fog_ms", -1.0) < 0.0, label + ": stale fog timing after disabling")
	if gi_state == 1:
		_check(data.get("gi_ms", -1.0) > 0.0, label + ": missing TG GI GPU timestamp")
	else:
		_check(data.get("gi_ms", -1.0) < 0.0, label + ": stale GI timing")
	var gpu_sum: float = maxf(data.get("fog_ms", 0.0), 0.0) + maxf(data.get("gi_ms", 0.0), 0.0)
	_check(gpu_sum <= data.get("viewport_ms", 0.0) * 1.3, label + ": effect times exceed viewport (units/scope error)")
	_check(root.get_texture().get_image().save_png(output.path_join(label + ".png")) == OK, label + ": screenshot write failed")

func _run() -> void:
	if RenderingServer.get_rendering_device() == null:
		push_error("Viewer GPU validation requires Vulkan; headless is not supported.")
		quit(2)
		return
	DirAccess.make_dir_recursive_absolute(output)
	viewer = root.get_node("DemoViewer")
	viewer.force_visible = true
	change_scene_to_file("res://scenes/city.tscn")
	await scene_changed
	await _settle()
	_snapshot("01_city", true, -1)
	_key(KEY_F)
	await _settle()
	_snapshot("02_city_fog_off", false, -1)
	_key(KEY_F)
	_key(KEY_2)
	await _settle()
	_snapshot("02b_city_balanced", true, -1)
	_check(current_scene.atmosphere.fog_length >= current_scene._required_fog_length(), "street preset covers city")
	if await _switch(KEY_F2, "res://scenes/sponza_atmosphere.tscn"):
		_snapshot("03_sponza", true, 1)
		_check(not current_scene.atmosphere.environment.fog_enabled, "city distant fog does not leak into Sponza")
		_key(KEY_1)
		await _settle()
		_snapshot("04_sponza_gi_off", true, 0)
		_key(KEY_F)
		await _settle()
		_snapshot("05_sponza_both_off", false, 0)
		_key(KEY_2)
		_key(KEY_F)
		await _settle()
		_snapshot("06_sponza_restored", true, 1)
	if await _switch(KEY_F3, "res://scenes/atmosphere.tscn"):
		_snapshot("07_courtyard", true, -1)
	if await _switch(KEY_F1, "res://scenes/city.tscn"):
		_snapshot("08_city_return", true, -1)
	if await _switch(KEY_F2, "res://scenes/sponza_atmosphere.tscn"):
		_snapshot("09_sponza_reload", true, 1)
	if await _switch(KEY_F1, "res://scenes/city.tscn"):
		_snapshot("10_final_city", true, -1)
	var report := {"passed": failures.is_empty(), "gpu": RenderingServer.get_video_adapter_name(), "samples": samples, "failures": failures}
	FileAccess.open(output.path_join("results.json"), FileAccess.WRITE).store_string(JSON.stringify(report, "\t"))
	print("VIEWER_RESULT: ", JSON.stringify(report))
	quit(0 if failures.is_empty() else 1)

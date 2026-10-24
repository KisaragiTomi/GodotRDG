extends CanvasLayer
## Shared scene navigation and render-thread GPU timestamp readback.
## Fog includes native integration and optional surface composition; GI includes history.

const SCENES := ["res://scenes/city.tscn", "res://scenes/sponza_atmosphere.tscn", "res://scenes/atmosphere.tscn"]
const TITLES := ["City", "Sponza", "Courtyard"]

var timing: Label
var navigation: Label
var latest: Dictionary = {}
var busy := false
var capture_mode := false
var force_visible := false
var _panel: PanelContainer
var _scene: WeakRef
var _fog: WeakRef
var _gi: WeakRef
var _title := ""
var _message := ""
var _hidden := false
var _generation := 0
var _pending_read := false
var _last_gpu_frame := -1
var _discard_frames := 3
var _state := Vector2i(-1, -1)
var _last_usec := 0
var _elapsed_ms := 0.0
var _frames := 0
var _samples: Array[Dictionary] = []
var _viewport_cpu_sum := 0.0
var _viewport_gpu_sum := 0.0

func _ready() -> void:
	layer = 100
	for arg in OS.get_cmdline_user_args():
		if arg.contains("capture") or arg.begins_with("--validate-viewer"): capture_mode = true
	_panel = PanelContainer.new()
	_panel.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_LEFT)
	_panel.grow_vertical = Control.GROW_DIRECTION_BEGIN
	_panel.offset_left = 12
	_panel.offset_right = 560
	_panel.offset_top = -166
	_panel.offset_bottom = -12
	_panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_panel)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.018, 0.026, 0.04, 0.93)
	style.set_content_margin_all(12)
	style.set_corner_radius_all(6)
	_panel.add_theme_stylebox_override("panel", style)
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 5)
	_panel.add_child(column)
	navigation = Label.new()
	navigation.add_theme_font_size_override("font_size", 15)
	navigation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	column.add_child(navigation)
	column.add_child(HSeparator.new())
	timing = Label.new()
	timing.add_theme_font_size_override("font_size", 16)
	timing.add_theme_color_override("font_color", Color(1.0, 0.82, 0.5))
	column.add_child(timing)
	_panel.visible = false
	RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)

func configure(scene: Node, title: String, fog: Node, gi: Resource = null) -> void:
	_scene = weakref(scene)
	_fog = weakref(fog) if fog != null else null
	_gi = weakref(gi) if gi != null else null
	_title = title
	_message = ""
	_hidden = false
	_state = Vector2i(-1, -1)
	_reset_samples()
	_refresh_text()

func _reset_samples() -> void:
	_generation += 1
	_samples.clear()
	latest.clear()
	_discard_frames = 3
	_elapsed_ms = 0.0
	_frames = 0
	_viewport_cpu_sum = 0.0
	_viewport_gpu_sum = 0.0
	_last_usec = 0

func _process(_delta: float) -> void:
	var scene = _scene.get_ref() if _scene != null else null
	_panel.visible = not _hidden and (not capture_mode or force_visible) and (is_instance_valid(scene) or busy or not _message.is_empty())
	if busy or not is_instance_valid(scene) or not scene.is_inside_tree(): return
	var fog = _fog.get_ref() if _fog != null else null
	var gi = _gi.get_ref() if _gi != null else null
	var state := Vector2i(int(is_instance_valid(fog) and fog.environment.volumetric_fog_enabled), -1 if gi == null else int(scene.mode != 0))
	if state != _state:
		_state = state
		_reset_samples()
		_refresh_text()
	if not _pending_read and RenderingServer.get_rendering_device() != null:
		_pending_read = true
		RenderingServer.call_on_render_thread(_read_timestamps.bind(_generation, get_viewport().get_viewport_rid().get_id()))
	var now := Time.get_ticks_usec()
	if _last_usec == 0:
		_last_usec = now
		return
	_elapsed_ms += float(now - _last_usec) / 1000.0
	_last_usec = now
	_frames += 1
	var viewport_rid := get_viewport().get_viewport_rid()
	_viewport_cpu_sum += RenderingServer.viewport_get_measured_render_time_cpu(viewport_rid)
	_viewport_gpu_sum += RenderingServer.viewport_get_measured_render_time_gpu(viewport_rid)
	if _elapsed_ms < 500.0: return
	latest["fps"] = _frames * 1000.0 / _elapsed_ms
	latest["frame_ms"] = _elapsed_ms / _frames
	latest["cpu_ms"] = _viewport_cpu_sum / _frames
	latest["viewport_ms"] = _viewport_gpu_sum / _frames
	for key in ["fog_ms", "gi_ms"]:
		var total := 0.0
		var count := 0
		for sample in _samples:
			if sample[key] >= 0.0:
				total += sample[key]
				count += 1
		latest[key] = total / count if count > 0 else -1.0
	latest["gpu_samples"] = _samples.size()
	latest["fog_state"] = _state.x
	latest["gi_state"] = _state.y
	_refresh_text()
	_samples.clear()
	_elapsed_ms = 0.0
	_frames = 0
	_viewport_cpu_sum = 0.0
	_viewport_gpu_sum = 0.0

func _read_timestamps(generation: int, viewport_id: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	var result := {"frame": -1, "fog_ms": -1.0, "gi_ms": -1.0}
	if rd != null:
		result.frame = rd.get_captured_timestamps_frame()
		var inside := false
		var stamps := {}
		for index in rd.get_captured_timestamps_count():
			var label := rd.get_captured_timestamp_name(index)
			if label == "vp_begin_%d" % viewport_id: inside = true
			if label == "vp_end_%d" % viewport_id: inside = false
			if inside and (label in ["> Volumetric Fog", "< Volumetric Fog", "TG GI begin", "TG GI end", "TG pass history begin", "TG pass history end", "TG pass fog_composite begin", "TG pass fog_composite end"]):
				stamps[label] = rd.get_captured_timestamp_gpu_time(index)
		# RenderingDevice driver results are nanoseconds, as used by RenderingServer.
		if stamps.has("> Volumetric Fog") and stamps.has("< Volumetric Fog"):
			result.fog_ms = float(stamps["< Volumetric Fog"] - stamps["> Volumetric Fog"]) / 1000000.0
			if stamps.has("TG pass fog_composite begin") and stamps.has("TG pass fog_composite end"):
				result.fog_ms += float(stamps["TG pass fog_composite end"] - stamps["TG pass fog_composite begin"]) / 1000000.0
		if stamps.has("TG GI begin") and stamps.has("TG GI end") and stamps.has("TG pass history begin") and stamps.has("TG pass history end"):
			# History is after opaque rendering; do not charge the opaque pass to GI.
			result.gi_ms = float(stamps["TG GI end"] - stamps["TG GI begin"] + stamps["TG pass history end"] - stamps["TG pass history begin"]) / 1000000.0
	_accept_timestamps.call_deferred(generation, result)

func _accept_timestamps(generation: int, result: Dictionary) -> void:
	_pending_read = false
	if generation != _generation or result.frame == _last_gpu_frame or result.frame < 0: return
	_last_gpu_frame = result.frame
	# GPU queries are delayed; discard old scene/toggle results before averaging.
	if _discard_frames > 0:
		_discard_frames -= 1
		return
	_samples.append(result)

func _refresh_text() -> void:
	navigation.text = "F1 City   F2 Sponza (GI + Fog)   F3 Courtyard\n%s" % (_message if not _message.is_empty() else _title)
	var fog_text := "OFF" if _state.x == 0 else ("%.3f ms" % latest.fog_ms if latest.get("fog_ms", -1.0) >= 0.0 else "-- (waiting)")
	var gi_text := "N/A (not enabled)" if _state.y < 0 else ("OFF" if _state.y == 0 else ("%.3f ms" % latest.gi_ms if latest.get("gi_ms", -1.0) >= 0.0 else "-- (waiting)"))
	timing.text = "Fog GPU  %s    GI GPU  %s\n" % [fog_text, gi_text]
	if latest.has("fps"):
		timing.text += "FPS %.1f   Frame %.2f ms   CPU %.2f ms   GPU %.2f ms\n" % [latest.fps, latest.frame_ms, latest.cpu_ms, latest.viewport_ms]
	else:
		timing.text += "FPS --   Frame -- ms   CPU -- ms   GPU -- ms\n"
	timing.text += "0.5 s avg | CPU/GPU: viewport | Frame includes pacing"

func _input(event: InputEvent) -> void:
	if not event is InputEventKey or not event.pressed or event.echo: return
	if event.physical_keycode in [KEY_F1, KEY_F2, KEY_F3]:
		get_viewport().set_input_as_handled()
		if not busy: switch_scene.call_deferred(event.physical_keycode - KEY_F1)
	elif event.physical_keycode == KEY_H:
		_hidden = not _hidden

func switch_scene(index: int) -> void:
	if busy or index < 0 or index >= SCENES.size(): return
	if get_tree().current_scene != null and get_tree().current_scene.scene_file_path == SCENES[index]: return
	if index == 1:
		var missing := _missing_sponza_asset()
		if not missing.is_empty():
			_message = "Sponza unavailable: run download-sponza.py + prepare-sponza.ps1"
			_refresh_text()
			return
	busy = true
	_reset_samples()
	_message = "Loading %s..." % TITLES[index]
	_refresh_text()
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	var error := ResourceLoader.load_threaded_request(SCENES[index], "PackedScene")
	if error == OK:
		while ResourceLoader.load_threaded_get_status(SCENES[index]) == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
			await get_tree().process_frame
		if ResourceLoader.load_threaded_get_status(SCENES[index]) == ResourceLoader.THREAD_LOAD_LOADED:
			var packed := ResourceLoader.load_threaded_get(SCENES[index]) as PackedScene
			error = get_tree().change_scene_to_packed(packed)
			if error == OK:
				await get_tree().scene_changed
				# Sponza creates GPU buffers asynchronously before accepting input.
				if index == 1:
					while is_instance_valid(get_tree().current_scene) and not get_tree().current_scene.ready_for_input:
						await get_tree().process_frame
		else:
			error = ERR_CANT_OPEN
	busy = false
	_message = "" if error == OK else "Scene could not be loaded (%s)" % error_string(error)
	_refresh_text()

func _missing_sponza_asset() -> String:
	if not ResourceLoader.exists("res://assets/sponza/glTF/Sponza.gltf"): return "Sponza.gltf"
	for file in ["cache.json", "bvh.bin", "triangles.bin"]:
		if not FileAccess.file_exists(TGScreenGI.get_default_geometry_root() + file): return file
	var metadata = JSON.parse_string(FileAccess.get_file_as_string(TGScreenGI.get_default_geometry_root() + "cache.json"))
	if not metadata is Dictionary or not metadata.has("materials"): return "cache.json"
	for index in metadata.materials.size():
		for prefix in ["albedo", "metal"]:
			var file := "%s_%d.png" % [prefix, index]
			if not FileAccess.file_exists(TGScreenGI.get_default_geometry_root() + file): return file
	return ""

extends Node3D
## RDG demo: a dark scene with moving emissive spheres. The post process (bloom, light trails, composite)
## is an RDGCompositorEffect, i.e. UE-style FRDGBuilder code running inside Godot's Forward+ frame.
##
## Keys: 1 debug view | T trails | U unused debug pass | C r.RDG.CullPasses | A r.RDG.TransientAllocator
##       I r.RDG.ImmediateMode | M r.RDG.MergeRenderPasses | Y r.RDG.AsyncCompute | P r.RDG.ParallelExecute
##       R r.RHICmd.ParallelTranslate.Enable | G dump graph | +/- bloom intensity

const MEASURE_WARMUP_FRAMES := 120
const MEASURE_BLOCK_FRAMES := 60
const MEASURE_SETTLE_FRAMES := 5

var effect := RDGCompositorEffect.new()
var orbs: Array[MeshInstance3D] = []
var camera: Camera3D
var label: Label
var time := 0.0
## Automation: `-- --screenshot=<path> [--frames=N] [--trail] [--debug-view=N] [--dump-graph]`
var screenshot_path := ""
var screenshot_frames := 90
var frames := 0
## `-- --measure-translate[=blocks]`: alternates serial / parallel translate (r.RHICmd.ParallelTranslate.Enable) in
## blocks of frames, prints the CPU time of the RenderingDevice's command graph translation for both, then quits.
var measure_blocks := 0
var measure_samples := [[], []]


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--screenshot="):
			screenshot_path = arg.trim_prefix("--screenshot=")
		elif arg.begins_with("--frames="):
			screenshot_frames = int(arg.trim_prefix("--frames="))
		elif arg == "--trail":
			effect.trail_enabled = true
		elif arg.begins_with("--debug-view="):
			effect.debug_view = int(arg.trim_prefix("--debug-view="))
		elif arg == "--dump-graph":
			RenderGraphConsole.set_cvar("r.RDG.DumpGraph", 2)
		elif arg.begins_with("--measure-translate"):
			measure_blocks = int(arg.trim_prefix("--measure-translate=")) if arg.contains("=") else 8
	_build_environment()
	_build_scene()
	_build_ui()


func _build_environment() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.015, 0.017, 0.025)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.25, 0.27, 0.32)
	environment.ambient_light_energy = 0.6
	environment.tonemap_mode = Environment.TONE_MAPPER_ACES
	environment.glow_enabled = false # Bloom comes from the RDG effect.

	var compositor := Compositor.new()
	compositor.compositor_effects = [effect]

	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	world_environment.compositor = compositor
	add_child(world_environment)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 35, 0)
	sun.light_energy = 0.35
	sun.shadow_enabled = true
	add_child(sun)

	camera = Camera3D.new()
	camera.fov = 60
	add_child(camera)


func _build_scene() -> void:
	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	floor_mesh.mesh = plane
	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_color = Color(0.12, 0.12, 0.14)
	floor_material.roughness = 0.35
	floor_material.metallic = 0.2
	floor_mesh.material_override = floor_material
	add_child(floor_mesh)

	for i in 6:
		var pillar := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(0.8, 2.5 + i * 0.4, 0.8)
		pillar.mesh = box
		var angle := TAU * i / 6.0
		pillar.position = Vector3(cos(angle) * 7.0, box.size.y * 0.5, sin(angle) * 7.0)
		var pillar_material := StandardMaterial3D.new()
		pillar_material.albedo_color = Color(0.5, 0.52, 0.58)
		pillar.material_override = pillar_material
		add_child(pillar)

	var colors := [Color(1.0, 0.35, 0.1), Color(0.2, 0.6, 1.0), Color(0.9, 0.2, 0.9), Color(0.3, 1.0, 0.4), Color(1.0, 0.85, 0.3)]
	for i in colors.size():
		var orb := MeshInstance3D.new()
		var sphere := SphereMesh.new()
		sphere.radius = 0.35
		sphere.height = 0.7
		orb.mesh = sphere
		var orb_material := StandardMaterial3D.new()
		orb_material.albedo_color = colors[i]
		orb_material.emission_enabled = true
		orb_material.emission = colors[i]
		orb_material.emission_energy_multiplier = 6.0
		orb.material_override = orb_material
		add_child(orb)
		orbs.append(orb)

		var light := OmniLight3D.new()
		light.light_color = colors[i]
		light.light_energy = 1.5
		light.omni_range = 5.0
		orb.add_child(light)


func _build_ui() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)
	var panel := PanelContainer.new()
	panel.position = Vector2(12, 12)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0, 0, 0, 0.6)
	style.set_content_margin_all(10)
	panel.add_theme_stylebox_override("panel", style)
	layer.add_child(panel)
	label = Label.new()
	label.add_theme_font_size_override("font_size", 14)
	panel.add_child(label)


func _process(delta: float) -> void:
	time += delta
	for i in orbs.size():
		var speed := 0.6 + i * 0.17
		var radius := 2.0 + i * 0.9
		var phase := time * speed + i * 1.3
		orbs[i].position = Vector3(cos(phase) * radius, 0.9 + sin(time * 1.7 + i) * 0.5, sin(phase) * radius)

	var cam_angle := time * 0.15
	camera.position = Vector3(cos(cam_angle) * 12.0, 5.5, sin(cam_angle) * 12.0)
	camera.look_at(Vector3(0, 1, 0))
	_update_label()

	frames += 1
	if measure_blocks > 0:
		_measure_translate()
	if not screenshot_path.is_empty() and frames == screenshot_frames:
		await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		image.save_png(screenshot_path)
		print("RDG demo stats: ", effect.get_last_stats())
		print("Screenshot saved to ", screenshot_path)
		get_tree().quit()


func _measure_translate() -> void:
	var frame := frames - MEASURE_WARMUP_FRAMES
	if frame < 0:
		return
	@warning_ignore("integer_division")
	var block := frame / MEASURE_BLOCK_FRAMES
	var parallel := block % 2
	if frame % MEASURE_BLOCK_FRAMES == 0:
		RenderGraphConsole.set_cvar("r.RHICmd.ParallelTranslate.Enable", parallel)
	var stats := effect.get_last_stats()
	if frame % MEASURE_BLOCK_FRAMES >= MEASURE_SETTLE_FRAMES and not stats.is_empty() and stats.parallel_translate == (parallel == 1):
		measure_samples[parallel].append([stats.translate_ms, stats.translate_tasks, stats.translate_commands])
	if block < measure_blocks:
		return
	for mode in 2:
		var samples: Array = measure_samples[mode]
		var times: Array = samples.map(func(sample: Array) -> float: return sample[0])
		times.sort()
		var total := 0.0
		for value: float in times:
			total += value
		var last: Array = samples[-1] if not samples.is_empty() else [0.0, 0, 0]
		@warning_ignore("integer_division")
		var median: float = times[times.size() / 2] if not times.is_empty() else 0.0
		print("RDG demo translate %s: %d frames, mean %.3f ms, median %.3f ms, %d commands, %d worker tasks" % [["serial", "parallel"][mode], times.size(), total / maxi(times.size(), 1), median, last[2], last[1]])
	get_tree().quit()


func _update_label() -> void:
	var stats := effect.get_last_stats()
	var text := "Godot RDG (UE-style Render Dependency Graph)\n"
	if stats.is_empty():
		label.text = text + "waiting for the first graph..."
		return
	text += "%s\n" % stats.graph
	text += "passes: %d   culled: %d   merged render passes: %d\n" % [stats.passes, stats.culled_passes, stats.merged_render_passes]
	text += "textures: %d   transient: %d -> RD textures: %d   extracted: %d\n" % [stats.textures, stats.transient_textures, stats.physical_textures, stats.extracted_textures]
	text += "buffers: %d (transient allocated: %d)\n" % [stats.buffers, stats.transient_buffers]
	text += "transient heap: %d textures + %d buffers placed, %.1f MB allocated -> %.1f MB peak (%d heaps, %.0f MB)\n" % [stats.placed_textures, stats.placed_buffers, stats.transient_allocated_mb, stats.transient_peak_mb, stats.transient_heaps, stats.transient_heap_mb]
	text += "pool: %d textures, %.1f MB   compiled shaders: %d\n" % [stats.pool_textures, stats.pool_texture_mb, stats.compiled_shaders]
	text += "binding errors: %d   validation errors: %d   immediate mode: %s\n" % [stats.binding_errors, stats.validation_errors, str(stats.immediate_mode)]
	text += "async compute: %d passes on the async compute queue (%s)\n" % [stats.async_compute_passes, "enabled" if stats.async_compute else "unavailable or disabled"]
	text += "parallel execute: %d passes recorded by %d tasks (%s)\n" % [stats.parallel_passes, stats.parallel_pass_sets, "enabled" if stats.parallel_execute else "disabled"]
	text += "parallel setup: %d passes set up by the async setup queue task (%s)\n" % [stats.async_setup_passes, "enabled" if stats.parallel_setup else "disabled"]
	text += "RenderingDevice translate: %.3f ms for %d commands, %d worker tasks (parallel translate %s)\n\n" % [stats.translate_ms, stats.translate_commands, stats.translate_tasks, _on_off(stats.parallel_translate)]
	text += "[1] debug view: %s   [T] trails: %s   [U] unused debug pass: %s\n" % [["none", "bloom", "bloom setup"][effect.debug_view], _on_off(effect.trail_enabled), _on_off(effect.add_unused_debug_pass)]
	text += "[C] r.RDG.CullPasses=%d   [A] r.RDG.TransientAllocator=%d   [M] r.RDG.MergeRenderPasses=%d\n" % [RenderGraphConsole.get_cvar("r.RDG.CullPasses"), RenderGraphConsole.get_cvar("r.RDG.TransientAllocator"), RenderGraphConsole.get_cvar("r.RDG.MergeRenderPasses")]
	text += "[I] r.RDG.ImmediateMode=%d   [Y] r.RDG.AsyncCompute=%d   [P] r.RDG.ParallelExecute=%d\n" % [RenderGraphConsole.get_cvar("r.RDG.ImmediateMode"), RenderGraphConsole.get_cvar("r.RDG.AsyncCompute"), RenderGraphConsole.get_cvar("r.RDG.ParallelExecute")]
	text += "[R] r.RHICmd.ParallelTranslate.Enable=%d   [G] dump graph   [+/-] bloom intensity %.2f" % [RenderGraphConsole.get_cvar("r.RHICmd.ParallelTranslate.Enable"), effect.bloom_intensity]
	var dump := RenderGraphConsole.get_last_dump_path()
	if not dump.is_empty():
		text += "\nlast dump: %s" % ProjectSettings.globalize_path(dump)
	label.text = text


func _on_off(value: bool) -> String:
	return "on" if value else "off"


func _toggle_cvar(name: String) -> void:
	RenderGraphConsole.set_cvar(name, 0 if RenderGraphConsole.get_cvar(name) != 0 else 1)


func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey and event.pressed and not event.echo):
		return
	match event.keycode:
		KEY_1:
			effect.debug_view = (effect.debug_view + 1) % 3
		KEY_T:
			effect.trail_enabled = not effect.trail_enabled
		KEY_U:
			effect.add_unused_debug_pass = not effect.add_unused_debug_pass
		KEY_C:
			_toggle_cvar("r.RDG.CullPasses")
		KEY_A:
			_toggle_cvar("r.RDG.TransientAllocator")
		KEY_I:
			_toggle_cvar("r.RDG.ImmediateMode")
		KEY_M:
			_toggle_cvar("r.RDG.MergeRenderPasses")
		KEY_Y:
			_toggle_cvar("r.RDG.AsyncCompute")
		KEY_P:
			_toggle_cvar("r.RDG.ParallelExecute")
		KEY_R:
			RenderGraphConsole.set_cvar("r.RHICmd.ParallelTranslate.Enable", 0 if effect.get_last_stats().get("parallel_translate", false) else 1)
		KEY_G:
			RenderGraphConsole.set_cvar("r.RDG.DumpGraph", 2)
		KEY_EQUAL, KEY_KP_ADD:
			effect.bloom_intensity = minf(effect.bloom_intensity + 0.1, 4.0)
		KEY_MINUS, KEY_KP_SUBTRACT:
			effect.bloom_intensity = maxf(effect.bloom_intensity - 0.1, 0.0)

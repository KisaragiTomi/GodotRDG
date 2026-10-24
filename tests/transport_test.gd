extends SceneTree
## GPU regression: a 4 cm wall deliberately cuts between atlas sample centers.
## A fully enclosed room removes legitimate paths around/over the divider.

var cache: SurfaceCache
var layout: SurfaceLayout
var received: Image
var report: Dictionary = {}

func _initialize() -> void:
	_run.call_deferred()

func _box(label: String, center: Vector3, size: Vector3) -> Dictionary:
	return {"name": label, "center": center, "size": size, "color": Color(0.7, 0.7, 0.7)}

func _geometry(partition: bool) -> Array[Dictionary]:
	var boxes: Array[Dictionary] = [
		_box("Ground", Vector3(0, -0.2, 0), Vector3(8.4, 0.4, 6.4)),
		_box("Roof", Vector3(0, 2.7, 0), Vector3(8.4, 0.4, 6.4)),
		_box("Left", Vector3(-4.1, 1.25, 0), Vector3(0.2, 2.5, 6.4)),
		_box("Right", Vector3(4.1, 1.25, 0), Vector3(0.2, 2.5, 6.4)),
		_box("Back", Vector3(0, 1.25, -3.1), Vector3(8.4, 2.5, 0.2)),
		_box("Front", Vector3(0, 1.25, 3.1), Vector3(8.4, 2.5, 0.2)),
	]
	if partition:
		boxes.append(_box("OffGridThinWall", Vector3(0.15, 1.25, 0), Vector3(0.04, 2.5, 6.0)))
	return boxes

func _light(x: float, energy: float = 5.0) -> PackedFloat32Array:
	return PackedFloat32Array([x, 1.4, 0, 12, energy, energy, energy, 1.5, 0, 0, 0, 0])

func _start(partition: bool) -> void:
	layout = SurfaceLayout.new()
	layout.build(_geometry(partition), 0.25, 0.5)
	cache = SurfaceCache.new()
	cache.failed.connect(func(message: String): push_error(message); quit(1))
	cache.initialize(layout, _light(-2), 256)
	await cache.ready_for_use

func _atlas() -> Image:
	for frame in 5:
		cache.poll_gpu_timing()
		await process_frame
	received = null
	cache.read_atlas(func(image: Image): received = image)
	while received == null:
		await process_frame
	return received

func _regions(image: Image) -> Dictionary:
	return {
		"left": CacheValidation.region_stats(layout, image, AABB(Vector3(-3.99, -0.01, -2.99), Vector3(4.10, 0.02, 5.98))),
		"right": CacheValidation.region_stats(layout, image, AABB(Vector3(0.18, -0.01, -2.99), Vector3(3.81, 0.02, 5.98))),
	}

func _run() -> void:
	await _start(true)
	var left_lit := _regions(await _atlas())
	report.left_lamp = left_lit
	report.sealed_right_dark = left_lit.right.luminance_max <= 0.000001 and left_lit.left.luminance_mean > 0.001
	cache.update_lights(_light(2.0))
	await cache.update_complete
	var right_lit := _regions(await _atlas())
	report.right_lamp = right_lit
	report.sealed_left_dark = right_lit.left.luminance_max <= 0.000001 and right_lit.right.luminance_mean > 0.001
	cache.update_lights(_light(2.0, 0.0))
	await cache.update_complete
	var dark := CacheValidation.summarize(layout, await _atlas())
	report.no_history = dark.luminance_max == 0.0
	# Activate a previously non-emissive wall after initialization. The first
	# activation builds area visibility lazily; changing source faces reuses it.
	layout.set_emission(2, Color.WHITE, 3.0)
	cache.update_materials(layout.face_data, _light(0.0, 0.0))
	await cache.update_complete
	var emissive_left := _regions(await _atlas())
	report.emissive_left_lamp = emissive_left
	report.emissive_right_dark = emissive_left.right.luminance_max <= 0.000001 and emissive_left.left.luminance_mean > 0.001
	layout.set_emission(2, Color.WHITE, 0.0)
	layout.set_emission(3, Color.WHITE, 3.0)
	cache.update_materials(layout.face_data, _light(0.0, 0.0))
	await cache.update_complete
	var emissive_right := _regions(await _atlas())
	report.emissive_right_lamp = emissive_right
	report.emissive_left_dark = emissive_right.left.luminance_max <= 0.000001 and emissive_right.right.luminance_mean > 0.001
	report.area_visibility_reused = int(cache.stats.emitter_transfer_builds) == 1
	cache.shutdown()
	await process_frame
	await _start(false)
	var open := _regions(await _atlas())
	report.partition_removed = open
	report.open_room_transmits = open.right.luminance_mean > 0.001
	report.passed = report.sealed_right_dark and report.sealed_left_dark and report.no_history and report.open_room_transmits and report.emissive_right_dark and report.emissive_left_dark and report.area_visibility_reused
	var folder := ProjectSettings.globalize_path("res://artifacts/validation")
	DirAccess.make_dir_recursive_absolute(folder)
	FileAccess.open(folder.path_join("transport_test.json"), FileAccess.WRITE).store_string(JSON.stringify(report, "\t"))
	print("TRANSPORT_TEST ", JSON.stringify(report))
	cache.shutdown()
	await process_frame
	quit(0 if report.passed else 1)

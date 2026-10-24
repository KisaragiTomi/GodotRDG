extends SceneTree
## Physical sphere falloff plus sealed-wall visibility, before material response.
var layout: SurfaceLayout
var cache: SurfaceCache
var received: Image

func _initialize() -> void:
	_run.call_deferred()

func _box(label: String, center: Vector3, size: Vector3) -> Dictionary:
	return {"name": label, "center": center, "size": size, "color": Color.BLACK}

func _start(boxes: Array[Dictionary]) -> void:
	layout = SurfaceLayout.new()
	layout.build(boxes)
	cache = SurfaceCache.new()
	cache.failed.connect(func(message: String): push_error(message); quit(1))
	cache.initialize(layout, PackedFloat32Array(), 128)
	await cache.ready_for_use

func _source(position: Vector3, energy: float = 1.0) -> Image:
	cache.update_sphere(PackedFloat32Array([position.x, position.y, position.z, 0.3, energy, energy, energy, 0.0]), PackedFloat32Array())
	await cache.update_complete
	received = null
	cache.read_atlas(func(image: Image): received = image)
	while received == null:
		await process_frame
	return received

func _run() -> void:
	await _start([_box("Ground", Vector3(0, -0.2, 0), Vector3(8, 0.4, 6))])
	var position := Vector3(0, 1.5, 0)
	var full := await _source(position)
	var face: Dictionary = layout.faces[2]
	var maximum_error := 0.0
	for y in range(1, face.rect.size.y - 1, 3):
		for x in range(1, face.rect.size.x - 1, 3):
			var uv := (Vector2(x, y) + Vector2(0.5, 0.5)) / Vector2(face.rect.size)
			var point: Vector3 = face.origin + face.u * uv.x + face.v * uv.y
			var delta := position - point
			var expected := 0.3 * 0.3 * delta.y / pow(delta.length(), 3.0)
			var actual := full.get_pixelv(face.rect.position + Vector2i(x, y)).r
			maximum_error = maxf(maximum_error, absf(actual - expected) / expected)
	var half := await _source(position, 0.5)
	var full_energy := CacheValidation.summarize(layout, full).luminance_mean as float
	var half_energy := CacheValidation.summarize(layout, half).luminance_mean as float
	var dark := await _source(position, 0.0)
	var report := {"maximum_relative_error": maximum_error, "sphere_matches_analytic_falloff": maximum_error < 0.002, "linear_strength": absf(half_energy / full_energy - 0.5) < 0.002, "off_is_zero": CacheValidation.summarize(layout, dark).luminance_max == 0.0}
	cache.shutdown()
	await process_frame
	await _start([
		_box("Ground", Vector3(0, -0.2, 0), Vector3(8.4, 0.4, 6.4)),
		_box("Roof", Vector3(0, 2.7, 0), Vector3(8.4, 0.4, 6.4)),
		_box("Left", Vector3(-4.1, 1.25, 0), Vector3(0.2, 2.5, 6.4)),
		_box("Right", Vector3(4.1, 1.25, 0), Vector3(0.2, 2.5, 6.4)),
		_box("Back", Vector3(0, 1.25, -3.1), Vector3(8.4, 2.5, 0.2)),
		_box("Front", Vector3(0, 1.25, 3.1), Vector3(8.4, 2.5, 0.2)),
		_box("ThinWall", Vector3(0.15, 1.25, 0), Vector3(0.04, 2.5, 6.0)),
	])
	var left_bounds := AABB(Vector3(-3.99, -0.01, -2.99), Vector3(4.10, 0.02, 5.98))
	var right_bounds := AABB(Vector3(0.18, -0.01, -2.99), Vector3(3.81, 0.02, 5.98))
	var left := await _source(Vector3(-2, 1.2, 0))
	report["sealed_right_dark"] = CacheValidation.region_stats(layout, left, right_bounds).luminance_max == 0.0 and CacheValidation.region_stats(layout, left, left_bounds).luminance_mean > 0.001
	var right := await _source(Vector3(2, 1.2, 0))
	report["sealed_left_dark"] = CacheValidation.region_stats(layout, right, left_bounds).luminance_max == 0.0 and CacheValidation.region_stats(layout, right, right_bounds).luminance_mean > 0.001
	report["move_reuses_transport"] = cache.stats.transport_builds == 1 and cache.stats.sphere_updates == 2
	report["passed"] = true
	for key in report:
		if report[key] is bool and not report[key]: report.passed = false
	var folder := ProjectSettings.globalize_path("res://artifacts/validation")
	DirAccess.make_dir_recursive_absolute(folder)
	FileAccess.open(folder.path_join("sphere_test.json"), FileAccess.WRITE).store_string(JSON.stringify(report, "\t"))
	print("SPHERE_TEST ", JSON.stringify(report))
	cache.shutdown()
	await process_frame
	quit(0 if report.passed else 1)

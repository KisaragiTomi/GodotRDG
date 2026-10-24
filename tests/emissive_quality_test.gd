extends SceneTree
## Independent radiometric regression: integrate the box's physical area on the
## CPU, then compare to the GPU's linear E / PI atlas (before tone mapping).
## Every surface is black, so a second reflected contribution must be zero.

const EMITTER_CENTER := Vector3(0, 2, 0)
const EMITTER_SIZE := Vector3(1.5, 0.1, 1.0)
const QUADRATURE_STEPS := 128
const GRID_SIZE := 7

var cache: SurfaceCache
var layout: SurfaceLayout
var received: Image
var report: Dictionary = {}

func _initialize() -> void:
	_run.call_deferred()

func _box(label: String, center: Vector3, size: Vector3) -> Dictionary:
	return {"name": label, "center": center, "size": size, "color": Color.BLACK}

func _emitter(center: Vector3, size: Vector3) -> Dictionary:
	var box := _box("Emitter", center, size)
	box.emission_color = Color.WHITE
	box.emission_energy = 1.0
	return box

func _start(boxes: Array[Dictionary]) -> void:
	layout = SurfaceLayout.new()
	layout.build(boxes, 0.25, 0.5)
	cache = SurfaceCache.new()
	cache.bounces = 2
	cache.failed.connect(func(message: String): push_error(message); quit(1))
	cache.initialize(layout, PackedFloat32Array(), 256)
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

func _set_energy(energy: float) -> Image:
	layout.set_emission(1, Color.WHITE, energy)
	if not cache.update_materials(layout.face_data, PackedFloat32Array()):
		push_error("The emissive update was unexpectedly rejected")
		quit(1)
		return null
	await cache.update_complete
	return await _atlas()

func _reference(receiver: Vector3, subdivisions: int = QUADRATURE_STEPS) -> float:
	# This reference deliberately does not use SurfaceLayout face descriptors,
	# hemisphere rays, visibility samples, or any GPU quadrature helper.
	# A convex box has no self-occlusion on a face oriented toward the receiver.
	var integral := 0.0
	for axis in 3:
		var axis_u := (axis + 1) % 3
		var axis_v := (axis + 2) % 3
		for sign_value in [-1.0, 1.0]:
			var normal := Vector3.ZERO
			normal[axis] = sign_value
			var face_center := EMITTER_CENTER
			face_center[axis] += sign_value * EMITTER_SIZE[axis] * 0.5
			if normal.dot(receiver - face_center) <= 0.0:
				continue
			var cell_area := EMITTER_SIZE[axis_u] * EMITTER_SIZE[axis_v] / float(subdivisions * subdivisions)
			for v in subdivisions:
				for u in subdivisions:
					var source := face_center
					source[axis_u] += ((float(u) + 0.5) / subdivisions - 0.5) * EMITTER_SIZE[axis_u]
					source[axis_v] += ((float(v) + 0.5) / subdivisions - 0.5) * EMITTER_SIZE[axis_v]
					var delta := source - receiver
					var distance_squared := delta.length_squared()
					# cos(receiver) * cos(emitter) / (PI * r^2), evaluated as
					# two unnormalized dot products / (PI * r^4).
					var projected := maxf(delta.y, 0.0) * maxf(-normal.dot(delta), 0.0)
					integral += projected * cell_area / (PI * distance_squared * distance_squared)
	return integral

func _quality(image: Image) -> Dictionary:
	var face: Dictionary = layout.faces[2]
	var rect: Rect2i = face.rect
	var samples: Array[Dictionary] = []
	var residuals: Array[float] = []
	var reference_peak := 0.0
	var relative_sum := 0.0
	var relative_max := 0.0
	var bias_sum := 0.0
	var reference_sum := 0.0
	var reference_convergence_max := 0.0
	for y in GRID_SIZE:
		for x in GRID_SIZE:
			var lattice := Vector2i(3 + x * 4, 3 + y * 4)
			var uv := (Vector2(lattice) + Vector2(0.5, 0.5)) / Vector2(rect.size)
			var point: Vector3 = face.origin + face.u * uv.x + face.v * uv.y
			var expected := _reference(point)
			var measured := image.get_pixelv(rect.position + lattice).r
			var residual := measured - expected
			var relative := absf(residual) / maxf(expected, 0.000001)
			# Check quadrature convergence at center and an off-axis corner.
			if (x == 0 and y == 0) or (x == GRID_SIZE / 2 and y == GRID_SIZE / 2):
				var coarse := _reference(point, QUADRATURE_STEPS / 2)
				reference_convergence_max = maxf(reference_convergence_max, absf(coarse - expected) / expected)
			reference_peak = maxf(reference_peak, expected)
			relative_sum += relative
			relative_max = maxf(relative_max, relative)
			bias_sum += residual
			reference_sum += expected
			residuals.append(residual)
			samples.append({"position": [point.x, point.y, point.z], "measured": measured, "reference": expected, "relative_error": relative})
	# Curvature of the residual removes the real falloff, detecting spatial noise
	# without demanding that physically varying irradiance itself be flat.
	var roughness_max := 0.0
	for y in GRID_SIZE:
		for x in GRID_SIZE:
			var index := y * GRID_SIZE + x
			if x > 0 and x + 1 < GRID_SIZE:
				roughness_max = maxf(roughness_max, absf(residuals[index - 1] - 2.0 * residuals[index] + residuals[index + 1]))
			if y > 0 and y + 1 < GRID_SIZE:
				roughness_max = maxf(roughness_max, absf(residuals[index - GRID_SIZE] - 2.0 * residuals[index] + residuals[index + GRID_SIZE]))
	return {
		"sample_count": samples.size(),
		"mean_relative_error": relative_sum / samples.size(),
		"maximum_relative_error": relative_max,
		"signed_energy_error": bias_sum / reference_sum,
		"residual_curvature_over_peak": roughness_max / reference_peak,
		"reference_convergence_relative": reference_convergence_max,
		"samples": samples,
	}

func _floor_update_error(full: Image, half: Image) -> Dictionary:
	var rect: Rect2i = layout.faces[2].rect
	var maximum_error := 0.0
	var maximum_full := 0.0
	for y in rect.size.y:
		for x in rect.size.x:
			var pixel := rect.position + Vector2i(x, y)
			var value := full.get_pixelv(pixel).r
			maximum_full = maxf(maximum_full, value)
			maximum_error = maxf(maximum_error, absf(half.get_pixelv(pixel).r * 2.0 - value))
	return {"maximum_absolute_error": maximum_error, "relative_to_peak": maximum_error / maxf(maximum_full, 0.000001)}

func _sealed_fixture() -> Array[Dictionary]:
	return [
		_box("Ground", Vector3(0, -0.2, 0), Vector3(8.4, 0.4, 6.4)),
		_emitter(Vector3(-2, 1.8, 0), Vector3(1.5, 0.1, 1.0)),
		_box("Roof", Vector3(0, 2.7, 0), Vector3(8.4, 0.4, 6.4)),
		_box("Left", Vector3(-4.1, 1.25, 0), Vector3(0.2, 2.5, 6.4)),
		_box("Right", Vector3(4.1, 1.25, 0), Vector3(0.2, 2.5, 6.4)),
		_box("Back", Vector3(0, 1.25, -3.1), Vector3(8.4, 2.5, 0.2)),
		_box("Front", Vector3(0, 1.25, 3.1), Vector3(8.4, 2.5, 0.2)),
		_box("OffGridThinWall", Vector3(0.15, 1.25, 0), Vector3(0.04, 2.5, 6.0)),
	]

func _run() -> void:
	await _start([
		_box("Ground", Vector3(0, -0.2, 0), Vector3(8, 0.4, 8)),
		_emitter(EMITTER_CENTER, EMITTER_SIZE),
	])
	var full := await _atlas()
	var quality := _quality(full)
	report.radiometric_reference = quality
	report.reference_converged = quality.reference_convergence_relative < 0.0001
	report.reference_accuracy = quality.mean_relative_error < 0.02 and quality.maximum_relative_error < 0.05
	report.spatial_smoothness = quality.residual_curvature_over_peak < 0.01
	var half := await _set_energy(0.5)
	var linearity := _floor_update_error(full, half)
	report.half_strength = linearity
	report.linear_response = linearity.relative_to_peak < 0.002
	var off := await _set_energy(0.0)
	var off_stats := CacheValidation.summarize(layout, off)
	report.off = off_stats
	report.no_history = off_stats.luminance_max == 0.0
	report.material_updates_reuse_transport = cache.stats.transport_builds == 1
	cache.shutdown()
	await process_frame
	await _start(_sealed_fixture())
	var sealed := await _atlas()
	var left := CacheValidation.region_stats(layout, sealed, AABB(Vector3(-3.99, -0.01, -2.99), Vector3(4.10, 0.02, 5.98)))
	var right := CacheValidation.region_stats(layout, sealed, AABB(Vector3(0.18, -0.01, -2.99), Vector3(3.81, 0.02, 5.98)))
	report.sealed_lit_side = left
	report.sealed_dark_side = right
	report.emission_respects_thin_wall = left.luminance_mean > 0.001 and right.luminance_max <= 0.000001
	report.passed = report.reference_converged and report.reference_accuracy and report.spatial_smoothness and report.linear_response and report.no_history and report.material_updates_reuse_transport and report.emission_respects_thin_wall
	var folder := ProjectSettings.globalize_path("res://artifacts/validation")
	DirAccess.make_dir_recursive_absolute(folder)
	FileAccess.open(folder.path_join("emissive_quality_test.json"), FileAccess.WRITE).store_string(JSON.stringify(report, "\t"))
	print("EMISSIVE_QUALITY_TEST ", JSON.stringify(report))
	cache.shutdown()
	await process_frame
	quit(0 if report.passed else 1)

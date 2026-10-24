extends SceneTree
## Compare reconstructed wall irradiance at sub-texel positions with an
## independent CPU area integral. Black albedo isolates the first light bounce.

const EMITTER_CENTER := Vector3(0.0, 2.0, 0.19)
const EMITTER_SIZE := Vector3(2.6, 1.8, 0.16)
const REFERENCE_STEPS := 512
const RECEIVER_FACE := 4

var cache: SurfaceCache
var received: Image
var points: Array[Vector3] = []
var references: Array[float] = []

func _initialize() -> void:
	_run.call_deferred()

func _box(label: String, center: Vector3, size: Vector3) -> Dictionary:
	return {"name": label, "center": center, "size": size, "color": Color.BLACK}

func _fixture() -> Array[Dictionary]:
	var emitter := _box("Panel", EMITTER_CENTER, EMITTER_SIZE)
	emitter.emission_color = Color.WHITE
	emitter.emission_energy = 1.0
	return [
		_box("Receiver", Vector3(0, 2, -0.1), Vector3(8, 4, 0.2)),
		emitter,
		_box("Ground", Vector3(0, -0.2, 2.5), Vector3(8, 0.4, 5)),
		_box("FarWall", Vector3(0, 2, 5.1), Vector3(8, 4, 0.2)),
	]

func _reference(receiver: Vector3, subdivisions: int = REFERENCE_STEPS) -> float:
	# Integrate physical box faces independently of the atlas and GPU helpers.
	# The receiver's outward normal is +Z. A convex emitting box has no
	# self-occlusion on a face that points toward the receiver.
	var integral := 0.0
	for axis in 3:
		var axis_u := (axis + 1) % 3
		var axis_v := (axis + 2) % 3
		for sign_value in [-1.0, 1.0]:
			var normal := Vector3.ZERO
			normal[axis] = sign_value
			var center := EMITTER_CENTER
			center[axis] += sign_value * EMITTER_SIZE[axis] * 0.5
			if normal.dot(receiver - center) <= 0.0:
				continue
			var cell_area := EMITTER_SIZE[axis_u] * EMITTER_SIZE[axis_v] / float(subdivisions * subdivisions)
			for y in subdivisions:
				for x in subdivisions:
					var source := center
					source[axis_u] += ((float(x) + 0.5) / subdivisions - 0.5) * EMITTER_SIZE[axis_u]
					source[axis_v] += ((float(y) + 0.5) / subdivisions - 0.5) * EMITTER_SIZE[axis_v]
					var delta := source - receiver
					var distance_squared := delta.length_squared()
					var projected := maxf(delta.z, 0.0) * maxf(-normal.dot(delta), 0.0)
					integral += projected * cell_area / (PI * distance_squared * distance_squared)
	return integral

func _sample(image: Image, face: Dictionary, point: Vector3) -> float:
	# Match the runtime's ordinary bilinear lookup. This fixture has no
	# chart-intersecting obstacles, so visibility rejection is unnecessary.
	var relative: Vector3 = point - face.origin
	var u: Vector3 = face.u
	var v: Vector3 = face.v
	var uv := Vector2(relative.dot(u) / u.length_squared(), relative.dot(v) / v.length_squared())
	var rect: Rect2i = face.rect
	var coordinate := (uv * Vector2(rect.size) - Vector2(0.5, 0.5)).clamp(Vector2.ZERO, Vector2(rect.size - Vector2i.ONE))
	var lower := Vector2i(coordinate.floor())
	var upper := (lower + Vector2i.ONE).min(rect.size - Vector2i.ONE)
	var fraction := coordinate - Vector2(lower)
	var bottom := lerpf(image.get_pixelv(rect.position + lower).r, image.get_pixelv(rect.position + Vector2i(upper.x, lower.y)).r, fraction.x)
	var top := lerpf(image.get_pixelv(rect.position + Vector2i(lower.x, upper.y)).r, image.get_pixelv(rect.position + upper).r, fraction.x)
	return lerpf(bottom, top, fraction.y)

func _measure(layout: SurfaceLayout) -> Dictionary:
	cache = SurfaceCache.new()
	cache.bounces = 2
	cache.failed.connect(func(message: String): push_error(message); quit(1))
	cache.initialize(layout, PackedFloat32Array(), 128)
	await cache.ready_for_use
	for frame in 3:
		await process_frame
	received = null
	cache.read_atlas(func(image: Image): received = image)
	while received == null:
		await process_frame
	var squared_error := 0.0
	var maximum_absolute_error := 0.0
	var samples: Array[Dictionary] = []
	var all_finite := true
	var face: Dictionary = layout.faces[RECEIVER_FACE]
	for index in points.size():
		var measured := _sample(received, face, points[index])
		var error := measured - references[index]
		all_finite = all_finite and is_finite(measured)
		squared_error += error * error
		maximum_absolute_error = maxf(maximum_absolute_error, absf(error))
		samples.append({"position": [points[index].x, points[index].y, points[index].z], "reference": references[index], "measured": measured})
	var result := {
		"rmse": sqrt(squared_error / points.size()),
		"maximum_absolute_error": maximum_absolute_error,
		"finite": all_finite,
		"surfel_count": layout.surfels.size() / 8,
		"receiver_resolution": [face.rect.size.x, face.rect.size.y],
		"samples": samples,
	}
	cache.shutdown()
	await process_frame
	return result

func _density_check(coarse: SurfaceLayout, refined: SurfaceLayout) -> Dictionary:
	var changed: Array[int] = []
	var preserves_default_spacing := true
	for index in refined.faces.size():
		var face: Dictionary = refined.faces[index]
		if face.rect.size != coarse.faces[index].rect.size:
			changed.append(index)
		if index == RECEIVER_FACE:
			continue
		var spacing := 0.25 if face.normal.y > 0.5 else 0.5
		var expected := Vector2i(ceili(face.u.length() / spacing), ceili(face.v.length() / spacing))
		preserves_default_spacing = preserves_default_spacing and face.rect.size == expected and not face.emitter_refined
	return {"changed_faces": changed, "only_near_receiver_refined": changed == [RECEIVER_FACE], "other_faces_keep_default_spacing": preserves_default_spacing}

func _run() -> void:
	# Fixed world positions, deliberately offset from either chart's texel centers.
	# Sample the visible halo on all four sides, including its steepest falloff.
	for edge_offset in [0.035, 0.085, 0.17]:
		for along in [-0.71, -0.33, 0.06, 0.38, 0.73]:
			for sign_value in [-1.0, 1.0]:
				points.append(Vector3(sign_value * (EMITTER_SIZE.x * 0.5 + edge_offset), EMITTER_CENTER.y + along, 0.0))
				points.append(Vector3(along * 1.4, EMITTER_CENTER.y + sign_value * (EMITTER_SIZE.y * 0.5 + edge_offset), 0.0))
	for point in points:
		references.append(_reference(point))
	var reference_convergence := 0.0
	for index in [0, 1, 17, 41, 59]:
		reference_convergence = maxf(reference_convergence, absf(references[index] - _reference(points[index], REFERENCE_STEPS / 2)))
	var coarse_layout := SurfaceLayout.new()
	coarse_layout.refine_emitter_receivers = false
	coarse_layout.build(_fixture(), 0.25, 0.5)
	var refined_layout := SurfaceLayout.new()
	refined_layout.refine_emitter_receivers = true
	refined_layout.build(_fixture(), 0.25, 0.5)
	var density := _density_check(coarse_layout, refined_layout)
	var coarse := await _measure(coarse_layout)
	var refined := await _measure(refined_layout)
	var report := {
		"sample_count": points.size(),
		"reference_convergence_maximum_absolute": reference_convergence,
		"coarse": coarse,
		"refined": refined,
		"density": density,
		"rmse_ratio": refined.rmse / maxf(coarse.rmse, 0.0000001),
		"reference_converged": reference_convergence < 0.0003,
		"interpolation_improved": refined.rmse < coarse.rmse * 0.65 and refined.maximum_absolute_error < coarse.maximum_absolute_error,
		"absolute_accuracy": refined.rmse < 0.04 and refined.maximum_absolute_error < 0.10,
	}
	report.passed = report.reference_converged and report.interpolation_improved and report.absolute_accuracy and coarse.finite and refined.finite and density.only_near_receiver_refined and density.other_faces_keep_default_spacing
	var folder := ProjectSettings.globalize_path("res://artifacts/validation")
	DirAccess.make_dir_recursive_absolute(folder)
	FileAccess.open(folder.path_join("emissive_detail_test.json"), FileAccess.WRITE).store_string(JSON.stringify(report, "\t"))
	print("EMISSIVE_DETAIL_TEST passed=%s samples=%d coarse_rmse=%.6f refined_rmse=%.6f refined_max=%.6f ratio=%.3f" % [report.passed, points.size(), coarse.rmse, refined.rmse, refined.maximum_absolute_error, report.rmse_ratio])
	quit(0 if report.passed else 1)

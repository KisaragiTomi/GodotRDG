class_name CacheValidation
extends RefCounted
## Readback metrics for physical failure modes, not an alternate implementation
## of the GPU solver. Camera invariance and lights-off reset compare actual output.

static func summarize(layout: SurfaceLayout, atlas: Image) -> Dictionary:
	if atlas.get_size() != layout.atlas_size:
		return {"error": "Atlas dimensions do not match the surface layout"}
	var totals := _empty_stats()
	var per_box: Array[Dictionary] = []
	for index in layout.boxes.size():
		var box_stats := _empty_stats()
		box_stats["name"] = str(layout.boxes[index].get("name", "box_%d" % index))
		box_stats["box_index"] = index
		per_box.append(box_stats)
	for sample in int(layout.surfels.size() / 8):
		var offset := sample * 8
		var face_index := int(layout.surfels[offset + 3])
		var box_index := int(layout.faces[face_index].box)
		var color := atlas.get_pixel(int(layout.surfels[offset + 4]), int(layout.surfels[offset + 5]))
		_record(totals, color)
		_record(per_box[box_index], color)
	var result := _finish_stats(totals)
	result["sha256"] = hash_image(atlas)
	result["atlas_size"] = [atlas.get_width(), atlas.get_height()]
	result["incident_units"] = "indirect irradiance divided by PI, scene linear"
	var box_results: Array[Dictionary] = []
	for index in per_box.size():
		var finished := _finish_stats(per_box[index])
		finished["name"] = per_box[index].name
		finished["box_index"] = index
		# This is a prediction from readback, NOT a screen/material shader test.
		var albedo: Color = layout.boxes[index].color.srgb_to_linear()
		var mean: Array = finished.rgb_mean
		finished["predicted_lambertian_radiance_rgb_mean"] = [mean[0] * albedo.r, mean[1] * albedo.g, mean[2] * albedo.b]
		box_results.append(finished)
	result["boxes"] = box_results
	return result

static func region_stats(layout: SurfaceLayout, atlas: Image, bounds: AABB, normal: Vector3 = Vector3.UP) -> Dictionary:
	## Select a world-space receiver region, e.g. floor behind a sealed thin wall.
	## The normal filter prevents a nearby opposite face contaminating the metric.
	var accumulator := _empty_stats()
	var region_normal := normal.normalized()
	for sample in int(layout.surfels.size() / 8):
		var offset := sample * 8
		var face: Dictionary = layout.faces[int(layout.surfels[offset + 3])]
		var face_normal: Vector3 = face.normal
		if face_normal.dot(region_normal) < 0.99:
			continue
		var position := Vector3(layout.surfels[offset], layout.surfels[offset + 1], layout.surfels[offset + 2])
		if not bounds.has_point(position):
			continue
		_record(accumulator, atlas.get_pixel(int(layout.surfels[offset + 4]), int(layout.surfels[offset + 5])))
	return _finish_stats(accumulator)

static func compare(layout: SurfaceLayout, before: Image, after: Image) -> Dictionary:
	## Useful acceptance checks:
	## - camera pan/zoom: identical=true, maximum_absolute_error=0;
	## - repeated unchanged lighting update: identical=true;
	## - lights off without emissive sources: summarize(after).rgb_max == [0,0,0];
	## - moved lamp: nonzero difference, followed by stable repeated result.
	if before.get_size() != layout.atlas_size or after.get_size() != layout.atlas_size:
		return {"error": "Atlas dimensions do not match the surface layout"}
	var maximum := 0.0
	var absolute_sum := 0.0
	var squared_sum := 0.0
	var changed := 0
	var nonfinite := 0
	var sample_count := int(layout.surfels.size() / 8)
	for sample in sample_count:
		var offset := sample * 8
		var x := int(layout.surfels[offset + 4])
		var y := int(layout.surfels[offset + 5])
		var a := before.get_pixel(x, y)
		var b := after.get_pixel(x, y)
		if not _finite(a) or not _finite(b):
			nonfinite += 1
			continue
		var difference := Vector3(absf(a.r - b.r), absf(a.g - b.g), absf(a.b - b.b))
		var local_maximum := maxf(difference.x, maxf(difference.y, difference.z))
		maximum = maxf(maximum, local_maximum)
		absolute_sum += difference.x + difference.y + difference.z
		squared_sum += difference.length_squared()
		if local_maximum > 0.0:
			changed += 1
	var components := float(maxi((sample_count - nonfinite) * 3, 1))
	return {
		"identical": before.get_data() == after.get_data(),
		"sample_count": sample_count,
		"changed_samples": changed,
		"nonfinite_samples": nonfinite,
		"maximum_absolute_error": maximum,
		"mean_absolute_error": absolute_sum / components,
		"root_mean_squared_error": sqrt(squared_sum / components),
		"before_sha256": hash_image(before),
		"after_sha256": hash_image(after),
	}

static func hash_image(atlas: Image) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(atlas.get_data())
	return context.finish().hex_encode()

static func _finite(color: Color) -> bool:
	return is_finite(color.r) and is_finite(color.g) and is_finite(color.b)

static func _empty_stats() -> Dictionary:
	return {
		"samples": 0, "finite_samples": 0, "nonfinite_samples": 0, "negative_samples": 0,
		"red_sum": 0.0, "green_sum": 0.0, "blue_sum": 0.0,
		"red_max": 0.0, "green_max": 0.0, "blue_max": 0.0,
		"luminance_sum": 0.0, "luminance_max": 0.0,
	}

static func _record(stats: Dictionary, color: Color) -> void:
	stats.samples += 1
	if not _finite(color):
		stats.nonfinite_samples += 1
		return
	stats.finite_samples += 1
	if color.r < 0.0 or color.g < 0.0 or color.b < 0.0:
		stats.negative_samples += 1
	stats.red_sum += color.r
	stats.green_sum += color.g
	stats.blue_sum += color.b
	stats.red_max = maxf(stats.red_max, color.r)
	stats.green_max = maxf(stats.green_max, color.g)
	stats.blue_max = maxf(stats.blue_max, color.b)
	var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
	stats.luminance_sum += luminance
	stats.luminance_max = maxf(stats.luminance_max, luminance)

static func _finish_stats(stats: Dictionary) -> Dictionary:
	var count := float(maxi(int(stats.finite_samples), 1))
	return {
		"samples": stats.samples,
		"nonfinite_samples": stats.nonfinite_samples,
		"negative_samples": stats.negative_samples,
		"rgb_mean": [stats.red_sum / count, stats.green_sum / count, stats.blue_sum / count],
		"rgb_max": [stats.red_max, stats.green_max, stats.blue_max],
		"luminance_mean": stats.luminance_sum / count,
		"luminance_max": stats.luminance_max,
	}

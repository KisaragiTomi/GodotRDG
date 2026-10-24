class_name SurfaceLayout
extends RefCounted
## Exact axis-aligned box surfaces. World-space layout is independent of the camera.
## Floor and horizontal walkable faces receive denser samples than walls.

const ATLAS_WIDTH := 256
const MAX_FACE_BLOCKERS := 32
const SURFACE_BIAS := 0.005
const FACE_FLOAT_COUNT := 28
const FACE_EMISSION_OFFSET := 20
const EMITTER_RECEIVER_SPACING := 0.125
const EMITTER_REFINEMENT_DISTANCE := 0.5
var boxes: Array[Dictionary] = []
var faces: Array[Dictionary] = []
var surfels := PackedFloat32Array()
var face_data := PackedFloat32Array()
var box_data := PackedFloat32Array()
var atlas_size := Vector2i(ATLAS_WIDTH, 1)
var floor_spacing := 0.25
var wall_spacing := 0.5
var refine_emitter_receivers := true
var _cursor := Vector2i.ZERO
var _row_height := 0

func build(source_boxes: Array[Dictionary], floor_step: float = 0.25, wall_step: float = 0.5) -> void:
	boxes = source_boxes.duplicate(true)
	floor_spacing = floor_step
	wall_spacing = wall_step
	faces.clear()
	surfels.clear()
	face_data.clear()
	box_data.clear()
	_cursor = Vector2i.ZERO
	_row_height = 0
	for box_index in boxes.size():
		var box: Dictionary = boxes[box_index]
		var center: Vector3 = box.center
		var size: Vector3 = box.size
		var lo := center - size * 0.5
		var hi := center + size * 0.5
		_append4(box_data, lo, float(faces.size()))
		_append4(box_data, hi, 0.0)
		# U x V is the outward normal. Shader / transfer face order is fixed.
		_add_face(box_index, Vector3(hi.x, lo.y, hi.z), Vector3(0, 0, -size.z), Vector3(0, size.y, 0), Vector3.RIGHT)
		_add_face(box_index, Vector3(lo.x, lo.y, lo.z), Vector3(0, 0, size.z), Vector3(0, size.y, 0), Vector3.LEFT)
		_add_face(box_index, Vector3(lo.x, hi.y, hi.z), Vector3(size.x, 0, 0), Vector3(0, 0, -size.z), Vector3.UP)
		_add_face(box_index, Vector3(lo.x, lo.y, lo.z), Vector3(size.x, 0, 0), Vector3(0, 0, size.z), Vector3.DOWN)
		_add_face(box_index, Vector3(lo.x, lo.y, hi.z), Vector3(size.x, 0, 0), Vector3(0, size.y, 0), Vector3.BACK)
		_add_face(box_index, Vector3(hi.x, lo.y, lo.z), Vector3(-size.x, 0, 0), Vector3(0, size.y, 0), Vector3.FORWARD)
	atlas_size.y = maxi(1, _cursor.y + _row_height)
	for face_index in faces.size():
		var face: Dictionary = faces[face_index]
		face["blockers"] = _face_blockers(face)
		face["blocker_overflow"] = face.blockers.size() > MAX_FACE_BLOCKERS
		if face.blocker_overflow:
			push_error("Surface cache face %d has %d UV blockers; limit is %d. Indirect shading on this face is disabled." % [face_index, face.blockers.size(), MAX_FACE_BLOCKERS])
		_append4(face_data, face.origin, 0.0)
		_append4(face_data, face.u, 0.0)
		_append4(face_data, face.v, 0.0)
		_append4(face_data, face.normal, 0.0)
		var linear: Color = boxes[face.box].color.srgb_to_linear()
		_append4(face_data, Vector3(linear.r, linear.g, linear.b), 0.0)
		_append4(face_data, _emission_radiance(boxes[face.box]), 0.0)
		var rect: Rect2i = face.rect
		face_data.append_array(PackedFloat32Array([rect.position.x, rect.position.y, rect.size.x, rect.size.y]))
		# The base terrain has no playable or lit region beneath it. Preserve its
		# descriptor/occlusion but spend no transport rays on the ground underside.
		if str(boxes[face.box].get("name", "")) == "Ground" and face.normal.y < -0.5:
			continue
		for y in rect.size.y:
			for x in rect.size.x:
				var p: Vector3 = face.origin + face.u * ((float(x) + 0.5) / rect.size.x) + face.v * ((float(y) + 0.5) / rect.size.y)
				# Buried samples never emit or receive light. Geometry still occludes rays.
				if _is_buried(p + face.normal * SURFACE_BIAS, face.box):
					continue
				_append4(surfels, p, float(face_index))
				surfels.append_array(PackedFloat32Array([rect.position.x + x, rect.position.y + y, 0.0, 0.0]))

func set_emission(box_index: int, color: Color, energy: float) -> void:
	if box_index < 0 or box_index >= boxes.size():
		push_error("Surface emission box index is outside the layout: %d" % box_index)
		return
	boxes[box_index]["emission_color"] = color
	boxes[box_index]["emission_energy"] = maxf(energy, 0.0)
	var radiance := _emission_radiance(boxes[box_index])
	for side in 6:
		var offset := (box_index * 6 + side) * FACE_FLOAT_COUNT + FACE_EMISSION_OFFSET
		face_data[offset] = radiance.x
		face_data[offset + 1] = radiance.y
		face_data[offset + 2] = radiance.z
		face_data[offset + 3] = 0.0

static func _emission_radiance(box: Dictionary) -> Vector3:
	var color: Color = box.get("emission_color", Color.BLACK)
	var linear := color.srgb_to_linear()
	var energy := maxf(float(box.get("emission_energy", 0.0)), 0.0)
	return Vector3(linear.r, linear.g, linear.b) * energy

func _add_face(box_index: int, origin: Vector3, u: Vector3, v: Vector3, normal: Vector3) -> void:
	var base_spacing := floor_spacing if normal.y > 0.5 else wall_spacing
	var spacing := _receiver_spacing(box_index, origin, u, v, normal, base_spacing)
	var w := clampi(ceili(u.length() / spacing), 1, ATLAS_WIDTH)
	var h := maxi(1, ceili(v.length() / spacing))
	if _cursor.x + w > ATLAS_WIDTH:
		_cursor.x = 0
		_cursor.y += _row_height
		_row_height = 0
	faces.append({"box": box_index, "origin": origin, "u": u, "v": v, "normal": normal, "rect": Rect2i(_cursor, Vector2i(w, h)), "emitter_refined": spacing < base_spacing})
	_cursor.x += w
	_row_height = maxi(_row_height, h)

func _receiver_spacing(owner: int, origin: Vector3, u: Vector3, v: Vector3, normal: Vector3, spacing: float) -> float:
	# Near a flush emitter, wall irradiance changes much faster than the usual
	# half-meter chart can reconstruct. Spend density on those vertical charts;
	# keep the large ground and all distant surfaces at their top-down budgets.
	if not refine_emitter_receivers or absf(normal.y) > 0.5 or spacing <= EMITTER_RECEIVER_SPACING:
		return spacing
	var face_minimum := origin + u.min(Vector3.ZERO) + v.min(Vector3.ZERO)
	var face_maximum := origin + u.max(Vector3.ZERO) + v.max(Vector3.ZERO)
	for index in boxes.size():
		if index == owner:
			continue
		var source: Dictionary = boxes[index]
		var emission: Color = source.get("emission_color", Color.BLACK)
		# Eligibility is independent of current energy, so toggling a declared
		# emitter never changes chart allocation or invalidates static transport.
		if maxf(emission.r, maxf(emission.g, emission.b)) <= 0.0:
			continue
		var center: Vector3 = source.center
		var half: Vector3 = source.size * 0.5
		if (center - origin).dot(normal) + half.dot(normal.abs()) <= SURFACE_BIAS:
			continue
		var gap := (center - half - face_maximum).max(face_minimum - center - half).max(Vector3.ZERO)
		if gap.length_squared() <= EMITTER_REFINEMENT_DISTANCE * EMITTER_REFINEMENT_DISTANCE:
			return EMITTER_RECEIVER_SPACING
	return spacing

func _face_blockers(face: Dictionary) -> PackedVector4Array:
	# Project boxes onto this exact face chart. This works for all signed axes,
	# including bridge undersides; a top-down XZ-only test would miss those faces.
	var result := PackedVector4Array()
	var normal: Vector3 = face.normal
	var origin: Vector3 = face.origin
	var plane_origin := origin + normal * SURFACE_BIAS
	var u: Vector3 = face.u
	var v: Vector3 = face.v
	var u_length_squared := u.length_squared()
	var v_length_squared := v.length_squared()
	for index in boxes.size():
		if index == int(face.box):
			continue
		var box: Dictionary = boxes[index]
		var half: Vector3 = box.size * 0.5
		var center: Vector3 = box.center
		if absf((center - plane_origin).dot(normal)) >= half.dot(normal.abs()):
			continue
		var relative := center - origin
		var uv_center := Vector2(relative.dot(u) / u_length_squared, relative.dot(v) / v_length_squared)
		var uv_half := Vector2(half.dot(u.abs()) / u_length_squared, half.dot(v.abs()) / v_length_squared)
		var minimum := (uv_center - uv_half).clamp(Vector2.ZERO, Vector2.ONE)
		var maximum := (uv_center + uv_half).clamp(Vector2.ZERO, Vector2.ONE)
		if maximum.x <= minimum.x or maximum.y <= minimum.y:
			continue
		# Uniform convention is (minimum U, minimum V, maximum U, maximum V).
		result.append(Vector4(minimum.x, minimum.y, maximum.x, maximum.y))
	return result

func _is_buried(p: Vector3, owner_index: int) -> bool:
	for i in boxes.size():
		if i == owner_index:
			continue
		var b: Dictionary = boxes[i]
		var half: Vector3 = b.size * 0.5
		var delta: Vector3 = (p - b.center).abs()
		if delta.x < half.x and delta.y < half.y and delta.z < half.z:
			return true
	return false

static func _append4(data: PackedFloat32Array, xyz: Vector3, w: float) -> void:
	data.append_array(PackedFloat32Array([xyz.x, xyz.y, xyz.z, w]))

func make_mesh(box_index: int, shader: Shader, texture: Texture2D) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	for side in 6:
		var face: Dictionary = faces[box_index * 6 + side]
		var arrays := []
		arrays.resize(Mesh.ARRAY_MAX)
		var o: Vector3 = face.origin - boxes[box_index].center
		var u: Vector3 = face.u
		var v: Vector3 = face.v
		arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([o, o + u, o + u + v, o + v])
		arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([face.normal, face.normal, face.normal, face.normal])
		arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)])
		# Godot front faces use clockwise winding.
		arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 2, 1, 0, 3, 2])
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		var material := ShaderMaterial.new()
		material.shader = shader
		material.set_shader_parameter("surface_color", boxes[box_index].color)
		material.set_shader_parameter("surface_emission", boxes[box_index].get("emission_color", Color.BLACK))
		material.set_shader_parameter("emission_energy", maxf(float(boxes[box_index].get("emission_energy", 0.0)), 0.0))
		material.set_shader_parameter("indirect_atlas", texture)
		var r: Rect2i = face.rect
		material.set_shader_parameter("atlas_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y))
		material.set_shader_parameter("atlas_size", Vector2(atlas_size))
		var blockers: PackedVector4Array = face.blockers.duplicate()
		var blocker_count := mini(blockers.size(), MAX_FACE_BLOCKERS)
		blockers.resize(MAX_FACE_BLOCKERS)
		material.set_shader_parameter("blockers", blockers)
		material.set_shader_parameter("blocker_count", blocker_count)
		material.set_shader_parameter("blocker_overflow", face.blocker_overflow)
		mesh.surface_set_material(side, material)
	return mesh

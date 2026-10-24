class_name EmissiveBall
extends Node3D
## A movable spherical emitter, separate from the static transport geometry.

const START := Vector3(-3.5, 0.9, -1.5)
const COLORS := [Color(1.0, 0.78, 0.38), Color(0.18, 0.65, 1.0), Color(0.4, 1.0, 0.35), Color.WHITE]
const COLOR_NAMES := ["Warm", "Blue", "Green", "White"]
var radius := 0.3
var energy := 24.0
var enabled := true
var color_index := 0
var visual: MeshInstance3D
var material: StandardMaterial3D

func _ready() -> void:
	for binding in [["ball_left", KEY_LEFT], ["ball_right", KEY_RIGHT], ["ball_up", KEY_UP], ["ball_down", KEY_DOWN], ["ball_raise", KEY_Q], ["ball_lower", KEY_Z]]:
		if InputMap.has_action(binding[0]):
			continue
		InputMap.add_action(binding[0])
		var key := InputEventKey.new()
		key.physical_keycode = binding[1]
		InputMap.action_add_event(binding[0], key)
	visual = MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = radius
	sphere.height = radius * 2.0
	sphere.radial_segments = 48
	sphere.rings = 24
	visual.mesh = sphere
	visual.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	material = StandardMaterial3D.new()
	material.emission_enabled = true
	visual.material_override = material
	add_child(visual)
	position = START
	refresh_visual()

func refresh_visual() -> void:
	material.albedo_color = Color(0.06, 0.06, 0.06)
	material.emission = COLORS[color_index]
	material.emission_energy_multiplier = energy if enabled else 0.0

func source_data() -> PackedFloat32Array:
	var radiance: Color = COLORS[color_index].srgb_to_linear() * (energy if enabled else 0.0)
	return PackedFloat32Array([position.x, position.y, position.z, radius, radiance.r, radiance.g, radiance.b, 0.0])

func move_with_input(delta: float, camera: Camera3D, boxes: Array[Dictionary]) -> void:
	var input := Input.get_vector("ball_left", "ball_right", "ball_up", "ball_down")
	var right := camera.global_basis.x
	right.y = 0.0
	var forward := -camera.global_basis.z
	forward.y = 0.0
	var movement := right.normalized() * input.x - forward.normalized() * input.y
	movement.y = Input.get_action_strength("ball_raise") - Input.get_action_strength("ball_lower")
	var speed := 5.0 if Input.is_physical_key_pressed(KEY_SHIFT) else 2.5
	move_by(movement.limit_length(1.0) * minf(delta, 0.1) * speed, boxes)

func move_by(displacement: Vector3, boxes: Array[Dictionary]) -> void:
	# Small swept steps prevent tunneling through thin partitions; separate axes
	# give predictable wall sliding without rebuilding physics/transport meshes.
	var steps := maxi(1, ceili(displacement.length() / (radius * 0.3)))
	var step := displacement / float(steps)
	for index in steps:
		for axis in [0, 2, 1]:
			var candidate := position
			candidate[axis] += step[axis]
			if can_place(candidate, boxes):
				position = candidate

func can_place(point: Vector3, boxes: Array[Dictionary]) -> bool:
	if absf(point.x) > 7.85 - radius or absf(point.z) > 5.85 - radius or point.y < radius + 0.04 or point.y > 2.8 - radius:
		return false
	for box in boxes:
		var center: Vector3 = box.center
		var half: Vector3 = box.size * 0.5
		var nearest := point.clamp(center - half, center + half)
		if point.distance_squared_to(nearest) < (radius + 0.005) * (radius + 0.005):
			return false
	return true

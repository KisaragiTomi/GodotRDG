class_name FreeFlyCamera
extends Node
## Viewer controls shared by the GI demos. No collision or camera roll.

var camera: Camera3D
var speed := 5.0
var sensitivity := 0.0025
var looking := false
var enabled := false:
	set(value):
		enabled = value
		if not enabled: release_mouse()

func _ready() -> void:
	get_window().focus_exited.connect(release_mouse)

func release_mouse() -> void:
	if looking:
		looking = false
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE

func _exit_tree() -> void:
	release_mouse()

func _input(event: InputEvent) -> void:
	if not enabled or camera == null: return
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_RIGHT:
			if event.pressed:
				looking = true
				camera.projection = Camera3D.PROJECTION_PERSPECTIVE
				Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
			else:
				release_mouse()
			get_viewport().set_input_as_handled()
		elif event.pressed and event.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN]:
			var direction := 1.0 if event.button_index == MOUSE_BUTTON_WHEEL_UP else -1.0
			if camera.projection == Camera3D.PROJECTION_ORTHOGONAL:
				camera.size = clampf(camera.size - direction, 1.0, 80.0)
			else:
				camera.global_position -= camera.global_basis.z * direction * 0.6
			get_viewport().set_input_as_handled()
	elif event is InputEventMouseMotion and looking:
		var angles := camera.rotation
		angles.y = wrapf(angles.y - event.screen_relative.x * sensitivity, -PI, PI)
		angles.x = clampf(angles.x - event.screen_relative.y * sensitivity, deg_to_rad(-89.0), deg_to_rad(89.0))
		camera.rotation = Vector3(angles.x, angles.y, 0.0)
		get_viewport().set_input_as_handled()
	elif event is InputEventKey and event.pressed and event.physical_keycode == KEY_ESCAPE and looking:
		release_mouse()
		get_viewport().set_input_as_handled()

func _process(delta: float) -> void:
	if not enabled or camera == null or not get_window().has_focus(): return
	var direction := Vector3.ZERO
	if Input.is_physical_key_pressed(KEY_W): direction -= camera.global_basis.z
	if Input.is_physical_key_pressed(KEY_S): direction += camera.global_basis.z
	if Input.is_physical_key_pressed(KEY_D): direction += camera.global_basis.x
	if Input.is_physical_key_pressed(KEY_A): direction -= camera.global_basis.x
	if Input.is_physical_key_pressed(KEY_SPACE): direction += Vector3.UP
	if Input.is_physical_key_pressed(KEY_CTRL): direction -= Vector3.UP
	var boost := 3.0 if Input.is_physical_key_pressed(KEY_SHIFT) else 1.0
	camera.global_position += direction.limit_length() * speed * boost * minf(delta, 0.1)

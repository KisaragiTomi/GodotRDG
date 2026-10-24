extends SceneTree
## Exercise the actual spatial shader output, independently of the GI solver.

const SURFACE_SHADER = preload("res://shaders/surface.gdshader")
var scene: Node3D
var camera: Camera3D
var report: Dictionary = {}

func _initialize() -> void:
	_run.call_deferred()

func _texture(width: int, values: Array[Color]) -> ImageTexture:
	var image := Image.create(width, 1, false, Image.FORMAT_RGBAF)
	for x in width:
		image.set_pixel(x, 0, values[x])
	return ImageTexture.create_from_image(image)

func _material(color: Color, texture: Texture2D) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = SURFACE_SHADER
	material.set_shader_parameter("surface_color", color)
	material.set_shader_parameter("gi_mode", 1)
	material.set_shader_parameter("indirect_atlas", texture)
	material.set_shader_parameter("atlas_rect", Vector4(0, 0, texture.get_width(), 1))
	material.set_shader_parameter("atlas_size", Vector2(texture.get_width(), 1))
	return material

func _frame() -> Image:
	for i in 5:
		await process_frame
	await RenderingServer.frame_post_draw
	return root.get_texture().get_image()

func _linear_pixel(image: Image, point: Vector3) -> float:
	var pixel := Vector2i(camera.unproject_position(point))
	return image.get_pixel(pixel.x, pixel.y).srgb_to_linear().r

func _run() -> void:
	scene = Node3D.new()
	root.add_child(scene)
	var world := WorldEnvironment.new()
	world.environment = Environment.new()
	world.environment.background_mode = Environment.BG_COLOR
	world.environment.background_color = Color.BLACK
	world.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	world.environment.ambient_light_energy = 0
	world.environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	world.environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	scene.add_child(world)
	camera = Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 8
	scene.add_child(camera)
	camera.position = Vector3(0, 6, 0)
	camera.look_at(Vector3.ZERO, Vector3.FORWARD)
	camera.current = true
	var uniform := _texture(1, [Color(0.25, 0.25, 0.25, 1)])
	var instances: Array[MeshInstance3D] = []
	for index in 3:
		var mesh := MeshInstance3D.new()
		var plane := PlaneMesh.new()
		plane.size = Vector2(1.5, 1.5)
		mesh.mesh = plane
		mesh.position.x = float(index - 1) * 2.0
		var value := float(index) * 0.5
		mesh.material_override = _material(Color(value, value, value), uniform)
		scene.add_child(mesh)
		instances.append(mesh)
	var image := await _frame()
	var measured: Array[float] = []
	for instance in instances:
		measured.append(_linear_pixel(image, instance.position))
	report.linear_material_values = measured
	report.black_absorbs = measured[0] < 0.001
	report.white_response = absf(measured[2] - 0.25) < 0.005
	report.gray_response = absf(measured[1] - 0.25 * Color(0.5, 0.5, 0.5).srgb_to_linear().r) < 0.005
	for instance in instances:
		instance.queue_free()
	await process_frame
	# Known discontinuity at an off-grid wall. A plain bilinear sample would
	# return about 0.38 on its dark side; visibility-filtered reconstruction is zero.
	var layout := SurfaceLayout.new()
	var boxes: Array[Dictionary] = [{"name": "TestFace", "center": Vector3(0, -0.1, 0), "size": Vector3(4, 0.2, 2), "color": Color.WHITE}]
	layout.build(boxes)
	var step_texture := _texture(2, [Color(1, 1, 1, 1), Color(0, 0, 0, 1)])
	var receiver := MeshInstance3D.new()
	receiver.mesh = layout.make_mesh(0, SURFACE_SHADER, step_texture)
	receiver.position = boxes[0].center
	var material := _material(Color.WHITE, step_texture)
	var blockers := PackedVector4Array([Vector4(0.54, 0, 0.55, 1)])
	blockers.resize(32)
	material.set_shader_parameter("blockers", blockers)
	material.set_shader_parameter("blocker_count", 1)
	receiver.set_surface_override_material(2, material)
	scene.add_child(receiver)
	image = await _frame()
	var lit := _linear_pixel(image, Vector3(-0.24, 0, 0))
	var dark := _linear_pixel(image, Vector3(0.24, 0, 0))
	report.filtered_lit_side = lit
	report.filtered_dark_side = dark
	report.receiver_rejects_cross_wall = lit > 0.9 and dark < 0.001
	report.passed = report.black_absorbs and report.white_response and report.gray_response and report.receiver_rejects_cross_wall
	var folder := ProjectSettings.globalize_path("res://artifacts/validation")
	DirAccess.make_dir_recursive_absolute(folder)
	FileAccess.open(folder.path_join("material_test.json"), FileAccess.WRITE).store_string(JSON.stringify(report, "\t"))
	print("MATERIAL_TEST ", JSON.stringify(report))
	quit(0 if report.passed else 1)

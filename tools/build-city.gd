extends SceneTree
## Deterministic, editable city assembled from bundled Kenney CC0 GLB instances.
## Run after import: godot --headless --path . --script tools/build-city.gd

const CityLab = preload("res://scripts/city_lab.gd")
const ASSETS := "res://assets/kenney_city/%s/Models/GLB format/%s.glb"
var city := Node3D.new()
var rng := RandomNumberGenerator.new()
var packs: Dictionary = {}
var building_count := 0

func _initialize() -> void:
	call_deferred("_build")

func _add(node: Node, parent: Node, label: String) -> Node:
	node.name = label
	parent.add_child(node, true)
	node.owner = city
	return node

func _group(label: String, parent: Node) -> Node3D:
	return _add(Node3D.new(), parent, label)

func _mat(color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 0.9
	return material

func _box(parent: Node, label: String, position_value: Vector3, size: Vector3, material: Material) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = position_value
	return _add(instance, parent, label)

func _model(pack: String, model: String, parent: Node, position_value: Vector3, scale_value: Vector3, yaw := 0.0) -> Node3D:
	var path := ASSETS % [pack, model]
	if not packs.has(path): packs[path] = load(path)
	var instance: Node3D = packs[path].instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	instance.position = position_value
	instance.scale = scale_value
	instance.rotation.y = yaw
	return _add(instance, parent, model.replace("-", "_").capitalize().replace(" ", ""))

func _mesh_bounds(node: Node3D) -> AABB:
	var result := AABB()
	for child in node.find_children("*", "MeshInstance3D", true, false):
		var bounds: AABB = child.get_aabb()
		result = result.merge(bounds)
	if node is MeshInstance3D: result = result.merge(node.get_aabb())
	return result

func _tree(parent: Node, position_value: Vector3) -> void:
	var tree := _group("Tree", parent)
	tree.position = position_value
	_box(tree, "Trunk", Vector3(0, 2, 0), Vector3(0.55, 4, 0.55), _mat(Color(0.29, 0.22, 0.15)))
	var mesh := SphereMesh.new()
	mesh.radius = 2.5
	mesh.height = 6.0
	mesh.radial_segments = 12
	mesh.rings = 6
	mesh.material = _mat(Color(0.25, 0.36, 0.17) * rng.randf_range(0.85, 1.15))
	var canopy := MeshInstance3D.new()
	canopy.mesh = mesh
	canopy.position.y = 5.2
	_add(canopy, tree, "Canopy")

func _build() -> void:
	rng.seed = 42107
	city.name = "CityAtmosphere"
	root.add_child(city)
	var ground := _group("Ground", city)
	_box(ground, "Surroundings", Vector3(0, -0.5, 0), Vector3(10000, 0.8, 10000), _mat(Color(0.34, 0.39, 0.29)))
	_box(ground, "CityFoundation", Vector3(0, -0.12, 0), Vector3(472, 0.2, 472), _mat(Color(0.48, 0.47, 0.42)))
	var roads := _group("Streets", city)
	# 16 m road modules, 64 m block pitch; intersections share exact tile edges.
	for ix in range(-14, 15):
		for iz in range(-14, 15):
			var vertical := posmod(ix + 14, 4) == 0
			var horizontal := posmod(iz + 14, 4) == 0
			if not vertical and not horizontal: continue
			var kind := "road-crossroad" if vertical and horizontal else "road-straight"
			if vertical != horizontal and (posmod(ix + 14, 4) in [1, 3] if horizontal else posmod(iz + 14, 4) in [1, 3]): kind = "road-crossing"
			_model("roads", kind, roads, Vector3(ix * 16, 0, iz * 16), Vector3.ONE * 16, PI / 2 if vertical and not horizontal else 0.0)
	var blocks := _group("CityBlocks", city)
	var pavement := _mat(Color(0.61, 0.59, 0.54))
	for bx in 7:
		for bz in 7:
			var block := _group("Block_%d_%d" % [bx, bz], blocks)
			block.position = Vector3(-192 + bx * 64, 0, -192 + bz * 64)
			_box(block, "Sidewalk", Vector3(0, 0.2, 0), Vector3(48, 0.35, 48), pavement)
			if (bx == 3 and bz in [3, 4]) or (bx == 1 and bz == 2):
				_build_park(block)
				continue
			for lx in [-1.0, 1.0]:
				for lz in [-1.0, 1.0]:
					var downtown := bx in [2, 3, 4] and bz in [1, 2, 3]
					var tower := downtown or (rng.randf() < 0.18 and bz < 5)
					var model := "building-skyscraper-" + char(97 + rng.randi_range(0, 4)) if tower else "building-" + char(97 + rng.randi_range(0, 13))
					var building := _model("commercial", model, block, Vector3(lx * 11, 0.375, lz * 11), Vector3.ONE, PI if lz > 0 else 0.0)
					var size := _mesh_bounds(building).size
					var width := rng.randf_range(16.0, 19.0)
					var factor := width / maxf(size.x, size.z)
					building.scale = Vector3.ONE * factor
					building.scale.y *= rng.randf_range(1.0, 1.35) if tower else rng.randf_range(0.85, 1.1)
					building_count += 1
			for side in [-1.0, 1.0]:
				_model("roads", "light-square", block, Vector3(side * 22, 0.375, 0), Vector3.ONE * 12, -side * PI / 2)
				_tree(block, Vector3(0, 0.375, side * 21))
	_build_environment()
	city.set_meta("building_count", building_count)
	city.set_meta("source", "https://kenney.nl/assets/city-kit-commercial ; https://kenney.nl/assets/city-kit-roads ; CC0")
	city.set_script(CityLab)
	var packed := PackedScene.new()
	var error := packed.pack(city)
	if error == OK: error = ResourceSaver.save(packed, "res://scenes/city.tscn")
	print("CITY_BUILD: ", building_count, " buildings; save status ", error)
	quit(0 if error == OK else 1)

func _build_park(block: Node3D) -> void:
	var grass := _mat(Color(0.30, 0.40, 0.20))
	var stone := _mat(Color(0.66, 0.64, 0.58))
	for x in [-12.0, 12.0]:
		for z in [-12.0, 12.0]:
			_box(block, "Garden", Vector3(x, 0.4, z), Vector3(18, 0.12, 18), grass)
			for offset in [-4.0, 4.0]: _tree(block, Vector3(x + offset, 0.46, z))
	_box(block, "FountainBase", Vector3(0, 0.7, 0), Vector3(7, 0.7, 7), stone)
	_box(block, "FountainWater", Vector3(0, 1.08, 0), Vector3(6.2, 0.08, 6.2), _mat(Color(0.24, 0.51, 0.60)))
	_box(block, "Monument", Vector3(0, 3.8, 0), Vector3(1.2, 6, 1.2), stone)

func _build_environment() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	var material := ProceduralSkyMaterial.new()
	material.sky_top_color = Color(0.24, 0.39, 0.57)
	material.sky_horizon_color = Color(0.77, 0.73, 0.62)
	material.ground_horizon_color = material.sky_horizon_color
	material.ground_bottom_color = Color(0.32, 0.36, 0.27)
	material.sun_angle_max = 2.0
	sky.sky_material = material
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.53, 0.63, 0.77)
	environment.ambient_light_energy = 0.4
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	environment.ssao_enabled = true
	environment.ssao_radius = 2.0
	environment.ssao_intensity = 1.3
	var world := WorldEnvironment.new()
	world.environment = environment
	_add(world, city, "WorldEnvironment")
	var sun := DirectionalLight3D.new()
	sun.light_color = Color(1, 0.85, 0.64)
	sun.light_energy = 2.8
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 600.0
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	sun.directional_shadow_blend_splits = true
	sun.shadow_bias = 0.1
	sun.shadow_normal_bias = 1.0
	_add(sun, city, "Sun")
	sun.look_at_from_position(Vector3(-0.55, 0.55, -0.75) * 100, Vector3.ZERO)
	# Evaluate the shared atmosphere once and serialize its volume for editor preview.
	var preview := TGAtmosphere.new()
	preview.thickness = CityLab.FOG_THICKNESS
	preview.distance_scale = CityLab.FOG_SCALE
	preview.fog_length = CityLab.EDITOR_FOG_LENGTH
	city.add_child(preview)
	preview.setup(environment, sun)
	var volume := preview.volume
	preview.remove_child(volume)
	_add(volume, city, "EditorFog")
	# Disarm restoration before freeing this temporary builder helper.
	preview.environment = null
	preview.sun = null
	preview.free()
	environment.volumetric_fog_temporal_reprojection_amount = 0.9
	var camera := Camera3D.new()
	camera.near = 0.2
	camera.far = 1800.0
	camera.fov = 65.0
	camera.current = true
	_add(camera, city, "Camera3D")
	camera.position = CityLab.VIEW_POSITIONS[0]
	camera.look_at(CityLab.VIEW_TARGETS[0])

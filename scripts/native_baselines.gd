class_name NativeBaselines
extends Node
## Native reference renderers. Bounds are in geometry_root's local space.
## Lights must be descendants of geometry_root so both native bakers see them.

var voxel: VoxelGI
var lightmap: LightmapGI
var geometry_root: Node3D
var current_mode := "direct"
var last_bake_msec := 0.0
var last_error := ""
var _lightmap_data: LightmapGIData


func setup(root: Node3D, bounds: AABB, lights: Array = []) -> void:
	geometry_root = root
	voxel = VoxelGI.new()
	voxel.name = "VoxelBaseline"
	# Top-down scenes need horizontal reach, not a tall cubic world volume.
	# Preserve the caller's complete vertical geometry bounds, with a small margin.
	voxel.size = Vector3(maxf(bounds.size.x + 1.0, 1.0), maxf(bounds.size.y + 1.0, 1.0), maxf(bounds.size.z + 1.0, 1.0))
	voxel.position = bounds.get_center()
	voxel.subdiv = VoxelGI.SUBDIV_128
	root.add_child(voxel)
	voxel.hide()
	lightmap = LightmapGI.new()
	lightmap.name = "LightmapBaseline"
	lightmap.quality = LightmapGI.BAKE_QUALITY_HIGH
	lightmap.bounces = 2
	lightmap.directional = false
	lightmap.interior = true
	lightmap.environment_mode = LightmapGI.ENVIRONMENT_MODE_DISABLED
	lightmap.generate_probes_subdiv = LightmapGI.GENERATE_PROBES_SUBDIV_8
	lightmap.max_texture_size = 2048
	root.add_child(lightmap)
	lightmap.hide()
	for source in lights:
		if source is Light3D:
			# Keep direct illumination real-time in every comparison. LightmapGI
			# records only the indirect contribution from BAKE_DYNAMIC lights.
			source.light_bake_mode = Light3D.BAKE_DYNAMIC
			if not root.is_ancestor_of(source):
				push_warning("Native baseline light must be below geometry_root: " + str(source.name))


func bake_voxel(save_path: String = "") -> Dictionary:
	if not is_instance_valid(voxel) or not geometry_root.is_inside_tree():
		return _failure("Voxel baseline has not been set up in the scene tree.")
	if RenderingServer.get_current_rendering_method() != "forward_plus":
		return _failure("VoxelGI requires the Forward+ renderer.")
	# Let procedurally created meshes and lights finish their ready notifications.
	await get_tree().process_frame
	# Godot's CPU voxelizer reads BaseMaterial3D, not spatial shader outputs.
	# Give it the same diffuse albedo and emission without changing draw materials.
	var material_backup: Array = []
	if not _prepare_voxel_materials(geometry_root, material_backup):
		_restore_voxel_materials(material_backup)
		return _failure("VoxelGI encountered a ShaderMaterial without the supported surface_color parameter.")
	var started := Time.get_ticks_usec()
	voxel.bake(geometry_root)
	last_bake_msec = float(Time.get_ticks_usec() - started) / 1000.0
	_restore_voxel_materials(material_backup)
	if voxel.data == null or voxel.data.get_octree_cells().is_empty():
		return _failure("Voxel bake produced no cells; check static geometry and bounds.")
	voxel.data.interior = true
	voxel.data.energy = 1.0
	voxel.data.propagation = 0.5
	voxel.data.use_two_bounces = true
	voxel.data.bias = 1.0
	voxel.data.normal_bias = 0.5
	if not save_path.is_empty():
		var directory_error := DirAccess.make_dir_recursive_absolute(save_path.get_base_dir())
		if directory_error != OK:
			return _failure("Cannot create VoxelGI output directory: " + error_string(directory_error))
		var save_error := ResourceSaver.save(voxel.data, save_path)
		if save_error != OK:
			return _failure("Cannot save VoxelGI data: " + error_string(save_error))
	last_error = ""
	return {"ok": true, "bake_ms": last_bake_msec, "grid": str(voxel.data.get_octree_size()), "path": save_path}


func load_voxel(path: String) -> bool:
	if not ResourceLoader.exists(path):
		return false
	var data := ResourceLoader.load(path) as VoxelGIData
	if data == null or data.get_octree_cells().is_empty():
		return false
	voxel.data = data
	return true


func prepare_lightmap(output_path: String) -> Error:
	# LightmapGI.bake is intentionally not bound to GDScript in Godot 4.6.1.
	# A saved empty resource gives the built-in editor baker a path without a dialog.
	var directory_error := DirAccess.make_dir_recursive_absolute(output_path.get_base_dir())
	if directory_error != OK:
		return directory_error
	var seed := LightmapGIData.new()
	var save_error := ResourceSaver.save(seed, output_path)
	if save_error == OK:
		seed.take_over_path(output_path)
		lightmap.light_data = seed
	return save_error


func load_lightmap(path: String) -> bool:
	if not ResourceLoader.exists(path):
		return false
	var data := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_REPLACE) as LightmapGIData
	if data == null or data.get_user_count() == 0 or data.lightmap_textures.is_empty():
		return false
	_lightmap_data = data
	if current_mode == "lightmap":
		lightmap.light_data = _lightmap_data
	return true


func select_mode(mode: String) -> bool:
	if mode not in ["direct", "voxel", "lightmap"]:
		last_error = "Unknown native mode: " + mode
		return false
	if mode == "voxel" and voxel.data == null:
		last_error = "VoxelGI has not been baked."
		return false
	if mode == "lightmap" and (_lightmap_data == null or _lightmap_data.get_user_count() == 0):
		last_error = "LightmapGI has not been baked in the editor."
		return false
	# Hiding LightmapGI alone does not clear per-mesh lightmap assignments.
	lightmap.light_data = _lightmap_data if mode == "lightmap" else null
	lightmap.visible = mode == "lightmap"
	voxel.visible = mode == "voxel"
	current_mode = mode
	last_error = ""
	return true


func status() -> Dictionary:
	return {
		"mode": current_mode,
		"voxel_ready": is_instance_valid(voxel) and voxel.data != null,
		"lightmap_ready": _lightmap_data != null and _lightmap_data.get_user_count() > 0,
		"voxel_bake_ms": last_bake_msec,
		"error": last_error,
	}


func _failure(message: String) -> Dictionary:
	last_error = message
	return {"ok": false, "error": message}


func _prepare_voxel_materials(node: Node, backups: Array) -> bool:
	if node is MeshInstance3D and node.mesh != null and node.gi_mode == GeometryInstance3D.GI_MODE_STATIC:
		var instance := node as MeshInstance3D
		if instance.material_override is ShaderMaterial:
			var converted := _native_material(instance.material_override)
			if converted == null:
				return false
			backups.append({"instance": instance, "surface": -1, "material": instance.material_override})
			instance.material_override = converted
		elif instance.material_override == null:
			for index in instance.mesh.get_surface_count():
				var active := instance.get_active_material(index)
				if active is ShaderMaterial:
					var converted := _native_material(active)
					if converted == null:
						return false
					backups.append({"instance": instance, "surface": index, "material": instance.get_surface_override_material(index)})
					instance.set_surface_override_material(index, converted)
	for child in node.get_children():
		if not _prepare_voxel_materials(child, backups):
			return false
	return true


func _native_material(source: ShaderMaterial) -> StandardMaterial3D:
	# This adapter is limited to the lab's constant diffuse/emissive shader.
	# Arbitrary textured or metallic shaders need their own bake adapter.
	var color_value: Variant = source.get_shader_parameter("surface_color")
	if not color_value is Color:
		return null
	var converted := StandardMaterial3D.new()
	converted.albedo_color = color_value
	converted.roughness = 1.0
	converted.metallic = 0.0
	converted.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
	converted.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
	var emission_color: Variant = source.get_shader_parameter("surface_emission")
	var emission_energy: Variant = source.get_shader_parameter("emission_energy")
	if emission_color is Color and (emission_energy is float or emission_energy is int):
		converted.emission_enabled = float(emission_energy) > 0.0
		converted.emission = emission_color
		converted.emission_energy_multiplier = maxf(float(emission_energy), 0.0)
	return converted


func _restore_voxel_materials(backups: Array) -> void:
	for entry in backups:
		var instance: MeshInstance3D = entry.instance
		if entry.surface == -1:
			instance.material_override = entry.material
		else:
			instance.set_surface_override_material(entry.surface, entry.material)

extends SceneTree
## Offline UV charts and triangle export; GPU lighting remains runtime work.

const OUTPUT := "res://generated/sponza"

func _initialize() -> void:
	call_deferred("_prepare")

func _prepare() -> void:
	DirAccess.make_dir_recursive_absolute(OUTPUT)
	var scene := (load("res://assets/sponza/glTF/Sponza.gltf") as PackedScene).instantiate()
	root.add_child(scene)
	var instance := scene.find_children("*", "MeshInstance3D", true, false)[0] as MeshInstance3D
	var mesh: ArrayMesh = instance.mesh.duplicate()
	var result := mesh.lightmap_unwrap(instance.global_transform, 0.3)
	if result != OK:
		push_error("Sponza UV unwrap failed: " + error_string(result))
		quit(1)
		return
	ResourceSaver.save(mesh, OUTPUT.path_join("mesh.res"))
	var material_ids: Dictionary = {}
	var materials: Array[Dictionary] = []
	var surface_materials: Array[int] = []
	var triangles := PackedFloat32Array()
	for surface in mesh.get_surface_count():
		var material := mesh.surface_get_material(surface) as StandardMaterial3D
		var id := materials.size()
		if material_ids.has(material.get_instance_id()):
			id = material_ids[material.get_instance_id()]
		else:
			material_ids[material.get_instance_id()] = id
			var color := material.albedo_color.srgb_to_linear()
			materials.append({"color": [color.r, color.g, color.b, color.a], "metallic": material.metallic, "metal_channel": material.metallic_texture_channel, "alpha_cutoff": material.alpha_scissor_threshold if material.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR else -1.0})
			_export_texture(material.albedo_texture, OUTPUT.path_join("albedo_%d.png" % id))
			_export_texture(material.metallic_texture, OUTPUT.path_join("metal_%d.png" % id))
		surface_materials.append(id)
		var arrays := mesh.surface_get_arrays(surface)
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
		var uv: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
		var uv2: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		for index in range(0, indices.size(), 3):
			for corner in 3:
				var vertex := indices[index + corner]
				var p: Vector3 = instance.global_transform * vertices[vertex]
				triangles.append_array(PackedFloat32Array([p.x, p.y, p.z, float(id)]))
			for corner in 3:
				var n := (instance.global_basis.inverse().transposed() * normals[indices[index + corner]]).normalized()
				triangles.append_array(PackedFloat32Array([n.x, n.y, n.z, 0.0]))
			for corner in 3:
				var vertex := indices[index + corner]
				triangles.append_array(PackedFloat32Array([uv[vertex].x, uv[vertex].y, uv2[vertex].x, uv2[vertex].y]))
	var file := FileAccess.open(OUTPUT.path_join("input.bin"), FileAccess.WRITE)
	file.store_buffer(triangles.to_byte_array())
	file.close()
	var metadata := {"atlas": [mesh.lightmap_size_hint.x, mesh.lightmap_size_hint.y], "triangles": triangles.size() / 36, "materials": materials, "surface_materials": surface_materials, "spacing_m": 0.3}
	file = FileAccess.open(OUTPUT.path_join("input.json"), FileAccess.WRITE)
	file.store_string(JSON.stringify(metadata, "\t"))
	print("SPONZA_PREPARE ", metadata.triangles, " triangles, atlas=", metadata.atlas)
	scene.free()
	quit()

func _export_texture(texture: Texture2D, path: String) -> void:
	var image: Image
	if texture == null:
		image = Image.create(512, 512, false, Image.FORMAT_RGBA8)
		image.fill(Color.WHITE)
	else:
		image = texture.get_image()
		if image.is_compressed(): image.decompress()
		image.convert(Image.FORMAT_RGBA8)
		image.resize(512, 512, Image.INTERPOLATE_BILINEAR)
		image.clear_mipmaps()
	image.save_png(path)

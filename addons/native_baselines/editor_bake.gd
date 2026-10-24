@tool
extends EditorPlugin
## Run with an editor rendering device, not --headless:
## godot --editor --path . -- --native-bake
##   --scene=res://generated/native_reference.tscn --output=res://generated/native_reference.lmbake
## This dispatches the existing native editor baker; it does not reimplement GI.

var _arguments: Dictionary = {}


func _enter_tree() -> void:
	if not OS.get_cmdline_user_args().has("--native-bake"):
		return
	for argument in OS.get_cmdline_user_args():
		var pair := argument.trim_prefix("--").split("=", true, 1)
		if pair.size() == 2:
			_arguments[pair[0]] = pair[1]
	call_deferred("_run")


func _run() -> void:
	if not Engine.is_editor_hint():
		_fail("Launch this script with --editor.")
		return
	if RenderingServer.get_rendering_device() == null:
		_fail("Native baking needs a rendering device. Do not use --headless or Compatibility.")
		return
	var scene_path: String = _arguments.get("scene", "")
	var output_path: String = _arguments.get("output", "")
	if scene_path.is_empty() or output_path.is_empty():
		_fail("Required arguments: --scene=res://...tscn --output=res://...lmbake")
		return
	await get_tree().process_frame
	while EditorInterface.get_resource_filesystem().is_scanning():
		await get_tree().process_frame
	# Editor startup restores its previously open scene tabs asynchronously. Let
	# that finish before selecting the scene the baker will retain across yields.
	await get_tree().create_timer(1.0).timeout
	EditorInterface.open_scene_from_path(scene_path)
	for frame in 20:
		await get_tree().process_frame
	var scene := EditorInterface.get_edited_scene_root()
	if scene == null or scene.scene_file_path != scene_path or not scene.is_inside_tree():
		_fail("Unable to open materialized reference scene: " + scene_path)
		return
	var kind: String = _arguments.get("kind", "lightmap")
	if kind == "voxel":
		_bake_voxel(scene, output_path)
		return
	var lightmap := _find_class(scene, "LightmapGI") as LightmapGI
	if lightmap == null:
		_fail("Scene has no LightmapGI. Save static meshes, lights and LightmapBaseline into the scene first.")
		return
	var directory_error := DirAccess.make_dir_recursive_absolute(output_path.get_base_dir())
	if directory_error != OK:
		_fail("Cannot create output directory: " + error_string(directory_error))
		return
	var seed := LightmapGIData.new()
	# Supply a destination without replacing a previous bake before success.
	seed.take_over_path(output_path)
	lightmap.light_data = seed
	lightmap.show()
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(lightmap)
	EditorInterface.edit_node(lightmap)
	await get_tree().process_frame
	# The plugin's _bake method is bound; the node's bake method is not.
	# Inspect signal targets rather than translated button text.
	var baker := _find_native_baker(EditorInterface.get_base_control())
	if baker == null:
		_fail("Native LightmapGI editor baker was not found; this helper targets Godot 4.6.1.")
		return
	if not lightmap.is_inside_tree() or EditorInterface.get_edited_scene_root() != scene:
		_fail("Reference scene lost its active editor tree before baking.")
		return
	var started := Time.get_ticks_usec()
	baker.call("_bake")
	var bake_msec := float(Time.get_ticks_usec() - started) / 1000.0
	var data := lightmap.light_data
	if data == null or data.get_user_count() == 0 or data.lightmap_textures.is_empty():
		_fail("Native lightmap bake produced no populated data. Check editor errors, UV2 and Static GI meshes.")
		return
	var scene_save_error := EditorInterface.save_scene()
	if scene_save_error != OK:
		_fail("Lightmaps baked but the scene could not be saved: " + error_string(scene_save_error))
		return
	print("NATIVE_BASELINE_BAKE " + JSON.stringify({"ok": true, "kind": "lightmap", "path": output_path, "bake_ms": bake_msec, "mesh_count": data.get_user_count(), "atlas_count": data.lightmap_textures.size()}))
	EditorInterface.get_selection().clear()
	EditorInterface.edit_node(null)
	await get_tree().process_frame
	await get_tree().process_frame
	get_tree().quit(0)


func _bake_voxel(scene: Node, output_path: String) -> void:
	var voxel := _find_class(scene, "VoxelGI") as VoxelGI
	if voxel == null:
		_fail("Scene has no VoxelGI.")
		return
	var started := Time.get_ticks_usec()
	voxel.bake(voxel.get_parent())
	if voxel.data == null or voxel.data.get_octree_cells().is_empty():
		_fail("Voxel bake produced no cells.")
		return
	voxel.data.interior = true
	var directory_error := DirAccess.make_dir_recursive_absolute(output_path.get_base_dir())
	if directory_error != OK:
		_fail("Cannot create VoxelGI output directory: " + error_string(directory_error))
		return
	var save_error := ResourceSaver.save(voxel.data, output_path)
	if save_error != OK:
		_fail("Cannot save VoxelGI output: " + error_string(save_error))
		return
	print("NATIVE_BASELINE_BAKE " + JSON.stringify({"ok": true, "kind": "voxel", "path": output_path, "bake_ms": float(Time.get_ticks_usec() - started) / 1000.0, "grid": str(voxel.data.get_octree_size())}))
	get_tree().quit(0)


func _find_class(node: Node, target_class: String) -> Node:
	if node.is_class(target_class):
		return node
	for child in node.get_children():
		var found := _find_class(child, target_class)
		if found != null:
			return found
	return null


func _find_native_baker(node: Node) -> Object:
	if node is Button:
		for connection in node.get_signal_connection_list("pressed"):
			var callback: Callable = connection["callable"]
			var target := callback.get_object()
			if target != null and target.is_class("LightmapGIEditorPlugin") and callback.get_method() == &"_bake":
				return target
	for child in node.get_children():
		var found := _find_native_baker(child)
		if found != null:
			return found
	return null


func _fail(message: String) -> void:
	push_error(message)
	print("NATIVE_BASELINE_BAKE " + JSON.stringify({"ok": false, "error": message}))
	get_tree().quit(1)

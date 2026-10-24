@tool
extends EditorPlugin
## RDG (modules/rdg) and the TG GI / fog effects (modules/tg_gi) are engine classes. An editor built without them
## (e.g. the stock Godot that Windows associates with project.godot) reports a parse error in every script that uses
## them; this says why once, with the way out. Only core API here, so it loads in any Godot 4.6 editor.

const REQUIRED_CLASSES: Array[String] = ["RDGCompositorEffect", "RenderGraphConsole", "TGScreenGI", "TGSurfaceFog"]

var _dialog: AcceptDialog


func _enter_tree() -> void:
	var missing := REQUIRED_CLASSES.filter(func(name: String) -> bool: return not ClassDB.class_exists(name))
	if missing.is_empty():
		return
	var message := "This editor is not the rdg engine build: it lacks the engine classes %s, so the project's scripts fail to parse.\nClose it and open the project with tools/open-editor.ps1 (the build comes from tools\\build_engine.bat, see README)." % ", ".join(PackedStringArray(missing))
	push_error(message)
	if DisplayServer.get_name() == "headless":
		return
	_dialog = AcceptDialog.new()
	_dialog.title = "GodotRDG needs the rdg engine build"
	_dialog.dialog_text = message
	_dialog.dialog_autowrap = true
	_dialog.min_size = Vector2i(640, 0)
	EditorInterface.get_base_control().add_child(_dialog)
	_dialog.popup_centered.call_deferred()


func _exit_tree() -> void:
	if is_instance_valid(_dialog):
		_dialog.queue_free()

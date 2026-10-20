extends SceneTree
## Runs the RDG module self tests.
##   godot --path . --script res://tests/run_tests.gd                  (GPU tests, Vulkan)
##   godot --headless --path . --script res://tests/run_tests.gd       (CPU tests only, GPU tests SKIP)
##   godot --path . --script res://tests/run_tests.gd -- --test=MipViews


func _initialize() -> void:
	var filter := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--test="):
			filter = arg.trim_prefix("--test=")

	var tests := RenderGraphTests.new()
	var summary: Dictionary = tests.run_test(filter) if not filter.is_empty() else tests.run_all()
	print("")
	print("RDG tests: %d passed, %d failed, %d skipped (device: %s)" % [summary.passed, summary.failed, summary.skipped, summary.device])
	for result in summary.results:
		if result.status == "FAIL":
			print("  FAIL %s: %s" % [result.name, result.message])
	quit(1 if summary.failed > 0 else 0)

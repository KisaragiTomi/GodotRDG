#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/typed_array.h"

/** Self tests of the RDG module. GPU tests use a local RenderingDevice and are skipped when none is available (headless). */
class RenderGraphTests : public RefCounted {
	GDCLASS(RenderGraphTests, RefCounted);

protected:
	static void _bind_methods();

public:
	PackedStringArray get_test_names() const;
	/** Returns { passed, failed, skipped, results: [{ name, status, message, time_ms }] }. */
	Dictionary run_all();
	Dictionary run_test(const String &p_name);
};

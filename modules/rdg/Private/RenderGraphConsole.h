#pragma once

#include "core/object/object.h"
#include "core/variant/typed_array.h"

class RenderingDevice;

/** Script access to the RDG console variables and pools (UE's console commands). */
class RenderGraphConsole : public Object {
	GDCLASS(RenderGraphConsole, Object);

protected:
	static void _bind_methods();

public:
	static bool set_cvar(const String &p_name, const Variant &p_value);
	static Variant get_cvar(const String &p_name);
	static PackedStringArray get_cvar_list();
	static String get_cvar_help(const String &p_name);

	static void tick_pool_elements();
	static void recompile_shaders();
	static Dictionary get_pool_stats();
	static String get_last_dump_path();
	/** Frees everything RDG cached for a local RenderingDevice; call before freeing the device. */
	static void release_device(Object *p_device);
};

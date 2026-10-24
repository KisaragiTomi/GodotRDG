#include "register_types.h"

#include "TGScreenGI.h"
#include "TGSurfaceFog.h"

#include "core/object/class_db.h"

void initialize_tg_gi_module(ModuleInitializationLevel p_level) {
	// The global shaders register themselves statically and are committed by the rdg module.
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
	GDREGISTER_CLASS(TGScreenGI);
	GDREGISTER_CLASS(TGScreenGIHistory);
	GDREGISTER_CLASS(TGSurfaceFog);
}

void uninitialize_tg_gi_module(ModuleInitializationLevel p_level) {
}

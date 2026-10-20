#include "register_types.h"

#include "Private/RDGEmbeddedShaders.gen.h"
#include "Private/RenderGraphConsole.h"
#include "Tests/RenderGraphTests.h"
#include "Demo/RDGCompositorEffect.h"

#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "RDGDeviceContext.h"
#include "RHICommandList.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"

void initialize_rdg_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		RegisterRDGEmbeddedShaders();
		FShaderTypeRegistration::CommitAll();
		return;
	}
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;

	GDREGISTER_ABSTRACT_CLASS(RenderGraphConsole);
	GDREGISTER_CLASS(RenderGraphTests);
	GDREGISTER_CLASS(RDGCompositorEffect);
	IConsoleManager::Get().ApplyCommandLine(OS::get_singleton()->get_cmdline_user_args());
}

void uninitialize_rdg_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) return;
	// RenderingServer is still alive at this point (it is finalized after the server modules).
	RenderingServer *RS = RenderingServer::get_singleton();
	if (RS && RS->is_on_render_thread()) FRDGDeviceContext::ReleaseAll();
	FRHICommandListExecutor::ShutdownImmediateCommandList();
	RHIReleaseVertexDeclarations();
}

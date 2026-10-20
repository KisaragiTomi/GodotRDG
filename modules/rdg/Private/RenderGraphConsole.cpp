#include "RenderGraphConsole.h"

#include "HAL/IConsoleManager.h"
#include "RDGDeviceContext.h"
#include "RenderGraphBuilder.h"

#include "servers/rendering/rendering_device.h"

void RenderGraphConsole::_bind_methods() {
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("set_cvar", "name", "value"), &RenderGraphConsole::set_cvar);
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("get_cvar", "name"), &RenderGraphConsole::get_cvar);
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("get_cvar_list"), &RenderGraphConsole::get_cvar_list);
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("get_cvar_help", "name"), &RenderGraphConsole::get_cvar_help);
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("tick_pool_elements"), &RenderGraphConsole::tick_pool_elements);
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("recompile_shaders"), &RenderGraphConsole::recompile_shaders);
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("get_pool_stats"), &RenderGraphConsole::get_pool_stats);
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("get_last_dump_path"), &RenderGraphConsole::get_last_dump_path);
	ClassDB::bind_static_method("RenderGraphConsole", D_METHOD("release_device", "device"), &RenderGraphConsole::release_device);
}

bool RenderGraphConsole::set_cvar(const String &p_name, const Variant &p_value) {
	IConsoleVariable *Variable = IConsoleManager::Get().FindConsoleVariable(p_name.utf8().get_data());
	ERR_FAIL_NULL_V_MSG(Variable, false, "[RDG] Unknown console variable: " + p_name);
	if (p_value.get_type() == Variant::FLOAT) Variable->Set((float)p_value);
	else if (p_value.get_type() == Variant::STRING) Variable->Set(String(p_value).utf8().get_data());
	else Variable->Set((int32)(int64_t)p_value);
	return true;
}

Variant RenderGraphConsole::get_cvar(const String &p_name) {
	IConsoleVariable *Variable = IConsoleManager::Get().FindConsoleVariable(p_name.utf8().get_data());
	ERR_FAIL_NULL_V_MSG(Variable, Variant(), "[RDG] Unknown console variable: " + p_name);
	const String Value = Variable->GetString();
	if (Value.is_valid_int()) return Variable->GetInt();
	return Variable->GetFloat();
}

PackedStringArray RenderGraphConsole::get_cvar_list() {
	PackedStringArray Result;
	IConsoleManager::Get().ForEachConsoleVariable([&](IConsoleVariable *Variable) { Result.push_back(String::utf8(Variable->GetName())); });
	Result.sort();
	return Result;
}

String RenderGraphConsole::get_cvar_help(const String &p_name) {
	IConsoleVariable *Variable = IConsoleManager::Get().FindConsoleVariable(p_name.utf8().get_data());
	return Variable ? String::utf8(Variable->GetHelp()) : String();
}

void RenderGraphConsole::tick_pool_elements() {
	FRDGBuilder::TickPoolElements();
}

void RenderGraphConsole::recompile_shaders() {
	if (FRDGDeviceContext *Context = FRDGDeviceContext::Find(FRDGDeviceContext::GetMainDevice())) Context->RecompileShaders();
}

Dictionary RenderGraphConsole::get_pool_stats() {
	Dictionary Result;
	FRDGDeviceContext *Context = FRDGDeviceContext::Find(FRDGDeviceContext::GetMainDevice());
	if (!Context) return Result;
	const FRDGPoolStats &Textures = Context->RenderTargetPool.GetStats();
	const FRDGPoolStats &Buffers = Context->BufferPool.GetStats();
	const FRDGDeviceContext::FCacheStats Caches = Context->GetCacheStats();
	Result["frame"] = (int64_t)Context->GetFrameNumber();
	Result["pooled_textures"] = Context->RenderTargetPool.GetNumElements();
	Result["pooled_texture_bytes"] = (int64_t)Textures.MemoryBytes;
	Result["textures_created_total"] = (int64_t)Textures.TotalCreated;
	Result["textures_reused_total"] = (int64_t)Textures.TotalReused;
	Result["pooled_buffers"] = Context->BufferPool.GetNumElements();
	Result["buffers_created_total"] = (int64_t)Buffers.TotalCreated;
	Result["compiled_shaders"] = Context->GlobalShaderMap.GetNumCompiledShaders();
	Result["uniform_sets"] = Caches.NumUniformSets;
	Result["framebuffers"] = Caches.NumFramebuffers;
	Result["pipelines"] = Caches.NumPipelines;
	const FRHITransientAllocationStats &Transient = Context->TransientAllocator.GetStats();
	Result["transient_supported"] = Context->TransientAllocator.IsSupported();
	Result["transient_heaps"] = Transient.NumHeaps;
	Result["transient_heap_bytes"] = (int64_t)Transient.HeapMemoryBytes;
	Result["transient_textures"] = Transient.NumCachedTextures;
	Result["transient_buffers"] = Transient.NumCachedBuffers;
	Result["transient_allocations"] = Transient.NumAllocations;
	Result["transient_allocated_bytes"] = (int64_t)Transient.AllocatedBytes;
	Result["transient_peak_bytes"] = (int64_t)Transient.PeakUsedBytes;
	Result["transient_failed_placements"] = Transient.NumFailedPlacements;
	Result["transient_resources_created_total"] = (int64_t)(Transient.TotalTexturesCreated + Transient.TotalBuffersCreated);
	return Result;
}

String RenderGraphConsole::get_last_dump_path() {
	return FRDGBuilder::GetLastDumpPath();
}

void RenderGraphConsole::release_device(Object *p_device) {
	RenderingDevice *Device = Object::cast_to<RenderingDevice>(p_device);
	ERR_FAIL_NULL(Device);
	FRDGDeviceContext::Release(Device);
}

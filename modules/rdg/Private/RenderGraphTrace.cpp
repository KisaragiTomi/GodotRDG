#include "RenderGraphBuilder.h"
#include "RenderGraphPrivate.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

namespace {
String EscapeDot(const String &In) {
	return In.replace("\\", "\\\\").replace("\"", "\\\"");
}

String GetPassFlagsString(ERDGPassFlags Flags) {
	String Result;
	auto Add = [&](ERDGPassFlags Flag, const char *Name) {
		if (!EnumHasAnyFlags(Flags, Flag)) return;
		if (!Result.is_empty()) Result += "|";
		Result += Name;
	};
	Add(ERDGPassFlags::Raster, "Raster");
	Add(ERDGPassFlags::Compute, "Compute");
	Add(ERDGPassFlags::AsyncCompute, "AsyncCompute");
	Add(ERDGPassFlags::Copy, "Copy");
	Add(ERDGPassFlags::NeverCull, "NeverCull");
	Add(ERDGPassFlags::SkipRenderPass, "SkipRenderPass");
	Add(ERDGPassFlags::NeverMerge, "NeverMerge");
	return Result.is_empty() ? String("None") : Result;
}
} //namespace

String RDGPrivate::DumpGraph(const FRDGBuilder &GraphBuilder) {
	static uint32 DumpIndex = 0;
	const String Directory = "user://rdg";
	Ref<DirAccess> Dir = DirAccess::create(DirAccess::ACCESS_USERDATA);
	if (Dir.is_valid()) Dir->make_dir_recursive(Directory);

	String GraphName = String::utf8(GraphBuilder.GetName()).validate_filename().replace(" ", "_");
	const String Path = vformat("%s/%s_%04d.dot", Directory, GraphName, DumpIndex++);

	String Dot = "digraph RDG {\n";
	Dot += vformat("\tlabel=\"%s\";\n\trankdir=LR;\n\tnode [fontname=\"Consolas\", fontsize=10];\n", EscapeDot(String::utf8(GraphBuilder.GetName())));

	const LocalVector<FRDGPass *> &Passes = GraphBuilder.GetPasses();
	bool bHasPrologueFork = false;
	bool bHasEpilogueJoin = false;
	for (const FRDGPass *Pass : Passes) {
		String Label = vformat("#%d %s\\n[%s]", Pass->GetHandle().GetIndex(), EscapeDot(String::utf8(Pass->GetName())), GetPassFlagsString(Pass->GetFlags()));
		if (Pass->SkipRenderPassBegin() || Pass->SkipRenderPassEnd()) Label += "\\n(merged render pass)";
		if (Pass->IsAsyncCompute()) Label += "\\nasync compute pipe";
		if (Pass->IsGraphicsFork()) Label += "\\nfork";
		if (Pass->IsGraphicsJoin()) Label += "\\njoin";
		if (Pass->IsCulled()) Label += "\\nCULLED";
		const char *Style = Pass->IsCulled() ? "style=\"dashed,filled\", fillcolor=\"#dddddd\", fontcolor=\"#888888\"" : (Pass->IsAsyncCompute() ? "style=filled, fillcolor=\"#ffd8b0\"" : "style=filled, fillcolor=\"#cfe8ff\"");
		Dot += vformat("\tP%d [shape=box, %s, label=\"%s\"];\n", Pass->GetHandle().GetIndex(), Style, Label);
		if (Pass->IsCulled() || !Pass->IsAsyncCompute()) continue;
		bHasPrologueFork |= Pass->IsAsyncComputeBegin() && Pass->GetGraphicsForkPass().IsNull();
		bHasEpilogueJoin |= Pass->IsAsyncComputeEnd() && Pass->GetGraphicsJoinPass().IsNull();
	}

	// Async compute fork / join fences.
	if (bHasPrologueFork) Dot += "\tPrologue [shape=circle, label=\"prologue\"];\n";
	if (bHasEpilogueJoin) Dot += "\tEpilogue [shape=circle, label=\"epilogue\"];\n";
	for (const FRDGPass *Pass : Passes) {
		if (Pass->IsCulled() || !Pass->IsAsyncCompute()) continue;
		const uint32 PassIndex = Pass->GetHandle().GetIndex();
		if (Pass->IsAsyncComputeBegin()) {
			const String Fork = Pass->GetGraphicsForkPass().IsNull() ? String("Prologue") : vformat("P%d", Pass->GetGraphicsForkPass().GetIndex());
			Dot += vformat("\t%s -> P%d [label=\"fork\", style=bold, color=\"#e67e22\"];\n", Fork, PassIndex);
		}
		if (Pass->IsAsyncComputeEnd()) {
			const String Join = Pass->GetGraphicsJoinPass().IsNull() ? String("Epilogue") : vformat("P%d", Pass->GetGraphicsJoinPass().GetIndex());
			Dot += vformat("\tP%d -> %s [label=\"join\", style=bold, color=\"#e67e22\"];\n", PassIndex, Join);
		}
	}

	const LocalVector<FRDGTexture *> &Textures = GraphBuilder.GetTextures();
	for (uint32 Index = 0; Index < Textures.size(); Index++) {
		const FRDGTexture *Texture = Textures[Index];
		const RID Allocated = GraphBuilder.GetAllocatedTextureRID(const_cast<FRDGTexture *>(Texture));
		String Label = vformat("%s\\n%dx%d %s", EscapeDot(String::utf8(Texture->Name)), Texture->Desc.Extent.X, Texture->Desc.Extent.Y, GetPixelFormatInfo(Texture->Desc.Format).Name);
		if (Texture->Desc.NumMips > 1) Label += vformat(" %d mips", Texture->Desc.NumMips);
		if (Texture->IsExternal()) Label += "\\nexternal";
		if (Texture->IsExtracted()) Label += "\\nextracted";
		if (Allocated.is_valid()) Label += vformat("\\nRD texture %d", (int64)Allocated.get_id());
		Dot += vformat("\tT%d [shape=ellipse, style=filled, fillcolor=\"%s\", label=\"%s\"];\n", Index, Texture->IsExternal() ? "#ffe2b3" : "#e6f5d0", Label);
	}

	const LocalVector<FRDGBuffer *> &Buffers = GraphBuilder.GetBuffers();
	for (uint32 Index = 0; Index < Buffers.size(); Index++) {
		const FRDGBuffer *Buffer = Buffers[Index];
		const RID Allocated = GraphBuilder.GetAllocatedBufferRID(const_cast<FRDGBuffer *>(Buffer));
		String Label = vformat("%s\\n%d x %d bytes", EscapeDot(String::utf8(Buffer->Name)), Buffer->Desc.NumElements, Buffer->Desc.BytesPerElement);
		if (Buffer->IsExternal()) Label += "\\nexternal";
		if (Buffer->IsExtracted()) Label += "\\nextracted";
		if (Allocated.is_valid()) Label += vformat("\\nRD buffer %d", (int64)Allocated.get_id());
		Dot += vformat("\tB%d [shape=parallelogram, style=filled, fillcolor=\"%s\", label=\"%s\"];\n", Index, Buffer->IsExternal() ? "#ffe2b3" : "#f3e5f5", Label);
	}

	auto FindTexture = [&](const FRDGTexture *Texture) -> int32 {
		for (uint32 Index = 0; Index < Textures.size(); Index++) {
			if (Textures[Index] == Texture) return (int32)Index;
		}
		return -1;
	};
	auto FindBuffer = [&](const FRDGBuffer *Buffer) -> int32 {
		for (uint32 Index = 0; Index < Buffers.size(); Index++) {
			if (Buffers[Index] == Buffer) return (int32)Index;
		}
		return -1;
	};

	for (const FRDGPass *Pass : Passes) {
		const uint32 PassIndex = Pass->GetHandle().GetIndex();
		const char *EdgeStyle = Pass->IsCulled() ? ", style=dashed, color=\"#aaaaaa\"" : "";
		for (const FRDGPass::FTextureState &State : Pass->GetTextureStates()) {
			const int32 TextureIndex = FindTexture(State.Texture);
			if (TextureIndex < 0) continue;
			const String Access = GetRHIAccessName(State.Access);
			if (IsWritableAccess(State.Access)) Dot += vformat("\tP%d -> T%d [label=\"%s\", color=\"#c0392b\"%s];\n", PassIndex, TextureIndex, Access, EdgeStyle);
			if (!IsWritableAccess(State.Access) || EnumHasAnyFlags(State.Access, ERHIAccess::ReadOnlyMask)) Dot += vformat("\tT%d -> P%d [label=\"%s\"%s];\n", TextureIndex, PassIndex, Access, EdgeStyle);
		}
		for (const FRDGPass::FBufferState &State : Pass->GetBufferStates()) {
			const int32 BufferIndex = FindBuffer(State.Buffer);
			if (BufferIndex < 0) continue;
			const String Access = GetRHIAccessName(State.Access);
			if (IsWritableAccess(State.Access)) Dot += vformat("\tP%d -> B%d [label=\"%s\", color=\"#c0392b\"%s];\n", PassIndex, BufferIndex, Access, EdgeStyle);
			if (!IsWritableAccess(State.Access) || EnumHasAnyFlags(State.Access, ERHIAccess::ReadOnlyMask)) Dot += vformat("\tB%d -> P%d [label=\"%s\"%s];\n", BufferIndex, PassIndex, Access, EdgeStyle);
		}
	}

	const FRDGBuilderStats &Stats = GraphBuilder.GetStats();
	Dot += vformat("\tStats [shape=note, label=\"passes: %d (culled %d, merged %d)\\ntextures: %d (transient %d -> %d RD textures)\\nbuffers: %d (transient %d -> %d RD buffers)\\nasync compute: %d passes, %d forks, %d joins\"];\n",
			Stats.NumPasses, Stats.NumCulledPasses, Stats.NumMergedRenderPasses, Stats.NumTextures, Stats.NumTransientTextures, Stats.NumPhysicalTransientTextures, Stats.NumBuffers, Stats.NumTransientBuffers, Stats.NumPhysicalTransientBuffers, Stats.NumAsyncComputePasses, Stats.NumAsyncComputeForks, Stats.NumAsyncComputeJoins);
	Dot += "}\n";

	Ref<FileAccess> File = FileAccess::open(Path, FileAccess::WRITE);
	if (File.is_null()) {
		ERR_PRINT("[RDG] Unable to write graph dump to " + Path);
		return String();
	}
	File->store_string(Dot);
	File->close();
	print_line("[RDG] Graph dumped to " + ProjectSettings::get_singleton()->globalize_path(Path));
	return Path;
}

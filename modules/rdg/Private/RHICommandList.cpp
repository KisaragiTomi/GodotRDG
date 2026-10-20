#include "RHICommandList.h"

#include "RDGDeviceContext.h"
#include "RenderGraphPrivate.h"
#include "RenderGraphResources.h"

#include "core/config/project_settings.h"
#include "core/object/callable_method_pointer.h"
#include "servers/rendering/rendering_device.h"

TAutoConsoleVariable<int32> CVarRHICmdParallelTranslateEnable(
		"r.RHICmd.ParallelTranslate.Enable", -1,
		"When on, allows the RenderingDevice to translate the command graph of a frame into command buffers on worker threads (RenderingDevice::parallel_translate_set_enabled()).\n"
		" -1: the project setting rendering/rendering_device/parallel_translate (default);\n"
		" 0: everything is translated on the render thread;\n"
		" 1: parallel translation;",
		ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarRHICmdParallelTranslateMaxCommandsPerTranslate(
		"r.RHICmd.ParallelTranslate.MaxCommandsPerTranslate", 256,
		"When parallel translation is enabled, specifies the target maximum number of commands in a single translation job. "
		"Compute and draw lists count one command per recorded instruction (about one driver call). "
		"A value of 0 means 'no limit': translate jobs are not split. A value less than 0 means 'always split'.",
		ECVF_RenderThreadSafe);

namespace {
struct FDeferredRHICommand {
	RenderingDevice *Device = nullptr;
	std::function<void(RenderingDevice *)> Command;
};

void RunDeferredRHICommand(void *Object, uint64_t) {
	FDeferredRHICommand *Deferred = static_cast<FDeferredRHICommand *>(Object);
	Deferred->Command(Deferred->Device);
	memdelete(Deferred);
}
} //namespace

void RDGPrivate::EnqueueRenderThreadCommand(RenderingDevice *Device, std::function<void(RenderingDevice *)> &&Command) {
	ERR_FAIL_NULL(Device);
	if (!Device->recording_context_get_current()) {
		Command(Device);
		return;
	}
	FDeferredRHICommand *Deferred = memnew(FDeferredRHICommand);
	Deferred->Device = Device;
	Deferred->Command = std::move(Command);
	Device->recording_context_add_callback(&RunDeferredRHICommand, Deferred, 0);
}

FRHIComputeCommandList::FRHIComputeCommandList(RenderingDevice *InDevice) :
		Device(InDevice) {}

FRHIComputeCommandList::~FRHIComputeCommandList() {
	if (Device && ComputeList >= 0) Device->compute_list_end();
}

RenderingDevice::ComputeListID FRHIComputeCommandList::GetComputeList() {
	ERR_FAIL_NULL_V(Device, -1);
	ERR_FAIL_COND_V_MSG(DrawList >= 0, -1, "[RDG] Compute work can't be recorded inside a raster pass (Godot can't nest compute lists in draw lists).");
	if (ComputeList < 0) ComputeList = Device->compute_list_begin();
	return ComputeList;
}

void FRHIComputeCommandList::FlushComputeList() {
	if (ComputeList >= 0) {
		Device->compute_list_end();
		ComputeList = -1;
	}
	CurrentComputeShader = nullptr;
	ResetLastUniformSets();
}

void FRHIComputeCommandList::ResetLastUniformSets() {
	for (FLastUniformSet &Last : LastUniformSets) Last = FLastUniformSet();
}

void FRHIComputeCommandList::ResetBindings() {
	PendingUniforms.clear();
	PushConstantData.clear();
	PushConstantSize = 0;
	bBindingError = false;
}

FRHIComputeCommandList::FPendingUniform &FRHIComputeCommandList::FindOrAddPendingUniform(uint32 Set, uint32 Binding) {
	for (FPendingUniform &Pending : PendingUniforms) {
		if (Pending.Set == Set && Pending.Binding == Binding) return Pending;
	}
	FPendingUniform Pending;
	Pending.Set = Set;
	Pending.Binding = Binding;
	PendingUniforms.push_back(Pending);
	return PendingUniforms[PendingUniforms.size() - 1];
}

void FRHIComputeCommandList::SetComputePipelineState(FRHIComputeShader *ComputeShader) {
	ResetBindings();
	CurrentComputeShader = ComputeShader;
	if (!ComputeShader || !ComputeShader->Shader) {
		bBindingError = true;
		ERR_FAIL_MSG("[RDG] SetComputePipelineState called with an invalid compute shader.");
	}
	const FShaderCompiledData *Data = ComputeShader->Shader->GetCompiledData();
	const RenderingDevice::ComputeListID List = GetComputeList();
	if (List < 0) {
		bBindingError = true;
		return;
	}
	Device->compute_list_bind_compute_pipeline(List, Data->ComputePipeline);
	CurrentComputeShader = ComputeShader;
}

void FRHIComputeCommandList::SetPushConstantData(const void *Data, uint32 Size, uint32 Offset) {
	if (PushConstantData.size() < Offset + Size) {
		const uint32 OldSize = PushConstantData.size();
		PushConstantData.resize(Offset + Size);
		for (uint32 Index = OldSize; Index < PushConstantData.size(); Index++) PushConstantData[Index] = 0;
	}
	memcpy(PushConstantData.ptr() + Offset, Data, Size);
}

void FRHIComputeCommandList::SetShaderParameters(const FShader *Shader, const void *ParametersData) {
	if (!Shader || !ParametersData) {
		bBindingError = true;
		ERR_FAIL_MSG("[RDG] SetShaderParameters called with a null shader or parameter struct.");
	}
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Device);
	const FShaderParameterBindings &Bindings = Shader->GetParameterBindings();
	const uint8 *Base = reinterpret_cast<const uint8 *>(ParametersData);

	for (const FShaderParameterBindings::FResourceBinding &Binding : Bindings.ResourceBindings) {
		const uint8 *StructBase = Base;
		if (Binding.ParentUniformBuffer >= 0) {
			const FRDGUniformBufferBinding *UniformBuffer = reinterpret_cast<const FRDGUniformBufferBinding *>(Base + Binding.ParentUniformBuffer);
			if (!UniformBuffer->GetUniformBuffer()) {
				ERR_PRINT(vformat("[RDG] Shader '%s': uniform buffer holding '%s' is null.", Shader->GetDebugName(), Binding.Name));
				bBindingError = true;
				continue;
			}
			StructBase = reinterpret_cast<const uint8 *>(UniformBuffer->GetUniformBuffer()->GetContents());
		}

		FPendingUniform &Pending = FindOrAddPendingUniform(Binding.Set, Binding.Binding);
		Pending.Type = Binding.UniformType;
		Pending.IDs.clear();

		if (Binding.UniformType == RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
			FRHISamplerState *Sampler = Binding.SamplerByteOffset != UINT32_MAX ? *reinterpret_cast<FRHISamplerState *const *>(Base + Binding.SamplerByteOffset) : nullptr;
			Pending.IDs.push_back(Context->GetSampler(Sampler));
		}

		if (Binding.BaseType == UBMT_RDG_UNIFORM_BUFFER) {
			const FRDGUniformBufferBinding *UniformBuffer = reinterpret_cast<const FRDGUniformBufferBinding *>(StructBase + Binding.ByteOffset);
			FRDGUniformBuffer *RDGUniformBuffer = UniformBuffer->GetUniformBuffer();
			const RID BufferRID = RDGUniformBuffer ? RDGUniformBuffer->GetOrCreateRDBuffer(Binding) : RID();
			if (!BufferRID.is_valid()) {
				ERR_PRINT(vformat("[RDG] Shader '%s': uniform buffer '%s' is not set.", Shader->GetDebugName(), Binding.Name));
				bBindingError = true;
				continue;
			}
			Pending.IDs.push_back(BufferRID);
			continue;
		}

		const uint32 Count = FMath::Max(1u, Binding.NumElements);
		for (uint32 Element = 0; Element < Count; Element++) {
			const void *Resource = *reinterpret_cast<const void *const *>(StructBase + Binding.ByteOffset + Element * sizeof(void *));
			RID Rid;
			if (Resource) {
				switch (Binding.BaseType) {
					case UBMT_RDG_TEXTURE: {
						const FRDGTexture *Texture = reinterpret_cast<const FRDGTexture *>(Resource);
						if (FRHITexture *RHI = Texture->GetRHI()) Rid = RHI->GetRID();
					} break;
					case UBMT_RDG_TEXTURE_SRV:
					case UBMT_RDG_TEXTURE_NON_PIXEL_SRV: {
						if (FRHIShaderResourceView *SRV = reinterpret_cast<const FRDGTextureSRV *>(Resource)->GetRHI()) Rid = SRV->ViewRID;
					} break;
					case UBMT_RDG_TEXTURE_UAV: {
						if (FRHIUnorderedAccessView *UAV = reinterpret_cast<const FRDGTextureUAV *>(Resource)->GetRHI()) Rid = UAV->ViewRID;
					} break;
					case UBMT_RDG_BUFFER_SRV: {
						if (FRHIShaderResourceView *SRV = reinterpret_cast<const FRDGBufferSRV *>(Resource)->GetRHI()) Rid = SRV->ViewRID;
					} break;
					case UBMT_RDG_BUFFER_UAV: {
						if (FRHIUnorderedAccessView *UAV = reinterpret_cast<const FRDGBufferUAV *>(Resource)->GetRHI()) Rid = UAV->ViewRID;
					} break;
					case UBMT_TEXTURE:
						Rid = reinterpret_cast<const FRHITexture *>(Resource)->GetRID();
						break;
					case UBMT_SRV:
						Rid = reinterpret_cast<const FRHIShaderResourceView *>(Resource)->ViewRID;
						break;
					case UBMT_UAV:
						Rid = reinterpret_cast<const FRHIUnorderedAccessView *>(Resource)->ViewRID;
						break;
					case UBMT_SAMPLER:
						Rid = Context->GetSampler(reinterpret_cast<const FRHISamplerState *>(Resource));
						break;
					default:
						break;
				}
			} else if (Binding.BaseType == UBMT_SAMPLER) {
				Rid = Context->GetDefaultSampler();
			}

			if (!Rid.is_valid()) {
				ERR_PRINT(vformat("[RDG] Shader '%s': parameter '%s' is null or has no RHI resource.", Shader->GetDebugName(), Binding.Name));
				bBindingError = true;
				break;
			}
			if (Binding.UniformType == RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE && Element > 0) {
				// Arrays of combined samplers interleave sampler + texture.
				Pending.IDs.push_back(Pending.IDs[0]);
			}
			Pending.IDs.push_back(Rid);
		}
	}

	if (Bindings.PushConstantSize > 0) {
		if (PushConstantData.size() < Bindings.PushConstantSize) {
			const uint32 OldSize = PushConstantData.size();
			PushConstantData.resize(Bindings.PushConstantSize);
			for (uint32 Index = OldSize; Index < PushConstantData.size(); Index++) PushConstantData[Index] = 0;
		}
		PushConstantSize = FMath::Max(PushConstantSize, Bindings.PushConstantSize);
		for (const FShaderParameterBindings::FResourceBinding::FConstantCopy &Copy : Bindings.PushConstantCopies) {
			for (uint32 Element = 0; Element < Copy.NumElements; Element++) {
				const uint8 *Src = Base + Copy.SrcOffset + Element * Copy.SrcStride;
				uint8 *Dst = PushConstantData.ptr() + Copy.DstOffset + Element * Copy.DstStride;
				if (Copy.DstOffset + Element * Copy.DstStride + Copy.Size > PushConstantData.size()) break;
				if (Copy.bBool) {
					const uint32 Value = *reinterpret_cast<const bool *>(Src) ? 1u : 0u;
					memcpy(Dst, &Value, 4);
				} else {
					memcpy(Dst, Src, Copy.Size);
				}
			}
		}
	}
}

bool FRHIComputeCommandList::FlushShaderBindings(RID ShaderRID, const FShaderReflectionInfo &Reflection, const String &ShaderName, bool bGraphics) {
	if (bBindingError) {
		NumBindingErrors++;
		ERR_PRINT(vformat("[RDG] Skipping %s of '%s' because of previous binding errors.", bGraphics ? "draw" : "dispatch", ShaderName));
		return false;
	}
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Device);
	const uint32 NumSets = Reflection.GetNumSets();
	thread_local LocalVector<RenderingDevice::Uniform> Uniforms;
	for (uint32 Set = 0; Set < NumSets; Set++) {
		Uniforms.clear();
		for (const FShaderResourceReflection &Resource : Reflection.Resources) {
			if (Resource.Set != Set) continue;
			FPendingUniform *Pending = nullptr;
			for (FPendingUniform &Candidate : PendingUniforms) {
				if (Candidate.Set == Set && Candidate.Binding == Resource.Binding) Pending = &Candidate;
			}
			if (!Pending || Pending->IDs.is_empty()) {
				ERR_PRINT(vformat("[RDG] Shader '%s' has unbound parameter '%s' (set %d, binding %d). Add a member with that name to the parameter struct.", ShaderName, Resource.Name.is_empty() ? Resource.BlockName : Resource.Name, Set, Resource.Binding));
				NumBindingErrors++;
				return false;
			}
			RenderingDevice::Uniform Uniform;
			Uniform.uniform_type = Resource.UniformType;
			Uniform.binding = Resource.Binding;
			for (const RID &Id : Pending->IDs) Uniform.append_id(Id);
			Uniforms.push_back(Uniform);
		}
		if (Uniforms.is_empty()) continue;
		const uint64 Key = FRDGDeviceContext::GetUniformSetKey(ShaderRID, Set, Uniforms);
		RID UniformSet;
		if (Set < NumLastUniformSets && LastUniformSets[Set].Key == Key && LastUniformSets[Set].UniformSet.is_valid()) {
			UniformSet = LastUniformSets[Set].UniformSet;
		} else {
			UniformSet = Context->GetUniformSet(Key, ShaderRID, Set, Uniforms);
			if (Set < NumLastUniformSets) {
				LastUniformSets[Set].Key = Key;
				LastUniformSets[Set].UniformSet = UniformSet;
			}
		}
		if (!UniformSet.is_valid()) {
			ERR_PRINT(vformat("[RDG] Failed to create uniform set %d for shader '%s'.", Set, ShaderName));
			NumBindingErrors++;
			return false;
		}
		if (bGraphics) Device->draw_list_bind_uniform_set(DrawList, UniformSet, Set);
		else Device->compute_list_bind_uniform_set(ComputeList, UniformSet, Set);
	}

	if (Reflection.PushConstantSize > 0) {
		if (PushConstantData.size() < Reflection.PushConstantSize) {
			const uint32 OldSize = PushConstantData.size();
			PushConstantData.resize(Reflection.PushConstantSize);
			for (uint32 Index = OldSize; Index < PushConstantData.size(); Index++) PushConstantData[Index] = 0;
		}
		if (bGraphics) Device->draw_list_set_push_constant(DrawList, PushConstantData.ptr(), Reflection.PushConstantSize);
		else Device->compute_list_set_push_constant(ComputeList, PushConstantData.ptr(), Reflection.PushConstantSize);
	}
	return true;
}

void FRHIComputeCommandList::DispatchComputeShader(uint32 ThreadGroupCountX, uint32 ThreadGroupCountY, uint32 ThreadGroupCountZ) {
	if (!CurrentComputeShader || !CurrentComputeShader->Shader || ComputeList < 0) {
		NumBindingErrors++;
		ERR_FAIL_MSG("[RDG] DispatchComputeShader without a valid compute pipeline state.");
	}
	const FShaderCompiledData *Data = CurrentComputeShader->Shader->GetCompiledData();
	if (!FlushShaderBindings(Data->ShaderRID, Data->Reflection, Data->DebugName, false)) return;
	if (ThreadGroupCountX == 0 || ThreadGroupCountY == 0 || ThreadGroupCountZ == 0) return;
	Device->compute_list_dispatch(ComputeList, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void FRHIComputeCommandList::DispatchIndirectComputeShader(FRHIBuffer *ArgumentBuffer, uint32 ArgumentOffset) {
	if (!CurrentComputeShader || !CurrentComputeShader->Shader || ComputeList < 0 || !ArgumentBuffer) {
		NumBindingErrors++;
		ERR_FAIL_MSG("[RDG] DispatchIndirectComputeShader without a valid compute pipeline state or argument buffer.");
	}
	const FShaderCompiledData *Data = CurrentComputeShader->Shader->GetCompiledData();
	if (!FlushShaderBindings(Data->ShaderRID, Data->Reflection, Data->DebugName, false)) return;
	Device->compute_list_dispatch_indirect(ComputeList, ArgumentBuffer->GetRID(), ArgumentOffset);
}

void FRHIComputeCommandList::CopyTexture(FRHITexture *SourceTexture, FRHITexture *DestTexture, const FRHICopyTextureInfo &CopyInfo) {
	ERR_FAIL_COND_MSG(!SourceTexture || !DestTexture, "[RDG] CopyTexture with null texture.");
	ERR_FAIL_COND_MSG(DrawList >= 0, "[RDG] CopyTexture can't be called inside a raster pass.");
	FlushComputeList();
	for (uint32 Mip = 0; Mip < FMath::Max(1u, CopyInfo.NumMips); Mip++) {
		const uint32 SrcMip = CopyInfo.SourceMipIndex + Mip;
		const uint32 DstMip = CopyInfo.DestMipIndex + Mip;
		FIntVector Size = CopyInfo.Size;
		if (Size.X <= 0 || Size.Y <= 0) {
			const FIntPoint MipExtent = SourceTexture->GetDesc().GetMipExtent(SrcMip);
			Size = FIntVector(MipExtent.X - CopyInfo.SourcePosition.X, MipExtent.Y - CopyInfo.SourcePosition.Y, SourceTexture->GetDesc().IsTexture3D() ? FMath::Max(1, (int32)SourceTexture->GetDesc().Depth >> SrcMip) : 1);
		} else if (Mip > 0) {
			Size = FIntVector(FMath::Max(1, Size.X >> Mip), FMath::Max(1, Size.Y >> Mip), FMath::Max(1, Size.Z >> Mip));
		}
		if (Size.Z <= 0) Size.Z = 1;
		for (uint32 Slice = 0; Slice < FMath::Max(1u, CopyInfo.NumSlices); Slice++) {
			const RID SourceRID = SourceTexture->GetRID();
			const RID DestRID = DestTexture->GetRID();
			const Vector3 From(CopyInfo.SourcePosition.X >> Mip, CopyInfo.SourcePosition.Y >> Mip, CopyInfo.SourcePosition.Z);
			const Vector3 To(CopyInfo.DestPosition.X >> Mip, CopyInfo.DestPosition.Y >> Mip, CopyInfo.DestPosition.Z);
			const Vector3 CopySize(Size.X, Size.Y, Size.Z);
			const uint32 SourceSlice = CopyInfo.SourceSliceIndex + Slice;
			const uint32 DestSlice = CopyInfo.DestSliceIndex + Slice;
			RDGPrivate::EnqueueRenderThreadCommand(Device, [=](RenderingDevice *RD) { RD->texture_copy(SourceRID, DestRID, From, To, CopySize, SrcMip, DstMip, SourceSlice, DestSlice); });
		}
	}
}

void FRHIComputeCommandList::CopyBufferRegion(FRHIBuffer *DestBuffer, uint64 DstOffset, FRHIBuffer *SourceBuffer, uint64 SrcOffset, uint64 NumBytes) {
	ERR_FAIL_COND_MSG(!SourceBuffer || !DestBuffer, "[RDG] CopyBufferRegion with null buffer.");
	ERR_FAIL_COND_MSG(DrawList >= 0, "[RDG] CopyBufferRegion can't be called inside a raster pass.");
	FlushComputeList();
	const RID SourceRID = SourceBuffer->GetRID();
	const RID DestRID = DestBuffer->GetRID();
	RDGPrivate::EnqueueRenderThreadCommand(Device, [=](RenderingDevice *RD) { RD->buffer_copy(SourceRID, DestRID, (uint32)SrcOffset, (uint32)DstOffset, (uint32)NumBytes); });
}

void FRHIComputeCommandList::ClearTexture(FRHITexture *Texture, const FLinearColor &Color, uint32 MipIndex, uint32 NumMips, uint32 FirstSlice, uint32 NumSlices) {
	ERR_FAIL_NULL(Texture);
	ERR_FAIL_COND_MSG(DrawList >= 0, "[RDG] ClearTexture can't be called inside a raster pass.");
	FlushComputeList();
	const RID TextureRID = Texture->GetRID();
	RDGPrivate::EnqueueRenderThreadCommand(Device, [=](RenderingDevice *RD) { RD->texture_clear(TextureRID, Color, MipIndex, FMath::Max(1u, NumMips), FirstSlice, FMath::Max(1u, NumSlices)); });
}

void FRHIComputeCommandList::ClearUAVFloat(FRHIUnorderedAccessView *UnorderedAccessViewRHI, const FVector4f &Values) {
	ERR_FAIL_NULL(UnorderedAccessViewRHI);
	if (UnorderedAccessViewRHI->bIsBuffer) {
		uint32 Bits = 0;
		memcpy(&Bits, &Values.X, 4);
		RDGPrivate::ClearBufferUAV(*this, static_cast<FRHIBuffer *>(UnorderedAccessViewRHI->Resource), Bits);
		return;
	}
	FRHITexture *Texture = static_cast<FRHITexture *>(UnorderedAccessViewRHI->Resource);
	ClearTexture(Texture, FLinearColor(Values.X, Values.Y, Values.Z, Values.W), UnorderedAccessViewRHI->MipLevel, 1, 0, Texture->GetDesc().GetRDLayerCount());
}

void FRHIComputeCommandList::ClearUAVUint(FRHIUnorderedAccessView *UnorderedAccessViewRHI, const FUintVector4 &Values) {
	ERR_FAIL_NULL(UnorderedAccessViewRHI);
	if (UnorderedAccessViewRHI->bIsBuffer) {
		RDGPrivate::ClearBufferUAV(*this, static_cast<FRHIBuffer *>(UnorderedAccessViewRHI->Resource), Values.X);
		return;
	}
	// vkCmdClearColorImage reads the clear value as a union: pass the uint bit patterns through the float components.
	FLinearColor Color;
	memcpy(&Color.R, &Values.X, 4);
	memcpy(&Color.G, &Values.Y, 4);
	memcpy(&Color.B, &Values.Z, 4);
	memcpy(&Color.A, &Values.W, 4);
	FRHITexture *Texture = static_cast<FRHITexture *>(UnorderedAccessViewRHI->Resource);
	if (IsIntegerFormat(Texture->GetFormat())) {
		ClearTexture(Texture, Color, UnorderedAccessViewRHI->MipLevel, 1, 0, Texture->GetDesc().GetRDLayerCount());
	} else {
		ClearTexture(Texture, FLinearColor((float)Values.X, (float)Values.Y, (float)Values.Z, (float)Values.W), UnorderedAccessViewRHI->MipLevel, 1, 0, Texture->GetDesc().GetRDLayerCount());
	}
}

void FRHIComputeCommandList::UpdateBuffer(FRHIBuffer *Buffer, uint32 Offset, uint32 Size, const void *Data) {
	ERR_FAIL_NULL(Buffer);
	FlushComputeList();
	// Inside a draw list the update is still ordered before the draws of this pass by RenderingDeviceGraph.
	const RID BufferRID = Buffer->GetRID();
	const bool bSkipCheck = DrawList >= 0;
	if (!Device->recording_context_get_current()) {
		Device->buffer_update(BufferRID, Offset, Size, Data, bSkipCheck);
		return;
	}
	LocalVector<uint8_t> Copy;
	Copy.resize(Size);
	memcpy(Copy.ptr(), Data, Size);
	RDGPrivate::EnqueueRenderThreadCommand(Device, [=](RenderingDevice *RD) { RD->buffer_update(BufferRID, Offset, Size, Copy.ptr(), bSkipCheck); });
}

void FRHIComputeCommandList::UpdateTexture(FRHITexture *Texture, uint32 Layer, const Vector<uint8_t> &Data) {
	ERR_FAIL_NULL(Texture);
	ERR_FAIL_COND_MSG(DrawList >= 0, "[RDG] UpdateTexture can't be called inside a raster pass.");
	FlushComputeList();
	const RID TextureRID = Texture->GetRID();
	RDGPrivate::EnqueueRenderThreadCommand(Device, [=](RenderingDevice *RD) { RD->texture_update(TextureRID, Layer, Data); });
}

void FRHIComputeCommandList::PushEvent(const char *Name, const FLinearColor &Color) {
	Device->draw_command_insert_label(String::utf8(Name), Color);
	NumOpenEvents++;
}

void FRHIComputeCommandList::PopEvent() {
	if (NumOpenEvents > 0) NumOpenEvents--;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Raster.

void FRHICommandList::BeginRenderPass(const FRenderPassInfo &Info) {
	ERR_FAIL_COND_MSG(DrawList >= 0, "[RDG] BeginRenderPass called while a render pass is already open.");
	FlushComputeList();
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Device);

	RID Attachments[MaxSimultaneousRenderTargets + 1];
	uint32 NumAttachments = 0;
	for (uint32 Index = 0; Index < Info.NumColorTargets; Index++) Attachments[NumAttachments++] = Info.ColorRIDs[Index];
	if (Info.DepthStencilRID.is_valid()) Attachments[NumAttachments++] = Info.DepthStencilRID;
	const RID Framebuffer = Context->GetFramebuffer(Attachments, NumAttachments);
	ERR_FAIL_COND_MSG(!Framebuffer.is_valid(), vformat("[RDG] Pass '%s': failed to create framebuffer.", Info.Name));

	BitField<RenderingDevice::DrawFlags> Flags = RenderingDevice::DRAW_DEFAULT_ALL;
	Vector<Color> ClearColors;
	for (uint32 Index = 0; Index < Info.NumColorTargets; Index++) {
		if (Info.ColorLoadActions[Index] == ERenderTargetLoadAction::EClear) Flags.set_flag(RenderingDevice::DrawFlags(RenderingDevice::DRAW_CLEAR_COLOR_0 << Index));
		else if (Info.ColorLoadActions[Index] == ERenderTargetLoadAction::ENoAction) Flags.set_flag(RenderingDevice::DrawFlags(RenderingDevice::DRAW_IGNORE_COLOR_0 << Index));
		ClearColors.push_back(Info.ClearColors[Index]);
	}
	if (Info.DepthStencilRID.is_valid()) {
		if (Info.DepthLoadAction == ERenderTargetLoadAction::EClear) Flags.set_flag(RenderingDevice::DRAW_CLEAR_DEPTH);
		else if (Info.DepthLoadAction == ERenderTargetLoadAction::ENoAction) Flags.set_flag(RenderingDevice::DRAW_IGNORE_DEPTH);
		if (Info.bHasStencil) {
			if (Info.StencilLoadAction == ERenderTargetLoadAction::EClear) Flags.set_flag(RenderingDevice::DRAW_CLEAR_STENCIL);
			else if (Info.StencilLoadAction == ERenderTargetLoadAction::ENoAction) Flags.set_flag(RenderingDevice::DRAW_IGNORE_STENCIL);
		}
	}

	DrawList = Device->draw_list_begin(Framebuffer, Flags, ClearColors, Info.ClearDepth, Info.ClearStencil);
	ERR_FAIL_COND_MSG(DrawList < 0, vformat("[RDG] Pass '%s': draw_list_begin failed.", Info.Name));
	CurrentFramebufferFormat = Device->framebuffer_get_format(Framebuffer);
	CurrentNumColorTargets = Info.NumColorTargets;
	CurrentRenderTargetSize = Info.Extent;
	bGraphicsPipelineValid = false;
	ResetBindings();
}

void FRHICommandList::EndRenderPass() {
	if (DrawList < 0) return;
	Device->draw_list_end();
	DrawList = -1;
	ResetStreamSources();
	ResetLastUniformSets();
	CurrentFramebufferFormat = RenderingDevice::INVALID_ID;
	CurrentGraphicsShaderRID = RID();
	CurrentGraphicsReflection = nullptr;
	bGraphicsPipelineValid = false;
}

void FRHICommandList::SetGraphicsPipelineState(const FGraphicsPipelineStateInitializer &Initializer, uint32 StencilRef) {
	ResetBindings();
	bGraphicsPipelineValid = false;
	if (DrawList < 0) {
		bBindingError = true;
		ERR_FAIL_MSG("[RDG] SetGraphicsPipelineState outside of a raster pass (the pass needs ERDGPassFlags::Raster and RENDER_TARGET_BINDING_SLOTS).");
	}
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Device);
	const FRDGDeviceContext::FBoundShaderState *BoundShaderState = Context->GetBoundShaderState(Initializer.BoundShaderState.VertexShaderRHI, Initializer.BoundShaderState.PixelShaderRHI);
	if (!BoundShaderState) {
		bBindingError = true;
		return;
	}
	const RID Pipeline = Context->GetGraphicsPipeline(*BoundShaderState, CurrentFramebufferFormat, CurrentNumColorTargets, Initializer, StencilRef);
	if (!Pipeline.is_valid()) {
		bBindingError = true;
		return;
	}
	Device->draw_list_bind_render_pipeline(DrawList, Pipeline);
	CurrentVertexDeclaration = Initializer.BoundShaderState.VertexDeclarationRHI;
	CurrentVertexFormat = CurrentVertexDeclaration ? CurrentVertexDeclaration->GetVertexFormat(Device) : RenderingDevice::INVALID_FORMAT_ID;
	CurrentGraphicsShaderRID = BoundShaderState->ShaderRID;
	CurrentGraphicsReflection = &BoundShaderState->Reflection;
	CurrentGraphicsShaderName = BoundShaderState->Name;
	CurrentPrimitiveType = Initializer.PrimitiveType;
	bGraphicsPipelineValid = true;
}

void FRHICommandList::SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) {
	ERR_FAIL_COND(DrawList < 0);
	Device->draw_list_set_viewport(DrawList, Rect2(MinX, MinY, MaxX - MinX, MaxY - MinY));
}

void FRHICommandList::SetScissorRect(bool bEnable, uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY) {
	ERR_FAIL_COND(DrawList < 0);
	if (bEnable) Device->draw_list_enable_scissor(DrawList, Rect2((float)MinX, (float)MinY, (float)(MaxX - MinX), (float)(MaxY - MinY)));
	else Device->draw_list_disable_scissor(DrawList);
}

void FRHICommandList::SetBlendFactor(const FLinearColor &BlendFactor) {
	ERR_FAIL_COND(DrawList < 0);
	Device->draw_list_set_blend_constants(DrawList, BlendFactor);
}

uint32 FRHICommandList::GetNumVertices(uint32 NumPrimitives) const {
	// UE's GetVertexCountForPrimitiveCount.
	switch (CurrentPrimitiveType) {
		case PT_TriangleStrip:
			return NumPrimitives + 2;
		case PT_LineList:
			return NumPrimitives * 2;
		case PT_PointList:
			return NumPrimitives;
		default:
			return NumPrimitives * 3;
	}
}

void FRHICommandList::SetStreamSource(uint32 StreamIndex, FRHIBuffer *VertexBuffer, uint32 Offset) {
	ERR_FAIL_UNSIGNED_INDEX(StreamIndex, (uint32)MaxVertexElementCount);
	StreamSources[StreamIndex].Buffer = VertexBuffer;
	StreamSources[StreamIndex].Offset = Offset;
}

void FRHICommandList::ResetStreamSources() {
	for (FStreamSource &Stream : StreamSources) Stream = FStreamSource();
	CurrentVertexDeclaration = nullptr;
	CurrentVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
}

bool FRHICommandList::BindStreamSources(int32 BaseVertexIndex, uint32 NumVertices) {
	const FVertexDeclarationElementList &Elements = CurrentVertexDeclaration->GetElements();
	RID Buffers[MaxVertexElementCount];
	uint64 Offsets[MaxVertexElementCount] = {};
	uint32 MaxVertices = UINT32_MAX;
	for (uint32 Index = 0; Index < Elements.size(); Index++) {
		const FVertexElement &Element = Elements[Index];
		const FStreamSource &Stream = StreamSources[Element.StreamIndex];
		if (!Stream.Buffer) {
			NumBindingErrors++;
			ERR_FAIL_V_MSG(false, vformat("[RDG] Vertex element %d reads stream %d, which has no stream source (SetStreamSource).", Index, Element.StreamIndex));
		}
		// External buffers (not created by RDG) are validated by RenderingDevice.
		if (Stream.Buffer->OwnsRID() && Stream.Buffer->GetRDType() != ERDBufferType::Vertex) {
			NumBindingErrors++;
			ERR_FAIL_V_MSG(false, vformat("[RDG] Stream %d buffer '%s' is not a vertex buffer: declare it in the pass parameters with RDG_BUFFER_ACCESS(..., ERHIAccess::VertexOrIndexBuffer).", Element.StreamIndex, Stream.Buffer->GetName()));
		}
		// Each element is its own RD binding; per-vertex streams start at the base vertex.
		const int64 Offset = (int64)Stream.Offset + (Element.bUseInstanceIndex ? 0 : (int64)BaseVertexIndex * Element.Stride);
		if (Offset < 0) {
			NumBindingErrors++;
			ERR_FAIL_V_MSG(false, vformat("[RDG] Stream %d: base vertex %d moves the stream offset before the start of the buffer.", Element.StreamIndex, BaseVertexIndex));
		}
		Buffers[Index] = Stream.Buffer->GetRID();
		Offsets[Index] = (uint64)Offset;
		if (!Element.bUseInstanceIndex && Element.Stride > 0) {
			const uint64 Size = Stream.Buffer->GetSize();
			const uint64 Available = Size > (uint64)Offset + Element.Offset ? Size - (uint64)Offset - Element.Offset : 0;
			MaxVertices = MIN(MaxVertices, (uint32)(Available / Element.Stride));
		}
	}
	if (NumVertices == 0) NumVertices = MaxVertices == UINT32_MAX ? 1 : FMath::Max(1u, MaxVertices);
	if (CurrentVertexFormat == RenderingDevice::INVALID_FORMAT_ID) {
		NumBindingErrors++;
		return false;
	}
	Device->draw_list_bind_vertex_buffers_format(DrawList, CurrentVertexFormat, NumVertices, Span<RID>(Buffers, Elements.size()), Span<uint64_t>(Offsets, Elements.size()));
	return true;
}

void FRHICommandList::DrawPrimitive(uint32 BaseVertexIndex, uint32 NumPrimitives, uint32 NumInstances) {
	if (DrawList < 0 || !bGraphicsPipelineValid || !CurrentGraphicsReflection) {
		NumBindingErrors++;
		ERR_FAIL_MSG("[RDG] DrawPrimitive without a valid graphics pipeline state.");
	}
	if (!FlushShaderBindings(CurrentGraphicsShaderRID, *CurrentGraphicsReflection, CurrentGraphicsShaderName, true)) return;
	const uint32 NumVertices = GetNumVertices(NumPrimitives);
	if (CurrentVertexDeclaration && CurrentVertexDeclaration->HasElements()) {
		if (!BindStreamSources((int32)BaseVertexIndex, NumVertices)) return;
		Device->draw_list_draw(DrawList, false, FMath::Max(1u, NumInstances), 0);
		return;
	}
	if (BaseVertexIndex != 0) WARN_PRINT_ONCE("[RDG] DrawPrimitive: BaseVertexIndex is not supported for procedural draws (no vertex declaration) and is ignored.");
	Device->draw_list_draw(DrawList, false, FMath::Max(1u, NumInstances), NumVertices);
}

void FRHICommandList::DrawIndexedPrimitive(FRHIBuffer *IndexBuffer, int32 BaseVertexIndex, uint32 FirstInstance, uint32 NumVertices, uint32 StartIndex, uint32 NumPrimitives, uint32 NumInstances) {
	if (DrawList < 0 || !bGraphicsPipelineValid || !CurrentGraphicsReflection || !IndexBuffer) {
		NumBindingErrors++;
		ERR_FAIL_MSG("[RDG] DrawIndexedPrimitive without a valid graphics pipeline state or index buffer.");
	}
	if (FirstInstance != 0) WARN_PRINT_ONCE("[RDG] DrawIndexedPrimitive: FirstInstance is not supported by RenderingDevice and is ignored.");
	if (!FlushShaderBindings(CurrentGraphicsShaderRID, *CurrentGraphicsReflection, CurrentGraphicsShaderName, true)) return;
	if (CurrentVertexDeclaration && CurrentVertexDeclaration->HasElements() && !BindStreamSources(BaseVertexIndex, NumVertices)) return;
	const RID IndexArray = IndexBuffer->GetOrCreateIndexArray(StartIndex, GetNumVertices(NumPrimitives));
	if (!IndexArray.is_valid()) {
		NumBindingErrors++;
		return;
	}
	Device->draw_list_bind_index_array(DrawList, IndexArray);
	Device->draw_list_draw(DrawList, true, FMath::Max(1u, NumInstances), 0);
}

void FRHICommandList::DrawPrimitiveIndirect(FRHIBuffer *ArgumentBuffer, uint32 ArgumentOffset) {
	if (DrawList < 0 || !bGraphicsPipelineValid || !CurrentGraphicsReflection || !ArgumentBuffer) {
		NumBindingErrors++;
		ERR_FAIL_MSG("[RDG] DrawPrimitiveIndirect without a valid graphics pipeline state or argument buffer.");
	}
	if (!FlushShaderBindings(CurrentGraphicsShaderRID, *CurrentGraphicsReflection, CurrentGraphicsShaderName, true)) return;
	if (CurrentVertexDeclaration && CurrentVertexDeclaration->HasElements() && !BindStreamSources(0, 0)) return;
	Device->draw_list_draw_indirect(DrawList, false, ArgumentBuffer->GetRID(), ArgumentOffset, 1, 0);
}

void FRHICommandList::DrawIndexedPrimitiveIndirect(FRHIBuffer *IndexBuffer, FRHIBuffer *ArgumentBuffer, uint32 ArgumentOffset) {
	if (DrawList < 0 || !bGraphicsPipelineValid || !CurrentGraphicsReflection || !IndexBuffer || !ArgumentBuffer) {
		NumBindingErrors++;
		ERR_FAIL_MSG("[RDG] DrawIndexedPrimitiveIndirect without a valid graphics pipeline state, index buffer or argument buffer.");
	}
	if (!FlushShaderBindings(CurrentGraphicsShaderRID, *CurrentGraphicsReflection, CurrentGraphicsShaderName, true)) return;
	if (CurrentVertexDeclaration && CurrentVertexDeclaration->HasElements() && !BindStreamSources(0, 0)) return;
	// The arguments carry the first index: bind the whole buffer.
	const uint32 NumIndices = IndexBuffer->GetStride() > 0 ? IndexBuffer->GetSize() / IndexBuffer->GetStride() : 0;
	const RID IndexArray = IndexBuffer->GetOrCreateIndexArray(0, NumIndices);
	if (!IndexArray.is_valid()) {
		NumBindingErrors++;
		return;
	}
	Device->draw_list_bind_index_array(DrawList, IndexArray);
	Device->draw_list_draw_indirect(DrawList, true, ArgumentBuffer->GetRID(), ArgumentOffset, 1, 0);
}

void FRHICommandListImmediate::SubmitCommandsAndFlushGPU() {
	FlushComputeList();
	if (Device && Device != FRDGDeviceContext::GetMainDevice()) {
		Device->submit();
		Device->sync();
	}
}

namespace {
FRHICommandListImmediate *GImmediateCommandList = nullptr;
}

FRHICommandListImmediate &FRHICommandListExecutor::GetImmediateCommandList() {
	RenderingDevice *MainDevice = FRDGDeviceContext::GetMainDevice();
	if (!GImmediateCommandList || GImmediateCommandList->GetDevice() != MainDevice) {
		if (GImmediateCommandList) memdelete(GImmediateCommandList);
		GImmediateCommandList = memnew(FRHICommandListImmediate(MainDevice));
	}
	return *GImmediateCommandList;
}

void FRHICommandListExecutor::UpdateParallelTranslate(RenderingDevice *Device) {
	ERR_FAIL_NULL(Device);
	// Same setting and default the RenderingDevice starts with.
	static const bool bProjectSetting = bool(ProjectSettings::get_singleton()->get_setting("rendering/rendering_device/parallel_translate", false));
	const int32 Enable = CVarRHICmdParallelTranslateEnable.GetValueOnRenderThread();
	Device->parallel_translate_set_enabled(Enable < 0 ? bProjectSetting : Enable > 0);
	Device->parallel_translate_set_max_commands_per_translate(CVarRHICmdParallelTranslateMaxCommandsPerTranslate.GetValueOnRenderThread());
}

void FRHICommandListExecutor::ShutdownImmediateCommandList() {
	if (GImmediateCommandList) memdelete(GImmediateCommandList);
	GImmediateCommandList = nullptr;
}

void SetComputePipelineState(FRHIComputeCommandList &RHICmdList, FRHIComputeShader *ComputeShader) {
	RHICmdList.SetComputePipelineState(ComputeShader);
}

void SetGraphicsPipelineState(FRHICommandList &RHICmdList, const FGraphicsPipelineStateInitializer &Initializer, uint32 StencilRef, bool bApplyAdditionalState) {
	RHICmdList.SetGraphicsPipelineState(Initializer, StencilRef);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Readbacks.

namespace {
uint64 GNextReadbackId = 1;
HashMap<uint64, FRHIGPUBufferReadback *> &GetBufferReadbacks() {
	static HashMap<uint64, FRHIGPUBufferReadback *> Readbacks;
	return Readbacks;
}
HashMap<uint64, FRHIGPUTextureReadback *> &GetTextureReadbacks() {
	static HashMap<uint64, FRHIGPUTextureReadback *> Readbacks;
	return Readbacks;
}
} //namespace

FRHIGPUBufferReadback::FRHIGPUBufferReadback(const char *InName) :
		Name(String::utf8(InName)), Id(GNextReadbackId++) {
	GetBufferReadbacks().insert(Id, this);
}

FRHIGPUBufferReadback::~FRHIGPUBufferReadback() {
	GetBufferReadbacks().erase(Id);
}

void FRHIGPUBufferReadback::EnqueueCopy(FRHIComputeCommandList &RHICmdList, FRHIBuffer *SourceBuffer, uint32 NumBytes) {
	ERR_FAIL_NULL(SourceBuffer);
	RHICmdList.FlushComputeList();
	bReady = false;
	bPending = true;
	const uint32 Size = NumBytes ? FMath::Min(NumBytes, SourceBuffer->GetSize()) : SourceBuffer->GetSize();
	const RID SourceRID = SourceBuffer->GetRID();
	const uint64 ReadbackId = Id;
	RDGPrivate::EnqueueRenderThreadCommand(RHICmdList.GetDevice(), [=](RenderingDevice *RD) { RD->buffer_get_data_async(SourceRID, callable_mp_static(&FRHIGPUBufferReadback::OnDataReceived).bind(ReadbackId), 0, Size); });
}

const void *FRHIGPUBufferReadback::Lock(uint32 NumBytes) {
	if (!bReady || (uint32)Data.size() < NumBytes) return nullptr;
	return Data.ptr();
}

void FRHIGPUBufferReadback::OnDataReceived(const Vector<uint8_t> &InData, uint64 ReadbackId) {
	FRHIGPUBufferReadback **Readback = GetBufferReadbacks().getptr(ReadbackId);
	if (!Readback) return;
	(*Readback)->Data = InData;
	(*Readback)->bReady = true;
	(*Readback)->bPending = false;
}

FRHIGPUTextureReadback::FRHIGPUTextureReadback(const char *InName) :
		Name(String::utf8(InName)), Id(GNextReadbackId++) {
	GetTextureReadbacks().insert(Id, this);
}

FRHIGPUTextureReadback::~FRHIGPUTextureReadback() {
	GetTextureReadbacks().erase(Id);
}

void FRHIGPUTextureReadback::EnqueueCopy(FRHIComputeCommandList &RHICmdList, FRHITexture *SourceTexture, uint32 SourceSlice) {
	ERR_FAIL_NULL(SourceTexture);
	RHICmdList.FlushComputeList();
	bReady = false;
	bPending = true;
	Size = SourceTexture->GetSizeXY();
	const RID SourceRID = SourceTexture->GetRID();
	const uint64 ReadbackId = Id;
	RDGPrivate::EnqueueRenderThreadCommand(RHICmdList.GetDevice(), [=](RenderingDevice *RD) { RD->texture_get_data_async(SourceRID, SourceSlice, callable_mp_static(&FRHIGPUTextureReadback::OnDataReceived).bind(ReadbackId)); });
}

const void *FRHIGPUTextureReadback::Lock(int32 &OutRowPitchInPixels) {
	OutRowPitchInPixels = Size.X;
	return bReady ? Data.ptr() : nullptr;
}

void FRHIGPUTextureReadback::OnDataReceived(const Vector<uint8_t> &InData, uint64 ReadbackId) {
	FRHIGPUTextureReadback **Readback = GetTextureReadbacks().getptr(ReadbackId);
	if (!Readback) return;
	(*Readback)->Data = InData;
	(*Readback)->bReady = true;
	(*Readback)->bPending = false;
}

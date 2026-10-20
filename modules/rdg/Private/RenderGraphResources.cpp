#include "RenderGraphResources.h"

#include "RDGDeviceContext.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphPrivate.h"

#include "core/templates/hashfuncs.h"
#include "servers/rendering/rendering_device.h"

#include <atomic>

namespace {
std::atomic<uint32> GValidationErrorCount{ 0 };
}

uint32 RDGPrivate::GetValidationErrorCount() {
	return GValidationErrorCount.load();
}

void RDGPrivate::ReportValidationError(const String &Message) {
	GValidationErrorCount++;
	ERR_PRINT("[RDG] " + Message);
}

void RDGPrivate::ReportValidationWarning(const String &Message) {
	WARN_PRINT("[RDG] " + Message);
}

uint32 FRDGBufferDesc::GetHash() const {
	return hash_fmix32(hash_murmur3_one_32(BytesPerElement, hash_murmur3_one_32(NumElements, hash_murmur3_one_32((uint32)Usage))));
}

void FRDGResource::ValidateRHIAccess() const {
	if (bAllowRHIAccess || !RDGPrivate::IsValidationEnabled()) return;
	RDGPrivate::ReportValidationError(vformat("Accessing the RHI resource of %s at this time is not allowed. If you hit this check in pass, that is due to this resource not being referenced in the parameters of your pass.", Name));
}

RID FRDGTexture::GetRID() const {
	FRHITexture *Texture = GetRHI();
	return Texture ? Texture->GetRID() : RID();
}

RID FRDGBuffer::GetRID() const {
	FRHIBuffer *Buffer = GetRHI();
	return Buffer ? Buffer->GetRID() : RID();
}

FRHIShaderResourceView *FRDGTextureSRV::GetRHI() const {
	FRHITexture *Texture = Desc.Texture ? Desc.Texture->GetRHI() : nullptr;
	if (!Texture) return nullptr;
	return Texture->GetOrCreateSRV(Desc.MipLevel, Desc.NumMipLevels, Desc.FirstArraySlice, Desc.NumArraySlices, Desc.Format);
}

FRDGTextureSubresourceRange FRDGTextureSRV::GetSubresourceRange() const {
	FRDGTextureSubresourceRange Range;
	const FRDGTextureDesc &TextureDesc = Desc.Texture->Desc;
	Range.MipIndex = Desc.MipLevel;
	Range.NumMips = Desc.NumMipLevels ? Desc.NumMipLevels : TextureDesc.NumMips - Desc.MipLevel;
	Range.ArraySlice = Desc.FirstArraySlice;
	Range.NumArraySlices = Desc.NumArraySlices ? Desc.NumArraySlices : TextureDesc.GetRDLayerCount() - Desc.FirstArraySlice;
	return Range;
}

FRHIUnorderedAccessView *FRDGTextureUAV::GetRHI() const {
	FRHITexture *Texture = Desc.Texture ? Desc.Texture->GetRHI() : nullptr;
	if (!Texture) return nullptr;
	return Texture->GetOrCreateUAV(Desc.MipLevel, Desc.FirstArraySlice, Desc.NumArraySlices, Desc.Format);
}

FRDGTextureSubresourceRange FRDGTextureUAV::GetSubresourceRange() const {
	FRDGTextureSubresourceRange Range;
	const FRDGTextureDesc &TextureDesc = Desc.Texture->Desc;
	Range.MipIndex = Desc.MipLevel;
	Range.NumMips = 1;
	Range.ArraySlice = Desc.FirstArraySlice;
	Range.NumArraySlices = Desc.NumArraySlices ? Desc.NumArraySlices : TextureDesc.GetRDLayerCount() - Desc.FirstArraySlice;
	return Range;
}

FRHIShaderResourceView *FRDGBufferSRV::GetRHI() const {
	FRHIBuffer *Buffer = Desc.Buffer ? Desc.Buffer->GetRHI() : nullptr;
	return Buffer ? Buffer->GetSRV(Desc.Format) : nullptr;
}

FRHIUnorderedAccessView *FRDGBufferUAV::GetRHI() const {
	FRHIBuffer *Buffer = Desc.Buffer ? Desc.Buffer->GetRHI() : nullptr;
	return Buffer ? Buffer->GetUAV(Desc.Format) : nullptr;
}

RID FRDGUniformBuffer::GetOrCreateRDBuffer(const FShaderParameterBindings::FResourceBinding &Binding) {
	// A uniform buffer can be bound by passes recorded in parallel.
	static Mutex LayoutMutex;
	MutexLock Lock(LayoutMutex);
	for (const FLayoutBuffer &Layout : LayoutBuffers) {
		if (Layout.LayoutKey == &Binding) return Layout.Buffer;
	}
	ERR_FAIL_NULL_V(Builder, RID());
	FRDGDeviceContext *Context = Builder->GetDeviceContext();
	const uint32 Size = FMath::Max(16u, Binding.UniformBufferSize);
	const RID Buffer = Context->AllocateFrameUniformBuffer(Size);
	ERR_FAIL_COND_V(!Buffer.is_valid(), RID());

	LocalVector<uint8> Data;
	Data.resize(Size);
	memset(Data.ptr(), 0, Size);
	const uint8 *Base = reinterpret_cast<const uint8 *>(Contents);
	for (const FShaderParameterBindings::FResourceBinding::FConstantCopy &Copy : Binding.UniformBufferCopies) {
		for (uint32 Element = 0; Element < Copy.NumElements; Element++) {
			const uint32 DstOffset = Copy.DstOffset + Element * Copy.DstStride;
			if (DstOffset + Copy.Size > Size) break;
			if (Copy.bBool) {
				const uint32 Value = *reinterpret_cast<const bool *>(Base + Copy.SrcOffset + Element * Copy.SrcStride) ? 1u : 0u;
				memcpy(Data.ptr() + DstOffset, &Value, 4);
			} else {
				memcpy(Data.ptr() + DstOffset, Base + Copy.SrcOffset + Element * Copy.SrcStride, Copy.Size);
			}
		}
	}
	// The buffer is unique for this frame, so updating it while a draw/compute list is open is safe (skip_check).
	RDGPrivate::EnqueueRenderThreadCommand(Context->Device, [=](RenderingDevice *RD) { RD->buffer_update(Buffer, 0, Size, Data.ptr(), true); });

	FLayoutBuffer Layout;
	Layout.LayoutKey = &Binding;
	Layout.Buffer = Buffer;
	LayoutBuffers.push_back(Layout);
	return Buffer;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Parameter enumeration.

void RDGPrivate::EnumerateGraphResources(const FShaderParametersMetadata *Metadata, const void *Parameters, ERDGPassFlags PassFlags, FGraphResourceVisitor &Visitor) {
	if (!Metadata || !Parameters) return;
	const uint8 *Base = reinterpret_cast<const uint8 *>(Parameters);
	const bool bCompute = EnumHasAnyFlags(PassFlags, ERDGPassFlags::Compute | ERDGPassFlags::AsyncCompute);
	const ERHIAccess SRVAccess = bCompute ? ERHIAccess::SRVCompute : ERHIAccess::SRVGraphics;
	const ERHIAccess UAVAccess = bCompute ? ERHIAccess::UAVCompute : ERHIAccess::UAVGraphics;

	for (const FShaderParametersMetadata::FFlatParameter &Parameter : Metadata->GetResourceParameters()) {
		const uint8 *Member = Base + Parameter.ByteOffset;
		const uint32 Count = FMath::Max(1u, Parameter.NumElements);
		const char *MemberName = Parameter.Member ? Parameter.Member->GetName() : "";
		switch (Parameter.BaseType) {
			case UBMT_RDG_TEXTURE:
				for (uint32 Index = 0; Index < Count; Index++) {
					FRDGTexture *Texture = reinterpret_cast<FRDGTexture *const *>(Member)[Index];
					if (!Texture) continue;
					FRDGTextureSubresourceRange Range;
					Range.NumMips = Texture->Desc.NumMips;
					Range.NumArraySlices = Texture->Desc.GetRDLayerCount();
					Visitor.OnTexture(Texture, SRVAccess, false, Range, MemberName);
				}
				break;
			case UBMT_RDG_TEXTURE_SRV:
			case UBMT_RDG_TEXTURE_NON_PIXEL_SRV:
				for (uint32 Index = 0; Index < Count; Index++) {
					FRDGTextureSRV *SRV = reinterpret_cast<FRDGTextureSRV *const *>(Member)[Index];
					if (!SRV || !SRV->GetParent()) continue;
					Visitor.OnTextureView(SRV);
					Visitor.OnTexture(SRV->GetParent(), SRVAccess, false, SRV->GetSubresourceRange(), MemberName);
				}
				break;
			case UBMT_RDG_TEXTURE_UAV:
				for (uint32 Index = 0; Index < Count; Index++) {
					FRDGTextureUAV *UAV = reinterpret_cast<FRDGTextureUAV *const *>(Member)[Index];
					if (!UAV || !UAV->GetParent()) continue;
					Visitor.OnTextureView(UAV);
					Visitor.OnTexture(UAV->GetParent(), UAVAccess, false, UAV->GetSubresourceRange(), MemberName);
				}
				break;
			case UBMT_RDG_TEXTURE_ACCESS: {
				const FRDGTextureAccess &Access = *reinterpret_cast<const FRDGTextureAccess *>(Member);
				if (Access.GetTexture()) {
					FRDGTextureSubresourceRange Range;
					Range.NumMips = Access->Desc.NumMips;
					Range.NumArraySlices = Access->Desc.GetRDLayerCount();
					Visitor.OnTexture(Access.GetTexture(), Access.GetAccess(), false, Range, MemberName);
				}
			} break;
			case UBMT_RDG_TEXTURE_ACCESS_ARRAY: {
				const FRDGTextureAccessArray &Array = *reinterpret_cast<const FRDGTextureAccessArray *>(Member);
				for (const FRDGTextureAccess &Access : Array) {
					if (!Access.GetTexture()) continue;
					FRDGTextureSubresourceRange Range;
					Range.NumMips = Access->Desc.NumMips;
					Range.NumArraySlices = Access->Desc.GetRDLayerCount();
					Visitor.OnTexture(Access.GetTexture(), Access.GetAccess(), false, Range, MemberName);
				}
			} break;
			case UBMT_RDG_BUFFER_SRV:
				for (uint32 Index = 0; Index < Count; Index++) {
					FRDGBufferSRV *SRV = reinterpret_cast<FRDGBufferSRV *const *>(Member)[Index];
					if (!SRV || !SRV->GetParent()) continue;
					Visitor.OnBufferView(SRV);
					Visitor.OnBuffer(SRV->GetParent(), SRVAccess, MemberName);
				}
				break;
			case UBMT_RDG_BUFFER_UAV:
				for (uint32 Index = 0; Index < Count; Index++) {
					FRDGBufferUAV *UAV = reinterpret_cast<FRDGBufferUAV *const *>(Member)[Index];
					if (!UAV || !UAV->GetParent()) continue;
					Visitor.OnBufferView(UAV);
					Visitor.OnBuffer(UAV->GetParent(), UAVAccess, MemberName);
				}
				break;
			case UBMT_RDG_BUFFER_ACCESS: {
				const FRDGBufferAccess &Access = *reinterpret_cast<const FRDGBufferAccess *>(Member);
				if (Access.GetBuffer()) Visitor.OnBuffer(Access.GetBuffer(), Access.GetAccess(), MemberName);
			} break;
			case UBMT_RDG_BUFFER_ACCESS_ARRAY: {
				const FRDGBufferAccessArray &Array = *reinterpret_cast<const FRDGBufferAccessArray *>(Member);
				for (const FRDGBufferAccess &Access : Array) {
					if (Access.GetBuffer()) Visitor.OnBuffer(Access.GetBuffer(), Access.GetAccess(), MemberName);
				}
			} break;
			case UBMT_RDG_UNIFORM_BUFFER: {
				const FRDGUniformBufferBinding &Binding = *reinterpret_cast<const FRDGUniformBufferBinding *>(Member);
				FRDGUniformBuffer *UniformBuffer = Binding.GetUniformBuffer();
				if (!UniformBuffer) break;
				Visitor.OnUniformBuffer(UniformBuffer);
				EnumerateGraphResources(UniformBuffer->GetParametersMetadata(), UniformBuffer->GetContents(), PassFlags, Visitor);
			} break;
			case UBMT_RENDER_TARGET_BINDING_SLOTS: {
				const FRenderTargetBindingSlots &RenderTargets = *reinterpret_cast<const FRenderTargetBindingSlots *>(Member);
				RenderTargets.Enumerate([&](const FRenderTargetBinding &RenderTarget) {
					FRDGTexture *Texture = RenderTarget.GetTexture();
					FRDGTextureSubresourceRange Range;
					Range.MipIndex = RenderTarget.GetMipIndex();
					Range.NumMips = 1;
					Range.ArraySlice = RenderTarget.GetArraySlice() < 0 ? 0 : RenderTarget.GetArraySlice();
					Range.NumArraySlices = RenderTarget.GetArraySlice() < 0 ? Texture->Desc.GetRDLayerCount() : 1;
					Visitor.OnTexture(Texture, ERHIAccess::RTV, true, Range, "RenderTargets");
					if (FRDGTexture *Resolve = RenderTarget.GetResolveTexture()) Visitor.OnTexture(Resolve, ERHIAccess::ResolveDst, true, Range, "RenderTargets");
				});
				const FDepthStencilBinding &DepthStencil = RenderTargets.DepthStencil;
				if (FRDGTexture *Texture = DepthStencil.GetTexture()) {
					FRDGTextureSubresourceRange Range;
					Range.NumMips = 1;
					Range.NumArraySlices = Texture->Desc.GetRDLayerCount();
					const ERHIAccess Access = DepthStencil.GetDepthStencilAccess().IsAnyWrite() ? ERHIAccess::DSVWrite : ERHIAccess::DSVRead;
					Visitor.OnTexture(Texture, Access, true, Range, "RenderTargets.DepthStencil");
				}
				if (RenderTargets.ShadingRateTexture) {
					FRDGTextureSubresourceRange Range;
					Range.NumMips = 1;
					Range.NumArraySlices = 1;
					Visitor.OnTexture(RenderTargets.ShadingRateTexture, ERHIAccess::ShadingRateSource, false, Range, "RenderTargets.ShadingRateTexture");
				}
			} break;
			default:
				break;
		}
	}
}

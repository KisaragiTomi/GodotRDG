#include "RenderGraphResourcePool.h"

#include "RDGDeviceContext.h"

#include "servers/rendering/rendering_device.h"

static uint64 GetTextureMemoryBytes(const RenderingDevice::TextureFormat &Format) {
	const uint64 PixelSize = RenderingDevice::get_image_format_pixel_size(Format.format);
	uint64 Bytes = 0;
	for (uint32 Mip = 0; Mip < Format.mipmaps; Mip++) Bytes += PixelSize * FMath::Max(1u, Format.width >> Mip) * FMath::Max(1u, Format.height >> Mip) * FMath::Max(1u, Format.depth >> Mip);
	return Bytes * Format.array_layers * (uint64(1) << (uint32)Format.samples);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Textures.

FRenderTargetPool::~FRenderTargetPool() {
	ReleaseAll();
}

TRefCountPtr<FPooledRenderTarget> FRenderTargetPool::FindFreeElement(const FRHITextureDesc &Desc, const char *Name, uint64 FrameNumber) {
	for (TRefCountPtr<FPooledRenderTarget> &Element : Elements) {
		if (!Element->IsFree() || Element->GetDesc() != Desc) continue;
		Element->LastUsedFrame = FrameNumber;
		Stats.NumReusedThisFrame++;
		Stats.TotalReused++;
		return Element;
	}

	const RenderingDevice::TextureFormat Format = ToRDTextureFormat(Device, Desc);
	const RID TextureRID = Device->texture_create(Format, RenderingDevice::TextureView());
	ERR_FAIL_COND_V_MSG(!TextureRID.is_valid(), TRefCountPtr<FPooledRenderTarget>(), vformat("[RDG] Failed to create texture '%s' (%dx%d, %s).", Name, Desc.Extent.X, Desc.Extent.Y, GetPixelFormatInfo(Desc.Format).Name));
	Device->set_resource_name(TextureRID, vformat("RDG Pool: %s", Name));

	FRHITexture *Texture = new FRHITexture(Device, TextureRID, Desc, String::utf8(Name), true);
	TRefCountPtr<FPooledRenderTarget> Element(new FPooledRenderTarget(Texture, true));
	Element->LastUsedFrame = FrameNumber;
	Elements.push_back(Element);

	Stats.NumCreatedThisFrame++;
	Stats.TotalCreated++;
	Stats.MemoryBytes += GetTextureMemoryBytes(Format);
	return Element;
}

void FRenderTargetPool::TickPoolElements(uint64 FrameNumber, uint32 FramesUntilRelease) {
	for (int64 Index = (int64)Elements.size() - 1; Index >= 0; Index--) {
		TRefCountPtr<FPooledRenderTarget> &Element = Elements[Index];
		if (!Element->IsFree() || FrameNumber - Element->LastUsedFrame <= FramesUntilRelease) continue;
		const FRHITextureDesc &Desc = Element->GetDesc();
		const RenderingDevice::TextureFormat Format = ToRDTextureFormat(Device, Desc);
		const uint64 Bytes = GetTextureMemoryBytes(Format);
		Stats.MemoryBytes = Stats.MemoryBytes > Bytes ? Stats.MemoryBytes - Bytes : 0;
		Stats.NumReleasedThisFrame++;
		Elements.remove_at_unordered(Index);
	}

	Stats.NumElements = Elements.size();
	Stats.NumFreeElements = 0;
	for (const TRefCountPtr<FPooledRenderTarget> &Element : Elements) {
		if (Element->IsFree()) Stats.NumFreeElements++;
	}
}

void FRenderTargetPool::FreeUnusedResources() {
	for (int64 Index = (int64)Elements.size() - 1; Index >= 0; Index--) {
		if (Elements[Index]->IsFree()) Elements.remove_at_unordered(Index);
	}
}

void FRenderTargetPool::ReleaseAll() {
	for (const TRefCountPtr<FPooledRenderTarget> &Element : Elements) {
		if (!Element->IsFree()) WARN_PRINT(vformat("[RDG] Pooled texture '%s' is still referenced outside the pool while the pool is released.", Element->GetRHI()->GetName()));
	}
	Elements.clear();
	Stats = FRDGPoolStats();
}

void FRenderTargetPool::ResetFrameStats() {
	Stats.NumCreatedThisFrame = 0;
	Stats.NumReusedThisFrame = 0;
	Stats.NumReleasedThisFrame = 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Buffers.

FRDGBufferPool::~FRDGBufferPool() {
	ReleaseAll();
}

ERDBufferType FRDGBufferPool::GetRDBufferType(const FRDGBufferDesc &Desc, bool bVertexOrIndexBinding) {
	if (EnumHasAnyFlags(Desc.Usage, BUF_UniformBuffer)) return ERDBufferType::Uniform;
	// UE also sets BUF_VertexBuffer on typed / indirect buffers: only an actual vertex / index binding makes an RD vertex / index buffer.
	if (bVertexOrIndexBinding) return EnumHasAnyFlags(Desc.Usage, BUF_IndexBuffer) ? ERDBufferType::Index : ERDBufferType::Vertex;
	return ERDBufferType::Storage;
}

TRefCountPtr<FRDGPooledBuffer> FRDGBufferPool::CreateBuffer(RenderingDevice *Device, const FRDGBufferDesc &Desc, const char *Name, bool bPooled, ERDBufferType Type) {
	const uint32 Size = FMath::Max(4u, Desc.GetSize());
	uint32 AlignedSize = (Size + 3u) & ~3u;
	RID BufferRID;
	switch (Type) {
		case ERDBufferType::Uniform: {
			BufferRID = Device->uniform_buffer_create(AlignedSize);
		} break;
		case ERDBufferType::Vertex: {
			// Storage bit so compute passes can also write the vertices (SRV / UAV of the buffer).
			const bool bStorage = EnumHasAnyFlags(Desc.Usage, BUF_UnorderedAccess | BUF_ShaderResource);
			BufferRID = Device->vertex_buffer_create(AlignedSize, {}, bStorage ? RenderingDevice::BUFFER_CREATION_AS_STORAGE_BIT : 0);
		} break;
		case ERDBufferType::Index: {
			ERR_FAIL_COND_V_MSG(Desc.BytesPerElement != 2 && Desc.BytesPerElement != 4, TRefCountPtr<FRDGPooledBuffer>(), vformat("[RDG] Index buffer '%s' must have 2 or 4 bytes per element, not %d.", Name, Desc.BytesPerElement));
			const uint32 NumIndices = FMath::Max(1u, Desc.NumElements);
			AlignedSize = NumIndices * Desc.BytesPerElement;
			BufferRID = Device->index_buffer_create(NumIndices, Desc.BytesPerElement == 2 ? RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 : RenderingDevice::INDEX_BUFFER_FORMAT_UINT32);
		} break;
		case ERDBufferType::Storage: {
			BitField<RenderingDevice::StorageBufferUsage> Usage = 0;
			if (EnumHasAnyFlags(Desc.Usage, BUF_DrawIndirect)) Usage.set_flag(RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
			BufferRID = Device->storage_buffer_create(AlignedSize, {}, Usage);
		} break;
	}
	ERR_FAIL_COND_V_MSG(!BufferRID.is_valid(), TRefCountPtr<FRDGPooledBuffer>(), vformat("[RDG] Failed to create buffer '%s' (%d bytes).", Name, AlignedSize));
	Device->set_resource_name(BufferRID, vformat("RDG Pool: %s", Name));
	FRHIBuffer *Buffer = new FRHIBuffer(Device, BufferRID, AlignedSize, Desc.BytesPerElement, Desc.Usage, String::utf8(Name), true, Type);
	return TRefCountPtr<FRDGPooledBuffer>(new FRDGPooledBuffer(Buffer, Desc, bPooled));
}

TRefCountPtr<FRDGPooledBuffer> FRDGBufferPool::FindFreeBuffer(const FRDGBufferDesc &Desc, const char *Name, uint64 FrameNumber, ERDBufferType Type) {
	for (TRefCountPtr<FRDGPooledBuffer> &Element : Elements) {
		if (!Element->IsFree() || Element->Desc != Desc || Element->GetRHI()->GetRDType() != Type) continue;
		Element->LastUsedFrame = FrameNumber;
		Stats.NumReusedThisFrame++;
		Stats.TotalReused++;
		return Element;
	}

	TRefCountPtr<FRDGPooledBuffer> Element = CreateBuffer(Device, Desc, Name, true, Type);
	if (!Element.IsValid()) return Element;
	Element->LastUsedFrame = FrameNumber;
	Elements.push_back(Element);
	Stats.NumCreatedThisFrame++;
	Stats.TotalCreated++;
	Stats.MemoryBytes += Element->GetSize();
	return Element;
}

void FRDGBufferPool::TickPoolElements(uint64 FrameNumber, uint32 FramesUntilRelease) {
	for (int64 Index = (int64)Elements.size() - 1; Index >= 0; Index--) {
		TRefCountPtr<FRDGPooledBuffer> &Element = Elements[Index];
		if (!Element->IsFree() || FrameNumber - Element->LastUsedFrame <= FramesUntilRelease) continue;
		Stats.MemoryBytes = Stats.MemoryBytes > Element->GetSize() ? Stats.MemoryBytes - Element->GetSize() : 0;
		Stats.NumReleasedThisFrame++;
		Elements.remove_at_unordered(Index);
	}
	Stats.NumElements = Elements.size();
	Stats.NumFreeElements = 0;
	for (const TRefCountPtr<FRDGPooledBuffer> &Element : Elements) {
		if (Element->IsFree()) Stats.NumFreeElements++;
	}
}

void FRDGBufferPool::FreeUnusedResources() {
	for (int64 Index = (int64)Elements.size() - 1; Index >= 0; Index--) {
		if (Elements[Index]->IsFree()) Elements.remove_at_unordered(Index);
	}
}

void FRDGBufferPool::ReleaseAll() {
	Elements.clear();
	Stats = FRDGPoolStats();
}

void FRDGBufferPool::ResetFrameStats() {
	Stats.NumCreatedThisFrame = 0;
	Stats.NumReusedThisFrame = 0;
	Stats.NumReleasedThisFrame = 0;
}

const char *FRDGPooledBuffer::GetName() const {
	static thread_local CharString Name;
	Name = Buffer->GetName().utf8();
	return Name.get_data();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Free functions.

TRefCountPtr<IPooledRenderTarget> AllocatePooledTexture(RenderingDevice *Device, const FRDGTextureDesc &Desc, const char *Name) {
	ERR_FAIL_NULL_V(Device, TRefCountPtr<IPooledRenderTarget>());
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Device);
	TRefCountPtr<FPooledRenderTarget> Element = Context->RenderTargetPool.FindFreeElement(Desc, Name, Context->GetFrameNumber());
	return TRefCountPtr<IPooledRenderTarget>(Element.GetReference());
}

TRefCountPtr<IPooledRenderTarget> AllocatePooledTexture(const FRDGTextureDesc &Desc, const char *Name) {
	return AllocatePooledTexture(FRDGDeviceContext::GetMainDevice(), Desc, Name);
}

TRefCountPtr<FRDGPooledBuffer> AllocatePooledBuffer(RenderingDevice *Device, const FRDGBufferDesc &Desc, const char *Name) {
	ERR_FAIL_NULL_V(Device, TRefCountPtr<FRDGPooledBuffer>());
	FRDGDeviceContext *Context = FRDGDeviceContext::Get(Device);
	return Context->BufferPool.FindFreeBuffer(Desc, Name, Context->GetFrameNumber());
}

TRefCountPtr<FRDGPooledBuffer> AllocatePooledBuffer(const FRDGBufferDesc &Desc, const char *Name) {
	return AllocatePooledBuffer(FRDGDeviceContext::GetMainDevice(), Desc, Name);
}

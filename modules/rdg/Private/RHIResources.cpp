#include "RHIResources.h"

#include "RDGDeviceContext.h"
#include "RHIStaticStates.h"
#include "RenderGraphResources.h"

#include "core/os/mutex.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/rendering_device.h"

namespace {
// View caches are shared by the passes recorded in parallel: lookups take the shared lock, creation (and
// RenderingDevice::texture_create_shared*(), which isn't thread safe) the exclusive one.
FRWLock GViewLock;
} //namespace

FRHIVertexDeclaration GEmptyVertexDeclaration;
FRHIVertexDeclaration GFilterVertexDeclaration;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Texture desc.

bool FRHITextureDesc::IsValid() const {
	if (Format == PF_Unknown || Format >= PF_MAX) return false;
	if (Extent.X <= 0 || Extent.Y <= 0 || Depth == 0 || ArraySize == 0 || NumMips == 0 || NumSamples == 0) return false;
	if (Dimension != ETextureDimension::Texture3D && Depth != 1) return false;
	if (NumMips > 1 + FMath::FloorLog2((uint32)FMath::Max(FMath::Max(Extent.X, Extent.Y), (int32)Depth))) return false;
	return true;
}

bool FRHITextureDesc::operator==(const FRHITextureDesc &Other) const {
	return Flags == Other.Flags && ClearValue == Other.ClearValue && Extent == Other.Extent && Depth == Other.Depth && ArraySize == Other.ArraySize && NumMips == Other.NumMips && NumSamples == Other.NumSamples && Dimension == Other.Dimension && Format == Other.Format && UAVFormat == Other.UAVFormat;
}

uint32 FRHITextureDesc::GetHash() const {
	uint32 Hash = hash_murmur3_one_64((uint64)Flags);
	Hash = hash_murmur3_one_32((uint32)Extent.X, Hash);
	Hash = hash_murmur3_one_32((uint32)Extent.Y, Hash);
	Hash = hash_murmur3_one_32(Depth | (ArraySize << 16), Hash);
	Hash = hash_murmur3_one_32(NumMips | (NumSamples << 8) | ((uint32)Dimension << 16) | ((uint32)Format << 24), Hash);
	Hash = hash_murmur3_one_32((uint32)UAVFormat, Hash);
	return hash_fmix32(Hash);
}

static RenderingDevice::TextureType ToRDTextureType(ETextureDimension Dimension) {
	switch (Dimension) {
		case ETextureDimension::Texture2D:
			return RenderingDevice::TEXTURE_TYPE_2D;
		case ETextureDimension::Texture2DArray:
			return RenderingDevice::TEXTURE_TYPE_2D_ARRAY;
		case ETextureDimension::Texture3D:
			return RenderingDevice::TEXTURE_TYPE_3D;
		case ETextureDimension::TextureCube:
			return RenderingDevice::TEXTURE_TYPE_CUBE;
		case ETextureDimension::TextureCubeArray:
			return RenderingDevice::TEXTURE_TYPE_CUBE_ARRAY;
	}
	return RenderingDevice::TEXTURE_TYPE_2D;
}

static RenderingDevice::TextureSamples ToRDSamples(uint32 NumSamples) {
	switch (NumSamples) {
		case 2:
			return RenderingDevice::TEXTURE_SAMPLES_2;
		case 4:
			return RenderingDevice::TEXTURE_SAMPLES_4;
		case 8:
			return RenderingDevice::TEXTURE_SAMPLES_8;
		case 16:
			return RenderingDevice::TEXTURE_SAMPLES_16;
		default:
			return RenderingDevice::TEXTURE_SAMPLES_1;
	}
}

RenderingDevice::TextureFormat ToRDTextureFormat(RenderingDevice *RD, const FRHITextureDesc &Desc) {
	RenderingDevice::TextureFormat Format;
	const bool bSRGB = EnumHasAnyFlags(Desc.Flags, TexCreate_SRGB);
	Format.format = GetRDDataFormat(RD, Desc.Format, bSRGB);
	Format.width = (uint32)Desc.Extent.X;
	Format.height = (uint32)Desc.Extent.Y;
	Format.depth = Desc.Dimension == ETextureDimension::Texture3D ? Desc.Depth : 1;
	Format.array_layers = Desc.GetRDLayerCount();
	Format.mipmaps = Desc.NumMips;
	Format.texture_type = ToRDTextureType(Desc.Dimension);
	Format.samples = ToRDSamples(Desc.NumSamples);

	uint32 Usage = RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	if (EnumHasAnyFlags(Desc.Flags, TexCreate_ShaderResource)) Usage |= RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
	if (EnumHasAnyFlags(Desc.Flags, TexCreate_UAV)) Usage |= RenderingDevice::TEXTURE_USAGE_STORAGE_BIT;
	if (EnumHasAnyFlags(Desc.Flags, TexCreate_AtomicCompatible | TexCreate_Atomic64Compatible)) Usage |= RenderingDevice::TEXTURE_USAGE_STORAGE_ATOMIC_BIT;
	if (EnumHasAnyFlags(Desc.Flags, TexCreate_RenderTargetable | TexCreate_ResolveTargetable) && !IsDepthOrStencilFormat(Desc.Format)) Usage |= RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
	if (EnumHasAnyFlags(Desc.Flags, TexCreate_DepthStencilTargetable) || (IsDepthOrStencilFormat(Desc.Format) && EnumHasAnyFlags(Desc.Flags, TexCreate_RenderTargetable))) Usage |= RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if (EnumHasAnyFlags(Desc.Flags, TexCreate_InputAttachmentRead)) Usage |= RenderingDevice::TEXTURE_USAGE_INPUT_ATTACHMENT_BIT;
	if (EnumHasAnyFlags(Desc.Flags, TexCreate_CPUWritable | TexCreate_Dynamic)) Usage |= RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT;
	if (EnumHasAnyFlags(Desc.Flags, TexCreate_CPUReadback)) Usage |= RenderingDevice::TEXTURE_USAGE_CPU_READ_BIT;
	// Without any shader / target usage the texture is still sampleable, as UE textures always are.
	if (!(Usage & (RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) Usage |= RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
	Format.usage_bits = Usage;

	// sRGB textures that are also UAVs need a linear storage view.
	const FPixelFormatInfo &Info = GetPixelFormatInfo(Desc.Format);
	if (bSRGB && Info.RDFormatSRGB != RenderingDevice::DATA_FORMAT_MAX) {
		Format.shareable_formats.push_back(Info.RDFormatSRGB);
		Format.shareable_formats.push_back(Info.RDFormat);
	} else if (Desc.UAVFormat != PF_Unknown && Desc.UAVFormat != Desc.Format) {
		Format.shareable_formats.push_back(Format.format);
		Format.shareable_formats.push_back(GetRDDataFormat(RD, Desc.UAVFormat));
	}
	return Format;
}

FRHITextureDesc FromRDTextureFormat(const RenderingDevice::TextureFormat &Format) {
	FRHITextureDesc Desc;
	bool bSRGB = false;
	Desc.Format = GetPixelFormatFromRD(Format.format, &bSRGB);
	Desc.UAVFormat = Desc.Format;
	Desc.Extent = FIntPoint((int32)Format.width, (int32)Format.height);
	Desc.NumMips = (uint8)Format.mipmaps;
	Desc.NumSamples = (uint8)(1u << (uint32)Format.samples);
	switch (Format.texture_type) {
		case RenderingDevice::TEXTURE_TYPE_3D:
			Desc.Dimension = ETextureDimension::Texture3D;
			Desc.Depth = (uint16)Format.depth;
			break;
		case RenderingDevice::TEXTURE_TYPE_2D_ARRAY:
			Desc.Dimension = ETextureDimension::Texture2DArray;
			Desc.ArraySize = (uint16)Format.array_layers;
			break;
		case RenderingDevice::TEXTURE_TYPE_CUBE:
			Desc.Dimension = ETextureDimension::TextureCube;
			break;
		case RenderingDevice::TEXTURE_TYPE_CUBE_ARRAY:
			Desc.Dimension = ETextureDimension::TextureCubeArray;
			Desc.ArraySize = (uint16)(Format.array_layers / 6);
			break;
		default:
			Desc.Dimension = ETextureDimension::Texture2D;
			break;
	}
	ETextureCreateFlags Flags = TexCreate_None;
	if (Format.usage_bits & RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT) Flags |= TexCreate_ShaderResource;
	if (Format.usage_bits & RenderingDevice::TEXTURE_USAGE_STORAGE_BIT) Flags |= TexCreate_UAV;
	if (Format.usage_bits & RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT) Flags |= TexCreate_RenderTargetable;
	if (Format.usage_bits & RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) Flags |= TexCreate_DepthStencilTargetable;
	if (Format.usage_bits & RenderingDevice::TEXTURE_USAGE_INPUT_ATTACHMENT_BIT) Flags |= TexCreate_InputAttachmentRead;
	if (bSRGB) Flags |= TexCreate_SRGB;
	Desc.Flags = Flags;
	Desc.ClearValue = IsDepthOrStencilFormat(Desc.Format) ? FClearValueBinding::DepthFar : FClearValueBinding::Black;
	return Desc;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Textures.

uint32 FRHITextureViewKey::hash() const {
	uint32 Hash = hash_murmur3_one_32(MipLevel | (NumMipLevels << 8) | (FirstArraySlice << 16));
	Hash = hash_murmur3_one_32(NumArraySlices | ((uint32)Format << 16) | (bUAV << 24), Hash);
	return hash_fmix32(Hash);
}

FRHITexture::FRHITexture(RenderingDevice *InDevice, RID InRID, const FRHITextureDesc &InDesc, const String &InName, bool bInOwnsRID) :
		FRHIViewableResource(InDevice, InRID, InName, bInOwnsRID), Desc(InDesc) {}

FRHITexture::~FRHITexture() {
	ReleaseViews();
	if (bOwnsRID && ResourceRID.is_valid() && Device && Device->texture_is_valid(ResourceRID)) Device->free_rid(ResourceRID);
}

void FRHITexture::ReleaseViews() {
	FWriteScopeLock Lock(GViewLock);
	for (KeyValue<FRHITextureViewKey, FRHIShaderResourceView *> &It : SRVs) memdelete(It.value);
	for (KeyValue<FRHITextureViewKey, FRHIUnorderedAccessView *> &It : UAVs) memdelete(It.value);
	SRVs.clear();
	UAVs.clear();
	if (Device) {
		for (KeyValue<FRHITextureViewKey, RID> &It : ViewRIDs) {
			if (It.value != ResourceRID && Device->texture_is_valid(It.value)) Device->free_rid(It.value);
		}
	}
	ViewRIDs.clear();
}

RID FRHITexture::GetOrCreateViewRID(const FRHITextureViewKey &InKey) {
	FRHITextureViewKey Key = InKey;
	const uint32 TotalSlices = Desc.GetRDLayerCount();
	if (Key.NumMipLevels == 0 || Key.MipLevel + Key.NumMipLevels > Desc.NumMips) Key.NumMipLevels = (uint8)(Desc.NumMips - FMath::Min<uint32>(Key.MipLevel, Desc.NumMips - 1));
	if (Key.NumArraySlices == 0 || (uint32)(Key.FirstArraySlice + Key.NumArraySlices) > TotalSlices) Key.NumArraySlices = (uint16)(TotalSlices - FMath::Min<uint32>(Key.FirstArraySlice, TotalSlices - 1));
	if (Key.Format == Desc.Format && !EnumHasAnyFlags(Desc.Flags, TexCreate_SRGB)) Key.Format = PF_Unknown;

	const bool bWholeResource = Key.MipLevel == 0 && Key.NumMipLevels == Desc.NumMips && Key.FirstArraySlice == 0 && Key.NumArraySlices == TotalSlices;
	// Storage image views must address exactly one mip level.
	const bool bNeedsSingleMip = Key.bUAV && Desc.NumMips > 1;
	const bool bUAVOnSRGB = Key.bUAV && EnumHasAnyFlags(Desc.Flags, TexCreate_SRGB);
	if (bWholeResource && Key.Format == PF_Unknown && !bNeedsSingleMip && !bUAVOnSRGB) return ResourceRID;

	// Owned views live as long as this texture. Views of external textures die with them, which can only happen
	// between graphs, i.e. while recording on the render thread (texture_is_valid() is render thread only).
	const bool bCheckValid = !bOwnsRID && !Device->recording_context_get_current();
	{
		FReadScopeLock Lock(GViewLock);
		if (const RID *Existing = ViewRIDs.getptr(Key)) {
			if (!bCheckValid || Device->texture_is_valid(*Existing)) return *Existing;
		}
	}

	FWriteScopeLock Lock(GViewLock);
	if (const RID *Existing = ViewRIDs.getptr(Key)) {
		if (!bCheckValid || Device->texture_is_valid(*Existing)) return *Existing;
		ViewRIDs.erase(Key);
	}

	RenderingDevice::TextureView View;
	if (Key.Format != PF_Unknown) View.format_override = GetRDDataFormat(Device, Key.Format);
	else if (bUAVOnSRGB) View.format_override = GetPixelFormatInfo(Desc.Format).RDFormat;

	RenderingDevice::TextureSliceType SliceType = RenderingDevice::TEXTURE_SLICE_2D;
	uint32 NumLayers = Key.NumArraySlices;
	if (Desc.IsTexture3D()) {
		SliceType = RenderingDevice::TEXTURE_SLICE_3D;
		NumLayers = 1;
	} else if (Desc.IsTextureCube() && Key.NumArraySlices == 6 && !Key.bUAV) {
		SliceType = RenderingDevice::TEXTURE_SLICE_CUBEMAP;
	} else if (Key.NumArraySlices > 1 || Desc.IsTextureArray() || Desc.IsTextureCube()) {
		SliceType = RenderingDevice::TEXTURE_SLICE_2D_ARRAY;
	}

	RID View_RID;
	if (bWholeResource && !bNeedsSingleMip) View_RID = Device->texture_create_shared(View, ResourceRID);
	else View_RID = Device->texture_create_shared_from_slice(View, ResourceRID, Key.FirstArraySlice, Key.MipLevel, Key.bUAV ? 1 : Key.NumMipLevels, SliceType, SliceType == RenderingDevice::TEXTURE_SLICE_2D_ARRAY ? NumLayers : 0);

	ERR_FAIL_COND_V_MSG(!View_RID.is_valid(), ResourceRID, vformat("[RDG] Failed to create view of texture '%s' (mip %d, slice %d).", Name, Key.MipLevel, Key.FirstArraySlice));
	ViewRIDs.insert(Key, View_RID);
	return View_RID;
}

FRHIShaderResourceView *FRHITexture::GetOrCreateSRV(uint8 MipLevel, uint8 NumMipLevels, uint16 FirstArraySlice, uint16 NumArraySlices, EPixelFormat Format) {
	FRHITextureViewKey Key;
	Key.MipLevel = MipLevel;
	Key.NumMipLevels = NumMipLevels;
	Key.FirstArraySlice = FirstArraySlice;
	Key.NumArraySlices = NumArraySlices;
	Key.Format = Format;
	Key.bUAV = 0;
	const RID ViewRID = GetOrCreateViewRID(Key);
	{
		FReadScopeLock Lock(GViewLock);
		FRHIShaderResourceView *const *Existing = SRVs.getptr(Key);
		if (Existing && (*Existing)->ViewRID == ViewRID) return *Existing;
	}
	FWriteScopeLock Lock(GViewLock);
	if (FRHIShaderResourceView **Existing = SRVs.getptr(Key)) {
		(*Existing)->ViewRID = ViewRID;
		return *Existing;
	}
	FRHIShaderResourceView *SRV = memnew(FRHIShaderResourceView);
	SRV->Resource = this;
	SRV->ViewRID = ViewRID;
	SRV->Format = Format == PF_Unknown ? Desc.Format : Format;
	SRVs.insert(Key, SRV);
	return SRV;
}

FRHIUnorderedAccessView *FRHITexture::GetOrCreateUAV(uint8 MipLevel, uint16 FirstArraySlice, uint16 NumArraySlices, EPixelFormat Format) {
	FRHITextureViewKey Key;
	Key.MipLevel = MipLevel;
	Key.NumMipLevels = 1;
	Key.FirstArraySlice = FirstArraySlice;
	Key.NumArraySlices = NumArraySlices;
	Key.Format = Format;
	Key.bUAV = 1;
	const RID ViewRID = GetOrCreateViewRID(Key);
	{
		FReadScopeLock Lock(GViewLock);
		FRHIUnorderedAccessView *const *Existing = UAVs.getptr(Key);
		if (Existing && (*Existing)->ViewRID == ViewRID) return *Existing;
	}
	FWriteScopeLock Lock(GViewLock);
	if (FRHIUnorderedAccessView **Existing = UAVs.getptr(Key)) {
		(*Existing)->ViewRID = ViewRID;
		return *Existing;
	}
	FRHIUnorderedAccessView *UAV = memnew(FRHIUnorderedAccessView);
	UAV->Resource = this;
	UAV->ViewRID = ViewRID;
	UAV->Format = Format == PF_Unknown ? Desc.Format : Format;
	UAV->MipLevel = MipLevel;
	UAVs.insert(Key, UAV);
	return UAV;
}

RID FRHITexture::GetRenderTargetView(uint8 MipIndex, int16 ArraySlice) {
	FRHITextureViewKey Key;
	Key.MipLevel = MipIndex;
	Key.NumMipLevels = 1;
	Key.FirstArraySlice = ArraySlice < 0 ? 0 : (uint16)ArraySlice;
	Key.NumArraySlices = ArraySlice < 0 ? 0 : 1;
	Key.bUAV = 0;
	if (MipIndex == 0 && ArraySlice < 0 && Desc.NumMips == 1) return ResourceRID;
	return GetOrCreateViewRID(Key);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Buffers.

FRHIBuffer::FRHIBuffer(RenderingDevice *InDevice, RID InRID, uint32 InSize, uint32 InStride, EBufferUsageFlags InUsage, const String &InName, bool bInOwnsRID, ERDBufferType InType) :
		FRHIViewableResource(InDevice, InRID, InName, bInOwnsRID), Size(InSize), Stride(InStride), Usage(InUsage), RDType(InType) {
	SRV.Resource = this;
	SRV.ViewRID = InRID;
	SRV.bIsBuffer = true;
	UAV.Resource = this;
	UAV.ViewRID = InRID;
	UAV.bIsBuffer = true;
}

FRHIBuffer::~FRHIBuffer() {
	// Index arrays depend on the buffer: RD frees them with an owned buffer, the ones of an external buffer are freed here.
	if (!bOwnsRID && Device) {
		for (KeyValue<uint64, RID> &It : IndexArrays) Device->free_rid(It.value);
	}
	IndexArrays.clear();
	if (bOwnsRID && ResourceRID.is_valid() && Device) Device->free_rid(ResourceRID);
}

RID FRHIBuffer::GetOrCreateIndexArray(uint32 FirstIndex, uint32 NumIndices) {
	ERR_FAIL_COND_V_MSG(bOwnsRID && RDType != ERDBufferType::Index, RID(), vformat("[RDG] Buffer '%s' is not an index buffer (bind it with ERHIAccess::VertexOrIndexBuffer and BUF_IndexBuffer).", Name));
	const uint64 Key = ((uint64)FirstIndex << 32) | NumIndices;
	{
		FReadScopeLock Lock(GViewLock);
		if (const RID *Existing = IndexArrays.getptr(Key)) return *Existing;
	}
	FWriteScopeLock Lock(GViewLock);
	if (const RID *Existing = IndexArrays.getptr(Key)) return *Existing;
	const RID IndexArray = Device->index_array_create(ResourceRID, FirstIndex, NumIndices);
	ERR_FAIL_COND_V_MSG(!IndexArray.is_valid(), RID(), vformat("[RDG] Failed to create index array [%d, +%d) of buffer '%s'.", FirstIndex, NumIndices, Name));
	IndexArrays.insert(Key, IndexArray);
	return IndexArray;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Vertex declarations.

namespace {
// Read on every graphics PSO bind (possibly from parallel recording tasks), written when a format is first created.
FRWLock GVertexDeclarationLock;
LocalVector<FRHIVertexDeclaration *> GVertexDeclarations;

RenderingDevice::DataFormat ToRDVertexFormat(EVertexElementType Type) {
	switch (Type) {
		case VET_Float1:
			return RenderingDevice::DATA_FORMAT_R32_SFLOAT;
		case VET_Float2:
			return RenderingDevice::DATA_FORMAT_R32G32_SFLOAT;
		case VET_Float3:
			return RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT;
		case VET_Float4:
			return RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT;
		case VET_PackedNormal:
			return RenderingDevice::DATA_FORMAT_R8G8B8A8_SNORM;
		case VET_UByte4:
			return RenderingDevice::DATA_FORMAT_R8G8B8A8_UINT;
		case VET_UByte4N:
			return RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
		case VET_Color:
			return RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM; // FColor is stored BGRA.
		case VET_Short2:
			return RenderingDevice::DATA_FORMAT_R16G16_SINT;
		case VET_Short4:
			return RenderingDevice::DATA_FORMAT_R16G16B16A16_SINT;
		case VET_Short2N:
			return RenderingDevice::DATA_FORMAT_R16G16_SNORM;
		case VET_Half2:
			return RenderingDevice::DATA_FORMAT_R16G16_SFLOAT;
		case VET_Half4:
			return RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT;
		case VET_Short4N:
			return RenderingDevice::DATA_FORMAT_R16G16B16A16_SNORM;
		case VET_UShort2:
			return RenderingDevice::DATA_FORMAT_R16G16_UINT;
		case VET_UShort4:
			return RenderingDevice::DATA_FORMAT_R16G16B16A16_UINT;
		case VET_UShort2N:
			return RenderingDevice::DATA_FORMAT_R16G16_UNORM;
		case VET_UShort4N:
			return RenderingDevice::DATA_FORMAT_R16G16B16A16_UNORM;
		case VET_URGB10A2N:
			return RenderingDevice::DATA_FORMAT_A2B10G10R10_UNORM_PACK32;
		case VET_UInt:
			return RenderingDevice::DATA_FORMAT_R32_UINT;
		default:
			return RenderingDevice::DATA_FORMAT_MAX;
	}
}
} //namespace

FRHIVertexDeclaration::FRHIVertexDeclaration(const FVertexDeclarationElementList &InElements) :
		Elements(InElements) {
	Hash = hash_murmur3_one_32(Elements.size());
	for (const FVertexElement &Element : Elements) {
		Hash = hash_murmur3_one_32(Element.StreamIndex | (Element.Offset << 8) | ((uint32)Element.Type << 16) | (Element.AttributeIndex << 24), Hash);
		Hash = hash_murmur3_one_32(Element.Stride | (Element.bUseInstanceIndex << 16), Hash);
	}
	Hash = hash_fmix32(Hash);
}

RenderingDevice::VertexFormatID FRHIVertexDeclaration::GetVertexFormat(RenderingDevice *Device) const {
	if (Elements.is_empty() || !Device) return RenderingDevice::INVALID_FORMAT_ID;
	{
		FReadScopeLock Lock(GVertexDeclarationLock);
		if (const RenderingDevice::VertexFormatID *Existing = Formats.getptr(Device)) return *Existing;
	}
	FWriteScopeLock Lock(GVertexDeclarationLock);
	if (const RenderingDevice::VertexFormatID *Existing = Formats.getptr(Device)) return *Existing;

	// One RD binding per element (binding = element index), fed by the buffer of the element's stream.
	Vector<RenderingDevice::VertexAttribute> Attributes;
	for (uint32 Index = 0; Index < Elements.size(); Index++) {
		const FVertexElement &Element = Elements[Index];
		RenderingDevice::VertexAttribute Attribute;
		Attribute.binding = Index;
		Attribute.location = Element.AttributeIndex;
		Attribute.offset = Element.Offset;
		Attribute.format = ToRDVertexFormat(Element.Type);
		Attribute.stride = Element.Stride;
		Attribute.frequency = Element.bUseInstanceIndex ? RenderingDevice::VERTEX_FREQUENCY_INSTANCE : RenderingDevice::VERTEX_FREQUENCY_VERTEX;
		ERR_FAIL_COND_V_MSG(Attribute.format == RenderingDevice::DATA_FORMAT_MAX, RenderingDevice::INVALID_FORMAT_ID, vformat("[RDG] Vertex element %d has an unsupported type (%d).", Index, (int)Element.Type));
		Attributes.push_back(Attribute);
	}
	const RenderingDevice::VertexFormatID Format = Device->vertex_format_create(Attributes);
	Formats.insert(Device, Format);
	return Format;
}

void FRHIVertexDeclaration::ReleaseDevice(RenderingDevice *Device) {
	FWriteScopeLock Lock(GVertexDeclarationLock);
	for (FRHIVertexDeclaration *Declaration : GVertexDeclarations) Declaration->Formats.erase(Device);
}

FRHIVertexDeclaration *RHICreateVertexDeclaration(const FVertexDeclarationElementList &Elements) {
	FRHIVertexDeclaration Candidate(Elements);
	FWriteScopeLock Lock(GVertexDeclarationLock);
	for (FRHIVertexDeclaration *Declaration : GVertexDeclarations) {
		if (Declaration->GetHash() != Candidate.GetHash() || Declaration->GetElements().size() != Elements.size()) continue;
		bool bEqual = true;
		for (uint32 Index = 0; Index < Elements.size() && bEqual; Index++) bEqual = Declaration->GetElements()[Index] == Elements[Index];
		if (bEqual) return Declaration;
	}
	FRHIVertexDeclaration *Declaration = memnew(FRHIVertexDeclaration(Elements));
	GVertexDeclarations.push_back(Declaration);
	return Declaration;
}

void RHIReleaseVertexDeclarations() {
	FWriteScopeLock Lock(GVertexDeclarationLock);
	for (FRHIVertexDeclaration *Declaration : GVertexDeclarations) memdelete(Declaration);
	GVertexDeclarations.clear();
}

FRHIShaderResourceView *FRHIBuffer::GetSRV(EPixelFormat Format) {
	{
		FReadScopeLock Lock(GViewLock);
		if (SRV.Format == Format) return &SRV;
	}
	FWriteScopeLock Lock(GViewLock);
	SRV.Format = Format;
	return &SRV;
}

FRHIUnorderedAccessView *FRHIBuffer::GetUAV(EPixelFormat Format) {
	{
		FReadScopeLock Lock(GViewLock);
		if (UAV.Format == Format) return &UAV;
	}
	FWriteScopeLock Lock(GViewLock);
	UAV.Format = Format;
	return &UAV;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Fixed function states.

static RenderingDevice::SamplerRepeatMode ToRDAddressMode(ESamplerAddressMode Mode) {
	switch (Mode) {
		case AM_Wrap:
			return RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT;
		case AM_Mirror:
			return RenderingDevice::SAMPLER_REPEAT_MODE_MIRRORED_REPEAT;
		case AM_Border:
			return RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER;
		default:
			return RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	}
}

RenderingDevice::SamplerState FRHISamplerState::ToRD() const {
	RenderingDevice::SamplerState State;
	const bool bLinear = Initializer.Filter != SF_Point && Initializer.Filter != SF_AnisotropicPoint;
	State.mag_filter = bLinear ? RenderingDevice::SAMPLER_FILTER_LINEAR : RenderingDevice::SAMPLER_FILTER_NEAREST;
	State.min_filter = State.mag_filter;
	State.mip_filter = (Initializer.Filter == SF_Trilinear || Initializer.Filter == SF_AnisotropicLinear) ? RenderingDevice::SAMPLER_FILTER_LINEAR : RenderingDevice::SAMPLER_FILTER_NEAREST;
	State.repeat_u = ToRDAddressMode(Initializer.AddressU);
	State.repeat_v = ToRDAddressMode(Initializer.AddressV);
	State.repeat_w = ToRDAddressMode(Initializer.AddressW);
	State.lod_bias = Initializer.MipBias;
	State.use_anisotropy = Initializer.Filter == SF_AnisotropicLinear || Initializer.Filter == SF_AnisotropicPoint;
	State.anisotropy_max = (float)FMath::Max(1, Initializer.MaxAnisotropy);
	State.enable_compare = Initializer.SamplerComparisonFunction != SCF_Never;
	State.compare_op = State.enable_compare ? RenderingDevice::COMPARE_OP_LESS : RenderingDevice::COMPARE_OP_ALWAYS;
	State.border_color = Initializer.BorderColor ? RenderingDevice::SAMPLER_BORDER_COLOR_FLOAT_OPAQUE_WHITE : RenderingDevice::SAMPLER_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	return State;
}

uint32 FRHISamplerState::GetHash() const {
	uint32 Hash = hash_murmur3_one_32(Initializer.Filter | (Initializer.AddressU << 8) | (Initializer.AddressV << 16) | (Initializer.AddressW << 24));
	Hash = hash_murmur3_one_float(Initializer.MipBias, Hash);
	Hash = hash_murmur3_one_32((uint32)Initializer.MaxAnisotropy, Hash);
	Hash = hash_murmur3_one_32(Initializer.BorderColor | (Initializer.SamplerComparisonFunction << 16), Hash);
	return hash_fmix32(Hash);
}

static RenderingDevice::BlendFactor ToRDBlendFactor(EBlendFactor Factor) {
	switch (Factor) {
		case BF_Zero:
			return RenderingDevice::BLEND_FACTOR_ZERO;
		case BF_One:
			return RenderingDevice::BLEND_FACTOR_ONE;
		case BF_SourceColor:
			return RenderingDevice::BLEND_FACTOR_SRC_COLOR;
		case BF_InverseSourceColor:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case BF_SourceAlpha:
			return RenderingDevice::BLEND_FACTOR_SRC_ALPHA;
		case BF_InverseSourceAlpha:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case BF_DestAlpha:
			return RenderingDevice::BLEND_FACTOR_DST_ALPHA;
		case BF_InverseDestAlpha:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case BF_DestColor:
			return RenderingDevice::BLEND_FACTOR_DST_COLOR;
		case BF_InverseDestColor:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case BF_ConstantBlendFactor:
			return RenderingDevice::BLEND_FACTOR_CONSTANT_COLOR;
		case BF_InverseConstantBlendFactor:
			return RenderingDevice::BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
	}
	return RenderingDevice::BLEND_FACTOR_ONE;
}

static RenderingDevice::BlendOperation ToRDBlendOp(EBlendOperation Op) {
	switch (Op) {
		case BO_Add:
			return RenderingDevice::BLEND_OP_ADD;
		case BO_Subtract:
			return RenderingDevice::BLEND_OP_SUBTRACT;
		case BO_ReverseSubtract:
			return RenderingDevice::BLEND_OP_REVERSE_SUBTRACT;
		case BO_Min:
			return RenderingDevice::BLEND_OP_MINIMUM;
		case BO_Max:
			return RenderingDevice::BLEND_OP_MAXIMUM;
	}
	return RenderingDevice::BLEND_OP_ADD;
}

RenderingDevice::PipelineColorBlendState FRHIBlendState::ToRD(uint32 NumRenderTargets) const {
	RenderingDevice::PipelineColorBlendState State;
	for (uint32 Index = 0; Index < NumRenderTargets; Index++) {
		const FBlendStateInitializerRHI::FRenderTarget &RT = Initializer.RenderTargets[Initializer.bUseIndependentRenderTargetBlendStates ? Index : 0];
		RenderingDevice::PipelineColorBlendState::Attachment Attachment;
		const bool bOpaque = RT.ColorBlendOp == BO_Add && RT.ColorSrcBlend == BF_One && RT.ColorDestBlend == BF_Zero && RT.AlphaBlendOp == BO_Add && RT.AlphaSrcBlend == BF_One && RT.AlphaDestBlend == BF_Zero;
		Attachment.enable_blend = !bOpaque;
		Attachment.color_blend_op = ToRDBlendOp(RT.ColorBlendOp);
		Attachment.src_color_blend_factor = ToRDBlendFactor(RT.ColorSrcBlend);
		Attachment.dst_color_blend_factor = ToRDBlendFactor(RT.ColorDestBlend);
		Attachment.alpha_blend_op = ToRDBlendOp(RT.AlphaBlendOp);
		Attachment.src_alpha_blend_factor = ToRDBlendFactor(RT.AlphaSrcBlend);
		Attachment.dst_alpha_blend_factor = ToRDBlendFactor(RT.AlphaDestBlend);
		Attachment.write_r = (RT.ColorWriteMask & CW_RED) != 0;
		Attachment.write_g = (RT.ColorWriteMask & CW_GREEN) != 0;
		Attachment.write_b = (RT.ColorWriteMask & CW_BLUE) != 0;
		Attachment.write_a = (RT.ColorWriteMask & CW_ALPHA) != 0;
		State.attachments.push_back(Attachment);
	}
	return State;
}

uint32 FRHIBlendState::GetHash() const {
	uint32 Hash = hash_murmur3_one_32(Initializer.bUseIndependentRenderTargetBlendStates ? 1 : 0);
	for (const FBlendStateInitializerRHI::FRenderTarget &RT : Initializer.RenderTargets) {
		Hash = hash_murmur3_one_32(RT.ColorBlendOp | (RT.ColorSrcBlend << 4) | (RT.ColorDestBlend << 8) | (RT.AlphaBlendOp << 12) | (RT.AlphaSrcBlend << 16) | (RT.AlphaDestBlend << 20) | (RT.ColorWriteMask << 24), Hash);
	}
	return hash_fmix32(Hash);
}

RenderingDevice::PipelineRasterizationState FRHIRasterizerState::ToRD() const {
	RenderingDevice::PipelineRasterizationState State;
	State.wireframe = Initializer.FillMode == FM_Wireframe;
	// UE's CM_CW culls clockwise triangles; Godot's front face is clockwise by default.
	State.cull_mode = Initializer.CullMode == CM_None ? RenderingDevice::POLYGON_CULL_DISABLED : (Initializer.CullMode == CM_CW ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK);
	State.depth_bias_enabled = Initializer.DepthBias != 0.0f || Initializer.SlopeScaleDepthBias != 0.0f;
	State.depth_bias_constant_factor = Initializer.DepthBias;
	State.depth_bias_slope_factor = Initializer.SlopeScaleDepthBias;
	return State;
}

uint32 FRHIRasterizerState::GetHash() const {
	uint32 Hash = hash_murmur3_one_32(Initializer.FillMode | (Initializer.CullMode << 8) | (Initializer.bAllowMSAA << 16));
	Hash = hash_murmur3_one_float(Initializer.DepthBias, Hash);
	Hash = hash_murmur3_one_float(Initializer.SlopeScaleDepthBias, Hash);
	return hash_fmix32(Hash);
}

static RenderingDevice::CompareOperator ToRDCompare(ECompareFunction Function) {
	switch (Function) {
		case CF_Less:
			return RenderingDevice::COMPARE_OP_LESS;
		case CF_LessEqual:
			return RenderingDevice::COMPARE_OP_LESS_OR_EQUAL;
		case CF_Greater:
			return RenderingDevice::COMPARE_OP_GREATER;
		case CF_GreaterEqual:
			return RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		case CF_Equal:
			return RenderingDevice::COMPARE_OP_EQUAL;
		case CF_NotEqual:
			return RenderingDevice::COMPARE_OP_NOT_EQUAL;
		case CF_Never:
			return RenderingDevice::COMPARE_OP_NEVER;
		default:
			return RenderingDevice::COMPARE_OP_ALWAYS;
	}
}

static RenderingDevice::StencilOperation ToRDStencilOp(EStencilOp Op) {
	switch (Op) {
		case SO_Keep:
			return RenderingDevice::STENCIL_OP_KEEP;
		case SO_Zero:
			return RenderingDevice::STENCIL_OP_ZERO;
		case SO_Replace:
			return RenderingDevice::STENCIL_OP_REPLACE;
		case SO_SaturatedIncrement:
			return RenderingDevice::STENCIL_OP_INCREMENT_AND_CLAMP;
		case SO_SaturatedDecrement:
			return RenderingDevice::STENCIL_OP_DECREMENT_AND_CLAMP;
		case SO_Invert:
			return RenderingDevice::STENCIL_OP_INVERT;
		case SO_Increment:
			return RenderingDevice::STENCIL_OP_INCREMENT_AND_WRAP;
		case SO_Decrement:
			return RenderingDevice::STENCIL_OP_DECREMENT_AND_WRAP;
	}
	return RenderingDevice::STENCIL_OP_KEEP;
}

RenderingDevice::PipelineDepthStencilState FRHIDepthStencilState::ToRD(uint32 StencilRef) const {
	RenderingDevice::PipelineDepthStencilState State;
	State.enable_depth_test = Initializer.DepthTest != CF_Always || Initializer.bEnableDepthWrite;
	State.enable_depth_write = Initializer.bEnableDepthWrite;
	State.depth_compare_operator = ToRDCompare(Initializer.DepthTest);
	State.enable_stencil = Initializer.bEnableFrontFaceStencil || Initializer.bEnableBackFaceStencil;
	State.front_op.compare = ToRDCompare(Initializer.FrontFaceStencilTest);
	State.front_op.fail = ToRDStencilOp(Initializer.FrontFaceStencilFailStencilOp);
	State.front_op.depth_fail = ToRDStencilOp(Initializer.FrontFaceDepthFailStencilOp);
	State.front_op.pass = ToRDStencilOp(Initializer.FrontFacePassStencilOp);
	State.front_op.compare_mask = Initializer.StencilReadMask;
	State.front_op.write_mask = Initializer.StencilWriteMask;
	State.front_op.reference = StencilRef;
	const bool bBackFace = Initializer.bEnableBackFaceStencil;
	State.back_op.compare = ToRDCompare(bBackFace ? Initializer.BackFaceStencilTest : Initializer.FrontFaceStencilTest);
	State.back_op.fail = ToRDStencilOp(bBackFace ? Initializer.BackFaceStencilFailStencilOp : Initializer.FrontFaceStencilFailStencilOp);
	State.back_op.depth_fail = ToRDStencilOp(bBackFace ? Initializer.BackFaceDepthFailStencilOp : Initializer.FrontFaceDepthFailStencilOp);
	State.back_op.pass = ToRDStencilOp(bBackFace ? Initializer.BackFacePassStencilOp : Initializer.FrontFacePassStencilOp);
	State.back_op.compare_mask = Initializer.StencilReadMask;
	State.back_op.write_mask = Initializer.StencilWriteMask;
	State.back_op.reference = StencilRef;
	return State;
}

uint32 FRHIDepthStencilState::GetHash() const {
	uint32 Hash = hash_murmur3_one_32((uint32)Initializer.bEnableDepthWrite | (Initializer.DepthTest << 4) | ((uint32)Initializer.bEnableFrontFaceStencil << 8) | (Initializer.FrontFaceStencilTest << 12) | (Initializer.FrontFaceStencilFailStencilOp << 16) | (Initializer.FrontFaceDepthFailStencilOp << 20) | (Initializer.FrontFacePassStencilOp << 24));
	Hash = hash_murmur3_one_32((uint32)Initializer.bEnableBackFaceStencil | (Initializer.BackFaceStencilTest << 4) | (Initializer.BackFaceStencilFailStencilOp << 8) | (Initializer.BackFaceDepthFailStencilOp << 12) | (Initializer.BackFacePassStencilOp << 16) | (Initializer.StencilReadMask << 20), Hash);
	Hash = hash_murmur3_one_32(Initializer.StencilWriteMask, Hash);
	return hash_fmix32(Hash);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// External render targets.

TRefCountPtr<IPooledRenderTarget> CreateRenderTarget(FRHITexture *Texture, const char *Name) {
	ERR_FAIL_NULL_V_MSG(Texture, TRefCountPtr<IPooledRenderTarget>(), "[RDG] CreateRenderTarget called with a null texture.");
	return TRefCountPtr<IPooledRenderTarget>(new FPooledRenderTarget(Texture, false));
}

TRefCountPtr<IPooledRenderTarget> CreateRenderTarget(RenderingDevice *RD, RID Texture, const char *Name) {
	ERR_FAIL_NULL_V(RD, TRefCountPtr<IPooledRenderTarget>());
	return FRDGDeviceContext::Get(RD)->GetOrCreateExternalRenderTarget(Texture, Name);
}

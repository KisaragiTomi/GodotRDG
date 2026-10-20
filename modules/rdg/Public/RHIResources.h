#pragma once

// Thin UE-style RHI objects on top of RenderingDevice RIDs.
// FRHITexture / FRHIBuffer own (or wrap) a RID; views are cached per resource so that the pointers returned to
// pass lambdas stay valid for the lifetime of the underlying resource, as in UE.

#include "RHIDefinitions.h"

#include "core/templates/hash_map.h"

class RenderingDevice;

/** Base class for reference counted RHI objects. */
class FRHIResource : public FRefCountBase {
public:
	virtual ~FRHIResource() = default;
};

/** UE's FRHITextureDesc (also used as FRDGTextureDesc). */
struct FRHITextureDesc {
	ETextureCreateFlags Flags = TexCreate_None;
	FClearValueBinding ClearValue;
	uint32 ExtData = 0;
	FIntPoint Extent = FIntPoint(1, 1);
	uint16 Depth = 1;
	uint16 ArraySize = 1;
	uint8 NumMips = 1;
	uint8 NumSamples = 1;
	ETextureDimension Dimension = ETextureDimension::Texture2D;
	EPixelFormat Format = PF_Unknown;
	EPixelFormat UAVFormat = PF_Unknown;
	uint8 FastVRAMPercentage = 0xFF;

	FRHITextureDesc() = default;
	FRHITextureDesc(ETextureDimension InDimension, ETextureCreateFlags InFlags, EPixelFormat InFormat, FClearValueBinding InClearValue, FIntPoint InExtent, uint16 InDepth, uint16 InArraySize, uint8 InNumMips, uint8 InNumSamples, uint32 InExtData) :
			Flags(InFlags), ClearValue(InClearValue), ExtData(InExtData), Extent(InExtent), Depth(InDepth), ArraySize(InArraySize), NumMips(InNumMips), NumSamples(InNumSamples), Dimension(InDimension), Format(InFormat), UAVFormat(InFormat) {}

	static FRHITextureDesc Create2D(FIntPoint Size, EPixelFormat Format, FClearValueBinding ClearValue, ETextureCreateFlags Flags, uint8 NumMips = 1, uint8 NumSamples = 1, uint32 ExtData = 0) {
		return FRHITextureDesc(ETextureDimension::Texture2D, Flags, Format, ClearValue, Size, 1, 1, NumMips, NumSamples, ExtData);
	}
	static FRHITextureDesc Create2DArray(FIntPoint Size, EPixelFormat Format, FClearValueBinding ClearValue, ETextureCreateFlags Flags, uint16 ArraySize, uint8 NumMips = 1, uint8 NumSamples = 1, uint32 ExtData = 0) {
		return FRHITextureDesc(ETextureDimension::Texture2DArray, Flags, Format, ClearValue, Size, 1, ArraySize, NumMips, NumSamples, ExtData);
	}
	static FRHITextureDesc Create3D(FIntVector Size, EPixelFormat Format, FClearValueBinding ClearValue, ETextureCreateFlags Flags, uint8 NumMips = 1, uint32 ExtData = 0) {
		return FRHITextureDesc(ETextureDimension::Texture3D, Flags, Format, ClearValue, FIntPoint(Size.X, Size.Y), (uint16)Size.Z, 1, NumMips, 1, ExtData);
	}
	static FRHITextureDesc CreateCube(uint32 Size, EPixelFormat Format, FClearValueBinding ClearValue, ETextureCreateFlags Flags, uint8 NumMips = 1, uint8 NumSamples = 1, uint32 ExtData = 0) {
		return FRHITextureDesc(ETextureDimension::TextureCube, Flags, Format, ClearValue, FIntPoint((int32)Size, (int32)Size), 1, 1, NumMips, NumSamples, ExtData);
	}
	static FRHITextureDesc CreateCubeArray(uint32 Size, EPixelFormat Format, FClearValueBinding ClearValue, ETextureCreateFlags Flags, uint16 ArraySize, uint8 NumMips = 1, uint8 NumSamples = 1, uint32 ExtData = 0) {
		return FRHITextureDesc(ETextureDimension::TextureCubeArray, Flags, Format, ClearValue, FIntPoint((int32)Size, (int32)Size), 1, ArraySize, NumMips, NumSamples, ExtData);
	}

	bool IsTexture2D() const { return Dimension == ETextureDimension::Texture2D || Dimension == ETextureDimension::Texture2DArray; }
	bool IsTexture3D() const { return Dimension == ETextureDimension::Texture3D; }
	bool IsTextureCube() const { return Dimension == ETextureDimension::TextureCube || Dimension == ETextureDimension::TextureCubeArray; }
	bool IsTextureArray() const { return Dimension == ETextureDimension::Texture2DArray || Dimension == ETextureDimension::TextureCubeArray; }
	bool IsMipChain() const { return NumMips > 1; }
	bool IsMultisample() const { return NumSamples > 1; }
	FIntVector GetSize() const { return FIntVector(Extent.X, Extent.Y, Depth); }
	/** Number of RenderingDevice layers (cube faces count as layers). */
	uint32 GetRDLayerCount() const { return IsTextureCube() ? 6u * ArraySize : ArraySize; }
	FIntPoint GetMipExtent(uint32 MipIndex) const { return FIntPoint(FMath::Max(Extent.X >> MipIndex, 1), FMath::Max(Extent.Y >> MipIndex, 1)); }

	bool IsValid() const;
	bool operator==(const FRHITextureDesc &Other) const;
	bool operator!=(const FRHITextureDesc &Other) const { return !(*this == Other); }
	uint32 GetHash() const;
};

/** Builds the RenderingDevice texture format for a UE texture desc (adds copy/clear bits RDG relies on). */
RenderingDevice::TextureFormat ToRDTextureFormat(RenderingDevice *RD, const FRHITextureDesc &Desc);
/** Inverse mapping, used to describe externally created textures. */
FRHITextureDesc FromRDTextureFormat(const RenderingDevice::TextureFormat &Format);

class FRHITexture;
class FRHIBuffer;

class FRHIViewableResource : public FRHIResource {
public:
	FRHIViewableResource(RenderingDevice *InDevice, RID InRID, const String &InName, bool bInOwnsRID) :
			Device(InDevice), ResourceRID(InRID), Name(InName), bOwnsRID(bInOwnsRID) {}

	RID GetRID() const { return ResourceRID; }
	RenderingDevice *GetDevice() const { return Device; }
	const String &GetName() const { return Name; }
	bool OwnsRID() const { return bOwnsRID; }

protected:
	RenderingDevice *Device = nullptr;
	RID ResourceRID;
	String Name;
	bool bOwnsRID = true;
};

/** Shader resource view of a texture or buffer. */
class FRHIShaderResourceView {
public:
	FRHIViewableResource *Resource = nullptr;
	RID ViewRID;
	EPixelFormat Format = PF_Unknown;
	bool bIsBuffer = false;
};

/** Unordered access view of a texture or buffer. */
class FRHIUnorderedAccessView {
public:
	FRHIViewableResource *Resource = nullptr;
	RID ViewRID;
	EPixelFormat Format = PF_Unknown;
	bool bIsBuffer = false;
	uint32 MipLevel = 0;
};

struct FRHITextureViewKey {
	uint8 MipLevel = 0;
	uint8 NumMipLevels = 0;
	uint16 FirstArraySlice = 0;
	uint16 NumArraySlices = 0;
	EPixelFormat Format = PF_Unknown;
	uint8 bUAV = 0;

	bool operator==(const FRHITextureViewKey &O) const { return MipLevel == O.MipLevel && NumMipLevels == O.NumMipLevels && FirstArraySlice == O.FirstArraySlice && NumArraySlices == O.NumArraySlices && Format == O.Format && bUAV == O.bUAV; }
	uint32 hash() const;
};

struct FRHITextureViewKeyHasher {
	static uint32 hash(const FRHITextureViewKey &Key) { return Key.hash(); }
};

class FRHITexture : public FRHIViewableResource {
public:
	FRHITexture(RenderingDevice *InDevice, RID InRID, const FRHITextureDesc &InDesc, const String &InName, bool bInOwnsRID);
	~FRHITexture() override;

	const FRHITextureDesc &GetDesc() const { return Desc; }
	FIntPoint GetSizeXY() const { return Desc.Extent; }
	FIntVector GetSizeXYZ() const { return Desc.GetSize(); }
	EPixelFormat GetFormat() const { return Desc.Format; }
	uint32 GetNumMips() const { return Desc.NumMips; }
	uint32 GetNumSamples() const { return Desc.NumSamples; }

	/** Returns a RID usable as a sampled/storage/attachment view of the requested subresource range. */
	RID GetOrCreateViewRID(const FRHITextureViewKey &Key);
	FRHIShaderResourceView *GetOrCreateSRV(uint8 MipLevel, uint8 NumMipLevels, uint16 FirstArraySlice, uint16 NumArraySlices, EPixelFormat Format);
	FRHIUnorderedAccessView *GetOrCreateUAV(uint8 MipLevel, uint16 FirstArraySlice, uint16 NumArraySlices, EPixelFormat Format);
	/** Attachment view for render target bindings (mip / array slice). */
	RID GetRenderTargetView(uint8 MipIndex, int16 ArraySlice);

	void ReleaseViews();

private:
	FRHITextureDesc Desc;
	HashMap<FRHITextureViewKey, RID, FRHITextureViewKeyHasher> ViewRIDs;
	HashMap<FRHITextureViewKey, FRHIShaderResourceView *, FRHITextureViewKeyHasher> SRVs;
	HashMap<FRHITextureViewKey, FRHIUnorderedAccessView *, FRHITextureViewKeyHasher> UAVs;
};

/** Kind of RenderingDevice buffer behind an FRHIBuffer. Vertex and index buffers are RD vertex / index buffers (RDG creates
 *  them for buffers bound with ERHIAccess::VertexOrIndexBuffer), the others RD storage / uniform buffers. */
enum class ERDBufferType : uint8 {
	Storage,
	Uniform,
	Vertex,
	Index,
};

class FRHIBuffer : public FRHIViewableResource {
public:
	FRHIBuffer(RenderingDevice *InDevice, RID InRID, uint32 InSize, uint32 InStride, EBufferUsageFlags InUsage, const String &InName, bool bInOwnsRID, ERDBufferType InType = ERDBufferType::Storage);
	~FRHIBuffer() override;

	uint32 GetSize() const { return Size; }
	uint32 GetStride() const { return Stride; }
	EBufferUsageFlags GetUsage() const { return Usage; }
	ERDBufferType GetRDType() const { return RDType; }

	FRHIShaderResourceView *GetSRV(EPixelFormat Format = PF_Unknown);
	FRHIUnorderedAccessView *GetUAV(EPixelFormat Format = PF_Unknown);
	/** RD index array of [FirstIndex, FirstIndex + NumIndices) of an index buffer (RD binds index arrays), created on first use. */
	RID GetOrCreateIndexArray(uint32 FirstIndex, uint32 NumIndices);

private:
	uint32 Size = 0;
	uint32 Stride = 0;
	EBufferUsageFlags Usage = BUF_None;
	ERDBufferType RDType = ERDBufferType::Storage;
	HashMap<uint64, RID> IndexArrays;
	FRHIShaderResourceView SRV;
	FRHIUnorderedAccessView UAV;
};

/** Sampler state description; the RenderingDevice sampler is created lazily per device. */
struct FSamplerStateInitializerRHI {
	ESamplerFilter Filter = SF_Point;
	ESamplerAddressMode AddressU = AM_Wrap;
	ESamplerAddressMode AddressV = AM_Wrap;
	ESamplerAddressMode AddressW = AM_Wrap;
	float MipBias = 0.0f;
	int32 MaxAnisotropy = 0;
	uint32 BorderColor = 0;
	ESamplerCompareFunction SamplerComparisonFunction = SCF_Never;
};

class FRHISamplerState {
public:
	explicit FRHISamplerState(const FSamplerStateInitializerRHI &InInitializer) :
			Initializer(InInitializer) {}
	const FSamplerStateInitializerRHI Initializer;
	RenderingDevice::SamplerState ToRD() const;
	uint32 GetHash() const;
};

struct FBlendStateInitializerRHI {
	struct FRenderTarget {
		EBlendOperation ColorBlendOp = BO_Add;
		EBlendFactor ColorSrcBlend = BF_One;
		EBlendFactor ColorDestBlend = BF_Zero;
		EBlendOperation AlphaBlendOp = BO_Add;
		EBlendFactor AlphaSrcBlend = BF_One;
		EBlendFactor AlphaDestBlend = BF_Zero;
		EColorWriteMask ColorWriteMask = CW_RGBA;
	};
	FRenderTarget RenderTargets[MaxSimultaneousRenderTargets];
	bool bUseIndependentRenderTargetBlendStates = false;
};

class FRHIBlendState {
public:
	explicit FRHIBlendState(const FBlendStateInitializerRHI &InInitializer) :
			Initializer(InInitializer) {}
	const FBlendStateInitializerRHI Initializer;
	RenderingDevice::PipelineColorBlendState ToRD(uint32 NumRenderTargets) const;
	uint32 GetHash() const;
};

struct FRasterizerStateInitializerRHI {
	ERasterizerFillMode FillMode = FM_Solid;
	ERasterizerCullMode CullMode = CM_None;
	float DepthBias = 0.0f;
	float SlopeScaleDepthBias = 0.0f;
	bool bAllowMSAA = true;
};

class FRHIRasterizerState {
public:
	explicit FRHIRasterizerState(const FRasterizerStateInitializerRHI &InInitializer) :
			Initializer(InInitializer) {}
	const FRasterizerStateInitializerRHI Initializer;
	RenderingDevice::PipelineRasterizationState ToRD() const;
	uint32 GetHash() const;
};

struct FDepthStencilStateInitializerRHI {
	bool bEnableDepthWrite = true;
	ECompareFunction DepthTest = CF_DepthNearOrEqual;
	bool bEnableFrontFaceStencil = false;
	ECompareFunction FrontFaceStencilTest = CF_Always;
	EStencilOp FrontFaceStencilFailStencilOp = SO_Keep;
	EStencilOp FrontFaceDepthFailStencilOp = SO_Keep;
	EStencilOp FrontFacePassStencilOp = SO_Keep;
	bool bEnableBackFaceStencil = false;
	ECompareFunction BackFaceStencilTest = CF_Always;
	EStencilOp BackFaceStencilFailStencilOp = SO_Keep;
	EStencilOp BackFaceDepthFailStencilOp = SO_Keep;
	EStencilOp BackFacePassStencilOp = SO_Keep;
	uint8 StencilReadMask = 0xFF;
	uint8 StencilWriteMask = 0xFF;
};

class FRHIDepthStencilState {
public:
	explicit FRHIDepthStencilState(const FDepthStencilStateInitializerRHI &InInitializer) :
			Initializer(InInitializer) {}
	const FDepthStencilStateInitializerRHI Initializer;
	RenderingDevice::PipelineDepthStencilState ToRD(uint32 StencilRef) const;
	uint32 GetHash() const;
};

class FShader;

/** Compiled shader stage. Owns the SPIR-V and its reflection; compute shaders also own their RD shader + pipeline. */
class FRHIShader : public FRHIResource {
public:
	EShaderFrequency Frequency = SF_Compute;
	FShader *Shader = nullptr;
};

class FRHIComputeShader : public FRHIShader {};
class FRHIVertexShader : public FRHIShader {};
class FRHIPixelShader : public FRHIShader {};
class FRHIGraphicsShader : public FRHIShader {};

/** UE's EVertexElementType. */
enum EVertexElementType : uint8 {
	VET_None,
	VET_Float1,
	VET_Float2,
	VET_Float3,
	VET_Float4,
	VET_PackedNormal,
	VET_UByte4,
	VET_UByte4N,
	VET_Color,
	VET_Short2,
	VET_Short4,
	VET_Short2N,
	VET_Half2,
	VET_Half4,
	VET_Short4N,
	VET_UShort2,
	VET_UShort4,
	VET_UShort2N,
	VET_UShort4N,
	VET_URGB10A2N,
	VET_UInt,
	VET_MAX
};

enum {
	MaxVertexElementCount = 16
};

/** UE's FVertexElement. AttributeIndex is the `layout(location = N)` of the vertex shader input. */
struct FVertexElement {
	uint8 StreamIndex = 0;
	uint8 Offset = 0;
	EVertexElementType Type = VET_None;
	uint8 AttributeIndex = 0;
	uint16 Stride = 0;
	uint16 bUseInstanceIndex = 0;

	FVertexElement() = default;
	FVertexElement(uint8 InStreamIndex, uint8 InOffset, EVertexElementType InType, uint8 InAttributeIndex, uint16 InStride, bool bInUseInstanceIndex = false) :
			StreamIndex(InStreamIndex), Offset(InOffset), Type(InType), AttributeIndex(InAttributeIndex), Stride(InStride), bUseInstanceIndex(bInUseInstanceIndex) {}
	bool operator==(const FVertexElement &Other) const { return StreamIndex == Other.StreamIndex && Offset == Other.Offset && Type == Other.Type && AttributeIndex == Other.AttributeIndex && Stride == Other.Stride && bUseInstanceIndex == Other.bUseInstanceIndex; }
};

typedef LocalVector<FVertexElement> FVertexDeclarationElementList;

/** Vertex input layout (UE's FRHIVertexDeclaration). Without elements the vertex shader has no inputs and geometry comes
 *  from gl_VertexIndex. Each element is its own RD vertex binding, fed by the buffer of its stream. */
class FRHIVertexDeclaration {
public:
	FRHIVertexDeclaration() = default;
	explicit FRHIVertexDeclaration(const FVertexDeclarationElementList &InElements);

	bool GetInitializer(FVertexDeclarationElementList &OutElements) const {
		OutElements = Elements;
		return true;
	}
	const FVertexDeclarationElementList &GetElements() const { return Elements; }
	bool HasElements() const { return !Elements.is_empty(); }
	uint32 GetHash() const { return Hash; }
	/** RD vertex format on a device (INVALID_FORMAT_ID without elements), created on first use. */
	RenderingDevice::VertexFormatID GetVertexFormat(RenderingDevice *Device) const;
	/** Forgets the vertex formats of a device that is about to be freed. */
	static void ReleaseDevice(RenderingDevice *Device);

private:
	FVertexDeclarationElementList Elements;
	uint32 Hash = 0;
	mutable HashMap<RenderingDevice *, RenderingDevice::VertexFormatID> Formats;
};

/** Returns the shared declaration for these elements (UE's PipelineStateCache::GetOrCreateVertexDeclaration). */
FRHIVertexDeclaration *RHICreateVertexDeclaration(const FVertexDeclarationElementList &Elements);
/** Frees the shared declarations (module shutdown). */
void RHIReleaseVertexDeclarations();

namespace PipelineStateCache {
inline FRHIVertexDeclaration *GetOrCreateVertexDeclaration(const FVertexDeclarationElementList &Elements) {
	return RHICreateVertexDeclaration(Elements);
}
} //namespace PipelineStateCache

struct FBoundShaderStateInput {
	FRHIVertexDeclaration *VertexDeclarationRHI = nullptr;
	FRHIVertexShader *VertexShaderRHI = nullptr;
	FRHIPixelShader *PixelShaderRHI = nullptr;
};

class FGraphicsPipelineStateInitializer {
public:
	FBoundShaderStateInput BoundShaderState;
	FRHIBlendState *BlendState = nullptr;
	FRHIRasterizerState *RasterizerState = nullptr;
	FRHIDepthStencilState *DepthStencilState = nullptr;
	EPrimitiveType PrimitiveType = PT_TriangleList;
};

struct FRHICopyTextureInfo {
	FIntVector Size = FIntVector(0);
	FIntVector SourcePosition = FIntVector(0);
	FIntVector DestPosition = FIntVector(0);
	uint32 SourceSliceIndex = 0;
	uint32 DestSliceIndex = 0;
	uint32 NumSlices = 1;
	uint32 SourceMipIndex = 0;
	uint32 DestMipIndex = 0;
	uint32 NumMips = 1;
};

struct FResolveRect {
	int32 X1 = -1, Y1 = -1, X2 = -1, Y2 = -1;
	FResolveRect() = default;
	FResolveRect(int32 InX1, int32 InY1, int32 InX2, int32 InY2) :
			X1(InX1), Y1(InY1), X2(InX2), Y2(InY2) {}
	bool IsValid() const { return X1 >= 0 && Y1 >= 0 && X2 > X1 && Y2 > Y1; }
};

/** Pooled render target, UE's IPooledRenderTarget. */
class IPooledRenderTarget : public FRefCountBase {
public:
	virtual FRHITexture *GetRHI() const = 0;
	virtual const FRHITextureDesc &GetDesc() const = 0;
	virtual bool IsFree() const = 0;
};

/** Wraps an externally owned texture (e.g. Godot's scene color) so it can be registered into a graph. */
TRefCountPtr<IPooledRenderTarget> CreateRenderTarget(FRHITexture *Texture, const char *Name);
/** Godot convenience: wraps a RenderingDevice texture RID. The wrapper does not free the RID. */
TRefCountPtr<IPooledRenderTarget> CreateRenderTarget(RenderingDevice *RD, RID Texture, const char *Name);

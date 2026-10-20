#pragma once

#include "PixelFormat.h"
#include "RDGCore.h"

namespace ERHIFeatureLevel {
enum Type : uint8 {
	ES3_1,
	SM5,
	SM6,
	Num
};
}

enum EShaderPlatform : uint16 {
	SP_VULKAN_SM5 = 0,
	SP_NumPlatforms,
};

extern ERHIFeatureLevel::Type GMaxRHIFeatureLevel;
extern EShaderPlatform GMaxRHIShaderPlatform;

inline bool IsFeatureLevelSupported(EShaderPlatform, ERHIFeatureLevel::Type) { return true; }

enum EShaderFrequency : uint8 {
	SF_Vertex = 0,
	SF_Pixel = 1,
	SF_Compute = 2,
	SF_NumFrequencies,
};

/** Pipelines a pass can run on. AsyncCompute runs on the RenderingDevice's async compute queue when the device has one
 *  (see RenderingDevice::async_compute_is_supported), otherwise async compute passes run on Graphics. */
enum class ERHIPipeline : uint8 {
	Graphics = 1 << 0,
	AsyncCompute = 1 << 1,
	None = 0,
	All = Graphics | AsyncCompute,
	Num = 2,
};
ENUM_CLASS_FLAGS(ERHIPipeline)

inline const char *GetRHIPipelineName(ERHIPipeline Pipeline) {
	switch (Pipeline) {
		case ERHIPipeline::Graphics:
			return "Graphics";
		case ERHIPipeline::AsyncCompute:
			return "AsyncCompute";
		case ERHIPipeline::All:
			return "Graphics|AsyncCompute";
		default:
			return "None";
	}
}

inline uint32 GetRHIPipelineIndex(ERHIPipeline Pipeline) {
	return Pipeline == ERHIPipeline::AsyncCompute ? 1 : 0;
}

/** Resource access states, identical to UE's ERHIAccess. RDG uses them for dependency tracking and validation;
 *  the actual Vulkan/D3D12 barriers are emitted by Godot's RenderingDeviceGraph. */
enum class ERHIAccess : uint32 {
	Unknown = 0,
	CPURead = 1 << 0,
	Present = 1 << 1,
	IndirectArgs = 1 << 2,
	VertexOrIndexBuffer = 1 << 3,
	SRVCompute = 1 << 4,
	SRVGraphics = 1 << 5,
	CopySrc = 1 << 6,
	ResolveSrc = 1 << 7,
	DSVRead = 1 << 8,
	UAVCompute = 1 << 9,
	UAVGraphics = 1 << 10,
	RTV = 1 << 11,
	CopyDest = 1 << 12,
	ResolveDst = 1 << 13,
	DSVWrite = 1 << 14,
	ShadingRateSource = 1 << 15,

	Last = ShadingRateSource,
	None = Unknown,
	Mask = (Last << 1) - 1,

	SRVMask = SRVCompute | SRVGraphics,
	UAVMask = UAVCompute | UAVGraphics,
	ReadOnlyExclusiveMask = CPURead | Present | IndirectArgs | VertexOrIndexBuffer | SRVGraphics | SRVCompute | CopySrc | ResolveSrc | ShadingRateSource,
	ReadOnlyExclusiveComputeMask = CPURead | IndirectArgs | SRVCompute | CopySrc,
	ReadOnlyMask = ReadOnlyExclusiveMask | DSVRead | ShadingRateSource,
	ReadableMask = ReadOnlyMask | UAVMask,
	WriteOnlyExclusiveMask = RTV | CopyDest | ResolveDst,
	WriteOnlyMask = WriteOnlyExclusiveMask | DSVWrite,
	WritableMask = WriteOnlyMask | UAVMask,
	SRVGraphicsPixel = SRVGraphics,
	SRVGraphicsNonPixel = SRVGraphics,
	UAVGraphicsPixel = UAVGraphics,
};
ENUM_CLASS_FLAGS(ERHIAccess)

inline bool IsWritableAccess(ERHIAccess Access) { return EnumHasAnyFlags(Access, ERHIAccess::WritableMask); }
inline bool IsReadOnlyAccess(ERHIAccess Access) { return EnumHasAnyFlags(Access, ERHIAccess::ReadOnlyMask) && !IsWritableAccess(Access); }
inline bool IsReadOnlyExclusiveAccess(ERHIAccess Access) { return EnumHasAnyFlags(Access, ERHIAccess::ReadOnlyExclusiveMask) && !EnumHasAnyFlags(Access, ~ERHIAccess::ReadOnlyExclusiveMask); }
const char *GetRHIAccessName(ERHIAccess Access);

enum class ETextureDimension : uint8 {
	Texture2D,
	Texture2DArray,
	Texture3D,
	TextureCube,
	TextureCubeArray,
};

enum class ETextureCreateFlags : uint64 {
	None = 0,
	RenderTargetable = 1ull << 0,
	ResolveTargetable = 1ull << 1,
	DepthStencilTargetable = 1ull << 2,
	ShaderResource = 1ull << 3,
	SRGB = 1ull << 4,
	CPUWritable = 1ull << 5,
	NoTiling = 1ull << 6,
	VideoDecode = 1ull << 7,
	Dynamic = 1ull << 8,
	InputAttachmentRead = 1ull << 9,
	Foveation = 1ull << 10,
	Tiling3D = 1ull << 11,
	Memoryless = 1ull << 12,
	UAV = 1ull << 16,
	Presentable = 1ull << 17,
	CPUReadback = 1ull << 18,
	FastVRAM = 1ull << 21,
	NoFastClear = 1ull << 25,
	DepthStencilResolveTarget = 1ull << 26,
	Atomic64Compatible = 1ull << 33,
	AtomicCompatible = 1ull << 36,
	Transient = 1ull << 45,
};
ENUM_CLASS_FLAGS(ETextureCreateFlags)

#define TexCreate_None ETextureCreateFlags::None
#define TexCreate_RenderTargetable ETextureCreateFlags::RenderTargetable
#define TexCreate_ResolveTargetable ETextureCreateFlags::ResolveTargetable
#define TexCreate_DepthStencilTargetable ETextureCreateFlags::DepthStencilTargetable
#define TexCreate_ShaderResource ETextureCreateFlags::ShaderResource
#define TexCreate_SRGB ETextureCreateFlags::SRGB
#define TexCreate_CPUWritable ETextureCreateFlags::CPUWritable
#define TexCreate_NoTiling ETextureCreateFlags::NoTiling
#define TexCreate_Dynamic ETextureCreateFlags::Dynamic
#define TexCreate_InputAttachmentRead ETextureCreateFlags::InputAttachmentRead
#define TexCreate_Memoryless ETextureCreateFlags::Memoryless
#define TexCreate_UAV ETextureCreateFlags::UAV
#define TexCreate_Presentable ETextureCreateFlags::Presentable
#define TexCreate_CPUReadback ETextureCreateFlags::CPUReadback
#define TexCreate_FastVRAM ETextureCreateFlags::FastVRAM
#define TexCreate_NoFastClear ETextureCreateFlags::NoFastClear
#define TexCreate_DepthStencilResolveTarget ETextureCreateFlags::DepthStencilResolveTarget
#define TexCreate_Atomic64Compatible ETextureCreateFlags::Atomic64Compatible
#define TexCreate_AtomicCompatible ETextureCreateFlags::AtomicCompatible
#define TexCreate_Transient ETextureCreateFlags::Transient

enum class EBufferUsageFlags : uint32 {
	None = 0,
	Static = 1 << 0,
	Dynamic = 1 << 1,
	Volatile = 1 << 2,
	UnorderedAccess = 1 << 3,
	ByteAddressBuffer = 1 << 4,
	SourceCopy = 1 << 5,
	DrawIndirect = 1 << 7,
	ShaderResource = 1 << 8,
	KeepCPUAccessible = 1 << 9,
	FastVRAM = 1 << 10,
	Shared = 1 << 12,
	AccelerationStructure = 1 << 13,
	VertexBuffer = 1 << 14,
	IndexBuffer = 1 << 15,
	StructuredBuffer = 1 << 16,
	MultiGPUAllocate = 1 << 17,
	MultiGPUGraphIgnore = 1 << 18,
	RayTracingScratch = (1 << 19) | UnorderedAccess,
	NullResource = 1 << 20,
	UniformBuffer = 1 << 21,
	ReservedResource = 1 << 22,

	AnyDynamic = Dynamic | Volatile,
};
ENUM_CLASS_FLAGS(EBufferUsageFlags)

#define BUF_None EBufferUsageFlags::None
#define BUF_Static EBufferUsageFlags::Static
#define BUF_Dynamic EBufferUsageFlags::Dynamic
#define BUF_Volatile EBufferUsageFlags::Volatile
#define BUF_UnorderedAccess EBufferUsageFlags::UnorderedAccess
#define BUF_ByteAddressBuffer EBufferUsageFlags::ByteAddressBuffer
#define BUF_SourceCopy EBufferUsageFlags::SourceCopy
#define BUF_DrawIndirect EBufferUsageFlags::DrawIndirect
#define BUF_ShaderResource EBufferUsageFlags::ShaderResource
#define BUF_KeepCPUAccessible EBufferUsageFlags::KeepCPUAccessible
#define BUF_FastVRAM EBufferUsageFlags::FastVRAM
#define BUF_VertexBuffer EBufferUsageFlags::VertexBuffer
#define BUF_IndexBuffer EBufferUsageFlags::IndexBuffer
#define BUF_StructuredBuffer EBufferUsageFlags::StructuredBuffer
#define BUF_UniformBuffer EBufferUsageFlags::UniformBuffer

enum class ERenderTargetLoadAction : uint8 {
	ENoAction,
	ELoad,
	EClear,
	Num,
};

enum class ERenderTargetStoreAction : uint8 {
	ENoAction,
	EStore,
	EMultisampleResolve,
	Num,
};

/** Depth / stencil access for a depth target binding. */
class FExclusiveDepthStencil {
public:
	enum Type : uint8 {
		DepthNop = 0x00,
		DepthRead = 0x01,
		DepthWrite = 0x02,
		DepthMask = 0x0f,
		StencilNop = 0x00,
		StencilRead = 0x10,
		StencilWrite = 0x20,
		StencilMask = 0xf0,

		DepthNop_StencilNop = DepthNop + StencilNop,
		DepthRead_StencilNop = DepthRead + StencilNop,
		DepthWrite_StencilNop = DepthWrite + StencilNop,
		DepthNop_StencilRead = DepthNop + StencilRead,
		DepthRead_StencilRead = DepthRead + StencilRead,
		DepthWrite_StencilRead = DepthWrite + StencilRead,
		DepthNop_StencilWrite = DepthNop + StencilWrite,
		DepthRead_StencilWrite = DepthRead + StencilWrite,
		DepthWrite_StencilWrite = DepthWrite + StencilWrite,
	};

	FExclusiveDepthStencil(Type InValue = DepthNop_StencilNop) :
			Value(InValue) {}

	bool IsUsingDepthStencil() const { return Value != DepthNop_StencilNop; }
	bool IsUsingDepth() const { return (Value & DepthMask) != DepthNop; }
	bool IsUsingStencil() const { return (Value & StencilMask) != StencilNop; }
	bool IsDepthWrite() const { return (Value & DepthMask) == DepthWrite; }
	bool IsDepthRead() const { return (Value & DepthMask) == DepthRead; }
	bool IsStencilWrite() const { return (Value & StencilMask) == StencilWrite; }
	bool IsStencilRead() const { return (Value & StencilMask) == StencilRead; }
	bool IsAnyWrite() const { return IsDepthWrite() || IsStencilWrite(); }
	Type GetIndex() const { return Value; }
	bool operator==(const FExclusiveDepthStencil &Rhs) const { return Value == Rhs.Value; }
	bool operator!=(const FExclusiveDepthStencil &Rhs) const { return Value != Rhs.Value; }

private:
	Type Value;
};

enum class EClearBinding : uint8 {
	ENoneBound,
	EColorBound,
	EDepthStencilBound,
};

/** Optimized clear value of a texture, used by ERenderTargetLoadAction::EClear. */
struct FClearValueBinding {
	EClearBinding ColorBinding = EClearBinding::EColorBound;
	float Color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float Depth = 0.0f;
	uint32 Stencil = 0;

	FClearValueBinding() = default;
	explicit FClearValueBinding(EClearBinding NoBinding) :
			ColorBinding(NoBinding) {}
	explicit FClearValueBinding(const FLinearColor &InClearColor) :
			ColorBinding(EClearBinding::EColorBound) {
		Color[0] = InClearColor.R;
		Color[1] = InClearColor.G;
		Color[2] = InClearColor.B;
		Color[3] = InClearColor.A;
	}
	explicit FClearValueBinding(float DepthClearValue, uint32 StencilClearValue = 0) :
			ColorBinding(EClearBinding::EDepthStencilBound), Depth(DepthClearValue), Stencil(StencilClearValue) {}

	FLinearColor GetClearColor() const { return FLinearColor(Color[0], Color[1], Color[2], Color[3]); }
	void GetDepthStencil(float &OutDepth, uint32 &OutStencil) const {
		OutDepth = Depth;
		OutStencil = Stencil;
	}
	bool operator==(const FClearValueBinding &O) const;
	bool operator!=(const FClearValueBinding &O) const { return !(*this == O); }

	static const FClearValueBinding None;
	static const FClearValueBinding Black;
	static const FClearValueBinding BlackMaxAlpha;
	static const FClearValueBinding White;
	static const FClearValueBinding Transparent;
	/** Godot uses reverse-Z: the far plane is 0. */
	static const FClearValueBinding DepthOne;
	static const FClearValueBinding DepthZero;
	static const FClearValueBinding DepthFar;
	static const FClearValueBinding DepthNear;
	static const FClearValueBinding Green;
	static const FClearValueBinding DefaultNormal8Bit;
};

enum ECompareFunction : uint8 {
	CF_Less,
	CF_LessEqual,
	CF_Greater,
	CF_GreaterEqual,
	CF_Equal,
	CF_NotEqual,
	CF_Never,
	CF_Always,
	/** Reverse-Z aliases (Godot 4.3+ renders with reverse-Z like UE). */
	CF_DepthNearOrEqual = CF_GreaterEqual,
	CF_DepthNear = CF_Greater,
	CF_DepthFartherOrEqual = CF_LessEqual,
	CF_DepthFarther = CF_Less,
};

enum EStencilOp : uint8 {
	SO_Keep,
	SO_Zero,
	SO_Replace,
	SO_SaturatedIncrement,
	SO_SaturatedDecrement,
	SO_Invert,
	SO_Increment,
	SO_Decrement,
};

enum EBlendOperation : uint8 {
	BO_Add,
	BO_Subtract,
	BO_Min,
	BO_Max,
	BO_ReverseSubtract,
};

enum EBlendFactor : uint8 {
	BF_Zero,
	BF_One,
	BF_SourceColor,
	BF_InverseSourceColor,
	BF_SourceAlpha,
	BF_InverseSourceAlpha,
	BF_DestAlpha,
	BF_InverseDestAlpha,
	BF_DestColor,
	BF_InverseDestColor,
	BF_ConstantBlendFactor,
	BF_InverseConstantBlendFactor,
};

enum EColorWriteMask : uint8 {
	CW_RED = 0x01,
	CW_GREEN = 0x02,
	CW_BLUE = 0x04,
	CW_ALPHA = 0x08,
	CW_NONE = 0,
	CW_RGB = CW_RED | CW_GREEN | CW_BLUE,
	CW_RGBA = CW_RED | CW_GREEN | CW_BLUE | CW_ALPHA,
	CW_RG = CW_RED | CW_GREEN,
	CW_BA = CW_BLUE | CW_ALPHA,
};

enum ERasterizerFillMode : uint8 {
	FM_Point,
	FM_Wireframe,
	FM_Solid,
};

enum ERasterizerCullMode : uint8 {
	CM_None,
	CM_CW,
	CM_CCW,
};

enum EPrimitiveType : uint8 {
	PT_TriangleList,
	PT_TriangleStrip,
	PT_LineList,
	PT_PointList,
	PT_Num,
};

enum ESamplerFilter : uint8 {
	SF_Point,
	SF_Bilinear,
	SF_Trilinear,
	SF_AnisotropicPoint,
	SF_AnisotropicLinear,
};

enum ESamplerAddressMode : uint8 {
	AM_Wrap,
	AM_Clamp,
	AM_Mirror,
	AM_Border,
};

enum ESamplerCompareFunction : uint8 {
	SCF_Never,
	SCF_Less,
};

/** Layout of the indirect dispatch argument buffer (3 uints). */
struct FRHIDispatchIndirectParameters {
	uint32 ThreadGroupCountX = 0;
	uint32 ThreadGroupCountY = 0;
	uint32 ThreadGroupCountZ = 0;
};
using FRHIDispatchIndirectParametersNoPadding = FRHIDispatchIndirectParameters;

struct FRHIDrawIndirectParameters {
	uint32 VertexCountPerInstance = 0;
	uint32 InstanceCount = 0;
	uint32 StartVertexLocation = 0;
	uint32 StartInstanceLocation = 0;
};

struct FRHIDrawIndexedIndirectParameters {
	uint32 IndexCountPerInstance = 0;
	uint32 InstanceCount = 0;
	uint32 StartIndexLocation = 0;
	int32 BaseVertexLocation = 0;
	uint32 StartInstanceLocation = 0;
};

static constexpr uint32 MaxSimultaneousRenderTargets = 8;

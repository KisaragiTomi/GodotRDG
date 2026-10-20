#pragma once

#include "RenderGraphDefinitions.h"
#include "Shader.h"
#include "ShaderParameterMacros.h"

using FRDGTextureDesc = FRHITextureDesc;

/** Descriptor for render graph tracked buffers (same factory functions as UE). */
struct FRDGBufferDesc {
	static FRDGBufferDesc CreateByteAddressDesc(uint32 NumBytes) {
		FRDGBufferDesc Desc;
		Desc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer;
		Desc.BytesPerElement = 4;
		Desc.NumElements = NumBytes / 4;
		return Desc;
	}
	template <typename ParameterStruct>
	static FRDGBufferDesc CreateByteAddressDesc(uint32 NumElements) { return CreateByteAddressDesc((uint32)sizeof(ParameterStruct) * NumElements); }

	static FRDGBufferDesc CreateIndirectDesc(uint32 BytesPerElement, uint32 NumElements) {
		FRDGBufferDesc Desc;
		Desc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::DrawIndirect | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::VertexBuffer;
		Desc.BytesPerElement = BytesPerElement;
		Desc.NumElements = NumElements;
		return Desc;
	}
	static FRDGBufferDesc CreateRawIndirectDesc(uint32 NumBytes) {
		FRDGBufferDesc Desc = CreateByteAddressDesc(NumBytes);
		Desc.Usage |= EBufferUsageFlags::DrawIndirect;
		return Desc;
	}
	template <typename IndirectParameterStruct>
	static FRDGBufferDesc CreateIndirectDesc(uint32 NumElements = 1) { return CreateIndirectDesc((uint32)sizeof(IndirectParameterStruct), NumElements); }
	static FRDGBufferDesc CreateIndirectDesc(uint32 NumElements = 1) { return CreateIndirectDesc(4u, NumElements); }

	static FRDGBufferDesc CreateStructuredDesc(uint32 BytesPerElement, uint32 NumElements) {
		FRDGBufferDesc Desc;
		Desc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer;
		Desc.BytesPerElement = BytesPerElement;
		Desc.NumElements = NumElements;
		return Desc;
	}
	template <typename ParameterStruct>
	static FRDGBufferDesc CreateStructuredDesc(uint32 NumElements) { return CreateStructuredDesc((uint32)sizeof(ParameterStruct), NumElements); }

	static FRDGBufferDesc CreateBufferDesc(uint32 BytesPerElement, uint32 NumElements) {
		FRDGBufferDesc Desc;
		Desc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::VertexBuffer;
		Desc.BytesPerElement = BytesPerElement;
		Desc.NumElements = NumElements;
		return Desc;
	}
	template <typename ParameterStruct>
	static FRDGBufferDesc CreateBufferDesc(uint32 NumElements) { return CreateBufferDesc((uint32)sizeof(ParameterStruct), NumElements); }

	static FRDGBufferDesc CreateUploadDesc(uint32 BytesPerElement, uint32 NumElements) {
		FRDGBufferDesc Desc;
		Desc.Usage = EBufferUsageFlags::ShaderResource | EBufferUsageFlags::VertexBuffer;
		Desc.BytesPerElement = BytesPerElement;
		Desc.NumElements = NumElements;
		return Desc;
	}
	template <typename ParameterStruct>
	static FRDGBufferDesc CreateUploadDesc(uint32 NumElements) { return CreateUploadDesc((uint32)sizeof(ParameterStruct), NumElements); }

	static FRDGBufferDesc CreateStructuredUploadDesc(uint32 BytesPerElement, uint32 NumElements) {
		FRDGBufferDesc Desc;
		Desc.Usage = EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer;
		Desc.BytesPerElement = BytesPerElement;
		Desc.NumElements = NumElements;
		return Desc;
	}
	template <typename ParameterStruct>
	static FRDGBufferDesc CreateStructuredUploadDesc(uint32 NumElements) { return CreateStructuredUploadDesc((uint32)sizeof(ParameterStruct), NumElements); }

	static FRDGBufferDesc CreateByteAddressUploadDesc(uint32 NumBytes) {
		FRDGBufferDesc Desc;
		Desc.Usage = EBufferUsageFlags::ShaderResource | EBufferUsageFlags::ByteAddressBuffer | EBufferUsageFlags::StructuredBuffer;
		Desc.BytesPerElement = 4;
		Desc.NumElements = NumBytes / 4;
		return Desc;
	}

	uint32 GetSize() const { return BytesPerElement * NumElements; }
	bool operator==(const FRDGBufferDesc &Other) const { return BytesPerElement == Other.BytesPerElement && NumElements == Other.NumElements && Usage == Other.Usage; }
	bool operator!=(const FRDGBufferDesc &Other) const { return !(*this == Other); }
	uint32 GetHash() const;

	uint32 BytesPerElement = 1;
	uint32 NumElements = 1;
	EBufferUsageFlags Usage = EBufferUsageFlags::None;
};

struct FRDGTextureSRVDesc {
	FRDGTextureRef Texture = nullptr;
	uint8 MipLevel = 0;
	/** 0 means all mips from MipLevel. */
	uint8 NumMipLevels = 0;
	uint16 FirstArraySlice = 0;
	/** 0 means all slices from FirstArraySlice. */
	uint16 NumArraySlices = 0;
	EPixelFormat Format = PF_Unknown;

	FRDGTextureSRVDesc() = default;
	explicit FRDGTextureSRVDesc(FRDGTextureRef InTexture, uint8 InMipLevel = 0) :
			Texture(InTexture), MipLevel(InMipLevel) {}

	static FRDGTextureSRVDesc Create(FRDGTextureRef Texture) { return FRDGTextureSRVDesc(Texture); }
	static FRDGTextureSRVDesc CreateForMipLevel(FRDGTextureRef Texture, int32 MipLevel) {
		FRDGTextureSRVDesc Desc(Texture, (uint8)MipLevel);
		Desc.NumMipLevels = 1;
		return Desc;
	}
	static FRDGTextureSRVDesc CreateWithPixelFormat(FRDGTextureRef Texture, EPixelFormat PixelFormat) {
		FRDGTextureSRVDesc Desc(Texture);
		Desc.Format = PixelFormat;
		return Desc;
	}
	static FRDGTextureSRVDesc CreateForSlice(FRDGTextureRef Texture, int32 SliceIndex) {
		FRDGTextureSRVDesc Desc(Texture);
		Desc.FirstArraySlice = (uint16)SliceIndex;
		Desc.NumArraySlices = 1;
		return Desc;
	}
};

struct FRDGTextureUAVDesc {
	FRDGTextureRef Texture = nullptr;
	uint8 MipLevel = 0;
	EPixelFormat Format = PF_Unknown;
	uint16 FirstArraySlice = 0;
	uint16 NumArraySlices = 0;

	FRDGTextureUAVDesc() = default;
	explicit FRDGTextureUAVDesc(FRDGTextureRef InTexture, uint8 InMipLevel = 0, EPixelFormat InFormat = PF_Unknown, uint16 InFirstArraySlice = 0, uint16 InNumArraySlices = 0) :
			Texture(InTexture), MipLevel(InMipLevel), Format(InFormat), FirstArraySlice(InFirstArraySlice), NumArraySlices(InNumArraySlices) {}
};

struct FRDGBufferSRVDesc {
	FRDGBufferRef Buffer = nullptr;
	EPixelFormat Format = PF_Unknown;
	FRDGBufferSRVDesc() = default;
	FRDGBufferSRVDesc(FRDGBufferRef InBuffer) :
			Buffer(InBuffer) {}
	FRDGBufferSRVDesc(FRDGBufferRef InBuffer, EPixelFormat InFormat) :
			Buffer(InBuffer), Format(InFormat) {}
};

struct FRDGBufferUAVDesc {
	FRDGBufferRef Buffer = nullptr;
	EPixelFormat Format = PF_Unknown;
	FRDGBufferUAVDesc() = default;
	FRDGBufferUAVDesc(FRDGBufferRef InBuffer) :
			Buffer(InBuffer) {}
	FRDGBufferUAVDesc(FRDGBufferRef InBuffer, EPixelFormat InFormat) :
			Buffer(InBuffer), Format(InFormat) {}
};

/** Pooled render target (UE's FPooledRenderTarget). Owns the RD texture. */
class FPooledRenderTarget final : public IPooledRenderTarget {
public:
	FPooledRenderTarget(FRHITexture *InTexture, bool bInPooled) :
			Texture(InTexture), bPooled(bInPooled) {}

	FRHITexture *GetRHI() const override { return Texture.GetReference(); }
	const FRHITextureDesc &GetDesc() const override { return Texture->GetDesc(); }
	/** Free when only the pool references it. */
	bool IsFree() const override { return bPooled && GetRefCount() == 1; }
	bool IsPooled() const { return bPooled; }

	uint64 LastUsedFrame = 0;
	/** Set while an RDG texture in a graph that is being executed holds this allocation. */
	bool bAllocatedInGraph = false;

private:
	TRefCountPtr<FRHITexture> Texture;
	bool bPooled = false;
};

/** Pooled buffer (UE's FRDGPooledBuffer). */
class FRDGPooledBuffer final : public FRefCountBase {
public:
	FRDGPooledBuffer(FRHIBuffer *InBuffer, const FRDGBufferDesc &InDesc, bool bInPooled) :
			Desc(InDesc), Buffer(InBuffer), bPooled(bInPooled) {}

	FRHIBuffer *GetRHI() const { return Buffer.GetReference(); }
	uint32 GetSize() const { return Desc.GetSize(); }
	uint32 GetAlignedSize() const { return Desc.GetSize(); }
	const char *GetName() const;
	bool IsFree() const { return bPooled && GetRefCount() == 1; }
	bool IsPooled() const { return bPooled; }

	const FRDGBufferDesc Desc;
	uint64 LastUsedFrame = 0;
	bool bAllocatedInGraph = false;

private:
	TRefCountPtr<FRHIBuffer> Buffer;
	bool bPooled = false;
};

/** Base of every resource tracked by the graph. */
class FRDGResource {
public:
	FRDGResource(const char *InName) :
			Name(InName) {}
	FRDGResource(const FRDGResource &) = delete;
	virtual ~FRDGResource() = default;

	const char *const Name;

	/** UE API: marks the resource as used by the current pass (only relevant for unused-parameter warnings). */
	void MarkResourceAsUsed() { bUsedThisPass = true; }

protected:
	/** Validation: the RHI resource can only be accessed while a pass that declares it is executing. */
	bool bAllowRHIAccess = false;
	bool bUsedThisPass = false;

	void ValidateRHIAccess() const;

	friend class FRDGBuilder;
	friend class FRDGUserValidation;
};

class FRDGViewableResource : public FRDGResource {
public:
	const ERDGViewableResourceType Type;

	bool IsExternal() const { return bExternal; }
	bool IsExtracted() const { return bExtracted; }
	bool IsCullRoot() const { return bExternal || bExtracted; }
	bool HasBeenProduced() const { return bProduced; }

	/** UE's access modes. In external access mode (FRDGBuilder::UseExternalAccessMode) the resource keeps one read-only
	 *  access and later passes using it are not tracked by the graph. */
	enum class EAccessMode : uint8 {
		Internal,
		External
	};

protected:
	FRDGViewableResource(const char *InName, ERDGViewableResourceType InType, bool bSkipTracking) :
			FRDGResource(InName), Type(InType) {
		if (bSkipTracking) {
			// UE: SkipTracking locks the resource in external access mode, with every access on every pipe.
			SetExternalAccessMode(ERHIAccess::Mask, ERHIPipeline::All);
			AccessModeState.bLocked = true;
			AccessModeState.ActiveMode = AccessModeState.Mode;
			AccessModeState.ActiveAccess = AccessModeState.Access;
			AccessModeState.ActivePipelines = AccessModeState.Pipelines;
		}
	}

	void SetExternalAccessMode(ERHIAccess InAccess, ERHIPipeline InPipelines) {
		AccessModeState.Mode = EAccessMode::External;
		AccessModeState.Access = InAccess;
		AccessModeState.Pipelines = InPipelines;
	}

	struct FAccessModeState {
		bool IsExternalAccess() const { return ActiveMode == EAccessMode::External; }

		ERHIAccess Access = ERHIAccess::None;
		ERHIPipeline Pipelines = ERHIPipeline::None;
		/** Mode requested by the user (setup timeline). */
		EAccessMode Mode = EAccessMode::Internal;
		bool bLocked = false;
		/** Waiting in the builder's access mode queue for the next pass (FlushAccessModeQueue). */
		bool bQueued = false;
		/** The actual access mode replayed on the pass setup timeline. With the async setup queue the passes are set up on
		 *  a task while the user changes Mode / Access / Pipelines: the setup only reads the Active* values. */
		EAccessMode ActiveMode = EAccessMode::Internal;
		ERHIAccess ActiveAccess = ERHIAccess::None;
		ERHIPipeline ActivePipelines = ERHIPipeline::None;
	} AccessModeState;

	bool bExternal = false;
	bool bExtracted = false;
	bool bProduced = false;
	/** Converted to external with ConvertToExternal*(); allocated up-front and never returned to the pool mid-graph. */
	bool bForceNonTransient = false;
	/** Has an allocation (pooled or external). */
	bool bAllocated = false;
	bool bQueuedForUpload = false;

	/** Compile data. */
	FRDGPass *LastProducer = nullptr;
	/** Last pass of each pipeline that read the resource since its last producer (UE's PassIfReadAccess). */
	FRDGPass *LastReaders[(uint32)ERHIPipeline::Num] = {};
	FRDGPassHandle FirstPass;
	FRDGPassHandle LastPass;
	/** Last pass using the resource on each pipeline (UE's LastPasses), for the transient allocation fences. */
	FRDGPassHandle LastPasses[(uint32)ERHIPipeline::Num];
	uint32 ReferenceCount = 0;
	/** Index into FRDGBuilder registries. */
	uint32 Index = 0;

	friend class FRDGBuilder;
	friend class FRDGUserValidation;
};

class FRDGTexture final : public FRDGViewableResource {
public:
	const FRDGTextureDesc Desc;
	const ERDGTextureFlags Flags;

	/** RHI texture. Only valid inside the lambda of a pass that declares this texture. */
	FRHITexture *GetRHI() const {
		ValidateRHIAccess();
		return RHI;
	}
	/** Godot convenience: the RID of the allocated texture (same validation rules as GetRHI()). */
	RID GetRID() const;
	FRHITexture *GetRHIUnchecked() const { return RHI; }
	IPooledRenderTarget *GetPooledRenderTarget() const { return PooledRenderTarget.GetReference(); }

private:
	FRDGTexture(const char *InName, const FRDGTextureDesc &InDesc, ERDGTextureFlags InFlags) :
			FRDGViewableResource(InName, ERDGViewableResourceType::Texture, EnumHasAnyFlags(InFlags, ERDGTextureFlags::SkipTracking)), Desc(InDesc), Flags(InFlags) {}

	TRefCountPtr<IPooledRenderTarget> PooledRenderTarget;
	FRHITexture *RHI = nullptr;
	/** Set while the texture is placed in a transient heap (r.RDG.TransientAllocator); PooledRenderTarget is null then. */
	class FRHITransientTexture *TransientTexture = nullptr;

	friend class FRDGBuilder;
	friend class FRDGUserValidation;
};

class FRDGBuffer final : public FRDGViewableResource {
public:
	/** Final once the graph compiles: NumElements may come from a callback (FRDGBuilder::CreateBuffer). */
	FRDGBufferDesc Desc;
	const ERDGBufferFlags Flags;

	FRHIBuffer *GetRHI() const {
		ValidateRHIAccess();
		return RHI;
	}
	RID GetRID() const;
	FRHIBuffer *GetRHIUnchecked() const { return RHI; }
	FRHIBuffer *GetIndirectRHICallBuffer() const { return GetRHI(); }
	uint32 GetSize() const { return Desc.GetSize(); }
	uint32 GetStride() const { return Desc.BytesPerElement; }
	FRDGPooledBuffer *GetPooledBuffer() const { return PooledBuffer.GetReference(); }

private:
	FRDGBuffer(const char *InName, const FRDGBufferDesc &InDesc, ERDGBufferFlags InFlags) :
			FRDGViewableResource(InName, ERDGViewableResourceType::Buffer, EnumHasAnyFlags(InFlags, ERDGBufferFlags::SkipTracking)), Desc(InDesc), Flags(InFlags) {}

	TRefCountPtr<FRDGPooledBuffer> PooledBuffer;
	FRHIBuffer *RHI = nullptr;
	class FRHITransientBuffer *TransientBuffer = nullptr;
	/** A pass binds the buffer with ERHIAccess::VertexOrIndexBuffer: it is allocated as an RD vertex / index buffer. */
	bool bVertexOrIndexBinding = false;

	/** Upload queued with QueueBufferUpload (index into the builder's uploaded buffers). */
	int32 UploadIndex = -1;
	/** Optional callback supplying NumElements after the creation of the buffer, resolved by FinalizeDesc. */
	FRDGBufferNumElementsCallback NumElementsCallback;

	void FinalizeDesc() {
		if (!NumElementsCallback) return;
		Desc.NumElements = FMath::Max(NumElementsCallback(), 1u);
		NumElementsCallback = nullptr;
	}

	friend class FRDGBuilder;
	friend class FRDGUserValidation;
};

class FRDGView : public FRDGResource {
public:
	const ERDGViewType Type;
	virtual FRDGViewableResource *GetParent() const = 0;

protected:
	FRDGView(const char *InName, ERDGViewType InType) :
			FRDGResource(InName), Type(InType) {}
};

class FRDGTextureSRV final : public FRDGView {
public:
	const FRDGTextureSRVDesc Desc;

	FRDGTextureRef GetParent() const override { return Desc.Texture; }
	FRHIShaderResourceView *GetRHI() const;
	FRDGTextureSubresourceRange GetSubresourceRange() const;

private:
	FRDGTextureSRV(const char *InName, const FRDGTextureSRVDesc &InDesc) :
			FRDGView(InName, ERDGViewType::TextureSRV), Desc(InDesc) {}
	friend class FRDGBuilder;
};

class FRDGTextureUAV final : public FRDGView {
public:
	const FRDGTextureUAVDesc Desc;
	const ERDGUnorderedAccessViewFlags Flags;

	FRDGTextureRef GetParent() const override { return Desc.Texture; }
	FRHIUnorderedAccessView *GetRHI() const;
	FRDGTextureSubresourceRange GetSubresourceRange() const;

private:
	FRDGTextureUAV(const char *InName, const FRDGTextureUAVDesc &InDesc, ERDGUnorderedAccessViewFlags InFlags) :
			FRDGView(InName, ERDGViewType::TextureUAV), Desc(InDesc), Flags(InFlags) {}
	friend class FRDGBuilder;
};

class FRDGBufferSRV final : public FRDGView {
public:
	const FRDGBufferSRVDesc Desc;

	FRDGBufferRef GetParent() const override { return Desc.Buffer; }
	FRHIShaderResourceView *GetRHI() const;

private:
	FRDGBufferSRV(const char *InName, const FRDGBufferSRVDesc &InDesc) :
			FRDGView(InName, ERDGViewType::BufferSRV), Desc(InDesc) {}
	friend class FRDGBuilder;
};

class FRDGBufferUAV final : public FRDGView {
public:
	const FRDGBufferUAVDesc Desc;
	const ERDGUnorderedAccessViewFlags Flags;

	FRDGBufferRef GetParent() const override { return Desc.Buffer; }
	FRHIUnorderedAccessView *GetRHI() const;

private:
	FRDGBufferUAV(const char *InName, const FRDGBufferUAVDesc &InDesc, ERDGUnorderedAccessViewFlags InFlags) :
			FRDGView(InName, ERDGViewType::BufferUAV), Desc(InDesc), Flags(InFlags) {}
	friend class FRDGBuilder;
};

/** Uniform buffer created with FRDGBuilder::CreateUniformBuffer. Contents are serialized per shader layout at bind time. */
class FRDGUniformBuffer : public FRDGResource {
public:
	const FShaderParametersMetadata *GetParametersMetadata() const { return Metadata; }
	const void *GetContents() const { return Contents; }
	/** Returns an RD uniform buffer containing the contents laid out for the given binding (std140 copy table). */
	RID GetOrCreateRDBuffer(const FShaderParameterBindings::FResourceBinding &Binding);

protected:
	FRDGUniformBuffer(const char *InName, const FShaderParametersMetadata *InMetadata, const void *InContents) :
			FRDGResource(InName), Metadata(InMetadata), Contents(InContents) {}

	const FShaderParametersMetadata *Metadata;
	const void *Contents;
	FRDGBuilder *Builder = nullptr;
	struct FLayoutBuffer {
		const void *LayoutKey = nullptr;
		RID Buffer;
	};
	LocalVector<FLayoutBuffer> LayoutBuffers;

	friend class FRDGBuilder;
};

template <typename TUniformStruct>
class TRDGUniformBuffer final : public FRDGUniformBuffer {
public:
	const TUniformStruct *GetContents() const { return reinterpret_cast<const TUniformStruct *>(Contents); }
	const TUniformStruct *operator->() const { return GetContents(); }

private:
	TRDGUniformBuffer(const char *InName, const TUniformStruct *InContents) :
			FRDGUniformBuffer(InName, TUniformStruct::FTypeInfo::GetStructMetadata(), InContents) {}
	friend class FRDGBuilder;
};

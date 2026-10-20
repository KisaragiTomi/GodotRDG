#pragma once

#include "RDGCore.h"

#include "servers/rendering/rendering_device.h"

/** UE pixel formats that have a RenderingDevice equivalent. */
enum EPixelFormat : uint8 {
	PF_Unknown = 0,
	PF_A32B32G32R32F,
	PF_B8G8R8A8,
	PF_G8,
	PF_G16,
	PF_FloatRGB,
	PF_FloatRGBA,
	PF_DepthStencil,
	PF_ShadowDepth,
	PF_R32_FLOAT,
	PF_G16R16,
	PF_G16R16F,
	PF_G32R32F,
	PF_A2B10G10R10,
	PF_A16B16G16R16,
	PF_D24,
	PF_R16F,
	PF_FloatR11G11B10,
	PF_R8G8B8A8,
	PF_R8G8,
	PF_R32_UINT,
	PF_R32_SINT,
	PF_R16_UINT,
	PF_R16_SINT,
	PF_R8_UINT,
	PF_R8,
	PF_R32G32_UINT,
	PF_R32G32B32A32_UINT,
	PF_R16G16B16A16_UINT,
	PF_R16G16B16A16_SINT,
	PF_R16G16_UINT,
	PF_R8G8B8A8_UINT,
	PF_R32G32B32F,
	PF_R32G32B32_UINT,
	PF_R16G16B16A16_UNORM,
	PF_MAX,
};

struct FPixelFormatInfo {
	const char *Name = "PF_Unknown";
	RenderingDevice::DataFormat RDFormat = RenderingDevice::DATA_FORMAT_MAX;
	RenderingDevice::DataFormat RDFormatSRGB = RenderingDevice::DATA_FORMAT_MAX;
	uint32 BlockBytes = 0;
	uint32 NumComponents = 0;
	bool bIsDepth = false;
	bool bHasStencil = false;
	bool bIsInteger = false;
};

const FPixelFormatInfo &GetPixelFormatInfo(EPixelFormat Format);
inline bool IsDepthOrStencilFormat(EPixelFormat Format) { return GetPixelFormatInfo(Format).bIsDepth || GetPixelFormatInfo(Format).bHasStencil; }
inline bool IsStencilFormat(EPixelFormat Format) { return GetPixelFormatInfo(Format).bHasStencil; }
inline bool IsIntegerFormat(EPixelFormat Format) { return GetPixelFormatInfo(Format).bIsInteger; }

/** Converts back from a RenderingDevice format (used when registering external textures). Returns PF_Unknown when there is no match. */
EPixelFormat GetPixelFormatFromRD(RenderingDevice::DataFormat Format, bool *bOutIsSRGB = nullptr);

/** Picks the depth/stencil format actually supported by the device for PF_DepthStencil. */
RenderingDevice::DataFormat GetRDDataFormat(RenderingDevice *RD, EPixelFormat Format, bool bSRGB = false);

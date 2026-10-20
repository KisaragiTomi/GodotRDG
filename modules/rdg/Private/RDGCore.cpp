#include "RDGCore.h"

#include "HAL/IConsoleManager.h"
#include "PixelFormat.h"
#include "RHIDefinitions.h"

const FLinearColor FLinearColor::White(1.0f, 1.0f, 1.0f, 1.0f);
const FLinearColor FLinearColor::Gray(0.5f, 0.5f, 0.5f, 1.0f);
const FLinearColor FLinearColor::Black(0.0f, 0.0f, 0.0f, 1.0f);
const FLinearColor FLinearColor::Transparent(0.0f, 0.0f, 0.0f, 0.0f);
const FLinearColor FLinearColor::Red(1.0f, 0.0f, 0.0f, 1.0f);
const FLinearColor FLinearColor::Green(0.0f, 1.0f, 0.0f, 1.0f);
const FLinearColor FLinearColor::Blue(0.0f, 0.0f, 1.0f, 1.0f);
const FLinearColor FLinearColor::Yellow(1.0f, 1.0f, 0.0f, 1.0f);

ERHIFeatureLevel::Type GMaxRHIFeatureLevel = ERHIFeatureLevel::SM5;
EShaderPlatform GMaxRHIShaderPlatform = SP_VULKAN_SM5;

namespace RDGPrivate {

String VFormat(const char *Format, va_list Args) {
	char Stack[1024];
	va_list ArgsCopy;
	va_copy(ArgsCopy, Args);
	const int Length = vsnprintf(Stack, sizeof(Stack), Format, ArgsCopy);
	va_end(ArgsCopy);
	if (Length < 0) return String::utf8(Format);
	if (Length < (int)sizeof(Stack)) return String::utf8(Stack, Length);

	LocalVector<char> Heap;
	Heap.resize(Length + 1);
	vsnprintf(Heap.ptr(), Heap.size(), Format, Args);
	return String::utf8(Heap.ptr(), Length);
}

String Format(const char *Format, ...) {
	va_list Args;
	va_start(Args, Format);
	String Result = VFormat(Format, Args);
	va_end(Args);
	return Result;
}

void LogFormatted(ELogVerbosity::Type Verbosity, const char *Format, ...) {
	va_list Args;
	va_start(Args, Format);
	const String Message = VFormat(Format, Args);
	va_end(Args);

	switch (Verbosity) {
		case ELogVerbosity::Fatal:
		case ELogVerbosity::Error:
			ERR_PRINT("[RDG] " + Message);
			break;
		case ELogVerbosity::Warning:
			WARN_PRINT("[RDG] " + Message);
			break;
		case ELogVerbosity::Display:
		case ELogVerbosity::Log:
			print_line("[RDG] " + Message);
			break;
		default:
			print_verbose("[RDG] " + Message);
			break;
	}
}

} //namespace RDGPrivate

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Console variables.

IConsoleManager &IConsoleManager::Get() {
	static IConsoleManager Manager;
	return Manager;
}

IConsoleVariable *IConsoleManager::FindConsoleVariable(const char *Name) const {
	const String Search = String::utf8(Name).to_lower();
	for (IConsoleVariable *Variable : Variables) {
		if (String::utf8(Variable->GetName()).to_lower() == Search) return Variable;
	}
	return nullptr;
}

void IConsoleManager::Register(IConsoleVariable *Variable) {
	Variables.push_back(Variable);
}

void IConsoleManager::Unregister(IConsoleVariable *Variable) {
	Variables.erase(Variable);
}

int32 IConsoleManager::ApplyCommandLine(const List<String> &Arguments) {
	int32 NumApplied = 0;
	for (const String &Argument : Arguments) {
		String Trimmed = Argument.strip_edges();
		while (Trimmed.begins_with("-")) Trimmed = Trimmed.substr(1);
		const int32 Separator = Trimmed.find("=");
		if (Separator <= 0) continue;
		const String Name = Trimmed.substr(0, Separator);
		const String Value = Trimmed.substr(Separator + 1);
		IConsoleVariable *Variable = FindConsoleVariable(Name.utf8().get_data());
		if (!Variable) continue;
		Variable->Set(Value.utf8().get_data());
		print_line(vformat("[RDG] %s = %s (command line)", Variable->GetName(), Variable->GetString()));
		NumApplied++;
	}
	return NumApplied;
}

FAutoConsoleVariableRef::FAutoConsoleVariableRef(const char *InName, int32 &InRef, const char *InHelp, uint32 InFlags) :
		IConsoleVariable(InName, InHelp, InFlags), IntRef(&InRef) {
	IConsoleManager::Get().Register(this);
}

FAutoConsoleVariableRef::FAutoConsoleVariableRef(const char *InName, float &InRef, const char *InHelp, uint32 InFlags) :
		IConsoleVariable(InName, InHelp, InFlags), FloatRef(&InRef) {
	IConsoleManager::Get().Register(this);
}

FAutoConsoleVariableRef::FAutoConsoleVariableRef(const char *InName, bool &InRef, const char *InHelp, uint32 InFlags) :
		IConsoleVariable(InName, InHelp, InFlags), BoolRef(&InRef) {
	IConsoleManager::Get().Register(this);
}

FAutoConsoleVariableRef::~FAutoConsoleVariableRef() {
	IConsoleManager::Get().Unregister(this);
}

int32 FAutoConsoleVariableRef::GetInt() const {
	if (IntRef) return *IntRef;
	if (FloatRef) return (int32)*FloatRef;
	return *BoolRef ? 1 : 0;
}

float FAutoConsoleVariableRef::GetFloat() const {
	if (FloatRef) return *FloatRef;
	return (float)GetInt();
}

String FAutoConsoleVariableRef::GetString() const {
	if (FloatRef) return String::num(*FloatRef);
	return itos(GetInt());
}

void FAutoConsoleVariableRef::Set(const char *InValue) {
	const String S = String::utf8(InValue);
	if (FloatRef) *FloatRef = (float)S.to_float();
	else Set((int32)S.to_int());
}

void FAutoConsoleVariableRef::Set(int32 InValue) {
	if (IntRef) *IntRef = InValue;
	else if (FloatRef) *FloatRef = (float)InValue;
	else *BoolRef = InValue != 0;
}

void FAutoConsoleVariableRef::Set(float InValue) {
	if (FloatRef) *FloatRef = InValue;
	else Set((int32)InValue);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// RHI definitions.

const FClearValueBinding FClearValueBinding::None(EClearBinding::ENoneBound);
const FClearValueBinding FClearValueBinding::Black(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
const FClearValueBinding FClearValueBinding::BlackMaxAlpha(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
const FClearValueBinding FClearValueBinding::White(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
const FClearValueBinding FClearValueBinding::Transparent(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
const FClearValueBinding FClearValueBinding::DepthOne(1.0f, 0);
const FClearValueBinding FClearValueBinding::DepthZero(0.0f, 0);
const FClearValueBinding FClearValueBinding::DepthFar(0.0f, 0);
const FClearValueBinding FClearValueBinding::DepthNear(1.0f, 0);
const FClearValueBinding FClearValueBinding::Green(FLinearColor(0.0f, 1.0f, 0.0f, 1.0f));
const FClearValueBinding FClearValueBinding::DefaultNormal8Bit(FLinearColor(128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1.0f));

bool FClearValueBinding::operator==(const FClearValueBinding &O) const {
	if (ColorBinding != O.ColorBinding) return false;
	if (ColorBinding == EClearBinding::EColorBound) return Color[0] == O.Color[0] && Color[1] == O.Color[1] && Color[2] == O.Color[2] && Color[3] == O.Color[3];
	if (ColorBinding == EClearBinding::EDepthStencilBound) return Depth == O.Depth && Stencil == O.Stencil;
	return true;
}

const char *GetRHIAccessName(ERHIAccess Access) {
	static thread_local CharString Result;
	if (Access == ERHIAccess::Unknown) return "Unknown";
	struct FEntry {
		ERHIAccess Access;
		const char *Name;
	};
	static const FEntry Entries[] = {
		{ ERHIAccess::CPURead, "CPURead" },
		{ ERHIAccess::Present, "Present" },
		{ ERHIAccess::IndirectArgs, "IndirectArgs" },
		{ ERHIAccess::VertexOrIndexBuffer, "VertexOrIndexBuffer" },
		{ ERHIAccess::SRVCompute, "SRVCompute" },
		{ ERHIAccess::SRVGraphics, "SRVGraphics" },
		{ ERHIAccess::CopySrc, "CopySrc" },
		{ ERHIAccess::ResolveSrc, "ResolveSrc" },
		{ ERHIAccess::DSVRead, "DSVRead" },
		{ ERHIAccess::UAVCompute, "UAVCompute" },
		{ ERHIAccess::UAVGraphics, "UAVGraphics" },
		{ ERHIAccess::RTV, "RTV" },
		{ ERHIAccess::CopyDest, "CopyDest" },
		{ ERHIAccess::ResolveDst, "ResolveDst" },
		{ ERHIAccess::DSVWrite, "DSVWrite" },
		{ ERHIAccess::ShadingRateSource, "ShadingRateSource" },
	};
	String Names;
	for (const FEntry &Entry : Entries) {
		if (!EnumHasAnyFlags(Access, Entry.Access)) continue;
		if (!Names.is_empty()) Names += "|";
		Names += Entry.Name;
	}
	Result = Names.utf8();
	return Result.get_data();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Pixel formats.

namespace {
using RDF = RenderingDevice::DataFormat;

struct FPixelFormatTable {
	FPixelFormatInfo Infos[PF_MAX];

	void Set(EPixelFormat Format, const char *Name, RDF RDFormat, uint32 BlockBytes, uint32 NumComponents, RDF RDFormatSRGB = RenderingDevice::DATA_FORMAT_MAX, bool bIsInteger = false) {
		FPixelFormatInfo &Info = Infos[Format];
		Info.Name = Name;
		Info.RDFormat = RDFormat;
		Info.RDFormatSRGB = RDFormatSRGB;
		Info.BlockBytes = BlockBytes;
		Info.NumComponents = NumComponents;
		Info.bIsInteger = bIsInteger;
	}

	FPixelFormatTable() {
		Set(PF_Unknown, "PF_Unknown", RenderingDevice::DATA_FORMAT_MAX, 0, 0);
		Set(PF_A32B32G32R32F, "PF_A32B32G32R32F", RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, 16, 4);
		Set(PF_B8G8R8A8, "PF_B8G8R8A8", RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM, 4, 4, RenderingDevice::DATA_FORMAT_B8G8R8A8_SRGB);
		Set(PF_G8, "PF_G8", RenderingDevice::DATA_FORMAT_R8_UNORM, 1, 1);
		Set(PF_G16, "PF_G16", RenderingDevice::DATA_FORMAT_R16_UNORM, 2, 1);
		Set(PF_FloatRGB, "PF_FloatRGB", RenderingDevice::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 3);
		Set(PF_FloatRGBA, "PF_FloatRGBA", RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, 8, 4);
		Set(PF_DepthStencil, "PF_DepthStencil", RenderingDevice::DATA_FORMAT_D32_SFLOAT_S8_UINT, 8, 2);
		Infos[PF_DepthStencil].bIsDepth = true;
		Infos[PF_DepthStencil].bHasStencil = true;
		Set(PF_ShadowDepth, "PF_ShadowDepth", RenderingDevice::DATA_FORMAT_D32_SFLOAT, 4, 1);
		Infos[PF_ShadowDepth].bIsDepth = true;
		Set(PF_R32_FLOAT, "PF_R32_FLOAT", RenderingDevice::DATA_FORMAT_R32_SFLOAT, 4, 1);
		Set(PF_G16R16, "PF_G16R16", RenderingDevice::DATA_FORMAT_R16G16_UNORM, 4, 2);
		Set(PF_G16R16F, "PF_G16R16F", RenderingDevice::DATA_FORMAT_R16G16_SFLOAT, 4, 2);
		Set(PF_G32R32F, "PF_G32R32F", RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, 8, 2);
		Set(PF_A2B10G10R10, "PF_A2B10G10R10", RenderingDevice::DATA_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 4);
		Set(PF_A16B16G16R16, "PF_A16B16G16R16", RenderingDevice::DATA_FORMAT_R16G16B16A16_UNORM, 8, 4);
		Set(PF_D24, "PF_D24", RenderingDevice::DATA_FORMAT_X8_D24_UNORM_PACK32, 4, 1);
		Infos[PF_D24].bIsDepth = true;
		Set(PF_R16F, "PF_R16F", RenderingDevice::DATA_FORMAT_R16_SFLOAT, 2, 1);
		Set(PF_FloatR11G11B10, "PF_FloatR11G11B10", RenderingDevice::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 3);
		Set(PF_R8G8B8A8, "PF_R8G8B8A8", RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, 4, 4, RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB);
		Set(PF_R8G8, "PF_R8G8", RenderingDevice::DATA_FORMAT_R8G8_UNORM, 2, 2);
		Set(PF_R32_UINT, "PF_R32_UINT", RenderingDevice::DATA_FORMAT_R32_UINT, 4, 1, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R32_SINT, "PF_R32_SINT", RenderingDevice::DATA_FORMAT_R32_SINT, 4, 1, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R16_UINT, "PF_R16_UINT", RenderingDevice::DATA_FORMAT_R16_UINT, 2, 1, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R16_SINT, "PF_R16_SINT", RenderingDevice::DATA_FORMAT_R16_SINT, 2, 1, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R8_UINT, "PF_R8_UINT", RenderingDevice::DATA_FORMAT_R8_UINT, 1, 1, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R8, "PF_R8", RenderingDevice::DATA_FORMAT_R8_UNORM, 1, 1);
		Set(PF_R32G32_UINT, "PF_R32G32_UINT", RenderingDevice::DATA_FORMAT_R32G32_UINT, 8, 2, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R32G32B32A32_UINT, "PF_R32G32B32A32_UINT", RenderingDevice::DATA_FORMAT_R32G32B32A32_UINT, 16, 4, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R16G16B16A16_UINT, "PF_R16G16B16A16_UINT", RenderingDevice::DATA_FORMAT_R16G16B16A16_UINT, 8, 4, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R16G16B16A16_SINT, "PF_R16G16B16A16_SINT", RenderingDevice::DATA_FORMAT_R16G16B16A16_SINT, 8, 4, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R16G16_UINT, "PF_R16G16_UINT", RenderingDevice::DATA_FORMAT_R16G16_UINT, 4, 2, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R8G8B8A8_UINT, "PF_R8G8B8A8_UINT", RenderingDevice::DATA_FORMAT_R8G8B8A8_UINT, 4, 4, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R32G32B32F, "PF_R32G32B32F", RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 12, 3);
		Set(PF_R32G32B32_UINT, "PF_R32G32B32_UINT", RenderingDevice::DATA_FORMAT_R32G32B32_UINT, 12, 3, RenderingDevice::DATA_FORMAT_MAX, true);
		Set(PF_R16G16B16A16_UNORM, "PF_R16G16B16A16_UNORM", RenderingDevice::DATA_FORMAT_R16G16B16A16_UNORM, 8, 4);
	}
};

const FPixelFormatTable &GetPixelFormatTable() {
	static FPixelFormatTable Table;
	return Table;
}
} //namespace

const FPixelFormatInfo &GetPixelFormatInfo(EPixelFormat Format) {
	const FPixelFormatTable &Table = GetPixelFormatTable();
	if (Format >= PF_MAX) return Table.Infos[PF_Unknown];
	return Table.Infos[Format];
}

EPixelFormat GetPixelFormatFromRD(RenderingDevice::DataFormat Format, bool *bOutIsSRGB) {
	if (bOutIsSRGB) *bOutIsSRGB = false;
	const FPixelFormatTable &Table = GetPixelFormatTable();
	for (int32 Index = 1; Index < PF_MAX; Index++) {
		const FPixelFormatInfo &Info = Table.Infos[Index];
		if (Info.RDFormat == Format) return (EPixelFormat)Index;
		if (Info.RDFormatSRGB == Format) {
			if (bOutIsSRGB) *bOutIsSRGB = true;
			return (EPixelFormat)Index;
		}
	}
	if (Format == RenderingDevice::DATA_FORMAT_D24_UNORM_S8_UINT) return PF_DepthStencil;
	if (Format == RenderingDevice::DATA_FORMAT_D16_UNORM) return PF_ShadowDepth;
	return PF_Unknown;
}

RenderingDevice::DataFormat GetRDDataFormat(RenderingDevice *RD, EPixelFormat Format, bool bSRGB) {
	const FPixelFormatInfo &Info = GetPixelFormatInfo(Format);
	if (bSRGB && Info.RDFormatSRGB != RenderingDevice::DATA_FORMAT_MAX) return Info.RDFormatSRGB;
	if (Format == PF_DepthStencil && RD) {
		// Prefer D32S8, fall back to D24S8 like UE does per platform.
		if (!RD->texture_is_format_supported_for_usage(RenderingDevice::DATA_FORMAT_D32_SFLOAT_S8_UINT, RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return RenderingDevice::DATA_FORMAT_D24_UNORM_S8_UINT;
	}
	return Info.RDFormat;
}

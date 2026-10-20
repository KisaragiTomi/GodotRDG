#pragma once

// Minimal UE-style core vocabulary (integer typedefs, math structs, enum flag helpers, TRefCountPtr, UE_LOG)
// so RDG code can be written the same way it is written against UE's RenderCore.

#include "core/error/error_macros.h"
#include "core/math/color.h"
#include "core/math/math_funcs.h"
#include "core/math/vector2i.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/typedefs.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <new>
#include <shared_mutex>
#include <type_traits>
#include <utility>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

/** UE's FRWLock / FReadScopeLock / FWriteScopeLock, for the caches read by passes recorded in parallel. Built on
 *  std::shared_mutex (an SRW lock on Windows): Godot's RWLock wraps std::shared_timed_mutex, whose shared locks take an
 *  internal exclusive mutex with MSVC and serialize the readers. */
using FRWLock = std::shared_mutex;
using FReadScopeLock = std::shared_lock<std::shared_mutex>;
using FWriteScopeLock = std::unique_lock<std::shared_mutex>;

#ifndef TEXT
#define TEXT(x) x
#endif

#ifndef INDEX_NONE
#define INDEX_NONE (-1)
#endif

#define RDG_STRINGIFY_INNER(x) #x
#define RDG_STRINGIFY(x) RDG_STRINGIFY_INNER(x)

/** Same as UE's ENUM_CLASS_FLAGS: bitwise operators for enum classes. */
#define ENUM_CLASS_FLAGS(Enum)                                                                                                                                                                 \
	inline constexpr Enum &operator|=(Enum &Lhs, Enum Rhs) { return Lhs = (Enum)((std::underlying_type_t<Enum>)Lhs | (std::underlying_type_t<Enum>)Rhs); }                               \
	inline constexpr Enum &operator&=(Enum &Lhs, Enum Rhs) { return Lhs = (Enum)((std::underlying_type_t<Enum>)Lhs & (std::underlying_type_t<Enum>)Rhs); }                               \
	inline constexpr Enum &operator^=(Enum &Lhs, Enum Rhs) { return Lhs = (Enum)((std::underlying_type_t<Enum>)Lhs ^ (std::underlying_type_t<Enum>)Rhs); }                               \
	inline constexpr Enum operator|(Enum Lhs, Enum Rhs) { return (Enum)((std::underlying_type_t<Enum>)Lhs | (std::underlying_type_t<Enum>)Rhs); }                                          \
	inline constexpr Enum operator&(Enum Lhs, Enum Rhs) { return (Enum)((std::underlying_type_t<Enum>)Lhs & (std::underlying_type_t<Enum>)Rhs); }                                          \
	inline constexpr Enum operator^(Enum Lhs, Enum Rhs) { return (Enum)((std::underlying_type_t<Enum>)Lhs ^ (std::underlying_type_t<Enum>)Rhs); }                                          \
	inline constexpr bool operator!(Enum E) { return !(std::underlying_type_t<Enum>)E; }                                                                                                    \
	inline constexpr Enum operator~(Enum E) { return (Enum) ~(std::underlying_type_t<Enum>)E; }

template <typename Enum>
constexpr bool EnumHasAnyFlags(Enum Flags, Enum Contains) {
	return (((std::underlying_type_t<Enum>)Flags) & (std::underlying_type_t<Enum>)Contains) != 0;
}

template <typename Enum>
constexpr bool EnumHasAllFlags(Enum Flags, Enum Contains) {
	return (((std::underlying_type_t<Enum>)Flags) & (std::underlying_type_t<Enum>)Contains) == ((std::underlying_type_t<Enum>)Contains);
}

template <typename Enum>
void EnumAddFlags(Enum &Flags, Enum FlagsToAdd) {
	Flags = (Enum)((std::underlying_type_t<Enum>)Flags | (std::underlying_type_t<Enum>)FlagsToAdd);
}

template <typename Enum>
void EnumRemoveFlags(Enum &Flags, Enum FlagsToRemove) {
	Flags = (Enum)((std::underlying_type_t<Enum>)Flags & ~(std::underlying_type_t<Enum>)FlagsToRemove);
}

/** Subset of UE's FMath used by RDG utilities. */
struct FMath {
	template <typename T>
	static constexpr T DivideAndRoundUp(T Dividend, T Divisor) { return (Dividend + Divisor - 1) / Divisor; }
	template <typename T>
	static constexpr T DivideAndRoundDown(T Dividend, T Divisor) { return Dividend / Divisor; }
	template <typename T>
	static constexpr T Max(T A, T B) { return A > B ? A : B; }
	template <typename T>
	static constexpr T Min(T A, T B) { return A < B ? A : B; }
	template <typename T>
	static constexpr T Clamp(T X, T MinValue, T MaxValue) { return X < MinValue ? MinValue : (X > MaxValue ? MaxValue : X); }
	template <typename T>
	static constexpr bool IsPowerOfTwo(T Value) { return Value > 0 && (Value & (Value - 1)) == 0; }
	static uint32 FloorLog2(uint32 Value) {
		uint32 Result = 0;
		while (Value >>= 1) Result++;
		return Result;
	}
	static uint32 CeilLogTwo(uint32 Value) {
		const uint32 Floor = FloorLog2(Value);
		return IsPowerOfTwo(Value) || Value == 0 ? Floor : Floor + 1;
	}
	static uint32 RoundUpToPowerOfTwo(uint32 Value) { return 1u << CeilLogTwo(Value); }
};

struct FIntPoint {
	int32 X = 0;
	int32 Y = 0;

	constexpr FIntPoint() = default;
	constexpr FIntPoint(int32 InX, int32 InY) :
			X(InX), Y(InY) {}
	explicit constexpr FIntPoint(int32 InXY) :
			X(InXY), Y(InXY) {}
	FIntPoint(const Vector2i &V) :
			X(V.x), Y(V.y) {}

	static constexpr FIntPoint ZeroValue() { return FIntPoint(0, 0); }
	static FIntPoint DivideAndRoundUp(FIntPoint Lhs, int32 Divisor) { return FIntPoint(FMath::DivideAndRoundUp(Lhs.X, Divisor), FMath::DivideAndRoundUp(Lhs.Y, Divisor)); }
	static FIntPoint DivideAndRoundUp(FIntPoint Lhs, FIntPoint Divisor) { return FIntPoint(FMath::DivideAndRoundUp(Lhs.X, Divisor.X), FMath::DivideAndRoundUp(Lhs.Y, Divisor.Y)); }
	static FIntPoint DivideAndRoundDown(FIntPoint Lhs, int32 Divisor) { return FIntPoint(Lhs.X / Divisor, Lhs.Y / Divisor); }
	static FIntPoint ComponentMax(FIntPoint A, FIntPoint B) { return FIntPoint(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y)); }

	constexpr bool operator==(const FIntPoint &Other) const { return X == Other.X && Y == Other.Y; }
	constexpr bool operator!=(const FIntPoint &Other) const { return !(*this == Other); }
	constexpr FIntPoint operator+(const FIntPoint &Other) const { return FIntPoint(X + Other.X, Y + Other.Y); }
	constexpr FIntPoint operator-(const FIntPoint &Other) const { return FIntPoint(X - Other.X, Y - Other.Y); }
	constexpr FIntPoint operator*(int32 Scale) const { return FIntPoint(X * Scale, Y * Scale); }
	constexpr FIntPoint operator/(int32 Divisor) const { return FIntPoint(X / Divisor, Y / Divisor); }
	operator Vector2i() const { return Vector2i(X, Y); }
	int32 Size() const { return (int32)Math::sqrt((float)(X * X + Y * Y)); }
};

struct FIntVector {
	int32 X = 0;
	int32 Y = 0;
	int32 Z = 0;

	constexpr FIntVector() = default;
	constexpr FIntVector(int32 InX, int32 InY, int32 InZ) :
			X(InX), Y(InY), Z(InZ) {}
	explicit constexpr FIntVector(int32 InXYZ) :
			X(InXYZ), Y(InXYZ), Z(InXYZ) {}

	static FIntVector DivideAndRoundUp(FIntVector Lhs, int32 Divisor) { return FIntVector(FMath::DivideAndRoundUp(Lhs.X, Divisor), FMath::DivideAndRoundUp(Lhs.Y, Divisor), FMath::DivideAndRoundUp(Lhs.Z, Divisor)); }
	static FIntVector DivideAndRoundUp(FIntVector Lhs, FIntVector Divisor) { return FIntVector(FMath::DivideAndRoundUp(Lhs.X, Divisor.X), FMath::DivideAndRoundUp(Lhs.Y, Divisor.Y), FMath::DivideAndRoundUp(Lhs.Z, Divisor.Z)); }

	constexpr bool operator==(const FIntVector &Other) const { return X == Other.X && Y == Other.Y && Z == Other.Z; }
	constexpr bool operator!=(const FIntVector &Other) const { return !(*this == Other); }
};

struct FIntVector4 {
	int32 X = 0, Y = 0, Z = 0, W = 0;
	constexpr FIntVector4() = default;
	constexpr FIntVector4(int32 InX, int32 InY, int32 InZ, int32 InW) :
			X(InX), Y(InY), Z(InZ), W(InW) {}
};

struct FUintVector2 {
	uint32 X = 0, Y = 0;
	constexpr FUintVector2() = default;
	constexpr FUintVector2(uint32 InX, uint32 InY) :
			X(InX), Y(InY) {}
};

struct FUintVector3 {
	uint32 X = 0, Y = 0, Z = 0;
	constexpr FUintVector3() = default;
	constexpr FUintVector3(uint32 InX, uint32 InY, uint32 InZ) :
			X(InX), Y(InY), Z(InZ) {}
};

struct FUintVector4 {
	uint32 X = 0, Y = 0, Z = 0, W = 0;
	constexpr FUintVector4() = default;
	constexpr FUintVector4(uint32 InX, uint32 InY, uint32 InZ, uint32 InW) :
			X(InX), Y(InY), Z(InZ), W(InW) {}
	explicit constexpr FUintVector4(uint32 V) :
			X(V), Y(V), Z(V), W(V) {}
};

struct FVector2f {
	float X = 0.0f, Y = 0.0f;
	constexpr FVector2f() = default;
	constexpr FVector2f(float InX, float InY) :
			X(InX), Y(InY) {}
	explicit constexpr FVector2f(float V) :
			X(V), Y(V) {}
	explicit FVector2f(const FIntPoint &P) :
			X((float)P.X), Y((float)P.Y) {}
	constexpr FVector2f operator*(float S) const { return FVector2f(X * S, Y * S); }
	constexpr FVector2f operator/(float S) const { return FVector2f(X / S, Y / S); }
};

struct FVector3f {
	float X = 0.0f, Y = 0.0f, Z = 0.0f;
	constexpr FVector3f() = default;
	constexpr FVector3f(float InX, float InY, float InZ) :
			X(InX), Y(InY), Z(InZ) {}
	explicit constexpr FVector3f(float V) :
			X(V), Y(V), Z(V) {}
};

struct FVector4f {
	float X = 0.0f, Y = 0.0f, Z = 0.0f, W = 0.0f;
	constexpr FVector4f() = default;
	constexpr FVector4f(float InX, float InY, float InZ, float InW) :
			X(InX), Y(InY), Z(InZ), W(InW) {}
	explicit constexpr FVector4f(float V) :
			X(V), Y(V), Z(V), W(V) {}
};

struct FLinearColor {
	float R = 0.0f, G = 0.0f, B = 0.0f, A = 0.0f;
	constexpr FLinearColor() = default;
	constexpr FLinearColor(float InR, float InG, float InB, float InA = 1.0f) :
			R(InR), G(InG), B(InB), A(InA) {}
	FLinearColor(const Color &C) :
			R(C.r), G(C.g), B(C.b), A(C.a) {}
	operator Color() const { return Color(R, G, B, A); }
	bool operator==(const FLinearColor &O) const { return R == O.R && G == O.G && B == O.B && A == O.A; }
	bool operator!=(const FLinearColor &O) const { return !(*this == O); }

	static const FLinearColor White;
	static const FLinearColor Gray;
	static const FLinearColor Black;
	static const FLinearColor Transparent;
	static const FLinearColor Red;
	static const FLinearColor Green;
	static const FLinearColor Blue;
	static const FLinearColor Yellow;
};

/** Plain 4x4 float matrix; uploaded as-is (16 floats). Declare it `layout(row_major)` in GLSL to read it like UE's HLSL. */
struct FMatrix44f {
	float M[4][4] = {};
	static FMatrix44f Identity() {
		FMatrix44f Result;
		for (int32 i = 0; i < 4; i++) Result.M[i][i] = 1.0f;
		return Result;
	}
};

struct FIntRect {
	FIntPoint Min;
	FIntPoint Max;
	constexpr FIntRect() = default;
	constexpr FIntRect(FIntPoint InMin, FIntPoint InMax) :
			Min(InMin), Max(InMax) {}
	constexpr FIntRect(int32 X0, int32 Y0, int32 X1, int32 Y1) :
			Min(X0, Y0), Max(X1, Y1) {}
	constexpr int32 Width() const { return Max.X - Min.X; }
	constexpr int32 Height() const { return Max.Y - Min.Y; }
	constexpr FIntPoint Size() const { return FIntPoint(Width(), Height()); }
	constexpr bool IsEmpty() const { return Width() <= 0 || Height() <= 0; }
	constexpr bool operator==(const FIntRect &O) const { return Min == O.Min && Max == O.Max; }
	constexpr bool operator!=(const FIntRect &O) const { return !(*this == O); }
};

/** Intrusive reference counting base, equivalent to UE's FRefCountBase. */
class FRefCountBase {
public:
	FRefCountBase() = default;
	FRefCountBase(const FRefCountBase &) = delete;
	FRefCountBase &operator=(const FRefCountBase &) = delete;
	virtual ~FRefCountBase() = default;

	uint32 AddRef() const { return ++NumRefs; }
	uint32 Release() const {
		const uint32 Refs = --NumRefs;
		if (Refs == 0) delete this;
		return Refs;
	}
	uint32 GetRefCount() const { return NumRefs.load(); }

private:
	mutable std::atomic<uint32> NumRefs{ 0 };
};

/** Same semantics as UE's TRefCountPtr. */
template <typename ReferencedType>
class TRefCountPtr {
public:
	TRefCountPtr() = default;
	TRefCountPtr(std::nullptr_t) {}
	TRefCountPtr(ReferencedType *InReference, bool bAddRef = true) :
			Reference(InReference) {
		if (Reference && bAddRef) Reference->AddRef();
	}
	TRefCountPtr(const TRefCountPtr &Copy) :
			Reference(Copy.Reference) {
		if (Reference) Reference->AddRef();
	}
	template <typename OtherType, typename = std::enable_if_t<std::is_convertible_v<OtherType *, ReferencedType *>>>
	TRefCountPtr(const TRefCountPtr<OtherType> &Copy) :
			Reference(Copy.GetReference()) {
		if (Reference) Reference->AddRef();
	}
	TRefCountPtr(TRefCountPtr &&Move) :
			Reference(Move.Reference) { Move.Reference = nullptr; }
	~TRefCountPtr() {
		if (Reference) Reference->Release();
	}

	TRefCountPtr &operator=(ReferencedType *InReference) {
		if (Reference != InReference) {
			ReferencedType *OldReference = Reference;
			Reference = InReference;
			if (Reference) Reference->AddRef();
			if (OldReference) OldReference->Release();
		}
		return *this;
	}
	TRefCountPtr &operator=(const TRefCountPtr &InPtr) { return *this = InPtr.Reference; }
	TRefCountPtr &operator=(TRefCountPtr &&InPtr) {
		if (this != &InPtr) {
			ReferencedType *OldReference = Reference;
			Reference = InPtr.Reference;
			InPtr.Reference = nullptr;
			if (OldReference) OldReference->Release();
		}
		return *this;
	}
	TRefCountPtr &operator=(std::nullptr_t) {
		SafeRelease();
		return *this;
	}

	ReferencedType *operator->() const { return Reference; }
	ReferencedType &operator*() const { return *Reference; }
	operator ReferencedType *() const { return Reference; }
	ReferencedType *GetReference() const { return Reference; }
	bool IsValid() const { return Reference != nullptr; }
	void SafeRelease() { *this = (ReferencedType *)nullptr; }
	uint32 GetRefCount() const { return Reference ? Reference->GetRefCount() : 0; }

private:
	ReferencedType *Reference = nullptr;
};

template <typename T>
bool operator==(const TRefCountPtr<T> &A, const TRefCountPtr<T> &B) { return A.GetReference() == B.GetReference(); }
template <typename T>
bool operator!=(const TRefCountPtr<T> &A, const TRefCountPtr<T> &B) { return A.GetReference() != B.GetReference(); }

/** UE_LOG replacement. The category is accepted for source compatibility and ignored. */
namespace ELogVerbosity {
enum Type : uint8 {
	Fatal,
	Error,
	Warning,
	Display,
	Log,
	Verbose,
	VeryVerbose,
};
}

namespace RDGPrivate {
void LogFormatted(ELogVerbosity::Type Verbosity, const char *Format, ...);
String VFormat(const char *Format, va_list Args);
String Format(const char *Format, ...);
} //namespace RDGPrivate

#define UE_LOG(CategoryName, Verbosity, Format, ...) ::RDGPrivate::LogFormatted(ELogVerbosity::Verbosity, Format, ##__VA_ARGS__)
#define DECLARE_LOG_CATEGORY_EXTERN(...)
#define DEFINE_LOG_CATEGORY(...)
#define DEFINE_LOG_CATEGORY_STATIC(...)

/** Internal assertion: reports through Godot's error macros instead of crashing like UE's check(). */
#define RDG_ENSURE_MSG(Condition, Format, ...) \
	((Condition) ? true : (::RDGPrivate::LogFormatted(ELogVerbosity::Error, Format, ##__VA_ARGS__), false))

#pragma once

// UE's shader permutation domains:
//
//	class FUseBloom : SHADER_PERMUTATION_BOOL("USE_BLOOM");
//	class FQuality  : SHADER_PERMUTATION_INT("QUALITY", 3);
//	using FPermutationDomain = TShaderPermutationDomain<FUseBloom, FQuality>;

#include "ShaderCore.h"

struct FShaderPermutationBool {
	using Type = bool;
	static constexpr int32 PermutationCount = 2;
	static constexpr bool IsMultiDimensional = false;
	static int32 ToDimensionValueId(Type E) { return E ? 1 : 0; }
	static bool ToDefineValue(Type E) { return E; }
	static Type FromDimensionValueId(int32 PermutationId) { return PermutationId == 1; }
};

template <int32 TDimensionSize, int32 TFirstValue = 0>
struct TShaderPermutationInt {
	using Type = int32;
	static constexpr int32 PermutationCount = TDimensionSize;
	static constexpr bool IsMultiDimensional = false;
	static constexpr Type MinValue = static_cast<Type>(TFirstValue);
	static constexpr Type MaxValue = static_cast<Type>(TFirstValue + TDimensionSize - 1);
	static int32 ToDimensionValueId(Type E) { return static_cast<int32>(E) - TFirstValue; }
	static int32 ToDefineValue(Type E) { return static_cast<int32>(E); }
	static Type FromDimensionValueId(int32 PermutationId) { return static_cast<Type>(PermutationId + TFirstValue); }
};

template <typename TEnum, int32 TDimensionSize>
struct TShaderPermutationEnumClass {
	using Type = TEnum;
	static constexpr int32 PermutationCount = TDimensionSize;
	static constexpr bool IsMultiDimensional = false;
	static int32 ToDimensionValueId(Type E) { return static_cast<int32>(E); }
	static int32 ToDefineValue(Type E) { return static_cast<int32>(E); }
	static Type FromDimensionValueId(int32 PermutationId) { return static_cast<Type>(PermutationId); }
};

template <int32... Ts>
struct TShaderPermutationSparseInt {
	using Type = int32;
	static constexpr int32 PermutationCount = (int32)sizeof...(Ts);
	static constexpr bool IsMultiDimensional = false;
	static int32 ToDimensionValueId(Type E) {
		const int32 Values[] = { Ts... };
		for (int32 Index = 0; Index < PermutationCount; Index++) {
			if (Values[Index] == E) return Index;
		}
		return 0;
	}
	static int32 ToDefineValue(Type E) { return E; }
	static Type FromDimensionValueId(int32 PermutationId) {
		const int32 Values[] = { Ts... };
		return Values[PermutationId];
	}
};

#define SHADER_PERMUTATION_BOOL(InDefineName)                \
	public                                                   \
	FShaderPermutationBool {                                 \
	public:                                                  \
		static constexpr const char *DefineName = InDefineName; \
	}

#define SHADER_PERMUTATION_INT(InDefineName, Count)          \
	public                                                   \
	TShaderPermutationInt<Count> {                           \
	public:                                                  \
		static constexpr const char *DefineName = InDefineName; \
	}

#define SHADER_PERMUTATION_RANGE_INT(InDefineName, Start, Count) \
	public                                                       \
	TShaderPermutationInt<Count, Start> {                        \
	public:                                                      \
		static constexpr const char *DefineName = InDefineName;     \
	}

#define SHADER_PERMUTATION_SPARSE_INT(InDefineName, ...)     \
	public                                                   \
	TShaderPermutationSparseInt<__VA_ARGS__> {               \
	public:                                                  \
		static constexpr const char *DefineName = InDefineName; \
	}

#define SHADER_PERMUTATION_ENUM_CLASS(InDefineName, EnumName)                         \
	public                                                                            \
	TShaderPermutationEnumClass<EnumName, static_cast<int32>(EnumName::MAX)> {        \
	public:                                                                           \
		static constexpr const char *DefineName = InDefineName;                          \
	}

template <typename... Ts>
struct TShaderPermutationDomain;

template <>
struct TShaderPermutationDomain<> {
	using Type = TShaderPermutationDomain<>;
	static constexpr bool IsMultiDimensional = true;
	static constexpr int32 PermutationCount = 1;

	TShaderPermutationDomain() = default;
	explicit TShaderPermutationDomain(int32 PermutationId) {}

	void ModifyCompilationEnvironment(FShaderCompilerEnvironment &) const {}
	int32 ToDimensionValueId() const { return 0; }
	static TShaderPermutationDomain FromDimensionValueId(int32) { return TShaderPermutationDomain(); }
	bool operator==(const TShaderPermutationDomain &) const { return true; }
	bool operator!=(const TShaderPermutationDomain &) const { return false; }
};

template <typename TDimension, typename... Ts>
struct TShaderPermutationDomain<TDimension, Ts...> {
	using Type = TShaderPermutationDomain<TDimension, Ts...>;
	using Super = TShaderPermutationDomain<Ts...>;
	static constexpr bool IsMultiDimensional = true;
	static constexpr int32 PermutationCount = Super::PermutationCount * TDimension::PermutationCount;

	TShaderPermutationDomain() :
			DimensionValue(TDimension::FromDimensionValueId(0)) {}
	explicit TShaderPermutationDomain(int32 PermutationId) :
			DimensionValue(TDimension::FromDimensionValueId(PermutationId % TDimension::PermutationCount)), Tail(PermutationId / TDimension::PermutationCount) {}

	template <class DimensionToSet>
	void Set(typename DimensionToSet::Type Value) {
		if constexpr (std::is_same_v<TDimension, DimensionToSet>) DimensionValue = Value;
		else Tail.template Set<DimensionToSet>(Value);
	}

	template <class DimensionToGet>
	typename DimensionToGet::Type Get() const {
		if constexpr (std::is_same_v<TDimension, DimensionToGet>) return DimensionValue;
		else return Tail.template Get<DimensionToGet>();
	}

	void ModifyCompilationEnvironment(FShaderCompilerEnvironment &OutEnvironment) const {
		OutEnvironment.SetDefine(TDimension::DefineName, TDimension::ToDefineValue(DimensionValue));
		Tail.ModifyCompilationEnvironment(OutEnvironment);
	}

	int32 ToDimensionValueId() const { return TDimension::ToDimensionValueId(DimensionValue) + TDimension::PermutationCount * Tail.ToDimensionValueId(); }
	static Type FromDimensionValueId(int32 PermutationId) { return Type(PermutationId); }

	bool operator==(const Type &Other) const { return DimensionValue == Other.DimensionValue && Tail == Other.Tail; }
	bool operator!=(const Type &Other) const { return !(*this == Other); }

private:
	template <typename... Other>
	friend struct TShaderPermutationDomain;

	typename TDimension::Type DimensionValue;
	Super Tail;
};

using FShaderPermutationNone = TShaderPermutationDomain<>;

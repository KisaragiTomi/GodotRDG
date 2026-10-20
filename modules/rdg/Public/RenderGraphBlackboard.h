#pragma once

// UE's FRDGBlackboard: a per-graph map from struct type to a single instance, used to pass data between systems
// that build parts of the same graph without threading parameters through every function.
//
//	RDG_REGISTER_BLACKBOARD_STRUCT(FMySceneTextures);
//	GraphBuilder.Blackboard.Create<FMySceneTextures>().Color = SceneColor;
//	const FMySceneTextures &Textures = GraphBuilder.Blackboard.GetChecked<FMySceneTextures>();

#include "RDGCore.h"

template <typename StructType>
struct TRDGBlackboardStructName {
	static const char *Get() { return "UnregisteredStruct"; }
};

class FRDGBlackboard {
public:
	FRDGBlackboard() = default;
	FRDGBlackboard(const FRDGBlackboard &) = delete;
	~FRDGBlackboard() { Clear(); }

	template <typename StructType, typename... ArgsType>
	StructType &Create(ArgsType &&...Args) {
		const uint32 StructIndex = GetStructIndex<StructType>();
		if (StructIndex >= Blackboard.size()) Blackboard.resize(StructIndex + 1);
		if (Blackboard[StructIndex].Data) {
			ERR_PRINT(vformat("[RDG] Blackboard struct '%s' already exists.", GetStructName<StructType>()));
			return *reinterpret_cast<StructType *>(Blackboard[StructIndex].Data);
		}
		StructType *Result = memnew(StructType(std::forward<ArgsType>(Args)...));
		Blackboard[StructIndex].Data = Result;
		Blackboard[StructIndex].Destructor = [](void *Data) { memdelete(reinterpret_cast<StructType *>(Data)); };
		return *Result;
	}

	template <typename StructType>
	StructType &GetOrCreate() {
		if (StructType *Struct = GetMutable<StructType>()) return *Struct;
		return Create<StructType>();
	}

	template <typename StructType>
	const StructType *Get() const { return const_cast<FRDGBlackboard *>(this)->GetMutable<StructType>(); }

	template <typename StructType>
	StructType *GetMutable() const {
		const uint32 StructIndex = GetStructIndex<StructType>();
		if (StructIndex < Blackboard.size()) return reinterpret_cast<StructType *>(Blackboard[StructIndex].Data);
		return nullptr;
	}

	template <typename StructType>
	const StructType &GetChecked() const {
		const StructType *Struct = Get<StructType>();
		CRASH_COND_MSG(!Struct, vformat("[RDG] Blackboard struct '%s' has not been created.", GetStructName<StructType>()));
		return *Struct;
	}

	template <typename StructType>
	StructType &GetMutableChecked() const {
		StructType *Struct = GetMutable<StructType>();
		CRASH_COND_MSG(!Struct, vformat("[RDG] Blackboard struct '%s' has not been created.", GetStructName<StructType>()));
		return *Struct;
	}

	void Clear() {
		for (FEntry &Entry : Blackboard) {
			if (Entry.Data) Entry.Destructor(Entry.Data);
			Entry.Data = nullptr;
		}
		Blackboard.clear();
	}

	template <typename StructType>
	static const char *GetStructName() { return TRDGBlackboardStructName<StructType>::Get(); }

private:
	struct FEntry {
		void *Data = nullptr;
		void (*Destructor)(void *) = nullptr;
	};

	static uint32 AllocateIndex();

	template <typename StructType>
	static uint32 GetStructIndex() {
		static const uint32 Index = AllocateIndex();
		return Index;
	}

	LocalVector<FEntry> Blackboard;
};

/** Same usage as UE (at namespace scope, after the struct). Provides the debug name. */
#define RDG_REGISTER_BLACKBOARD_STRUCT(StructType)      \
	template <>                                         \
	struct TRDGBlackboardStructName<StructType> {       \
		static const char *Get() { return #StructType; } \
	};

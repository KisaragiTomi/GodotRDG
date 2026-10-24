#pragma once

#include "RenderGraphDefinitions.h"

/** Formatted name of a pass or event scope (UE's FRDGEventName). */
class FRDGEventName {
public:
	FRDGEventName() = default;
	explicit FRDGEventName(const char *EventFormat, ...);
	FRDGEventName(const FRDGEventName &Other) = default;
	FRDGEventName(FRDGEventName &&Other) = default;
	FRDGEventName &operator=(const FRDGEventName &Other) = default;
	FRDGEventName &operator=(FRDGEventName &&Other) = default;

	/** UE spells it GetTCHAR(); RDG strings are UTF-8 here. */
	const char *GetTCHAR() const { return Formatted.get_data(); }
	const char *GetName() const { return Formatted.get_data(); }
	String ToString() const { return String::utf8(Formatted.get_data()); }
	bool IsEmpty() const { return Formatted.length() == 0; }

private:
	CharString Formatted;
};

#define RDG_EVENT_NAME(Format, ...) FRDGEventName(Format, ##__VA_ARGS__)

/** Hierarchical event scope; every pass records the scope that was active when it was added. */
class FRDGEventScope {
public:
	FRDGEventScope(const FRDGEventScope *InParentScope, FRDGEventName &&InName) :
			ParentScope(InParentScope), Name(std::move(InName)) {}

	const FRDGEventScope *const ParentScope;
	const FRDGEventName Name;

	/** "Parent/Child" path, used for the RD debug labels and the graph dump. */
	String GetPath() const;
};

class FRDGEventScopeGuard {
public:
	FRDGEventScopeGuard(FRDGBuilder &InGraphBuilder, FRDGEventName &&ScopeName, bool bCondition = true);
	~FRDGEventScopeGuard();
	FRDGEventScopeGuard(const FRDGEventScopeGuard &) = delete;

private:
	FRDGBuilder &GraphBuilder;
	bool bPushed = false;
};

#define RDG_EVENT_SCOPE_CONCAT_INNER(A, B) A##B
#define RDG_EVENT_SCOPE_CONCAT(A, B) RDG_EVENT_SCOPE_CONCAT_INNER(A, B)

#define RDG_EVENT_SCOPE(GraphBuilder, Format, ...) \
	FRDGEventScopeGuard RDG_EVENT_SCOPE_CONCAT(PREPROCESSOR_RDGEventScope_, __LINE__)((GraphBuilder), RDG_EVENT_NAME(Format, ##__VA_ARGS__))
#define RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Condition, Format, ...) \
	FRDGEventScopeGuard RDG_EVENT_SCOPE_CONCAT(PREPROCESSOR_RDGEventScope_, __LINE__)((GraphBuilder), RDG_EVENT_NAME(Format, ##__VA_ARGS__), (Condition))
#define RDG_EVENT_SCOPE_STAT(GraphBuilder, Stat, Format, ...) RDG_EVENT_SCOPE(GraphBuilder, Format, ##__VA_ARGS__)
#define RDG_EVENT_SCOPE_FINAL(GraphBuilder, Format, ...) RDG_EVENT_SCOPE(GraphBuilder, Format, ##__VA_ARGS__)

/** GPU stat (UE's DECLARE_GPU_STAT): the passes added inside its RDG_GPU_STAT_SCOPE are timed with RenderingDevice
 *  timestamps named "<Name> begin" / "<Name> end" (RenderingDevice::get_captured_timestamp_*, one frame later). */
class FRDGGPUStat {
public:
	explicit FRDGGPUStat(const char *InName) :
			Name(InName), BeginName(String::utf8(InName) + " begin"), EndName(String::utf8(InName) + " end") {}

	const char *const Name;
	const String BeginName;
	const String EndName;
};

class FRDGGPUStatScopeGuard {
public:
	FRDGGPUStatScopeGuard(FRDGBuilder &InGraphBuilder, const FRDGGPUStat &Stat);
	~FRDGGPUStatScopeGuard();
	FRDGGPUStatScopeGuard(const FRDGGPUStatScopeGuard &) = delete;

private:
	FRDGBuilder &GraphBuilder;
	const FRDGGPUStat *const ParentStat;
};

#define DECLARE_GPU_STAT_NAMED(StatName, NameString) static FRDGGPUStat GPUStat_##StatName(NameString);
#define DECLARE_GPU_STAT(StatName) DECLARE_GPU_STAT_NAMED(StatName, #StatName)
#define DEFINE_GPU_STAT(StatName)
#define RDG_GPU_STAT_SCOPE(GraphBuilder, StatName) \
	FRDGGPUStatScopeGuard RDG_EVENT_SCOPE_CONCAT(PREPROCESSOR_RDGGPUStatScope_, __LINE__)((GraphBuilder), GPUStat_##StatName)

// CSV / mask / task scopes compile to nothing.
#define RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, StatName)
#define RDG_GPU_MASK_SCOPE(GraphBuilder, GPUMask)
#define RDG_WAIT_FOR_TASKS_CONDITIONAL(GraphBuilder, bCondition)
#define SCOPED_NAMED_EVENT(Name, Color)
#define SCOPE_CYCLE_COUNTER(Stat)
#define TRACE_CPUPROFILER_EVENT_SCOPE(Name)

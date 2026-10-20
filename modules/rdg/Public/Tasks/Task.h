#pragma once

// Minimal UE::Tasks (FTask, Launch, Wait) on Godot's WorkerThreadPool, for FRDGBuilder::AddSetupTask.
// - A task goes to the pool once its prerequisites completed: the last prerequisite to complete submits it. Pool threads
//   never block on other tasks (Godot rejects a pool thread waiting for an older task, see wait_for_task_completion).
// - WorkerThreadPool requires every task to be waited for: Wait() joins it, from a thread outside the pool (the graph
//   builder waits for all of its tasks on the render thread).
// FTask is a single intrusive reference, so it can live in Godot containers (they relocate elements with memrealloc).

#include "core/object/worker_thread_pool.h"
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"

#include <atomic>
#include <functional>
#include <type_traits>

namespace UE::Tasks {

/** Accepted for source compatibility; High maps to a WorkerThreadPool high priority task. */
enum class ETaskPriority : int8 {
	High,
	Normal,
	Default = Normal,
	BackgroundHigh,
	BackgroundNormal,
	BackgroundLow,
};

class FTask;

namespace Private {

class FTaskBase {
public:
	std::function<void()> Body;
	const char *DebugName = "";
	bool bHighPriority = false;
	SafeRefCount RefCount;
	/** Incomplete prerequisites, plus one held while the task is launched. */
	std::atomic<int32> NumPending{ 1 };

	FTaskBase() { RefCount.init(); }

	bool IsCompleted() const { return bCompleted.load(std::memory_order_acquire); }

	/** Blocks until the task completed and joins its pool task. Not from pool threads (use prerequisites). */
	void Wait() {
		WorkerThreadPool::TaskID TaskToJoin = WorkerThreadPool::INVALID_TASK_ID;
		{
			MutexLock Lock(StateMutex);
			while (!bCompleted.load(std::memory_order_acquire)) Completed.wait(Lock);
			if (!bJoined) {
				bJoined = true;
				TaskToJoin = TaskID;
			}
		}
		if (TaskToJoin != WorkerThreadPool::INVALID_TASK_ID) WorkerThreadPool::get_singleton()->wait_for_task_completion(TaskToJoin);
	}

	/** Makes Subsequent wait for this task. False when this task already completed. */
	bool AddSubsequent(FTaskBase *Subsequent) {
		MutexLock Lock(StateMutex);
		if (bCompleted.load(std::memory_order_relaxed)) return false;
		Subsequents.push_back(Subsequent);
		return true;
	}

	/** Called when a prerequisite completed (or the launch finished): submits the task once nothing is pending. */
	void ReleasePending() {
		if (NumPending.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
		// Holding the lock while submitting: the task can't complete before TaskID is stored.
		MutexLock Lock(StateMutex);
		TaskID = WorkerThreadPool::get_singleton()->add_native_task(&FTaskBase::Run, this, bHighPriority, DebugName);
	}

	static void AddRef(FTaskBase *Task) {
		if (Task) Task->RefCount.ref();
	}
	static void Release(FTaskBase *Task) {
		if (Task && Task->RefCount.unref()) delete Task;
	}

	static void Run(void *Data) {
		FTaskBase *Task = reinterpret_cast<FTaskBase *>(Data);
		Task->Body();
		Task->Body = nullptr;
		LocalVector<FTaskBase *> ReadySubsequents;
		{
			MutexLock Lock(Task->StateMutex);
			Task->bCompleted.store(true, std::memory_order_release);
			ReadySubsequents = std::move(Task->Subsequents);
			Task->Subsequents.clear();
			Task->Completed.notify_all();
		}
		for (FTaskBase *Subsequent : ReadySubsequents) {
			Subsequent->ReleasePending();
			Release(Subsequent);
		}
		// The reference of the pool task.
		Release(Task);
	}

private:
	BinaryMutex StateMutex;
	ConditionVariable Completed;
	std::atomic<bool> bCompleted{ false };
	bool bJoined = false;
	WorkerThreadPool::TaskID TaskID = WorkerThreadPool::INVALID_TASK_ID;
	/** Tasks waiting for this one (each holds a reference). */
	LocalVector<FTaskBase *> Subsequents;
};

} // namespace Private

class FTask {
public:
	FTask() = default;
	FTask(const FTask &Other) :
			Pimpl(Other.Pimpl) { Private::FTaskBase::AddRef(Pimpl); }
	FTask(FTask &&Other) :
			Pimpl(Other.Pimpl) { Other.Pimpl = nullptr; }
	~FTask() { Private::FTaskBase::Release(Pimpl); }
	FTask &operator=(const FTask &Other) {
		if (Pimpl != Other.Pimpl) {
			Private::FTaskBase::AddRef(Other.Pimpl);
			Private::FTaskBase::Release(Pimpl);
			Pimpl = Other.Pimpl;
		}
		return *this;
	}
	FTask &operator=(FTask &&Other) {
		if (this != &Other) {
			Private::FTaskBase::Release(Pimpl);
			Pimpl = Other.Pimpl;
			Other.Pimpl = nullptr;
		}
		return *this;
	}

	bool IsValid() const { return Pimpl != nullptr; }
	bool IsCompleted() const { return !Pimpl || Pimpl->IsCompleted(); }
	/** Blocks until the task completed. Not from inside a task: express the dependency with prerequisites. */
	bool Wait() const {
		if (Pimpl) Pimpl->Wait();
		return true;
	}

private:
	Private::FTaskBase *Pimpl = nullptr;

	template <typename TaskBodyType>
	friend FTask Launch(const char *DebugName, TaskBodyType &&TaskBody, LocalVector<FTask> &&Prerequisites, ETaskPriority Priority);
};

/** Launches a task on the WorkerThreadPool once its prerequisites completed. The caller must Wait() for it. */
template <typename TaskBodyType>
FTask Launch(const char *DebugName, TaskBodyType &&TaskBody, LocalVector<FTask> &&Prerequisites = {}, ETaskPriority Priority = ETaskPriority::Normal) {
	FTask Task;
	Private::FTaskBase *Base = new Private::FTaskBase();
	Task.Pimpl = Base;
	Base->Body = std::forward<TaskBodyType>(TaskBody);
	Base->DebugName = DebugName;
	Base->bHighPriority = Priority == ETaskPriority::High;
	// The reference of the pool task, released at the end of Run.
	Private::FTaskBase::AddRef(Base);
	for (const FTask &Prerequisite : Prerequisites) {
		if (!Prerequisite.Pimpl) continue;
		Base->NumPending.fetch_add(1, std::memory_order_relaxed);
		Private::FTaskBase::AddRef(Base);
		if (!Prerequisite.Pimpl->AddSubsequent(Base)) {
			Base->NumPending.fetch_sub(1, std::memory_order_relaxed);
			Private::FTaskBase::Release(Base);
		}
	}
	Base->ReleasePending();
	return Task;
}

inline void Wait(const LocalVector<FTask> &Tasks) {
	for (const FTask &Task : Tasks) Task.Wait();
}

namespace Private {

/** Prerequisites as UE accepts them: one task or a collection of tasks. */
inline void AppendPrerequisites(LocalVector<FTask> &Out, const FTask &Task) {
	if (Task.IsValid()) Out.push_back(Task);
}
template <typename CollectionType, typename = decltype(std::declval<const CollectionType &>().begin())>
inline void AppendPrerequisites(LocalVector<FTask> &Out, const CollectionType &Tasks) {
	for (const FTask &Task : Tasks) AppendPrerequisites(Out, Task);
}

template <typename T, typename = void>
struct TIsTaskCollection : std::false_type {};
template <typename T>
struct TIsTaskCollection<T, std::enable_if_t<std::is_same_v<std::decay_t<decltype(*std::declval<const T &>().begin())>, FTask>>> : std::true_type {};
template <typename T>
inline constexpr bool TIsPrerequisites = std::is_same_v<std::decay_t<T>, FTask> || TIsTaskCollection<std::decay_t<T>>::value;

} // namespace Private

} // namespace UE::Tasks

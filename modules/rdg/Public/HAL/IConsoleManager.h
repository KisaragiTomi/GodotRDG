#pragma once

// UE-style console variables (TAutoConsoleVariable / FAutoConsoleVariableRef / IConsoleManager).
// Values can be changed from GDScript through RenderGraphConsole.set_cvar() or from the command line
// with user arguments, e.g. `godot -- r.RDG.ImmediateMode=1 r.RDG.Debug=1`.

#include "RDGCore.h"

#include "core/templates/list.h"

enum EConsoleVariableFlags : uint32 {
	ECVF_Default = 0,
	ECVF_Cheat = 1 << 0,
	ECVF_ReadOnly = 1 << 2,
	ECVF_Scalability = 1 << 4,
	ECVF_RenderThreadSafe = 1 << 5,
};

class IConsoleVariable {
public:
	IConsoleVariable(const char *InName, const char *InHelp, uint32 InFlags) :
			Name(InName), Help(InHelp), Flags(InFlags) {}
	virtual ~IConsoleVariable() = default;

	virtual int32 GetInt() const = 0;
	virtual float GetFloat() const = 0;
	virtual bool GetBool() const { return GetInt() != 0; }
	virtual String GetString() const = 0;
	virtual void Set(const char *InValue) = 0;
	virtual void Set(int32 InValue) = 0;
	virtual void Set(float InValue) = 0;

	const char *GetName() const { return Name; }
	const char *GetHelp() const { return Help; }
	uint32 GetFlags() const { return Flags; }

private:
	const char *Name;
	const char *Help;
	uint32 Flags;
};

class IConsoleManager {
public:
	static IConsoleManager &Get();

	IConsoleVariable *FindConsoleVariable(const char *Name) const;
	void Register(IConsoleVariable *Variable);
	void Unregister(IConsoleVariable *Variable);

	template <typename FunctionType>
	void ForEachConsoleVariable(FunctionType &&Function) const {
		for (IConsoleVariable *Variable : Variables) Function(Variable);
	}

	/** Applies `name=value` pairs, e.g. from OS::get_cmdline_user_args(). Returns the number applied. */
	int32 ApplyCommandLine(const List<String> &Arguments);

private:
	LocalVector<IConsoleVariable *> Variables;
};

template <typename T>
class TAutoConsoleVariable final : public IConsoleVariable {
	static_assert(std::is_same_v<T, int32> || std::is_same_v<T, float> || std::is_same_v<T, bool>, "Only int32, float and bool console variables are supported.");

public:
	TAutoConsoleVariable(const char *InName, T DefaultValue, const char *InHelp, uint32 InFlags = ECVF_Default) :
			IConsoleVariable(InName, InHelp, InFlags), Value(DefaultValue) {
		IConsoleManager::Get().Register(this);
	}
	~TAutoConsoleVariable() override { IConsoleManager::Get().Unregister(this); }

	T GetValueOnAnyThread() const { return Value; }
	T GetValueOnRenderThread() const { return Value; }
	T GetValueOnGameThread() const { return Value; }

	int32 GetInt() const override { return (int32)Value; }
	float GetFloat() const override { return (float)Value; }
	String GetString() const override {
		if constexpr (std::is_same_v<T, float>) return String::num(Value);
		else return itos((int64)Value);
	}
	void Set(const char *InValue) override {
		const String S = String::utf8(InValue);
		if constexpr (std::is_same_v<T, float>) Value = (float)S.to_float();
		else Value = (T)S.to_int();
	}
	void Set(int32 InValue) override { Value = (T)InValue; }
	void Set(float InValue) override { Value = (T)InValue; }

	IConsoleVariable *operator->() { return this; }

private:
	T Value;
};

/** Binds a console variable to an existing global, like UE's FAutoConsoleVariableRef. */
class FAutoConsoleVariableRef final : public IConsoleVariable {
public:
	FAutoConsoleVariableRef(const char *InName, int32 &InRef, const char *InHelp, uint32 InFlags = ECVF_Default);
	FAutoConsoleVariableRef(const char *InName, float &InRef, const char *InHelp, uint32 InFlags = ECVF_Default);
	FAutoConsoleVariableRef(const char *InName, bool &InRef, const char *InHelp, uint32 InFlags = ECVF_Default);
	~FAutoConsoleVariableRef() override;

	int32 GetInt() const override;
	float GetFloat() const override;
	String GetString() const override;
	void Set(const char *InValue) override;
	void Set(int32 InValue) override;
	void Set(float InValue) override;

private:
	int32 *IntRef = nullptr;
	float *FloatRef = nullptr;
	bool *BoolRef = nullptr;
};

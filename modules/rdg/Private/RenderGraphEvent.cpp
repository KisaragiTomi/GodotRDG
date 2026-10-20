#include "RenderGraphBlackboard.h"
#include "RenderGraphEvent.h"

#include <atomic>

FRDGEventName::FRDGEventName(const char *EventFormat, ...) {
	if (!EventFormat) return;
	va_list Args;
	va_start(Args, EventFormat);
	Formatted = RDGPrivate::VFormat(EventFormat, Args).utf8();
	va_end(Args);
}

String FRDGEventScope::GetPath() const {
	String Path = Name.ToString();
	for (const FRDGEventScope *Scope = ParentScope; Scope; Scope = Scope->ParentScope) Path = Scope->Name.ToString() + "/" + Path;
	return Path;
}

uint32 FRDGBlackboard::AllocateIndex() {
	static std::atomic<uint32> NextIndex{ 0 };
	return NextIndex++;
}

#pragma once
#include <Windows.h>

void* splice(void* splicedFunctionAddress, void* hookFunction);
BOOL restore(void*& oldProc);

// Same as splice(), except a missing export (null splicedFunctionAddress)
// returns nullptr instead of throwing. For engine builds where some of a
// version's expected hooks are gone (renamed, inlined, or the whole class
// removed) - lets *Wrapper::Init() keep setting up every hook it still can
// instead of failing Application::Init() entirely on the first missing one.
void* trySplice(void* splicedFunctionAddress, void* hookFunction);
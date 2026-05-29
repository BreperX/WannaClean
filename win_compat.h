#pragma once

#include <process.h>
#include <cstdint>

extern "C" uintptr_t __cdecl _beginthreadex(void* _Security, unsigned _StackSize,
											 unsigned (__stdcall* _StartAddress)(void*),
											 void* _ArgList, unsigned _InitFlag,
											 unsigned* _ThrdAddr);

namespace std { using ::_beginthreadex; }

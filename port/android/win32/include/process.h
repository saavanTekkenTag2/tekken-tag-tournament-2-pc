#pragma once
#include <windows.h>
extern "C" uintptr_t _beginthreadex(void* security, unsigned stack, unsigned (*start)(void*), void* arg, unsigned flags, unsigned* id);
extern "C" uintptr_t _beginthread(void (*start)(void*), unsigned stack, void* arg);
extern "C" void _endthreadex(unsigned code);

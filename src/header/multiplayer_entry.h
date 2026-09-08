#pragma once

extern "C" __declspec(dllexport) void __cdecl WANdStart();
extern "C" __declspec(dllexport) int __cdecl WANdHost(unsigned short port);
extern "C" __declspec(dllexport) int __cdecl WANdJoin(const char* address, unsigned short port);
extern "C" __declspec(dllexport) void __cdecl WANdStop();
extern "C" __declspec(dllexport) int __cdecl WANdStatus();
extern "C" __declspec(dllexport) void* __cdecl WANdLuaState();

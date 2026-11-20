// The Linux build's Win32: the renderer's D3D12 code runs on vkd3d-proton (libvkd3d-proton-d3d12.so)
// with DXVK Native's DXGI (libdxvk_dxgi.so), and DXVK Native's Windows and DirectX headers. Those
// headers have the types only; the functions the renderer calls are here, on POSIX and SDL3
// (linux/win32.cpp). Every renderer source gets this file first (-include, rexnative.cmake).
//
// A HANDLE is a file descriptor, as vkd3d-proton's own on Linux: an event is an eventfd (what
// ID3D12Fence::SetEventOnCompletion writes to, and what a swap chain's frame latency object is), a
// waitable timer a timerfd; WaitForSingleObject polls it. An HWND is the game's SDL_Window, which
// DXVK's DXGI takes for a swap chain.
#pragma once

// vkd3d-proton returns structures (GetCPUDescriptorHandleForHeapStart, GetDesc) through a
// pointer, as MSVC does: the headers' methods must do the same.
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif

#include <windows.h>
#include <unknwn.h>

#include <climits>
#include <cstdint>
#include <cstdio>

#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef TIMER_ALL_ACCESS
#define TIMER_ALL_ACCESS 0x001F0003
#endif

#ifndef CALLBACK
#define CALLBACK
#endif
typedef LONG_PTR LRESULT;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef BYTE BOOLEAN;
typedef intptr_t (*FARPROC)();
typedef DWORD (*LPTHREAD_START_ROUTINE)(void* parameter);
typedef void (*PTIMERAPCROUTINE)(void* arg, DWORD low, DWORD high);
typedef SECURITY_ATTRIBUTES* LPSECURITY_ATTRIBUTES;

// Events (eventfd).
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state,
                    LPCWSTR name);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state,
                    LPCSTR name);
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
BOOL CloseHandle(HANDLE handle);
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD milliseconds);
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL wait_all,
                               DWORD milliseconds, BOOL alertable);

// Waitable timers (timerfd).
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags,
                              DWORD access);
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG period,
                      PTIMERAPCROUTINE routine, void* arg, BOOL resume);

// Threads.
HANDLE GetCurrentThread();
DWORD GetCurrentThreadId();
HRESULT SetThreadDescription(HANDLE thread, LPCWSTR description);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack_size,
                    LPTHREAD_START_ROUTINE start, void* parameter, DWORD flags, DWORD* thread_id);
void Sleep(DWORD milliseconds);

// Modules: a Windows DLL name is not found (no DLSS, no FidelityFX, no Wine's ntdll); a path or
// a lib*.so name is dlopen'ed.
HMODULE LoadLibraryW(LPCWSTR name);
HMODULE LoadLibraryA(LPCSTR name);
HMODULE GetModuleHandleW(LPCWSTR name);
FARPROC GetProcAddress(HMODULE module, LPCSTR name);
BOOL FreeLibrary(HMODULE module);
DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size);
DWORD GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size);

// Strings.
int WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR wide, int wide_count, LPSTR out,
                        int out_size, LPCSTR default_char, BOOL* used_default);
int MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR text, int count, LPWSTR out,
                        int out_size);

// The rest.
DWORD GetLastError();
DWORD GetTempPathW(DWORD size, LPWSTR buffer);
BOOL QueryPerformanceCounter(LARGE_INTEGER* count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency);
USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG count, void** frames, ULONG* hash);
// The client area of an HWND, the game's SDL_Window, in pixels.
BOOL GetClientRect(HWND window, RECT* rect);

// Files with wide names (_wfopen), and std::filesystem::path::c_str(), which is char here.
std::FILE* _wfopen(const wchar_t* path, const wchar_t* mode);
std::FILE* _wfopen(const char* path, const wchar_t* mode);
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value);
// No asynchronous key state here (the renderer's F12 capture shortcut): never down.
inline SHORT GetAsyncKeyState(int) { return 0; }

// Virtual key codes (host.cpp's key names).
#ifndef VK_F1
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87
#endif

// The flag enums' operators constexpr, as the Windows SDK's are (the renderer's constexpr
// resource state masks).
#undef DEFINE_ENUM_FLAG_OPERATORS
#define DEFINE_ENUM_FLAG_OPERATORS(type)                                                     extern "C++" {                                                                             constexpr type operator&(type x, type y) { return (type)((int)x & (int)y); }               constexpr type& operator&=(type& x, type y) { return x = (type)((int)x & (int)y); }        constexpr type operator~(type x) { return (type) ~(int)x; }                                constexpr type operator|(type x, type y) { return (type)((int)x | (int)y); }               constexpr type& operator|=(type& x, type y) { return x = (type)((int)x | (int)y); }        constexpr type operator^(type x, type y) { return (type)((int)x ^ (int)y); }               constexpr type& operator^=(type& x, type y) { return x = (type)((int)x ^ (int)y); }        }

// D3D12: mingw-w64's d3d12.h (newer than DXVK Native's), and what it does not have yet, as
// vkd3d-proton's vkd3d_d3d12.idl declares it.
#include <d3d12.h>

// Beyond the enum's range in this header: macros, not constexpr values.
#define D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS ((D3D12_PIPELINE_STATE_FLAGS)0x4)
#define D3D12_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT ((D3D12_PIPELINE_STATE_FLAGS)0x8)

struct ID3D12GraphicsCommandList8 : public ID3D12GraphicsCommandList7 {
  virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT front, UINT back) = 0;
};
DECLARE_UUIDOF_HELPER(ID3D12GraphicsCommandList8, 0xee936ef9, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4,
                      0xad, 0x05, 0xce, 0x51)
struct ID3D12GraphicsCommandList9 : public ID3D12GraphicsCommandList8 {
  virtual void STDMETHODCALLTYPE RSSetDepthBias(FLOAT bias, FLOAT clamp, FLOAT slope) = 0;
  virtual void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(
      D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value) = 0;
};
DECLARE_UUIDOF_HELPER(ID3D12GraphicsCommandList9, 0x34ed2808, 0xffe6, 0x4c2b, 0xb1, 0x1a, 0xca, 0xbd,
                      0x2b, 0x0c, 0x59, 0xe1)

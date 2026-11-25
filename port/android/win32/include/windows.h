// TTT2 Android: a small Win32 layer over POSIX/Linux (bionic) and SDL2.
//
// The PC runtime is written against Win32. Rather than fork every file, the Android
// build compiles the same sources against this header. Only the subset of Win32 the
// runtime uses is provided. Base types, COM basics and DirectX headers come from
// dxvk-native (include/native/windows), so D3D11 code links against DXVK unchanged.
//
// Implementation: port/android/win32/src/*.cpp.
#pragma once
#ifndef TTT2_WIN32_SHIM
#define TTT2_WIN32_SHIM 1

#include <windows_base.h>
#include <unknwn.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <atomic>
#include <strings.h>
#include <unistd.h>
#include <sched.h>
#include <msvc_crt.h>

// ---------------------------------------------------------------------------------
// Compiler / SAL
// ---------------------------------------------------------------------------------
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#define __declspec(x) __declspec_##x
#define __declspec_noinline __attribute__((noinline))
#define __declspec_noreturn __attribute__((noreturn))
#define __declspec_thread thread_local
#define __declspec_selectany __attribute__((weak))
#define __declspec_dllexport __attribute__((visibility("default")))
#define __declspec_dllimport
#define __declspec_restrict
#define __declspec_novtable
#define __declspec_naked __attribute__((naked))
#define __declspec_safebuffers
#define __declspec_nothrow
#define __declspec_allocator
#define __declspec_uuid(x)
#define __declspec_align(x) __attribute__((aligned(x)))
#define __cdecl
#define __fastcall
#define __vectorcall
#define __thiscall
#define CALLBACK
#define APIENTRY
#define NTAPI
#define WINAPIV
#define FAR
#define NEAR
#define IN
#define OUT
#define OPTIONAL
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define _In_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#define _Inout_opt_
#define _In_z_
#define _In_reads_(x)
#define _In_reads_bytes_(x)
#define _Out_writes_(x)
#define _Out_writes_bytes_(x)
#define _Out_writes_z_(x)
#define _Printf_format_string_
#define _Success_(x)
#define _Check_return_
#define _Ret_maybenull_
#define _Use_decl_annotations_
#define _Analysis_assume_(x)
#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define ARRAYSIZE(a) _countof(a)
#define FIELD_OFFSET(t, f) ((LONG)offsetof(t, f))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define LOWORD(l) ((WORD)(((uintptr_t)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((uintptr_t)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((uintptr_t)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((uintptr_t)(w)) >> 8) & 0xff))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define TEXT(x) x
#define _T(x) x

// ---------------------------------------------------------------------------------
// Types (LLP64 sizes, as on Windows: LONG/DWORD are 32-bit)
// ---------------------------------------------------------------------------------
typedef unsigned char UCHAR, *PUCHAR, *PBYTE, *LPBYTE;
typedef char CHAR, CCHAR, *PCHAR, *PSTR, *NPSTR;
typedef const char *PCSTR;
typedef wchar_t *PWSTR, *LPWSTR, *PWCHAR, *NWPSTR;
typedef const wchar_t *PCWSTR, *LPCWCH;
typedef int8_t INT8;
typedef int16_t INT16;
typedef int32_t INT32, LONG32;
typedef int64_t INT64, LONG64, *PINT64;
typedef uint32_t ULONG32, DWORD32;
typedef uint64_t ULONG64, DWORD64, *PULONG64, *PDWORD64, QWORD;
typedef intptr_t INT_PTR, LONG_PTR, SSIZE_T, *PLONG_PTR;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR, *PULONG_PTR, *PDWORD_PTR;
typedef ULONG_PTR KAFFINITY;
typedef DWORD *PDWORD, *LPDWORD;
typedef LONG *PLONG, *LPLONG;
typedef ULONG *PULONG;
typedef USHORT *PUSHORT;
typedef WORD *PWORD, *LPWORD;
typedef BOOL *PBOOL, *LPBOOL;
typedef BYTE BOOLEAN, *PBOOLEAN;
typedef INT *PINT, *LPINT;
typedef UINT *PUINT;
typedef void *PVOID;
typedef HANDLE *PHANDLE, *LPHANDLE;
typedef HANDLE HINSTANCE, HGLOBAL, HLOCAL, HICON, HCURSOR, HBRUSH, HMENU, HFONT, HBITMAP, HGDIOBJ, HRGN, HPEN;
typedef HANDLE HPALETTE, HRSRC, HHOOK, HWINEVENTHOOK, HDESK, HWINSTA, HTASK, HACCEL, HKL, HDROP, HDEVNOTIFY;
typedef HANDLE HPOWERNOTIFY, HRAWINPUT, HGESTUREINFO, HTOUCHINPUT, DPI_AWARENESS_CONTEXT;
typedef LONG_PTR LRESULT;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef WORD ATOM;
typedef DWORD ACCESS_MASK, *PACCESS_MASK;
typedef LONG NTSTATUS;
typedef DWORD LCID;
typedef WORD LANGID;
typedef DWORD EXECUTION_STATE;
typedef int (*FARPROC)();
typedef int (*NEARPROC)();
typedef int (*PROC)();
typedef LONG HRESULT_T;
typedef RECT *PRECT, *LPRECT;
typedef const RECT *LPCRECT;
typedef SIZE *PSIZE, *LPSIZE;
typedef POINT *PPOINT;
typedef LARGE_INTEGER *PLARGE_INTEGER;
typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;
typedef SECURITY_ATTRIBUTES *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef GUID *LPGUID;
typedef const GUID *LPCGUID;
typedef GUID CLSID;
typedef const GUID& REFCLSID;
typedef wchar_t OLECHAR, *LPOLESTR, *BSTR;
typedef const wchar_t *LPCOLESTR;
typedef double DATE;
typedef ULONGLONG *PULONGLONG;
typedef SIZE_T *PSIZE_T;

#define VOID void
#define CONST const
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define MAXDWORD 0xffffffffu
#define MAXLONG 0x7fffffff
#define MINLONG ((LONG)0x80000000)
#define MAXULONG_PTR (~((ULONG_PTR)0))
#define MAXUINT ((UINT)~((UINT)0))
#define MAXINT ((INT)(MAXUINT >> 1))
#define MAXUINT64 (~((UINT64)0))
#define MAXINT64 ((INT64)(MAXUINT64 >> 1))
#define MAXSIZE_T ((SIZE_T)~((SIZE_T)0))
#define MAXSHORT 0x7fff

typedef struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME, *PFILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;
typedef struct _TIME_ZONE_INFORMATION {
    LONG Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; LONG StandardBias;
    WCHAR DaylightName[32]; SYSTEMTIME DaylightDate; LONG DaylightBias;
} TIME_ZONE_INFORMATION, *PTIME_ZONE_INFORMATION, *LPTIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_UNKNOWN 0
#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union { struct { DWORD Offset; DWORD OffsetHigh; }; PVOID Pointer; };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef void (*LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD, DWORD, LPOVERLAPPED);

typedef uint64_t DWORDLONG, *PDWORDLONG;
typedef struct _MEMORYSTATUSEX {
    DWORD dwLength; DWORD dwMemoryLoad;
    DWORDLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile;
    DWORDLONG ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

typedef struct _SYSTEM_INFO {
    union { DWORD dwOemId; struct { WORD wProcessorArchitecture; WORD wReserved; }; };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
#define PROCESSOR_ARCHITECTURE_ARM64 12
#define PROCESSOR_ARCHITECTURE_AMD64 9

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress, AllocationBase; DWORD AllocationProtect; WORD PartitionId;
    SIZE_T RegionSize; DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

// ---------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------
#define ERROR_SUCCESS 0L
#define NO_ERROR 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INVALID_DATA 13L
#define ERROR_OUTOFMEMORY 14L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_WRITE_PROTECT 19L
#define ERROR_NOT_READY 21L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_LOCK_VIOLATION 33L
#define ERROR_HANDLE_EOF 38L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_DISK_FULL 112L
#define ERROR_CALL_NOT_IMPLEMENTED 120L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_BUSY 170L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_ENVVAR_NOT_FOUND 203L
#define ERROR_FILENAME_EXCED_RANGE 206L
#define ERROR_MORE_DATA 234L
#define WAIT_TIMEOUT 258L
#define ERROR_NO_MORE_ITEMS 259L
#define ERROR_DIRECTORY 267L
#define ERROR_NOT_OWNER 288L
#define ERROR_OPERATION_ABORTED 995L
#define ERROR_IO_INCOMPLETE 996L
#define ERROR_IO_PENDING 997L
#define ERROR_NOACCESS 998L
#define ERROR_TIMEOUT 1460L
#define ERROR_FILE_CORRUPT 1392L
#define ERROR_DISK_CORRUPT 1393L
#define ERROR_INVALID_FLAGS 1004L
#define ERROR_NOT_FOUND 1168L
#define ERROR_CANCELLED 1223L
#define ERROR_INTERNAL_ERROR 1359L
#define ERROR_CONNECTION_REFUSED 1225L
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#define ERROR_UNRECOGNIZED_VOLUME 1005L
#define ERROR_NOT_SAME_DEVICE 17L
#define ERROR_BAD_PATHNAME 161L
#define ERROR_EMPTY 4306L
#define ERROR_FILE_INVALID 1006L
#define ERROR_BAD_ARGUMENTS 160L
#define ERROR_CRC 23L
#define ERROR_WRITE_FAULT 29L
#define ERROR_READ_FAULT 30L
#define ERROR_GEN_FAILURE 31L
#define ERROR_NEGATIVE_SEEK 131L
#define ERROR_IO_DEVICE 1117L
#define ERROR_NOT_LOGGED_ON 1245L
#define ERROR_MR_MID_NOT_FOUND 317L
#define ERROR_PARTIAL_COPY 299L
#define ERROR_INVALID_ADDRESS 487L
#define ERROR_SEM_TIMEOUT 121L
#define ERROR_PIPE_NOT_CONNECTED 233L
#define ERROR_NO_DATA 232L

#define STATUS_WAIT_0 ((DWORD)0x00000000L)
#define STATUS_ABANDONED_WAIT_0 ((DWORD)0x00000080L)
#define STATUS_TIMEOUT ((DWORD)0x00000102L)
#define STATUS_PENDING ((DWORD)0x00000103L)
#define STATUS_GUARD_PAGE_VIOLATION ((DWORD)0x80000001L)
#define STATUS_BREAKPOINT ((DWORD)0x80000003L)
#define STATUS_SINGLE_STEP ((DWORD)0x80000004L)
#define STATUS_ACCESS_VIOLATION ((DWORD)0xC0000005L)
#define STATUS_IN_PAGE_ERROR ((DWORD)0xC0000006L)
#define STATUS_ILLEGAL_INSTRUCTION ((DWORD)0xC000001DL)
#define STATUS_NONCONTINUABLE_EXCEPTION ((DWORD)0xC0000025L)
#define STATUS_ARRAY_BOUNDS_EXCEEDED ((DWORD)0xC000008CL)
#define STATUS_FLOAT_DENORMAL_OPERAND ((DWORD)0xC000008DL)
#define STATUS_FLOAT_DIVIDE_BY_ZERO ((DWORD)0xC000008EL)
#define STATUS_FLOAT_INEXACT_RESULT ((DWORD)0xC000008FL)
#define STATUS_FLOAT_INVALID_OPERATION ((DWORD)0xC0000090L)
#define STATUS_FLOAT_OVERFLOW ((DWORD)0xC0000091L)
#define STATUS_FLOAT_STACK_CHECK ((DWORD)0xC0000092L)
#define STATUS_FLOAT_UNDERFLOW ((DWORD)0xC0000093L)
#define STATUS_INTEGER_DIVIDE_BY_ZERO ((DWORD)0xC0000094L)
#define STATUS_INTEGER_OVERFLOW ((DWORD)0xC0000095L)
#define STATUS_PRIVILEGED_INSTRUCTION ((DWORD)0xC0000096L)
#define STATUS_STACK_OVERFLOW ((DWORD)0xC00000FDL)
#define STATUS_HEAP_CORRUPTION ((DWORD)0xC0000374L)
#define STATUS_STACK_BUFFER_OVERRUN ((DWORD)0xC0000409L)
#define STATUS_INVALID_HANDLE ((DWORD)0xC0000008L)
#define STATUS_FLOAT_MULTIPLE_FAULTS ((DWORD)0xC00002B4L)
#define STATUS_FLOAT_MULTIPLE_TRAPS ((DWORD)0xC00002B5L)
#define EXCEPTION_ACCESS_VIOLATION STATUS_ACCESS_VIOLATION
#define EXCEPTION_DATATYPE_MISALIGNMENT ((DWORD)0x80000002L)
#define EXCEPTION_BREAKPOINT STATUS_BREAKPOINT
#define EXCEPTION_SINGLE_STEP STATUS_SINGLE_STEP
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED STATUS_ARRAY_BOUNDS_EXCEEDED
#define EXCEPTION_FLT_DENORMAL_OPERAND STATUS_FLOAT_DENORMAL_OPERAND
#define EXCEPTION_FLT_DIVIDE_BY_ZERO STATUS_FLOAT_DIVIDE_BY_ZERO
#define EXCEPTION_FLT_INEXACT_RESULT STATUS_FLOAT_INEXACT_RESULT
#define EXCEPTION_FLT_INVALID_OPERATION STATUS_FLOAT_INVALID_OPERATION
#define EXCEPTION_FLT_OVERFLOW STATUS_FLOAT_OVERFLOW
#define EXCEPTION_FLT_STACK_CHECK STATUS_FLOAT_STACK_CHECK
#define EXCEPTION_FLT_UNDERFLOW STATUS_FLOAT_UNDERFLOW
#define EXCEPTION_INT_DIVIDE_BY_ZERO STATUS_INTEGER_DIVIDE_BY_ZERO
#define EXCEPTION_INT_OVERFLOW STATUS_INTEGER_OVERFLOW
#define EXCEPTION_PRIV_INSTRUCTION STATUS_PRIVILEGED_INSTRUCTION
#define EXCEPTION_IN_PAGE_ERROR STATUS_IN_PAGE_ERROR
#define EXCEPTION_ILLEGAL_INSTRUCTION STATUS_ILLEGAL_INSTRUCTION
#define EXCEPTION_NONCONTINUABLE_EXCEPTION STATUS_NONCONTINUABLE_EXCEPTION
#define EXCEPTION_STACK_OVERFLOW STATUS_STACK_OVERFLOW
#define EXCEPTION_GUARD_PAGE STATUS_GUARD_PAGE_VIOLATION
#define EXCEPTION_INVALID_HANDLE STATUS_INVALID_HANDLE
#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

#define FACILITY_WIN32 7
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) : ((HRESULT)(((x) & 0x0000FFFF) | (FACILITY_WIN32 << 16) | 0x80000000)))
#define HRESULT_CODE(hr) ((hr) & 0xFFFF)
#define E_UNEXPECTED ((HRESULT)0x8000FFFFL)
#define E_ACCESSDENIED ((HRESULT)0x80070005L)
#define E_HANDLE ((HRESULT)0x80070006L)
#define E_ABORT ((HRESULT)0x80004004L)
#define E_PENDING ((HRESULT)0x8000000AL)
#define CO_E_NOTINITIALIZED ((HRESULT)0x800401F0L)
#define RPC_E_CHANGED_MODE ((HRESULT)0x80010106L)
#define AUDCLNT_E_DEVICE_INVALIDATED ((HRESULT)0x88890004L)

extern "C" DWORD GetLastError(void);
extern "C" void SetLastError(DWORD);

// ---------------------------------------------------------------------------------
// Structured exception handling
//
// __try/__except is MSVC-only. The runtime uses it to log a crash before the process
// dies; here the guarded block runs inside a C++ try and the handler body only runs for
// C++ exceptions (the filter expression is dropped). Hardware faults are seen by the
// vectored handlers instead (AddVectoredExceptionHandler, built on sigaction).
// (libstdc++ itself spells "#define __try try", so this definition is compatible.)
// ---------------------------------------------------------------------------------
#define __try try
#define __except(filter) catch (...)
#define GetExceptionCode() ((DWORD)0)
#define GetExceptionInformation() ((struct _EXCEPTION_POINTERS*)nullptr)

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct _M128A { ULONGLONG Low; LONGLONG High; } M128A, *PM128A;
// x86-64 shaped CONTEXT so the diagnostic code compiles. On ARM64 the shim fills
// Rip = pc, Rsp = sp, Rbp = x29 and Rax.. = x0..; the ARM64 registers are in X[].
typedef struct _CONTEXT {
    DWORD64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    DWORD ContextFlags, MxCsr;
    WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD EFlags;
    DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    DWORD64 R8, R9, R10, R11, R12, R13, R14, R15;
    DWORD64 Rip;
    struct { WORD ControlWord, StatusWord; BYTE TagWord, Reserved1; WORD ErrorOpcode; DWORD ErrorOffset; WORD ErrorSelector, Reserved2; DWORD DataOffset; WORD DataSelector, Reserved3; DWORD MxCsr, MxCsr_Mask; M128A FloatRegisters[8]; M128A XmmRegisters[16]; BYTE Reserved4[96]; } FltSave;
    M128A Xmm[16];
    DWORD64 X[31], Sp, Pc, Lr, Fp;
    DWORD Cpsr;
} CONTEXT, *PCONTEXT, *LPCONTEXT;
#define CONTEXT_AMD64 0x00100000L
#define CONTEXT_CONTROL (CONTEXT_AMD64 | 0x1L)
#define CONTEXT_INTEGER (CONTEXT_AMD64 | 0x2L)
#define CONTEXT_SEGMENTS (CONTEXT_AMD64 | 0x4L)
#define CONTEXT_FLOATING_POINT (CONTEXT_AMD64 | 0x8L)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_AMD64 | 0x10L)
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)
#define CONTEXT_ALL (CONTEXT_FULL | CONTEXT_SEGMENTS | CONTEXT_DEBUG_REGISTERS)

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(struct _EXCEPTION_POINTERS*);
typedef LONG (*PTOP_LEVEL_EXCEPTION_FILTER)(struct _EXCEPTION_POINTERS*);
typedef PTOP_LEVEL_EXCEPTION_FILTER LPTOP_LEVEL_EXCEPTION_FILTER;

extern "C" PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
extern "C" ULONG RemoveVectoredExceptionHandler(PVOID handle);
extern "C" PVOID AddVectoredContinueHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
extern "C" ULONG RemoveVectoredContinueHandler(PVOID handle);
extern "C" LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
extern "C" LONG UnhandledExceptionFilter(struct _EXCEPTION_POINTERS*);
extern "C" void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args);
extern "C" void RtlCaptureContext(PCONTEXT context);
extern "C" WORD RtlCaptureStackBackTrace(DWORD skip, DWORD count, PVOID* trace, PDWORD hash);
#define CaptureStackBackTrace RtlCaptureStackBackTrace
extern "C" void GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high);
extern "C" BOOL ReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T* read);
extern "C" BOOL WriteProcessMemory(HANDLE process, LPVOID address, LPCVOID buffer, SIZE_T size, SIZE_T* written);
extern "C" BOOL QueryFullProcessImageNameW(HANDLE process, DWORD flags, LPWSTR name, PDWORD size);
extern "C" BOOL QueryFullProcessImageNameA(HANDLE process, DWORD flags, LPSTR name, PDWORD size);
extern "C" BOOL IsDebuggerPresent(void);
extern "C" void DebugBreak(void);
extern "C" void OutputDebugStringA(LPCSTR);
extern "C" void OutputDebugStringW(LPCWSTR);
extern "C" UINT SetErrorMode(UINT mode);
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SEM_NOOPENFILEERRORBOX 0x8000

// x64 unwind tables (PE only; the diagnostics that walk them compile to no-ops)
typedef struct _RUNTIME_FUNCTION { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;
typedef struct _UNWIND_HISTORY_TABLE { DWORD Count; } UNWIND_HISTORY_TABLE, *PUNWIND_HISTORY_TABLE;
typedef struct _KNONVOLATILE_CONTEXT_POINTERS { PVOID dummy; } KNONVOLATILE_CONTEXT_POINTERS, *PKNONVOLATILE_CONTEXT_POINTERS;
typedef LONG (*PEXCEPTION_ROUTINE)(void*, void*, void*, void*);
#define UNW_FLAG_NHANDLER 0x0
#define UNW_FLAG_EHANDLER 0x1
#define UNW_FLAG_UHANDLER 0x2
extern "C" PRUNTIME_FUNCTION RtlLookupFunctionEntry(DWORD64 pc, PDWORD64 imageBase, PUNWIND_HISTORY_TABLE history);
extern "C" PEXCEPTION_ROUTINE RtlVirtualUnwind(DWORD type, DWORD64 imageBase, DWORD64 pc, PRUNTIME_FUNCTION fn, PCONTEXT ctx,
                                               PVOID* handlerData, PDWORD64 establisher, PKNONVOLATILE_CONTEXT_POINTERS nv);

typedef struct _IMAGE_DOS_HEADER { WORD e_magic; WORD e_pad[29]; LONG e_lfanew; } IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;
typedef struct _IMAGE_FILE_HEADER {
    WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    WORD SizeOfOptionalHeader, Characteristics;
} IMAGE_FILE_HEADER;
typedef struct _IMAGE_DATA_DIRECTORY { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct _IMAGE_OPTIONAL_HEADER64 {
    WORD Magic; BYTE MajorLinkerVersion, MinorLinkerVersion;
    DWORD SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode;
    ULONGLONG ImageBase; DWORD SectionAlignment, FileAlignment;
    WORD MajorOperatingSystemVersion, MinorOperatingSystemVersion, MajorImageVersion, MinorImageVersion;
    WORD MajorSubsystemVersion, MinorSubsystemVersion; DWORD Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    WORD Subsystem, DllCharacteristics;
    ULONGLONG SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    DWORD LoaderFlags, NumberOfRvaAndSizes; IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER64;
typedef struct _IMAGE_NT_HEADERS64 { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64, IMAGE_NT_HEADERS, *PIMAGE_NT_HEADERS;
typedef struct _IMAGE_SECTION_HEADER {
    BYTE Name[8]; union { DWORD PhysicalAddress; DWORD VirtualSize; } Misc;
    DWORD VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations, PointerToLinenumbers;
    WORD NumberOfRelocations, NumberOfLinenumbers; DWORD Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;
#define IMAGE_FIRST_SECTION(nt) ((PIMAGE_SECTION_HEADER)((ULONG_PTR)(nt) + FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) + (nt)->FileHeader.SizeOfOptionalHeader))
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_CNT_CODE 0x00000020

// ---------------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------------
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#define MEM_RESET 0x00080000
#define MEM_TOP_DOWN 0x00100000
#define MEM_WRITE_WATCH 0x00200000
#define MEM_PHYSICAL 0x00400000
#define MEM_LARGE_PAGES 0x20000000
#define MEM_IMAGE 0x01000000
#define WRITE_WATCH_FLAG_RESET 0x01
#define SEC_COMMIT 0x08000000
#define FILE_MAP_COPY 0x0001
#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_ALL_ACCESS 0x000F001F

extern "C" LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
extern "C" BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type);
extern "C" BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD oldProtect);
extern "C" SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length);
extern "C" BOOL VirtualLock(LPVOID address, SIZE_T size);
extern "C" BOOL VirtualUnlock(LPVOID address, SIZE_T size);
extern "C" UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID* addresses, ULONG_PTR* count, LPDWORD granularity);
extern "C" UINT ResetWriteWatch(LPVOID base, SIZE_T size);
extern "C" BOOL FlushInstructionCache(HANDLE process, LPCVOID address, SIZE_T size);
extern "C" BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX status);
extern "C" void GetSystemInfo(LPSYSTEM_INFO info);
extern "C" void GetNativeSystemInfo(LPSYSTEM_INFO info);
extern "C" HANDLE GetProcessHeap(void);
extern "C" LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
extern "C" LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T size);
extern "C" BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem);
extern "C" HLOCAL LocalAlloc(UINT flags, SIZE_T size);
extern "C" HLOCAL LocalFree(HLOCAL mem);
extern "C" HGLOBAL GlobalAlloc(UINT flags, SIZE_T size);
extern "C" HGLOBAL GlobalFree(HGLOBAL mem);
#define HEAP_ZERO_MEMORY 0x00000008
#define LMEM_FIXED 0x0000
#define LMEM_ZEROINIT 0x0040
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)
#define GMEM_FIXED 0x0000
#define GMEM_ZEROINIT 0x0040
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
#define ZeroMemory(p, n) std::memset((p), 0, (n))
#define SecureZeroMemory(p, n) std::memset((p), 0, (n))
#define RtlZeroMemory(p, n) std::memset((p), 0, (n))
#define CopyMemory(d, s, n) std::memcpy((d), (s), (n))
#define MoveMemory(d, s, n) std::memmove((d), (s), (n))
#define FillMemory(d, n, v) std::memset((d), (v), (n))
#define RtlCopyMemory(d, s, n) std::memcpy((d), (s), (n))
#define RtlMoveMemory(d, s, n) std::memmove((d), (s), (n))
inline void* _aligned_malloc(size_t size, size_t align) { void* p = nullptr; if (align < sizeof(void*)) align = sizeof(void*); return posix_memalign(&p, align, size ? size : 1) == 0 ? p : nullptr; }
inline void _aligned_free(void* p) { free(p); }
inline size_t _msize(void*) { return 0; }

extern "C" HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD sizeHigh, DWORD sizeLow, LPCWSTR name);
extern "C" HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD sizeHigh, DWORD sizeLow, LPCSTR name);
extern "C" LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T size);
extern "C" BOOL UnmapViewOfFile(LPCVOID address);
extern "C" BOOL FlushViewOfFile(LPCVOID address, SIZE_T size);

// ---------------------------------------------------------------------------------
// Handles, processes, threads
// ---------------------------------------------------------------------------------
extern "C" BOOL CloseHandle(HANDLE h);
extern "C" BOOL DuplicateHandle(HANDLE srcProcess, HANDLE src, HANDLE dstProcess, LPHANDLE dst, DWORD access, BOOL inherit, DWORD options);
extern "C" BOOL SetHandleInformation(HANDLE h, DWORD mask, DWORD flags);
extern "C" BOOL GetHandleInformation(HANDLE h, LPDWORD flags);
#define DUPLICATE_CLOSE_SOURCE 0x00000001
#define DUPLICATE_SAME_ACCESS 0x00000002
#define HANDLE_FLAG_INHERIT 0x00000001
#define HANDLE_FLAG_PROTECT_FROM_CLOSE 0x00000002

extern "C" HANDLE GetCurrentProcess(void);
extern "C" DWORD GetCurrentProcessId(void);
extern "C" HANDLE GetCurrentThread(void);
extern "C" DWORD GetCurrentThreadId(void);
extern "C" DWORD GetThreadId(HANDLE thread);
extern "C" DWORD GetProcessId(HANDLE process);
extern "C" DWORD GetCurrentProcessorNumber(void);
extern "C" DWORD GetActiveProcessorCount(WORD group);
extern "C" DWORD GetMaximumProcessorCount(WORD group);
#define ALL_PROCESSOR_GROUPS 0xffff
extern "C" [[noreturn]] void ExitProcess(UINT code);
extern "C" BOOL TerminateProcess(HANDLE process, UINT code);
extern "C" BOOL GetExitCodeProcess(HANDLE process, LPDWORD code);
extern "C" BOOL GetExitCodeThread(HANDLE thread, LPDWORD code);
extern "C" BOOL TerminateThread(HANDLE thread, DWORD code);
extern "C" [[noreturn]] void ExitThread(DWORD code);
#define STILL_ACTIVE 259

typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef LPTHREAD_START_ROUTINE PTHREAD_START_ROUTINE;
extern "C" HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPDWORD id);
extern "C" HANDLE OpenThread(DWORD access, BOOL inherit, DWORD id);
extern "C" HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD id);
extern "C" DWORD ResumeThread(HANDLE thread);
extern "C" DWORD SuspendThread(HANDLE thread);
extern "C" BOOL GetThreadContext(HANDLE thread, LPCONTEXT context);
extern "C" BOOL SetThreadContext(HANDLE thread, const CONTEXT* context);
extern "C" BOOL SetThreadPriority(HANDLE thread, int priority);
extern "C" int GetThreadPriority(HANDLE thread);
extern "C" BOOL SetThreadPriorityBoost(HANDLE thread, BOOL disable);
extern "C" DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
extern "C" BOOL SetProcessAffinityMask(HANDLE process, DWORD_PTR mask);
extern "C" BOOL GetProcessAffinityMask(HANDLE process, PDWORD_PTR processMask, PDWORD_PTR systemMask);
extern "C" DWORD SetThreadIdealProcessor(HANDLE thread, DWORD ideal);
extern "C" HRESULT SetThreadDescription(HANDLE thread, PCWSTR name);
extern "C" HRESULT GetThreadDescription(HANDLE thread, PWSTR* name);
extern "C" BOOL SetPriorityClass(HANDLE process, DWORD cls);
extern "C" DWORD GetPriorityClass(HANDLE process);
extern "C" BOOL SwitchToThread(void);
extern "C" void Sleep(DWORD ms);
extern "C" DWORD SleepEx(DWORD ms, BOOL alertable);
extern "C" BOOL GetThreadTimes(HANDLE thread, LPFILETIME creation, LPFILETIME exitTime, LPFILETIME kernel, LPFILETIME user);
extern "C" BOOL GetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exitTime, LPFILETIME kernel, LPFILETIME user);
extern "C" BOOL QueryThreadCycleTime(HANDLE thread, PULONG64 cycles);
extern "C" BOOL QueryProcessCycleTime(HANDLE process, PULONG64 cycles);
extern "C" BOOL QueryIdleProcessorCycleTime(PULONG size, PULONG64 times);
extern "C" BOOL GetProcessHandleCount(HANDLE process, PDWORD count);
extern "C" BOOL IsWow64Process(HANDLE process, PBOOL wow);
extern "C" DWORD TlsAlloc(void);
extern "C" BOOL TlsFree(DWORD index);
extern "C" LPVOID TlsGetValue(DWORD index);
extern "C" BOOL TlsSetValue(DWORD index, LPVOID value);
#define TLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
#define CREATE_SUSPENDED 0x00000004
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_ERROR_RETURN (MAXLONG)
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#define THREAD_MODE_BACKGROUND_END 0x00020000
#define NORMAL_PRIORITY_CLASS 0x00000020
#define IDLE_PRIORITY_CLASS 0x00000040
#define HIGH_PRIORITY_CLASS 0x00000080
#define REALTIME_PRIORITY_CLASS 0x00000100
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#define PROCESS_MODE_BACKGROUND_BEGIN 0x00100000
#define PROCESS_MODE_BACKGROUND_END 0x00200000
#define SYNCHRONIZE 0x00100000L
#define STANDARD_RIGHTS_REQUIRED 0x000F0000L
#define DELETE 0x00010000L
#define READ_CONTROL 0x00020000L
#define WRITE_DAC 0x00040000L
#define WRITE_OWNER 0x00080000L
#define THREAD_TERMINATE 0x0001
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_SET_CONTEXT 0x0010
#define THREAD_QUERY_INFORMATION 0x0040
#define THREAD_SET_INFORMATION 0x0020
#define THREAD_SET_LIMITED_INFORMATION 0x0400
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#define THREAD_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF)
#define PROCESS_TERMINATE 0x0001
#define PROCESS_VM_READ 0x0010
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_SET_INFORMATION 0x0200
#define PROCESS_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF)

// Process power throttling / information classes (no-ops)
typedef enum _PROCESS_INFORMATION_CLASS { ProcessMemoryPriority, ProcessMemoryExhaustionInfo, ProcessAppMemoryInfo,
    ProcessInPrivateInfo, ProcessPowerThrottling, ProcessReservedValue1, ProcessTelemetryCoverageInfo,
    ProcessProtectionLevelInfo, ProcessLeapSecondInfo, ProcessMachineTypeInfo, ProcessInformationClassMax } PROCESS_INFORMATION_CLASS;
typedef enum _THREAD_INFORMATION_CLASS { ThreadMemoryPriority, ThreadAbsoluteCpuPriority, ThreadDynamicCodePolicy,
    ThreadPowerThrottling, ThreadInformationClassMax } THREAD_INFORMATION_CLASS;
typedef struct _PROCESS_POWER_THROTTLING_STATE { ULONG Version, ControlMask, StateMask; } PROCESS_POWER_THROTTLING_STATE;
typedef struct _THREAD_POWER_THROTTLING_STATE { ULONG Version, ControlMask, StateMask; } THREAD_POWER_THROTTLING_STATE;
typedef struct _MEMORY_PRIORITY_INFORMATION { ULONG MemoryPriority; } MEMORY_PRIORITY_INFORMATION;
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#define THREAD_POWER_THROTTLING_CURRENT_VERSION 1
#define THREAD_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define MEMORY_PRIORITY_NORMAL 5
extern "C" BOOL SetProcessInformation(HANDLE process, PROCESS_INFORMATION_CLASS cls, LPVOID info, DWORD size);
extern "C" BOOL SetThreadInformation(HANDLE thread, THREAD_INFORMATION_CLASS cls, LPVOID info, DWORD size);
extern "C" BOOL SetProcessPriorityBoost(HANDLE process, BOOL disable);
extern "C" BOOL SetProcessWorkingSetSize(HANDLE process, SIZE_T minSize, SIZE_T maxSize);
extern "C" EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE flags);
#define ES_SYSTEM_REQUIRED 0x00000001
#define ES_DISPLAY_REQUIRED 0x00000002
#define ES_CONTINUOUS 0x80000000

typedef enum _LOGICAL_PROCESSOR_RELATIONSHIP { RelationProcessorCore, RelationNumaNode, RelationCache,
    RelationProcessorPackage, RelationGroup, RelationProcessorDie, RelationNumaNodeEx, RelationProcessorModule, RelationAll = 0xffff } LOGICAL_PROCESSOR_RELATIONSHIP;
typedef struct _GROUP_AFFINITY { KAFFINITY Mask; WORD Group; WORD Reserved[3]; } GROUP_AFFINITY, *PGROUP_AFFINITY;
typedef struct _PROCESSOR_RELATIONSHIP { BYTE Flags, EfficiencyClass; BYTE Reserved[20]; WORD GroupCount; GROUP_AFFINITY GroupMask[1]; } PROCESSOR_RELATIONSHIP;
typedef struct _CACHE_RELATIONSHIP { BYTE Level, Associativity; WORD LineSize; DWORD CacheSize; int Type; BYTE Reserved[18]; WORD GroupCount; GROUP_AFFINITY GroupMask; } CACHE_RELATIONSHIP;
typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship; DWORD Size;
    union { PROCESSOR_RELATIONSHIP Processor; CACHE_RELATIONSHIP Cache; BYTE raw[64]; };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;
extern "C" BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD length);
extern "C" BOOL SetThreadGroupAffinity(HANDLE thread, const GROUP_AFFINITY* affinity, PGROUP_AFFINITY previous);
extern "C" BOOL GetThreadGroupAffinity(HANDLE thread, PGROUP_AFFINITY affinity);
typedef struct _PROCESSOR_NUMBER { WORD Group; BYTE Number, Reserved; } PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;
extern "C" void GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER n);
extern "C" BOOL SetThreadSelectedCpuSets(HANDLE thread, const ULONG* ids, ULONG count);
extern "C" BOOL GetSystemCpuSetInformation(void* info, ULONG length, PULONG returned, HANDLE process, ULONG flags);

// Child processes (none on Android: creation fails cleanly)
typedef struct _STARTUPINFOW {
    DWORD cb; LPWSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;
typedef struct _STARTUPINFOA {
    DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef struct _PROC_THREAD_ATTRIBUTE_LIST *PPROC_THREAD_ATTRIBUTE_LIST, *LPPROC_THREAD_ATTRIBUTE_LIST;
typedef struct _STARTUPINFOEXW { STARTUPINFOW StartupInfo; LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList; } STARTUPINFOEXW, *LPSTARTUPINFOEXW;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
#define STARTF_USESHOWWINDOW 0x00000001
#define STARTF_USESTDHANDLES 0x00000100
#define CREATE_NO_WINDOW 0x08000000
#define CREATE_NEW_CONSOLE 0x00000010
#define CREATE_UNICODE_ENVIRONMENT 0x00000400
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#define DETACHED_PROCESS 0x00000008
#define CREATE_BREAKAWAY_FROM_JOB 0x01000000
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
extern "C" BOOL CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                               DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi);
extern "C" BOOL CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                               DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi);
extern "C" BOOL InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD count, DWORD flags, PSIZE_T size);
extern "C" BOOL UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags, DWORD_PTR attr, PVOID value, SIZE_T size, PVOID prev, PSIZE_T retSize);
extern "C" void DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST list);
extern "C" BOOL CreatePipe(PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES sa, DWORD size);
extern "C" BOOL PeekNamedPipe(HANDLE pipe, LPVOID buffer, DWORD size, LPDWORD read, LPDWORD avail, LPDWORD left);
extern "C" HANDLE CreateJobObjectW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name);
extern "C" BOOL AssignProcessToJobObject(HANDLE job, HANDLE process);
typedef enum _JOBOBJECTINFOCLASS { JobObjectBasicLimitInformation = 2, JobObjectExtendedLimitInformation = 9 } JOBOBJECTINFOCLASS;
typedef struct _JOBOBJECT_BASIC_LIMIT_INFORMATION {
    LARGE_INTEGER PerProcessUserTimeLimit, PerJobUserTimeLimit; DWORD LimitFlags; SIZE_T MinimumWorkingSetSize, MaximumWorkingSetSize;
    DWORD ActiveProcessLimit; ULONG_PTR Affinity; DWORD PriorityClass, SchedulingClass;
} JOBOBJECT_BASIC_LIMIT_INFORMATION;
typedef struct _IO_COUNTERS { ULONGLONG ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount; } IO_COUNTERS;
typedef struct _JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
    JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation; IO_COUNTERS IoInfo;
    SIZE_T ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
} JOBOBJECT_EXTENDED_LIMIT_INFORMATION;
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
#define JOB_OBJECT_LIMIT_BREAKAWAY_OK 0x00000800
extern "C" BOOL SetInformationJobObject(HANDLE job, JOBOBJECTINFOCLASS cls, LPVOID info, DWORD size);

// Toolhelp (no snapshot on Android)
#define TH32CS_SNAPTHREAD 0x00000004
#define TH32CS_SNAPPROCESS 0x00000002
#define TH32CS_SNAPMODULE 0x00000008
typedef struct tagTHREADENTRY32 { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32, *LPTHREADENTRY32;
extern "C" HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
extern "C" BOOL Thread32First(HANDLE snap, LPTHREADENTRY32 entry);
extern "C" BOOL Thread32Next(HANDLE snap, LPTHREADENTRY32 entry);

// Modules
extern "C" HMODULE GetModuleHandleW(LPCWSTR name);
extern "C" HMODULE GetModuleHandleA(LPCSTR name);
extern "C" BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module);
extern "C" BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module);
extern "C" DWORD GetModuleFileNameW(HMODULE module, LPWSTR name, DWORD size);
extern "C" DWORD GetModuleFileNameA(HMODULE module, LPSTR name, DWORD size);
extern "C" HMODULE LoadLibraryW(LPCWSTR name);
extern "C" HMODULE LoadLibraryA(LPCSTR name);
extern "C" HMODULE LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);
extern "C" HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
extern "C" BOOL FreeLibrary(HMODULE module);
extern "C" FARPROC GetProcAddress(HMODULE module, LPCSTR name);
extern "C" BOOL SetDllDirectoryW(LPCWSTR dir);
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#define LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x00000200
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#define LOAD_LIBRARY_AS_DATAFILE 0x00000002
typedef struct _MODULEINFO { LPVOID lpBaseOfDll; DWORD SizeOfImage; LPVOID EntryPoint; } MODULEINFO, *LPMODULEINFO;
extern "C" BOOL GetModuleInformation(HANDLE process, HMODULE module, LPMODULEINFO info, DWORD size);
typedef struct _PROCESS_MEMORY_COUNTERS {
    DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS, *PPROCESS_MEMORY_COUNTERS;
typedef struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX;
extern "C" BOOL GetProcessMemoryInfo(HANDLE process, PPROCESS_MEMORY_COUNTERS counters, DWORD size);
#define K32GetProcessMemoryInfo GetProcessMemoryInfo

// ---------------------------------------------------------------------------------
// Synchronization
// ---------------------------------------------------------------------------------
#define WAIT_OBJECT_0 0x00000000L
#define WAIT_ABANDONED 0x00000080L
#define WAIT_ABANDONED_0 0x00000080L
#define WAIT_IO_COMPLETION 0x000000C0L
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define MAXIMUM_WAIT_OBJECTS 64
#define CREATE_EVENT_MANUAL_RESET 0x00000001
#define CREATE_EVENT_INITIAL_SET 0x00000002
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define EVENT_ALL_ACCESS 0x1F0003
#define EVENT_MODIFY_STATE 0x0002
#define SEMAPHORE_ALL_ACCESS 0x1F0003
#define MUTEX_ALL_ACCESS 0x1F0001
#define TIMER_ALL_ACCESS 0x1F0003
#define TIMER_MODIFY_STATE 0x0002

extern "C" HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name);
extern "C" HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name);
extern "C" HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
extern "C" HANDLE CreateEventExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access);
extern "C" BOOL SetEvent(HANDLE h);
extern "C" BOOL ResetEvent(HANDLE h);
extern "C" BOOL PulseEvent(HANDLE h);
extern "C" HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name);
extern "C" HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name);
extern "C" HANDLE CreateSemaphoreExW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name, DWORD flags, DWORD access);
extern "C" BOOL ReleaseSemaphore(HANDLE h, LONG count, LPLONG previous);
extern "C" HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name);
extern "C" HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name);
extern "C" BOOL ReleaseMutex(HANDLE h);
extern "C" HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name);
extern "C" HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
typedef void (*PTIMERAPCROUTINE)(LPVOID, DWORD, DWORD);
extern "C" BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE fn, LPVOID arg, BOOL resume);
extern "C" BOOL SetWaitableTimerEx(HANDLE timer, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE fn, LPVOID arg, void* reason, ULONG delay);
extern "C" BOOL CancelWaitableTimer(HANDLE timer);
extern "C" DWORD WaitForSingleObject(HANDLE h, DWORD ms);
extern "C" DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
extern "C" DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms);
extern "C" DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL alertable);
extern "C" DWORD SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD ms, BOOL alertable);
extern "C" DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, DWORD mask);
typedef void (*PAPCFUNC)(ULONG_PTR);
extern "C" DWORD QueueUserAPC(PAPCFUNC fn, HANDLE thread, ULONG_PTR data);

extern "C" BOOL WaitOnAddress(volatile void* address, PVOID compare, SIZE_T size, DWORD ms);
extern "C" void WakeByAddressSingle(PVOID address);
extern "C" void WakeByAddressAll(PVOID address);

// CRITICAL_SECTION: recursive mutex. Storage holds a pthread_mutex_t (40 bytes on bionic arm64).
typedef struct _RTL_CRITICAL_SECTION {
    alignas(16) unsigned char storage[64];
    LONG LockCount; LONG RecursionCount; HANDLE OwningThread; HANDLE LockSemaphore; ULONG_PTR SpinCount;
} CRITICAL_SECTION, *PCRITICAL_SECTION, *LPCRITICAL_SECTION, RTL_CRITICAL_SECTION;
extern "C" void InitializeCriticalSection(LPCRITICAL_SECTION cs);
extern "C" BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
extern "C" BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags);
extern "C" void DeleteCriticalSection(LPCRITICAL_SECTION cs);
extern "C" void EnterCriticalSection(LPCRITICAL_SECTION cs);
extern "C" BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);
extern "C" void LeaveCriticalSection(LPCRITICAL_SECTION cs);
extern "C" DWORD SetCriticalSectionSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
#define CRITICAL_SECTION_NO_DEBUG_INFO 0x01000000

// SRWLOCK / CONDITION_VARIABLE / INIT_ONCE: zero-initialisable, lazily backed.
typedef struct _RTL_SRWLOCK { PVOID Ptr; } SRWLOCK, *PSRWLOCK, RTL_SRWLOCK;
#define SRWLOCK_INIT {0}
#define RTL_SRWLOCK_INIT {0}
extern "C" void InitializeSRWLock(PSRWLOCK l);
extern "C" void AcquireSRWLockExclusive(PSRWLOCK l);
extern "C" void ReleaseSRWLockExclusive(PSRWLOCK l);
extern "C" void AcquireSRWLockShared(PSRWLOCK l);
extern "C" void ReleaseSRWLockShared(PSRWLOCK l);
extern "C" BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l);
extern "C" BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l);
typedef struct _RTL_CONDITION_VARIABLE { PVOID Ptr; } CONDITION_VARIABLE, *PCONDITION_VARIABLE, RTL_CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {0}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1
extern "C" void InitializeConditionVariable(PCONDITION_VARIABLE cv);
extern "C" BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms);
extern "C" BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags);
extern "C" void WakeConditionVariable(PCONDITION_VARIABLE cv);
extern "C" void WakeAllConditionVariable(PCONDITION_VARIABLE cv);
typedef union _RTL_RUN_ONCE { PVOID Ptr; } INIT_ONCE, *PINIT_ONCE, *LPINIT_ONCE, RTL_RUN_ONCE;
#define INIT_ONCE_STATIC_INIT {0}
typedef BOOL (*PINIT_ONCE_FN)(PINIT_ONCE, PVOID, PVOID*);
extern "C" BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID* context);
typedef struct _RTL_BARRIER { DWORD Reserved1, Reserved2; ULONG_PTR Reserved3[2]; DWORD Reserved4, Reserved5; } SYNCHRONIZATION_BARRIER;

// Interlocked (Windows semantics: full barrier, returns new/old value as documented)
inline LONG InterlockedIncrement(volatile LONG* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedDecrement(volatile LONG* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchangeAdd(volatile LONG* p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedAdd(volatile LONG* p, LONG v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedOr(volatile LONG* p, LONG v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedAnd(volatile LONG* p, LONG v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedXor(volatile LONG* p, LONG v) { return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c) { __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
inline LONG64 InterlockedIncrement64(volatile LONG64* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedDecrement64(volatile LONG64* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedExchange64(volatile LONG64* p, LONG64 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedExchangeAdd64(volatile LONG64* p, LONG64 v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedAdd64(volatile LONG64* p, LONG64 v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedOr64(volatile LONG64* p, LONG64 v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedAnd64(volatile LONG64* p, LONG64 v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedCompareExchange64(volatile LONG64* p, LONG64 x, LONG64 c) { __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
inline PVOID InterlockedExchangePointer(PVOID volatile* p, PVOID v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline PVOID InterlockedCompareExchangePointer(PVOID volatile* p, PVOID x, PVOID c) { __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
inline SHORT InterlockedIncrement16(volatile SHORT* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline SHORT InterlockedDecrement16(volatile SHORT* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline SHORT InterlockedCompareExchange16(volatile SHORT* p, SHORT x, SHORT c) { __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
inline char InterlockedExchange8(volatile char* p, char v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline char InterlockedOr8(volatile char* p, char v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
inline char InterlockedAnd8(volatile char* p, char v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
inline void MemoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _ReadBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _WriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
// MSVC intrinsic spellings (also used directly by the runtime)
#define _InterlockedIncrement(p) InterlockedIncrement((volatile LONG*)(p))
#define _InterlockedDecrement(p) InterlockedDecrement((volatile LONG*)(p))
#define _InterlockedExchange(p, v) InterlockedExchange((volatile LONG*)(p), (LONG)(v))
#define _InterlockedExchangeAdd(p, v) InterlockedExchangeAdd((volatile LONG*)(p), (LONG)(v))
#define _InterlockedCompareExchange(p, x, c) InterlockedCompareExchange((volatile LONG*)(p), (LONG)(x), (LONG)(c))
#define _InterlockedOr(p, v) InterlockedOr((volatile LONG*)(p), (LONG)(v))
#define _InterlockedAnd(p, v) InterlockedAnd((volatile LONG*)(p), (LONG)(v))
#define _InterlockedXor(p, v) InterlockedXor((volatile LONG*)(p), (LONG)(v))
#define _InterlockedIncrement64(p) InterlockedIncrement64((volatile LONG64*)(p))
#define _InterlockedDecrement64(p) InterlockedDecrement64((volatile LONG64*)(p))
#define _InterlockedExchange64(p, v) InterlockedExchange64((volatile LONG64*)(p), (LONG64)(v))
#define _InterlockedExchangeAdd64(p, v) InterlockedExchangeAdd64((volatile LONG64*)(p), (LONG64)(v))
#define _InterlockedCompareExchange64(p, x, c) InterlockedCompareExchange64((volatile LONG64*)(p), (LONG64)(x), (LONG64)(c))
#define _InterlockedOr64(p, v) InterlockedOr64((volatile LONG64*)(p), (LONG64)(v))
#define _InterlockedAnd64(p, v) InterlockedAnd64((volatile LONG64*)(p), (LONG64)(v))
#define _InterlockedExchangePointer(p, v) InterlockedExchangePointer((PVOID volatile*)(p), (PVOID)(v))
#define _InterlockedCompareExchangePointer(p, x, c) InterlockedCompareExchangePointer((PVOID volatile*)(p), (PVOID)(x), (PVOID)(c))
#define _InterlockedIncrement16(p) InterlockedIncrement16((volatile SHORT*)(p))
#define _InterlockedCompareExchange16(p, x, c) InterlockedCompareExchange16((volatile SHORT*)(p), (SHORT)(x), (SHORT)(c))
#define _InterlockedExchange8(p, v) InterlockedExchange8((volatile char*)(p), (char)(v))
#define _InterlockedOr8(p, v) InterlockedOr8((volatile char*)(p), (char)(v))
#define _InterlockedAnd8(p, v) InterlockedAnd8((volatile char*)(p), (char)(v))

// ---------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------
extern "C" BOOL QueryPerformanceCounter(LARGE_INTEGER* counter);
extern "C" BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency);
extern "C" DWORD GetTickCount(void);
extern "C" ULONGLONG GetTickCount64(void);
extern "C" void GetSystemTimeAsFileTime(LPFILETIME ft);
extern "C" void GetSystemTimePreciseAsFileTime(LPFILETIME ft);
extern "C" void GetSystemTime(LPSYSTEMTIME st);
extern "C" void GetLocalTime(LPSYSTEMTIME st);
extern "C" BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st);
extern "C" BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft);
extern "C" BOOL FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME local);
extern "C" BOOL LocalFileTimeToFileTime(const FILETIME* local, LPFILETIME ft);
extern "C" BOOL SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION* tz, const SYSTEMTIME* utc, LPSYSTEMTIME local);
extern "C" DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz);
extern "C" LONG CompareFileTime(const FILETIME* a, const FILETIME* b);
extern "C" BOOL QueryUnbiasedInterruptTime(PULONGLONG t);
extern "C" void QueryInterruptTime(PULONGLONG t);
extern "C" void QueryInterruptTimePrecise(PULONGLONG t);
typedef UINT MMRESULT;
#define TIMERR_NOERROR 0
#define TIMERR_NOCANDO 97
typedef struct timecaps_tag { UINT wPeriodMin, wPeriodMax; } TIMECAPS, *LPTIMECAPS;
extern "C" MMRESULT timeBeginPeriod(UINT period);
extern "C" MMRESULT timeEndPeriod(UINT period);
extern "C" MMRESULT timeGetDevCaps(LPTIMECAPS caps, UINT size);
extern "C" DWORD timeGetTime(void);
extern "C" NTSTATUS NtSetTimerResolution(ULONG desired, BOOLEAN set, PULONG current);
extern "C" NTSTATUS NtQueryTimerResolution(PULONG minimum, PULONG maximum, PULONG current);
extern "C" NTSTATUS NtDelayExecution(BOOLEAN alertable, PLARGE_INTEGER interval);

// ---------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define GENERIC_EXECUTE 0x20000000L
#define GENERIC_ALL 0x10000000L
#define FILE_READ_DATA 0x0001
#define FILE_LIST_DIRECTORY 0x0001
#define FILE_WRITE_DATA 0x0002
#define FILE_APPEND_DATA 0x0004
#define FILE_READ_EA 0x0008
#define FILE_WRITE_EA 0x0010
#define FILE_EXECUTE 0x0020
#define FILE_TRAVERSE 0x0020
#define FILE_READ_ATTRIBUTES 0x0080
#define FILE_WRITE_ATTRIBUTES 0x0100
#define FILE_GENERIC_READ (0x00120089)
#define FILE_GENERIC_WRITE (0x00120116)
#define FILE_ALL_ACCESS (0x001F01FF)
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_DEVICE 0x00000040
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_SPARSE_FILE 0x00000200
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#define FILE_ATTRIBUTE_COMPRESSED 0x00000800
#define FILE_ATTRIBUTE_OFFLINE 0x00001000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_POSIX_SEMANTICS 0x01000000
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define FILE_TYPE_UNKNOWN 0x0000
#define FILE_TYPE_DISK 0x0001
#define FILE_TYPE_CHAR 0x0002
#define FILE_TYPE_PIPE 0x0003
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define MOVEFILE_WRITE_THROUGH 0x00000008
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define LOCKFILE_FAIL_IMMEDIATELY 0x00000001
#define LOCKFILE_EXCLUSIVE_LOCK 0x00000002

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;
typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *PWIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; CHAR cFileName[MAX_PATH]; CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef enum _FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic, FindExInfoMaxInfoLevel } FINDEX_INFO_LEVELS;
typedef enum _FINDEX_SEARCH_OPS { FindExSearchNameMatch, FindExSearchLimitToDirectories, FindExSearchLimitToDevices } FINDEX_SEARCH_OPS;
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002
typedef enum _FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo, FileStandardInfo, FileNameInfo, FileRenameInfo, FileDispositionInfo, FileAllocationInfo,
    FileEndOfFileInfo, FileStreamInfo, FileCompressionInfo, FileAttributeTagInfo, FileIdBothDirectoryInfo,
    FileIdBothDirectoryRestartInfo, FileIoPriorityHintInfo, FileRemoteProtocolInfo, FileFullDirectoryInfo,
    FileFullDirectoryRestartInfo, FileStorageInfo, FileAlignmentInfo, FileIdInfo, FileIdExtdDirectoryInfo,
    FileIdExtdDirectoryRestartInfo, FileDispositionInfoEx, FileRenameInfoEx, MaximumFileInfoByHandleClass
} FILE_INFO_BY_HANDLE_CLASS;
typedef struct _FILE_RENAME_INFO { union { BOOLEAN ReplaceIfExists; DWORD Flags; }; HANDLE RootDirectory; DWORD FileNameLength; WCHAR FileName[1]; } FILE_RENAME_INFO, *PFILE_RENAME_INFO;
typedef struct _FILE_DISPOSITION_INFO { BOOLEAN DeleteFile; } FILE_DISPOSITION_INFO, *PFILE_DISPOSITION_INFO;
typedef struct _FILE_DISPOSITION_INFO_EX { DWORD Flags; } FILE_DISPOSITION_INFO_EX;
typedef struct _FILE_END_OF_FILE_INFO { LARGE_INTEGER EndOfFile; } FILE_END_OF_FILE_INFO, *PFILE_END_OF_FILE_INFO;
typedef struct _FILE_ALLOCATION_INFO { LARGE_INTEGER AllocationSize; } FILE_ALLOCATION_INFO;
typedef struct _FILE_BASIC_INFO { LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime; DWORD FileAttributes; } FILE_BASIC_INFO, *PFILE_BASIC_INFO;
typedef struct _FILE_STANDARD_INFO { LARGE_INTEGER AllocationSize, EndOfFile; DWORD NumberOfLinks; BOOLEAN DeletePending, Directory; } FILE_STANDARD_INFO, *PFILE_STANDARD_INFO;
typedef struct _FILE_ID_INFO { ULONGLONG VolumeSerialNumber; BYTE FileId[16]; } FILE_ID_INFO;
#define FILE_RENAME_FLAG_REPLACE_IF_EXISTS 0x00000001
#define FILE_RENAME_FLAG_POSIX_SEMANTICS 0x00000002
#define FILE_DISPOSITION_FLAG_DELETE 0x00000001
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x00000002

extern "C" HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ);
extern "C" HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ);
extern "C" BOOL ReadFile(HANDLE f, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED ov);
extern "C" BOOL WriteFile(HANDLE f, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED ov);
extern "C" BOOL ReadFileEx(HANDLE f, LPVOID buffer, DWORD size, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE done);
extern "C" BOOL GetOverlappedResult(HANDLE f, LPOVERLAPPED ov, LPDWORD transferred, BOOL wait);
extern "C" BOOL CancelIo(HANDLE f);
extern "C" BOOL CancelIoEx(HANDLE f, LPOVERLAPPED ov);
extern "C" DWORD SetFilePointer(HANDLE f, LONG distance, PLONG distanceHigh, DWORD method);
extern "C" BOOL SetFilePointerEx(HANDLE f, LARGE_INTEGER distance, PLARGE_INTEGER newPos, DWORD method);
extern "C" DWORD GetFileSize(HANDLE f, LPDWORD high);
extern "C" BOOL GetFileSizeEx(HANDLE f, PLARGE_INTEGER size);
extern "C" BOOL SetEndOfFile(HANDLE f);
extern "C" BOOL FlushFileBuffers(HANDLE f);
extern "C" DWORD GetFileType(HANDLE f);
extern "C" BOOL GetFileTime(HANDLE f, LPFILETIME created, LPFILETIME accessed, LPFILETIME written);
extern "C" BOOL SetFileTime(HANDLE f, const FILETIME* created, const FILETIME* accessed, const FILETIME* written);
extern "C" BOOL GetFileInformationByHandle(HANDLE f, LPBY_HANDLE_FILE_INFORMATION info);
extern "C" BOOL GetFileInformationByHandleEx(HANDLE f, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size);
extern "C" BOOL SetFileInformationByHandle(HANDLE f, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size);
extern "C" BOOL LockFileEx(HANDLE f, DWORD flags, DWORD reserved, DWORD lowBytes, DWORD highBytes, LPOVERLAPPED ov);
extern "C" BOOL UnlockFileEx(HANDLE f, DWORD reserved, DWORD lowBytes, DWORD highBytes, LPOVERLAPPED ov);
extern "C" DWORD GetFinalPathNameByHandleW(HANDLE f, LPWSTR path, DWORD size, DWORD flags);
extern "C" DWORD GetFinalPathNameByHandleA(HANDLE f, LPSTR path, DWORD size, DWORD flags);
#define FILE_NAME_NORMALIZED 0x0
#define FILE_NAME_OPENED 0x8
#define VOLUME_NAME_DOS 0x0
extern "C" DWORD GetFileAttributesW(LPCWSTR name);
extern "C" DWORD GetFileAttributesA(LPCSTR name);
extern "C" BOOL GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
extern "C" BOOL GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
extern "C" BOOL SetFileAttributesW(LPCWSTR name, DWORD attrs);
extern "C" BOOL SetFileAttributesA(LPCSTR name, DWORD attrs);
extern "C" BOOL CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa);
extern "C" BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa);
extern "C" BOOL RemoveDirectoryW(LPCWSTR name);
extern "C" BOOL RemoveDirectoryA(LPCSTR name);
extern "C" BOOL DeleteFileW(LPCWSTR name);
extern "C" BOOL DeleteFileA(LPCSTR name);
extern "C" BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
extern "C" BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
extern "C" BOOL MoveFileW(LPCWSTR from, LPCWSTR to);
extern "C" BOOL MoveFileA(LPCSTR from, LPCSTR to);
extern "C" BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
extern "C" BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL failIfExists);
extern "C" BOOL ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, LPVOID, LPVOID);
extern "C" HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data);
extern "C" HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
extern "C" HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags);
extern "C" BOOL FindNextFileW(HANDLE find, LPWIN32_FIND_DATAW data);
extern "C" BOOL FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data);
extern "C" BOOL FindClose(HANDLE find);
extern "C" DWORD GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buffer, LPWSTR* filePart);
extern "C" DWORD GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buffer, LPSTR* filePart);
extern "C" DWORD GetCurrentDirectoryW(DWORD size, LPWSTR buffer);
extern "C" DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer);
extern "C" BOOL SetCurrentDirectoryW(LPCWSTR dir);
extern "C" BOOL SetCurrentDirectoryA(LPCSTR dir);
extern "C" DWORD GetTempPathW(DWORD size, LPWSTR buffer);
extern "C" DWORD GetTempPathA(DWORD size, LPSTR buffer);
extern "C" UINT GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR name);
extern "C" BOOL GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER freeToCaller, PULARGE_INTEGER total, PULARGE_INTEGER totalFree);
extern "C" BOOL GetDiskFreeSpaceExA(LPCSTR dir, PULARGE_INTEGER freeToCaller, PULARGE_INTEGER total, PULARGE_INTEGER totalFree);
extern "C" BOOL GetVolumeInformationW(LPCWSTR root, LPWSTR name, DWORD nameSize, LPDWORD serial, LPDWORD maxComp, LPDWORD flags, LPWSTR fs, DWORD fsSize);
extern "C" UINT GetDriveTypeW(LPCWSTR root);
extern "C" HANDLE GetStdHandle(DWORD which);
extern "C" BOOL SetStdHandle(DWORD which, HANDLE h);
extern "C" BOOL AllocConsole(void);
extern "C" BOOL AttachConsole(DWORD pid);
extern "C" BOOL FreeConsole(void);
extern "C" HWND GetConsoleWindow(void);
extern "C" BOOL SetConsoleOutputCP(UINT cp);
extern "C" BOOL SetConsoleTitleW(LPCWSTR title);
extern "C" BOOL SetConsoleTitleA(LPCSTR title);
extern "C" BOOL WriteConsoleW(HANDLE h, const void* buffer, DWORD chars, LPDWORD written, LPVOID reserved);
extern "C" BOOL WriteConsoleA(HANDLE h, const void* buffer, DWORD chars, LPDWORD written, LPVOID reserved);
extern "C" BOOL GetConsoleMode(HANDLE h, LPDWORD mode);
extern "C" BOOL SetConsoleMode(HANDLE h, DWORD mode);
typedef BOOL (*PHANDLER_ROUTINE)(DWORD);
extern "C" BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE fn, BOOL add);
#define ATTACH_PARENT_PROCESS ((DWORD)-1)
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#define CTRL_C_EVENT 0
#define CTRL_BREAK_EVENT 1
#define CTRL_CLOSE_EVENT 2

// std::filesystem::path::c_str() is char* on Android: accept it wherever Windows code passes a path to a W API.
inline HANDLE CreateFileW(const char* n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) { return CreateFileA(n, a, s, sa, d, f, t); }
inline DWORD GetFileAttributesW(const char* n) { return GetFileAttributesA(n); }
inline BOOL GetFileAttributesExW(const char* n, GET_FILEEX_INFO_LEVELS l, LPVOID i) { return GetFileAttributesExA(n, l, i); }
inline BOOL SetFileAttributesW(const char* n, DWORD a) { return SetFileAttributesA(n, a); }
inline BOOL CreateDirectoryW(const char* n, LPSECURITY_ATTRIBUTES sa) { return CreateDirectoryA(n, sa); }
inline BOOL RemoveDirectoryW(const char* n) { return RemoveDirectoryA(n); }
inline BOOL DeleteFileW(const char* n) { return DeleteFileA(n); }
inline BOOL MoveFileExW(const char* a, const char* b, DWORD f) { return MoveFileExA(a, b, f); }
inline BOOL MoveFileW(const char* a, const char* b) { return MoveFileA(a, b); }
inline BOOL CopyFileW(const char* a, const char* b, BOOL f) { return CopyFileA(a, b, f); }
extern "C" HANDLE FindFirstFileExA(LPCSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags);
// Wide find data (WIN32_FIND_DATAW) for a UTF-8 pattern
extern "C" HANDLE Ttt2FindFirstFileExUtf8W(LPCSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags);
inline HANDLE FindFirstFileExW(const char* p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) { return Ttt2FindFirstFileExUtf8W(p, l, d, o, f, fl); }
inline HANDLE FindFirstFileW(const char* p, LPWIN32_FIND_DATAW d) { return Ttt2FindFirstFileExUtf8W(p, FindExInfoStandard, d, FindExSearchNameMatch, nullptr, 0); }

// Android: where "C:\..." / %LOCALAPPDATA% paths land (set by the launcher via env TTT2_ROOT).
extern "C" const char* Ttt2AndroidDataRoot(void);
// Win32 path (UTF-8 or UTF-32 wchar) -> host path: '\\' -> '/', drive letters mapped under the
// data root, case-insensitive lookup of every component (Xbox paths ignore case).
extern "C" int Ttt2HostPath(const char* winPath, char* out, size_t outSize, int mustExist);

// INI files
extern "C" DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file);
extern "C" UINT GetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT def, LPCSTR file);
extern "C" BOOL WritePrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file);
extern "C" DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR file);
extern "C" UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT def, LPCWSTR file);
extern "C" BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file);

// ---------------------------------------------------------------------------------
// Strings, locale, environment
// ---------------------------------------------------------------------------------
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF7 65000
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x00000008
#define WC_ERR_INVALID_CHARS 0x00000080
#define WC_NO_BEST_FIT_CHARS 0x00000400
extern "C" int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srcLen, LPWSTR dst, int dstLen);
extern "C" int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srcLen, LPSTR dst, int dstLen, LPCSTR defChar, LPBOOL usedDef);
extern "C" UINT GetACP(void);
extern "C" UINT GetOEMCP(void);
extern "C" LANGID GetUserDefaultUILanguage(void);
extern "C" LANGID GetSystemDefaultUILanguage(void);
extern "C" LANGID GetUserDefaultLangID(void);
extern "C" LCID GetUserDefaultLCID(void);
extern "C" int GetUserDefaultLocaleName(LPWSTR name, int size);
extern "C" BOOL GetUserPreferredUILanguages(DWORD flags, PULONG count, PWSTR buffer, PULONG size);
extern "C" int GetLocaleInfoEx(LPCWSTR locale, DWORD type, LPWSTR data, int size);
extern "C" int GetLocaleInfoW(LCID locale, DWORD type, LPWSTR data, int size);
extern "C" int GetLocaleInfoA(LCID locale, DWORD type, LPSTR data, int size);
extern "C" int GetGeoInfoW(LONG geo, DWORD type, LPWSTR data, int size, LANGID lang);
extern "C" LONG GetUserGeoID(DWORD cls);
#define LOCALE_NAME_MAX_LENGTH 85
#define LOCALE_USER_DEFAULT 0x0400
#define LOCALE_SYSTEM_DEFAULT 0x0800
#define LOCALE_NAME_USER_DEFAULT nullptr
#define LOCALE_SISO639LANGNAME 0x00000059
#define LOCALE_SISO3166CTRYNAME 0x0000005A
#define LOCALE_SNAME 0x0000005c
#define LOCALE_IGEOID 0x0000005B
#define LOCALE_RETURN_NUMBER 0x20000000
#define GEOCLASS_NATION 16
#define GEO_ISO2 0x0004
#define GEOID_NOT_AVAILABLE -1
#define MUI_LANGUAGE_NAME 0x8
#define LANG_NEUTRAL 0x00
#define LANG_ENGLISH 0x09
#define LANG_JAPANESE 0x11
#define LANG_FRENCH 0x0c
#define LANG_GERMAN 0x07
#define LANG_ITALIAN 0x10
#define LANG_SPANISH 0x0a
#define LANG_PORTUGUESE 0x16
#define LANG_KOREAN 0x12
#define LANG_CHINESE 0x04
#define LANG_RUSSIAN 0x19
#define LANG_POLISH 0x15
#define SUBLANG_DEFAULT 0x01
#define SUBLANG_NEUTRAL 0x00
#define SUBLANG_CHINESE_TRADITIONAL 0x01
#define SUBLANG_CHINESE_SIMPLIFIED 0x02
#define PRIMARYLANGID(l) ((WORD)(l) & 0x3ff)
#define SUBLANGID(l) ((WORD)(l) >> 10)
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define LANGIDFROMLCID(l) ((WORD)(l))
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_FROM_STRING 0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE 0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define FORMAT_MESSAGE_MAX_WIDTH_MASK 0x000000FF
extern "C" DWORD FormatMessageA(DWORD flags, LPCVOID source, DWORD id, DWORD lang, LPSTR buffer, DWORD size, va_list* args);
extern "C" DWORD FormatMessageW(DWORD flags, LPCVOID source, DWORD id, DWORD lang, LPWSTR buffer, DWORD size, va_list* args);
extern "C" DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size);
extern "C" DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size);
extern "C" BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
extern "C" BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value);
extern "C" DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD size);
extern "C" DWORD ExpandEnvironmentStringsW(LPCWSTR src, LPWSTR dst, DWORD size);
extern "C" LPWSTR GetCommandLineW(void);
extern "C" LPSTR GetCommandLineA(void);
extern "C" LPWSTR* CommandLineToArgvW(LPCWSTR cmd, int* argc);
extern "C" BOOL GetComputerNameA(LPSTR name, LPDWORD size);
extern "C" BOOL GetComputerNameW(LPWSTR name, LPDWORD size);
extern "C" BOOL GetUserNameA(LPSTR name, LPDWORD size);
extern "C" BOOL GetUserNameW(LPWSTR name, LPDWORD size);
#define MAX_COMPUTERNAME_LENGTH 15
#define UNLEN 256
extern "C" int lstrlenW(LPCWSTR s);
extern "C" int lstrlenA(LPCSTR s);
extern "C" int lstrcmpiW(LPCWSTR a, LPCWSTR b);
extern "C" int lstrcmpiA(LPCSTR a, LPCSTR b);
extern "C" int lstrcmpW(LPCWSTR a, LPCWSTR b);
extern "C" LPWSTR lstrcpynW(LPWSTR dst, LPCWSTR src, int size);
extern "C" LPSTR lstrcpynA(LPSTR dst, LPCSTR src, int size);
extern "C" LPWSTR CharLowerW(LPWSTR s);
extern "C" LPWSTR CharUpperW(LPWSTR s);
extern "C" LPSTR CharLowerA(LPSTR s);
extern "C" LPSTR CharUpperA(LPSTR s);
extern "C" int CompareStringOrdinal(LPCWCH a, int alen, LPCWCH b, int blen, BOOL ignoreCase);
#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3

// Version
typedef struct _OSVERSIONINFOW { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; WCHAR szCSDVersion[128]; } OSVERSIONINFOW, RTL_OSVERSIONINFOW, *PRTL_OSVERSIONINFOW, *LPOSVERSIONINFOW;
typedef struct _OSVERSIONINFOEXW { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; WCHAR szCSDVersion[128]; WORD wServicePackMajor, wServicePackMinor, wSuiteMask; BYTE wProductType, wReserved; } OSVERSIONINFOEXW, RTL_OSVERSIONINFOEXW;
extern "C" NTSTATUS RtlGetVersion(PRTL_OSVERSIONINFOW info);
extern "C" BOOL GetVersionExW(LPOSVERSIONINFOW info);
extern "C" BOOL IsProcessorFeaturePresent(DWORD feature);
#define PF_FASTFAIL_AVAILABLE 23
#define PF_ARM_NEON_INSTRUCTIONS_AVAILABLE 19
#define PF_SSE3_INSTRUCTIONS_AVAILABLE 13
#define PF_XMMI64_INSTRUCTIONS_AVAILABLE 10
#define PF_AVX_INSTRUCTIONS_AVAILABLE 39
#define PF_AVX2_INSTRUCTIONS_AVAILABLE 40
#define PF_SSSE3_INSTRUCTIONS_AVAILABLE 36
#define PF_SSE4_1_INSTRUCTIONS_AVAILABLE 37
#define PF_SSE4_2_INSTRUCTIONS_AVAILABLE 38
#define VER_PLATFORM_WIN32_NT 2

// Random / crypt (bcrypt.h, wincrypt.h features used by the runtime)
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
#define BCRYPT_SHA1_ALGORITHM L"SHA1"
#define BCRYPT_RNG_ALGORITHM L"RNG"
#define BCRYPT_OBJECT_LENGTH L"ObjectLength"
#define BCRYPT_HASH_LENGTH L"HashDigestLength"
#define BCRYPT_ALG_HANDLE_HMAC_FLAG 0x00000008
#define BCRYPT_SHA256_ALG_HANDLE ((BCRYPT_ALG_HANDLE)0x00000041)
#define BCRYPT_SHA1_ALG_HANDLE ((BCRYPT_ALG_HANDLE)0x00000031)
#define BCRYPT_HMAC_SHA256_ALG_HANDLE ((BCRYPT_ALG_HANDLE)0x000000b1)
#define BCRYPT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
typedef PVOID BCRYPT_HANDLE, BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE, BCRYPT_KEY_HANDLE;
extern "C" NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE alg, PUCHAR buffer, ULONG size, ULONG flags);
extern "C" NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* alg, LPCWSTR id, LPCWSTR impl, ULONG flags);
extern "C" NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE alg, ULONG flags);
extern "C" NTSTATUS BCryptGetProperty(BCRYPT_HANDLE obj, LPCWSTR prop, PUCHAR out, ULONG size, ULONG* result, ULONG flags);
extern "C" NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE* hash, PUCHAR obj, ULONG objSize, PUCHAR secret, ULONG secretSize, ULONG flags);
extern "C" NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, PUCHAR input, ULONG size, ULONG flags);
extern "C" NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR out, ULONG size, ULONG flags);
extern "C" NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash);
extern "C" NTSTATUS BCryptHash(BCRYPT_ALG_HANDLE alg, PUCHAR secret, ULONG secretSize, PUCHAR input, ULONG inputSize, PUCHAR out, ULONG outSize);
typedef struct _CRYPTOAPI_BLOB { DWORD cbData; BYTE* pbData; } DATA_BLOB, CRYPT_DATA_BLOB, CRYPT_HASH_BLOB, CRYPT_INTEGER_BLOB, CRYPT_OBJID_BLOB, CERT_NAME_BLOB, *PDATA_BLOB;
#define CRYPTPROTECT_UI_FORBIDDEN 0x1
#define CRYPTPROTECT_LOCAL_MACHINE 0x4
extern "C" BOOL CryptProtectData(DATA_BLOB* in, LPCWSTR desc, DATA_BLOB* entropy, PVOID reserved, PVOID prompt, DWORD flags, DATA_BLOB* out);
extern "C" BOOL CryptUnprotectData(DATA_BLOB* in, LPWSTR* desc, DATA_BLOB* entropy, PVOID reserved, PVOID prompt, DWORD flags, DATA_BLOB* out);
#define CRYPT_STRING_BASE64HEADER 0x00000000
#define CRYPT_STRING_BASE64 0x00000001
#define CRYPT_STRING_BINARY 0x00000002
#define CRYPT_STRING_HEX 0x00000004
#define CRYPT_STRING_HEXRAW 0x0000000c
#define CRYPT_STRING_NOCRLF 0x40000000
#define CRYPT_STRING_NOCR 0x80000000
extern "C" BOOL CryptBinaryToStringA(const BYTE* bin, DWORD size, DWORD flags, LPSTR out, DWORD* outSize);
extern "C" BOOL CryptBinaryToStringW(const BYTE* bin, DWORD size, DWORD flags, LPWSTR out, DWORD* outSize);
extern "C" BOOL CryptStringToBinaryA(LPCSTR str, DWORD len, DWORD flags, BYTE* out, DWORD* outSize, DWORD* skip, DWORD* usedFlags);
extern "C" BOOL CryptStringToBinaryW(LPCWSTR str, DWORD len, DWORD flags, BYTE* out, DWORD* outSize, DWORD* skip, DWORD* usedFlags);

// min/max macros are never defined (C++ code uses std::min/std::max).

// ---------------------------------------------------------------------------------
// User32 / GDI subset (implemented over SDL2, win32_sdl.cpp)
// ---------------------------------------------------------------------------------
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *PMSG, *LPMSG;
typedef struct tagWNDCLASSA {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor;
    HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName;
} WNDCLASSA, *PWNDCLASSA, *LPWNDCLASSA;
typedef struct tagWNDCLASSW {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor;
    HBRUSH hbrBackground; LPCWSTR lpszMenuName, lpszClassName;
} WNDCLASSW, *PWNDCLASSW, *LPWNDCLASSW;
typedef struct tagWNDCLASSEXA {
    UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor;
    HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; HICON hIconSm;
} WNDCLASSEXA;
typedef struct tagWNDCLASSEXW {
    UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor;
    HBRUSH hbrBackground; LPCWSTR lpszMenuName, lpszClassName; HICON hIconSm;
} WNDCLASSEXW;
typedef struct tagCREATESTRUCTA { LPVOID lpCreateParams; HINSTANCE hInstance; HMENU hMenu; HWND hwndParent; int cy, cx, y, x; LONG style; LPCSTR lpszName, lpszClass; DWORD dwExStyle; } CREATESTRUCTA, *LPCREATESTRUCTA;
typedef struct tagMONITORINFO { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO, *LPMONITORINFO;
typedef struct tagMONITORINFOEXW : public tagMONITORINFO { WCHAR szDevice[32]; } MONITORINFOEXW, *LPMONITORINFOEXW;
typedef struct tagMONITORINFOEXA : public tagMONITORINFO { CHAR szDevice[32]; } MONITORINFOEXA, *LPMONITORINFOEXA;
typedef struct tagWINDOWPLACEMENT { UINT length, flags, showCmd; POINT ptMinPosition, ptMaxPosition; RECT rcNormalPosition; } WINDOWPLACEMENT;
typedef struct tagMINMAXINFO { POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize; } MINMAXINFO, *LPMINMAXINFO;
typedef struct _devicemodeW {
    WCHAR dmDeviceName[32]; WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; DWORD dmFields;
    POINT dmPosition; DWORD dmDisplayOrientation, dmDisplayFixedOutput; short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    WCHAR dmFormName[32]; WORD dmLogPixels; DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
} DEVMODEW, *LPDEVMODEW, DEVMODE;
typedef struct _devicemodeA {
    CHAR dmDeviceName[32]; WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; DWORD dmFields;
    POINT dmPosition; DWORD dmDisplayOrientation, dmDisplayFixedOutput; short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    CHAR dmFormName[32]; WORD dmLogPixels; DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
} DEVMODEA, *LPDEVMODEA;
typedef struct _DISPLAY_DEVICEA { DWORD cb; CHAR DeviceName[32], DeviceString[128]; DWORD StateFlags; CHAR DeviceID[128], DeviceKey[128]; } DISPLAY_DEVICEA;
#define DISPLAY_DEVICE_ATTACHED_TO_DESKTOP 0x00000001
#define DISPLAY_DEVICE_PRIMARY_DEVICE 0x00000004
typedef struct _DISPLAY_DEVICEW { DWORD cb; WCHAR DeviceName[32], DeviceString[128]; DWORD StateFlags; WCHAR DeviceID[128], DeviceKey[128]; } DISPLAY_DEVICEW;
typedef struct tagBITMAPINFOHEADER { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct tagRGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct tagBITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO, *LPBITMAPINFO;
typedef struct tagPAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgbReserved[32]; } PAINTSTRUCT, *LPPAINTSTRUCT;
typedef struct tagTRACKMOUSEEVENT { DWORD cbSize, dwFlags; HWND hwndTrack; DWORD dwHoverTime; } TRACKMOUSEEVENT;
typedef struct tagRAWINPUTDEVICE { USHORT usUsagePage, usUsage; DWORD dwFlags; HWND hwndTarget; } RAWINPUTDEVICE, *PRAWINPUTDEVICE;
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define SRCCOPY 0x00CC0020

#define CW_USEDEFAULT ((int)0x80000000)
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_APPWINDOW 0x00040000L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#define WS_EX_LAYERED 0x00080000
#define WS_EX_TRANSPARENT 0x00000020L
#define WS_EX_NOACTIVATE 0x08000000L
#define GW_HWNDFIRST 0
#define GW_HWNDLAST 1
#define GW_HWNDNEXT 2
#define GW_HWNDPREV 3
#define GW_OWNER 4
#define GW_CHILD 5
#define IS_HIGH_SURROGATE(c) (((c) >= 0xd800) && ((c) <= 0xdbff))
#define IS_LOW_SURROGATE(c) (((c) >= 0xdc00) && ((c) <= 0xdfff))
#define CS_HREDRAW 0x0002
#define CS_VREDRAW 0x0001
#define CS_OWNDC 0x0020
#define CS_DBLCLKS 0x0008
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_WNDPROC (-4)
#define GWLP_WNDPROC (-4)
#define GWLP_USERDATA (-21)
#define GWLP_HINSTANCE (-6)
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOOWNERZORDER 0x0200
#define SWP_ASYNCWINDOWPOS 0x4000
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define HWND_TOPMOST ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)
#define HWND_DESKTOP ((HWND)0)
#define HWND_MESSAGE ((HWND)-3)
#define MONITOR_DEFAULTTONULL 0x00000000
#define MONITOR_DEFAULTTOPRIMARY 0x00000001
#define MONITOR_DEFAULTTONEAREST 0x00000002
#define MONITORINFOF_PRIMARY 0x00000001
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define PM_NOYIELD 0x0002
#define QS_ALLINPUT 0x04FF
#define QS_ALLEVENTS 0x04BF
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CMONITORS 80
#define SM_CXSIZEFRAME 32
#define SM_CYSIZEFRAME 33
#define SM_CYCAPTION 4
#define SM_REMOTESESSION 0x1000
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#define CDS_FULLSCREEN 0x00000004
#define CDS_TEST 0x00000002
#define DISP_CHANGE_SUCCESSFUL 0
#define DM_PELSWIDTH 0x00080000L
#define DM_PELSHEIGHT 0x00100000L
#define DM_DISPLAYFREQUENCY 0x00400000L
#define DM_BITSPERPEL 0x00040000L
#define IDC_ARROW ((LPCSTR)32512)
#define IDC_HAND ((LPCSTR)32649)
#define IDI_APPLICATION ((LPCSTR)32512)
#define COLOR_WINDOW 5
#define BLACK_BRUSH 4
#define IMAGE_ICON 1
#define LR_DEFAULTSIZE 0x00000040
#define LR_SHARED 0x00008000
#define MAKEINTRESOURCEA(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE MAKEINTRESOURCEA

// Window messages
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_DISPLAYCHANGE 0x007E
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCACTIVATE 0x0086
#define WM_INPUT_DEVICE_CHANGE 0x00FE
#define WM_INPUT 0x00FF
#define WM_KEYFIRST 0x0100
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_DEADCHAR 0x0103
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_KEYLAST 0x0109
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEFIRST 0x0200
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_MOUSEHWHEEL 0x020E
#define WM_MOUSELAST 0x020E
#define WM_SIZING 0x0214
#define WM_CAPTURECHANGED 0x0215
#define WM_MOVING 0x0216
#define WM_POWERBROADCAST 0x0218
#define WM_DEVICECHANGE 0x0219
#define WM_ENTERSIZEMOVE 0x0231
#define WM_EXITSIZEMOVE 0x0232
#define WM_DROPFILES 0x0233
#define WM_MOUSELEAVE 0x02A3
#define WM_DPICHANGED 0x02E0
#define WM_DWMCOMPOSITIONCHANGED 0x031E
#define WM_USER 0x0400
#define WM_APP 0x8000
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
#define SC_SIZE 0xF000
#define SC_MOVE 0xF010
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SC_CLOSE 0xF060
#define SC_KEYMENU 0xF100
#define SC_RESTORE 0xF120
#define SC_SCREENSAVE 0xF140
#define SC_MONITORPOWER 0xF170
#define HTCLIENT 1
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define WHEEL_DELTA 120
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define GET_KEYSTATE_WPARAM(wParam) (LOWORD(wParam))
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))
#define XBUTTON1 0x0001
#define XBUTTON2 0x0002
#define TME_LEAVE 0x00000002
#define RIDEV_INPUTSINK 0x00000100
#define RIDEV_REMOVE 0x00000001

// Message boxes
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_YESNO 0x00000004L
#define MB_YESNOCANCEL 0x00000003L
#define MB_ICONERROR 0x00000010L
#define MB_ICONHAND 0x00000010L
#define MB_ICONSTOP 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONWARNING 0x00000030L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONINFORMATION 0x00000040L
#define MB_SETFOREGROUND 0x00010000L
#define MB_TOPMOST 0x00040000L
#define MB_SYSTEMMODAL 0x00001000L
#define MB_TASKMODAL 0x00002000L
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7

// Virtual keys
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
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
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_SLEEP 0x5F
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
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
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_BROWSER_BACK 0xA6
#define VK_VOLUME_MUTE 0xAD
#define VK_VOLUME_DOWN 0xAE
#define VK_VOLUME_UP 0xAF
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_8 0xDF
#define VK_OEM_102 0xE2
#define VK_PROCESSKEY 0xE5
#define VK_PACKET 0xE7
#define VK_GAMEPAD_A 0xC3
#define MAPVK_VK_TO_VSC 0
#define MAPVK_VSC_TO_VK 1
#define MAPVK_VK_TO_CHAR 2
#define MAPVK_VSC_TO_VK_EX 3
#define KF_EXTENDED 0x0100
#define KF_REPEAT 0x4000
#define KF_UP 0x8000

extern "C" ATOM RegisterClassA(const WNDCLASSA* wc);
extern "C" ATOM RegisterClassW(const WNDCLASSW* wc);
extern "C" ATOM RegisterClassExA(const WNDCLASSEXA* wc);
extern "C" ATOM RegisterClassExW(const WNDCLASSEXW* wc);
extern "C" BOOL UnregisterClassA(LPCSTR name, HINSTANCE inst);
extern "C" BOOL UnregisterClassW(LPCWSTR name, HINSTANCE inst);
extern "C" HWND CreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
extern "C" HWND CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
#define CreateWindowA(c, t, s, x, y, w, h, p, m, i, pa) CreateWindowExA(0, c, t, s, x, y, w, h, p, m, i, pa)
#define CreateWindowW(c, t, s, x, y, w, h, p, m, i, pa) CreateWindowExW(0, c, t, s, x, y, w, h, p, m, i, pa)
extern "C" BOOL DestroyWindow(HWND w);
extern "C" BOOL ShowWindow(HWND w, int cmd);
extern "C" BOOL UpdateWindow(HWND w);
extern "C" BOOL IsWindow(HWND w);
extern "C" BOOL IsWindowVisible(HWND w);
extern "C" BOOL IsIconic(HWND w);
extern "C" BOOL IsZoomed(HWND w);
extern "C" BOOL SetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags);
extern "C" BOOL MoveWindow(HWND w, int x, int y, int cx, int cy, BOOL repaint);
extern "C" BOOL GetClientRect(HWND w, LPRECT r);
extern "C" BOOL GetWindowRect(HWND w, LPRECT r);
extern "C" BOOL AdjustWindowRect(LPRECT r, DWORD style, BOOL menu);
extern "C" BOOL AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD exStyle);
extern "C" BOOL AdjustWindowRectExForDpi(LPRECT r, DWORD style, BOOL menu, DWORD exStyle, UINT dpi);
extern "C" BOOL ClientToScreen(HWND w, LPPOINT p);
extern "C" BOOL ScreenToClient(HWND w, LPPOINT p);
extern "C" LONG GetWindowLongA(HWND w, int index);
extern "C" LONG GetWindowLongW(HWND w, int index);
extern "C" LONG SetWindowLongA(HWND w, int index, LONG value);
extern "C" LONG SetWindowLongW(HWND w, int index, LONG value);
extern "C" LONG_PTR GetWindowLongPtrA(HWND w, int index);
extern "C" LONG_PTR GetWindowLongPtrW(HWND w, int index);
extern "C" LONG_PTR SetWindowLongPtrA(HWND w, int index, LONG_PTR value);
extern "C" LONG_PTR SetWindowLongPtrW(HWND w, int index, LONG_PTR value);
extern "C" BOOL SetWindowTextA(HWND w, LPCSTR text);
extern "C" BOOL SetWindowTextW(HWND w, LPCWSTR text);
extern "C" int GetWindowTextA(HWND w, LPSTR text, int size);
extern "C" int GetWindowTextW(HWND w, LPWSTR text, int size);
extern "C" int GetClassNameW(HWND w, LPWSTR name, int size);
extern "C" int GetClassNameA(HWND w, LPSTR name, int size);
extern "C" HWND GetWindow(HWND w, UINT cmd);
extern "C" BOOL IntersectRect(LPRECT dst, const RECT* a, const RECT* b);
extern "C" BOOL UnionRect(LPRECT dst, const RECT* a, const RECT* b);
extern "C" BOOL PtInRect(const RECT* r, POINT p);
extern "C" BOOL SetRect(LPRECT r, int l, int t, int ri, int b);
extern "C" BOOL IsRectEmpty(const RECT* r);
extern "C" BOOL OffsetRect(LPRECT r, int dx, int dy);
extern "C" BOOL EqualRect(const RECT* a, const RECT* b);
typedef BOOL (*WNDENUMPROC)(HWND, LPARAM);
extern "C" BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp);
extern "C" BOOL EnumThreadWindows(DWORD tid, WNDENUMPROC fn, LPARAM lp);
extern "C" BOOL GetWindowPlacement(HWND w, WINDOWPLACEMENT* p);
extern "C" BOOL SetWindowPlacement(HWND w, const WINDOWPLACEMENT* p);
extern "C" BOOL PeekMessageA(LPMSG msg, HWND w, UINT first, UINT last, UINT remove);
extern "C" BOOL PeekMessageW(LPMSG msg, HWND w, UINT first, UINT last, UINT remove);
extern "C" BOOL GetMessageA(LPMSG msg, HWND w, UINT first, UINT last);
extern "C" BOOL GetMessageW(LPMSG msg, HWND w, UINT first, UINT last);
extern "C" BOOL TranslateMessage(const MSG* msg);
extern "C" LRESULT DispatchMessageA(const MSG* msg);
extern "C" LRESULT DispatchMessageW(const MSG* msg);
extern "C" LRESULT DefWindowProcA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
extern "C" LRESULT DefWindowProcW(HWND w, UINT msg, WPARAM wp, LPARAM lp);
extern "C" LRESULT CallWindowProcA(WNDPROC fn, HWND w, UINT msg, WPARAM wp, LPARAM lp);
extern "C" LRESULT CallWindowProcW(WNDPROC fn, HWND w, UINT msg, WPARAM wp, LPARAM lp);
extern "C" BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
extern "C" BOOL PostMessageW(HWND w, UINT msg, WPARAM wp, LPARAM lp);
extern "C" LRESULT SendMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
extern "C" LRESULT SendMessageW(HWND w, UINT msg, WPARAM wp, LPARAM lp);
extern "C" BOOL PostThreadMessageA(DWORD tid, UINT msg, WPARAM wp, LPARAM lp);
extern "C" BOOL PostThreadMessageW(DWORD tid, UINT msg, WPARAM wp, LPARAM lp);
extern "C" void PostQuitMessage(int code);
extern "C" HWND GetForegroundWindow(void);
extern "C" BOOL SetForegroundWindow(HWND w);
extern "C" HWND GetActiveWindow(void);
extern "C" HWND SetActiveWindow(HWND w);
extern "C" HWND GetFocus(void);
extern "C" HWND SetFocus(HWND w);
extern "C" HWND GetDesktopWindow(void);
extern "C" HWND WindowFromPoint(POINT p);
extern "C" HWND FindWindowA(LPCSTR cls, LPCSTR title);
extern "C" HWND FindWindowW(LPCWSTR cls, LPCWSTR title);
extern "C" DWORD GetWindowThreadProcessId(HWND w, LPDWORD pid);
extern "C" HWND GetParent(HWND w);
extern "C" HWND GetAncestor(HWND w, UINT flags);
#define GA_ROOT 2
extern "C" BOOL FlashWindow(HWND w, BOOL invert);
extern "C" BOOL BringWindowToTop(HWND w);
extern "C" BOOL InvalidateRect(HWND w, const RECT* r, BOOL erase);
extern "C" BOOL ValidateRect(HWND w, const RECT* r);
extern "C" HDC BeginPaint(HWND w, LPPAINTSTRUCT ps);
extern "C" BOOL EndPaint(HWND w, const PAINTSTRUCT* ps);
extern "C" HDC GetDC(HWND w);
extern "C" int ReleaseDC(HWND w, HDC dc);
extern "C" int FillRect(HDC dc, const RECT* r, HBRUSH b);
extern "C" HGDIOBJ GetStockObject(int i);
extern "C" BOOL DeleteObject(HGDIOBJ o);
extern "C" HBRUSH CreateSolidBrush(COLORREF c);
extern "C" int StretchDIBits(HDC dc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, const void* bits, const BITMAPINFO* bmi, UINT usage, DWORD rop);
extern "C" int SetDIBitsToDevice(HDC dc, int xd, int yd, DWORD w, DWORD h, int xs, int ys, UINT start, UINT lines, const void* bits, const BITMAPINFO* bmi, UINT usage);
extern "C" HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name);
extern "C" HCURSOR LoadCursorW(HINSTANCE inst, LPCWSTR name);
extern "C" HICON LoadIconA(HINSTANCE inst, LPCSTR name);
extern "C" HICON LoadIconW(HINSTANCE inst, LPCWSTR name);
extern "C" HANDLE LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags);
extern "C" HANDLE LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy, UINT flags);
extern "C" HCURSOR SetCursor(HCURSOR c);
extern "C" int ShowCursor(BOOL show);
extern "C" BOOL GetCursorPos(LPPOINT p);
extern "C" BOOL SetCursorPos(int x, int y);
extern "C" BOOL ClipCursor(const RECT* r);
extern "C" HWND SetCapture(HWND w);
extern "C" BOOL ReleaseCapture(void);
extern "C" BOOL TrackMouseEvent(TRACKMOUSEEVENT* t);
extern "C" BOOL RegisterRawInputDevices(const RAWINPUTDEVICE* d, UINT count, UINT size);
extern "C" SHORT GetAsyncKeyState(int vk);
extern "C" SHORT GetKeyState(int vk);
extern "C" BOOL GetKeyboardState(PBYTE keys);
extern "C" UINT MapVirtualKeyA(UINT code, UINT type);
extern "C" UINT MapVirtualKeyW(UINT code, UINT type);
extern "C" int GetKeyNameTextA(LONG lp, LPSTR out, int size);
extern "C" int GetKeyNameTextW(LONG lp, LPWSTR out, int size);
extern "C" int ToUnicode(UINT vk, UINT scan, const BYTE* state, LPWSTR out, int size, UINT flags);
extern "C" int GetSystemMetrics(int index);
extern "C" BOOL SystemParametersInfoW(UINT action, UINT param, PVOID out, UINT winIni);
extern "C" BOOL SystemParametersInfoA(UINT action, UINT param, PVOID out, UINT winIni);
#define SPI_GETWORKAREA 0x0030
#define SPI_GETSCREENSAVEACTIVE 0x0010
#define SPI_SETSCREENSAVEACTIVE 0x0011
extern "C" HMONITOR MonitorFromWindow(HWND w, DWORD flags);
extern "C" HMONITOR MonitorFromPoint(POINT p, DWORD flags);
extern "C" HMONITOR MonitorFromRect(LPCRECT r, DWORD flags);
extern "C" BOOL GetMonitorInfoA(HMONITOR m, LPMONITORINFO info);
extern "C" BOOL GetMonitorInfoW(HMONITOR m, LPMONITORINFO info);
typedef BOOL (*MONITORENUMPROC)(HMONITOR, HDC, LPRECT, LPARAM);
extern "C" BOOL EnumDisplayMonitors(HDC dc, LPCRECT clip, MONITORENUMPROC fn, LPARAM data);
extern "C" BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* dm);
extern "C" BOOL EnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEA* dm);
extern "C" BOOL EnumDisplayDevicesW(LPCWSTR device, DWORD index, DISPLAY_DEVICEW* dd, DWORD flags);
extern "C" BOOL EnumDisplayDevicesA(LPCSTR device, DWORD index, DISPLAY_DEVICEA* dd, DWORD flags);
extern "C" LONG ChangeDisplaySettingsW(DEVMODEW* dm, DWORD flags);
extern "C" LONG ChangeDisplaySettingsExW(LPCWSTR device, DEVMODEW* dm, HWND w, DWORD flags, LPVOID param);
extern "C" UINT GetDpiForWindow(HWND w);
extern "C" UINT GetDpiForSystem(void);
extern "C" BOOL SetProcessDPIAware(void);
extern "C" BOOL SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT ctx);
extern "C" HRESULT SetProcessDpiAwareness(int value);
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((DPI_AWARENESS_CONTEXT)-3)
#define PROCESS_PER_MONITOR_DPI_AWARE 2
extern "C" int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type);
extern "C" int MessageBoxW(HWND w, LPCWSTR text, LPCWSTR caption, UINT type);
extern "C" BOOL MessageBeep(UINT type);
extern "C" BOOL Beep(DWORD freq, DWORD duration);
typedef void (*TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
extern "C" UINT_PTR SetTimer(HWND w, UINT_PTR id, UINT ms, TIMERPROC fn);
extern "C" BOOL KillTimer(HWND w, UINT_PTR id);
extern "C" BOOL OpenClipboard(HWND w);
extern "C" BOOL CloseClipboard(void);
extern "C" BOOL EmptyClipboard(void);
extern "C" HANDLE SetClipboardData(UINT fmt, HANDLE mem);
extern "C" HANDLE GetClipboardData(UINT fmt);
extern "C" LPVOID GlobalLock(HGLOBAL mem);
extern "C" BOOL GlobalUnlock(HGLOBAL mem);
#define CF_TEXT 1
#define CF_UNICODETEXT 13
#define GMEM_MOVEABLE 0x0002
extern "C" HWND GetShellWindow(void);
extern "C" BOOL LockSetForegroundWindow(UINT code);
extern "C" BOOL AllowSetForegroundWindow(DWORD pid);
#define ASFW_ANY ((DWORD)-1)
typedef struct tagFLASHWINFO { UINT cbSize; HWND hwnd; DWORD dwFlags; UINT uCount; DWORD dwTimeout; } FLASHWINFO;
extern "C" BOOL FlashWindowEx(FLASHWINFO* info);
#define FLASHW_ALL 3
#define FLASHW_TIMERNOFG 12
extern "C" HMENU GetSystemMenu(HWND w, BOOL revert);
extern "C" BOOL EnableMenuItem(HMENU m, UINT id, UINT flags);
#define MF_BYCOMMAND 0
#define MF_GRAYED 1
extern "C" BOOL DragAcceptFiles_shim(HWND w, BOOL accept);

// Android: the SDL_Window behind an HWND created by CreateWindowEx (what DXVK-native wants as HWND).
struct SDL_Window;
extern "C" SDL_Window* Ttt2SdlWindow(HWND w);

// ---------------------------------------------------------------------------------
// COM basics
// ---------------------------------------------------------------------------------
typedef enum tagCOINIT { COINIT_APARTMENTTHREADED = 0x2, COINIT_MULTITHREADED = 0x0, COINIT_DISABLE_OLE1DDE = 0x4, COINIT_SPEED_OVER_MEMORY = 0x8 } COINIT;
#define CLSCTX_INPROC_SERVER 0x1
#define CLSCTX_INPROC_HANDLER 0x2
#define CLSCTX_LOCAL_SERVER 0x4
#define CLSCTX_ALL 0x17
#define CLSCTX_INPROC (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)
extern "C" HRESULT CoInitialize(LPVOID reserved);
extern "C" HRESULT CoInitializeEx(LPVOID reserved, DWORD flags);
extern "C" void CoUninitialize(void);
extern "C" HRESULT CoCreateInstance(REFCLSID clsid, IUnknown* outer, DWORD ctx, REFIID iid, LPVOID* out);
extern "C" void CoTaskMemFree(LPVOID p);
extern "C" LPVOID CoTaskMemAlloc(SIZE_T size);
extern "C" BSTR SysAllocString(const OLECHAR* s);
extern "C" void SysFreeString(BSTR s);
#define STGM_READ 0x00000000L
#define SUCCEEDED_OR_FALSE(hr) ((hr) >= 0)

// ---------------------------------------------------------------------------------
// Registry (not available: every open fails)
// ---------------------------------------------------------------------------------
typedef LONG LSTATUS;
typedef ACCESS_MASK REGSAM;
#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)((LONG)0x80000000))
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)((LONG)0x80000001))
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)((LONG)0x80000002))
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE 0x0002
#define KEY_WOW64_64KEY 0x0100
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_QWORD 11
#define RRF_RT_REG_SZ 0x00000002
#define RRF_RT_REG_DWORD 0x00000010
#define RRF_RT_ANY 0x0000ffff
#define REG_OPTION_NON_VOLATILE 0
extern "C" LSTATUS RegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD opts, REGSAM sam, HKEY* out);
extern "C" LSTATUS RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD opts, REGSAM sam, HKEY* out);
extern "C" LSTATUS RegCreateKeyExW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR cls, DWORD opts, REGSAM sam, LPSECURITY_ATTRIBUTES sa, HKEY* out, LPDWORD disp);
extern "C" LSTATUS RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size);
extern "C" LSTATUS RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size);
extern "C" LSTATUS RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size);
extern "C" LSTATUS RegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD size);
extern "C" LSTATUS RegGetValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD size);
extern "C" LSTATUS RegDeleteValueW(HKEY key, LPCWSTR name);
extern "C" LSTATUS RegDeleteTreeW(HKEY key, LPCWSTR sub);
extern "C" LSTATUS RegCloseKey(HKEY key);

// Shell
typedef GUID KNOWNFOLDERID;
typedef const GUID& REFKNOWNFOLDERID;
DEFINE_GUID(FOLDERID_LocalAppData, 0xF1B32785, 0x6FBA, 0x4FCF, 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91)
DEFINE_GUID(FOLDERID_RoamingAppData, 0x3EB685DB, 0x65F9, 0x4CF6, 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D)
DEFINE_GUID(FOLDERID_Documents, 0xFDD39AD0, 0x238F, 0x46AF, 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7)
DEFINE_GUID(FOLDERID_SavedGames, 0x4C5C32FF, 0xBB9D, 0x43b0, 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4)
DEFINE_GUID(FOLDERID_Desktop, 0xB4BFCC3A, 0xDB2C, 0x424C, 0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41)
DEFINE_GUID(FOLDERID_ProgramFiles, 0x905e63b6, 0xc1bf, 0x494e, 0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a)
DEFINE_GUID(FOLDERID_Downloads, 0x374DE290, 0x123F, 0x4565, 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B)
#define KF_FLAG_DEFAULT 0x00000000
#define KF_FLAG_CREATE 0x00008000
#define CSIDL_LOCAL_APPDATA 0x001c
#define CSIDL_APPDATA 0x001a
#define CSIDL_PERSONAL 0x0005
#define CSIDL_FLAG_CREATE 0x8000
#define SHGFP_TYPE_CURRENT 0
extern "C" HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR* path);
extern "C" HRESULT SHGetFolderPathW(HWND w, int csidl, HANDLE token, DWORD flags, LPWSTR path);
extern "C" HRESULT SHGetFolderPathA(HWND w, int csidl, HANDLE token, DWORD flags, LPSTR path);
extern "C" int SHCreateDirectoryExW(HWND w, LPCWSTR path, const SECURITY_ATTRIBUTES* sa);
extern "C" int SHCreateDirectoryExA(HWND w, LPCSTR path, const SECURITY_ATTRIBUTES* sa);
extern "C" HINSTANCE ShellExecuteW(HWND w, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show);
extern "C" HINSTANCE ShellExecuteA(HWND w, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT show);
extern "C" LPWSTR* CommandLineToArgvW(LPCWSTR cmd, int* argc);
extern "C" BOOL PathFileExistsW(LPCWSTR path);
extern "C" BOOL PathFileExistsA(LPCSTR path);

// DWM
extern "C" HRESULT DwmSetWindowAttribute(HWND w, DWORD attr, LPCVOID value, DWORD size);
extern "C" HRESULT DwmGetWindowAttribute(HWND w, DWORD attr, PVOID value, DWORD size);
extern "C" HRESULT DwmFlush(void);
extern "C" HRESULT DwmIsCompositionEnabled(BOOL* enabled);
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#define DWMWA_CLOAKED 14
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#define DWMWCP_DONOTROUND 1
typedef struct _DWM_TIMING_INFO { UINT32 cbSize; BYTE rest[284]; } DWM_TIMING_INFO;
extern "C" HRESULT DwmGetCompositionTimingInfo(HWND w, DWM_TIMING_INFO* info);

// Power
typedef struct _SYSTEM_POWER_STATUS { BYTE ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag; DWORD BatteryLifeTime, BatteryFullLifeTime; } SYSTEM_POWER_STATUS, *LPSYSTEM_POWER_STATUS;
extern "C" BOOL GetSystemPowerStatus(LPSYSTEM_POWER_STATUS status);

#endif // TTT2_WIN32_SHIM

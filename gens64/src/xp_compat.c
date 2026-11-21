/* gens64: Windows XP compatibility of the 32-bit build (only that build links this file).

   The C++ runtime of the compiler (libgcc's gthr-win32, used by libstdc++ for the C++ exceptions
   of the 7z code) imports GetThreadId, which kernel32 has only since Vista; with it the exe does
   not even start on XP ("entry point GetThreadId not found"). Here the import pointer
   __imp__GetThreadId@4 is defined by us, so the linker does not take kernel32's: it calls the real
   GetThreadId where there is one, and on XP asks ntdll (NtQueryInformationThread,
   ThreadBasicInformation) for the thread id. Nothing in the emulation depends on this.

   The other Vista function of that runtime (condition variables, for thread-safe initialisation of
   static locals) is avoided with -fno-threadsafe-statics. tools/check_imports.py checks the imports
   of the built exe against the list of those known to exist on XP. */
#include <windows.h>

typedef DWORD (WINAPI *GetThreadId_t)(HANDLE);
typedef LONG (WINAPI *NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);

typedef struct
{
	LONG ExitStatus;
	PVOID TebBaseAddress;
	struct { HANDLE UniqueProcess, UniqueThread; } ClientId;
	ULONG_PTR AffinityMask;
	LONG Priority, BasePriority;
} xp_thread_basic_information;

static DWORD WINAPI xp_GetThreadId(HANDLE thread)
{
	static GetThreadId_t real;
	static NtQueryInformationThread_t query;
	static int looked;
	xp_thread_basic_information info;

	if (!looked)
	{
		real = (GetThreadId_t) GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadId");
		query = (NtQueryInformationThread_t) GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
		looked = 1;
	}
	if (real)
		return real(thread);
	if (thread == GetCurrentThread())
		return GetCurrentThreadId();
	if (query && query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), NULL) >= 0)
		return (DWORD) (ULONG_PTR) info.ClientId.UniqueThread;
	SetLastError(ERROR_INVALID_HANDLE);
	return 0;
}

/* the import pointer libgcc calls through (the name has no C spelling: hence the asm label) */
DWORD (WINAPI *xp_imp_GetThreadId)(HANDLE) __asm__("__imp__GetThreadId@4") = xp_GetThreadId;

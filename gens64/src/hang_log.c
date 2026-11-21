/* gens64: hang recorder for the 32-bit build (-DGENS64_HANGLOG=ON only; not in releases).

   Started before main() by a constructor, on the main thread. A second thread samples the main
   thread once a second for the first 60 s: suspends it, copies EIP/ESP/EBP and the top of its
   stack, resumes it, then writes the sample to gens_hang.log next to the exe. Return addresses
   on the stack that fall inside the exe are what the build's symbol map turns into function names.
   Nothing is written while the main thread is suspended (it may hold the heap or CRT locks). */
#include <windows.h>
#include <stdio.h>

#define STACK_DWORDS 512

static HANDLE s_main;
static DWORD s_main_id;

static int readable(const void *p, SIZE_T n)
{
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
		return 0;
	return (const char *) p + n <= (const char *) mi.BaseAddress + mi.RegionSize;
}

static void module_of(DWORD addr, char *name, int n)
{
	MEMORY_BASIC_INFORMATION mi;
	name[0] = 0;
	if (VirtualQuery((void *) addr, &mi, sizeof(mi)) && mi.AllocationBase)
	{
		char full[MAX_PATH];
		if (GetModuleFileNameA((HMODULE) mi.AllocationBase, full, sizeof(full)))
		{
			const char *s = strrchr(full, '\\');
			_snprintf(name, n, "%s+%lx", s ? s + 1 : full, (unsigned long) (addr - (DWORD) mi.AllocationBase));
			name[n - 1] = 0;
		}
	}
}

static DWORD WINAPI sampler(LPVOID unused)
{
	char path[MAX_PATH], *slash;
	DWORD stack[STACK_DWORDS];
	HMODULE exe = GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *) ((char *) exe + ((IMAGE_DOS_HEADER *) exe)->e_lfanew);
	DWORD lo = (DWORD) exe, hi = lo + nt->OptionalHeader.SizeOfImage;
	FILE *f;
	int t;

	GetModuleFileNameA(NULL, path, sizeof(path));
	slash = strrchr(path, '\\');
	strcpy(slash ? slash + 1 : path, "gens_hang.log");
	f = fopen(path, "w");
	if (!f)
		return 0;
	{
		OSVERSIONINFOA vi;
		vi.dwOSVersionInfoSize = sizeof(vi);
		GetVersionExA(&vi);
		fprintf(f, "Windows %lu.%lu build %lu %s; exe %08lx-%08lx\n", vi.dwMajorVersion, vi.dwMinorVersion,
			vi.dwBuildNumber, vi.szCSDVersion, (unsigned long) lo, (unsigned long) hi);
		fflush(f);
	}

	for (t = 1; t <= 60; t++)
	{
		CONTEXT c;
		int n = 0, i;
		char mod[MAX_PATH + 16];

		Sleep(1000);
		memset(&c, 0, sizeof(c));
		c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		if (SuspendThread(s_main) == (DWORD) -1)
			break;
		if (GetThreadContext(s_main, &c))
		{
			DWORD *sp = (DWORD *) c.Esp;
			for (n = STACK_DWORDS; n > 0 && !readable(sp, n * sizeof(DWORD)); n /= 2)
				;
			if (n)
				memcpy(stack, sp, n * sizeof(DWORD));
		}
		ResumeThread(s_main);

		module_of(c.Eip, mod, sizeof(mod));
		fprintf(f, "t=%d eip=%08lx (%s) esp=%08lx ebp=%08lx eax=%08lx ecx=%08lx edx=%08lx\n  ret:", t,
			(unsigned long) c.Eip, mod, (unsigned long) c.Esp, (unsigned long) c.Ebp,
			(unsigned long) c.Eax, (unsigned long) c.Ecx, (unsigned long) c.Edx);
		for (i = 0; i < n; i++)
			if (stack[i] >= lo && stack[i] < hi)
				fprintf(f, " %08lx", (unsigned long) stack[i]);
		fprintf(f, "\n  dll:");
		for (i = 0; i < n && i < 128; i++)	/* system DLL frames near the top (waits, locks) */
			if ((stack[i] < lo || stack[i] >= hi) && stack[i] >= 0x10000000 && stack[i] < 0x80000000)
			{
				module_of(stack[i], mod, sizeof(mod));
				if (mod[0])
					fprintf(f, " %s", mod);
			}
		fprintf(f, "\n");
		fflush(f);
	}
	fclose(f);
	return 0;
}

__attribute__((constructor)) static void hang_log_start(void)
{
	s_main_id = GetCurrentThreadId();
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_main,
		THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
	CloseHandle(CreateThread(NULL, 0, sampler, NULL, 0, NULL));
}

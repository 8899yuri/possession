// GTA IV Runtime Analyzer v1
//
// Target:
//   GTA IV Steam Complete Edition, verified against the user's uploaded
//   GTAIV.exe build: 1.2.0.59 / PE32 x86.
//
// IMPORTANT:
//   This DLL is an OBSERVER only.
//   It does not patch code, change game variables, alter FPS limits,
//   change NPC/traffic/AI/physics/streaming, or install hooks.
//
// Output:
//   GTAIV_RuntimeAnalyzer.log in the GTA IV root directory.
//
// The analyzer is intentionally Win32-only. No GTA V ScriptHook APIs and
// no guessed GTA IV memory addresses are used.

#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>

static HANDLE g_log = NULL;
static HMODULE g_self = NULL;

static void Log(const char* fmt, ...)
{
    if (!g_log)
        return;

    char buffer[2048];

    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, ap);
    va_end(ap);

    // With _TRUNCATE, MSVC may return -1 when the formatted text was
    // truncated, while the buffer still contains the truncated text.
    if (n < 0)
        n = (int)sizeof(buffer) - 1;

    if (n == 0)
        return;

    DWORD written = 0;
    WriteFile(g_log, buffer, (DWORD)n, &written, NULL);
    FlushFileBuffers(g_log);
}

static void GetRootPath(char* out, DWORD outSize)
{
    out[0] = 0;

    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return;

    // Remove filename.
    for (DWORD i = n; i > 0; --i)
    {
        if (path[i - 1] == '\\' || path[i - 1] == '/')
        {
            path[i] = 0;
            lstrcpynA(out, path, outSize);
            return;
        }
    }
}

static void LogPEImage(HMODULE module)
{
    if (!module)
        return;

    BYTE* base = reinterpret_cast<BYTE*>(module);

    __try
    {
        IMAGE_DOS_HEADER* dos =
            reinterpret_cast<IMAGE_DOS_HEADER*>(base);

        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            Log("PE: invalid DOS signature\r\n");
            return;
        }

        IMAGE_NT_HEADERS32* nt =
            reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);

        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            Log("PE: invalid NT signature\r\n");
            return;
        }

        Log("PE: ImageBase=%p EntryPointRVA=0x%08X SizeOfImage=0x%08X "
            "TimeDateStamp=0x%08X\r\n",
            base,
            nt->OptionalHeader.AddressOfEntryPoint,
            nt->OptionalHeader.SizeOfImage,
            nt->FileHeader.TimeDateStamp);

        IMAGE_SECTION_HEADER* sec =
            IMAGE_FIRST_SECTION(nt);

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            char name[9] = {};
            memcpy(name, sec[i].Name, 8);

            BYTE* start = base + sec[i].VirtualAddress;

            Log("SECTION[%u]: %-8s RVA=0x%08X VA=%p "
                "VirtualSize=0x%08X RawSize=0x%08X Characteristics=0x%08X\r\n",
                i,
                name,
                sec[i].VirtualAddress,
                start,
                sec[i].Misc.VirtualSize,
                sec[i].SizeOfRawData,
                sec[i].Characteristics);
        }

        BYTE* ep = base + nt->OptionalHeader.AddressOfEntryPoint;

        Log("ENTRYPOINT: VA=%p\r\n", ep);
        Log("ENTRYPOINT_BYTES:");

        for (int i = 0; i < 32; ++i)
            Log(" %02X", ep[i]);

        Log("\r\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("PE: exception while reading image\r\n");
    }
}

struct ThreadSample
{
    DWORD tid;
    ULONGLONG kernel100ns;
    ULONGLONG user100ns;
    ULONGLONG total100ns;
};

static bool ReadThreadTimes(DWORD tid, ThreadSample& out)
{
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h)
        return false;

    FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};

    BOOL ok = GetThreadTimes(
        h,
        &createTime,
        &exitTime,
        &kernelTime,
        &userTime);

    CloseHandle(h);

    if (!ok)
        return false;

    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernelTime.dwLowDateTime;
    k.HighPart = kernelTime.dwHighDateTime;
    u.LowPart = userTime.dwLowDateTime;
    u.HighPart = userTime.dwHighDateTime;

    out.tid = tid;
    out.kernel100ns = k.QuadPart;
    out.user100ns = u.QuadPart;
    out.total100ns = k.QuadPart + u.QuadPart;

    return true;
}

static int CollectThreads(ThreadSample* out, int maxCount)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);

    int count = 0;

    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid)
                continue;

            if (count >= maxCount)
                break;

            ThreadSample s{};
            if (ReadThreadTimes(te.th32ThreadID, s))
                out[count++] = s;

        } while (Thread32Next(snap, &te));
    }

    CloseHandle(snap);
    return count;
}

static void LogThreadSnapshot(const char* label,
                              const ThreadSample* before,
                              int beforeCount,
                              const ThreadSample* after,
                              int afterCount,
                              ULONGLONG elapsed100ns)
{
    Log("\r\nTHREAD_SAMPLE: %s\r\n", label);
    Log("SampleInterval100ns=%I64u\r\n", elapsed100ns);

    struct Result
    {
        DWORD tid;
        ULONGLONG delta;
    };

    Result results[512]{};
    int resultCount = 0;

    for (int i = 0; i < afterCount; ++i)
    {
        for (int j = 0; j < beforeCount; ++j)
        {
            if (after[i].tid != before[j].tid)
                continue;

            ULONGLONG delta =
                (after[i].total100ns >= before[j].total100ns)
                ? (after[i].total100ns - before[j].total100ns)
                : 0;

            if (resultCount < 512)
            {
                results[resultCount].tid = after[i].tid;
                results[resultCount].delta = delta;
                ++resultCount;
            }

            break;
        }
    }

    // Simple selection sort for the top 12 CPU-consuming threads.
    for (int i = 0; i < resultCount; ++i)
    {
        int best = i;

        for (int j = i + 1; j < resultCount; ++j)
        {
            if (results[j].delta > results[best].delta)
                best = j;
        }

        if (best != i)
        {
            Result tmp = results[i];
            results[i] = results[best];
            results[best] = tmp;
        }
    }

    int limit = resultCount < 12 ? resultCount : 12;

    for (int i = 0; i < limit; ++i)
    {
        double cpuPct =
            elapsed100ns
            ? (100.0 * (double)results[i].delta /
               (double)elapsed100ns)
            : 0.0;

        Log("TOP[%02d] TID=%lu CPU_Time=%.2f%% Delta100ns=%I64u\r\n",
            i + 1,
            (unsigned long)results[i].tid,
            cpuPct,
            results[i].delta);
    }
}

static void LogProcessInfo()
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;

    BOOL affinityOk =
        GetProcessAffinityMask(
            GetCurrentProcess(),
            &processMask,
            &systemMask);

    Log("PROCESS: PID=%lu\r\n",
        (unsigned long)GetCurrentProcessId());

    Log("CPU: Architecture=%u LogicalProcessors=%lu\r\n",
        (unsigned)si.wProcessorArchitecture,
        (unsigned long)si.dwNumberOfProcessors);

    if (affinityOk)
    {
        Log("CPU_AFFINITY: ProcessMask=0x%p SystemMask=0x%p\r\n",
            (void*)processMask,
            (void*)systemMask);
    }
}

static void LogLoadedModules()
{
    HANDLE snap =
        CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            GetCurrentProcessId());

    if (snap == INVALID_HANDLE_VALUE)
    {
        Log("MODULES: CreateToolhelp32Snapshot failed error=%lu\r\n",
            (unsigned long)GetLastError());
        return;
    }

    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);

    Log("\r\nMODULES:\r\n");

    if (Module32First(snap, &me))
    {
        do
        {
            Log("MODULE: %s Base=%p Size=0x%08lX\r\n",
                me.szModule,
                me.modBaseAddr,
                (unsigned long)me.modBaseSize);

        } while (Module32Next(snap, &me));
    }

    CloseHandle(snap);
}

static DWORD WINAPI AnalyzerThread(LPVOID)
{
    // Give the loader and ScriptHook/other ASIs time to finish loading.
    Sleep(5000);

    char root[MAX_PATH] = {};
    GetRootPath(root, sizeof(root));

    char logPath[MAX_PATH] = {};
    _snprintf_s(
        logPath,
        sizeof(logPath),
        _TRUNCATE,
        "%sGTAIV_RuntimeAnalyzer.log",
        root);

    g_log = CreateFileA(
        logPath,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (g_log == INVALID_HANDLE_VALUE)
    {
        g_log = NULL;
        return 0;
    }

    Log("GTA IV Runtime Analyzer v1\r\n");
    Log("Observer-only build. No game memory is modified.\r\n");
    Log("Target build: GTA IV 1.2.0.59 / PE32 x86\r\n");
    Log("=============================================\r\n\r\n");

    HMODULE gta = GetModuleHandleA(NULL);

    LogProcessInfo();
    LogPEImage(gta);
    LogLoadedModules();

    ThreadSample before[512]{};
    ThreadSample after[512]{};

    int beforeCount = CollectThreads(before, 512);

    LARGE_INTEGER qpf{}, q0{}, q1{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&q0);

    // 8 seconds gives enough time for the game to settle while remaining
    // short enough to be practical on the user's phone/emulator.
    Sleep(8000);

    QueryPerformanceCounter(&q1);

    int afterCount = CollectThreads(after, 512);

    ULONGLONG elapsed100ns = 0;

    if (qpf.QuadPart > 0 && q1.QuadPart >= q0.QuadPart)
    {
        // Convert QPC interval to 100 ns units.
        elapsed100ns =
            (ULONGLONG)(
                ((q1.QuadPart - q0.QuadPart) * 10000000.0) /
                (double)qpf.QuadPart);
    }

    LogThreadSnapshot(
        "8-second process CPU sample",
        before,
        beforeCount,
        after,
        afterCount,
        elapsed100ns);

    Log("\r\nEND\r\n");

    CloseHandle(g_log);
    g_log = NULL;

    return 0;
}

extern "C" BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD reason,
    LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);

        // Only create the worker. All actual analysis occurs after
        // DLL_PROCESS_ATTACH has returned.
        HANDLE h = CreateThread(
            NULL,
            0,
            AnalyzerThread,
            NULL,
            0,
            NULL);

        if (h)
            CloseHandle(h);
    }

    return TRUE;
}

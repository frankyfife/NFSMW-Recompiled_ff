// Sampling profiler for one thread of a running process (no admin rights).
//
//   sampler.exe <process.exe> <thread name substring> <seconds> [out.txt]
//
// Suspends the thread about 2000 times per second, reads its instruction
// pointer and resumes it. At the end the addresses are resolved to functions
// with DbgHelp (PDBs next to the DLLs) and written as a table: self time per
// function and per module. Built for the GPU command processor thread
// ("GPU Commands") of nfsmw.exe, where ETW profilers would need admin rights.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

static DWORD FindProcess(const wchar_t* name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W pe = {sizeof(pe)};
  DWORD pid = 0;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (_wcsicmp(pe.szExeFile, name) == 0) {
      pid = pe.th32ProcessID;
      break;
    }
  }
  CloseHandle(snap);
  return pid;
}

static HANDLE FindThread(DWORD pid, const wchar_t* part) {
  using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  auto get_desc = reinterpret_cast<GetDescFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te = {sizeof(te)};
  HANDLE found = nullptr;
  for (BOOL ok = Thread32First(snap, &te); ok && !found; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME |
                              THREAD_GET_CONTEXT,
                          FALSE, te.th32ThreadID);
    if (!t) continue;
    PWSTR desc = nullptr;
    if (get_desc && SUCCEEDED(get_desc(t, &desc)) && desc) {
      if (wcsstr(desc, part)) {
        wprintf(L"thread %lu: %s\n", te.th32ThreadID, desc);
        found = t;
      }
      LocalFree(desc);
    }
    if (found != t) CloseHandle(t);
  }
  CloseHandle(snap);
  return found;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    fwprintf(stderr, L"usage: sampler <process.exe> <thread name part> <seconds> [out.txt]\n");
    return 2;
  }
  const DWORD pid = FindProcess(argv[1]);
  if (!pid) {
    fwprintf(stderr, L"process %s not found\n", argv[1]);
    return 1;
  }
  HANDLE thread = FindThread(pid, argv[2]);
  if (!thread) {
    fwprintf(stderr, L"no thread named like '%s'\n", argv[2]);
    return 1;
  }
  const double seconds = _wtof(argv[3]);

  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  LARGE_INTEGER freq, start, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  std::unordered_map<DWORD64, uint32_t> hits;
  uint64_t samples = 0;
  do {
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT ctx = {};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(thread, &ctx)) {
        ++hits[ctx.Rip];
        ++samples;
      }
      ResumeThread(thread);
    }
    LARGE_INTEGER due;
    due.QuadPart = -5000;  // 500 us, relative, in 100 ns units
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
    QueryPerformanceCounter(&now);
  } while (double(now.QuadPart - start.QuadPart) / double(freq.QuadPart) < seconds);
  CloseHandle(thread);

  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitializeW(process, nullptr, TRUE);

  std::map<std::string, uint64_t> by_func, by_module;
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
  for (const auto& [addr, n] : hits) {
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    IMAGEHLP_MODULE64 mod = {sizeof(mod)};
    std::string module = SymGetModuleInfo64(process, addr, &mod) ? mod.ModuleName : "?";
    std::string func;
    DWORD64 disp = 0;
    if (SymFromAddr(process, addr, &disp, sym)) {
      func = module + "!" + sym->Name;
    } else {
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "%s!0x%llx", module.c_str(), (unsigned long long)addr);
      func = tmp;
    }
    by_func[func] += n;
    by_module[module] += n;
  }
  SymCleanup(process);
  CloseHandle(process);

  FILE* out = argc > 4 ? _wfopen(argv[4], L"w") : stdout;
  fprintf(out, "%llu samples in %.1f s\n\nby module:\n", (unsigned long long)samples, seconds);
  std::vector<std::pair<uint64_t, std::string>> v;
  for (auto& [k, n] : by_module) v.push_back({n, k});
  std::sort(v.rbegin(), v.rend());
  for (auto& [n, k] : v) fprintf(out, "  %6.2f%%  %s\n", 100.0 * n / samples, k.c_str());
  v.clear();
  for (auto& [k, n] : by_func) v.push_back({n, k});
  std::sort(v.rbegin(), v.rend());
  fprintf(out, "\nby function (self time):\n");
  for (size_t i = 0; i < v.size() && i < 80; ++i) {
    fprintf(out, "  %6.2f%%  %s\n", 100.0 * v[i].first / samples, v[i].second.c_str());
  }
  if (out != stdout) fclose(out);
  return 0;
}

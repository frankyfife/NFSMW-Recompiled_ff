// Crash report for host crashes (Windows): the exception and a symbolized
// stack of the crashing thread go to nfsmw_crash.log in the working directory
// (the PDBs next to the executables give the names). Covers unhandled
// exceptions, abort() and std::terminate; a __fastfail does not reach it.

#if defined(_WIN32)

#include <windows.h>
#include <dbghelp.h>

#include <csignal>
#include <cstdio>
#include <exception>
#include <mutex>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::mutex g_mutex;

void WriteStack(FILE* out, HANDLE thread, CONTEXT context) {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(process, nullptr, TRUE);
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        !frame.AddrPC.Offset) {
      break;
    }
    const DWORD64 address = frame.AddrPC.Offset;
    char module_name[MAX_PATH] = "?";
    const DWORD64 module_base = SymGetModuleBase64(process, address);
    if (module_base) {
      GetModuleFileNameA(HMODULE(module_base), module_name, MAX_PATH);
    }
    const char* short_name = strrchr(module_name, '\\');
    short_name = short_name ? short_name + 1 : module_name;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line = {sizeof(IMAGEHLP_LINE64)};
    DWORD line_displacement = 0;
    const bool has_symbol = SymFromAddr(process, address, &displacement, symbol);
    const bool has_line = SymGetLineFromAddr64(process, address, &line_displacement, &line);
    fprintf(out, "  #%02d %s+0x%llx %s+0x%llx", i, short_name,
            (unsigned long long)(address - module_base), has_symbol ? symbol->Name : "?",
            (unsigned long long)displacement);
    if (has_line) {
      fprintf(out, " (%s:%lu)", line.FileName, line.LineNumber);
    }
    fprintf(out, "\n");
  }
}

void Report(const char* what, const EXCEPTION_RECORD* record, const CONTEXT& context) {
  std::lock_guard<std::mutex> lock(g_mutex);
  FILE* out = fopen("nfsmw_crash.log", "a");
  if (!out) {
    return;
  }
  SYSTEMTIME time;
  GetLocalTime(&time);
  fprintf(out, "%04u-%02u-%02u %02u:%02u:%02u thread %lu: %s", time.wYear, time.wMonth, time.wDay,
          time.wHour, time.wMinute, time.wSecond, GetCurrentThreadId(), what);
  if (record) {
    fprintf(out, " code 0x%08lx at %p", record->ExceptionCode, record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
      fprintf(out, " (%s 0x%llx)", record->ExceptionInformation[0] ? "write" : "read",
              (unsigned long long)record->ExceptionInformation[1]);
    }
  }
  fprintf(out, "\n");
  WriteStack(out, GetCurrentThread(), context);
  fclose(out);
}

void ReportHere(const char* what) {
  CONTEXT context;
  RtlCaptureContext(&context);
  Report(what, nullptr, context);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
  Report("unhandled exception", info->ExceptionRecord, *info->ContextRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}

struct Install {
  Install() {
    SetUnhandledExceptionFilter(UnhandledFilter);
    std::signal(SIGABRT, [](int) { ReportHere("abort"); });
    std::set_terminate([] {
      ReportHere("std::terminate");
      std::abort();
    });
  }
} g_install;

}  // namespace

#endif

// Prints exception code/address (module+offset) of a minidump. Read-only.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <string>
#pragma comment(lib, "dbghelp.lib")

static std::wstring Str(BYTE* base, RVA rva) {
    auto* s = reinterpret_cast<MINIDUMP_STRING*>(base + rva);
    return std::wstring(s->Buffer, s->Length / 2);
}

int wmain(int argc, wchar_t** argv) {
    for (int a = 1; a < argc; ++a) {
        HANDLE f = CreateFileW(argv[a], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        auto* base = static_cast<BYTE*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
        wprintf(L"== %s\n", argv[a]);
        MINIDUMP_DIRECTORY* dir; void* p; ULONG sz;
        MINIDUMP_MODULE_LIST* mods = nullptr;
        if (MiniDumpReadDumpStream(base, ModuleListStream, &dir, &p, &sz)) mods = (MINIDUMP_MODULE_LIST*)p;
        if (MiniDumpReadDumpStream(base, ExceptionStream, &dir, &p, &sz)) {
            auto* ex = (MINIDUMP_EXCEPTION_STREAM*)p;
            ULONG64 addr = ex->ExceptionRecord.ExceptionAddress;
            wprintf(L"thread %u code 0x%08X addr 0x%llX", ex->ThreadId, ex->ExceptionRecord.ExceptionCode, addr);
            for (int i = 0; i < (int)ex->ExceptionRecord.NumberParameters && i < 3; ++i)
                wprintf(L" p%d=0x%llX", i, ex->ExceptionRecord.ExceptionInformation[i]);
            wprintf(L"\n");
            bool found = false;
            for (ULONG i = 0; mods && i < mods->NumberOfModules; ++i) {
                auto& md = mods->Modules[i];
                if (addr >= md.BaseOfImage && addr < md.BaseOfImage + md.SizeOfImage) {
                    wprintf(L"  in %s +0x%llX\n", Str(base, md.ModuleNameRva).c_str(), addr - md.BaseOfImage);
                    found = true;
                }
            }
            if (!found) wprintf(L"  in no module (freed/unloaded code?)\n");
        } else {
            wprintf(L"no exception stream\n");
        }
        // Unloaded modules that covered the address help with "freed code".
        if (MiniDumpReadDumpStream(base, UnloadedModuleListStream, &dir, &p, &sz)) {
            auto* ul = (MINIDUMP_UNLOADED_MODULE_LIST*)p;
            auto* e = (MINIDUMP_UNLOADED_MODULE*)((BYTE*)ul + ul->SizeOfHeader);
            for (ULONG i = 0; i < ul->NumberOfEntries; ++i)
                wprintf(L"  unloaded: %s base 0x%llX size 0x%X\n", Str(base, e[i].ModuleNameRva).c_str(),
                        e[i].BaseOfImage, e[i].SizeOfImage);
        }
        UnmapViewOfFile(base); CloseHandle(m); CloseHandle(f);
    }
}

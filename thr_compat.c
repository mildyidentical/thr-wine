// SPDX-License-Identifier: 0BSD
/*
 * thr_compat: runtime compatibility shim for running THR Remote under Wine.
 *
 * Built as a stand-in "version.dll" that is placed next to THR Remote.exe and loaded through a Wine
 * DLL override. It does not modify any file of the application: when the engine DLL is loaded, it
 * redirects a few of that DLL's imported Windows functions (in memory only) to the replacements below,
 * which paper over differences between Windows and Wine. Everything else is forwarded unchanged.
 *
 * Replacements:
 *   SetupDiGetClassDevsW / SetupDiEnumDeviceInfo
 *       Windows publishes every USB device under the USB_DEVICE interface GUID; Wine does not. Enumerate
 *       Wine's "USB" enumerator instead and hide composite-child nodes ("...&MI_xx").
 *   midiInMessage / midiOutMessage
 *       Wine's ALSA MIDI driver lacks DRV_QUERYDEVICEINTERFACE(SIZE). Report a USB interface path for
 *       the amp's MIDI port so the application can match the port to the USB device.
 *   midiInClose
 *       Wine frees the handle even when midiInClose fails with MIDIERR_STILLPLAYING, leaving the ALSA
 *       port open. Reset (return queued buffers) before closing.
 *
 * Set THR_COMPAT_LOG=<path> to write a trace.
 */
#include <windows.h>
#include <mmsystem.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

/* ---------------------------------------------------------------- logging */

static void logf_(const char *fmt, ...) {
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("THR_COMPAT_LOG", path, sizeof path)) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

static FARPROC sys_proc(const wchar_t *dll, const char *name) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    return m ? GetProcAddress(m, name) : NULL;
}

/* ------------------------------------------------- SetupAPI replacements */

static const GUID USBDEV = {0xa5dcbf10, 0x6530, 0x11d2, {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};
static HDEVINFO g_usb_list;

static HDEVINFO WINAPI my_SetupDiGetClassDevsW(const GUID *g, PCWSTR en, HWND h, DWORD flags) {
    typedef HDEVINFO (WINAPI *fn)(const GUID *, PCWSTR, HWND, DWORD);
    fn real = (fn)sys_proc(L"setupapi.dll", "SetupDiGetClassDevsW");
    if (g && !memcmp(g, &USBDEV, sizeof *g) && (flags & DIGCF_DEVICEINTERFACE)) {
        HDEVINFO r = real(NULL, L"USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        g_usb_list = r;
        logf_("SetupDiGetClassDevsW(USB_DEVICE, %lx) -> enumerator USB %p\n", flags, r);
        return r;
    }
    return real(g, en, h, flags);
}

static BOOL WINAPI my_SetupDiEnumDeviceInfo(HDEVINFO h, DWORD idx, PSP_DEVINFO_DATA d) {
    typedef BOOL (WINAPI *fn_enum)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
    typedef BOOL (WINAPI *fn_iid)(HDEVINFO, PSP_DEVINFO_DATA, PWSTR, DWORD, PDWORD);
    fn_enum real = (fn_enum)sys_proc(L"setupapi.dll", "SetupDiEnumDeviceInfo");
    if (h != g_usb_list || !g_usb_list) return real(h, idx, d);
    fn_iid iid = (fn_iid)sys_proc(L"setupapi.dll", "SetupDiGetDeviceInstanceIdW");
    DWORD seen = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVINFO_DATA tmp; tmp.cbSize = sizeof tmp;
        if (!real(h, i, &tmp)) return FALSE;
        WCHAR id[256] = {0};
        if (iid(h, &tmp, id, 255, NULL) && wcsstr(id, L"&MI_")) continue;
        if (seen++ == idx) { *d = tmp; return TRUE; }
    }
}

/* ------------------------------------------------------ winmm replacements */

#define DRV_QUERYDEVICEINTERFACE_     0x80C
#define DRV_QUERYDEVICEINTERFACESIZE_ 0x80D

/* What the Windows USB audio driver reports for the amp's MIDI streaming interface. */
static const wchar_t *THR_PATH =
    L"\\\\?\\usb#vid_0499&pid_7182&mi_01#6&1a2b3c4d&0&0001#{6994ad04-93ef-11d0-a3cc-00a0c9223196}";

static int is_thr_port(int out, UINT id) {
    wchar_t name[32] = {0};
    if (out) { MIDIOUTCAPSW c; if (midiOutGetDevCapsW(id, &c, sizeof c)) return 0; wcsncpy(name, c.szPname, 31); }
    else     { MIDIINCAPSW  c; if (midiInGetDevCapsW(id, &c, sizeof c))  return 0; wcsncpy(name, c.szPname, 31); }
    return wcsstr(name, L"THR") != NULL;
}

static int query_interface(int out, UINT id, UINT msg, DWORD_PTR d1, DWORD_PTR d2, MMRESULT *res) {
    if (msg != DRV_QUERYDEVICEINTERFACE_ && msg != DRV_QUERYDEVICEINTERFACESIZE_) return 0;
    if (!is_thr_port(out, id)) return 0;
    ULONG bytes = (ULONG)((wcslen(THR_PATH) + 1) * sizeof(wchar_t));
    if (msg == DRV_QUERYDEVICEINTERFACESIZE_) {
        *(ULONG *)d1 = bytes;
    } else if (d2 < bytes) {
        *res = MMSYSERR_INVALPARAM; return 1;
    } else {
        memcpy((void *)d1, THR_PATH, bytes);
        logf_("%s port %u: interface path reported\n", out ? "out" : "in", id);
    }
    *res = MMSYSERR_NOERROR;
    return 1;
}

static MMRESULT WINAPI my_midiOutMessage(HMIDIOUT h, UINT msg, DWORD_PTR d1, DWORD_PTR d2) {
    typedef MMRESULT (WINAPI *fn)(HMIDIOUT, UINT, DWORD_PTR, DWORD_PTR);
    MMRESULT r;
    if (query_interface(1, (UINT)(UINT_PTR)h, msg, d1, d2, &r)) return r;
    return ((fn)sys_proc(L"winmm.dll", "midiOutMessage"))(h, msg, d1, d2);
}

static MMRESULT WINAPI my_midiInMessage(HMIDIIN h, UINT msg, DWORD_PTR d1, DWORD_PTR d2) {
    typedef MMRESULT (WINAPI *fn)(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR);
    MMRESULT r;
    if (query_interface(0, (UINT)(UINT_PTR)h, msg, d1, d2, &r)) return r;
    return ((fn)sys_proc(L"winmm.dll", "midiInMessage"))(h, msg, d1, d2);
}

static MMRESULT WINAPI my_midiInClose(HMIDIIN h) {
    typedef MMRESULT (WINAPI *fn)(HMIDIIN);
    ((fn)sys_proc(L"winmm.dll", "midiInReset"))(h);
    return ((fn)sys_proc(L"winmm.dll", "midiInClose"))(h);
}

/* ------------------------------------------------------ in-memory IAT patch */

static const struct { const char *dll, *func; void *hook; } HOOKS[] = {
    {"SETUPAPI.dll", "SetupDiGetClassDevsW",  my_SetupDiGetClassDevsW},
    {"SETUPAPI.dll", "SetupDiEnumDeviceInfo", my_SetupDiEnumDeviceInfo},
    {"WINMM.dll",    "midiInMessage",         my_midiInMessage},
    {"WINMM.dll",    "midiOutMessage",        my_midiOutMessage},
    {"WINMM.dll",    "midiInClose",           my_midiInClose},
};

static void patch_imports(HMODULE mod) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(base + d->Name);
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char *func = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            for (size_t i = 0; i < sizeof HOOKS / sizeof *HOOKS; i++) {
                if (_stricmp(dll, HOOKS[i].dll) || strcmp(func, HOOKS[i].func)) continue;
                DWORD old;
                if (VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    iat->u1.Function = (ULONG_PTR)HOOKS[i].hook;
                    VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                    logf_("hooked %s!%s\n", dll, func);
                }
            }
        }
    }
}

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } LDR_USTR;
typedef struct { ULONG Flags; const LDR_USTR *FullDllName; const LDR_USTR *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } LDR_LOADED;
typedef VOID (CALLBACK *LDR_NOTIFY)(ULONG reason, const LDR_LOADED *data, PVOID ctx);

static const wchar_t *ENGINE = L"APIV3EngineDLL.dll";

static VOID CALLBACK on_dll_load(ULONG reason, const LDR_LOADED *data, PVOID ctx) {
    if (reason != 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */) return;
    if (data->BaseDllName->Length / sizeof(wchar_t) != wcslen(ENGINE)) return;
    if (_wcsnicmp(data->BaseDllName->Buffer, ENGINE, wcslen(ENGINE))) return;
    patch_imports((HMODULE)data->DllBase);
}

/* ----------------------------------------- forwarders for the real version.dll */

static FARPROC real_version(const char *name) {
    static HMODULE h;
    if (!h) {
        wchar_t p[MAX_PATH];
        GetSystemDirectoryW(p, MAX_PATH);
        wcscat(p, L"\\version.dll");
        h = LoadLibraryW(p);
    }
    return GetProcAddress(h, name);
}

/* Every version.dll entry point takes only pointer-sized stdcall arguments, so one wrapper per arity works. */
#define A2 DWORD_PTR a, DWORD_PTR b
#define A3 A2, DWORD_PTR c
#define A4 A3, DWORD_PTR d
#define A5 A4, DWORD_PTR e
#define A8 A5, DWORD_PTR f, DWORD_PTR g, DWORD_PTR h
#define V2 a, b
#define V3 V2, c
#define V4 V3, d
#define V5 V4, e
#define V8 V5, f, g, h
#define FWD(name, n) \
    DWORD_PTR WINAPI fwd_##name(A##n) { \
        typedef DWORD_PTR (WINAPI *fn)(A##n); \
        return ((fn)real_version(#name))(V##n); }

FWD(GetFileVersionInfoA, 4)
FWD(GetFileVersionInfoW, 4)
FWD(GetFileVersionInfoExA, 5)
FWD(GetFileVersionInfoExW, 5)
FWD(GetFileVersionInfoSizeA, 2)
FWD(GetFileVersionInfoSizeW, 2)
FWD(GetFileVersionInfoSizeExA, 3)
FWD(GetFileVersionInfoSizeExW, 3)
FWD(VerQueryValueA, 4)
FWD(VerQueryValueW, 4)
FWD(VerFindFileA, 8)
FWD(VerFindFileW, 8)
FWD(VerInstallFileA, 8)
FWD(VerInstallFileW, 8)
FWD(VerLanguageNameA, 3)
FWD(VerLanguageNameW, 3)

/* ------------------------------------------------------------------ entry */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        typedef LONG (NTAPI *reg_fn)(ULONG, LDR_NOTIFY, PVOID, PVOID *);
        DisableThreadLibraryCalls(inst);
        reg_fn reg = (reg_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
        PVOID cookie;
        if (reg) reg(0, on_dll_load, NULL, &cookie);
        /* If the engine is somehow already loaded, patch it now. */
        HMODULE engine = GetModuleHandleW(ENGINE);
        if (engine) patch_imports(engine);
        logf_("thr_compat loaded\n");
    }
    return TRUE;
}

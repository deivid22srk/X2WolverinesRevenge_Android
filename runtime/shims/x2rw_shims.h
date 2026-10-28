/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * Camada de shims Win32 (interna).
 *
 * Este header é o "contrato interno" entre os arquivos de shim:
 *   - a LISTA MESTRA de símbolos importados (X-macros) e o registro estático
 *     `g_shims[]` montado a partir dela em x2rw_imports.c;
 *   - as assinaturas de TODOS os handlers (geradas da mesma lista: uma fonte
 *     única de verdade — impossível registrar um handler que não existe);
 *   - os helpers compartilhados (leitura de argumentos, strings do guest,
 *     caminhos Win32 -> host, tabela de erros, ponte de COM sintético);
 *   - as interfaces de HOOK que outro engenheiro deve implementar:
 *       x2rw_d3d8_hooks  (renderizador D3D8 -> Vulkan/GLES)
 *       x2rw_audio_hooks (DirectSound/waveOut -> SDL2/AAudio)
 *
 * Convenções de nível (ver política no README da camada):
 *   [REAL]    — semântica correta dentro do modelo de memória do guest;
 *   [PARCIAL] — progresso possível, sabidamente incompleto;
 *   [STUB]    — valor sintético benigno, logado, sem renderizar/tocar.
 * O nível de cada símbolo está no 5º parâmetro da lista mestra e é contado em
 * tempo de execução por x2rw_host_register_shims() (contagens auditáveis).
 * ============================================================================
 */
#ifndef X2RW_SHIMS_H
#define X2RW_SHIMS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "x2rw_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Registro de shims                                                  */
/* ------------------------------------------------------------------ */

/* Níveis de implementação (usados no 5º campo da lista mestra). */
enum {
    X2RW_LEVEL_REAL = 0,
    X2RW_LEVEL_PARCIAL = 1,
    X2RW_LEVEL_STUB = 2
};

typedef struct {
    const char *dll;    /* nome da DLL como aparece na IAT ("KERNEL32.dll") */
    const char *name;   /* nome do export, exatamente como importado         */
    x2rw_api_t  fn;     /* handler                                          */
    int         nargs;  /* nº de argumentos de 4 bytes (stdcall: callee pop) */
} x2rw_shim_t;

/* Teto rígido do registro. 226 símbolos únicos + folga para exports
 * resolvidos dinamicamente por GetProcAddress. */
#define X2RW_MAX_SHIMS 320

/* Endereços VA sintéticos (não são memória real: o dispatcher os reconhece).
 *   X2RW_COM_BASE    — thunks de métodos COM (D3D8/DInput8/DSound) alocados
 *                      por x2rw_com_register();
 *   X2RW_EXPORT_BASE — thunks devolvidos por GetProcAddress(); o índice
 *                      codifica a posição do handler em g_shims[]. */
#define X2RW_COM_BASE    0x60000000u
#define X2RW_COM_LIMIT   0x61000000u
#define X2RW_EXPORT_BASE 0x61000000u
#define X2RW_EXPORT_LIMIT 0x62000000u

/* ------------------------------------------------------------------ */
/* Lista mestra dos 226 símbolos importados                           */
/* ------------------------------------------------------------------ */
/* X(dll, nome, handler, nargs, nível)
 *
 * Esta lista é a ÚNICA fonte de verdade: em x2rw_imports.c ela vira
 * `g_shims[]`; aqui embaixo vira as declarações de todos os handlers.
 * nargs precisa ser o número REAL de parâmetros de 4 bytes da API, porque
 * stdcall é quem limpa a pilha (o dispatcher faz esp += 4*nargs).         */

/* --- KERNEL32.dll: núcleo (arquivo x2rw_kernel32.c) --- */
#define X2RW_SHIMS_KERNEL32(X) \
    X("KERNEL32.dll", "CloseHandle", k32_CloseHandle, 1, REAL) \
    X("KERNEL32.dll", "CreateDirectoryA", k32_CreateDirectoryA, 2, REAL) \
    X("KERNEL32.dll", "CreateEventA", k32_CreateEventA, 4, REAL) \
    X("KERNEL32.dll", "CreateFileA", k32_CreateFileA, 7, REAL) \
    X("KERNEL32.dll", "CreateFileMappingA", k32_CreateFileMappingA, 6, PARCIAL) \
    X("KERNEL32.dll", "CreateFileW", k32_CreateFileW, 7, REAL) \
    X("KERNEL32.dll", "CreateMutexA", k32_CreateMutexA, 3, REAL) \
    X("KERNEL32.dll", "CreateThread", k32_CreateThread, 6, PARCIAL) \
    X("KERNEL32.dll", "DeleteCriticalSection", k32_DeleteCriticalSection, 1, REAL) \
    X("KERNEL32.dll", "DeleteFileA", k32_DeleteFileA, 1, REAL) \
    X("KERNEL32.dll", "EnterCriticalSection", k32_EnterCriticalSection, 1, REAL) \
    X("KERNEL32.dll", "ExitProcess", k32_ExitProcess, 1, REAL) \
    X("KERNEL32.dll", "ExitThread", k32_ExitThread, 1, REAL) \
    X("KERNEL32.dll", "FatalAppExitA", k32_FatalAppExitA, 2, REAL) \
    X("KERNEL32.dll", "FileTimeToLocalFileTime", k32_FileTimeToLocalFileTime, 2, REAL) \
    X("KERNEL32.dll", "FileTimeToSystemTime", k32_FileTimeToSystemTime, 2, REAL) \
    X("KERNEL32.dll", "FindClose", k32_FindClose, 1, REAL) \
    X("KERNEL32.dll", "FindFirstFileA", k32_FindFirstFileA, 2, REAL) \
    X("KERNEL32.dll", "FindNextFileA", k32_FindNextFileA, 2, REAL) \
    X("KERNEL32.dll", "FindResourceA", k32_FindResourceA, 3, PARCIAL) \
    X("KERNEL32.dll", "FindResourceW", k32_FindResourceW, 3, PARCIAL) \
    X("KERNEL32.dll", "FlushConsoleInputBuffer", k32_FlushConsoleInputBuffer, 1, PARCIAL) \
    X("KERNEL32.dll", "FlushFileBuffers", k32_FlushFileBuffers, 1, REAL) \
    X("KERNEL32.dll", "FreeEnvironmentStringsA", k32_FreeEnvironmentStringsA, 1, REAL) \
    X("KERNEL32.dll", "FreeEnvironmentStringsW", k32_FreeEnvironmentStringsW, 1, REAL) \
    X("KERNEL32.dll", "FreeLibrary", k32_FreeLibrary, 1, REAL) \
    X("KERNEL32.dll", "GetCommandLineA", k32_GetCommandLineA, 0, REAL) \
    X("KERNEL32.dll", "GetConsoleMode", k32_GetConsoleMode, 2, PARCIAL) \
    X("KERNEL32.dll", "GetConsoleScreenBufferInfo", k32_GetConsoleScreenBufferInfo, 2, PARCIAL) \
    X("KERNEL32.dll", "GetCurrentDirectoryA", k32_GetCurrentDirectoryA, 2, REAL) \
    X("KERNEL32.dll", "GetCurrentProcess", k32_GetCurrentProcess, 0, REAL) \
    X("KERNEL32.dll", "GetCurrentProcessId", k32_GetCurrentProcessId, 0, REAL) \
    X("KERNEL32.dll", "GetCurrentThread", k32_GetCurrentThread, 0, REAL) \
    X("KERNEL32.dll", "GetCurrentThreadId", k32_GetCurrentThreadId, 0, REAL) \
    X("KERNEL32.dll", "GetDiskFreeSpaceExA", k32_GetDiskFreeSpaceExA, 4, REAL) \
    X("KERNEL32.dll", "GetEnvironmentStrings", k32_GetEnvironmentStrings, 0, REAL) \
    X("KERNEL32.dll", "GetEnvironmentStringsW", k32_GetEnvironmentStringsW, 0, REAL) \
    X("KERNEL32.dll", "GetEnvironmentVariableA", k32_GetEnvironmentVariableA, 3, REAL) \
    X("KERNEL32.dll", "GetFileSize", k32_GetFileSize, 2, REAL) \
    X("KERNEL32.dll", "GetFileType", k32_GetFileType, 1, REAL) \
    X("KERNEL32.dll", "GetLastError", k32_GetLastError, 0, REAL) \
    X("KERNEL32.dll", "GetLocalTime", k32_GetLocalTime, 1, REAL) \
    X("KERNEL32.dll", "GetModuleFileNameA", k32_GetModuleFileNameA, 3, REAL) \
    X("KERNEL32.dll", "GetModuleHandleA", k32_GetModuleHandleA, 1, REAL) \
    X("KERNEL32.dll", "GetNumberOfConsoleInputEvents", k32_GetNumberOfConsoleInputEvents, 2, PARCIAL) \
    X("KERNEL32.dll", "GetPrivateProfileIntA", k32_GetPrivateProfileIntA, 4, REAL) \
    X("KERNEL32.dll", "GetPrivateProfileStringA", k32_GetPrivateProfileStringA, 6, REAL) \
    X("KERNEL32.dll", "GetProcAddress", k32_GetProcAddress, 2, REAL) \
    X("KERNEL32.dll", "GetStartupInfoA", k32_GetStartupInfoA, 1, REAL) \
    X("KERNEL32.dll", "GetStdHandle", k32_GetStdHandle, 1, PARCIAL) \
    X("KERNEL32.dll", "GetSystemDirectoryA", k32_GetSystemDirectoryA, 2, PARCIAL) \
    X("KERNEL32.dll", "GetSystemInfo", k32_GetSystemInfo, 1, REAL) \
    X("KERNEL32.dll", "GetSystemTime", k32_GetSystemTime, 1, REAL) \
    X("KERNEL32.dll", "GetSystemTimeAsFileTime", k32_GetSystemTimeAsFileTime, 1, REAL) \
    X("KERNEL32.dll", "GetTickCount", k32_GetTickCount, 0, REAL) \
    X("KERNEL32.dll", "GetTimeZoneInformation", k32_GetTimeZoneInformation, 1, REAL) \
    X("KERNEL32.dll", "GetVersion", k32_GetVersion, 0, REAL) \
    X("KERNEL32.dll", "GetVersionExA", k32_GetVersionExA, 1, REAL) \
    X("KERNEL32.dll", "GetWindowsDirectoryA", k32_GetWindowsDirectoryA, 2, PARCIAL) \
    X("KERNEL32.dll", "HeapAlloc", k32_HeapAlloc, 3, REAL) \
    X("KERNEL32.dll", "HeapCreate", k32_HeapCreate, 3, REAL) \
    X("KERNEL32.dll", "HeapDestroy", k32_HeapDestroy, 1, REAL) \
    X("KERNEL32.dll", "HeapFree", k32_HeapFree, 3, REAL) \
    X("KERNEL32.dll", "HeapReAlloc", k32_HeapReAlloc, 4, REAL) \
    X("KERNEL32.dll", "HeapSize", k32_HeapSize, 3, REAL) \
    X("KERNEL32.dll", "InitializeCriticalSection", k32_InitializeCriticalSection, 1, REAL) \
    X("KERNEL32.dll", "InterlockedDecrement", k32_InterlockedDecrement, 1, REAL) \
    X("KERNEL32.dll", "InterlockedIncrement", k32_InterlockedIncrement, 1, REAL) \
    X("KERNEL32.dll", "IsBadCodePtr", k32_IsBadCodePtr, 1, REAL) \
    X("KERNEL32.dll", "IsBadReadPtr", k32_IsBadReadPtr, 2, REAL) \
    X("KERNEL32.dll", "IsBadWritePtr", k32_IsBadWritePtr, 2, REAL) \
    X("KERNEL32.dll", "IsProcessorFeaturePresent", k32_IsProcessorFeaturePresent, 1, REAL) \
    X("KERNEL32.dll", "LeaveCriticalSection", k32_LeaveCriticalSection, 1, REAL) \
    X("KERNEL32.dll", "LoadLibraryA", k32_LoadLibraryA, 1, REAL) \
    X("KERNEL32.dll", "LoadResource", k32_LoadResource, 2, PARCIAL) \
    X("KERNEL32.dll", "LockResource", k32_LockResource, 1, PARCIAL) \
    X("KERNEL32.dll", "MapViewOfFile", k32_MapViewOfFile, 5, PARCIAL) \
    X("KERNEL32.dll", "MoveFileA", k32_MoveFileA, 2, REAL) \
    X("KERNEL32.dll", "OutputDebugStringA", k32_OutputDebugStringA, 1, REAL) \
    X("KERNEL32.dll", "PeekConsoleInputA", k32_PeekConsoleInputA, 4, PARCIAL) \
    X("KERNEL32.dll", "QueryPerformanceCounter", k32_QueryPerformanceCounter, 1, REAL) \
    X("KERNEL32.dll", "QueryPerformanceFrequency", k32_QueryPerformanceFrequency, 1, REAL) \
    X("KERNEL32.dll", "RaiseException", k32_RaiseException, 4, REAL) \
    X("KERNEL32.dll", "ReadConsoleInputA", k32_ReadConsoleInputA, 4, PARCIAL) \
    X("KERNEL32.dll", "ReadFile", k32_ReadFile, 5, REAL) \
    X("KERNEL32.dll", "ReleaseMutex", k32_ReleaseMutex, 1, REAL) \
    X("KERNEL32.dll", "ResumeThread", k32_ResumeThread, 1, PARCIAL) \
    X("KERNEL32.dll", "RtlUnwind", shim_unimplemented, 4, STUB) \
    X("KERNEL32.dll", "SetConsoleCtrlHandler", k32_SetConsoleCtrlHandler, 2, PARCIAL) \
    X("KERNEL32.dll", "SetConsoleMode", k32_SetConsoleMode, 2, PARCIAL) \
    X("KERNEL32.dll", "SetEndOfFile", k32_SetEndOfFile, 1, REAL) \
    X("KERNEL32.dll", "SetEnvironmentVariableA", k32_SetEnvironmentVariableA, 2, REAL) \
    X("KERNEL32.dll", "SetErrorMode", k32_SetErrorMode, 1, PARCIAL) \
    X("KERNEL32.dll", "SetEvent", k32_SetEvent, 1, REAL) \
    X("KERNEL32.dll", "SetFilePointer", k32_SetFilePointer, 4, REAL) \
    X("KERNEL32.dll", "SetHandleCount", k32_SetHandleCount, 1, STUB) \
    X("KERNEL32.dll", "SetLastError", k32_SetLastError, 1, REAL) \
    X("KERNEL32.dll", "SetStdHandle", k32_SetStdHandle, 2, STUB) \
    X("KERNEL32.dll", "SetThreadPriority", k32_SetThreadPriority, 2, PARCIAL) \
    X("KERNEL32.dll", "SetUnhandledExceptionFilter", k32_SetUnhandledExceptionFilter, 1, REAL) \
    X("KERNEL32.dll", "SizeofResource", k32_SizeofResource, 2, PARCIAL) \
    X("KERNEL32.dll", "Sleep", k32_Sleep, 1, REAL) \
    X("KERNEL32.dll", "TerminateProcess", k32_TerminateProcess, 2, REAL) \
    X("KERNEL32.dll", "TerminateThread", k32_TerminateThread, 2, PARCIAL) \
    X("KERNEL32.dll", "TlsAlloc", k32_TlsAlloc, 0, REAL) \
    X("KERNEL32.dll", "TlsFree", k32_TlsFree, 1, REAL) \
    X("KERNEL32.dll", "TlsGetValue", k32_TlsGetValue, 1, REAL) \
    X("KERNEL32.dll", "TlsSetValue", k32_TlsSetValue, 2, REAL) \
    X("KERNEL32.dll", "UnhandledExceptionFilter", k32_UnhandledExceptionFilter, 1, REAL) \
    X("KERNEL32.dll", "UnmapViewOfFile", k32_UnmapViewOfFile, 1, PARCIAL) \
    X("KERNEL32.dll", "VirtualAlloc", k32_VirtualAlloc, 4, REAL) \
    X("KERNEL32.dll", "VirtualFree", k32_VirtualFree, 3, REAL) \
    X("KERNEL32.dll", "VirtualProtect", k32_VirtualProtect, 4, REAL) \
    X("KERNEL32.dll", "VirtualQuery", k32_VirtualQuery, 3, REAL) \
    X("KERNEL32.dll", "WaitForMultipleObjects", k32_WaitForMultipleObjects, 4, REAL) \
    X("KERNEL32.dll", "WaitForSingleObject", k32_WaitForSingleObject, 2, REAL) \
    X("KERNEL32.dll", "WriteConsoleA", k32_WriteConsoleA, 5, PARCIAL) \
    X("KERNEL32.dll", "WriteFile", k32_WriteFile, 5, REAL) \
    X("KERNEL32.dll", "WritePrivateProfileStringA", k32_WritePrivateProfileStringA, 4, REAL)

/* --- KERNEL32.dll: CRT (string/locale) — arquivo x2rw_strings.c --- */
#define X2RW_SHIMS_STRINGS(X) \
    X("KERNEL32.dll", "CompareStringA", str_CompareStringA, 6, PARCIAL) \
    X("KERNEL32.dll", "CompareStringW", str_CompareStringW, 6, PARCIAL) \
    X("KERNEL32.dll", "EnumSystemLocalesA", str_EnumSystemLocalesA, 2, PARCIAL) \
    X("KERNEL32.dll", "GetACP", str_GetACP, 0, REAL) \
    X("KERNEL32.dll", "GetCPInfo", str_GetCPInfo, 2, PARCIAL) \
    X("KERNEL32.dll", "GetLocaleInfoA", str_GetLocaleInfoA, 4, PARCIAL) \
    X("KERNEL32.dll", "GetLocaleInfoW", str_GetLocaleInfoW, 4, PARCIAL) \
    X("KERNEL32.dll", "GetOEMCP", str_GetOEMCP, 0, REAL) \
    X("KERNEL32.dll", "GetStringTypeA", str_GetStringTypeA, 5, PARCIAL) \
    X("KERNEL32.dll", "GetStringTypeW", str_GetStringTypeW, 5, PARCIAL) \
    X("KERNEL32.dll", "GetUserDefaultLCID", str_GetUserDefaultLCID, 0, PARCIAL) \
    X("KERNEL32.dll", "IsValidCodePage", str_IsValidCodePage, 1, PARCIAL) \
    X("KERNEL32.dll", "IsValidLocale", str_IsValidLocale, 2, PARCIAL) \
    X("KERNEL32.dll", "LCMapStringA", str_LCMapStringA, 6, PARCIAL) \
    X("KERNEL32.dll", "LCMapStringW", str_LCMapStringW, 6, PARCIAL) \
    X("KERNEL32.dll", "MultiByteToWideChar", str_MultiByteToWideChar, 6, REAL) \
    X("KERNEL32.dll", "WideCharToMultiByte", str_WideCharToMultiByte, 8, REAL)

/* --- ADVAPI32.dll (registro) — arquivo x2rw_advapi.c --- */
#define X2RW_SHIMS_ADVAPI32(X) \
    X("ADVAPI32.dll", "RegCloseKey", adv_RegCloseKey, 1, PARCIAL) \
    X("ADVAPI32.dll", "RegOpenKeyA", adv_RegOpenKeyA, 3, PARCIAL) \
    X("ADVAPI32.dll", "RegOpenKeyExA", adv_RegOpenKeyExA, 5, PARCIAL) \
    X("ADVAPI32.dll", "RegQueryValueExA", adv_RegQueryValueExA, 6, PARCIAL)

/* --- USER32.dll (janela) — arquivo x2rw_user32.c --- */
#define X2RW_SHIMS_USER32(X) \
    X("USER32.dll", "AdjustWindowRect", u32_AdjustWindowRect, 3, STUB) \
    X("USER32.dll", "BeginPaint", u32_BeginPaint, 2, STUB) \
    X("USER32.dll", "ChangeDisplaySettingsA", u32_ChangeDisplaySettingsA, 2, STUB) \
    X("USER32.dll", "ClientToScreen", u32_ClientToScreen, 2, STUB) \
    X("USER32.dll", "CreateWindowExA", u32_CreateWindowExA, 12, STUB) \
    X("USER32.dll", "DefWindowProcA", u32_DefWindowProcA, 4, STUB) \
    X("USER32.dll", "DestroyWindow", u32_DestroyWindow, 1, STUB) \
    X("USER32.dll", "DispatchMessageA", u32_DispatchMessageA, 1, STUB) \
    X("USER32.dll", "EndPaint", u32_EndPaint, 2, STUB) \
    X("USER32.dll", "GetActiveWindow", u32_GetActiveWindow, 0, STUB) \
    X("USER32.dll", "GetClassLongA", u32_GetClassLongA, 2, STUB) \
    X("USER32.dll", "GetClientRect", u32_GetClientRect, 2, STUB) \
    X("USER32.dll", "GetCursorPos", u32_GetCursorPos, 1, STUB) \
    X("USER32.dll", "GetDC", u32_GetDC, 1, STUB) \
    X("USER32.dll", "GetFocus", u32_GetFocus, 0, STUB) \
    X("USER32.dll", "GetSystemMetrics", u32_GetSystemMetrics, 1, STUB) \
    X("USER32.dll", "GetTopWindow", u32_GetTopWindow, 1, STUB) \
    X("USER32.dll", "GetWindow", u32_GetWindow, 2, STUB) \
    X("USER32.dll", "GetWindowLongA", u32_GetWindowLongA, 2, STUB) \
    X("USER32.dll", "GetWindowRect", u32_GetWindowRect, 2, STUB) \
    X("USER32.dll", "GetWindowThreadProcessId", u32_GetWindowThreadProcessId, 2, STUB) \
    X("USER32.dll", "IsWindowVisible", u32_IsWindowVisible, 1, STUB) \
    X("USER32.dll", "LoadCursorA", u32_LoadCursorA, 2, STUB) \
    X("USER32.dll", "LoadIconA", u32_LoadIconA, 2, STUB) \
    X("USER32.dll", "MessageBoxA", u32_MessageBoxA, 4, STUB) \
    X("USER32.dll", "MoveWindow", u32_MoveWindow, 6, STUB) \
    X("USER32.dll", "PeekMessageA", u32_PeekMessageA, 5, STUB) \
    X("USER32.dll", "RegisterClassA", u32_RegisterClassA, 1, STUB) \
    X("USER32.dll", "ReleaseDC", u32_ReleaseDC, 2, STUB) \
    X("USER32.dll", "ScreenToClient", u32_ScreenToClient, 2, STUB) \
    X("USER32.dll", "SetActiveWindow", u32_SetActiveWindow, 1, STUB) \
    X("USER32.dll", "SetCursor", u32_SetCursor, 1, STUB) \
    X("USER32.dll", "SetFocus", u32_SetFocus, 1, STUB) \
    X("USER32.dll", "SetForegroundWindow", u32_SetForegroundWindow, 1, STUB) \
    X("USER32.dll", "ShowCursor", u32_ShowCursor, 1, STUB) \
    X("USER32.dll", "ShowWindow", u32_ShowWindow, 2, STUB) \
    X("USER32.dll", "TranslateMessage", u32_TranslateMessage, 1, STUB) \
    X("USER32.dll", "UnregisterClassA", u32_UnregisterClassA, 2, STUB) \
    X("USER32.dll", "UpdateWindow", u32_UpdateWindow, 1, STUB)

/* --- GDI32.dll (superfícies em memória do guest) — arquivo x2rw_user32.c --- */
#define X2RW_SHIMS_GDI32(X) \
    X("GDI32.dll", "CreateCompatibleBitmap", gdi_CreateCompatibleBitmap, 3, STUB) \
    X("GDI32.dll", "CreateCompatibleDC", gdi_CreateCompatibleDC, 1, STUB) \
    X("GDI32.dll", "CreateDIBSection", gdi_CreateDIBSection, 6, PARCIAL) \
    X("GDI32.dll", "DeleteDC", gdi_DeleteDC, 1, STUB) \
    X("GDI32.dll", "DeleteObject", gdi_DeleteObject, 1, STUB) \
    X("GDI32.dll", "GetDIBits", gdi_GetDIBits, 7, PARCIAL) \
    X("GDI32.dll", "GetDeviceCaps", gdi_GetDeviceCaps, 2, PARCIAL) \
    X("GDI32.dll", "GetPixel", gdi_GetPixel, 3, PARCIAL) \
    X("GDI32.dll", "GetStockObject", gdi_GetStockObject, 1, STUB) \
    X("GDI32.dll", "SelectObject", gdi_SelectObject, 2, STUB) \
    X("GDI32.dll", "SetPixel", gdi_SetPixel, 4, PARCIAL) \
    X("GDI32.dll", "SetStretchBltMode", gdi_SetStretchBltMode, 2, STUB) \
    X("GDI32.dll", "StretchBlt", gdi_StretchBlt, 11, PARCIAL)

/* --- d3d8.dll (entry point; métodos via vtable COM) --- */
#define X2RW_SHIMS_D3D8(X) \
    X("d3d8.dll", "Direct3DCreate8", d3d_Direct3DCreate8, 1, PARCIAL)

/* --- DSOUND.dll + WINMM.dll — arquivo x2rw_audio.c --- */
#define X2RW_SHIMS_AUDIO(X) \
    X("DSOUND.dll", "ordinal_11", aud_DirectSoundCreate, 3, STUB) \
    X("WINMM.dll", "mmioAscend", aud_mmioAscend, 3, REAL) \
    X("WINMM.dll", "mmioClose", aud_mmioClose, 2, REAL) \
    X("WINMM.dll", "mmioDescend", aud_mmioDescend, 4, REAL) \
    X("WINMM.dll", "mmioOpenA", aud_mmioOpenA, 3, REAL) \
    X("WINMM.dll", "mmioRead", aud_mmioRead, 3, REAL) \
    X("WINMM.dll", "mmioStringToFOURCCA", aud_mmioStringToFOURCCA, 2, REAL) \
    X("WINMM.dll", "timeGetTime", aud_timeGetTime, 0, REAL) \
    X("WINMM.dll", "waveOutClose", aud_waveOutClose, 1, STUB) \
    X("WINMM.dll", "waveOutOpen", aud_waveOutOpen, 6, STUB) \
    X("WINMM.dll", "waveOutPause", aud_waveOutPause, 1, STUB) \
    X("WINMM.dll", "waveOutPrepareHeader", aud_waveOutPrepareHeader, 3, STUB) \
    X("WINMM.dll", "waveOutReset", aud_waveOutReset, 1, STUB) \
    X("WINMM.dll", "waveOutRestart", aud_waveOutRestart, 1, STUB) \
    X("WINMM.dll", "waveOutSetVolume", aud_waveOutSetVolume, 2, STUB) \
    X("WINMM.dll", "waveOutUnprepareHeader", aud_waveOutUnprepareHeader, 3, STUB) \
    X("WINMM.dll", "waveOutWrite", aud_waveOutWrite, 3, STUB)

/* --- DINPUT8.dll — arquivo x2rw_input.c --- */
#define X2RW_SHIMS_DINPUT8(X) \
    X("DINPUT8.dll", "DirectInput8Create", inp_DirectInput8Create, 5, PARCIAL)

/* --- binkw32.dll (nomes com sufixo @n, stdcall decorado) — x2rw_bink.c --- */
#define X2RW_SHIMS_BINK(X) \
    X("binkw32.dll", "_BinkClose@4", bnk_BinkClose, 1, STUB) \
    X("binkw32.dll", "_BinkCopyToBuffer@28", bnk_BinkCopyToBuffer, 7, STUB) \
    X("binkw32.dll", "_BinkDX8SurfaceType@4", bnk_BinkDX8SurfaceType, 1, STUB) \
    X("binkw32.dll", "_BinkDoFrame@4", bnk_BinkDoFrame, 1, STUB) \
    X("binkw32.dll", "_BinkGetError@0", bnk_BinkGetError, 0, STUB) \
    X("binkw32.dll", "_BinkLogoAddress@0", bnk_BinkLogoAddress, 0, STUB) \
    X("binkw32.dll", "_BinkNextFrame@4", bnk_BinkNextFrame, 1, STUB) \
    X("binkw32.dll", "_BinkOpen@8", bnk_BinkOpen, 2, STUB) \
    X("binkw32.dll", "_BinkOpenDirectSound@4", bnk_BinkOpenDirectSound, 1, STUB) \
    X("binkw32.dll", "_BinkPause@8", bnk_BinkPause, 2, STUB) \
    X("binkw32.dll", "_BinkSetSoundSystem@8", bnk_BinkSetSoundSystem, 2, STUB) \
    X("binkw32.dll", "_BinkSetSoundTrack@8", bnk_BinkSetSoundTrack, 2, STUB) \
    X("binkw32.dll", "_BinkSetVideoOnOff@8", bnk_BinkSetVideoOnOff, 2, STUB) \
    X("binkw32.dll", "_BinkSetVolume@12", bnk_BinkSetVolume, 3, STUB) \
    X("binkw32.dll", "_BinkWait@4", bnk_BinkWait, 1, STUB)

/* Concatenação de tudo: é daqui que sai g_shims[] e a lista de declarações. */
#define X2RW_SHIMS_ALL(X) \
    X2RW_SHIMS_KERNEL32(X) \
    X2RW_SHIMS_STRINGS(X) \
    X2RW_SHIMS_ADVAPI32(X) \
    X2RW_SHIMS_USER32(X) \
    X2RW_SHIMS_GDI32(X) \
    X2RW_SHIMS_D3D8(X) \
    X2RW_SHIMS_AUDIO(X) \
    X2RW_SHIMS_DINPUT8(X) \
    X2RW_SHIMS_BINK(X)

/* Declarações de TODOS os handlers (uma fonte de verdade). O handler
 * `shim_unimplemented` é declarado junto porque aparece na lista mestra. */
uint32_t shim_unimplemented(x2rw_ctx *c, uint32_t *args, int nargs);

/* Cuidado: o 4º parâmetro NÃO pode se chamar `nargs` aqui, senão o
 * pré-processador substituiria o nome do parâmetro da declaração pelo
 * número (ex.: `int 1`). */
#define X2RW_DECLARE(dll, name, fn, na, level) \
    uint32_t fn(x2rw_ctx *c, uint32_t *args, int nargs);
X2RW_SHIMS_ALL(X2RW_DECLARE)
#undef X2RW_DECLARE

/* ------------------------------------------------------------------ */
/* API pública da camada de shims                                     */
/* ------------------------------------------------------------------ */

/* Registro: só loga o total e as contagens por nível (o registro é estático;
 * não é preciso chamar isto para o dispatch funcionar). */
void x2rw_host_register_shims(void);

/* Despacho: x2rw_runtime.c chama isto quando `call` não resolve numa função
 * lifted. Devolve 1 se tratou um thunk conhecido, 0 se não é import. */
int x2rw_import_dispatch(x2rw_ctx *c, uint32_t va);

/* Variante de baixo nível: chama o handler associado a `va` com os argumentos
 * já marshallados. NÃO mexe em esp (o chamador é dono da pilha). Devolve 0 se
 * `va` não for um import conhecido. */
uint32_t x2rw_call_import(x2rw_ctx *c, uint32_t va, uint32_t *args, int nargs);

/* Número de símbolos no registro estático (226). */
int x2rw_import_shim_count(void);

/* Correção opcional da IAT: escreve em cada slot o VA do próprio slot.
 * NÃO é necessária (o despacho resolve RVA de hint/name e thunks), mas fica
 * disponível. O dispatcher só a aplica se X2RW_PATCH_IAT estiver definida. */
void x2rw_imports_patch_iat(x2rw_ctx *c);

/* GetProcAddress: devolve um VA sintético que despacha para o handler
 * (dll, nome) ou 0 se o export não for conhecido. */
uint32_t x2rw_import_lookup_export(const char *dll, const char *name);
/* O mesmo por ordinal (usado por dsound/ordinal_11). */
uint32_t x2rw_import_lookup_ordinal(const char *dll, uint32_t ordinal);

/* --- Ponte de COM sintético ---------------------------------------- *
 * O jogo chama métodos COM indiretamente: vtbl = *obj; call vtbl[4*i].
 * Como as vtables ficam na memória do guest, cada slot recebe um VA
 * sintético (X2RW_COM_BASE + 4*idx). O dispatcher reconhece esses VAs,
 * marshalla os argumentos e chama o handler registrado. Isso evita ter de
 * inventar código x86 na memória do guest. */
uint32_t x2rw_com_register(x2rw_api_t fn, int nargs);
/* Cria obj + vtbl na memória do guest: [0]=ptr vtbl; vtbl[i]=vtbl_vas[i]. */
uint32_t x2rw_com_object_create(x2rw_ctx *c, const uint32_t *vtbl_vas, uint32_t count);

/* ------------------------------------------------------------------ */
/* Helpers compartilhados                                             */
/* ------------------------------------------------------------------ */

/* Acesso em bloco à memória do guest (definidos em x2rw_runtime.c). */
void x2rw_mem_write(uint32_t va, const void *data, size_t size);
void x2rw_mem_read(uint32_t va, void *dst, size_t size);

/* Último erro (por thread, espelhado em pthread_key_t). */
uint32_t x2rw_last_error_get(void);
void     x2rw_last_error_set(uint32_t err);

/* Comprimento de string no guest (NUL-terminada) e cópia limitada. */
uint32_t x2rw_guest_strlen(x2rw_ctx *c, uint32_t va);
void     x2rw_guest_strcpy(x2rw_ctx *c, uint32_t dst, const char *src, size_t cap);

/* Aloca uma string NUL-terminada no heap do guest. */
uint32_t x2rw_guest_alloc_str(x2rw_ctx *c, const char *s);

/* CP-1252 <-> UTF-16 (a tabela real está em x2rw_strings.c). */
int x2rw_cp1252_to_utf16(const char *src, int srclen, uint16_t *dst, int dstcount);
int x2rw_utf16_to_cp1252(const uint16_t *src, int srclen, char *dst, int dstcount);
void x2rw_guest_write_utf16(x2rw_ctx *c, uint32_t va, const uint16_t *src, int count);

/* Converte um caminho Win32 ("C:\\Jogo\\x.bik", "\\\\?\\C:\\...", barras
 * invertidas) em caminho POSIX relativo ao diretório atual. */
void x2rw_win32_path_to_host(const char *win, char *out, size_t outlen);

/* Códigos de erro Win32 usados aqui. */
#define X2RW_ERROR_SUCCESS            0u
#define X2RW_ERROR_FILE_NOT_FOUND     2u
#define X2RW_ERROR_PATH_NOT_FOUND     3u
#define X2RW_ERROR_ACCESS_DENIED      5u
#define X2RW_ERROR_INVALID_HANDLE     6u
#define X2RW_ERROR_NOT_ENOUGH_MEMORY  8u
#define X2RW_ERROR_OUTOFMEMORY        14u
#define X2RW_ERROR_INVALID_PARAMETER  87u
#define X2RW_ERROR_INSUFFICIENT_BUFFER 122u
#define X2RW_ERROR_MOD_NOT_FOUND      126u
#define X2RW_ERROR_PROC_NOT_FOUND     127u
#define X2RW_ERROR_ALREADY_EXISTS     183u
#define X2RW_ERROR_ENVVAR_NOT_FOUND   203u
#define X2RW_ERROR_NO_MORE_FILES      18u
#define X2RW_ERROR_CALL_NOT_IMPLEMENTED 120u

/* Helpers de last-error por chamada. */
#define X2RW_FAIL(c, err) do { x2rw_last_error_set(err); return 0u; } while (0)
#define X2RW_OK() do { x2rw_last_error_set(X2RW_ERROR_SUCCESS); } while (0)

/* ------------------------------------------------------------------ */
/* Estado global compartilhado                                        */
/* ------------------------------------------------------------------ */

/* Superfície de render corrente (USER32/GDI32/D3D8). Default 1280x720. */
extern int32_t g_render_w;
extern int32_t g_render_h;

/* Estado de entrada escrito pelo overlay de toque do Android (x2rw_input.c).
 * x2rw_key_state: 256 bytes no formato DirectInput (bit 0x80 = pressionada).
 * x2rw_mouse_dx/dy: deslocamento relativo acumulado desde a última leitura.
 * x2rw_mouse_buttons: bit0 = botão 0, bit1 = botão 1, ... */
extern volatile uint8_t x2rw_key_state[256];
extern volatile int32_t x2rw_mouse_dx;
extern volatile int32_t x2rw_mouse_dy;
extern volatile int32_t x2rw_mouse_buttons;

/* ------------------------------------------------------------------ */
/* HOOKS: Direct3D 8                                                  */
/* ------------------------------------------------------------------ */
/* O renderizador real (dx8vk ou wrapper próprio D3D8->Vulkan/GLES) é outro
 * módulo. A camada de shim mantém os objetos/vtables/estado e delega todo o
 * trabalho de GPU a estes hooks. Todos devolvem 0 (D3D_OK) em caso de sucesso
 * e um HRESULT de erro em caso de falha; o hook default só loga e devolve
 * sucesso sem renderizar (execução headless). */
typedef struct {
    /* Chamado quando Direct3DCreate8() cria o objeto IDirect3D8. */
    int (*init)(x2rw_ctx *c, uint32_t d3d);
    /* Cria o device. `present_params_ptr` aponta para D3DPRESENT_PARAMETERS.
     * Deve devolver em *out_device o VA do objeto do renderizador ou 0. */
    int (*create_device)(x2rw_ctx *c, uint32_t d3d, uint32_t adapter,
                         uint32_t devtype, uint32_t hwnd, uint32_t flags,
                         uint32_t present_params_ptr, uint32_t *out_device);
    int (*reset)(x2rw_ctx *c, uint32_t device, uint32_t present_params_ptr);
    int (*present)(x2rw_ctx *c, uint32_t device);
    int (*clear)(x2rw_ctx *c, uint32_t device, uint32_t flags, uint32_t rects_ptr,
                 uint32_t color, float z, uint32_t stencil);
    int (*begin_scene)(x2rw_ctx *c, uint32_t device);
    int (*end_scene)(x2rw_ctx *c, uint32_t device);
    int (*set_render_state)(x2rw_ctx *c, uint32_t device, uint32_t state, uint32_t value);
    int (*set_texture)(x2rw_ctx *c, uint32_t device, uint32_t stage, uint32_t texture);
    int (*set_transform)(x2rw_ctx *c, uint32_t device, uint32_t state, uint32_t matrix_ptr);
    int (*set_stream_source)(x2rw_ctx *c, uint32_t device, uint32_t stream,
                             uint32_t vb, uint32_t stride);
    int (*set_indices)(x2rw_ctx *c, uint32_t device, uint32_t ib, uint32_t base_vertex);
    int (*set_vertex_shader)(x2rw_ctx *c, uint32_t device, uint32_t handle);
    int (*set_pixel_shader)(x2rw_ctx *c, uint32_t device, uint32_t handle);
    int (*set_viewport)(x2rw_ctx *c, uint32_t device, uint32_t viewport_ptr);
    int (*draw_primitive)(x2rw_ctx *c, uint32_t device, uint32_t prim,
                          uint32_t start, uint32_t count);
    int (*draw_indexed_primitive)(x2rw_ctx *c, uint32_t device, uint32_t prim,
                                  uint32_t min_index, uint32_t num_vertices,
                                  uint32_t start, uint32_t count);
    int (*draw_primitive_up)(x2rw_ctx *c, uint32_t device, uint32_t prim,
                             uint32_t count, uint32_t vertex_ptr, uint32_t stride);
    int (*draw_indexed_primitive_up)(x2rw_ctx *c, uint32_t device, uint32_t prim,
                                     uint32_t min_vertex, uint32_t num_vertices,
                                     uint32_t index_count, uint32_t index_ptr,
                                     uint32_t index_format, uint32_t vertex_ptr,
                                     uint32_t stride);
    int (*create_texture)(x2rw_ctx *c, uint32_t device, uint32_t width, uint32_t height,
                          uint32_t levels, uint32_t usage, uint32_t format,
                          uint32_t pool, uint32_t *out_texture);
    /* Blit final do back buffer para a tela: `src_ptr` aponta para pixels no
     * guest. Devolve 0 em sucesso. */
    int (*copy_to_screen)(x2rw_ctx *c, uint32_t device, uint32_t src_ptr,
                          uint32_t src_format, uint32_t src_width,
                          uint32_t src_height, uint32_t src_stride);
    void (*shutdown)(x2rw_ctx *c);
} x2rw_d3d8_hooks;

void x2rw_d3d8_set_hooks(const x2rw_d3d8_hooks *h);
/* Devolve os hooks instalados, ou NULL se nenhum foi (modo headless). */
const x2rw_d3d8_hooks *x2rw_d3d8_get_hooks(void);

/* ------------------------------------------------------------------ */
/* HOOKS: áudio (DirectSound + waveOut)                               */
/* ------------------------------------------------------------------ */
/* Nenhum som sai deste módulo: outro engenheiro liga estes hooks ao
 * SDL2/AAudio. O default consome os buffers e marca as WAVEHDR como
 * concluídas, sem tocar nada (headless). */
typedef struct {
    int (*init)(x2rw_ctx *c);
    int (*ds_create)(x2rw_ctx *c, uint32_t guid_ptr, uint32_t *out_ds);
    int (*ds_create_buffer)(x2rw_ctx *c, uint32_t ds, uint32_t desc_ptr,
                            uint32_t *out_buffer);
    int (*ds_buffer_play)(x2rw_ctx *c, uint32_t buffer, uint32_t flags,
                          uint32_t reserved1, uint32_t reserved2, uint32_t priority);
    int (*ds_buffer_stop)(x2rw_ctx *c, uint32_t buffer);
    /* Lock: devolve ponteiros (VA do guest) e tamanhos das duas regiões. */
    int (*ds_buffer_lock)(x2rw_ctx *c, uint32_t buffer, uint32_t offset,
                          uint32_t bytes, uint32_t *out_ptr1, uint32_t *out_bytes1,
                          uint32_t *out_ptr2, uint32_t *out_bytes2, uint32_t flags);
    int (*ds_buffer_unlock)(x2rw_ctx *c, uint32_t buffer);
    int (*ds_buffer_set_volume)(x2rw_ctx *c, uint32_t buffer, int32_t volume);
    int (*wave_out_open)(x2rw_ctx *c, uint32_t *out_handle, uint32_t device,
                         uint32_t format_ptr, uint32_t callback, uint32_t instance,
                         uint32_t flags);
    int (*wave_out_write)(x2rw_ctx *c, uint32_t handle, uint32_t hdr_ptr);
    int (*wave_out_reset)(x2rw_ctx *c, uint32_t handle);
    void (*shutdown)(x2rw_ctx *c);
} x2rw_audio_hooks;

void x2rw_audio_set_hooks(const x2rw_audio_hooks *h);
/* Devolve os hooks instalados, ou NULL se nenhum foi (modo headless). */
const x2rw_audio_hooks *x2rw_audio_get_hooks(void);

/* ------------------------------------------------------------------ */
/* Helper de argumentos (ABI stdcall do lifter)                       */
/* ------------------------------------------------------------------ */
/* `x2rw_call` empilha um endereço de retorno sintético (X2RW_RET_SENTINEL)
 * antes de entrar na função, exatamente como um `call` de verdade: ao entrar,
 * [esp] é o retorno e o primeiro argumento está em esp+4.
 *
 * A versão anterior lia o argumento em esp+0 (modelo "sem retorno"), o que
 * deslocava toda leitura de argumento do código liftado em 4 bytes — foi o
 * bloqueador medido em docs/TESTED.md §2.8. O dispatcher usa esta mesma
 * regra em invoke_from_stack(). */
static inline uint32_t x2rw_arg(x2rw_ctx *c, int i) {
    return x2rw_read32(c, c->r.esp + (uint32_t)(4 + 4 * i));
}

#ifdef __cplusplus
}
#endif

#endif /* X2RW_SHIMS_H */

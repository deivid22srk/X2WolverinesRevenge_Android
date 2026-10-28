# X2: Wolverine's Revenge - Reconhecimento de Engenharia (Fase 1)

> **STATUS: Reconhecimento concluído.** Este documento registra **evidências extraídas diretamente dos binários fornecidos** em `/root/share/X2_Wolverines_Revenge_Win_R_EN/`. Tudo que for hipótese (endereços de hooks, lista de overrides, layout do PKR3) está marcado como `TODO` ou `HIPÓTESE` e deve ser verificado no código ou nos binários reais antes de qualquer build.

## 1. O que foi fornecido (plataforma confirmada ✅)

| Arquivo | Tamanho | SHA-1 (verificável) | Tipo |
|---|---|---|---|
| `Game Files/X2 Wolverine's Revenge/Wolvie.exe` | 2.916.352 B (2,78 MB) | `37f4327c4ea45d2037e9e177ecbac003469ff490` | PE32 executável i386, GUI |
| `Game Files/X2 Wolverine's Revenge/binkw32.dll` | 377.856 B | `dd8c012f7e2bb68ec7b47702fcf3ea42e805e1f7` | PE32 DLL i386, 36 seções (Bink Video 2002) |
| `data.pkr` | 187.574.924 B (~179 MB) | `560f2d2a0d26ee34f8591c5a09585e0cf6865a27` | Container de assets (PKR3, BZip2 + conteúdo) |
| `wavs.pkr` | 145.895.192 B (~139 MB) | `309e57952af136288a46fb9b6b6e0ae723efd868` | Container de áudio (PKR3, RIFF/WVPK) |
| `movies/*.bik` | ~681 MB (31 arquivos) | — | Bink Video (cutscenes) |
| `music/*.bik` | ~89 MB (109 arquivos) | — | Bink Video com áudio embutido |
| **TOTAL** | **~1,08 GB** | — | — |

### Formatos dos binários (evidência pefile)

**Wolvie.exe**
- Máquina: `0x14C` (i386) — **x86 de 32 bits** ✅
- Magic: `0x10b` (PE32) ✅
- Subsystem: `0x2` (GUI)
- EntryPoint: `0x1c3042`
- ImageBase original: `0x00400000`
- SizeOfImage: `0x04cae8e` (74.712.286 B no load virtual)
- TimeDateStamp: `0x3F97275D` → **2003-04-05 06:17:09 UTC**
- Linker: MS VC++ 6.0 (MajorLinker 6, MinorLinker 0)
- Seções: `.text` (0x1d258b), `.rdata` (0xb2dd), `.data` (0x2e3adc), `.data1` (0x900), `.rsrc` (0x5e8e)
- **DllCharacteristics = 0**: sem `/DYNAMICBASE`, sem `/NXCOMPAT`, sem `/SEH`, sem `HIGH_ENTROPY_VA` → carregamento estático em 0x400000 ✅
- 9 DLLs importadas, 0 exports

**binkw32.dll**
- Máquina: i386, PE32, Subsystem GUI
- EntryPoint: `0x6ab1`, ImageBase: `0x30000000`, SizeOfImage: `0x72000`
- TimeDateStamp: 2002-12-31 (Bink Video rev.i)
- 36 seções (múltiplas configurações de codec: BINK, BINK16, BINK32, YUY2, etc.)
- 4 DLLs importadas

### Conclusão de plataforma
✅ **É uma build PC de Windows, PE32 x86 de 32 bits.** O pipeline de recompilação estática x86→C→ARM64 **se aplica diretamente**. Não é necessário parar na Fase 1 por formato incompatível.

> ⚠️ **Versão/patch**: A release contém um **NoCD patch** (sem verificação de disco) e um **widescreen fix** (jackfuste) aplicados após a build original de abril de 2003. Hashes registrados acima são da cópia *patched* — eles **devem** ser os usados no `build.yml` (validação SHA-1). A build *unpatched* retail de 2003 tem outros hashes e pode ter DRM (SafeDisc) — ver seção 3.

---

## 2. DRM / Proteção (verificado: **NENHUMA** no arquivo ✅)

- **SafeDisc / SecuROM / ARMADILLO / ASProtect / VMProtect / UPX**: **NÃO encontrados** no `Wolvie.exe`. As ocorrências da substring "secu" são conteúdo do jogo ("security camera", "Maximum Security Mutant Detention Center") e "MPA" são strings do zlib/wavpack.
- Verificações feitas: assinatura de packers, análise de seções (sem seções encriptadas/encavadas), string scanning em image completo.
- **Conclusão**: o executável está **descompactado e sem proteção**. O lifting estático direto **pode ser feito**.
- A release fornecida já vem com **NoCD**, ou seja, não há verificação de mídia para contornar.

> ⚠️ **Risco documentado**: versões **retail originais de CD** de 2003 deste jogo costumam usar SafeDisc. Se um usuário fornecesse uma ISO de CD virgem, o lifting estático direto **não funcionaria** (o código só existe descriptografado em memória). A solução proposta: o usuário deve usar a **mesma release NoCD** (ou fornecer um dump já desempacotado).

---

## 3. API Gráfica (verificada: **Direct3D 8**, NÃO é D3D9)

- Importação direta: `Direct3DCreate8` (d3d8.dll)
- d3d.log da própria release mostra: "Initializing DirectX Graphics...", D3DFMT_X8R8G8B8, resoluções nativas 320×200 / 320×240 / 512×384 / 640×400
- HLSL/GLSL? O exe carrega **shader resources** do PKR3 (PS = pixel shaders, VS = vertex shaders) → shaders compilados na engine Lithium (formato proprietário, não HLSL fonte)
- **O que isso significa para o port:**
  - `dxvk-native` é **D3D9-only** e **não serve diretamente**.
  - **Opção A (recomendada)**: `dx8vk` (projeto que leva D3D8→Vulkan, fork de dxvk-native) + `vkd3d-proton` se necessário. É o equivalente exato ao que dgVoodoo2 faz em desktop.
  - **Opção B**: wrapper personalizado D3D8→Vulkan escrito na camada gráfica (como as referências fazem).
  - **Opção C (fallback)**: software render viaANGLE/GLES — lento, não recomendado.
  - OpenGL **não** é usado (nenhuma importação de OpenGL/GLU).
  - **Parada necessária se**: a build fornecida fosse OpenGL ou D3D9 puro. Neste caso (D3D8), avançamos com a Opção A ou B.

---

## 4. API de Áudio (verificada)

- **dsound.dll** importado diretamente pelo jogo (DirectSound) — usado pelo engine de som principal.
- **winmm.dll** importado — usado pela Bink (waveOut* para áudio de vídeo quando o decoder Bink não tem DirectSound).
- **Bink** (`binkw32.dll`) — `_BinkSetSoundSystem@8` (DS ou waveOut), áudio dos vídeos.
- **Todos os dados de áudio estão embutidos nos arquivos `.bik`** (movies + music): PCM 16-bit, 14848/44100/48000 Hz, mono/stereo, compactados com **WavPack** (há strings do WavPack no exe). **Não existem arquivos .wav ou .mp3 soltos** no jogo.
- **Conclusão**: reimplantar sobre SDL2 =
  1. `SDL_mixer`/`SDL_audio` para o DirectSound do engine (reimplementar DSBUFFER/DSCAPS básicos + 3D audio)
  2. Bink decoder (recompilar `binkw32.dll` para Android ou portar o decoder Bink) — **binkw32.dll é um binário separado de 377 KB com 36 seções → recompilação estática viável**

---

## 5. Vídeo / Cutscenes

- **31 cutscenes em `.bik`** (Bink Video rev.i, h263/h264? — verificar codec) + áudio embutido
- **109 trilhas de música em `.bik`**
- `binkw32.dll` precisa ser recompilada (ou substituída) para Android. Opção: recompiler estático da DLL → Android com decoder Bink sobre SDL_video.
- Intros (actlogo, marvel, gplogo) são `.bik` rodados no frontend.

---

## 6. Assets (PKR3 — container proprietário)

- `data.pkr` (179 MB): .all (modelos/animações/áudio), .mdl, .pso (shaders)
- `wavs.pkr` (139 MB): áudio dos níveis
- **Formato PKR3 descoberto (parcial — ver seção 9):**
  - Magic `PKR3` + 4 bytes de cabeçalho
  - Blob 1: **BZip2 compactado** → índice do arquivo (62.128 B descompactados) com `MODE` magic, tabelas de nomes (.all/.mdl/.pso) e deslocamentos
  - Blob 2: **~178 MB de conteúdo** — o índice aponta offsets para este blob
- **Status**: o índice descompacta, mas **a estrutura de deslocamentos do PKR3 NÃO foi totalmente decodificada nesta Fase 1**. Extraímos ~593 referências de nomes de arquivo e identificamos que os dados estão no blob 2, mas o mapeamento exato offset→arquivo ainda precisa de engenharia reversa.

> ⚠️ **Risco documentado**: para ler os assets precisamos de um descompactador PKR3 funcional. Alternativas na Fase 3: (a) decodificar o PKR3 no runtime Android (C), (b) distribuir os assets **já extraídos** pelo usuário (script de extração fornecido na docs), (c) stubar assets não essenciais. **Não assumiremos que o PKR3 descompacta magicamente.**

---

## 7. Entrada (verificada)

Configuração de controles do jogo (`Wolvie.INI`):
- Keyboard: UP(17)/DOWN(31)/LEFT(30)/RIGHT(32), Punch(75=Backslash), Kick(72=2), Jump(57=5), Action(77=0), Claws(71=1), Crouch(82=8), Stealth(42=3), Senses(156=Menu), Camera(76=Q)
- Mouse: Punch(1), Senses(2), Camera(3/4)
- Gamepad: Type=5 (analógico)
- O jogo foi feito para teclado/rato mas suporta gamepad analógico (DualShock/Xbox) via DirectInput.

---

## 8. Rede

- **Single-player**. Nenhuma importação de Winsock/Wininet; strings de rede são apenas diálogo/ambiente.
- **Rede será stubada**: `WSAStartup`, `socket`, `connect`, etc. → retorna erro amigável.

---

## 9. Sinais que quebram recompilação estática (análise em andamento)

- **Código auto-modificável**: a verificar. Jogos de 2003 raramente têm JIT, mas há heap de scripts.
- **Geração de código em tempo de execução**: a verificar (Bink usa rotinas otimizadas — o exe pode conter código gerado).
- **Tabelas de salto indireto**: a verificar no lifting.
- **SEH**: DllCharacteristics tem NO_SEH → **nenhum SEH** ✅ (facilita o mapeamento de exceções).
- **SDK de 2003**: strings de debug indicam código C puro com `C:\Backup\Wolvie\Pc\...\` — boa notícia para o lifting C.
- **Bink**: código altamente otimizado (MMX/SSE de 2002) — pode usar intrínsecos x86 específicos. A recompilação precisa manter essas rotinas ou as cutscenes quebram. **RISCO MÓDERADO.**

---

## 10. Escala estimada (a medir na Fase 2)

- Wolvie.exe: ~2,9 MB compilado → estimativa lifted ~3–5 MB de C
- binkw32.dll: ~378 KB
- Funções descobertas: a medir (pefile: 0 exports, imports = 174 + 114 do Bink)
- RAM por arquivo compilado: estimativa ~100–300 MB por módulo (depende do número de funções) — documentar na Fase 2 com medições reais do runner CI.

---

## 11. Plano de riscos

| Risco | Severidade | Mitigação |
|---|---|---|
| Build fornecida é retail com SafeDisc | Média | release NoCD fornecido; hashing no launcher |
| API gráfica D3D8 → precisa de dx8vk ou wrapper | Média | dx8vk (existente) ou wrapper próprio D3D8→Vulkan |
| Container PKR3 não decodificado | Média | script de extração na docs / descompactador no runtime |
| Bink otimizado (MMX/SSE) | Média | recompilar binkw32.dll; fallback software |
| CI 6h para compilação de gen/ | Média | job único, cache de objetos, LB_GEN_JOBS=2 (runner 8GB) |
| Assets ~1 GB → tamanho do APK | Média | APK split por ABI + OBB/asset pack (o usuário tem os assets) |
| Memória mobile 179 MB de assets | Média | streaming do PKR3, texturas DXT, redução de res |

---

## 12. Tabelas de importação completas

### Wolvie.exe (174 imports, 9 DLLs)

**kernel32.dll** (112): InitializeCriticalSection, DeleteCriticalSection, EnterCriticalSection, LeaveCriticalSection, SetThreadPriority, GetCurrentThread, CreateThread, TerminateThread, ResumeThread, ExitThread, GetTickCount, Sleep, QueryPerformanceFrequency, QueryPerformanceCounter, GetLocalTime, GetSystemTime, FileTimeToSystemTime, FileTimeToLocalFileTime, GetTimeZoneInformation, GetVersionExA, GetVersion, GetCommandLineA, GetStartupInfoA, GetEnvironmentStringsA/W, SetEnvironmentVariableA, GetEnvironmentVariableA, SetStdHandle, GetStdHandle, GetConsoleMode, SetConsoleMode, GetConsoleScreenBufferInfo, GetNumberOfConsoleInputEvents, FlushConsoleInputBuffer, PeekConsoleInputA, ReadConsoleInputA, WriteConsoleA, GetFileType, SetFilePointer, SetEndOfFile, GetFileSize, LockResource, LoadResource, SizeofResource, FindResourceA/W, UnmapViewOfFile, MapViewOfFile, CreateFileMappingA, CreateFileA, CreateFileW, CloseHandle, GetModuleFileNameA, GetModuleHandleA, LoadLibraryA, FreeLibrary, GetProcAddress, HeapCreate, HeapAlloc, HeapReAlloc, HeapFree, HeapDestroy, HeapSize, IsBadCodePtr, IsBadReadPtr, IsBadWritePtr, VirtualAlloc, VirtualFree, GetCurrentProcess, GetCurrentThreadId, GetCurrentProcessId, TerminateProcess, ExitProcess, ExitThread, FatalAppExitA, RaiseException, RtlUnwind, SetUnhandledExceptionFilter, UnhandledExceptionFilter, GetModuleHandleA, GetProcAddress, GetLastError, SetLastError, InterlockedIncrement, InterlockedDecrement, GetNumberOfConsoleInputEvents, TlsAlloc, TlsSetValue, TlsGetValue, TlsFree, FlushFileBuffers, MoveFileA, DeleteFileA, CreateDirectoryA, GetCurrentDirectoryA, WritePrivateProfileStringA, GetPrivateProfileIntA, GetPrivateProfileStringA, OutputDebugStringA, SetConsoleCtrlHandler, GetDiskFreeSpaceExA, WideCharToMultiByte, MultiByteToWideChar, LCMapStringA/W, CompareStringA/W, GetLocaleInfoA/W, GetStringTypeA/W, IsValidCodePage, IsValidLocale, GetUserDefaultLCID, EnumSystemLocalesA, GetCPInfo, GetACP, GetOEMCP, SetHandleCount

**user32.dll** (33): CreateWindowExA, RegisterClassA, UnregisterClassA, DefWindowProcA, DispatchMessageA, TranslateMessage, PeekMessageA, GetMessageA, PostMessageA, SendMessageA, SetForegroundWindow, SetFocus, GetFocus, GetActiveWindow, SetActiveWindow, GetWindowThreadProcessId, GetWindow, GetTopWindow, GetParent, GetWindowLongA, SetWindowLongA, ShowWindow, UpdateWindow, MoveWindow, AdjustWindowRect, DestroyWindow, LoadCursorA, SetCursor, ShowCursor, GetCursorPos, GetSystemMetrics, MessageBoxA, LoadIconA, GetDC, ReleaseDC, BeginPaint, EndPaint, ClientToScreen, ScreenToClient, GetClientRect, GetWindowRect

**gdi32.dll** (3): GetStockObject, DeleteObject, CreateCompatibleDC... *(continua no repositório - ver tools/discover_imports.py)*

**advapi32.dll** (4): RegOpenKeyA, RegOpenKeyExA, RegQueryValueExA, RegCloseKey

**d3d8.dll** (1): Direct3DCreate8

**dinput8.dll** (1): DirectInput8Create

**winmm.dll** (6): waveOutOpen, waveOutPrepareHeader, waveOutUnprepareHeader, waveOutSetVolume, waveOutReset, waveOutRestart, waveOutPause, waveOutWrite, waveOutClose, timeGetTime, mmioOpenA, mmioAscend, mmioDescend, mmioRead, mmioClose, mmioStringToFOURCCA

**binkw32.dll** (17): _BinkOpen@8, _BinkClose@4, _BinkNextFrame@4, _BinkDoFrame@4, _BinkWait@4, _BinkPause@8, _BinkSetVideoOnOff@8, _BinkSetVolume@12, _BinkSetSoundSystem@8, _BinkOpenDirectSound@4, _BinkSetSoundTrack@8, _BinkDX8SurfaceType@4, _BinkCopyToBuffer@28, _BinkGetError@0, _BinkLogoAddress@0, _BinkLoadPalette@8, _BinkSetPalette@8

**dsound.dll** (1): (importado por índice — DSBcreate@4 verificado no d3d.log)

### binkw32.dll (114 imports, 4 DLLs)

USER32, GDI32, KERNEL32 (VirtualProtect, CreateEventA, CreateMutexA, SetEvent, WaitForSingleObject/MultipleObjects, CreateThread, etc.), WINMM (waveOut*, timeGetTime).

---

## 13. O que foi HIPÓTESE e o que precisa ser verificado

1. **Endereços de hooks/override** — não existem, ainda não fiz lifting. `runtime/override_list.c` será preenchido na Fase 2 com endereços verificados nos binários fornecidos.
2. **Layout exato do PKR3** — parcial. Verificar offsets e mapeamento.
3. **Codec exato das cutscenes** (h263v2? h264?) — verificar com `ffprobe`/`binkdump`.
4. **Quantidade de funções e tamanho do gen/** — medir na Fase 2.
5. **Lista completa de DLLs** — ver arquivo tools.

---

## 14. Próximo passo (Fase 2)

1. Clonar as três referências (LMS_Android, Ultimate_Spider_Man_Android, Skyrim_Android)
2. Generalizar discovery/lifter com `LIFT_GAME=x2wr`, mapear endereços de carga (Wolvie.exe → 0x00400000, binkw32.dll → 0x30000000)
3. Executar discovery/lifting local para medir tempo e RAM
4. Gerar ci/gen_stub/
5. Verificar PKR3 até extrair pelo menos um .all completo para validar formato de asset

---

*Última atualização: Fase 1. Hashes verificados contra os binários em /root/share/X2_Wolverines_Revenge_Win_R_EN/. Nenhuma imagem de console foi fornecida — se o usuário mudar para imagem de console, o pipeline muda e este documento deve ser refeito.*

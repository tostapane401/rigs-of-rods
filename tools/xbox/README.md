# Rigs of Rods su Xbox — guida tecnica al porting (UWP / Dev Mode)

Questa cartella e le modifiche collegate portano RoR (OGRE **1.11.6**, come fissato in `conanfile.py`)
su Xbox One / Xbox Series X|S come **app UWP in Dev Mode**, con build e pacchettizzazione su GitHub Actions.

---

## 0. Due decisioni da prendere prima di tutto

### UWP (Dev Mode) oppure GDK?

| | UWP su Xbox in Dev Mode | Microsoft GDK per console (GDKX) |
|---|---|---|
| Chi può usarlo | Chiunque abbia una console retail attivata in Dev Mode | Solo partner ID@Xbox/Xbox con dev kit e accordo NDA |
| SDK | Windows SDK + workload UWP di VS (pubblici) | GDKX (non pubblico, non ridistribuibile) |
| API grafica | D3D11 (Xbox One: FL 10.1; Series: FL 11.0) o D3D12 FL 11.0 | D3D12.X (Series), D3D11.X / D3D12.X (One) |
| Build su runner GitHub ospitati | **Sì** | **No**: serve un runner self-hosted con GDKX installato |
| OGRE 1.11 | Supportato (`WINDOWS_STORE`, `D3D11RenderWindowCoreWindow`) | Nessun render system D3D12 in OGRE 1.x (né in Ogre-Next) |

Il "Xbox Dev Kit" che si attiva da Dev Home su una console retail è **Dev Mode → UWP**. Questo porting segue
quella strada. Le astrazioni (storage, input) hanno già il punto di innesto per un backend GDK, ma un vero
porting GDK console richiede un render system D3D12.X scritto apposta: è un progetto a parte.

### D3D11 o D3D12?

**D3D11.** OGRE 1.11 non ha un render system D3D12. Su Xbox in modalità **Game** (vedi sotto) D3D11 è
disponibile a feature level 10.1 (One) / 11.0 (Series): l'RTSS genera shader `vs_4_0`/`ps_4_0`, che girano su entrambe.

### Risorse che il sistema concede (UWP su Xbox)

| | App | **Game** |
|---|---|---|
| RAM in foreground | 1 GB | **5 GB** |
| CPU | 2–4 core condivisi | 4 core esclusivi + 2 condivisi |
| GPU | ~45 % condivisa | Tutta la GPU |

Dopo l'installazione: **Dev Home → seleziona l'app → View details → App type = Game**. Senza questo passaggio
RoR (fisica soft-body + content pack) esaurisce la memoria.

---

## 1. RTShader System su D3D11 (crash "Could not create gpu programs from render state")

### Diagnosi (verificata sul sorgente di OGRE 1.11.6)

1. **Libreria shader sbagliata.** `resources/rtshader` conteneva una RTShaderLib più vecchia di 1.11.6 (file
   `*.hlsl`/`*.cg`, senza `FFPLib_AlphaTest`). L'RTSS 1.11.6 su D3D11 genera chiamate a `FFP_Alpha_Test`,
   `FFP_Normalize`, `FFP_PixelFog_PositionDepth`, che lì non esistono → HLSL non compila →
   `ProgramManager::createGpuProgram()` restituisce null → eccezione `Could not create gpu programs from render state`.
   **Correzione:** la cartella ora contiene esattamente `Samples/Media/RTShaderLib/HLSL_Cg` del tag `v1.11.6`.
   Il writer HLSL cerca `<lib>.hlsl` nel gruppo *General* e ripiega su `<lib>.cg`; l'include handler D3D11 cerca in
   tutti i gruppi. Per questo **non** devono esserci copie `*.hlsl` vecchie da nessuna parte.
2. **Cache shader non scrivibile.** `ShaderGenerator::setShaderCachePath()` lancia un'eccezione se la cartella non è
   scrivibile, e `createGpuProgram()` restituisce null se la scrittura fallisce. Nella sandbox UWP qualunque path fuori
   da `ApplicationData` è negato. **Correzione:** cache in `LocalCacheFolder\shaders\rtshader`, verificata prima
   dell'uso; se non è scrivibile si prosegue con la sola memoria.
3. **Pass fixed-function che arrivano comunque al device.** Lo schema viene impostato *per viewport*
   (`SceneManager::setViewport()` chiama `MaterialManager::setActiveScheme(vp->getMaterialScheme())`). RoR, Caelum,
   Hydrax, l'envmap, l'acqua e PagedGeometry creano molte viewport: impostare lo schema solo sulla viewport principale
   non basta.

### Soluzione — `source/main/gfx/RTShaderBootstrap.{h,cpp}`

- `SafeResolverListener` **deriva da `OgreBites::SGTechniqueResolverListener`**, quello fornito da OGRE 1.11.6
  insieme alla sua firma di `handleSchemeNotFound` (che quindi coincide sempre). Avvolge la chiamata in `try/catch` e,
  in caso di errore, registra nel log il nome del materiale e lo associa a una tecnica di fallback **magenta** in HLSL
  scritto a mano (non dipende dalla libreria RTSS). I materiali magenta a schermo sono quelli da correggere.
- `SchemeEnforcer` (`SceneManager::Listener::preFindVisibleObjects`) forza lo schema RTSS su **ogni** viewport
  renderizzata da quello scene manager, comprese le RTT create da plugin.
- `QueueGuard` (`RenderQueue::RenderableListener`) è l'ultima difesa: se un renderable arriva in coda con un pass
  senza vertex/pixel shader, lo sostituisce con la tecnica RTSS o con il fallback. Così il gioco non va mai in crash
  con "Attempted to render to a D3D11 device without a vertex shader".
- Cache del microcodice D3D (`GpuProgramManager::saveMicrocodeCache`): salvata alla chiusura e alla sospensione,
  elimina le ricompilazioni HLSL con `D3DCompile` agli avvii successivi.
- Aggancio: `ContentManager::InitContentManager()` (inizializzazione, attiva automaticamente se il render system non ha
  `RSC_FIXED_FUNCTION`), `GfxScene::Init()` (`AttachSceneManager`), `main.cpp` (salvataggio della cache alla chiusura).

Debug: in `RoR.log` cerca `[RoR|RTSS]`. Subito sopra ogni errore RTSS il render system D3D11 scrive l'errore HLSL vero
e proprio, con riga e colonna.

**Rischi residui:** i materiali che hanno **solo** tecniche Cg (alcuni di Caelum/Hydrax/SkyX e delle mod) su UWP
non hanno il plugin Cg: Ogre ripiega sulla tecnica FFP successiva (convertita da RTSS) oppure sul magenta.
Vanno aggiunte tecniche HLSL `vs_4_0`/`ps_4_0`.

**Altra correzione:** MyGUI 3.4.0 su render system senza FFP carica `MyGUI_VP.hlsl`/`MyGUI_FP.hlsl`, mentre RoR
distribuiva soltanto `MyGUI_Ogre_*.hlsl`. Ora ci sono entrambi i nomi.

---

## 2. Toolchain, CMake e Conan per UWP x64

```bat
:: deps (una volta, ~40-60 min): OGRE 1.11.6, MyGUI, zlib, zziplib, freetype, openal-soft, Caelum, PagedGeometry
pwsh tools\xbox\build-deps.ps1 -Prefix C:\ror-uwp\prefix -Work C:\ror-uwp\work

:: RoR (fmt, rapidjson e angelscript arrivano da Conan tramite cmake-conan)
cmake -S . -B build-uwp -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/WindowsStore-x64.cmake ^
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/conan_provider.cmake -DCONAN_INSTALL_ARGS=--build=missing ^
  -DROR_DEPENDENCY_DIR=C:/ror-uwp/prefix -DOGRE_DIR=C:/ror-uwp/prefix/CMake
cmake --build build-uwp --config Release --parallel

pwsh tools\xbox\package-msix.ps1 -BinDir build-uwp\bin -DepsPrefix C:\ror-uwp\prefix -OutDir dist -Version 2026.1.1.0
```

| Elemento | Dove | Perché |
|---|---|---|
| `CMAKE_SYSTEM_NAME=WindowsStore`, `CMAKE_SYSTEM_VERSION=10.0` | `cmake/toolchains/WindowsStore-x64.cmake` | CMake genera progetti AppContainer (`/APPCONTAINER`, `WINAPI_FAMILY_APP`); OGRE vede `WINDOWS_STORE` |
| Generatore **Visual Studio**, `-A x64` | obbligatorio | CMake emette progetti UWP solo con i generatori VS; Xbox accetta solo x64 |
| CRT dinamico `/MD` | toolchain | UWP richiede il framework `Microsoft.VCLibs.140.00` (incluso nell'artifact) |
| Solo `Release` | toolchain | La `RelWithDebInfo` di RoR usa `/DYNAMICBASE:NO`, che UWP rifiuta |
| `ROR_PLATFORM_UWP`, `WINAPI_FAMILY=WINAPI_FAMILY_APP`, `_WIN32_WINNT=0x0A00` | `cmake/XboxUWP.cmake` | macro di piattaforma usate dal codice |
| `WindowsApp.lib`, `d3d11.lib`, `dxgi.lib` | idem | libreria "ombrello" delle API consentite (al posto di `Ws2_32`, `user32`…) |
| `/std:c++17` + `SKIP_PRECOMPILE_HEADERS` solo sui file C++/WinRT | idem | RoR resta C++14 + PCH; C++/WinRT richiede C++17. Nessun `/ZW` nel codice RoR (lo usa solo OGRE internamente) |
| `os=WindowsStore`, `os.version=10.0` | `conan_provider.cmake` (patch), `tools/xbox/conan/uwp-x64.profile` | cmake-conan non emetteva `os.version`, che `settings.yml` richiede |
| ramo `WindowsStore` in `conanfile.py` | `conanfile.py` | Da Conan arrivano solo librerie portabili: le ricette del remote RoR non hanno binari WindowsStore per lo stack OGRE |
| OIS "core" (senza backend DirectInput) | `cmake/XboxUWP.cmake` | DirectInput non esiste in UWP; si tengono i tipi OIS |
| `Codec_STBI`, niente Cg/FreeImage/GL/D3D9 | `plugins.cfg.in` + `source/main/CMakeLists.txt` | quei plugin non esistono per UWP |
| Mumble e Discord disattivati | `source/main/CMakeLists.txt`, `conanfile.py` | richiedono shared memory / named pipe, negate nell'AppContainer |

---

## 3. Storage sandbox — `source/main/utils/PlatformStorage.{h,cpp}`

L'unico punto che decide dove RoR può scrivere. `AppContext::SetUpProgramPaths()` e `main.cpp` lo usano; il
comportamento desktop (`Documents\My Games\Rigs of Rods`, modalità portable `config\`) non cambia.

| Backend | user_dir (config, mods, saves, logs) | cache_dir (modcache, shaders) | install_dir |
|---|---|---|---|
| Win32 | `Documents\My Games\Rigs of Rods` | `<user_dir>\cache` | cartella exe |
| **UWP / Xbox** | `ApplicationData.LocalFolder\RigsOfRods` | `ApplicationData.LocalCacheFolder` | `Package.InstalledLocation` (**sola lettura**) |
| GDK | `XPersistentLocalStorageGetPath()\RigsOfRods` | `<PLS>\cache` | cartella exe (sola lettura) |

Regole: dentro queste cartelle le API file Win32/CRT (`CreateFile2`, `std::fstream`, quelle usate dal
`FileSystemArchive` di OGRE) funzionano normalmente, quindi non serve passare per `StorageFile` asincrono.
Non scrivere mai nell'install dir. Non usare `%USERPROFILE%`, `SHGetFolderPath`, `getenv` su UWP/GDK.
`PlatformUtils.cpp` ha un ramo UWP: `GetFileAttributesExW`, `std::filesystem::create_directories`, `Launcher::LaunchUriAsync`.
Per i salvataggi in cloud su GDK il passo successivo è `XGameSaveFiles` (cartella `savegames`).

Accesso ai file dal telefono: Device Portal → File explorer → LocalAppData → pacchetto → `LocalState\RigsOfRods\mods`.

---

## 4. Input nativo Xbox — `source/main/utils/XboxInput.{h,cpp}`

Su Xbox UWP l'API corretta è **Windows.Gaming.Input**: GameInput fa parte del GDK e su UWP non esiste;
XInput c'è ma non vede volanti né force feedback. Il percorso con meno modifiche è **tenere i tipi OIS**
(`InputEngine`, i file `.map`, la UI dei controlli e `ForceFeedback.cpp` dipendono da quelli) e **sostituire solo
i backend**:

| Classe | Base OIS | Sorgente |
|---|---|---|
| `GamepadJoyStick` | `OIS::JoyStick` | `Windows.Gaming.Input.Gamepad` |
| `RacingWheelJoyStick` + `WheelForceFeedback` | `OIS::JoyStick` / `OIS::ForceFeedback` | `RacingWheel` + `WheelMotor` (`ConstantForceEffect`, asincrono) |
| `CoreWindowKeyboard` | `OIS::Keyboard` | `CoreWindow.KeyDown/KeyUp/CharacterReceived` (scancode → `KeyCode` per via aritmetica) |
| `CoreWindowMouse` | `OIS::Mouse` | `CoreWindow.Pointer*` + `MouseDevice.MouseMoved` |

- **Joystick 0** ha lo stesso layout del driver DirectInput dell'Xbox 360 e lo stesso nome dispositivo, quindi
  carica `Controller__Xbox_360_Wireless_Receiver_for_Windows_.map` già esistente: nessuna mappatura nuova da scrivere.
- **Joystick 1** = volante, nuova mappa `resources/skeleton/config/Xbox_Racing_Wheel__Windows_Gaming_Input_.map`
  (pedali in stile G27: usa `REVERSE`).
- Gli slot sono fissi e reggono il collegamento a caldo: subito dopo l'avvio `Gamepad::Gamepads()` è spesso vuota e
  i dispositivi arrivano con gli eventi `*Added`.
- Menu: `ImGuiConfigFlags_NavEnableGamepad` + `FeedImGuiGamepadNav()` (ImGui 1.73 `NavInputs`): A conferma,
  B torna indietro, croce direzionale o levetta sinistra per muoversi, LB/RB per cambiare finestra.
  `BackRequested` viene gestito, quindi B non chiude l'app.
- Se il force feedback risulta invertito su un volante, cambia il segno in `WheelForceFeedback::modify()`.

---

## 5. Pipeline cloud — `.github/workflows/xbox-uwp.yml`

1. Fai il push di questi file su un branch `xbox-port`: la build parte da sola. In alternativa, dal telefono:
   **Actions → Xbox UWP build → Run workflow**.
2. Runner `windows-2022` (VS 2022 con il workload UWP, `makeappx`/`makepri`/`signtool`, VCLibs appx).
3. Il prefisso delle dipendenze va in cache (chiave = hash di `build-deps.ps1` + toolchain). La prima esecuzione è
   lenta, le successive compilano solo RoR.
4. Artifact `ror-xbox-uwp-<versione>`: `.msix`, `.msixbundle`, `Microsoft.VCLibs.x64.14.00.appx`, `.cer`, `INSTALL-XBOX.txt`.
5. Firma: senza secret viene generato un certificato self-signed a ogni esecuzione (in Dev Mode va bene, ma prima di
   installare una build nuova devi disinstallare quella vecchia). Con i secret `XBOX_SIGNING_PFX_BASE64`,
   `XBOX_SIGNING_PFX_PASSWORD` e `XBOX_PUBLISHER` l'identità resta stabile e gli aggiornamenti si installano sopra.

Installazione: vedi `tools/xbox/INSTALL-XBOX.txt` (Device Portal `https://<ip-console>:11443` → Add → `.msix` + dipendenza VCLibs).

---

## 6. Cosa è stato verificato e cosa no

Verificato in questo lavoro:
- API e firme confrontate con il sorgente di **OGRE v1.11.6** (RTSS, Bites, D3D11 CoreWindow, DynLib WinRT),
  **OIS v1.4** e **MyGUI 3.4.0**.
- `RTShaderBootstrap.cpp` compila (`-fsyntax-only`) contro gli header reali di OGRE 1.11.6.
- `XboxInput.cpp` compila contro gli header di OIS 1.4 (WinRT simulato).
- Workflow validato con actionlint; script PowerShell analizzati con il parser di PowerShell 7.

Non verificabile qui (serve Windows + MSVC + SDK UWP), quindi da sistemare nelle prime esecuzioni CI:
- la build WindowsStore di OGRE 1.11.6 (supportata dal suo CMake, ma upstream non la prova da anni),
  di MyGUI (patch clipboard), Caelum e PagedGeometry (opzionali), angelscript via Conan su WindowsStore;
- eventuali API Win32 rimaste nel codice RoR non guardate da `#if`, che MSVC segnalerà come "identifier not found";
- shader HLSL di SkyX/Hydrax/Caelum a feature level 10.1 su Xbox One.

Il job carica i `CMakeError.log`/`CMakeCache.txt` come artifact quando fallisce, così l'errore si legge anche dal telefono.

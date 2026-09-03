# Dynamic Dialogue Replacer 1.4.1 maintenance port

This branch is a narrow runtime-compatibility port of the released `v1.4.1`
source (`a4f7e727b90c58b208bf9e479a4aff9ab2f5d5df`). It does not import later
behavioral changes.

## Compatibility boundary

- Skyrim SE/AE runtime: **1.7.104.0 only**
- SKSE: **2.3.1**
- Address Library: binary **format 5** (`versionlib-1-7-104-0.bin`)
- CommonLibSSE-NG: Ensrick `ensrick/no-modal-fail-v7.1.0`, commit
  `90a64a4d65ce659a139137c968f42151bb6ecec9`
- Microsoft Detours: pinned upstream commit
  `b2bf32a657be1114b173ad28d2d65463ca4c466f`

The exported SKSE metadata lists only 1.7.104.0 and advertises format-5 support.
`SKSEPlugin_Load` independently rejects every other runtime before it resolves
or writes a hook. This is important: SKSE accepting a DLL is necessary, but it
does not prove that the plugin's CommonLib address database can parse the
installed Address Library.

The CommonLib pin parses format 5 and changes its fatal reporter, when
`COMMONLIBSSE_NO_MODAL_ERRORS` is enabled, to log, flush, and terminate without
opening a native message box.

## Hook-safety changes

- Uses the audited 1.7.104 AE relocation IDs and offsets directly.
- Preflights all four `E8` call sites and verifies every function/vtable target
  maps to executable committed memory before changing game code.
- Checks every Detours transaction result and aborts a failed transaction.
- Defers SKSE listener and Papyrus registration until the hook preflight and
  Detours transaction succeed.
- Returns `false` only while it remains safe for SKSE to unload the DLL. After
  the first hook is committed, any unrecoverable failure uses CommonLib's
  log-only, non-modal fatal path instead of leaving a dangling callback or
  trampoline into an unloaded DLL.
- Fixes the post-build copy step so a missing `dist/SKSE/Plugins` directory is
  created as a directory rather than as a file named `Plugins`.

## Verification

Build the release DLL with xmake 3.1.1:

```powershell
xmake f -y -m release --build_papyrus=n
xmake -y DynamicDialogueReplacer
```

Then validate the actual 1.7.104 executable and address database:

```powershell
pwsh -NoProfile -File .\scripts\Test-HookSites.ps1 `
  -SkyrimExe 'C:\path\to\SkyrimSE.exe' `
  -AddressLibrary 'C:\path\to\versionlib-1-7-104-0.bin'
```

The verifier rejects any non-format-5/non-1.7.104 database, checks the four
call opcodes, validates both callable function prologues, and resolves the
DialogueMenu vtable entry back into the executable's `.text` section.

The CI artifact is a DLL/PDB overlay for the official DDR 1.4.1 package. Keep
the official package's scripts, interface/configuration, and content files;
replace only `SKSE/Plugins/DynamicDialogueReplacer.dll` with the built DLL.
An in-game smoke test remains required before this overlay is enabled in a
playthrough profile.

## Licensing

DDR 1.4.1 and this maintenance work remain under GPL-3.0 with the repository's
`EXCEPTIONS.md` linking/modding exception. CommonLibSSE-NG and Microsoft Detours
retain their respective licenses and exceptions in their source trees.

# Controlled-save hook source review v2

This review updates the private no-launch controlled-save draft to use the current `axr_main.script` bytes and retain a copy of the previously staged hook.

- Current game-root source: `<PRIVATE_LOCAL_PATH>`, SHA-256 `51adde1c505d140f7bce4b37119ed82ed554c335f227bcc5690c0375b02b26e0`.
- Previously staged hook: SHA-256 `c0f0fa05e7b495a06f9f841df716a69f4613beeeafe153e0db01aac6f279e182`; preserved at `runs/mo2-reference-startup-01/mo2-controlled-save-run-01/input/axr_main.previous-staged.script`.
- Regenerated hook from the current source: SHA-256 `c0f0fa05e7b495a06f9f841df716a69f4613beeeafe153e0db01aac6f279e182`. The hash is unchanged because the old wrapper's preserved original handler body matches the current game-root handler body byte-for-byte, and the same gated wrapper is retained. The current original is also preserved at `input/axr_main.current-base-original.script`.
- The private launch plan now records the current original source hash and generated hook hash as separate pins. The runner checks both independently before any requested launch.

Source selection evidence is incomplete. A read of the active `MyTree` profile found no loose `gamedata/scripts/axr_main.script` in enabled MO2 mods; primary MO2 Overwrite has no such file; and the V3 startup USVFS log contained no `axr_main` redirect. The enabled mod archives were not fully unpacked and compared, so these observations do not prove the game-root file wins the complete VFS. The later root-owned run must produce fresh USVFS or engine evidence tying the hook path to the active Overwrite before its result is accepted. The script refuses a loose-profile or primary-Overwrite collision.

The immutable runner draft also now launches MO2 using the same short lexical alias for executable path and working directory as the prior startup trial. Its Wine module-load parser finds a unique `AnomalyDX11.exe` image-load line, takes that line's Wine thread ID, and checks builtin D3D11/DXGI module lines only for that thread. It reports the host game PID, guest WPID, and Wine module-log TID as separate identities. Missing or ambiguous executable attribution fails qualification; there is no fallback to unrelated MO2 module lines.

Validation was limited to Python syntax parsing, pin/hash consistency, and a synthetic module-log attribution check. The runner was not executed, and no game launch, build, or production-source edit was performed.

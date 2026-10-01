# Minimum master-runner support for a private Windows guard reference

The existing master runner already pins the Windows stock executable at `WINBASE/bin/AnomalyDX11.exe` (`c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`) and clones `WINBASE/bin` into the new owned run directory. Keep that stock source pin and clone flow. The lightweight runtime manifest intentionally omits `bin`, so record the selected executable hash separately.

Proposal only: [private-guard-reference-runner-proposal.patch](<<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/luna-harness/compatibility/private-guard-reference-runner-proposal.patch>). It adds only a two-value executable mode:

- `stock` remains the default and pins the clone's `AnomalyDX11.exe` to the verified stock SHA.
- `guarded-custom` accepts only the verified planned guard SHA `1a3ff9acbab231da7348ad61969d6e910d229233ae982b48a8e4e5a23b271c20`; during `--prepare-only`, it saves `AnomalyDX11.stock.exe` in that run's output and verifies the preserved stock hash before placing the guard copy into the run clone. At runtime it re-pins the active clone executable and records mode + hash in `environment.txt` and `common-settings.txt`.
- `guarded-custom` refuses `--run-prepared` and native-arm runs, so it cannot be mistaken for a matched stock performance pair.

The guard input is supplied as `PARITY_SILO_GUARD_EXE` from the isolated copy root has prepared. Nothing points to or writes back into the source GAMMA runtime, original Windows runtime, Wine prefix, or mod sources. The only custom behavior is selecting an already-patched executable within the newly cloned run folder.

No master runner changes were applied, no executable was written or launched by this audit. The script syntax was not executed; the proposed hunk is for root review only.

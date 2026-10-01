# Controlled-save runner V3 parser review

V3 is an immutable copy of the root-reviewed V2 runner with only Wine debug path normalization added before matching `AnomalyDX11.exe`. It does not alter the currently running V2 process, its private MO2 configuration, or its receipt.

## Wine module attribution

Archived startup log SHA-256: `bab2da50905d44e4e325f69816e8470dbc06ba6d71d68c8a302715cc01d22fb4` at `runs/mo2-reference-startup-01/menu-startup-long-attach-01/stdout.log`.

The actual executable row is prefixed `01b0:` and contains `<PRIVATE_LOCAL_PATH>` in Wine's escaped debug-string form. V2 compared against a path with single separators, so it found no game module TID. V3 collapses repeated backslashes in the debug text before exact alias/executable matching. Against this archived log, V3 selects TID `01b0`, then finds both builtin `d3d11.dll` and `dxgi.dll` rows on that TID. Synthetic unique-TID attribution passes; two distinct executable-image TIDs fail closed. Python syntax parsing passed.

V3 runner SHA-256: `c2bdef725f91ccd1f8ca94544ffdea090a46fc16e2ad5287dde33df1e4d3b816`. V2 remains unchanged at SHA-256 `a13b4e588e5c912df0d94178d7d9b813e1c6f561f7c4d741dd53613c28aac61c`.

## File-mapping acceptance limitation

V2's `log_contains` searches each fresh log's entire text and returns true when every requested substring occurs anywhere in that file. For the hook it searches `overwrite` plus `axr_main.script`; for the save it searches only the save basename. That proves neither a single path record nor that the game process resolved the requested virtual file. The USVFS startup logs inspected here show process injection/registration and API hooks such as `NtOpenFile`; they do not record per-file open or virtual-to-physical resolution rows. The archived engine log only says `loading script axr_main.script`, without a resolved origin. A more exact regular expression over the same files would still not create evidence the logging source never emitted.

Therefore `fresh_usvfs_contains_hook_name` and `fresh_usvfs_contains_save_name` should be treated as weak text hints, never acceptance gates. Keep the raw V2 result and state explicit path-resolution qualification as unavailable unless fresh evidence identifies the resolved physical files.

For a future run, the smallest hook-specific proof is a unique per-run sentinel emitted only by the generated private Overwrite wrapper and copied into the harness's `loaded_state` record, with the expected nonce and hook SHA pinned in the plan. This demonstrates that the exact wrapped code executed, though it does not expose the OS path independently. To prove the save file path, use an observer that records a successful open/read of the canonical clone save path by the game host process, or an equivalent engine-level resolved-path receipt; USVFS startup text and saved-state field checks alone do not prove source-file identity.

No runner was executed and no launch/configuration/script/receipt was changed by this review. Parent owns interpretation of the currently running V2 result.

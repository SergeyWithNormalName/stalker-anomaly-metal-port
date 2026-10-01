# Portal observer runtime validation: observe-04

The independent validator cross-checked the live frame-700 receipt against the captured active level, Flora ID capture, E01 row, and pinned offline C++ replay. Every directly checkable integrity condition passed:

- The output files are newer than the run launch epoch. The active-reader log reports generation 1, `copy_ok=1`, and 6,416,980 bytes; that generation and byte count match the receipt and captured file. The captured active-level SHA-256 is `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`.
- The ID CSV has 5,316 unique visual IDs, exact frame/extent metadata, and the terminal `encode_complete,5316` footer. Its IDs and materials match the pinned offline inventory.
- The receipt reports `complete=true`, `suppressed=0`, a complete 5,316-ID structural join, and zero unknown joins. Its raw flags and owner-frustum test counts match all 5,316 rows of the pinned offline replay exactly: 889 visible and 4,427 not visible.
- Start sector is 25, with six nonempty reached sectors and eight frusta. The E01 file has exactly one completed engine-frame-700 row, serial 700.
- View, projection, and eye bit arrays independently match the ID CSV. The receipt also reports its internal seven-value SSA comparison as true.

The validator exits with a partial result (`passed=false`) because two bindings are absent from the receipt. It does not serialize the seven SSA settings bits, so the validator cannot independently recompute the receipt's `csv_ssa_match=true` assertion. The runtime `level.geom` symlink currently hashes to the pinned `a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a`, but the receipt does not bind that hash to the prepared geometry cache. These are unknown links, not observed mismatches.

This establishes source-observer receipt consistency for this captured run. It does not prove GPU visibility equivalence, visual quality, general culling safety, or FPS benefit.

The read-only validator is `validate_runtime_portal_receipt.py`; its machine-readable output is `portal-runtime-observe-04-validation.json`. SHA-256 values:

- Validator: `b8ee29a4664875e208087a9ffe8e262775529c6407f598cc219aa3a144ed9010`
- Validation JSON: `3da22a5ea11c2324e47bd279f5f20afab454532a08c5c7d84b1bb027895e56cb`

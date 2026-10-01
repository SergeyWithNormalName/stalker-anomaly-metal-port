# WineDbg carriage-return control check

On the saved Help-crash minidump, the command `break *0x14008e3cf` followed by one carriage return created Breakpoint 1 exactly once and returned to the `Wine-dbg>` prompt. Sending a second, empty carriage return repeated the prior command and incremented its refcount to 2. This confirms the earlier live controller's extra CRs replayed commands; they must be omitted. The check used the owned offline prefix and was cleaned with `wineserver -k`; no game ran. Typed expression evidence is recorded in `EXPRESSION-RESULT.md`.

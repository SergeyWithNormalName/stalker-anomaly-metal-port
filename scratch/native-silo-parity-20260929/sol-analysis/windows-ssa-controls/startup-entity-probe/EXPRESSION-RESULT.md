# WineDbg expression check on saved minidump

This check used only the existing `windows-console-probe-01` Help-crash minidump, through a PTY on the owned `leaf-lq-off-01` Wine prefix. It did not start or attach to the game. Full logs are in `expression-syntax-test.log`; command/result JSON is `expression-syntax-test.json`.

WineDbg accepted the breakpoint condition

```text
condition 1 (($rdx==0)||(*((char*)$rdx)==0))
```

and returned to its prompt without an expression error. The separate evaluated predicate `print *((char*)$rdx)==0` returned `0x1` for the minidump's RDX; `print *((char*)$rdx)` returned `0`. The typed 64-bit memory read `print *(unsigned long long*)$rdx` returned `0x732065676e616800`, and address arithmetic also worked in `print *(unsigned long long*)($rdx+8)` (`0x6572206e65657263`). Thus the typed char-pointer predicate and qword reads are confirmed on readable memory in this dump. This does not establish which bytes a live model-name pointer will contain.

The `x/s` and `x/1gx` forms produced no value in this offline frontend transcript, so the live probe should use the proven `print` casts. For a section string, first read the qword handle at `this+0xec`, confirm nonzero, then read qwords from `handle+0x14`, `handle+0x1c`, etc. as needed; the debugger presents little-endian words, whose bytes can be decoded as the string. Do not dereference a null handle.

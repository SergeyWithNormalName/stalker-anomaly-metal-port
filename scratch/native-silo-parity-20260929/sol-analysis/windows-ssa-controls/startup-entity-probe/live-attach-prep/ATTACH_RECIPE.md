# Live WineDbg attach preparation (not executed here)

The bundled Wine-CX 26.3 help in `../winedbg-help.stdout.txt` documents `winedbg <num>` as attaching to a running process by Wine guest PID (`wpid`). It separately documents the prompt commands `detach`, `cont [N]`, `bt [<tid>|all]`, `thread <tid>`, and `quit`. Never pass the macOS host PID to WineDbg.

`prepare_attach_command.py` reads one selected, fresh USVFS log, extracts the latest `AnomalyDX11.exe:<id>`, and refuses to print a command unless matching injection and process-registration records for that same guest ID are present. `--host-pid` is printed as separate crosswalk context and is never included in the WineDbg command. The script only prints a command; it never invokes Wine or WineDbg. Example shape:

```sh
python3 prepare_attach_command.py \
  --usvfs-log /path/to/current/mo2/logs/usvfs-current.log \
  --wineprefix /path/to/current/private-prefix \
  --host-pid 70157
```

Confirm that the selected log belongs to the current short-alias retry and that the host PID still maps to its Anomaly guest process. The two IDs can differ substantially. In Root’s currently reported crosswalk, guest `0x1a8` is decimal `424`; host PID `70157` is crosswalk-only. Treat that as an example from the active experiment, not a reusable PID.

The printed command uses the same Wine-CX 26.3 `wine64`/x86_64 `winedbg.exe` pair, an explicit `WINEPREFIX`, and only `WINEDEBUG=-all` plus the runtime’s standard DYLD fallback paths. Do not use `--gdb`, `--help`, a minidump, LLDB, PDB/LLVM commands, or a second Wine process as an alternate path for this capture.

At the debugger PTY, wait for the prompt and send exactly one carriage return per command. Debug04 documented that a blank CR repeats the previous command. Record the initial stop line and then use the commands directly supported by the saved help:

```text
bt
print $rip
print $rsp
print $rbp
print $rax
print $rbx
print $rcx
print $rdx
print $rsi
print $rdi
print $r8
print $r9
print $r10
print $r11
print $r12
print $r13
print $r14
print $r15
help info
bt all
```

`bt` captures the currently selected guest thread; `bt all` records all stacks and their thread labels. `help info` is required before using an `info` subcommand, since the captured global help defers its exact syntax. If it exposes a thread listing, record the selected thread ID and use the documented `thread <tid>` command only if a second thread’s registers are needed. Save the WineDbg stop header, exact guest RIP, register values, selected thread, `bt`, and `bt all` output with the PTY transcript. Avoid typed PDB expressions; Debug03 showed those can fail and the capture needs only raw registers/stack.

After the short sample, enter `detach` once so the game resumes, then `quit` once to leave WineDbg. Only after detaching should the owner stop/clean the private prefix through its matching `wineserver`; do not run `wineserver -k` against any shared or primary prefix. This preparation performs no launch, attach, breakpoint, process enumeration, or prefix cleanup.

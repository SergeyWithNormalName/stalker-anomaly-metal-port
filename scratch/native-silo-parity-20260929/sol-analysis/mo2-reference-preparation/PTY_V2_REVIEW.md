# WineDbg raw-PTY controller v2

The prior attach transcript proves WineDbg attached to guest PID 424 and reached a prompt, then the first `print $rip` input was echoed with `^J` and no fresh prompt followed. The controller writes `\r` to the PTY master. A canonical PTY slave with `ICRNL` maps that carriage return to newline before delivering input; this is a plausible source of the mismatch, not a claim that it is the only WineDbg issue.

`launch_menu_startup_short_attach_root_v2.py` is a new copy; the v1/root controller and its failed run remain untouched. Before starting WineDbg, v2 calls `tty.setraw(slave, TCSANOW)`. Raw mode disables CR/NL input remapping and canonical buffering so the CR written to the master is delivered byte-for-byte to the child. It also disables echo/signals and output postprocessing; the existing reader continues to recognize fresh `Wine-dbg>` prompts in the byte stream. A second output directory (`menu-startup-short-attach-02`) prevents the future run from overwriting the failed receipt.

The PTY-only test `pty_cr_transport_test.py` exercises `openpty()` without starting a subprocess. On the default slave, it read `b"print $rip\\n"` after the controller wrote `b"print $rip\\r"`; with the v2 raw-mode helper it read `b"print $rip\\r"` unchanged. The test also asserts `ICRNL`, `INLCR`, `IGNCR`, canonical mode, and echo are disabled in raw mode. Result: PASS. No Wine, MO2, debugger, or game process was launched.

The v2 diff changes only the terminal imports, the new output directory, the raw-mode helper, and its call immediately after `openpty`. It preserves the launch and attach opt-in gates; short-alias and primary-hash checks; fresh USVFS guest identity plus unique new host-process gate; distinct guest/host PID receipt fields; source sandbox path; fresh-prompt gating between commands; command timeouts; 2 MiB transcript cap; prompted detach before quit; and bounded terminate/kill cleanup. Root owns any future launch and live attach review.

## Pinned inputs and outputs

- v1 controller SHA-256: `5f1dea32d159a349f32d58a0466866a5fa08d599d729e74d74965dcd0734fe12`
- Failed transcript SHA-256: `59190b00d10e4ed3ca2b2208b35d5214392e9bda6f695e417d47ec9e3bec8f1d`
- Failed launch receipt SHA-256: `8da9cb76b36a878d86d46c2c556070d352539a7a9c0d7a88de121b177632be51`
- v2 controller SHA-256: `61fdc5ad638b1a74da1a9e15c29fb6d8932de62a776725d8abc1cb0666fafbc7`
- PTY test SHA-256: `e1ef188712eae9aba482475b14ddba57042bf05503ac952ec1befa1f845b6aa5`
- Reviewed patch SHA-256: `25d5fac238ad2aa1cd993f680a4a3615606c6a6883ee9ab94d485ddb6454a7b2`

The original controller and failed transcript remain immutable inputs. No primary source, build, Wine prefix, or game state was changed by this v2 preparation.

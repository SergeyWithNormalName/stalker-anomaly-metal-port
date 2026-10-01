"""Pure local PTY line-discipline check; starts no subprocess or Wine process."""
from __future__ import annotations

import os
import pty
import select
import sys
import termios
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from launch_menu_startup_short_attach_root_v2 import configure_winedbg_pty_slave

COMMAND = b"print $rip"


def read_one_line_from_slave(configure_raw: bool) -> tuple[bytes, list]:
    master, slave = pty.openpty()
    try:
        if configure_raw:
            configure_winedbg_pty_slave(slave)
        attrs = termios.tcgetattr(slave)
        os.write(master, COMMAND + b"\r")
        ready, _, _ = select.select([slave], [], [], 1.0)
        if not ready:
            raise AssertionError("slave did not receive a completed command")
        return os.read(slave, 128), attrs
    finally:
        os.close(slave)
        os.close(master)


def main() -> int:
    canonical_bytes, canonical_attrs = read_one_line_from_slave(False)
    assert canonical_attrs[0] & termios.ICRNL, "test host PTY lacks default CR-to-NL mapping"
    assert canonical_attrs[3] & termios.ICANON, "test host PTY is not canonical by default"
    assert canonical_bytes == COMMAND + b"\n", repr(canonical_bytes)

    raw_bytes, raw_attrs = read_one_line_from_slave(True)
    assert not (raw_attrs[0] & termios.ICRNL), "raw mode retained ICRNL"
    assert not (raw_attrs[0] & termios.INLCR), "raw mode retained INLCR"
    assert not (raw_attrs[0] & termios.IGNCR), "raw mode retained IGNCR"
    assert not (raw_attrs[3] & termios.ICANON), "raw mode retained canonical buffering"
    assert not (raw_attrs[3] & termios.ECHO), "raw mode retained local echo"
    assert raw_bytes == COMMAND + b"\r", repr(raw_bytes)

    print(f"default slave input: {canonical_bytes!r} (CR became NL before child)")
    print(f"raw slave input: {raw_bytes!r} (CR preserved byte-for-byte)")
    print("PTY-only test; no subprocess, Wine, or game launched")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

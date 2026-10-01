from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT = Path(__file__).with_name("materialize_mo2_profile.py")
SPEC = importlib.util.spec_from_file_location("materialize_mo2_profile", SCRIPT)
assert SPEC and SPEC.loader
materializer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = materializer
SPEC.loader.exec_module(materializer)


class GamepadOverlayTests(unittest.TestCase):
    def test_gamepad_compat_overlay_is_added_above_existing_profile_winner(self) -> None:
        key = "scripts/gamepad_ui_compat.script"
        winners = {
            key: materializer.Candidate(
                key,
                "/mo2/mods/profile/gamedata/scripts/gamepad_ui_compat.script",
                "MO2 mod: profile script",
                10,
            )
        }

        applied = materializer.add_compatibility_gamepad_overlay(winners, priority=20)

        self.assertTrue(applied)
        self.assertEqual(len(winners), 1)
        self.assertEqual(
            winners[key].source,
            str((SCRIPT.parent.parent / "gamedata/scripts/gamepad_ui_compat.script").resolve()),
        )
        self.assertEqual(winners[key].priority, 20)
        self.assertIn("compatibility overlay", winners[key].layer)


if __name__ == "__main__":
    unittest.main()

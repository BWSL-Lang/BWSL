#!/usr/bin/env python3
"""Exercise Tint checkout setup against local Git repositories."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "bootstrap_tint", ROOT / "tools/wgsl-bench/bootstrap_tint.py")
bootstrap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bootstrap)


def git(*arguments):
    return subprocess.check_output(["git", *map(str, arguments)], text=True).strip()


class CheckoutTests(unittest.TestCase):
    def test_fresh_checkout_can_be_reused_and_other_revisions_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            remote, checkout = root / "remote", root / "dawn"
            git("init", remote)
            git("-C", remote, "-c", "user.name=Test", "-c", "user.email=test@example.com",
                "commit", "--allow-empty", "-m", "Pinned revision")
            revision = git("-C", remote, "rev-parse", "HEAD")
            with patch.object(bootstrap, "REMOTE", str(remote)), \
                    patch.object(bootstrap, "REVISION", revision):
                bootstrap.prepare_checkout(checkout)
                self.assertEqual(git("-C", checkout, "rev-parse", "HEAD"), revision)
                with patch.object(bootstrap, "run", side_effect=AssertionError("unexpected fetch")):
                    bootstrap.prepare_checkout(checkout)
                git("-C", checkout, "-c", "user.name=Test", "-c", "user.email=test@example.com",
                    "commit", "--allow-empty", "-m", "User work")
                user_revision = git("-C", checkout, "rev-parse", "HEAD")
                with self.assertRaisesRegex(SystemExit, "Refusing to overwrite checkout"):
                    bootstrap.prepare_checkout(checkout)
                self.assertEqual(git("-C", checkout, "rev-parse", "HEAD"), user_revision)


if __name__ == "__main__":
    unittest.main()

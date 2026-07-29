from __future__ import annotations

import json
import os
from pathlib import Path
import pickle
import stat
import unittest

from bioseq_packaging import signing


ARMORED_PRIVATE_KEY = """-----BEGIN PGP PRIVATE KEY BLOCK-----
test-only-private-key-material
-----END PGP PRIVATE KEY BLOCK-----
"""


class SigningMaterialTests(unittest.TestCase):
    def test_normalization_registers_every_secret_form_and_stays_redacted(self) -> None:
        registered: list[str] = []
        key = signing.normalize_signing_key(
            json.dumps({"GPG_PRIVATE_KEY": ARMORED_PRIVATE_KEY}),
            register_redaction=registered.append,
        )
        try:
            self.assertEqual(str(key), "<redacted>")
            self.assertEqual(repr(key), "SecretBytes(<redacted>)")
            self.assertIn(ARMORED_PRIVATE_KEY.strip(), registered)
            self.assertGreaterEqual(len(registered), 3)
            with self.assertRaises(TypeError):
                pickle.dumps(key)
            with self.assertRaises(TypeError):
                json.dumps(key)
        finally:
            key.destroy()
        self.assertTrue(key.destroyed)
        with self.assertRaises(signing.SigningError):
            signing.repository_environment(key).__enter__()

    def test_repository_environment_is_single_purpose_cleared_and_redacted(self) -> None:
        key = signing.normalize_signing_key(ARMORED_PRIVATE_KEY)
        exposed: dict[str, str] | None = None
        raw_value = ""
        try:
            with self.assertRaises(signing.SigningError) as raised:
                with signing.repository_environment(key) as environment:
                    exposed = environment
                    self.assertEqual(set(environment), {"GPG_PRIVATE_KEY"})
                    raw_value = environment["GPG_PRIVATE_KEY"]
                    raise RuntimeError(f"repository failed with {raw_value}")
            self.assertNotIn(raw_value, str(raised.exception))
            self.assertIn("<redacted>", str(raised.exception))
            self.assertEqual(exposed, {})
        finally:
            key.destroy()

    def test_isolated_workspace_has_strict_modes_and_is_always_removed(self) -> None:
        key = signing.normalize_signing_key(ARMORED_PRIVATE_KEY)
        workspace_home: Path | None = None
        try:
            with signing.signing_workspace(key) as workspace:
                workspace_home = workspace.home
                self.assertEqual(stat.S_IMODE(workspace.home.stat().st_mode), 0o700)
                self.assertIsNotNone(workspace.private_key_path)
                assert workspace.private_key_path is not None
                self.assertEqual(
                    stat.S_IMODE(workspace.private_key_path.stat().st_mode), 0o600
                )
                self.assertNotIn("GPG_PRIVATE_KEY", os.environ)
            assert workspace_home is not None
            self.assertFalse(workspace_home.exists())
        finally:
            key.destroy()

    def test_only_the_pinned_primary_fingerprint_is_accepted(self) -> None:
        pinned = signing.PINNED_SIGNING_FINGERPRINT
        self.assertEqual(signing.require_pinned_fingerprint(pinned.lower()), pinned)
        self.assertEqual(
            signing.parse_gpg_fingerprint(f"pub:::::::::\nfpr:::::::::{pinned}:\n"),
            pinned,
        )
        self.assertEqual(signing.parse_validsig(f"[GNUPG:] VALIDSIG {pinned}"), pinned)
        with self.assertRaises(signing.SigningError):
            signing.require_pinned_fingerprint("A" * 40)
        with self.assertRaises(signing.SigningError):
            signing.parse_validsig(f"[GNUPG:] VALIDSIG {'A' * 40}")


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "initialize_package_signing_key.py"
SPEC = importlib.util.spec_from_file_location("initialize_package_signing_key", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
signing = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = signing
SPEC.loader.exec_module(signing)


class CurrentSecretTests(unittest.TestCase):
    def test_existing_armored_secret_is_accepted_without_mutation(self) -> None:
        value = "-----BEGIN PGP PRIVATE KEY BLOCK-----\nsecret\n-----END PGP PRIVATE KEY BLOCK-----"
        with mock.patch.object(
            signing,
            "run",
            return_value=subprocess.CompletedProcess(
                [], 0, json.dumps({"SecretString": value}), ""
            ),
        ) as run:
            self.assertEqual(signing.current_secret("aws", "us-east-1", "arn"), value)
        run.assert_called_once()

    def test_unexpected_read_failure_never_rotates_the_key(self) -> None:
        denied = subprocess.CompletedProcess(
            [], 254, "", "AccessDeniedException: denied"
        )
        with mock.patch.object(signing, "run", return_value=denied) as run:
            with self.assertRaisesRegex(RuntimeError, "refusing to initialize"):
                signing.current_secret("aws", "us-east-1", "arn")
        run.assert_called_once()

    def test_empty_secret_container_is_the_only_initializable_state(self) -> None:
        missing_value = subprocess.CompletedProcess(
            [], 254, "", "ResourceNotFoundException: no AWSCURRENT value"
        )
        described = subprocess.CompletedProcess([], 0, "{}", "")
        with mock.patch.object(
            signing, "run", side_effect=[missing_value, described]
        ) as run:
            self.assertIsNone(signing.current_secret("aws", "us-east-1", "arn"))
        self.assertEqual(run.call_count, 2)


if __name__ == "__main__":
    unittest.main()

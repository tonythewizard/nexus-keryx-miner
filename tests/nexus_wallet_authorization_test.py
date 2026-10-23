#!/usr/bin/env python3
import contextlib
import importlib.util
import io
import pathlib
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location(
    "nexus_wallet_authorization", ROOT / "scripts/nexus_wallet_authorization.py")
wallet = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wallet)


class WalletAuthorizationTests(unittest.TestCase):
    def setUp(self):
        if len(sys.argv) < 2:
            raise RuntimeError("persistent test directory argument required")
        self.root = pathlib.Path(sys.argv[1]).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.cert_file = self.root / "wallet-output.escrow-cert"
        self.store = self.root / "escrow.cert"
        for path in (self.cert_file, self.store):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        self.cert = "a1" * 64
        self.payout = "keryx:test-address"
        self.pubkey = "b2" * 32

    def args(self):
        return type("Args", (), {"certificate": self.cert_file,
                                  "cert_store": self.store,
                                  "launcher": pathlib.Path("launcher"),
                                  "key": pathlib.Path("key"),
                                  "probe": pathlib.Path("probe")})()

    def invoke(self, valid=True, legacy=None, stored=None, valid_existing=None):
        self.cert_file.write_text(self.cert + "\n", encoding="ascii")
        if stored:
            self.store.write_text(stored + "\n", encoding="ascii")
        with mock.patch.object(wallet, "resolve_config",
                               return_value=(self.payout, self.pubkey, legacy or "c3" * 64)), \
             mock.patch.object(wallet, "verify_certificate",
                               side_effect=lambda _p, _k, cert, *_args:
                               valid and (cert == self.cert or cert == valid_existing)), \
             mock.patch.object(wallet, "atomic_store") as save, \
             contextlib.redirect_stdout(io.StringIO()) as output:
            rc = wallet.command_import(self.args())
        return rc, save, output.getvalue()

    def test_rejects_noncanonical_certificate_length(self):
        self.cert_file.write_text("a1" * 63, encoding="ascii")
        with self.assertRaises(ValueError):
            wallet.read_certificate(self.cert_file)

    def test_wrong_payout_or_pubkey_is_rejected_without_storage(self):
        rc, save, output = self.invoke(valid=False)
        self.assertEqual(rc, 3)
        save.assert_not_called()
        self.assertIn("payout_or_escrow_key_mismatch", output)

    def test_matching_wallet_delegation_is_stored_only_after_verification(self):
        rc, save, output = self.invoke(valid=True)
        self.assertEqual(rc, 0)
        save.assert_called_once_with(self.store, self.cert)
        self.assertIn("certificate_verified=true", output)

    def test_valid_existing_certificate_is_never_replaced(self):
        rc, save, output = self.invoke(valid=True, stored="d4" * 64,
                                       valid_existing="d4" * 64)
        self.assertEqual(rc, 3)
        save.assert_not_called()
        self.assertIn("refusing_to_replace_valid_certificate", output)

    def test_already_configured_valid_authorization_remains_unchanged(self):
        rc, save, output = self.invoke(valid=True, legacy=self.cert)
        self.assertEqual(rc, 0)
        save.assert_not_called()
        self.assertIn("existing_valid_certificate_preserved=true", output)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: nexus_wallet_authorization_test.py PERSISTENT_TEST_DIR")
    unittest.main(argv=[sys.argv[0]], verbosity=2)

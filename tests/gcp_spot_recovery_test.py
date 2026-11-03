#!/usr/bin/env python3
"""Offline recovery contracts. All cloud/process operations are fake or mocked."""
import copy
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock
from urllib.error import HTTPError
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gcp_spot_checkpoint as checkpoint
import gcp_spot_cloud as transport
import gcp_spot_janitor as janitor
import gcp_spot_recovery as recovery
import gcp_spot_runner as spot
import gcp_spot_setup as setup
import remote_runner as runner


def upload_plan(run_id="run-" + "a" * 32):
    base = "/test-bucket/gti-spot/v2/" + "b" * 32 + "/checkpoints/" + run_id + "/"
    query = urlencode({"X-Goog-Algorithm": "GOOG4-RSA-SHA256", "X-Goog-SignedHeaders":
                       "content-length;content-type;host;x-goog-if-generation-match",
                       "X-Goog-Signature": "abc", "X-Goog-Expires": "5400"})
    return {"schema": 1, "run": run_id, "prefix": base,
            "urls": ["https://storage.googleapis.com" + base + name + ".tar.gz?" + query for name in ("00", "final")]}


class FakeCloud:
    def __init__(self):
        self.calls = []
        self.docs = {}
        self.inventory = []
        self.billing = {"jobComplete": True, "schema": {"fields": [{"name": name} for name in ("currency", "total", "daily")]},
                        "rows": [{"f": [{"v": "AUD"}, {"v": "0"}, {"v": "0"}]}]}

    def object(self, bucket, name):
        self.calls.append(("object", name))
        return copy.deepcopy(self.docs[name])

    def objects(self, bucket, base, maximum=8):
        return copy.deepcopy(self.inventory)

    def create(self, bucket, name, value):
        if name in self.docs:
            raise spot.SpotError("immutable duplicate")
        self.calls.append(("create", name))
        self.docs[name] = copy.deepcopy(value)

    def replace(self, bucket, name, value):
        self.calls.append(("replace", name))
        self.docs[name] = copy.deepcopy(value)

    def request(self, method, service, path, value=None, **kwargs):
        self.calls.append((method, service, path, copy.deepcopy(value)))
        if service == "bigquery":
            return copy.deepcopy(self.billing)
        return {}


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.now = time.time()
        self.config = json.loads((ROOT / "scripts/gcp_spot_policy.example.json").read_text())
        self.config.update(enabled=True, controller_hostname=spot.platform.node(),
                           pricing_valid_until=spot.utc(self.now + 86400), trial_acknowledged_until=spot.utc(self.now + 86400))
        self.settings = json.loads((ROOT / "scripts/gcp_spot_recovery.example.json").read_text())
        self.settings.update(enabled=True, pilot_expires_at=spot.utc(self.now + 86400))
        self.ledger = spot.Ledger(self.root / "ledger")
        self.lease = self.ledger.reserve(self.config, "run-" + "a" * 32, self.now)
        self.ledger.update(self.lease, source={"head": "c" * 40, "snapshot_sha256": "d" * 64})
        self.cloud = FakeCloud()
        self.client = recovery.Recovery(self.config, self.settings, self.cloud)
        self.registry = recovery.registration(self.config, self.settings, self.lease["pool"], spot.utc(self.now - 60))
        self.client.registry = self.registry
        self.client.pool = self.lease["pool"]
        self.client.base = recovery.prefix(self.lease["pool"])

    def test_setup_is_disabled_and_pins_identity(self):
        value = setup.render(self.config, self.settings, self.lease["pool"], self.settings["janitor_image"], self.now)
        self.assertFalse(value["enable_scheduler"])
        self.assertEqual(value["pool"], self.lease["pool"])
        with self.assertRaises(spot.SpotError):
            setup.render(self.config, self.settings, self.lease["pool"], "some-image:latest", self.now)
        with self.assertRaises(spot.SpotError):
            setup.render(self.config, dict(self.settings, pilot_expires_at=spot.utc(self.now + 8 * 86400)),
                         self.lease["pool"], self.settings["janitor_image"], self.now)

    def test_recovery_policy_rejects_unknown_or_unbounded_fields(self):
        path = self.root / "recovery.json"
        for key, value in (("schema", True), ("enabled", 1), ("billing_maximum_bytes_billed", 10**12),
                           ("infrastructure_reserve_micros", -1), ("janitor_image", "mutable:latest"),
                           ("billing_table", "table`; DELETE FROM anything"), ("unexpected", 0)):
            path.write_text(json.dumps(dict(self.settings, **{key: value})))
            with self.assertRaises(spot.SpotError, msg=key):
                recovery.configuration(path, self.config)

    def test_lease_rejects_foreign_identity_lifetime_and_source(self):
        recovery.validate_lease(self.lease, self.registry)
        for changes in ({"pool": "f" * 32}, {"project": "foreign-project"}, {"source": {}},
                        {"expires": spot.utc(self.now + 5401)}, {"created": spot.utc(self.now - 120)},
                        {"image": "projects/foreign/global/images/test-image"}):
            with self.assertRaises(spot.SpotError):
                recovery.validate_lease(dict(self.lease, **changes), self.registry)

    def test_lease_is_durable_before_billing_failure(self):
        self.cloud.billing["jobComplete"] = False
        with mock.patch.object(recovery.subprocess, "run") as command:
            with self.assertRaises(spot.SpotError):
                self.client.prepare(self.ledger, self.lease)
            command.assert_not_called()
        self.assertIn(self.client.base + "leases/" + self.lease["id"] + ".json", self.cloud.docs)
        self.assertEqual(self.cloud.calls[0][0], "create")

    def test_gross_billing_is_bounded_and_never_refunds(self):
        self.client.observe_cost(self.ledger, self.lease)
        query = self.cloud.calls[-1][3]
        self.assertIn("GREATEST(cost, 0)", query["query"])
        self.assertNotIn("credits", query["query"])
        self.assertEqual(query["maximumBytesBilled"], str(10 * 1024 * 1024))
        with self.assertRaises(spot.SpotError):
            self.ledger.observe_cost(self.config, self.lease, 21_000_000, 0, 1, self.now)
        with self.assertRaises(spot.SpotError):
            self.ledger.observe_cost(self.config, self.lease, 0, 0, 1, self.now + 1)
        self.assertEqual(self.ledger.status()["cost_observation"]["total_micros"], 21_000_000)
        self.ledger.update(self.lease, state="deleted")
        with self.assertRaises(spot.SpotError):
            self.ledger.reserve(self.config, "run-" + "e" * 32, self.now + 86400)

    def test_billing_rejects_wrong_currency_or_untrusted_shape(self):
        for fields in ([{"v": "USD"}, {"v": "1"}, {"v": "1"}],
                       [{"v": "AUD"}, {"v": "-1"}, {"v": "0"}],
                       [{"v": "AUD"}, {"v": "1.5"}, {"v": "0"}]):
            self.cloud.billing["rows"] = [{"f": fields}]
            with self.assertRaises(spot.SpotError):
                self.client.observe_cost(self.ledger, self.lease)

    def test_capabilities_are_exact_create_only_objects(self):
        plan = upload_plan()
        checkpoint.validate_plan(plan, plan["run"])
        for change in ("http://storage.googleapis.com", "https://foreign.example", "https://storage.googleapis.com:443"):
            bad = copy.deepcopy(plan)
            bad["urls"][0] = bad["urls"][0].replace("https://storage.googleapis.com", change)
            with self.assertRaises(ValueError):
                checkpoint.validate_plan(bad, bad["run"])
        with self.assertRaises(ValueError):
            checkpoint.validate_plan(dict(plan, urls=plan["urls"] * 17), plan["run"])

    def test_upload_is_fixed_size_and_does_not_follow_redirects(self):
        plan = upload_plan()
        response = mock.MagicMock()
        response.__enter__.return_value.status = 200
        opener = mock.Mock()
        opener.open.return_value = response
        with mock.patch.object(checkpoint, "build_opener", return_value=opener):
            checkpoint.put(plan["urls"][0], b"small", checkpoint.CHECKPOINT_BYTES)
        request = opener.open.call_args.args[0]
        self.assertEqual(len(request.data), checkpoint.CHECKPOINT_BYTES)
        self.assertEqual(request.get_header("X-goog-if-generation-match"), "0")
        self.assertEqual(opener.open.call_args.kwargs["timeout"], 30)

    def test_checkpoint_contains_only_receipts_and_log_tails(self):
        run = self.root / self.lease["id"]
        (run / "input").mkdir(parents=True)
        (run / "source").mkdir()
        (run / "logs").mkdir()
        (run / "input" / "checkpoint_upload.json").write_text("PRIVATE CAPABILITY")
        (run / "source" / ".env").write_text("PRIVATE SOURCE")
        (run / "logs" / "test.log").write_text("diagnostic tail")
        data = checkpoint.snapshot(run, self.lease["source"])
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            names = archive.getnames()
            self.assertEqual(names, ["checkpoint.json", "logs/test.log"])
            report = json.load(archive.extractfile("checkpoint.json"))
            self.assertTrue(report["partial"])
            self.assertEqual(report["source"], self.lease["source"])

    def test_janitor_never_deletes_unexpired_or_foreign_leases(self):
        name = self.client.base + "leases/" + self.lease["id"] + ".json"
        self.cloud.docs[name] = self.lease
        self.cloud.inventory = [{"name": name}]
        provider = mock.Mock()
        janitor.reap(self.cloud, provider, self.settings["bucket"], self.registry, self.now)
        provider.delete.assert_not_called()
        janitor.reap(self.cloud, provider, self.settings["bucket"], self.registry, self.now + 5600)
        provider.delete.assert_called_once()
        # Existing absence receipts must not suppress later deletion checks.
        janitor.reap(self.cloud, provider, self.settings["bucket"], self.registry, self.now + 5700)
        self.assertEqual(provider.delete.call_count, 2)
        self.cloud.docs[name] = dict(self.lease, pool="f" * 32)
        result = janitor.reap(self.cloud, provider, self.settings["bucket"], self.registry, self.now + 5800)
        self.assertFalse(result["healthy"])
        self.assertEqual(provider.delete.call_count, 2)

    def test_final_upload_precedes_terminal_publication(self):
        run = self.root / "publication"
        run.mkdir()
        state = {"id": self.lease["id"], "state": "passed", "exit_code": 0}
        with tarfile.open(run / "results.tar.gz", "w:gz") as archive:
            runner.add_bytes(archive, "report.json", json.dumps(state).encode())
        seen = []
        def upload(path):
            self.assertTrue(path.is_file())
            self.assertFalse((run / "state.json").exists())
            seen.append("uploaded")
        runner.publish_report(run, state, before_publish=upload)
        self.assertEqual(seen, ["uploaded"])
        self.assertEqual(json.loads((run / "state.json").read_text())["state"], "passed")
        def failed_upload(_path):
            raise RuntimeError("private signed URL must not enter diagnostics")
        runner.publish_report(run, state, before_publish=failed_upload)
        final = json.loads((run / "state.json").read_text())
        self.assertEqual(final["state"], "failed")
        self.assertIn("durable_result_error", final)
        self.assertNotIn("private signed URL", json.dumps(final))

    def test_expired_janitor_pauses_even_when_reaping_fails(self):
        self.registry["pilot_expires_at"] = spot.utc(self.now - 1)
        self.cloud.docs["gti-spot/v2/registry.json"] = self.registry
        args = ["--project", self.config["project"], "--bucket", self.settings["bucket"],
                "--pool", self.lease["pool"], "--expires", self.registry["pilot_expires_at"],
                "--scheduler", self.settings["scheduler_job"]]
        with mock.patch.object(janitor.Cloud, "workload", return_value=self.cloud), \
                mock.patch.object(janitor, "Google"), \
                mock.patch.object(janitor, "reap", side_effect=RuntimeError("lost API")), \
                mock.patch.object(janitor.time, "time", return_value=self.now + 180):
            with self.assertRaises(RuntimeError):
                janitor.main(args)
        self.assertEqual(self.cloud.calls[-1][0:2], ("POST", "cloudscheduler"))
        self.assertTrue(self.cloud.calls[-1][2].endswith(":pause"))

    def test_recovery_rejects_wrong_snapshot_before_download(self):
        self.cloud.docs["gti-spot/v2/registry.json"] = self.registry
        self.cloud.docs[self.client.base + "leases/" + self.lease["id"] + ".json"] = self.lease
        with mock.patch.object(recovery.subprocess, "run") as command:
            with self.assertRaises(spot.SpotError):
                recovery.recover(self.config, self.settings, self.lease["id"], "f" * 64, self.root / "output", cloud=self.cloud)
            command.assert_not_called()
        self.assertFalse((self.root / "output").exists())

    def test_missing_cloud_objects_are_not_inferred_from_permission_errors(self):
        cloud = transport.Cloud(lambda: "private-token")
        for code in (403, 404):
            opener = mock.Mock()
            opener.open.side_effect = HTTPError("https://storage.googleapis.com/test", code, "failure", {}, None)
            with mock.patch.object(transport, "build_opener", return_value=opener):
                if code == 404:
                    self.assertIsNone(cloud.object("test-bucket", "test", missing=True))
                else:
                    with self.assertRaises(spot.SpotError):
                        cloud.object("test-bucket", "test", missing=True)


if __name__ == "__main__":
    unittest.main()

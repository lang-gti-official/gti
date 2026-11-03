#!/usr/bin/env python3
"""Offline cost admission and fault-injected cloud lifecycle. Never uses GCP."""
import copy
import json
import multiprocessing
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gcp_spot_runner as spot
import remote_runner as runner
from gcp_spot_recovery_test import RecoveryTests


def configuration(now=None):
    now = time.time() if now is None else now
    data = json.loads((ROOT / "scripts/gcp_spot_policy.example.json").read_text())
    data.update(enabled=True, controller_hostname=platform.node(),
                pricing_valid_until=spot.utc(now + 86400),
                trial_acknowledged_until=spot.utc(now + 86400))
    return data


def instance(config, lease):
    prefix = "https://www.googleapis.com/compute/v1/"
    return {"name": lease["instance"], "id": "123456789", "zone": prefix + "projects/" + config["project"] + "/zones/" + config["zone"],
            "machineType": prefix + "projects/" + config["project"] + "/zones/" + config["zone"] + "/machineTypes/" + config["machine_type"],
            "labels": {"gti-runner": "spot-v1", "gti-pool": lease["pool"], "gti-run": lease["id"][4:]},
            "scheduling": {"provisioningModel": "SPOT", "instanceTerminationAction": "DELETE",
                           "automaticRestart": False, "maxRunDuration": {"seconds": str(config["vm_lifetime_seconds"])}},
            "disks": [{"boot": True, "autoDelete": True, "diskSizeGb": str(config["boot_disk_gib"]),
                       "source": prefix + "projects/" + config["project"] + "/zones/" + config["zone"] + "/disks/" + lease["instance"]}],
            "networkInterfaces": [{"subnetwork": prefix + config["subnetwork"]}]}


class FakeGoogle:
    def __init__(self):
        self.calls = []
        self.fail = None
        self.present = False

    def preflight(self):
        self.calls.append("preflight")

    def create(self, lease):
        self.calls.append("create")
        self.present = True
        if self.fail == "create":
            raise TimeoutError("lost insert response")

    def verify(self, lease):
        self.calls.append("verify")
        if self.fail == "verify":
            raise spot.SpotError("invalid deadline")
        return {}

    def host_keys(self, lease, actual, path):
        self.calls.append("keys")
        path.write_text("public host key\n")

    def delete(self, lease):
        self.calls.append("delete")
        if self.fail == "delete":
            raise spot.SpotError("delete API unavailable")
        self.present = False


def reserve_child(root, config, number, queue):
    try:
        spot.Ledger(root).reserve(config, "run-" + f"{number:032x}", time.time())
        queue.put("reserved")
    except spot.SpotError:
        queue.put("blocked")


class SpotRunner(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gti-spot-test-")
        self.root = Path(self.temporary.name)
        self.config = configuration()
        self.ledger = spot.Ledger(self.root / "controller")
        self.provider = FakeGoogle()

    def tearDown(self):
        self.temporary.cleanup()

    def reserve(self, value=1):
        return self.ledger.reserve(self.config, "run-" + f"{value:032x}", time.time())

    def arguments(self, *extra):
        return runner.parser().parse_args(["--repo", str(ROOT), "--runner", "gcp-spot", "run", "--target", "gti", *extra])

    def execute(self, args=None, failure=None):
        def executor(args, local, name, config, run_id, before_submit):
            self.assertEqual(name, "gcp-spot")
            self.assertEqual(config["transport"], "gcloud-iap")
            runner.validate_connection(config)
            before_submit({"head": "a" * 40, "snapshot_sha256": "b" * 64})
            if failure:
                raise failure
            return 0
        with mock.patch.object(runner, "execute_run", side_effect=executor), \
             mock.patch.object(runner, "local_directory", return_value=self.root), \
             mock.patch.object(runner, "connection", return_value={"platform": "Linux", "machine": "x86_64"}):
            return spot.run(args or self.arguments(), runner, self.config, self.ledger, self.provider)

    def test_policy_disabled_expired_and_shape(self):
        path = self.root / "policy.json"
        for key, value in (("max_instances", 3), ("boot_disk_gib", 101), ("total_limit_micros", True),
                           ("vm_lifetime_seconds", 5401), ("schema", True), ("unexpected", "field"),
                           ("image", "projects/a/global/images/family/latest"), ("subnetwork", "projects/other/regions/us-central1/subnetworks/private")):
            data = dict(self.config, **{key: value})
            path.write_text(json.dumps(data))
            with self.assertRaises(spot.SpotError, msg=key):
                spot.policy(path)
        for change in ({"enabled": False}, {"controller_hostname": "another-host"},
                       {"pricing_valid_until": spot.utc(time.time() - 1)},
                       {"trial_acknowledged_until": spot.utc(time.time() + 10 * 86400)}):
            with self.assertRaises(spot.SpotError):
                spot.admission(dict(self.config, **change), time.time())

    def test_one_slot_and_no_refund_or_policy_reset(self):
        first = self.reserve()
        self.assertEqual(first["cost_micros"], 3_250_000)
        with self.assertRaisesRegex(spot.SpotError, "occupied"):
            self.reserve(2)
        self.ledger.update(first, creation_confirmed=True)
        spot.cleanup(self.provider, self.ledger, first)
        with self.assertRaisesRegex(spot.SpotError, "daily"):
            self.reserve(2)
        with self.assertRaisesRegex(spot.SpotError, "another project"):
            self.ledger.reserve(dict(self.config, project="another-project"), "run-" + "a" * 32, time.time())
        with self.assertRaisesRegex(spot.SpotError, "duplicate"):
            self.reserve()

    def test_total_allowance_survives_day_boundary(self):
        self.config.update(hourly_ceiling_micros=1_000_000, per_run_extra_micros=1_000_000,
                           daily_limit_micros=5_000_000, total_limit_micros=5_000_000)
        now = time.time()
        for i in range(2):
            self.config.update(pricing_valid_until=spot.utc(now + (i + 1) * 86400),
                               trial_acknowledged_until=spot.utc(now + (i + 1) * 86400))
            lease = self.ledger.reserve(self.config, "run-" + f"{i:032x}", now + i * 86400)
            self.ledger.update(lease, state="deleted")
        with self.assertRaisesRegex(spot.SpotError, "total"):
            self.ledger.reserve(self.config, "run-" + "f" * 32, now + 86400)

    def test_multiple_processes_share_one_slot(self):
        context = multiprocessing.get_context("spawn")
        queue = context.Queue()
        children = [context.Process(target=reserve_child, args=(self.ledger.root, self.config, n, queue)) for n in range(4)]
        for child in children:
            child.start()
        results = [queue.get(timeout=15) for _ in children]
        for child in children:
            child.join(10)
            self.assertEqual(child.exitcode, 0)
        self.assertEqual(results.count("reserved"), 1)
        self.assertEqual(results.count("blocked"), 3)

    def test_attempt_caps_are_independent_of_price_estimates(self):
        self.config.update(hourly_ceiling_micros=1, per_run_extra_micros=1)
        now = time.time()
        for i in range(8):
            day = i // 2
            self.config.update(pricing_valid_until=spot.utc(now + (day + 1) * 86400),
                               trial_acknowledged_until=spot.utc(now + (day + 1) * 86400))
            lease = self.ledger.reserve(self.config, "run-" + f"{i:032x}", now + day * 86400)
            self.ledger.update(lease, state="deleted")
            if i == 1:
                with self.assertRaisesRegex(spot.SpotError, "two-attempt"):
                    self.ledger.reserve(self.config, "run-" + "f" * 32, now)
        with self.assertRaisesRegex(spot.SpotError, "eight-attempt"):
            self.ledger.reserve(self.config, "run-" + "f" * 32, now + 3 * 86400)

    def test_missing_or_corrupt_journal_fails_closed(self):
        self.reserve()
        path = self.ledger.root / "leases.json"
        path.unlink()
        with self.assertRaisesRegex(spot.SpotError, "missing"):
            self.reserve(2)
        path.write_text("{broken")
        with self.assertRaises(ValueError):
            self.reserve(2)

    def test_corrupt_confirmation_cannot_release_uncertain_creation(self):
        self.reserve()
        path = self.ledger.root / "leases.json"
        state = json.loads(path.read_text())
        state["leases"][0]["creation_confirmed"] = "false"
        path.write_text(json.dumps(state))
        with self.assertRaisesRegex(spot.SpotError, "lifecycle"):
            self.ledger.status()

    def test_complete_cancel_disconnect_and_bad_boot_delete(self):
        self.assertEqual(self.execute(), 0)
        self.assertFalse(self.provider.present)
        self.assertEqual(self.ledger.status()["leases"][0]["state"], "deleted")
        for error in (KeyboardInterrupt(), runner.RunnerError("SSH disconnected"), SystemExit(2)):
            with self.subTest(error=type(error).__name__):
                other = spot.Ledger(self.root / type(error).__name__)
                with mock.patch.object(self, "ledger", other):
                    with self.assertRaises(type(error)):
                        self.execute(failure=error)
                self.assertFalse(self.provider.present)
                self.assertEqual(other.status()["leases"][0]["state"], "deleted")
        self.provider.fail = "verify"
        with mock.patch.object(self, "ledger", spot.Ledger(self.root / "bad-boot")):
            with self.assertRaisesRegex(spot.SpotError, "invalid deadline"):
                self.execute()
        self.assertFalse(self.provider.present)

    def test_unknown_create_and_delete_failure_keep_slot(self):
        for mode in ("create", "delete"):
            with self.subTest(mode=mode):
                self.provider.fail = mode
                ledger = spot.Ledger(self.root / mode)
                with mock.patch.object(self, "ledger", ledger):
                    with self.assertRaises(spot.SpotError):
                        self.execute()
                    self.assertEqual(ledger.status()["leases"][0]["state"], "cleanup-required")
                    with self.assertRaisesRegex(spot.SpotError, "occupied"):
                        self.reserve(2)

    def test_sigterm_cleans_and_restores_the_original_handler(self):
        old_handler = signal.getsignal(signal.SIGTERM)
        def executor(*args):
            args[-1]()
            signal.raise_signal(signal.SIGTERM)
        with mock.patch.object(runner, "execute_run", side_effect=executor), \
             mock.patch.object(runner, "local_directory", return_value=self.root), \
             mock.patch.object(runner, "connection", return_value={"platform": "Linux", "machine": "x86_64"}):
            with self.assertRaises(KeyboardInterrupt):
                spot.run(self.arguments(), runner, self.config, self.ledger, self.provider)
        self.assertEqual(signal.getsignal(signal.SIGTERM), old_handler)
        self.assertFalse(self.provider.present)
        self.assertEqual(self.ledger.status()["leases"][0]["state"], "deleted")

    def test_invalid_requests_and_snapshot_failure_never_provision(self):
        for extra in (("--detach",), ("--full-suite",), ("--platform", "Darwin"), ("--timeout", "7200"), ("--jobs", "64")):
            with self.assertRaises(spot.SpotError):
                self.execute(self.arguments(*extra))
        self.assertFalse(self.provider.calls)
        with mock.patch.object(runner, "execute_run", side_effect=runner.RunnerError("invalid source snapshot")):
            with self.assertRaises(runner.RunnerError):
                spot.run(self.arguments(), runner, self.config, self.ledger, self.provider)
        self.assertNotIn("create", self.provider.calls)
        self.assertFalse(self.ledger.root.exists())

    def test_provider_commands_pin_cost_and_identity_controls(self):
        google = spot.Google(self.config)
        lease = self.reserve()
        with mock.patch.object(google, "command", return_value=[]) as command:
            google.create(lease)
            args = command.call_args.args
            for flag in ("--provisioning-model=SPOT", "--instance-termination-action=DELETE", "--no-address",
                         "--no-service-account", "--no-scopes", "--boot-disk-auto-delete", "--no-restart-on-failure"):
                self.assertIn(flag, args)
            self.assertIn("--max-run-duration=5400s", args)
            self.assertFalse(any(a.startswith("--termination-time") for a in args))
        with mock.patch("subprocess.run", return_value=subprocess.CompletedProcess([], 0, "[]", "")) as execute:
            google.instances()
            argv = execute.call_args.args[0]
            self.assertIn("--account=" + self.config["account"], argv)
            self.assertIn("--project=" + self.config["project"], argv)
            self.assertEqual(execute.call_args.kwargs["timeout"], 90)
        with mock.patch("subprocess.run", return_value=subprocess.CompletedProcess([], 0, "", "")):
            self.assertIsNone(google.command("compute", "instances", "delete", lease["instance"]))
            with self.assertRaisesRegex(spot.SpotError, "not proof of absence"):
                google.instances()
        for malformed in (None, {}, False, [None]):
            with mock.patch.object(google, "command", return_value=malformed):
                with self.assertRaisesRegex(spot.SpotError, "absence is unproven"):
                    google.instances()

    def test_read_only_preflight_rejects_billing_currency_image_and_existing_vms(self):
        google = spot.Google(self.config)
        responses = [{"billingAccountName": "billingAccounts/" + self.config["billing_account"], "billingEnabled": True},
                     {"open": True, "currencyCode": self.config["currency"]},
                     {"privateIpGoogleAccess": True}, {"status": "READY", "architecture": "X86_64"}, [], []]
        with mock.patch.object(google, "command", side_effect=responses):
            google.preflight()
        for index, replacement in ((0, {"billingEnabled": False}), (1, {"open": True, "currencyCode": "USD"}),
                                   (2, {}), (3, {"status": "READY", "architecture": "ARM64"}),
                                   (4, [{"name": "unmanaged"}]), (5, [{"name": "orphan-disk"}])):
            bad = copy.deepcopy(responses)
            bad[index] = replacement
            with mock.patch.object(google, "command", side_effect=bad), self.assertRaises(spot.SpotError):
                google.preflight()

    def test_created_vm_contract_and_host_keys(self):
        google = spot.Google(self.config)
        lease = self.reserve()
        actual = instance(self.config, lease)
        with mock.patch.object(google, "instances", return_value=[actual]):
            self.assertEqual(google.verify(lease), actual)
        variants = []
        for key, value in (("instanceTerminationAction", "STOP"), ("maxRunDuration", {"seconds": "86400"}), ("automaticRestart", True)):
            bad = copy.deepcopy(actual)
            bad["scheduling"][key] = value
            variants.append(bad)
        bad = copy.deepcopy(actual)
        bad["networkInterfaces"][0]["accessConfigs"] = [{"natIP": "203.0.113.1"}]
        variants.append(bad)
        bad = copy.deepcopy(actual)
        bad["disks"][0]["autoDelete"] = False
        variants.append(bad)
        bad = copy.deepcopy(actual)
        bad["labels"]["gti-pool"] = "another-pool"
        variants.append(bad)
        for bad in variants:
            with mock.patch.object(google, "instances", return_value=[bad]):
                with self.assertRaises(spot.SpotError):
                    google.verify(lease)
        for data in ([{"key": "ssh-ed25519", "value": "YWJjZA=="}],
                     {"queryValue": {"items": [{"key": "ssh-ed25519", "value": "YWJjZA=="}]}}):
            with mock.patch.object(google, "command", return_value=data):
                destination = self.root / "host-keys"
                google.host_keys(lease, actual, destination)
                self.assertEqual(destination.read_text(), "compute.123456789 ssh-ed25519 YWJjZA==\n")
        with mock.patch.object(google, "command", return_value=[]):
            with self.assertRaisesRegex(spot.SpotError, "refusing"):
                google.host_keys(lease, actual, self.root / "keys")

    def test_foreign_resources_and_api_failure_are_not_absence(self):
        google = spot.Google(self.config)
        lease = self.reserve()
        foreign = instance(self.config, lease)
        foreign["labels"]["gti-pool"] = "other"
        with mock.patch.object(google, "instances", return_value=[foreign]), mock.patch.object(google, "command") as command:
            with self.assertRaisesRegex(spot.SpotError, "refusing"):
                google.delete(lease)
            command.assert_not_called()
        with mock.patch.object(google, "instances", side_effect=spot.SpotError("API unavailable")):
            with self.assertRaisesRegex(spot.SpotError, "API unavailable"):
                google.delete(lease)

    def test_iap_transport_does_not_disable_host_verification(self):
        config = {"host": "gti-spot-" + "a" * 32, "root": "/tmp/gti-remote-runner", "python": "/usr/bin/python3",
                  "transport": "gcloud-iap", "gcp_project": self.config["project"], "gcp_zone": self.config["zone"],
                  "gcp_account": self.config["account"], "known_hosts": str(self.root / "keys")}
        runner.validate_connection(config)
        with mock.patch.object(runner.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"{}", b"")) as command:
            runner.connection(config, "status")
            argv = command.call_args.args[0]
            self.assertIn("--tunnel-through-iap", argv)
            self.assertIn("--strict-host-key-checking=yes", argv)
            self.assertIn("--ssh-flag=-oForwardAgent=no", argv)
            self.assertIn("--ssh-flag=-oClearAllForwardings=yes", argv)
            self.assertFalse(any("StrictHostKeyChecking=no" in a for a in argv))

    def test_plan_is_offline_and_default_example_cannot_run(self):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/gcp_spot_runner.py"),
                                 "--policy", str(ROOT / "scripts/gcp_spot_policy.example.json"), "plan"],
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(json.loads(result.stdout)["enabled"])
        with self.assertRaises(spot.SpotError), mock.patch.object(spot.Google, "command") as command:
            spot.admission(spot.policy(ROOT / "scripts/gcp_spot_policy.example.json"), time.time())
        command.assert_not_called()


if __name__ == "__main__":
    unittest.main()

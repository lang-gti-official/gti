#!/usr/bin/env python3
"""Opt-in, single-controller Spot leases around the ordinary SSH executor.

This is a conservative admission allowance, not a provider billing hard cap.
No credentials, price discovery, billing upgrade, quota increase or paid
fallback is performed. See the remote-host contract before enabling a policy.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import sys
import time
import uuid

DEFAULT_POLICY = Path.home() / ".config/gti/spot-runner.json"
STATE_ROOT = Path.home() / ".local/state/gti-spot-runner"
FIELDS = {"schema", "enabled", "project", "account", "billing_account", "currency", "controller_hostname",
          "zone", "machine_type", "image", "subnetwork", "boot_disk_gib",
          "max_instances", "vm_lifetime_seconds", "job_timeout_seconds",
          "daily_limit_micros", "total_limit_micros", "hourly_ceiling_micros",
          "per_run_extra_micros", "pricing_valid_until", "trial_acknowledged_until"}


class SpotError(ValueError):
    pass


def timestamp(value):
    if not isinstance(value, str) or not value.endswith("Z"):
        raise SpotError("timestamps must be explicit UTC strings ending in Z")
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except ValueError as error:
        raise SpotError("invalid UTC policy timestamp") from error


def provider_timestamp(value):
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            raise ValueError("missing timezone")
        return parsed.timestamp()
    except (AttributeError, TypeError, ValueError) as error:
        raise SpotError("invalid provider timestamp") from error


def utc(value):
    return datetime.fromtimestamp(value, timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def policy(path):
    data = json.loads(Path(path).read_text())
    if not isinstance(data, dict) or set(data) != FIELDS or type(data["schema"]) is not int or data["schema"] != 1:
        raise SpotError("Spot policy must have schema 1 and exactly the documented fields")
    patterns = {"project": r"[a-z][a-z0-9-]{4,61}[a-z0-9]",
                "account": r"[A-Za-z0-9_.+%-]+@[A-Za-z0-9.-]+",
                "billing_account": r"[A-Z0-9]{6}-[A-Z0-9]{6}-[A-Z0-9]{6}",
                "currency": r"[A-Z]{3}", "zone": r"[a-z]+-[a-z0-9]+[0-9]-[a-z]",
                "image": r"projects/[a-z][a-z0-9-]+/global/images/[a-z][a-z0-9-]+",
                "subnetwork": r"projects/[a-z][a-z0-9-]+/regions/[a-z]+-[a-z0-9]+[0-9]/subnetworks/[a-z][a-z0-9-]+"}
    for key, pattern in patterns.items():
        if not isinstance(data[key], str) or not re.fullmatch(pattern, data[key]):
            raise SpotError("invalid Spot policy " + key)
    if not isinstance(data["controller_hostname"], str) or not re.fullmatch(r"[A-Za-z0-9.-]{1,253}", data["controller_hostname"]):
        raise SpotError("invalid controller hostname")
    if type(data["enabled"]) is not bool or data["machine_type"] not in ("e2-standard-4", "e2-standard-8"):
        raise SpotError("Spot requires an explicit enable switch and an approved E2 machine profile")
    limits = {"boot_disk_gib": (20, 100), "max_instances": (1, 1),
              "vm_lifetime_seconds": (1200, 5400), "job_timeout_seconds": (60, 3600),
              "daily_limit_micros": (1, 5_000_000), "total_limit_micros": (1, 20_000_000),
              "hourly_ceiling_micros": (1, 20_000_000), "per_run_extra_micros": (1, 20_000_000)}
    for key, (low, high) in limits.items():
        if type(data[key]) is not int or not low <= data[key] <= high:
            raise SpotError(f"{key} must be an integer in {low}..{high}")
    if data["job_timeout_seconds"] + 900 > data["vm_lifetime_seconds"]:
        raise SpotError("reserve at least 15 minutes beyond the job timeout for boot/upload/cleanup")
    if data["daily_limit_micros"] > data["total_limit_micros"]:
        raise SpotError("daily allowance cannot exceed the total trial allowance")
    region = data["zone"].rsplit("-", 1)[0]
    if not data["subnetwork"].startswith(f"projects/{data['project']}/regions/{region}/subnetworks/"):
        raise SpotError("subnetwork must be in the dedicated project and selected region")
    if not data["image"].startswith(f"projects/{data['project']}/global/images/"):
        raise SpotError("prepared image must belong to the dedicated project")
    for key in ("pricing_valid_until", "trial_acknowledged_until"):
        timestamp(data[key])
    return data


def admission(config, now):
    if not config["enabled"]:
        raise SpotError("Spot provisioning is disabled; owner configuration is required")
    if config["controller_hostname"] != platform.node():
        raise SpotError("cloud provisioning must run on the one designated controller host")
    for key in ("pricing_valid_until", "trial_acknowledged_until"):
        until = timestamp(config[key])
        if not now + config["vm_lifetime_seconds"] <= until <= now + 7 * 86400:
            raise SpotError(key + " must cover this lease and be reviewed within seven days")
    # Full-lifetime, rounded-up reservation, including unsuccessful attempts.
    return (config["hourly_ceiling_micros"] * config["vm_lifetime_seconds"] + 3599) // 3600 + config["per_run_extra_micros"]


def sync_directory(path):
    directory = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def write_json(path, value):
    temporary = path.with_suffix(".new")
    with temporary.open("w") as output:
        os.chmod(temporary, 0o600)
        json.dump(value, output, indent=2, sort_keys=True)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    sync_directory(path.parent)


class Ledger:
    """One user/controller-wide journal, independent of clone or worktree."""
    def __init__(self, root=STATE_ROOT):
        self.root = Path(root)

    @contextmanager
    def locked(self):
        if self.root.is_symlink():
            raise SpotError("Spot state directory must not be a symlink")
        missing, parent = [], self.root
        while not parent.exists():
            missing.append(parent)
            parent = parent.parent
        for directory in reversed(missing):
            directory.mkdir(exist_ok=True, mode=0o700)
            sync_directory(directory.parent)
        if self.root.stat().st_uid != os.getuid() or self.root.stat().st_mode & 0o022:
            raise SpotError("Spot state directory must be owner-controlled and not group/world-writable")
        with (self.root / "lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            path, marker = self.root / "leases.json", self.root / "pool-id"
            if path.exists():
                state = json.loads(path.read_text())
                if not marker.is_file() or state.get("pool") != marker.read_text():
                    raise SpotError("Spot journal identity mismatch; investigate, do not reset it")
                if state.get("schema") != 1 or not isinstance(state.get("leases"), list):
                    raise SpotError("invalid Spot journal")
                observation = state.get("cost_observation")
                if observation is not None:
                    if (not isinstance(observation, dict) or
                            set(observation) != {"total_micros", "daily_micros", "infrastructure_reserve_micros", "day", "queried_at"} or
                            any(type(observation.get(key)) is not int or observation[key] < 0 for key in
                                ("total_micros", "daily_micros", "infrastructure_reserve_micros")) or
                            observation["daily_micros"] > observation["total_micros"] or
                            not isinstance(observation.get("day"), str) or
                            not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", observation["day"])):
                        raise SpotError("invalid persistent billing observation")
                    timestamp(observation["queried_at"])
                if state["leases"] and (not isinstance(state.get("identity"), dict) or
                                        set(state["identity"]) != {"project", "billing_account", "currency", "account"}):
                    raise SpotError("Spot journal lost its billing identity")
                for lease in state["leases"]:
                    if not isinstance(lease, dict) or not re.fullmatch(r"run-[0-9a-f]{32}", lease.get("id", "")):
                        raise SpotError("invalid Spot lease identity")
                    if lease.get("instance") != "gti-spot-" + lease["id"][4:] or type(lease.get("cost_micros")) is not int or lease["cost_micros"] <= 0:
                        raise SpotError("invalid Spot lease accounting")
                    if (type(lease.get("creation_confirmed")) is not bool or
                            lease.get("state") not in {"reserved", "creating", "booting", "running", "deleting", "deleted", "cleanup-required"} or
                            lease.get("pool") != state["pool"]):
                        raise SpotError("invalid Spot lease lifecycle")
                    timestamp(lease.get("created"))
                    timestamp(lease.get("expires"))
                    if any(lease.get(key) != state["identity"][key] for key in ("project", "account")):
                        raise SpotError("Spot lease does not match its controller identity")
            else:
                if marker.exists():
                    raise SpotError("Spot journal is missing; never reset an unresolved cost ledger")
                pool = uuid.uuid4().hex
                marker.write_text(pool)
                state = {"schema": 1, "pool": pool, "leases": []}
                write_json(path, state)
            yield state
            write_json(path, state)

    def reserve(self, config, run_id, now):
        amount = admission(config, now)
        with self.locked() as state:
            identity = {key: config[key] for key in ("project", "billing_account", "currency", "account")}
            if state.get("identity", identity) != identity:
                raise SpotError("controller journal is already bound to another project/account/currency")
            if not re.fullmatch(r"run-[0-9a-f]{32}", run_id) or any(lease["id"] == run_id for lease in state["leases"]):
                raise SpotError("invalid or duplicate Spot run identity")
            if any(lease.get("state") != "deleted" for lease in state["leases"]):
                raise SpotError("the one Spot slot is occupied or unresolved; inspect status/reap")
            if len(state["leases"]) >= 8:
                raise SpotError("eight-attempt pilot cap reached; owner review is required")
            observation = state.get("cost_observation", {})
            infrastructure = observation.get("infrastructure_reserve_micros", 0)
            if max(sum(lease["cost_micros"] for lease in state["leases"]), observation.get("total_micros", 0)) + infrastructure + amount > config["total_limit_micros"]:
                raise SpotError("total Spot trial allowance exhausted")
            day = utc(now)[:10]
            if sum(lease["created"][:10] == day for lease in state["leases"]) >= 2:
                raise SpotError("two-attempt daily pilot cap reached")
            daily = sum(lease["cost_micros"] for lease in state["leases"] if lease["created"][:10] == day)
            observed_daily = observation.get("daily_micros", 0) if observation.get("day") == day else 0
            if max(daily, observed_daily) + infrastructure + amount > config["daily_limit_micros"]:
                raise SpotError("daily Spot allowance exhausted")
            lease = {"id": run_id, "instance": "gti-spot-" + run_id[4:], "pool": state["pool"],
                     "project": config["project"], "zone": config["zone"], "account": config["account"],
                     "image": config["image"], "created": utc(now), "expires": utc(now + config["vm_lifetime_seconds"]),
                     "cost_micros": amount, "state": "reserved", "creation_confirmed": False}
            state["identity"] = identity
            state["leases"].append(lease)
            return dict(lease)

    def observe_cost(self, config, lease, total, daily, infrastructure, now):
        if any(type(value) is not int or value < 0 for value in (total, daily, infrastructure)) or daily > total:
            raise SpotError("invalid billing observation")
        exceeded = False
        with self.locked() as state:
            if state.get("identity") != {key: config[key] for key in ("project", "billing_account", "currency", "account")}:
                raise SpotError("billing observation does not match the journal identity")
            previous = state.get("cost_observation", {})
            current = next((item for item in state["leases"] if item["id"] == lease["id"]), None)
            if current is None or current["cost_micros"] != lease["cost_micros"]:
                raise SpotError("billing observation has no matching reserved lease")
            day = utc(now)[:10]
            observation = {"total_micros": max(total, previous.get("total_micros", 0)),
                           "daily_micros": max(daily, previous.get("daily_micros", 0) if previous.get("day") == day else 0),
                           "infrastructure_reserve_micros": max(infrastructure, previous.get("infrastructure_reserve_micros", 0)),
                           "day": day, "queried_at": utc(now)}
            state["cost_observation"] = observation
            prior = [item for item in state["leases"] if item["id"] != lease["id"]]
            overhead = observation["infrastructure_reserve_micros"] + current["cost_micros"]
            exceeded = (max(sum(item["cost_micros"] for item in prior), observation["total_micros"]) + overhead > config["total_limit_micros"] or
                        max(sum(item["cost_micros"] for item in prior if item["created"][:10] == day), observation["daily_micros"]) + overhead > config["daily_limit_micros"])
        # Persist new high-water marks even when this admission is rejected.
        if exceeded:
            raise SpotError("observed gross project charges plus reservations/infrastructure exceed the allowance")

    def update(self, lease, **changes):
        with self.locked() as state:
            found = next(item for item in state["leases"] if item["id"] == lease["id"])
            found.update(changes)
            lease.update(changes)

    def status(self):
        with self.locked() as state:
            return json.loads(json.dumps(state))


class Google:
    def __init__(self, config):
        self.config = config

    def command(self, *args, timeout=90):
        try:
            result = subprocess.run(["gcloud", "--quiet", "--account=" + self.config["account"], *args,
                                     "--project=" + self.config["project"], "--format=json"],
                                    capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise SpotError("Google API command timed out; its outcome may be unknown") from error
        if result.returncode:
            raise SpotError("Google API command failed: " + " ".join(args[:3]) + ": " + result.stderr[-2000:])
        if not result.stdout.strip():
            if args[:3] in (("compute", "instances", "delete"), ("compute", "disks", "delete")):
                return None
            raise SpotError("Google API returned no structured result; this is not proof of absence")
        return json.loads(result.stdout)

    def preflight(self):
        billing = self.command("billing", "projects", "describe", self.config["project"])
        if billing.get("billingAccountName") != "billingAccounts/" + self.config["billing_account"] or billing.get("billingEnabled") is not True:
            raise SpotError("dedicated project is not linked to the approved billing account")
        account = self.command("billing", "accounts", "describe", self.config["billing_account"])
        if account.get("open") is not True or account.get("currencyCode") != self.config["currency"]:
            raise SpotError("billing account is closed or its currency differs from the reviewed policy")
        # BillingAccount has no reliable Free Trial flag. The fresh owner
        # acknowledgement is required separately, never inferred from credits.
        subnet = self.config["subnetwork"].split("/")
        network = self.command("compute", "networks", "subnets", "describe", subnet[-1], "--region=" + subnet[3])
        if network.get("privateIpGoogleAccess") is not True:
            raise SpotError("the private subnet must already have Private Google Access")
        image = self.command("compute", "images", "describe", self.config["image"].split("/")[-1])
        if image.get("status") != "READY" or image.get("architecture") != "X86_64":
            raise SpotError("the prepared image must be READY and X86_64")
        if self.instances():
            raise SpotError("dedicated Spot project contains a VM; reconcile it before provisioning")
        if self.disks():
            raise SpotError("dedicated Spot project contains a persistent disk; reconcile it before provisioning")

    def instances(self):
        return self.resources("instances")

    def disks(self):
        return self.resources("disks")

    def resources(self, kind):
        data = self.command("compute", kind, "list")
        if not isinstance(data, list) or any(not isinstance(item, dict) for item in data):
            raise SpotError("Google resource inventory is not a structured list; absence is unproven")
        return data

    def create(self, lease):
        c = self.config
        self.command("compute", "instances", "create", lease["instance"], "--zone=" + c["zone"],
                     "--machine-type=" + c["machine_type"], "--image=" + c["image"],
                     "--subnet=" + c["subnetwork"], "--no-address", "--no-service-account", "--no-scopes",
                     "--provisioning-model=SPOT", "--instance-termination-action=DELETE",
                     "--max-run-duration=" + str(c["vm_lifetime_seconds"]) + "s", "--no-restart-on-failure",
                     "--maintenance-policy=TERMINATE", "--no-deletion-protection",
                     "--boot-disk-auto-delete", "--boot-disk-size=" + str(c["boot_disk_gib"]) + "GB",
                     "--boot-disk-type=pd-standard",
                     "--labels=gti-runner=spot-v1,gti-pool=" + lease["pool"] + ",gti-run=" + lease["id"][4:],
                     "--metadata=enable-oslogin=TRUE,enable-guest-attributes=TRUE,block-project-ssh-keys=TRUE")

    @staticmethod
    def owned(instance, lease):
        labels = instance.get("labels", {})
        return (instance.get("name") == lease["instance"] and
                labels.get("gti-pool") == lease["pool"] and labels.get("gti-run") == lease["id"][4:] and
                labels.get("gti-runner") == "spot-v1" and instance.get("zone", "").endswith("/zones/" + lease["zone"]))

    def verify(self, lease):
        instance = next((i for i in self.instances() if i.get("name") == lease["instance"]), {})
        schedule = instance.get("scheduling", {})
        disks = instance.get("disks", [])
        interfaces = instance.get("networkInterfaces", [])
        if (not self.owned(instance, lease) or schedule.get("provisioningModel") != "SPOT" or
                schedule.get("instanceTerminationAction") != "DELETE" or schedule.get("automaticRestart") is not False or
                schedule.get("maxRunDuration", {}).get("seconds") not in (self.config["vm_lifetime_seconds"], str(self.config["vm_lifetime_seconds"])) or
                schedule.get("maxRunDuration", {}).get("nanos", 0) != 0 or instance.get("serviceAccounts") or
                not instance.get("machineType", "").endswith("/machineTypes/" + self.config["machine_type"]) or
                instance.get("deletionProtection", False) is not False or instance.get("guestAccelerators") or
                len(disks) != 1 or disks[0].get("boot") is not True or disks[0].get("autoDelete") is not True or
                not disks[0].get("source", "").endswith("/disks/" + lease["instance"]) or
                not str(disks[0].get("diskSizeGb", "")).isdigit() or
                int(disks[0]["diskSizeGb"]) > self.config["boot_disk_gib"] or
                len(interfaces) != 1 or interfaces[0].get("accessConfigs") or interfaces[0].get("ipv6AccessConfigs") or
                not interfaces[0].get("subnetwork", "").endswith("/" + self.config["subnetwork"])):
            raise SpotError("created VM does not satisfy the approved Spot/expiry/private/disk contract")
        return instance

    def host_keys(self, lease, instance, destination):
        data = self.command("compute", "instances", "get-guest-attributes", lease["instance"],
                            "--zone=" + lease["zone"], "--query-path=hostkeys/")
        identifier = str(instance.get("id", ""))
        if not identifier.isdigit():
            raise SpotError("VM has no numeric provider identity")
        lines = []
        items = data if isinstance(data, list) else data.get("queryValue", {}).get("items", [])
        for key in items:
            if key.get("key") in ("ssh-ed25519", "ssh-rsa", "ecdsa-sha2-nistp256") and re.fullmatch(r"[A-Za-z0-9+/]+=*", key.get("value", "")):
                lines.append(f"compute.{identifier} {key['key']} {key['value']}\n")
        if not lines:
            raise SpotError("trusted guest host keys are unavailable; refusing unverified SSH")
        destination.write_text("".join(lines))
        os.chmod(destination, 0o600)

    def delete(self, lease):
        instance = next((i for i in self.instances() if i.get("name") == lease["instance"]), None)
        if instance:
            if not self.owned(instance, lease):
                raise SpotError("refusing to delete a VM without this lease's exact ownership labels")
            self.command("compute", "instances", "delete", lease["instance"], "--zone=" + lease["zone"], "--delete-disks=boot")
        for disk in self.disks():
            if disk.get("name") != lease["instance"] or not disk.get("zone", "").endswith("/zones/" + lease["zone"]):
                continue
            if (disk.get("users") or not disk.get("sourceImage", "").endswith("/" + lease["image"]) or
                    provider_timestamp(disk.get("creationTimestamp")) < timestamp(lease["created"])):
                raise SpotError("orphan disk ownership is uncertain; manual review required")
            self.command("compute", "disks", "delete", lease["instance"], "--zone=" + lease["zone"])
        if any(i.get("name") == lease["instance"] for i in self.instances()) or any(d.get("name") == lease["instance"] for d in self.disks()):
            raise SpotError("Google has not confirmed VM and boot-disk deletion")


def cleanup(provider, ledger, lease):
    ledger.update(lease, state="deleting")
    try:
        provider.delete(lease)
        if not lease["creation_confirmed"]:
            # A lost insert response can hide an in-flight creation. Absence
            # alone is not proof that it cannot appear later; quarantine.
            raise SpotError("creation outcome is uncertain; slot quarantined until operator reconciliation")
        ledger.update(lease, state="deleted", deleted=utc(time.time()))
    except BaseException:
        ledger.update(lease, state="cleanup-required")
        raise


def run(args, runner, config, ledger, provider, recovery=None):
    if args.detach or args.full_suite or (not args.target and not args.steps_file):
        raise SpotError("phase one requires a foreground run with explicit targets/steps; no full suite or detached mode")
    if args.platform not in (None, "Linux") or any(getattr(args, key) for key in ("host", "root", "python", "transport", "cc", "cxx")):
        raise SpotError("Spot is a fixed Linux/IAP profile; connection overrides are not allowed")
    args.timeout = config["job_timeout_seconds"] if args.timeout is None else args.timeout
    cpus = int(config["machine_type"].rsplit("-", 1)[1])
    if not 1 <= args.timeout <= config["job_timeout_seconds"] or any(v is not None and not 1 <= v <= cpus for v in (args.jobs, args.test_jobs)):
        raise SpotError("requested timeout or parallelism exceeds the Spot policy")
    admission(config, time.time())
    provider.preflight()
    if recovery is not None:
        recovery.preflight(ledger)
    run_id = "run-" + uuid.uuid4().hex
    host_file = ledger.root / (run_id + "-known-hosts")
    connection = {"host": "gti-spot-" + run_id[4:], "root": "/tmp/gti-remote-runner", "python": "/usr/bin/python3",
                  "transport": "gcloud-iap", "platform": "Linux", "cpus": cpus, "jobs": min(3, cpus), "test_jobs": min(6, cpus),
                  "cc": "/usr/bin/gcc-14", "cxx": "/usr/bin/g++-14", "gcp_project": config["project"],
                  "gcp_zone": config["zone"], "gcp_account": config["account"], "known_hosts": str(host_file)}
    lease = None

    def provision(identity):
        nonlocal lease
        lease = ledger.reserve(config, run_id, time.time())
        print(f"Spot lease {run_id}: reserved {lease['cost_micros']} micro-{config['currency']}; expires {lease['expires']}", flush=True)
        ledger.update(lease, state="creating")
        ledger.update(lease, source={key: identity[key] for key in ("head", "snapshot_sha256")})
        uploads = recovery.prepare(ledger, lease) if recovery is not None else None
        provider.create(lease)
        ledger.update(lease, creation_confirmed=True, state="booting")
        instance = provider.verify(lease)
        deadline = min(time.monotonic() + 300, time.monotonic() + config["vm_lifetime_seconds"] - args.timeout - 300)
        while True:
            try:
                provider.host_keys(lease, instance, host_file)
                observed = runner.connection(connection, "status")
                if observed.get("platform") != "Linux" or observed.get("machine") not in ("x86_64", "amd64"):
                    raise SpotError("cloud worker is not the required Linux x64 profile")
                break
            except (SpotError, runner.RunnerError, OSError):
                if time.monotonic() >= deadline:
                    raise SpotError("private worker bootstrap/host-key check timed out")
                time.sleep(10)
        ledger.update(lease, state="running")
        return uploads

    def terminate(*_):
        raise KeyboardInterrupt()

    old_handler = signal.signal(signal.SIGTERM, terminate)
    try:
        return runner.execute_run(args, runner.local_directory(args.repo), "gcp-spot", connection, run_id, provision)
    finally:
        try:
            if lease is not None:
                cleanup(provider, ledger, lease)
        finally:
            signal.signal(signal.SIGTERM, old_handler)


def dispatch(args, runner):
    config = policy(args.spot_policy or DEFAULT_POLICY)
    ledger, provider = Ledger(), Google(config)
    if args.command == "run":
        from gcp_spot_recovery import DEFAULT_RECOVERY, Recovery, configuration
        recovery = Recovery(config, configuration(args.spot_recovery or DEFAULT_RECOVERY, config))
        return run(args, runner, config, ledger, provider, recovery)
    if args.command == "status":
        if args.all:
            raise SpotError("Spot status uses its one controller journal; omit --all")
        print(json.dumps(ledger.status(), indent=2))
        return 0
    if args.command == "clean":
        if args.run_id and not re.fullmatch(r"run-[0-9a-f]{32}", args.run_id):
            raise SpotError("invalid Spot run identity")
        for lease in ledger.status()["leases"]:
            if lease["state"] == "deleted" or (args.run_id and lease["id"] != args.run_id):
                continue
            if not args.run_id and timestamp(lease["expires"]) > time.time():
                continue
            if any(config[key] != lease[key] for key in ("project", "zone", "account")):
                raise SpotError("cleanup policy differs from the lease; use its original project/zone/account")
            cleanup(provider, ledger, lease)
        return 0
    raise SpotError("Spot supports run, status and clean only; existing local hosts are unchanged")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("command", choices=["plan"])
    args = parser.parse_args()
    config = policy(args.policy)
    print(json.dumps({"enabled": config["enabled"], "project": config["project"], "zone": config["zone"],
                      "machine_type": config["machine_type"], "max_instances": 1,
                      "reservation_micros": (config["hourly_ceiling_micros"] * config["vm_lifetime_seconds"] + 3599) // 3600 + config["per_run_extra_micros"],
                      "currency": config["currency"], "vm_lifetime_seconds": config["vm_lifetime_seconds"],
                      "note": "Offline plan only. No Google API calls or provisioning; prices and Free Trial status require owner review."}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print(f"gcp-spot: {error}", file=sys.stderr)
        sys.exit(1)

#!/usr/bin/env python3
"""Controller-owned cloud leases, billing observations and upload capabilities."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from gcp_spot_checkpoint import CHECKPOINT_BYTES, FINAL_BYTES, INTERVAL, validate_plan
from gcp_spot_cloud import Cloud
from gcp_spot_runner import Ledger, SpotError, policy, timestamp, utc

DEFAULT_RECOVERY = Path.home() / ".config/gti/spot-recovery.json"
FIELDS = {"schema", "enabled", "bucket", "signer_service_account", "reaper_job", "scheduler_job",
          "pilot_expires_at", "billing_table", "billing_maximum_bytes_billed", "infrastructure_reserve_micros", "janitor_image"}


def configuration(path, config):
    data = json.loads(Path(path).read_text())
    if (not isinstance(data, dict) or set(data) != FIELDS or type(data["schema"]) is not int or
            data["schema"] != 1 or type(data["enabled"]) is not bool):
        raise SpotError("recovery policy must use schema 1 and exactly the documented fields")
    region = config["zone"].rsplit("-", 1)[0]
    job = f"projects/{config['project']}/locations/{region}/jobs/gti-spot-reaper"
    if data["reaper_job"] != job or data["scheduler_job"] != job:
        raise SpotError("recovery jobs must use the dedicated project/region and fixed name")
    if not isinstance(data["bucket"], str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]{2,62}", data["bucket"]):
        raise SpotError("invalid private recovery bucket")
    if data["signer_service_account"] != f"gti-spot-upload@{config['project']}.iam.gserviceaccount.com":
        raise SpotError("upload signer must be the dedicated project upload identity")
    if not isinstance(data["janitor_image"], str) or not re.fullmatch(r"[a-z0-9.-]+/[a-zA-Z0-9_./-]+@sha256:[a-f0-9]{64}", data["janitor_image"]):
        raise SpotError("janitor image must be reviewed and pinned by digest")
    if not isinstance(data["billing_table"], str) or not re.fullmatch(r"[a-z][a-z0-9-]{4,61}[a-z0-9]\.[A-Za-z_][A-Za-z0-9_]{0,100}\.[A-Za-z_][A-Za-z0-9_]{0,100}", data["billing_table"]):
        raise SpotError("billing_table must be an exact project.dataset.table")
    for key, low, high in (("billing_maximum_bytes_billed", 10 * 1024 * 1024, 100 * 1024 * 1024),
                           ("infrastructure_reserve_micros", 1, 5_000_000)):
        if type(data[key]) is not int or not low <= data[key] <= high:
            raise SpotError("invalid recovery allowance: " + key)
    timestamp(data["pilot_expires_at"])
    return data


def prefix(pool):
    if not isinstance(pool, str) or not re.fullmatch(r"[0-9a-f]{32}", pool):
        raise SpotError("invalid cloud pool identity")
    return "gti-spot/v2/" + pool + "/"


def registration(config, recovery, pool, started):
    return {"schema": 1, "pool": pool, "project": config["project"], "zone": config["zone"],
            "account": config["account"], "billing_account": config["billing_account"], "currency": config["currency"],
            "started": started, "pilot_expires_at": recovery["pilot_expires_at"]}


def validate_lease(value, registry):
    if not isinstance(value, dict):
        raise SpotError("invalid cloud lease")
    if (not re.fullmatch(r"run-[0-9a-f]{32}", value.get("id", "")) or
            value.get("instance") != "gti-spot-" + value["id"][4:] or
            any(value.get(key) != registry[key] for key in ("pool", "project", "zone", "account")) or
            not isinstance(value.get("image"), str) or
            not re.fullmatch(r"projects/" + re.escape(registry["project"]) + r"/global/images/[a-z][a-z0-9-]+", value["image"])):
        raise SpotError("cloud lease has foreign or malformed resource ownership")
    source = value.get("source", {})
    if (not isinstance(source, dict) or set(source) != {"head", "snapshot_sha256"} or
            not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", source.get("head", "")) or
            not re.fullmatch(r"[0-9a-f]{64}", source.get("snapshot_sha256", ""))):
        raise SpotError("cloud lease lost its exact source identity")
    created, expires = timestamp(value.get("created")), timestamp(value.get("expires"))
    if (not timestamp(registry["started"]) <= created < expires <= timestamp(registry["pilot_expires_at"]) or
            not 1200 <= expires - created <= 5400):
        raise SpotError("cloud lease has an invalid lifetime")
    return value


def recover(config, settings, run_id, snapshot_hash, destination, partial=False, cloud=None):
    import remote_runner as runner
    if not re.fullmatch(r"run-[0-9a-f]{32}", run_id) or not re.fullmatch(r"[0-9a-f]{64}", snapshot_hash):
        raise SpotError("recovery requires exact run and snapshot identities")
    cloud = cloud or Cloud.controller(config["account"])
    registry = cloud.object(settings["bucket"], "gti-spot/v2/registry.json")
    expected = registration(config, settings, registry.get("pool"), registry.get("started"))
    if registry != expected:
        raise SpotError("foreign cloud pool registration")
    base = prefix(registry["pool"])
    lease = validate_lease(cloud.object(settings["bucket"], base + "leases/" + run_id + ".json"), registry)
    if lease["source"]["snapshot_sha256"] != snapshot_hash:
        raise SpotError("recovery snapshot does not match the requested source")
    directory = base + "checkpoints/" + run_id + "/"
    name = directory + "final.tar.gz"
    if partial:
        values = cloud.objects(settings["bucket"], directory, maximum=32)
        names = [item.get("name", "") for item in values]
        names = [item for item in names if re.fullmatch(re.escape(directory) + r"[0-9]{2}\.tar\.gz", item)]
        if not names:
            raise SpotError("no partial checkpoint was uploaded")
        name = sorted(names)[-1]
    destination = Path(destination).absolute()
    if destination.exists() or destination.is_symlink():
        raise SpotError("recovery output must not already exist")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gti-spot-recover-", dir=destination.parent) as temporary:
        archive = Path(temporary) / "checkpoint.tar.gz"
        result = subprocess.run(["gcloud", "storage", "cp", "--quiet", "--account=" + config["account"],
                                 "--project=" + config["project"], "gs://" + settings["bucket"] + "/" + name, str(archive)],
                                capture_output=True, text=True, timeout=120)
        if result.returncode or not archive.is_file() or archive.stat().st_size != (CHECKPOINT_BYTES if partial else FINAL_BYTES):
            raise SpotError("durable result download failed or has an invalid byte allowance")
        output = Path(temporary) / "results"
        output.mkdir()
        runner.extract(archive, output, runner.MAX_RESULTS + 2 * 1024 * 1024)
        report = json.loads((output / ("checkpoint.json" if partial else "report.json")).read_text())
        if (report.get("id") != run_id or any(report.get("source", {}).get(key) != value for key, value in lease["source"].items()) or
                (not partial and report.get("state") not in runner.TERMINAL) or
                (partial and report.get("partial") is not True)):
            raise SpotError("durable result identity/state mismatch")
        # Claim the output name exclusively; never replace even an empty
        # directory created by another command after the initial check.
        destination.mkdir(mode=0o700)
        for path in output.iterdir():
            path.rename(destination / path.name)
    return {"id": run_id, "snapshot_sha256": snapshot_hash, "partial": partial, "output": str(destination)}


class Recovery:
    def __init__(self, config, settings, cloud=None):
        self.config, self.settings = config, settings
        self.cloud = cloud or Cloud.controller(config["account"])
        self.registry = None

    def preflight(self, ledger):
        now = time.time()
        settings, config = self.settings, self.config
        until = timestamp(settings["pilot_expires_at"])
        if not settings["enabled"] or not now + config["vm_lifetime_seconds"] <= until <= now + 7 * 86400:
            raise SpotError("recovery must be enabled for a reviewed pilot ending within seven days")
        state = ledger.status()
        self.pool = state["pool"]
        self.base = prefix(self.pool)
        bucket = self.cloud.request("GET", "storage", "/storage/v1/b/" + settings["bucket"])
        project = self.cloud.request("GET", "cloudresourcemanager", "/v1/projects/" + config["project"])
        if (not project.get("projectNumber") or bucket.get("projectNumber") != project["projectNumber"] or
                bucket.get("iamConfiguration", {}).get("publicAccessPrevention") != "enforced" or
                bucket.get("iamConfiguration", {}).get("uniformBucketLevelAccess", {}).get("enabled") is not True or
                bucket.get("location", "").lower() != config["zone"].rsplit("-", 1)[0] or
                bucket.get("storageClass") != "STANDARD" or bucket.get("autoclass", {}).get("enabled", False) or
                bucket.get("versioning", {}).get("enabled", False) or bucket.get("softDeletePolicy", {}).get("retentionDurationSeconds") not in (0, "0")):
            raise SpotError("recovery bucket must be private, uniform, unversioned and without soft-delete retention")
        lifecycle = bucket.get("lifecycle", {}).get("rule", [])
        if lifecycle != [{"action": {"type": "Delete"}, "condition": {"age": 1, "matchesPrefix": [self.base + "checkpoints/"]}}]:
            raise SpotError("recovery bucket must expire checkpoint objects after one day, never leases")
        registry = self.cloud.object(settings["bucket"], "gti-spot/v2/registry.json")
        if not isinstance(registry, dict) or registry != registration(config, settings, self.pool, registry.get("started")):
            raise SpotError("cloud registry differs from the local controller; do not reset either journal")
        if not now - 7 * 86400 <= timestamp(registry["started"]) <= now:
            raise SpotError("cloud registry is outside the seven-day pilot")
        self.registry = registry
        job = self.cloud.request("GET", "run", "/v2/" + settings["reaper_job"])
        scheduler = self.cloud.request("GET", "cloudscheduler", "/v1/" + settings["scheduler_job"])
        expected_sa = f"gti-spot-reaper@{config['project']}.iam.gserviceaccount.com"
        template = job.get("template", {})
        task = template.get("template", {})
        expected_args = ["--project", config["project"], "--bucket", settings["bucket"], "--pool", self.pool,
                         "--expires", settings["pilot_expires_at"], "--scheduler", settings["scheduler_job"]]
        containers = task.get("containers", [])
        if (template.get("taskCount") != 1 or template.get("parallelism") != 1 or
                task.get("maxRetries") != 0 or task.get("timeout") != "60s" or task.get("serviceAccount") != expected_sa or
                len(containers) != 1 or containers[0].get("image") != settings["janitor_image"] or
                containers[0].get("args") != expected_args or
                containers[0].get("resources", {}).get("limits", {}).get("cpu") not in ("1", "1000m") or
                containers[0].get("resources", {}).get("limits", {}).get("memory") != "512Mi" or
                scheduler.get("state") != "ENABLED" or scheduler.get("schedule") != "*/10 * * * *" or
                scheduler.get("httpTarget", {}).get("uri") != "https://run.googleapis.com/v2/" + settings["reaper_job"] + ":run" or
                scheduler.get("httpTarget", {}).get("oauthToken", {}).get("serviceAccountEmail") != f"gti-spot-scheduler@{config['project']}.iam.gserviceaccount.com"):
            raise SpotError("independent cleanup job/schedule does not match the approved control plane")
        health = self.cloud.object(settings["bucket"], self.base + "health.json")
        if (not isinstance(health, dict) or health.get("registry") != registry or health.get("healthy") is not True or
                not now - 1200 <= timestamp(health.get("at")) <= now + 60):
            raise SpotError("independent cleanup has no recent healthy heartbeat; no VM will be created")

    def observe_cost(self, ledger, lease):
        """Query gross positive project charges, never subtract credits or reservations."""
        table = self.settings["billing_table"]
        query = f"""SELECT currency, CAST(CEIL(SUM(GREATEST(cost, 0)) * 1000000) AS STRING) AS total,
 CAST(CEIL(SUM(IF(DATE(usage_start_time, 'UTC') = CURRENT_DATE('UTC'), GREATEST(cost, 0), 0)) * 1000000) AS STRING) AS daily
 FROM `{table}` WHERE project.id = @project AND usage_start_time >= TIMESTAMP(@started)
 GROUP BY currency"""
        parameters = [{"name": name, "parameterType": {"type": "STRING"}, "parameterValue": {"value": value}}
                      for name, value in (("project", self.config["project"]), ("started", self.registry["started"]))]
        result = self.cloud.request("POST", "bigquery", "/bigquery/v2/projects/" + self.config["project"] + "/queries", {
            "query": query, "useLegacySql": False, "parameterMode": "NAMED", "queryParameters": parameters,
            "maximumBytesBilled": str(self.settings["billing_maximum_bytes_billed"]),
            "timeoutMs": 10000, "requestId": lease["id"][4:]})
        if result.get("jobComplete") is not True or result.get("errors") or result.get("pageToken"):
            raise SpotError("bounded billing query did not complete; no VM will be created")
        schema = result.get("schema", {}).get("fields", [])
        if [field.get("name") for field in schema] != ["currency", "total", "daily"]:
            raise SpotError("billing export response has an unexpected schema")
        rows = result.get("rows", [])
        if not isinstance(rows, list) or len(rows) > 1:
            raise SpotError("billing export contains multiple currencies")
        total = daily = 0
        if rows:
            fields = rows[0].get("f", [])
            if len(fields) != 3 or fields[0].get("v") != self.config["currency"]:
                raise SpotError("billing export currency differs from policy")
            values = [field.get("v") for field in fields[1:]]
            if any(not isinstance(value, str) or not re.fullmatch(r"[0-9]{1,18}", value) for value in values):
                raise SpotError("invalid billing amount")
            total, daily = map(int, values)
        ledger.observe_cost(self.config, lease, total, daily, self.settings["infrastructure_reserve_micros"], time.time())

    def prepare(self, ledger, lease):
        validate_lease(lease, self.registry)
        self.cloud.create(self.settings["bucket"], self.base + "leases/" + lease["id"] + ".json", lease)
        self.observe_cost(ledger, lease)
        count = math.ceil(self.config["job_timeout_seconds"] / INTERVAL) + 1
        plan = {"schema": 1, "run": lease["id"],
                "prefix": "/" + self.settings["bucket"] + "/" + self.base + "checkpoints/" + lease["id"] + "/", "urls": []}
        for index in range(count + 1):
            final = index == count
            name = ("final" if final else f"{index:02d}") + ".tar.gz"
            size = FINAL_BYTES if final else CHECKPOINT_BYTES
            uri = "gs:/" + plan["prefix"] + name
            result = subprocess.run(["gcloud", "storage", "sign-url", uri, "--quiet",
                                     "--account=" + self.config["account"], "--project=" + self.config["project"],
                                     "--impersonate-service-account=" + self.settings["signer_service_account"],
                                     "--http-verb=PUT", "--duration=" + str(self.config["vm_lifetime_seconds"]) + "s",
                                     f"--headers=content-length={size},content-type=application/octet-stream,x-goog-if-generation-match=0",
                                     "--format=json"], capture_output=True, text=True, timeout=35)
            if result.returncode:
                raise SpotError("could not obtain bounded upload capability")
            value = json.loads(result.stdout)
            if not isinstance(value, list) or len(value) != 1 or not isinstance(value[0].get("signed_url"), str):
                raise SpotError("signed URL response has an unexpected shape")
            plan["urls"].append(value[0]["signed_url"])
        validate_plan(plan, lease["id"])
        return {"checkpoint_upload.json": json.dumps(plan).encode()}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--recovery", type=Path, default=DEFAULT_RECOVERY)
    parser.add_argument("command", choices=["plan", "recover"])
    parser.add_argument("--run-id")
    parser.add_argument("--snapshot")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--partial", action="store_true", help="retrieve diagnostic tails only, never verification evidence")
    args = parser.parse_args(argv)
    config = policy(args.policy)
    settings = configuration(args.recovery, config)
    if args.command == "recover":
        if not args.run_id or not args.snapshot or args.output is None:
            parser.error("recover requires --run-id, --snapshot and --output")
        print(json.dumps(recover(config, settings, args.run_id, args.snapshot, args.output, args.partial), indent=2))
        return
    print(json.dumps({"enabled": config["enabled"] and settings["enabled"], "settings": settings,
                      "maximum_upload_bytes": (math.ceil(config["job_timeout_seconds"] / INTERVAL) + 1) * CHECKPOINT_BYTES + FINAL_BYTES,
                      "note": "Offline plan. Terraform apply, image building and all Google calls require owner commissioning."}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print("gcp-spot-recovery: " + str(error), file=sys.stderr)
        sys.exit(1)

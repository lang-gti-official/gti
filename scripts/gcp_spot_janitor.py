#!/usr/bin/env python3
"""Scheduled, non-provisioning cloud cleanup. Does not release controller loans."""
from __future__ import annotations

import argparse
import json
import sys
import time

from gcp_spot_cloud import Cloud
from gcp_spot_recovery import prefix, validate_lease
from gcp_spot_runner import Google, SpotError, timestamp, utc


def reap(cloud, provider, bucket, registry, now):
    base = prefix(registry["pool"])
    objects = cloud.objects(bucket, base + "leases/")
    errors, deleted = [], []
    for item in objects:
        name = item.get("name", "")
        try:
            value = cloud.object(bucket, name)
            lease = validate_lease(value, registry)
            if name != base + "leases/" + lease["id"] + ".json":
                raise SpotError("lease object path does not match its identity")
            # Grace permits controller cleanup; expiry is immutable and never renewed.
            if timestamp(lease["expires"]) + 120 > now:
                continue
            provider.delete(lease)
            deleted.append(lease["id"])
        except Exception:
            # Neither exception text nor arbitrary provider documents enter logs.
            errors.append(name)
    health = {"schema": 1, "registry": registry, "at": utc(now), "healthy": not errors,
              "observed_absent": deleted, "errors": errors}
    cloud.replace(bucket, base + "health.json", health)
    # A receipt is one observation, never permanent evidence that a delayed insert
    # cannot appear later. Every scheduled pass rechecks every immutable lease.
    for run_id in deleted:
        cloud.replace(bucket, base + "receipts/" + run_id + ".json",
                      {"schema": 1, "id": run_id, "absent_at": utc(now), "recheck_required": True})
    return health


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--bucket", required=True)
    parser.add_argument("--pool", required=True)
    parser.add_argument("--expires", required=True)
    parser.add_argument("--scheduler", required=True)
    args = parser.parse_args(argv)
    cloud = Cloud.workload()
    registry = cloud.object(args.bucket, "gti-spot/v2/registry.json")
    if (not isinstance(registry, dict) or registry.get("schema") != 1 or registry.get("project") != args.project or
            registry.get("pool") != args.pool or registry.get("pilot_expires_at") != args.expires or
            args.scheduler != f"projects/{args.project}/locations/{registry['zone'].rsplit('-', 1)[0]}/jobs/gti-spot-reaper"):
        raise SpotError("janitor deployment differs from its immutable pool registration")
    now = time.time()
    expired = now >= timestamp(args.expires) + 120
    provider = Google({"project": args.project, "account": f"gti-spot-reaper@{args.project}.iam.gserviceaccount.com"})
    try:
        health = reap(cloud, provider, args.bucket, registry, now)
        print(json.dumps(health))
        return 0 if health["healthy"] else 1
    finally:
        if expired:
            # Finite pilot: stop recurring requests even when cleanup needs an
            # operator. Provider VM expiry remains independent of this scheduler.
            cloud.request("POST", "cloudscheduler", "/v1/" + args.scheduler + ":pause", {})


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        print("gcp-spot-janitor: recovery failed; operator inspection required", file=sys.stderr)
        sys.exit(1)

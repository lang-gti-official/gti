#!/usr/bin/env python3
"""Render owner-reviewed Terraform inputs offline; never runs apply or login."""
import argparse
import json
from pathlib import Path
import re
import sys
import time

from gcp_spot_recovery import configuration
from gcp_spot_runner import SpotError, policy, timestamp, utc


def render(config, recovery, pool, image, now):
    if not re.fullmatch(r"[a-z0-9.-]+/[a-zA-Z0-9_./-]+@sha256:[a-f0-9]{64}", image):
        raise SpotError("janitor image must be an owner-published immutable digest")
    if not re.fullmatch(r"[0-9a-f]{32}", pool):
        raise SpotError("invalid controller pool")
    if image != recovery["janitor_image"]:
        raise SpotError("janitor image must match the reviewed recovery policy")
    if not now + config["vm_lifetime_seconds"] < timestamp(recovery["pilot_expires_at"]) <= now + 7 * 86400:
        raise SpotError("review a pilot expiry covering the lease and no more than seven days away")
    expected_subnet = f"projects/{config['project']}/regions/{config['zone'].rsplit('-', 1)[0]}/subnetworks/runner-private"
    if config["subnetwork"] != expected_subnet:
        raise SpotError("setup owns only the runner-private subnet")
    return {"project": config["project"], "zone": config["zone"], "bucket": recovery["bucket"], "pool": pool,
            "controller_account": config["account"], "billing_account": config["billing_account"], "currency": config["currency"],
            "started": utc(now), "pilot_expires_at": recovery["pilot_expires_at"],
            "janitor_image": image, "enable_scheduler": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--recovery", type=Path, required=True)
    parser.add_argument("--pool", required=True, help="pool ID from the designated controller's status, never invent a replacement")
    parser.add_argument("--janitor-image", required=True)
    args = parser.parse_args()
    config = policy(args.policy)
    settings = configuration(args.recovery, config)
    print(json.dumps(render(config, settings, args.pool, args.janitor_image, time.time()), indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        print("gcp-spot-setup: " + str(error), file=sys.stderr)
        sys.exit(1)

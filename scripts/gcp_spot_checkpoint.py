#!/usr/bin/env python3
"""Worker-side bounded checkpoints. Upload capabilities never enter result bundles."""
from __future__ import annotations

import io
import json
from pathlib import Path
import re
import tarfile
import threading
import time
from urllib.parse import parse_qs, urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler

CHECKPOINT_BYTES = 2 * 1024 * 1024
FINAL_BYTES = 40 * 1024 * 1024
INTERVAL = 120


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *_args, **_kwargs):
        return None


def validate_plan(plan, run_id):
    if (not isinstance(plan, dict) or set(plan) != {"schema", "run", "prefix", "urls"} or
            type(plan["schema"]) is not int or plan["schema"] != 1 or plan["run"] != run_id or
            not re.fullmatch(r"run-[0-9a-f]{32}", run_id) or
            not isinstance(plan["prefix"], str) or
            not re.fullmatch(r"/[a-z0-9][a-z0-9-]{2,62}/gti-spot/v2/[0-9a-f]{32}/checkpoints/" + run_id + r"/", plan["prefix"]) or
            not isinstance(plan["urls"], list) or not 2 <= len(plan["urls"]) <= 32):
        raise ValueError("invalid checkpoint upload plan")
    for index, url in enumerate(plan["urls"]):
        if not isinstance(url, str):
            raise ValueError("invalid checkpoint URL")
        parsed = urlsplit(url)
        query = parse_qs(parsed.query)
        size = FINAL_BYTES if index == len(plan["urls"]) - 1 else CHECKPOINT_BYTES
        expected = "final" if size == FINAL_BYTES else f"{index:02d}"
        if (parsed.scheme != "https" or parsed.netloc != "storage.googleapis.com" or
                parsed.fragment or parsed.path != plan["prefix"] + expected + ".tar.gz" or
                query.get("X-Goog-Algorithm") != ["GOOG4-RSA-SHA256"] or
                query.get("X-Goog-SignedHeaders") != ["content-length;content-type;host;x-goog-if-generation-match"] or
                not query.get("X-Goog-Signature") or
                not query.get("X-Goog-Expires", [""])[0].isdigit() or
                not 1 <= int(query["X-Goog-Expires"][0]) <= 5400):
            raise ValueError("upload capability is not an exact bounded Storage PUT")
    return plan


def snapshot(run, identity):
    """Only public execution receipts/log tails; never traverse source or input."""
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        def add(name, data):
            item = tarfile.TarInfo(name)
            item.size = len(data)
            item.mode = 0o600
            archive.addfile(item, io.BytesIO(data))
        report = json.dumps({"schema": 1, "id": run.name, "source": identity,
                             "checkpoint_at": time.time(), "partial": True}).encode()
        add("checkpoint.json", report)
        remaining = 1024 * 1024
        paths = [run / "state.json", run / "worker.log"] + sorted((run / "logs").glob("*.log"))[:64]
        for path in paths:
            if path.is_symlink() or not path.is_file():
                continue
            try:
                with path.open("rb") as stream:
                    stream.seek(0, 2)
                    length = min(stream.tell(), 64 * 1024, remaining)
                    stream.seek(-length, 2)
                    data = stream.read(length)
            except FileNotFoundError:
                continue
            add(str(path.relative_to(run)), data)
            remaining -= len(data)
    return output.getvalue()


def put(url, data, size):
    if len(data) > size:
        raise ValueError("checkpoint exceeds its signed byte allowance")
    body = data + b"\0" * (size - len(data))
    request = Request(url, data=body, method="PUT", headers={
        "Content-Length": str(size), "Content-Type": "application/octet-stream",
        "x-goog-if-generation-match": "0"})
    # Do not print exceptions: urllib errors can contain the signed URL.
    with build_opener(NoRedirect()).open(request, timeout=30) as response:
        if response.status not in (200, 201):
            raise ValueError("checkpoint upload was not accepted")


class Checkpoints:
    def __init__(self, plan, run, identity):
        self.plan = validate_plan(plan, run.name)
        self.run, self.identity = run, identity
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.observe, daemon=True)
        self.thread.start()

    def observe(self):
        for url in self.plan["urls"][:-1]:
            if self.stop.is_set():
                return
            try:
                put(url, snapshot(self.run, self.identity), CHECKPOINT_BYTES)
            except Exception:
                pass  # One attempt per slot; final upload failure is not hidden.
            if self.stop.wait(INTERVAL):
                return

    def finish(self, archive):
        self.stop.set()
        self.thread.join(timeout=35)
        if self.thread.is_alive():
            raise ValueError("checkpoint observer did not stop")
        with archive.open("rb") as source:
            data = source.read(FINAL_BYTES + 1)
        try:
            put(self.plan["urls"][-1], data, FINAL_BYTES)
        except Exception:
            raise ValueError("durable final-result upload failed; inspect partial checkpoints") from None

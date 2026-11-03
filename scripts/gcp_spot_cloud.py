#!/usr/bin/env python3
"""Small authenticated REST transport for the Spot recovery control plane."""
from __future__ import annotations

import json
import subprocess
from urllib.error import HTTPError
from urllib.parse import quote, urlencode
from urllib.request import Request, build_opener

from gcp_spot_checkpoint import NoRedirect
from gcp_spot_runner import SpotError

MAX_JSON = 2 * 1024 * 1024


class Cloud:
    def __init__(self, token):
        self.token = token

    @classmethod
    def controller(cls, account):
        def token():
            result = subprocess.run(["gcloud", "auth", "print-access-token", "--account=" + account,
                                     "--quiet"], capture_output=True, text=True, timeout=30)
            if result.returncode or not result.stdout.strip():
                raise SpotError("controller Google authentication failed")
            return result.stdout.strip()
        return cls(token)

    @classmethod
    def workload(cls):
        def token():
            request = Request("http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token",
                              headers={"Metadata-Flavor": "Google"})
            with build_opener(NoRedirect()).open(request, timeout=5) as response:
                value = json.loads(response.read(16384))
            return value["access_token"]
        return cls(token)

    def request(self, method, service, path, value=None, query=None, missing=False):
        if service not in {"storage", "bigquery", "run", "cloudscheduler", "compute", "cloudresourcemanager"} or not path.startswith("/"):
            raise SpotError("unsupported recovery API")
        url = "https://" + service + ".googleapis.com" + path
        if query:
            url += "?" + urlencode(query)
        body = None if value is None else json.dumps(value).encode()
        request = Request(url, data=body, method=method,
                          headers={"Authorization": "Bearer " + self.token(), "Content-Type": "application/json"})
        try:
            with build_opener(NoRedirect()).open(request, timeout=35) as response:
                data = response.read(MAX_JSON + 1)
            if len(data) > MAX_JSON:
                raise SpotError("recovery API result exceeds its limit")
            return json.loads(data) if data else {}
        except HTTPError as error:
            if missing and error.code == 404:
                return None
            raise SpotError(f"{service} recovery API failed (HTTP {error.code}); no absence inferred") from None

    def object(self, bucket, name, missing=False):
        return self.request("GET", "storage", "/storage/v1/b/" + bucket + "/o/" + quote(name, safe=""),
                            query={"alt": "media"}, missing=missing)

    def create(self, bucket, name, value):
        return self.request("POST", "storage", "/upload/storage/v1/b/" + bucket + "/o", value,
                            query={"uploadType": "media", "name": name, "ifGenerationMatch": "0"})

    def objects(self, bucket, prefix, maximum=8):
        result = self.request("GET", "storage", "/storage/v1/b/" + bucket + "/o",
                              query={"prefix": prefix, "maxResults": str(maximum + 1)})
        values = result.get("items", [])
        if (result.get("kind") != "storage#objects" or not isinstance(values, list) or
                len(values) > maximum or result.get("nextPageToken")):
            raise SpotError("recovery inventory exceeds the pilot bound")
        return values

    def replace(self, bucket, name, value):
        metadata = self.request("GET", "storage", "/storage/v1/b/" + bucket + "/o/" + quote(name, safe=""), missing=True)
        generation = "0" if metadata is None else metadata.get("generation", "")
        if not str(generation).isdigit():
            raise SpotError("Storage object has no verified generation")
        return self.request("POST", "storage", "/upload/storage/v1/b/" + bucket + "/o", value,
                            query={"uploadType": "media", "name": name, "ifGenerationMatch": generation})

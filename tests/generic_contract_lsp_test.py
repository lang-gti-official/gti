#!/usr/bin/env python3
"""Contextual pack selection and complete clause facts retain semantic tooling."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    root = pathlib.Path(__file__).resolve().parent
    pack = (root / "coverage/reproducers/a18-function-item-pack-invoke.gti").read_text()
    clause = (root / "coverage/reproducers/a19-trailing-clause-dependent-compound.gti").read_text()
    with tempfile.TemporaryDirectory(prefix="gti-contract-lsp-") as directory:
        uri = (pathlib.Path(directory) / "contract.gti").as_uri()
        session = LspSession(sys.argv[1])
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
            reply = session.receive_until(lambda message: message.get("id") == request_id)
            assert "error" not in reply, reply
            return reply["result"]

        def change(source, version):
            if version == 1:
                session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                    "textDocument": {"uri": uri, "languageId": "gti", "version": version, "text": source}}})
            else:
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}})
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics" and
                message["params"].get("uri") == uri and
                message["params"].get("version") == version)["params"]["diagnostics"]

        try:
            request("initialize", {"capabilities": {
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            assert change(pack, 1) == []
            params = {"textDocument": {"uri": uri},
                      "position": lsp_position(pack, pack.index("ops::add3") + 6)}
            hover = request("textDocument/hover", params)
            assert "add3(int32_t a, int32_t b, int32_t c)" in str(hover), hover
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location == {"uri": uri, "range": {
                "start": lsp_position(pack, pack.index("add3(")),
                "end": lsp_position(pack, pack.index("add3(") + 4)}}, location
            assert change(clause, 2) == []
            params = {"textDocument": {"uri": uri},
                      "position": lsp_position(clause, clause.rindex("read(b)") + 1)}
            hover = request("textDocument/hover", params)
            assert "requires" in str(hover) and "boxed_int" in str(hover), hover
            invalid = clause.replace("std::copyable<T> && ", "")
            failures = change(invalid, 3)
            assert failures and all(item["code"] == "GTI-S2029" for item in failures), failures
            assert all("type parameter" not in str(item) for item in failures), failures
            assert any("Type 'T'" in item["message"] for item in failures), failures
            assert change(clause, 4) == []
            assert change(pack, 5) == []
        finally:
            session.close()


if __name__ == "__main__":
    main()

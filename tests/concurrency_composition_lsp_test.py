#!/usr/bin/env python3
"""Shared jobs use compiler capabilities and versioned editor diagnostics."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """#include <std/functional>
#include <std/memory>
#include <std/mutex>
#include <std/thread>
[[no_transfer]] struct LocalOnly { int operator()() { return 7; } };
[[no_share]] struct State { mut int total = 42; };
int main() {
  std::function<int()> local{LocalOnly()};
  if (local() != 7) return 1;
  auto state = std::make_shared<std::mutex<State>>(State());
  mut std::transfer_function<int()> job{
      [owner = std::move(state)]() -> int {
        auto guard = owner->lock();
        return guard.get().total;
      }};
  /*😀*/ /*replacement*/
  mut std::thread_result<int> worker{};
  auto started = worker.start(std::move(job));
  if (!started) return 2;
  auto result = worker.join();
  return result && result.value() == 42 ? 0 : 3;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-shared-jobs-lsp-") as directory:
        path = pathlib.Path(directory) / "jobs.gti"
        path.write_text(SOURCE)
        uri = path.as_uri()
        session = LspSession(sys.argv[1], ["--execution-profile", "concurrent"])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda message: message.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def errors(version):
            publication = session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)
            return [d for d in publication["params"]["diagnostics"]
                    if d.get("severity") == 1]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": SOURCE}}})
            assert not errors(1)
            position = {"textDocument": {"uri": uri},
                        "position": lsp_position(SOURCE, SOURCE.index("transfer_function") + 2)}
            hover = request("textDocument/hover", position)
            assert "transfer_function" in str(hover), hover
            definition = request("textDocument/definition", position)
            assert definition and "functional.gti" in str(definition), definition

            invalid = SOURCE.replace("/*replacement*/", "job.replace(LocalOnly());")
            change(2, invalid)
            diagnostics = errors(2)
            failure = next((d for d in diagnostics if d.get("code") == "GTI-S2029"), None)
            assert failure is not None, diagnostics
            assert not any(d.get("code") == "GTI-B0001" for d in diagnostics), diagnostics
            start = lsp_position(invalid, invalid.index("job.replace"))
            end = lsp_position(invalid, invalid.index("job.replace") + len("job.replace(LocalOnly());"))
            assert failure["range"]["start"]["line"] == start["line"], failure
            assert start["character"] <= failure["range"]["start"]["character"] <= end["character"], failure

            change(3, SOURCE)
            assert not errors(3)
            change(4, SOURCE.replace("/*replacement*/", "job.replace("))
            assert errors(4), "incomplete call should have a recovered diagnostic"
            change(5, SOURCE)
            assert not errors(5)
        finally:
            session.close()
    print("shared jobs: local targets, transfer contracts, navigation and recovery passed")


if __name__ == "__main__":
    main()

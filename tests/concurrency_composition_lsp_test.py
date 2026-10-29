#!/usr/bin/env python3
"""Shared jobs use compiler capabilities and versioned editor diagnostics."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """import std.functional;
import std.memory;
import std.mutex;
import std.thread;
[[no_transfer]] struct LocalOnly { LocalOnly() = default; int operator()() { return 7; } };
[[no_share]] struct State { State() = default; mut int total = 42; };
class Write {
  mut int& target;
public:
  Write(mut int& target) : target(target) {}
  void operator()() mut { this.target = 7; }
};
int main() {
  mut int first = 0;
  mut int second = 0;
  /*😀*/ [[discard]] std::parallel_invoke(Write(first), Write(second));
  first = 9;
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

            scoped_position = {"textDocument": {"uri": uri}, "position":
                lsp_position(SOURCE, SOURCE.index("parallel_invoke") + 2)}
            hover = request("textDocument/hover", scoped_position)
            assert "parallel_invoke" in str(hover), hover
            definition = request("textDocument/definition", scoped_position)
            assert definition and "thread.gti" in str(definition), definition
            overlap = SOURCE.replace("Write(second)", "Write(first)")
            change(6, overlap)
            diagnostics = errors(6)
            assert any(d.get("code") in ("GTI-S2017", "GTI-S2084")
                       for d in diagnostics), diagnostics
            assert not any(d.get("code") == "GTI-B0001"
                           for d in diagnostics), diagnostics
            change(7, SOURCE)
            assert not errors(7)

            version = 7
            for nested in (False, True):
                receiver = "this.holder.owner" if nested else "this.owner"
                stored = """import std.mutex;
void see(std::mutex<int32_t>& value) {}
"""
                if nested:
                    stored += """class Holder { public:
  std::mutex<int32_t>& owner;
  Holder(std::mutex<int32_t>& owner) : owner(owner) {}
};
class User {
  Holder& holder;
public:
  User(Holder& holder) : holder(holder) {}
"""
                else:
                    stored += """class User {
  std::mutex<int32_t>& owner;
public:
  User(std::mutex<int32_t>& owner) : owner(owner) {}
"""
                publication = "auto guard = " + receiver + ".lock();"
                for second in (False, True):
                    conflict = ("auto second = " + receiver + ".lock();" if second
                                else "see(" + receiver + ");")
                    bad = (stored + "  void use() { /*😀*/ " + publication +
                           " /*😀*/ " + conflict + " }\n};\nint main() { return 0; }\n")
                    version += 1
                    change(version, bad)
                    diagnostics = errors(version)
                    assert len(diagnostics) == 1, diagnostics
                    failure = diagnostics[0]
                    assert failure.get("code") == "GTI-S2017", failure
                    offset = (bad.index("();", bad.index(conflict)) + 1 if second
                              else bad.index("owner", bad.index(conflict)))
                    expected = {"start": lsp_position(bad, offset),
                                "end": lsp_position(bad, offset + (1 if second else 5))}
                    assert failure["range"] == expected, failure
                    origin = bad.index("();", bad.index(publication)) + 1
                    related = failure.get("relatedInformation", [])
                    assert len(related) == 1, failure
                    assert related[0]["location"] == {
                        "uri": uri, "range": {"start": lsp_position(bad, origin),
                                               "end": lsp_position(bad, origin + 1)}}, failure
                    assert "borrow originates here" in related[0]["message"], failure
                    assert "until its carrier is destroyed" in failure["message"], failure
                    action_params = {"textDocument": {"uri": uri},
                                     "range": failure["range"],
                                     "context": {"diagnostics": [failure], "only": ["quickfix"]}}
                    assert not request("textDocument/codeAction", action_params)
                    fixed = bad.replace(publication, "{ " + publication + " }")
                    version += 1
                    change(version, fixed)
                    assert not errors(version)
                    assert not request("textDocument/codeAction", action_params), \
                        "stale conflict must not edit the corrected document"
                    change(version - 1, bad)
                    # A stale didChange cannot replace the newer valid snapshot.
                    assert not request("textDocument/codeAction", action_params)
        finally:
            session.close()
    print("shared/scoped jobs: transfer, borrow conflicts, navigation and recovery passed")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""The resource protocol shares compiler diagnostics, navigation and completion."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = """#include <std/resource_vector>
#include <std/resource>
#include <std/memory>
struct Pool {
  unsafe expected<std::raw_allocation, std::alloc_errc> allocate(std::layout request) {
    return unexpected(std::alloc_errc::resource_exhausted);
  }
  unsafe void release(std::raw_allocation allocation) {}
  void reset() mut {}
};
int fill(std::resource_ref<Pool> resource) {
  mut std::resource_vector<int, Pool> values{resource};
  auto pushed = values.try_push_back(7);
  return pushed ? 0 : 1;
}
int main() {
  mut Pool pool{};
  unsafe {
    /*😀*/ auto resource = std::bind_resource(pool);
    int status = fill(resource);
    if (status != 1) { return status; }
  }
  pool.reset();
  return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-resource-lsp-") as directory:
        path = pathlib.Path(directory) / "main.gti"
        path.write_text(SOURCE)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        sequence = 0

        def request(method, params):
            nonlocal sequence
            sequence += 1
            session.send({"jsonrpc": "2.0", "id": sequence,
                          "method": method, "params": params})
            reply = session.receive_until(lambda message: message.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            return session.receive_until(lambda message:
                message.get("method") == "textDocument/publishDiagnostics"
                and message["params"].get("uri") == uri
                and message["params"].get("version") == version)["params"]["diagnostics"]

        def errors(version):
            return [d for d in diagnostics(version) if d.get("severity") == 1]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        def position(text, offset):
            return {"textDocument": {"uri": uri}, "position": lsp_position(text, offset)}

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "publishDiagnostics": {"relatedInformation": True, "dataSupport": True}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": uri, "languageId": "gti",
                                                      "version": 1, "text": SOURCE}}})
            assert not errors(1)

            # The trust step is an ordinary unsafe library function.
            bind = position(SOURCE, SOURCE.index("bind_resource") + 2)
            hover = request("textDocument/hover", bind)
            assert "unsafe" in str(hover) and "resource_ref" in str(hover), hover
            definitions = request("textDocument/definition", bind)
            assert definitions and "resource.gti" in str(definitions), definitions

            # Omitting the explicit acknowledgement is the shared unsafe-call error.
            unbound = SOURCE.replace("  unsafe {\n    /*😀*/", "  {\n    /*😀*/")
            change(2, unbound)
            found = errors(2)
            missing = next((d for d in found if d.get("code") == "GTI-S2055"), None)
            assert missing is not None, found
            assert missing["range"]["start"]["line"] == lsp_position(
                unbound, unbound.index("bind_resource"))["line"], missing

            # Resetting the provider beneath a live container is a checked
            # borrow conflict, reported at the reset call.
            live = SOURCE.replace(
                "    int status = fill(resource);\n",
                "    mut std::resource_vector<int, Pool> held{resource};\n"
                "    pool.reset();\n"
                "    int status = fill(resource);\n"
                "    held.clear();\n")
            change(3, live)
            found = errors(3)
            conflict = next((d for d in found if d.get("code") == "GTI-S2017"), None)
            assert conflict is not None, found
            assert conflict["range"]["start"]["line"] == lsp_position(
                live, live.index("pool.reset();\n    int status"))["line"], conflict

            # Member completion lists the recoverable container operations.
            incomplete = SOURCE.replace("values.try_push_back(7)", "values.")
            change(4, incomplete)
            diagnostics(4)
            offset = incomplete.index("values.;") + len("values.")
            completions = request("textDocument/completion", position(incomplete, offset))
            items = completions["items"] if isinstance(completions, dict) else completions
            labels = {item["label"] for item in items}
            for name in ("try_push_back", "try_emplace_back", "try_reserve",
                         "push_back", "capacity", "release"):
                assert name in labels, (name, sorted(labels))

            change(5, SOURCE)
            assert not errors(5)
        finally:
            session.close()
    print("resource protocol LSP diagnostics, navigation and completion passed")


if __name__ == "__main__":
    main()

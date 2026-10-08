#!/usr/bin/env python3
"""std::string_view members are ordinary source declarations in tooling.

Hover, definition, and completion reach the prelude's GTI declarations of
std::string_view through the same paths as any class. The compiler-private
text capability, its bridge constructor, and the trusted accessor never
appear in application tooling.
"""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


SOURCE = '''#include <std/string>
int main() {
  std::string owned = std::string("key=value");
  mut std::string_view view = owned.view();
  std::size_t split = view.find('=');
  view.remove_prefix(split + std::size_t(1));
  return view.starts_with("val") ? 0 : 1;
}
'''

HIDDEN = {"text", "text_capability", "text_view_slice"}


def main():
    with tempfile.TemporaryDirectory(prefix="gti-string-view-lsp-") as directory:
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
            reply = session.receive_until(lambda item: item.get("id") == sequence)
            assert "error" not in reply, reply
            return reply["result"]

        def diagnostics(version):
            return session.receive_until(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version)["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        def at(text, needle, delta=0):
            return {"textDocument": {"uri": uri},
                    "position": lsp_position(text, text.index(needle) + delta)}

        def labels(result):
            items = result["items"] if isinstance(result, dict) else result
            return {item["label"] for item in items}

        try:
            request("initialize", {"capabilities": {"textDocument": {
                "hover": {"contentFormat": ["markdown", "plaintext"]}}}})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1,
                                 "text": SOURCE}}})
            assert not diagnostics(1)

            # Hover shows the selected source declaration, not an intrinsic.
            hover = request("textDocument/hover", at(SOURCE, "find(", 1))
            value = hover["contents"]["value"]
            assert "std::string_view::find(char needle)" in value, value
            hover = request("textDocument/hover", at(SOURCE, "remove_prefix", 1))
            assert "std::string_view::remove_prefix(" in hover["contents"]["value"], hover

            # Definition lands in the prelude's GTI source for the method.
            for needle in ("find(", "remove_prefix", "starts_with"):
                definition = request("textDocument/definition", at(SOURCE, needle, 1))
                locations = definition if isinstance(definition, list) else [definition]
                assert locations, (needle, definition)
                assert any(location["uri"].endswith("prelude.gti")
                           for location in locations), (needle, locations)

            # Completion lists the source-defined API, including mutating
            # members for a mutable view, and withholds every private name.
            incomplete = SOURCE.replace("  view.remove_prefix(split + std::size_t(1));\n",
                                        "  view.\n")
            change(2, incomplete)
            diagnostics(2)
            offset = incomplete.index("view.\n") + len("view.")
            members = labels(request("textDocument/completion", {
                "textDocument": {"uri": uri},
                "position": lsp_position(incomplete, offset)}))
            for expected in ("size", "empty", "find", "rfind", "find_first_of",
                             "substr", "starts_with", "ends_with", "contains",
                             "compare", "remove_prefix", "remove_suffix",
                             "front", "back", "at", "begin", "end"):
                assert expected in members, (expected, sorted(members))
            assert not members & HIDDEN, sorted(members & HIDDEN)

            # The private capability is not an application type.
            private = SOURCE.replace("int main() {",
                                     "void leak(gti_internal::text_view raw) {}\nint main() {")
            change(3, private)
            assert any(item.get("severity") == 1 for item in diagnostics(3))

            change(4, SOURCE)
            assert not diagnostics(4)
        finally:
            session.close()


if __name__ == "__main__":
    main()

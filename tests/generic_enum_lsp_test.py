#!/usr/bin/env python3
"""Generic enums keep compiler-owned exact signatures across dirty overlays."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import (
    LspSession, has_error_diagnostics, lsp_position, semantic_tokens_by_position,
)


DECLARATIONS = """enum Message<T, U = int64_t, uint64_t Channel = 3> {
  idle,
  item(T payload, U detail)
};
"""

SOURCE = """#include "messages.gti"
using Count = Message<int32_t>;
int main() {
  // The supplementary character tests UTF-16 navigation below.
  /* 😀 */ auto first = Count::item(1, int64_t(2));
  auto second = Message<bool, bool, 7>::item(true, false);
  switch (first) {
    case Message<int32_t>::idle: return 1;
    case Message<int32_t>::item(payload, detail): break;
  }
  switch (second) {
    case Message<bool, bool, 7>::idle: return 2;
    case Message<bool, bool, 7>::item(payload, detail): return 0;
  }
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="gti-generic-enum-lsp-") as temporary:
        root = pathlib.Path(temporary)
        declarations = root / "messages.gti"
        declarations.write_text(DECLARATIONS, encoding="utf-8")
        entry = root / "main.gti"
        entry.write_text(SOURCE, encoding="utf-8")
        uri, declaration_uri = entry.as_uri(), declarations.as_uri()
        session = LspSession(sys.argv[1])
        request_id = 1
        pending_publications = []

        def receive(predicate):
            def selected(item):
                if predicate(item):
                    return True
                if item.get("method") == "textDocument/publishDiagnostics":
                    pending_publications.append(item["params"])
                return False
            return session.receive_until(selected)

        def request(method, document, source=None, offset=None):
            nonlocal request_id
            request_id += 1
            params = {"textDocument": {"uri": document}}
            if source is not None and offset is not None:
                params["position"] = lsp_position(source, offset)
            session.send({"jsonrpc": "2.0", "id": request_id, "method": method,
                          "params": params})
            reply = receive(lambda item: item.get("id") == request_id)
            assert "error" not in reply, reply
            return reply.get("result")

        def publication(document, version):
            for index, item in enumerate(pending_publications):
                if item["uri"] == document and item.get("version") == version:
                    return pending_publications.pop(index)
            return receive(lambda item:
                item.get("method") == "textDocument/publishDiagnostics"
                and item["params"]["uri"] == document
                and item["params"].get("version") == version)["params"]

        def open_document(document, source):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {"uri": document,
                              "languageId": "gti", "version": 1, "text": source}}})
            result = publication(document, 1)
            assert not has_error_diagnostics(result), result

        def change(document, source, version):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": document,
                              "version": version}, "contentChanges": [{"text": source}]}})

        def assert_hover(document, source, marker, expected, start=0):
            offset = source.index(marker, start)
            result = request("textDocument/hover", document, source, offset)
            assert result and expected in str(result), (marker, expected, result)
            assert result["range"]["start"] == lsp_position(source, offset), result
            return offset

        def assert_completion(source, marker, expected):
            result = request("textDocument/completion", uri, source,
                             source.index(marker) + len(marker))
            items = result.get("items", []) if isinstance(result, dict) else result
            variant = next((item for item in items if item["label"] == "item"), None)
            assert variant and expected in variant.get("detail", ""), (expected, result)
            assert any(item["label"] == "idle" for item in items), result

        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            initialized = session.receive_until(lambda item: item.get("id") == 1)
            token_types = initialized["result"]["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            open_document(declaration_uri, DECLARATIONS)
            open_document(uri, SOURCE)

            # Binder identity comes from semantic lookup, not a name search.
            binder = DECLARATIONS.index("T,")
            use = DECLARATIONS.index("T payload")
            assert_hover(declaration_uri, DECLARATIONS, "T,", "type parameter T")
            location = request("textDocument/definition", declaration_uri, DECLARATIONS, use)
            location = location[0] if isinstance(location, list) else location
            assert location["uri"] == declaration_uri, location
            assert location["range"]["start"] == lsp_position(DECLARATIONS, binder), location
            assert_hover(declaration_uri, DECLARATIONS, "Channel =", "uint64_t Channel")
            assert_hover(declaration_uri, DECLARATIONS, "Message<", "enum Message<")
            assert_hover(declaration_uri, DECLARATIONS, "item(T", "T payload, U detail")
            tokens = semantic_tokens_by_position(request(
                "textDocument/semanticTokens/full", declaration_uri)["data"])
            binder_position = lsp_position(DECLARATIONS, binder)
            token = tokens[(binder_position["line"], binder_position["character"])]
            assert token_types[token["type"]] == "typeParameter", token

            first = SOURCE.index("item(1")
            assert_hover(uri, SOURCE, "item(1", "Message<int32_t, int64_t, 3>::item(int32_t payload, int64_t detail)")
            assert_hover(uri, SOURCE, "item(true", "Message<bool, bool, 7>::item(bool payload, bool detail)")
            assert_hover(uri, SOURCE, "item(payload", "int32_t payload, int64_t detail")
            assert_completion(SOURCE, "Count::", "int32_t payload, int64_t detail")
            assert_completion(SOURCE, "Message<bool, bool, 7>::", "bool payload, bool detail")
            tokens = semantic_tokens_by_position(request(
                "textDocument/semanticTokens/full", uri)["data"])
            variant_position = lsp_position(SOURCE, first)
            token = tokens[(variant_position["line"], variant_position["character"])]
            assert token_types[token["type"]] == "enumMember", token
            location = request("textDocument/definition", uri, SOURCE, first)
            location = location[0] if isinstance(location, list) else location
            assert location["uri"] == declaration_uri, location
            assert location["range"]["start"] == lsp_position(
                DECLARATIONS, DECLARATIONS.index("item(T")), location

            # The other spelling has precisely the same semantics. This edit
            # exists only in the dependency overlay, never on disk.
            spelled = DECLARATIONS.replace("enum Message", "enum class Message")
            change(declaration_uri, spelled, 2)
            assert not has_error_diagnostics(publication(declaration_uri, 2))
            assert not has_error_diagnostics(publication(uri, 1))
            assert_hover(declaration_uri, spelled, "Message<", "enum class Message<")
            assert_hover(uri, SOURCE, "item(1", "int32_t payload, int64_t detail")

            # Altering a generic default updates the consumer's selected
            # signature and diagnostics, rather than retaining the disk type.
            changed = spelled.replace("U = int64_t", "U = bool")
            change(declaration_uri, changed, 3)
            assert not has_error_diagnostics(publication(declaration_uri, 3))
            rejected = publication(uri, 1)
            assert has_error_diagnostics(rejected), rejected
            fixed = SOURCE.replace("item(1, int64_t(2))", "item(1, true)")
            change(uri, fixed, 2)
            assert not has_error_diagnostics(publication(uri, 2))
            assert_hover(uri, fixed, "item(1", "Message<int32_t, bool, 3>::item(int32_t payload, bool detail)")
            assert_completion(fixed, "Count::", "int32_t payload, bool detail")

            broken = changed.replace("U detail)", "U detail")
            change(declaration_uri, broken, 4)
            assert has_error_diagnostics(publication(declaration_uri, 4))
            publication(uri, 2)
            change(declaration_uri, changed, 5)
            assert not has_error_diagnostics(publication(declaration_uri, 5))
            assert not has_error_diagnostics(publication(uri, 2))
            assert_hover(uri, fixed, "item(1", "int32_t payload, bool detail")

            # Close/reopen removes overlay defaults and restores the on-disk
            # declaration; a repaired consumer clears the latest generation.
            session.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                          "params": {"textDocument": {"uri": declaration_uri}}})
            change(uri, SOURCE, 3)
            assert not has_error_diagnostics(publication(uri, 3))
            assert_hover(uri, SOURCE, "item(1", "int32_t payload, int64_t detail")
            open_document(declaration_uri, DECLARATIONS)
            assert_hover(declaration_uri, DECLARATIONS, "Message<", "enum Message<")
        finally:
            session.close()
    print("Generic enum LSP exact variants, binders, overlays and UTF-16 navigation passed")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""A general cleanup-bound escape diagnostic is not mutex-specific."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position


def main():
    source = """// 😀 UTF-16 control
struct Resource { int value = 7; };
class Guard {
  Resource& resource;
public:
  Guard(Resource& resource) : resource(resource) {}
  ~Guard() { int observed = this.resource.value; }
  Resource& read() { return this.resource; }
};
Resource& leak(Resource& resource) {
  Guard guard{resource};
  return guard.read();
}
int main() { return 0; }
"""
    with tempfile.TemporaryDirectory(prefix="gti-cleanup-borrow-lsp-") as directory:
        path = pathlib.Path(directory) / "cleanup.gti"
        path.write_text(source)
        uri = path.as_uri()
        session = LspSession(sys.argv[1])
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "capabilities": {"textDocument": {"publishDiagnostics": {
                    "relatedInformation": True, "dataSupport": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "gti", "version": 1, "text": source}}})

            def diagnostics(version):
                return session.receive_until(lambda message:
                    message.get("method") == "textDocument/publishDiagnostics" and
                    message["params"]["uri"] == uri and message["params"].get("version") == version
                )["params"]["diagnostics"]

            errors = diagnostics(1)
            error = next(item for item in errors if item.get("code") == "GTI-S2017"
                         and "cleanup-bound carrier" in item["message"])
            assert "mutex" not in error["message"] and "unlock" not in error["message"], error
            assert error["severity"] == 1 and error["data"]["phase"] == "semantics", error
            offset = source.index("guard.read()") + len("guard.read(")
            assert error["range"] == {"start": lsp_position(source, offset),
                                      "end": lsp_position(source, offset + 1)}, error
            assert any(item["location"]["uri"] == uri and
                       "carrier is declared here" in item["message"]
                       for item in error["relatedInformation"]), error
            valid = source.replace("  Guard guard{resource};\n  return guard.read();",
                                   "  return resource;")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": valid}]}})
            assert not diagnostics(2)
            nested = """struct Resource {
  mut int value = 7;
  void reset() mut { this.value = 8; }
};
class Guard {
  Resource& resource;
public:
  Guard(Resource& resource) : resource(resource) {}
  ~Guard() { int observed = this.resource.value; }
};
class Owner {
  Guard guard;
public:
  Owner(Resource& resource) : guard(Guard(resource)) {}
};
int main() { mut Resource resource{}; Owner owner{resource}; resource.reset(); return 0; }
"""
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": nested}]}})
            errors = diagnostics(3)
            assert any(item.get("code") == "GTI-S2017" for item in errors), errors
            assert not any(item.get("code") == "GTI-B0001" for item in errors), errors
            nested_valid = nested.replace(" resource.reset();", "").replace(
                "mut Resource resource{}", "Resource resource{}")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": 4}, "contentChanges": [{"text": nested_valid}]}})
            assert not diagnostics(4)
        finally:
            session.close()
    print("cleanup-bound borrow LSP diagnostic passed")


if __name__ == "__main__":
    main()

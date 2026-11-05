"""Call diagnostics use caller ranges and clear after fixing generic uses."""
from pathlib import Path
import sys
import tempfile
from lsp_smoke_test import LspSession, lsp_position


def main():
    with tempfile.TemporaryDirectory(prefix="gti-semantic-call-lsp-") as directory:
        uri = (Path(directory) / "main.gti").as_uri()
        session = LspSession(sys.argv[1])
        version = 0
        def publish(source):
            nonlocal version
            version += 1
            if version == 1:
                method = "textDocument/didOpen"
                params = {"textDocument": {"uri": uri, "languageId": "gti", "version": version, "text": source}}
            else:
                method = "textDocument/didChange"
                params = {"textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}]}
            session.send({"jsonrpc": "2.0", "method": method, "params": params})
            reply = session.receive_until(lambda item: item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri and item["params"].get("version") == version)
            return [item for item in reply["params"]["diagnostics"] if item.get("severity") == 1]
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda item: item.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            macro = "#macro BORDER(uint8_t width) {}\nint main() { BORDER!(33); return 0; }\n"
            assert not publish(macro)
            invalid_macro = macro.replace("BORDER!(33)", "BORDER!(256)")
            errors = publish(invalid_macro)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2003", errors
            offset = invalid_macro.index("256")
            assert errors[0]["range"] == {"start": lsp_position(invalid_macro, offset),
                                          "end": lsp_position(invalid_macro, offset + 3)}, errors
            assert "Integer literal '256' does not fit" in errors[0]["message"], errors
            assert "macro invocation" in str(errors[0].get("relatedInformation")), errors
            assert not publish(macro)
            fixture = Path(__file__).parent / "fixtures/semantic_call_regressions.gti"
            assert not publish(fixture.read_text())
            immediate = "int runtime_value() { return 1; }\nconsteval int check(int value) { return value; }\nint main() { return check(runtime_value()); }\n"
            errors = publish(immediate)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2057", errors
            error = errors[0]
            offset = immediate.index("runtime_value());") + len("runtime_value()")
            assert error["range"] == {"start": lsp_position(immediate, offset), "end": lsp_position(immediate, offset + 1)}, error
            assert "Argument 1" in str(error) and "runtime_value" in str(error), error
            assert "does not name a constexpr" not in str(error), error
            assert any(item["location"]["uri"] == uri and "failed at this operation" in item["message"]
                       for item in error["relatedInformation"]), error
            assert not publish(immediate.replace("int runtime_value", "constexpr int runtime_value"))
            formatting = """import std.format;
struct Opaque { public: Opaque() = default; };
int main() {
  Opaque opaque{};
  std::println("{} {} {}", 1, 2, opaque);
  return 0;
}
"""
            errors = publish(formatting)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2029", errors
            assert errors[0]["range"]["start"]["line"] == 4, errors
            assert "formattable" in str(errors[0]) and "Opaque" in str(errors[0]), errors
            assert "Argument 6" not in str(errors), errors
            assert not publish(formatting.replace('1, 2, opaque', '1, 2, 3'))
            move = """import std.string;
void consume<Args...>(int tag, Args... args) {}
void consume(char tag) {}
int main() { std::string text{"hello"}; consume(1, text); return 0; }
"""
            errors = publish(move)
            assert len(errors) == 1 and errors[0]["code"] == "GTI-S2018", errors
            offset = move.index("text);")
            assert errors[0]["range"]["start"] == lsp_position(move, offset), errors
            assert "deleted copy constructor" in str(errors[0]), errors
            assert not publish(move.replace('1, text', '1, text.view()'))
            borrowed = """import std.string;
import std.format;
int main() { std::string text{"hello"}; std::println("{}", text); return 0; }
"""
            assert not publish(borrowed)
            assert not publish(fixture.read_text())
        finally:
            session.close()
    print("semantic call diagnostics, caller locations and edit clearing passed")

if __name__ == "__main__":
    main()

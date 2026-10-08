#!/usr/bin/env python3
"""Closed programs reject missing GTI bodies before native linking."""

import pathlib
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position
from lsp_project_facts_test import diagnostics_for, open_document


def main():
    compiler, lsp = map(lambda value: str(pathlib.Path(value).resolve()), sys.argv[1:3])
    fixtures = pathlib.Path(__file__).parent / "coverage/reproducers"
    with tempfile.TemporaryDirectory(prefix="gti-missing-definition-") as directory:
        root = pathlib.Path(directory)
        for name in ("f7-undefined-user-function", "f7-declaration-only-weak-ptr",
                     "f7-json-decode-without-codec"):
            for mode in ([], ["--emit-cpp"]):
                rejected = subprocess.run(
                    [compiler, str(fixtures / f"{name}.gti"), *mode, "--cxx",
                     str(root / "no-native-compiler"), "--progress=never",
                     "--color=never", "-o", str(root / name)],
                    capture_output=True, text=True, timeout=90)
                text = rejected.stdout + rejected.stderr
                assert rejected.returncode == 65, (name, mode, rejected)
                assert "GTI-S2106" in text and "has no definition" in text, text
                assert "GTI-B0001" not in text and "native C++ compiler" not in text, text
                if name == "f7-json-decode-without-codec":
                    primary = next(line for line in text.splitlines()
                                   if "error[GTI-S2106]" in line)
                    assert f"{name}.gti:17:29:" in primary, text
        source = "// \U0001f600\nint helper(int value);\nint main() { return helper(7); }\n"
        path = root / "src/main.gti"
        path.parent.mkdir()
        path.write_text(source)
        (root / "gti.toml").write_text(
            'manifest-version = 1\n[package]\nname = "closure"\nversion = "0.1.0"\n'
            '[targets.app]\nkind = "executable"\nroot = "src/main.gti"\n')
        checked = subprocess.run([compiler, "check", "--progress=never", "--color=never"],
                                 cwd=root, capture_output=True, text=True, timeout=90)
        assert checked.returncode == 65 and "GTI-S2106" in checked.stderr, checked
        mapper = """namespace traits {
class Mapper<T> {
public:
  int decode(T& input);
  int inspect();
};
}
int decode<T>(T& input) {
  traits::Mapper<T> mapping{};
  return mapping.decode(input);
}
int inspect<T>() {
  traits::Mapper<T> mapping{};
  return mapping.inspect();
}
"""
        specialization = """class traits::Mapper<int> {
public:
  int decode(int& input) { return input; }
  int inspect() { return 3; }
};
"""
        invocation = "int main() { int input = 7; return decode<int>(input) + inspect<int>() - 10; }\n"
        nested = (mapper + "int outer<T>(T& input) { return decode<T>(input); }\n"
                  "int main() { int input = 7; /* \U0001f600 */ return outer<int>(input); }\n")
        unused_mapper = mapper + "int main() { return 0; }\n"
        selected_mapper = mapper + specialization + invocation
        for name, text in (("unused-mapper", unused_mapper),
                           ("specialized-mapper", selected_mapper)):
            path.write_text(text)
            checked = subprocess.run(
                [compiler, "check", "--progress=never", "--color=never"],
                cwd=root, capture_output=True, text=True, timeout=90)
            assert checked.returncode == 0, (name, checked)
            executable = root / name
            built = subprocess.run(
                [compiler, str(path), "--progress=never", "--color=never",
                 "-o", str(executable)], capture_output=True, text=True, timeout=90)
            assert built.returncode == 0, (name, built)
            ran = subprocess.run([str(executable)], capture_output=True,
                                 text=True, timeout=10)
            assert ran.returncode == 0, (name, ran)
        path.write_text(source)
        session = LspSession(lsp)
        try:
            session.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"capabilities": {"textDocument": {
                              "publishDiagnostics": {"relatedInformation": True}}}}})
            session.receive_until(lambda message: message.get("id") == 1)
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            uri = open_document(session, path, source)
            diagnostics = diagnostics_for(session, uri, 1)
            missing = [item for item in diagnostics if item.get("code") == "GTI-S2106"]
            assert len(missing) == 1, diagnostics
            diagnostic = missing[0]
            assert diagnostic["range"] == {
                "start": {"line": 2, "character": 20},
                "end": {"line": 2, "character": 26}}, diagnostic
            related = diagnostic["relatedInformation"]
            assert len(related) == 1 and related[0]["location"]["uri"] == uri, diagnostic
            assert related[0]["location"]["range"] == {
                "start": {"line": 1, "character": 4},
                "end": {"line": 1, "character": 10}}, diagnostic
            fixed = source.replace("int helper(int value);",
                                   "int helper(int value) { return value; }")
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 2},
                                     "contentChanges": [{"text": fixed}]}})
            assert not diagnostics_for(session, uri, 2)
            for version, text, expected_errors in (
                    (3, unused_mapper, 0),
                    (4, selected_mapper, 0),
                    (5, mapper + invocation, 2),
                    (6, selected_mapper, 0),
                    (7, nested, 1),
                    (8, nested.replace(mapper, mapper + specialization), 0)):
                session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                              "params": {"textDocument": {"uri": uri, "version": version},
                                         "contentChanges": [{"text": text}]}})
                diagnostics = diagnostics_for(session, uri, version)
                missing = [item for item in diagnostics if item.get("code") == "GTI-S2106"]
                assert len(missing) == expected_errors, (version, diagnostics)
                assert not any(item.get("code") == "GTI-B0001" for item in diagnostics), diagnostics
                if expected_errors:
                    names = ("outer",) if version == 7 else ("decode", "inspect")
                    for diagnostic, name in zip(missing, names):
                        offset = text.rindex(name + "<int>")
                        assert diagnostic["range"] == {
                            "start": lsp_position(text, offset),
                            "end": lsp_position(text, offset + len(name))}, diagnostic
                        related = diagnostic["relatedInformation"]
                        assert any("bodyless declaration" in item["message"] for item in related), diagnostic
                        assert any("selected call requires" in item["message"] for item in related), diagnostic
                        assert all(item["location"]["uri"] == uri for item in related), diagnostic
                if not expected_errors:
                    assert not diagnostics, (version, diagnostics)
            dependency = path.parent / "mapper.gti"
            dependency.write_text(mapper +
                                  "int outer<T>(T& input) { return decode<T>(input); }\n")
            consumer = ('#include "mapper.gti"\n'
                        'int main() { int input = 7; return outer<int>(input); }\n')
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 9},
                                     "contentChanges": [{"text": consumer}]}})
            diagnostics = diagnostics_for(session, uri, 9)
            missing = [item for item in diagnostics if item.get("code") == "GTI-S2106"]
            assert len(missing) == 1, diagnostics
            offset = consumer.index("outer<int>")
            assert missing[0]["range"] == {
                "start": lsp_position(consumer, offset),
                "end": lsp_position(consumer, offset + len("outer"))}, missing
            related = missing[0]["relatedInformation"]
            assert len(related) == 3, related
            assert all(item["location"]["uri"] == dependency.resolve().as_uri()
                       for item in related), related
            constructor = ('#include "mapper.gti"\n'
                           'class Factory<T> { public: int value;\n'
                           '  Factory(T& input) : value(outer<T>(input)) {}\n};\n'
                           'int main() { int input = 7; Factory<int> result{input}; return 0; }\n')
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": 10},
                                     "contentChanges": [{"text": constructor}]}})
            diagnostics = diagnostics_for(session, uri, 10)
            missing = [item for item in diagnostics if item.get("code") == "GTI-S2106"]
            assert len(missing) == 1, diagnostics
            offset = constructor.index("{input}")
            assert missing[0]["range"] == {
                "start": lsp_position(constructor, offset),
                "end": lsp_position(constructor, offset + 1)}, missing
        finally:
            session.close()


if __name__ == "__main__":
    main()

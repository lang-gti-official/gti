#!/usr/bin/env python3
"""Signature metadata shares diagnostics, hover, and formatting with the compiler."""

import pathlib
import sys
import tempfile

from lsp_smoke_test import LspSession


def main():
    source = (
        "// supplementary character: \U0001f600\n"
        "using Action = void();\n"
        "class Tag<S = void> { public: Tag() = default; };\n"
        "int describe<R>(Tag<R()>& tag) { return 0; }\n"
        "int main() { Tag<Action> tag{}; Tag<void> metadata{}; return describe(tag); }\n"
    )
    with tempfile.TemporaryDirectory(prefix="gti-signature-lsp-") as directory:
        uri = (pathlib.Path(directory) / "main.gti").as_uri()
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
            reply = session.receive_until(
                lambda item: item.get("method") == "textDocument/publishDiagnostics"
                and item["params"].get("uri") == uri
                and item["params"].get("version") == version
            )
            return reply["params"]["diagnostics"]

        def change(version, text):
            session.send({"jsonrpc": "2.0", "method": "textDocument/didChange",
                          "params": {"textDocument": {"uri": uri, "version": version},
                                     "contentChanges": [{"text": text}]}})

        try:
            request("initialize", {"capabilities": {
                "textDocument": {"publishDiagnostics": {"relatedInformation": True}}
            }})
            session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            session.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                          "params": {"textDocument": {
                              "uri": uri, "languageId": "gti", "version": 1,
                              "text": source}}})
            assert not diagnostics(1)
            position = {"line": 4, "character": source.splitlines()[4].index("Action")}
            params = {"textDocument": {"uri": uri}, "position": position}
            assert "void()" in str(request("textDocument/hover", params))
            definition = request("textDocument/definition", params)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["range"]["start"] == {"line": 1, "character": 6}, definition
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 4, "character": source.splitlines()[4].index("describe")}})
            assert "void" in str(selected) and "R()" not in str(selected), selected

            invalid = source.replace("void()", "void(void)")
            change(2, invalid)
            errors = diagnostics(2)
            signature_error = next(error for error in errors
                                   if error.get("code") == "GTI-S2093")
            assert signature_error["range"] == {
                "start": {"line": 1, "character": 20},
                "end": {"line": 1, "character": 24}}, signature_error
            change(3, source.replace("void()", "void("))
            assert diagnostics(3)
            change(4, source)
            assert not diagnostics(4)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else source
            change(5, formatted)
            assert not diagnostics(5)
            # Concrete aliases must be revalidated in the editor too; int(void)
            # is invalid even when the symbolic alias int(S) was valid.
            invalid_alias = source.replace("class Tag<S = void> { public: Tag() = default; };",
                                           "class Tag<S = void> { using Bad = int(S);  public: Tag() = default; };")
            change(6, invalid_alias)
            assert any(error.get("code") == "GTI-S2093" for error in diagnostics(6))
            change(7, source)
            assert not diagnostics(7)
            packs = (
                "class Signature<R, Args...> { using type = R(Args...);  public: Signature() = default; };\n"
                "class Forward<R, Args...> { Signature<R, Args...> field{};  public: Forward() = default; };\n"
                "class Types<Args...> { public: Types() = default; };\n"
                "int main() { Forward<void, int&> value{}; Types<> empty{}; return 0; }\n"
            )
            change(8, packs)
            assert not diagnostics(8)
            pack_use = {"textDocument": {"uri": uri},
                        "position": {"line": 0, "character": packs.index("R(Args") + 2}}
            definition = request("textDocument/definition", pack_use)
            location = definition[0] if isinstance(definition, list) else definition
            assert location["range"]["start"] == {"line": 0, "character": 19}, definition
            change(9, packs.replace("R(Args...)", "R(Args..., int)"))
            errors = diagnostics(9)
            error = next(item for item in errors if item.get("code") == "GTI-S2023")
            start = packs.index("R(Args") + len("R(Args")
            assert error["range"] == {"start": {"line": 0, "character": start},
                                      "end": {"line": 0, "character": start + 3}}, error
            change(10, packs.replace("Args...);", "Args...;"))
            assert diagnostics(10)
            change(11, packs)
            assert not diagnostics(11)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else packs
            change(12, formatted)
            assert not diagnostics(12)
            methods = (
                "class Relay<Args...> { public: void send(Args... args) {}  public: Relay() = default; };\n"
                "int main() { Relay<int, bool> value{}; value.send(1, true); return 0; }\n"
            )
            change(13, methods)
            assert not diagnostics(13)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 1, "character": methods.splitlines()[1].index("send")}})
            assert "int32_t" in str(selected) and "bool" in str(selected), selected
            invalid_method = methods.replace("send(1, true)", "send(1)")
            change(14, invalid_method)
            errors = diagnostics(14)
            error = next(error for error in errors
                         if error.get("code") == "GTI-S2005")
            assert "class pack" in error["message"], error
            call_end = invalid_method.splitlines()[1].index("send(1)") + len("send(1")
            assert error["range"] == {
                "start": {"line": 1, "character": call_end},
                "end": {"line": 1, "character": call_end + 1}}, error
            pack_name = methods.splitlines()[0].index("args)")
            related = error["relatedInformation"][0]
            assert related["location"] == {"uri": uri, "range": {
                "start": {"line": 0, "character": pack_name},
                "end": {"line": 0, "character": pack_name + 4}}}, error
            change(15, methods)
            assert not diagnostics(15)
            decomposition = (
                "class Tag<S> { public: Tag() = default; };\n"
                "int unpack<R, Args...>(Tag<R(Args...)>& tag) { return 7; }\n"
                "int main() { Tag<int(int, bool)> tag{}; return unpack(tag); }\n"
            )
            change(16, decomposition)
            assert not diagnostics(16)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": decomposition.splitlines()[2].index("unpack")}})
            assert "int32_t(int32_t, bool)" in str(selected) and "Args..." not in str(selected), selected
            invalid_decomposition = decomposition.replace("unpack(tag)", "unpack<int, bool>(tag)")
            change(17, invalid_decomposition)
            errors = diagnostics(17)
            error = next(error for error in errors if error.get("code") == "GTI-S2023"
                         and "one exact type pack" in error["message"])
            call_end = invalid_decomposition.splitlines()[2].index("(tag)") + 4
            assert error["range"] == {
                "start": {"line": 2, "character": call_end},
                "end": {"line": 2, "character": call_end + 1}}, error
            assert error["relatedInformation"][0]["location"]["range"] == {
                "start": {"line": 1, "character": 14},
                "end": {"line": 1, "character": 18}}, error
            change(18, decomposition)
            assert not diagnostics(18)

            # One public signature binds the class's result and argument pack.
            pattern = "// \U0001f600\nclass Callback<R(Args...)> { public: Callback() = default; };\n"
            change(19, pattern)
            assert not diagnostics(19)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 1, "character": 8}})
            assert "class Callback<R(Args...)>" in str(selected), selected
            change(20, pattern + "Callback<int> invalid{};\n")
            errors = diagnostics(20)
            error = next(error for error in errors if error.get("code") == "GTI-S2023"
                         and "argument pattern" in error["message"])
            assert error["range"] == {
                "start": {"line": 2, "character": 0},
                "end": {"line": 2, "character": 8}}, error
            assert error["relatedInformation"][0]["location"]["range"] == {
                "start": {"line": 1, "character": 6},
                "end": {"line": 1, "character": 14}}, error
            assert not any(error.get("code") == "GTI-B0001" for error in errors), errors
            change(21, pattern + "Callback<void()> valid{};\n")
            assert not diagnostics(21)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 18}})
            assert "Callback<void()>" in str(selected), selected

            borrowed = (
                "#include <std/functional>\n"
                "struct Read { int operator()(int& value) { return value; }  public: Read() = default; };\n"
                "int main() {\n"
                "  Read reader{};\n"
                "  std::function<int(int&)> callback{std::move(reader)};\n"
                "  int value = 7;\n"
                "  return callback(value);\n"
                "}\n"
            )
            change(22, borrowed)
            assert not diagnostics(22)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 6, "character": 10}})
            assert "int32_t&" in str(selected), selected
            change(23, borrowed.replace("callback(value)", "callback(true)"))
            errors = diagnostics(23)
            assert errors and not any(error.get("code") == "GTI-B0001" for error in errors), errors
            change(24, borrowed)
            assert not diagnostics(24)
            stored_item = (
                "#include <std/functional>\n"
                "int selected(int value) { return value; }\n"
                "int main() {\n"
                "  auto item = selected;\n"
                "  std::function<int(int)> callback{item};\n"
                "  return callback(7);\n"
                "}\n"
            )
            change(25, stored_item)
            assert not diagnostics(25)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": 15}})
            assert "selected" in str(selected) and "int32_t" in str(selected), selected
            definition = request("textDocument/definition", {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": 15}})
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri and location["range"]["start"]["line"] == 1, definition
            # An unsaved overload changes selection, then removal clears it.
            change(26, stored_item.replace("int main()", "int selected(bool value) { return 1; }\nint main()"))
            errors = diagnostics(26)
            error = next(error for error in errors if error.get("code") == "GTI-S2090")
            assert "ambiguous" in error["message"], error
            assert len(error["relatedInformation"]) == 2, error
            assert error["range"] == {"start": {"line": 4, "character": 14},
                                      "end": {"line": 4, "character": 22}}, error
            change(27, stored_item)
            assert not diagnostics(27)
            qualified = (
                "using Mutable = int(int) mut;\n"
                "class Tag<R(Args...) mut> { public: Tag() = default; };\n"
                "int main() { Tag<Mutable> value{}; return 0; }\n"
            )
            change(28, qualified)
            assert not diagnostics(28)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 18}})
            assert "int32_t(int32_t) mut" in str(selected), selected
            change(29, qualified.replace("int(int) mut", "int(int)"))
            errors = diagnostics(29)
            assert any(error.get("code") == "GTI-S2023" for error in errors), errors
            assert not any(error.get("code") == "GTI-B0001" for error in errors), errors
            change(30, qualified.replace("int(int) mut", "int(int) mut mut"))
            assert diagnostics(30)
            change(31, qualified)
            assert not diagnostics(31)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else qualified
            change(32, formatted)
            assert not diagnostics(32)
            family = (
                "// \U0001f600\n"
                "class Callback<R(Args...)> { public: Callback() = default; int mode() { return 1; } };\n"
                "class Callback<R(Args...) mut> { public: Callback() = default; int mode() { return 2; } };\n"
                "int main() { Callback<int() mut> value{}; return value.mode(); }\n"
            )
            change(33, family)
            assert not diagnostics(33)
            definition = request("textDocument/definition", {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": 15}})
            location = definition[0] if isinstance(definition, list) else definition
            assert location["uri"] == uri and location["range"]["start"]["line"] == 2, definition
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": 15}})
            assert "mut" in str(selected), selected
            # A duplicate receiver mode is an error even with different binders.
            change(34, family.replace("R(Args...) mut", "R(Args...)"))
            errors = diagnostics(34)
            assert any(error.get("code") == "GTI-S2006" for error in errors), errors
            change(35, family.replace("int() mut", "int() &&"))
            errors = diagnostics(35)
            assert any(error.get("code") == "GTI-S2023" for error in errors), errors
            change(36, family)
            assert not diagnostics(36)
            mutable_arguments = (
                "#include <std/functional>\n"
                "void change(mut int& value) { value++; }\n"
                "int main() {\n"
                "  std::function<void(mut int&)> callback{change};\n"
                "  mut int value = 0;\n"
                "  callback(value);\n"
                "  return value;\n"
                "}\n"
            )
            change(37, mutable_arguments)
            assert not diagnostics(37)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 5, "character": 5}})
            assert "mut int32_t&" in str(selected), selected
            change(38, mutable_arguments.replace("mut int&)", "mut int)"))
            errors = diagnostics(38)
            error = next(error for error in errors if error.get("code") == "GTI-S2093")
            start = mutable_arguments.splitlines()[3].index("mut")
            assert error["range"] == {
                "start": {"line": 3, "character": start},
                "end": {"line": 3, "character": start + 3}}, error
            change(39, mutable_arguments.replace("mut int value", "int value"))
            errors = diagnostics(39)
            assert errors and not any(error.get("code") == "GTI-B0001" for error in errors), errors
            change(40, mutable_arguments)
            assert not diagnostics(40)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else mutable_arguments
            change(41, formatted)
            assert not diagnostics(41)
            consuming_virtual = (
                "#include <std/memory>\n"
                "interface Operation { int operator()(int value) &&; };\n"
                "class Increment : public Operation { public:\n"
                "  int operator()(int value) && override { return value + 1; }\n"
                " public: Increment() = default; };\n"
                "int main() { Increment operation{}; return std::move(operation)(2); }\n"
            )
            change(42, consuming_virtual)
            assert not diagnostics(42)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character":
                             consuming_virtual.splitlines()[3].index("operator") + len("operator")}})
            assert "&&" in str(selected), selected
            wrong_override = consuming_virtual.replace("&& override", "mut override")
            change(43, wrong_override)
            errors = diagnostics(43)
            error = next(error for error in errors if error.get("code") == "GTI-S2042")
            start = wrong_override.splitlines()[3].index("override")
            assert error["range"] == {
                "start": {"line": 3, "character": start},
                "end": {"line": 3, "character": start + len("override")}}, error
            change(44, consuming_virtual)
            assert not diagnostics(44)
            nested_callback = (
                "#include <std/memory>\n"
                "int combine(int left, int right) { return left + right; }\n"
                "int invoke<T>(T callback) {\n"
                "  return combine(std::move(callback)(3), 2);\n"
                "}\n"
                "int main() {\n"
                "  auto callback = [](int value) -> int { return value; };\n"
                "  return invoke(std::move(callback));\n"
                "}\n"
            )
            change(45, nested_callback)
            assert not diagnostics(45)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character":
                             nested_callback.splitlines()[3].index("combine")}})
            assert "int32_t" in str(selected) and "void" not in str(selected), selected
            change(46, nested_callback.replace(
                "(callback)(3), 2", "(callback)(3), callback(2)"))
            errors = diagnostics(46)
            assert any("moved" in error["message"] for error in errors), errors
            change(47, nested_callback)
            assert not diagnostics(47)
            consuming_wrapper = (
                "#include <std/functional>\n"
                "int main() {\n"
                "  std::function<int(int) &&> callback{[](int value) -> int { return value; }};\n"
                "  return std::move(callback)(3);\n"
                "}\n"
            )
            change(48, consuming_wrapper)
            assert not diagnostics(48)
            selected = request("textDocument/hover", {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character":
                             consuming_wrapper.splitlines()[2].index("callback")}})
            assert "function" in str(selected) and "&&" in str(selected), selected
            change(49, consuming_wrapper.replace("std::move(callback)(3)", "callback(3)"))
            errors = diagnostics(49)
            assert any("consum" in error["message"].lower() for error in errors), errors
            change(50, consuming_wrapper.replace(
                "return std::move(callback)(3);",
                "int first = std::move(callback)(3); return std::move(callback)(4);"))
            errors = diagnostics(50)
            assert any("moved" in error["message"] for error in errors), errors
            change(51, consuming_wrapper)
            assert not diagnostics(51)
            # Imported nominal types are not declared in the formatted buffer.
            # Keep the callback's generic angles and anonymous reference
            # parameters compact, without recognizing std::function by name.
            (pathlib.Path(directory) / "events.gti").write_text(
                "export module events;\nexport struct Event { int code = 0;  public: Event() = default; };\n")
            window = (
                "// supplementary character: \U0001f600\n"
                "export module window;\nimport std;\nimport \"events.gti\";\n"
                "export interface Window {\n"
                "  using EventCallbackFn = std::function < void(Event &) >;\n"
                "  void SetEventCallback(EventCallbackFn& callback);\n"
                "};\n"
                "export std::unique_ptr < Event > CreateEvent() {\n"
                "  return std::make_unique<Event>();\n"
                "}\n"
            )
            change(52, window)
            assert not diagnostics(52)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            formatted = edits[0]["newText"] if edits else window
            assert "using EventCallbackFn = std::function<void(Event&)>;" in formatted, formatted
            assert "export std::unique_ptr<Event> CreateEvent()" in formatted, formatted
            change(53, formatted)
            assert not diagnostics(53)
            edits = request("textDocument/formatting", {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": True}})
            assert not edits or edits[0]["newText"] == formatted, edits
        finally:
            session.close()


if __name__ == "__main__":
    main()

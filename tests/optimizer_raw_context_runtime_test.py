#!/usr/bin/env python3
"""Contextual arithmetic before raw writes, checked failures after them."""

from pathlib import Path
import tempfile

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/optimizer_raw_context.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "return destination + 1" in text)
    overflow = (f"{fixture.name}:{line}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n")
    cases = {
        "optimizer_raw_context": [
            ((), (0, "8\n2147483647\n", "")),
            (("overflow",), (70, "8\n2147483647\n", overflow)),
        ]
    }
    source_path = fixture.parent / "optimizer_scalar_addresses.gti"
    source = source_path.read_text()
    functions = ("bad_replaced", "bad_joined", "bad_alias", "bad_raw_write",
                 "bad_pointer_alias", "bad_lifetime", "bad_return_write",
                 "bad_return_choice", "bad_return_redirect", "bad_return_alias")
    cases["scalar_addresses"] = [((), (0, "", ""))]
    paths = {"optimizer_raw_context": fixture, "scalar_addresses": source_path}
    with tempfile.TemporaryDirectory(prefix="gti-scalar-addresses-") as temporary:
        for function in functions:
            name = f"scalar_addresses_{function}"
            path = Path(temporary) / f"{name}.gti"
            call = (f"{function}(true)"
                    if function in ("bad_joined", "bad_return_choice")
                    else f"{function}()")
            dispatch = f"if (false) return {call};"
            assert source.count(dispatch) == 1
            path.write_text(source.replace(dispatch, f"if (true) return {call};"))
            paths[name] = path
            lines = source.splitlines()
            start = next(index for index, text in enumerate(lines, 1)
                         if text.startswith(f"int {function}("))
            line = next(index for index, text in enumerate(lines, 1)
                        if index > start and "return *pointer + 1;" in text)
            error = (f"{path.name}:{line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n")
            cases[name] = [((), (70, "", error))]
        constructed_path = fixture.parent / "optimizer_constructed_addresses.gti"
        constructed_source = constructed_path.read_text()
        paths["constructed_addresses"] = constructed_path
        cases["constructed_addresses"] = [((), (0, "", ""))]
        paths["constructed_budget"] = fixture.parent / "optimizer_constructed_address_budget.gti"
        cases["constructed_budget"] = [((), (0, "", ""))]
        constructions = {
            "bad_constructed_value": "if (false) return bad_constructed_value(storage.data());",
            "bad_constructed_hole": "if (false) {",
            "bad_constructed_conditional": "if (false) return bad_constructed_conditional(&pair[1], false);",
            "bad_constructed_alias": "if (false) return bad_constructed_alias(storage.data());",
            "bad_constructed_replacement": "if (false) return bad_constructed_replacement(storage.data());",
            "bad_constructed_call_write": "if (false) return bad_constructed_call_write(storage.data());",
            "bad_constructed_second_alias": "if (false) return bad_constructed_second_alias(storage.data(), storage.data());",
            "bad_constructed_parameter": "if (false) return bad_constructed_parameter(storage.data());",
            "bad_constructed_redirect": "if (false) return bad_constructed_redirect(storage.data(), &pair[1]);",
            "bad_constructed_pack": "if (false) return bad_constructed_pack(storage.data());",
        }
        for function, dispatch in constructions.items():
            name = f"constructed_addresses_{function}"
            path = Path(temporary) / f"{name}.gti"
            assert constructed_source.count(dispatch) == 1
            path.write_text(constructed_source.replace(
                dispatch, dispatch.replace("if (false)", "if (true)", 1)))
            paths[name] = path
            lines = constructed_source.splitlines()
            start = next(index for index, text in enumerate(lines, 1)
                         if text.startswith(f"unsafe int {function}("))
            line = next(index for index, text in enumerate(lines, 1)
                        if index > start and "return *pointer + 1;" in text)
            error = (f"{path.name}:{line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n")
            cases[name] = [((), (70, "", error))]
        fields_path = fixture.parent / "optimizer_pointer_fields.gti"
        fields_source = fields_path.read_text()
        paths["pointer_fields"] = fields_path
        cases["pointer_fields"] = [((), (0, "", ""))]
        field_dispatches = {
            "bad_field_value": "if (false) return addresses.bad_field_value();",
            "bad_field_hole": "if (false) {\n      std::destroy_at(&pair[0]);",
            "bad_field_alias": "if (false) return aliasing.bad_field_alias();",
            "bad_field_redirect": "if (false) return addresses.bad_field_redirect();",
            "bad_field_old_address": "if (false) return addresses.bad_field_old_address();",
            "bad_field_replacement": "if (false) return addresses.bad_field_replacement();",
            "bad_member_value": "if (false) return addresses.bad_member_value();",
            "bad_member_conditional": "if (false) {\n      AddressPair existing",
            "bad_member_alias": "if (false) return aliasing.bad_member_alias();",
            "bad_reconstructed_metadata": "if (false) return bad_reconstructed_metadata(metadata, &metadata.count);",
            "bad_metadata_alias": "if (false) return bad_metadata_alias(metadata, &metadata.count, &metadata.count);",
        }
        for function, dispatch in field_dispatches.items():
            name = f"pointer_fields_{function}"
            path = Path(temporary) / f"{name}.gti"
            assert fields_source.count(dispatch) == 1
            path.write_text(fields_source.replace(
                dispatch, dispatch.replace("if (false)", "if (true)", 1)))
            paths[name] = path
            lines = fields_source.splitlines()
            start = next(index for index, text in enumerate(lines, 1)
                         if text.strip().startswith(f"unsafe int {function}("))
            line = next(index for index, text in enumerate(lines, 1)
                        if index > start and "return *pointer + 1;" in text)
            error = (f"{path.name}:{line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n")
            cases[name] = [((), (70, "", error))]
        main(names=tuple(paths), cases=cases, fixture_paths=paths)

"""Resolve the C++ name GTI emits for an extern "C" link symbol."""

import re


def native_spelling(generated: str, symbol: str) -> str:
    """Return the identity generated C++ uses to declare and call `symbol`.

    GTI declares each extern "C" function under a private C++ identity and
    binds its link symbol with an assembler label or an MSVC alternate name,
    so the symbol never collides with a system header prototype. A symbol
    without such a binding is spelled as itself.
    """
    label = re.escape(symbol)
    match = re.search(r"(__gti_c_fn_\d+)\([^;]*?\)\s*__asm__\(\"_?" + label + r'"\)',
                      generated, re.S)
    if match is None:
        match = re.search(r"/alternatename:(__gti_c_fn_\d+)=" + label + r'"', generated)
    return match.group(1) if match else symbol

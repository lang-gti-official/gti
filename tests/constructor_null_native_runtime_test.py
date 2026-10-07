"""Nominal null field construction and isolated native C declarations."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = Path(__file__).resolve().parent
    sources = {
        "nullable_member": root / "fixtures/nullable_member_construction.gti",
        "unique_member": root / "coverage/reproducers/a36-unique-ptr-member-init-nullptr.gti",
        "native_strlen": root / "coverage/reproducers/a37-libc-strlen-prototype-conflict.gti",
        "native_qsort": root / "coverage/reproducers/a37-libc-qsort-link-name-conflict.gti",
    }
    main(names=tuple(sources), fixture_paths=sources,
         cases={"native_qsort": [((), (0, "1\n", ""))]})

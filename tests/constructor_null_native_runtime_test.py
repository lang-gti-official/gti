"""Nominal null field construction and isolated native C declarations."""
from pathlib import Path
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    root = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="gti-sentinel-negative-") as temporary:
        rejected = subprocess.run(
            [sys.argv[1], str(root / "fixtures/value_sentinel_noncopyable.gti"),
             "--emit-mir", "-o", str(Path(temporary) / "invalid.mir")],
            text=True, capture_output=True, timeout=30)
        if rejected.returncode != 65 or "error[GTI-S2029]" not in rejected.stderr:
            raise AssertionError((rejected.returncode, rejected.stdout, rejected.stderr))
        if (Path(temporary) / "invalid.mir").exists():
            raise AssertionError("noncopyable value endpoint must not publish MIR")
    sources = {
        "nullable_member": root / "fixtures/nullable_member_construction.gti",
        "unique_member": root / "coverage/reproducers/a36-unique-ptr-member-init-nullptr.gti",
        "native_strlen": root / "coverage/reproducers/a37-libc-strlen-prototype-conflict.gti",
        "native_qsort": root / "coverage/reproducers/a37-libc-qsort-link-name-conflict.gti",
        "value_sentinel": root / "fixtures/value_sentinel_range.gti",
        "reference_sentinel": root / "fixtures/reference_sentinel_noncopyable.gti",
        "nullable_example": root.parent / "examples/13-ownership.gti",
        "sentinel_example": root.parent / "examples/83-complete-ranges.gti",
    }
    main(names=tuple(sources), fixture_paths=sources,
         cases={"native_qsort": [((), (0, "1\n", ""))],
                "nullable_example": [((), (0, "ownership worked\nwidget dropped\n", ""))]})

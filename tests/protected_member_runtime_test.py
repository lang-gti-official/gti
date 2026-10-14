#!/usr/bin/env python3
"""Protected visibility uses the existing verified MIR execution paths."""
from pathlib import Path
from lambda_pack_invocation_runtime_test import main
if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    main(names=("protected_events",),
         fixture_paths={"protected_events": root / "examples/150-protected-events.gti"},
         cases={"protected_events": [((), (0, "protected events work\n", ""))]})

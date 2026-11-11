Removed the unreleased `GTI_LEGACY_BORROW_CHECKER` comparison switch and the
legacy-only semantic borrow checks behind it. MIR remains the only body-local
borrow checker; compiled programs are unchanged.

Speed up missing-import suggestions in `gti_lsp`. Repeated discoveries reuse
candidate interface scans while their source text and module registrations
are unchanged. A qualified name such as `api::value` no longer triggers a trial
compile of every standard-library module that declares `value`. A discovery
that took about 9 s now takes under 0.5 s, and suggestions are unchanged.

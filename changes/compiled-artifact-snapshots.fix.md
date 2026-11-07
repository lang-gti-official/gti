Project builds retain the exact compiled-library artifacts selected or produced
for their dependency graph. Replacing or deleting a selected cache file can no
longer silently switch a later consumer to a different library revision. Each
new build still checks source freshness and validates its selected artifacts.

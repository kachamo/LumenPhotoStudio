## Summary

<!-- What does this PR change, and why? -->

## Related issue(s)

<!-- e.g. Closes #123, relates to #456 -->

## Testing

<!-- How did you verify this? Manual steps, screenshots, sample files, etc. -->

## Checklist

- [ ] Builds on all three platforms (CI green) — Windows, Linux, macOS
- [ ] `--smoke-test` passes (CI runs this headlessly with `QT_QPA_PLATFORM=offscreen`)
- [ ] New/changed `#include` paths and `.qrc` entries use the correct on-disk case (the `static-hygiene` CI job checks this, but Windows-only local testing can miss it)
- [ ] No secrets, absolute local paths, or machine-specific config committed
- [ ] Updated relevant docs (`README.md`, `docs/`) if behavior or build steps changed
- [ ] Added/updated tests or a manual test plan for the change, where applicable

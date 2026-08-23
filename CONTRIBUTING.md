# Contributing to Lumen Photo Studio

Thanks for considering it. This project is early — see
[`docs/ROADMAP.md`](docs/ROADMAP.md) for the honest state of things and
[`docs/ROADMAP.md#4-good-first-issues`](docs/ROADMAP.md#4-good-first-issues)
for small, verified, well-scoped starting points if you're new here.

## Getting the code building

Build prerequisites and platform-specific steps live in
[`docs/BUILDING.md`](docs/BUILDING.md) — this file won't duplicate them.
Short version: CMake ≥ 3.21, a C++20 compiler, Qt 6.2+ (Core, Gui,
Concurrent, Widgets). LibRaw is optional and only gates RAW file support —
its absence is not a build failure, just a feature flag
(`LPS_HAS_LIBRAW`) that turns off.

## Code style

There's no `.clang-format` or `.editorconfig` in the repo yet, so match
the surrounding code by eye. Observed conventions, verified against the
existing source:

- **4-space indentation, no tabs.**
- **`namespace lps { ... }`** wraps everything in `src/`. UI code in `ui/`
  is not namespaced.
- **`#pragma once`**, not include guards, on every header.
- **`QStringLiteral("...")`** for every Qt string literal — not raw
  `QString("...")` or `"..."` where a `QString` is expected. This is used
  consistently across the entire codebase (`PluginManager.cpp`,
  `WhiteBalance.cpp`, every UI file, etc.) — it avoids a runtime UTF-16
  conversion for literals known at compile time. New code should follow
  it.
- **Banner comments** — every file opens with a `// ====...====` block
  giving the file path and a short description, and non-trivial functions
  often get a multi-line comment above them explaining *why*, not just
  *what*. This codebase leans heavily on comments-as-documentation (see
  `Vignette.cpp`, `WhiteBalance.cpp`, `Look.h` for representative
  examples) — new engine code should keep that up. A future contributor
  reading a diff six months from now is the audience.
- **Private members prefixed `m_`** (e.g. `m_pixels`, `m_lastError`,
  `m_undoStack`).
- **Static-method "engine" classes.** Most pipeline stages are a class
  with a single `static void apply(PixelBuffer&, const XParams&)` method
  and no instance state (see `WhiteBalance`, `Vignette`, `TransformEngine`,
  etc.). Follow this shape for new pipeline stages rather than introducing
  instance state unless you have a specific reason to.
- **`isIdentity()` / `clampRanges()` on every parameter struct.** Every
  `*Params` struct in `core/Look.h` implements both:
  - `isIdentity() const` — true at neutral defaults, used for fast-path
    skipping (an engine should check this first and return immediately
    without touching pixels if true).
  - `clampRanges()` — clamps every field to its documented range. Called
    once per render on a local copy of the whole `Look`
    (`ImagePipeline::render()` takes `Look` by value specifically so this
    mutation is safe and doesn't touch the caller's copy).

  New parameter structs must implement both, and the documented range for
  each field should be a comment next to the field declaration (see any
  existing `*Params` struct in `Look.h` for the pattern).

## Adding a field to `Look`

This is a **three-file change**, verified against how the existing (if
currently inert) Lift/Gamma/Gain/Offset fields are wired end-to-end:

1. **`src/core/Look.h`** — declare the field on the relevant `*Params`
   struct, with a range comment.
2. **`src/core/Look.cpp`** — fold it into that struct's `isIdentity()`
   (does the new field's default value make the struct identity?) and
   `clampRanges()` (clamp it to the documented range).
3. **`src/preset/LookSerializer.cpp`** — read and write it in the JSON
   (de)serializer, with a sane default for `readFloat`/equivalent so old
   `.lxp` files without the field still load cleanly.

Note that this three-file contract only guarantees the field
**round-trips through save/load/undo**. It does not mean an engine
actually *does* anything with the field yet — see `docs/ARCHITECTURE.md`
for which grading/layer fields currently persist without an engine
consumer. If you're adding a field that's meant to affect rendering, wire
it into the relevant engine's `apply()` too, and don't leave a slider
connected to nothing — see the roadmap's "good first issues" for examples
of exactly that problem (`ExportDialog`'s "Adobe RGB placeholder" entry,
the layers panel's blend-mode combo).

## Keeping comments honest

Some existing header comments in this codebase describe behavior that's
since changed (e.g. `LocalAdjustmentEngine.h` still describes brush masks
as an inert placeholder, when `MaskGeometry.h`'s `maskWeightBrush()` and
`PreviewWidget.cpp`'s brush painting are both fully implemented). If your
change makes an existing comment inaccurate, update it in the same PR.
Stale "this is a placeholder" comments are actively misleading to the next
contributor — worse than no comment.

## Pull request expectations

- **Keep PRs scoped to one thing.** A PR that fixes the vignette-roundness
  bug should not also refactor `MainWindow.cpp`.
- **If you touch an engine (`src/`), say what you tested.** There's no
  unit test suite yet, so a PR description should say how you verified
  the change (e.g. "loaded a RAW file, dragged the roundness slider from
  -100 to +100, confirmed the vignette shape changes and the pixel math
  at d=0 is still gain=1").
- **If you add a new engine stage or pipeline field, update
  `docs/ARCHITECTURE.md`'s pipeline diagram and module map** in the same
  PR — that document is only useful if it stays accurate.
- **If you fix or complete something described as a gap in
  `docs/ROADMAP.md`, update the roadmap in the same PR.** A roadmap that
  claims a gap after it's been closed is as bad as a feature table that
  overclaims.
- **Don't introduce a new third-party dependency without discussion
  first** — open an issue before the PR. `LibRaw` is the one existing
  precedent, and it's deliberately optional/detected-at-build-time rather
  than vendored, which is the bar for anything new.

## Running CI locally

You can't, honestly — there's no way to run the exact GitHub Actions
workflow (`.github/workflows/ci.yml`) on your machine bit-for-bit. What
you *can* do before opening a PR is the thing CI almost certainly checks:
configure and build clean with CMake for your platform per
[`docs/BUILDING.md`](docs/BUILDING.md), on both a from-scratch build
directory and (if you touched `CMakeLists.txt`) a from-scratch clone, and
fix any compiler warnings your change introduces (`-Wall -Wextra
-Wpedantic` on GCC/Clang, `/W4 /permissive-` on MSVC — see
`CMakeLists.txt`). If CI fails on something environment-specific you
can't reproduce locally, say so in the PR rather than guessing at a fix.

## License

By contributing, you agree your contribution will be licensed under the
GNU General Public License v3.0 (see [`LICENSE`](LICENSE) in the repository
root). New source files should carry the header template in
[`docs/LICENSE-HEADER.txt`](docs/LICENSE-HEADER.txt).

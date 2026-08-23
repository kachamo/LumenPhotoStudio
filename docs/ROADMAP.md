# Roadmap

This is a working roadmap, not a promise with dates. Effort sizes are rough
shirt sizes for a contributor who already knows the codebase:

- **S** — a few hours to a couple of days. Good first issue territory.
- **M** — roughly a week of focused work. Touches one module, no schema/architecture changes.
- **L** — multiple weeks. Touches architecture, a data schema, or several modules at once.
- **XL** — multiple months, or requires a design decision the maintainers need to make before anyone should start writing code.

No item below has a ship date attached to it. Dates are how roadmaps become
lies. Themes and "definition of done" are how they stay useful.

Everything in the gap analysis was verified against the current source tree
(`src/`, `ui/`, `CMakeLists.txt`) as of this writing, not against what the
engine's own comments claim — several of the engine's own header comments
are stale relative to what the code actually does (noted inline below).

---

## 1. Gap analysis vs. Lightroom Classic

Lightroom Classic is really two products glued together: a **catalog**
(Library module) and a **RAW/photo editor** (Develop module), plus a long
tail of modules (Map, Book, Slideshow, Print, Web) that most working
photographers touch rarely if ever. We size the gap against each.

### Library / Catalog — does not exist

This is the single biggest gap, and it is not close. Verified by grep and
by reading `src/project/ProjectDocument.h`: a "project" in Lumen today is
**one image + one `Look`** (`QString sourceImagePath; Look look;`). There is
no concept of a set of images. Concretely, none of the following exist
anywhere in `src/` or `ui/`:

- A catalog database of any kind (no SQLite, no embedded DB of any kind)
- An import workflow (copy/move/add, duplicate detection, import presets)
- A grid or filmstrip browser
- Thumbnail or preview caching/pyramids
- Star ratings, pick/reject flags, or color labels
- Keywords, keyword hierarchies
- Collections or smart collections
- Any search or filter (by date, camera, rating, keyword, folder…)
- Background/async indexing of a folder

The UI is honest about this today, which is worth noting: the "Library"
entry in the left navigation rail (`ui/MainWindow.cpp`, `handleRailAction`)
opens a message box that says **"This feature isn't implemented yet."**
That is the correct behavior for now — better than pretending — but it
means a photographer with 40,000 images cannot use Lumen as their primary
tool no matter how good Develop gets. They can only ever open one file at a
time from a system file picker. This is priority #1 for the next major
phase (see v1.5 below).

### Develop — the real strength, with real holes

The linear-light pipeline, tone/color/curve/grading engines, masking, crop,
and non-destructive editing model are genuinely solid and are the project's
best argument for existing. But claims about what's "done" need three
corrections to what you may have been told:

1. **Adjustment layers and blend modes are not real yet.** `Look.h` is
   explicit about this in its own comments (`AdjustmentLayer` docblock:
   "V1 scope — data and UI plumbing only... Rendering is a placeholder.").
   The UI has a full layers panel — opacity slider, an 11-mode blend-mode
   combo box (Multiply, Screen, Overlay, etc.), add/duplicate/delete — and
   it all persists correctly to `.lxp`/project files. But
   `ImagePipeline.cpp` never reads `look.adjustmentLayers` at all. Moving
   the blend-mode dropdown currently does nothing to the rendered image.
   This is the kind of gap that destroys trust fastest with a new user —
   a control that visibly does nothing is worse than a control that isn't
   there.
2. **DaVinci-style Lift/Gamma/Gain/Offset and the "filmic" grading
   controls are the same story** — real UI, real fields, real
   serialization (`LookSerializer.cpp` round-trips all nine fields), zero
   engine math. `ColorGrading.cpp` never reads `lift`, `gamma`, `gain`,
   `offset`, `filmicContrast`, `highlightRolloff`, `shadowLift`,
   `fadeBlacks`, or `colorSeparation`.
3. **Masks are further along than a skim of the code suggests.** The
   `LocalAdjustmentEngine.h` and `Look.h` comments both say brush masks are
   "a placeholder... treated as zero-weight everywhere." That comment is
   stale. `PreviewWidget.cpp` fully implements brush painting
   (`mousePressEvent`/`mouseMoveEvent` → `beginBrushStroke`, live stamping),
   and `MaskGeometry.h`'s `maskWeightBrush()` genuinely computes per-pixel
   weight from the painted strokes (flow, density, erase mode, feather).
   Brush, linear-gradient, and radial-gradient masks all work end-to-end.
   The comments just weren't updated after the feature landed — a small
   documentation-hygiene item, but worth fixing so the next contributor
   doesn't get misled the way this audit almost did.

Other real Develop gaps, in rough order of how much they matter to a
switching Lightroom user:

- **RAW is decoded to 8-bit sRGB and thrown into the pipeline.**
  `RawImageLoader::load()` hardcodes `output_bps = 8`, `output_color = 1`
  (sRGB), and `use_camera_wb = 1`. The `RawDevelopSettings` struct has
  fields for camera profile, white-balance mode, highlight recovery, and
  color space — none of them are read (`Q_UNUSED(settings)` at the top of
  the function, then only `demosaicQuality` is actually used further down).
  So even though the internal pipeline is float32 and can represent
  headroom, a RAW file's actual 12–14 stops of dynamic range are crushed to
  8 bits *before* they ever reach the linear-light buffer. This is a
  correctness bug dressed up as a feature: the UI can offer "camera
  profile" and "highlight recovery" controls today, and they will silently
  do nothing.
- **No camera-specific color rendering.** No DCP or ICC input profiles —
  every camera is treated identically after LibRaw's default demosaic/WB.
  This is the single biggest reason a working pro looks at a RAW workflow
  and says "that's not what my camera looks like."
- **No lens-correction profiles.** `LensCorrectionEngine` has real
  vignetting compensation, but distortion, chromatic aberration, and
  purple/green fringe are explicit no-ops (values persist, pixels
  untouched — the header comments are honest about this one). There is no
  Lensfun (or any) lens-profile database, so even when the distortion math
  lands, there's no per-lens data to drive it.
- **No perspective/geometry correction** ("Upright" in Lightroom). Grepped
  for `perspective`/`keystone` — nothing exists. `TransformEngine` does
  crop, rotate, flip, and straighten (all real, all working), but no
  keystone/perspective warp.
- **No color management beyond "assume sRGB."** The pipeline is internally
  linear-light float, which is excellent, but there's no ICC profile
  awareness on ingest or output, no wide-gamut working space, no
  soft-proofing. `ExportDialog` literally has a combo box entry labeled
  **"Adobe RGB placeholder"** — it's selectable and does nothing.
- **Export is 8-bit only.** PNG/JPEG/TIFF via `QImage`-backed `ARGB32`.
  No 16-bit TIFF/PSD output, which matters to anyone round-tripping into
  Photoshop or a printer workflow.
- **Clarity is a midtone-contrast proxy, not unsharp-mask** — the code's
  own comment says so, and it's accurate. Note this is *separate* from
  Details/Sharpening (`DetailsEngine.cpp`), which **is** a real
  unsharp-mask implementation with edge masking and luminance/color noise
  reduction — that part is further along than the old README implied.
- **White balance is per-channel multiplication, not chromatic
  adaptation.** Matches Lightroom's slider *feel* but isn't a von
  Kries/Bradford transform. Documented honestly in the code's own
  comments.
- **`.cube` LUT support stops at combined shaper+3D LUTs** — plain 1D and
  3D are supported; shaper-then-3D is not (`LUTLoader.h` says so).
- **The node graph is read-only.** `NodeGraphWidget.h` says plainly: "The
  graph is purely informational. It does NOT drive rendering." It's a nice
  diagnostic view of the fixed pipeline order, not a real node-based
  editor. Don't market it as one.
- **The plugin system is an installer shell with no runtime.**
  `PluginManager` can scan a folder, validate `plugin.json` manifests, and
  copy plugin folders/zips into place — but grepping the whole codebase for
  `QPluginLoader`/`QLibrary`/`dlopen` turns up nothing. Nothing ever
  *executes* a plugin. `readPackagedPlugin()` even says so directly:
  "ZIP package installed; extraction is not enabled yet." Today this is a
  file manager for a folder, not a plugin system.

### Map — do not build

Lightroom's Map module shows geotagged photos on a map. It's a nice-to-have
for travel photographers and nobody else. Building it means picking a map
tile provider (licensing/cost), building GPS-EXIF extraction and reverse
geocoding, and a whole map-canvas widget — for a feature most working
photographers never open. Skip indefinitely.

### People (face detection/recognition) — do not build from scratch, not yet

High effort (a real face-detection + clustering + re-identification
pipeline is an ML project on its own), privacy-sensitive by nature (faces
are biometric data — this needs real consent/opt-in design, not a bolt-on),
and it is worthless without a catalog to attach the tags to. This is
explicitly a "catalog must exist and prove itself first" feature, and even
then, the right move is likely integrating an existing open model (e.g. via
an optional plugin) rather than building face recognition in-house. Do not
schedule this until the Catalog phase has shipped and has real users.

### Print — do not build

A desktop print-layout module (multi-photo contact sheets, print package
layouts, printer color management) is a lot of surface area for a feature
most photographers have replaced with either a lab's own upload tool or
just printing a single exported JPEG. Skip.

### Web (gallery export) — do not build

Even Lightroom itself has deprioritized this module for years in favor of
Lightroom cloud sharing. Static HTML gallery export was a 2010s feature.
Skip.

### Cross-cutting concerns

- **Performance/GPU.** The entire pipeline is CPU-only: `ScanlineParallel.h`
  parallelizes rows via `QtConcurrent::blockingMap`, nothing else. Grepped
  for `QRhi`/`QOpenGL`/`Vulkan`/`OpenCL`/`CUDA`/`Metal` — zero hits anywhere
  in `src/` or `ui/`. This works fine for a ~2 MP preview buffer (the
  identity fast-path and LUT fusion are genuinely good engineering), but a
  24–45 MP full-res interactive edit on a CPU scanline pipeline will not
  feel interactive on real hardware. See v2.0 below.
- **Color management.** Covered above under Develop — no ICC awareness
  anywhere in the pipeline.
- **RAW fidelity.** Covered above — this and the catalog are the two
  credibility gaps that decide whether a professional takes this project
  seriously.
- **Metadata reading.** `ImageMetadataReader` reads EXIF for non-RAW files
  via `QImageReader::textKeys()`/`text()`, which is populated inconsistently
  across Qt's image plugins and platforms (it's not a dedicated EXIF
  parser like Exiv2). RAW metadata via LibRaw is solid. Worth a real EXIF
  library at some point rather than depending on whatever Qt's JPEG/TIFF
  plugin happens to expose as "text."

---

## 2. Phased milestones

### v1.2 — "Make Develop tell the truth"

**Theme:** close the gap between what the UI promises and what the engine
actually renders, and fix the RAW pipeline's bit-depth problem. No new
modules — this phase is about removing dead controls and finishing
half-built ones so the existing Develop module is fully trustworthy.

| Item | Effort | Why a Lightroom user cares |
|---|---|---|
| RAW pipeline: stop hardcoding 8-bit output; carry real bit depth (16-bit) from LibRaw into `PixelBuffer` | L | This is the actual "does my RAW file look right" fix. Everything else in RAW fidelity is built on top of this. |
| Wire `RawDevelopSettings` (white balance mode, highlight recovery, color space) into `RawImageLoader::load()` instead of ignoring them | M | Right now the RAW import dialog can offer controls that silently do nothing. Either make them work or remove them — both are better than the current state. |
| Camera-specific color rendering: DCP or ICC input profile support for common camera makes | XL | The #1 reason a working pro compares RAW output against Lightroom and walks away. Needs a profile format decision (DCP is what Adobe/most tools ship; ICC is simpler to support but less common for camera profiles) before implementation starts. |
| Lens-correction profiles via Lensfun (distortion + CA, replacing the current no-ops) | L | Distortion and CA correction are common, expected sliders; right now they're wired up to nothing. |
| Perspective/keystone correction ("Upright"-equivalent) | L | Architecture shots, buildings, real-estate photography — common enough that its total absence is noticeable. |
| Implement real compositing for `AdjustmentLayers`/`BlendMode`, or remove/hide the panel until it's real | L (implement) / S (hide) | A blend-mode dropdown that does nothing is worse for trust than no dropdown. Pick one. |
| Implement Lift/Gamma/Gain/Offset + filmic grading engine math, or remove/hide those controls | M (implement) / S (hide) | Same principle — dead sliders erode trust in every other slider. |
| 16-bit TIFF export | M | Anyone round-tripping into Photoshop or a print workflow needs more than 8-bit output. |
| Real color-managed export (sRGB / Adobe RGB / ProPhoto RGB, not a placeholder label) | L | The export dialog currently offers a choice that does nothing for two of its three options. |
| Bradford chromatic adaptation for white balance (replace per-channel multiply) | M | Mostly matters at extreme WB corrections; lower priority than the items above but a real accuracy gap. |
| True unsharp-mask option for Clarity (currently a midtone-contrast proxy) | M | Cosmetic vs. Details/Sharpening's real unsharp-mask; lower priority since Sharpening already covers the real use case. |

**Definition of done:** every visible Develop control in the UI either
changes the rendered pixels or is removed/hidden. A RAW file from a
current-generation Canon/Nikon/Sony/Fuji body, opened with default
settings, is recognizably in the right ballpark against that camera's
manufacturer-default rendering (not pixel-identical to Lightroom — that's
not a realistic bar — but not obviously wrong).

### v1.5 — "Catalog MVP" (centerpiece of the next major phase)

**Theme:** Lumen becomes usable as a primary tool for someone with a real
photo library, not just a one-file-at-a-time editor. This is the phase
that decides whether the project is a "RAW converter" or a "Lightroom
alternative."

This phase forces real architectural decisions up front, before feature
work starts:

- **Catalog schema (SQLite).** Recommend SQLite specifically — it's the
  same choice Lightroom, darktable, and digiKam all made, it's
  public-domain, ships as a single file, and Qt has first-class support
  (`QSqlDatabase` with the `QSQLITE` driver) with no new third-party
  dependency. Rough schema shape: `images` (path, folder id, capture
  metadata, import date, current preview hash), `folders` (watched roots),
  `looks` (the existing `Look` JSON, one per image, versioned), `keywords`
  + `image_keywords` (many-to-many), `collections` +
  `collection_images` (many-to-many, ordered), `ratings_flags` (star
  rating, pick/reject, color label per image). Keep the schema additive —
  new columns/tables should not require a destructive migration, mirroring
  the `.lxp` "missing fields default to identity" philosophy that already
  exists for `Look`.
- **Thumbnail/preview cache pyramid.** At minimum two tiers on disk: a
  small grid thumbnail (fast, generated on import) and a larger
  screen-fit preview (generated on first view or as a background job,
  regenerated when the `Look` changes). Store as files on disk keyed by
  image id + a content/edit hash, not blobs in SQLite — matches how
  Lightroom/darktable do it and keeps the DB small and fast to back up.
- **Background indexing thread.** Import/scan must not block the UI
  thread. A worker thread (or `QThreadPool` job queue) walks a folder,
  reads metadata, and generates the small thumbnail tier, posting results
  back to the UI incrementally — the grid should start populating within
  a second or two even for a folder of thousands of images, not block
  until the whole scan finishes.
- **XMP sidecar read/write.** This is a trust-building decision, not just
  a technical one: if Lumen can read and write standard XMP sidecars
  (ratings, keywords, and ideally develop settings in a form darktable/LR
  can partially interpret), a photographer can try Lumen on a folder
  without burning the bridge back to their existing tool. This is the
  single highest-leverage adoption feature in this phase — it turns
  "risky bet" into "low-risk trial."

| Item | Effort | Why a Lightroom user cares |
|---|---|---|
| SQLite catalog schema + migration scaffolding | L | Foundation for everything else in this phase. |
| Import workflow (scan a folder, add without moving files — copy/move can come later) | M | The most basic "get my photos into the tool" step; currently there isn't one. |
| Background indexing thread + progress UI | M | 40,000 images cannot block the main thread on import; this is table stakes, not a nice-to-have. |
| Thumbnail + preview cache pyramid on disk | L | Grid browsing is unusable without cached thumbnails; regenerating full-res previews per grid cell is a non-starter. |
| Grid/filmstrip browser view | L | This is *the* missing UI surface. Without it, Develop-module quality doesn't matter to someone with a real library. |
| Star ratings, pick/reject flags, color labels | M | Baseline culling workflow every working photographer uses on every shoot. |
| Keywords (flat, no hierarchy yet) | M | Second most common organization primitive after folders. |
| Collections (manual, non-smart) | M | Lets a user group images across folders (e.g., "Sarah & Tom wedding — selects"). |
| Search/filter (by rating, flag, keyword, camera, date range) | M | Without this, ratings/keywords/collections are collected but unusable at scale. |
| XMP sidecar read/write (ratings, keywords, and Look data where mappable) | L | The adoption unlock — try Lumen without a one-way door. |
| Smart collections (rule-based) | M | Real value, but defer to after manual collections prove the data model — don't build the query engine before the schema is validated by real use. |

**Definition of done:** a user can point Lumen at a folder with 5,000+
images, get a responsive (not "frozen for two minutes") grid within
seconds, rate/flag/keyword/collect images, filter/search across them, and
close/reopen the app without losing any of that state. Edits made in Lumen
are visible as XMP metadata to darktable or Lightroom opening the same
folder (full develop-setting interop is a stretch goal for this phase —
ratings/keywords/labels round-tripping is the bar).

### v2.0 — "Performance and GPU"

**Theme:** make full-resolution interactive editing actually feel
interactive on real 24–45 MP files, and close the remaining Develop
polish gaps.

GPU acceleration option space for a Qt 6 desktop app, with tradeoffs:

- **QRhi compute shaders.** Qt 6's own hardware abstraction layer sits on
  top of D3D11/D3D12, Vulkan, Metal, and OpenGL depending on platform.
  *Pro:* one codebase, no new third-party dependency (already ships with
  Qt), and it's the "native Qt way" — future Qt releases will keep
  investing here. *Con:* QRhi's compute-shader support is newer and less
  battle-tested than its graphics-pipeline support; debugging tooling
  across three backends is more fragmented than a single-API approach.
- **Direct Vulkan/Metal/D3D per-platform.** *Pro:* maximum control and
  performance ceiling, best profiling tooling per platform. *Con:* three
  separate backends to write and maintain — this is a much bigger
  ongoing maintenance cost for an open-source project without a large
  contributor base, and it's the opposite of what makes a scanline CPU
  pipeline easy to review and extend today.
- **OpenCL.** *Pro:* one API across vendors and platforms (including
  older/integrated GPUs), historically the choice for darktable's GPU
  path. *Con:* macOS deprecated OpenCL years ago (still works today but
  Apple has explicitly not invested in it since Metal), so this is a
  gradually sinking option for a project that wants first-class macOS
  support long-term.

**Recommendation:** QRhi compute, specifically because it avoids adding a
new dependency and keeps one codebase, with the existing CPU scanline path
kept permanently as the fallback (older/integrated GPUs, and as the
reference implementation for correctness — CPU and GPU paths should be
checked against each other rather than the CPU path being deleted).

| Item | Effort | Why a Lightroom user cares |
|---|---|---|
| GPU-accelerated preview pipeline (QRhi compute), CPU path kept as fallback | XL | The difference between "slider drag feels instant" and "slider drag feels laggy" is the difference between a tool people enjoy using and one they tolerate. |
| GPU-accelerated full-res export path | L (once preview path exists) | Export time on a 45 MP file matters when you're processing a wedding shoot. |
| Smart collections, saved searches | M | Deferred from v1.5; now the catalog data model has real usage to validate the query design against. |
| Plugin execution runtime (actually load/run "adjustment" and "export" plugin types `PluginManager` already defines) | XL | Turns the plugin *installer* that exists today into an actual plugin *system* — this is what lets a community build presets/LUT packs/export targets without waiting on core maintainers. |

---

## 3. What NOT to build, and why

Good roadmaps are defined by exclusions as much as inclusions. These are
plausible-sounding ideas that would burn a disproportionate amount of a
small open-source project's limited attention:

- **Cloud sync.** Running a hosted sync service is an ongoing operational
  and financial commitment (servers, uptime, abuse handling, data
  retention/privacy obligations) that is a fundamentally different kind of
  project from "build a good desktop editor." It also cuts against a
  likely reason people choose an open-source Lightroom alternative in the
  first place — not wanting their photo library tied to someone else's
  subscription/cloud. If sync is ever pursued, the honest version is
  "point it at a folder the user already syncs with their own
  Dropbox/Syncthing/NAS," not building and operating a proprietary sync
  backend.
- **Mobile apps.** A companion mobile app is a second, mostly-separate
  codebase, a second UI/UX design language, and a second set of platform
  release processes (App Store/Play Store review, mobile-specific RAW
  decode constraints). It does nothing for the credibility gaps
  (catalog, RAW fidelity) that actually decide adoption today. Revisit
  only after the desktop product has real usage and a mobile companion
  view (not full editor) would clearly serve that existing user base.
- **AI subject/sky/person masking built from scratch.** Training or even
  fine-tuning a segmentation model is a multi-month ML effort with ongoing
  retraining/maintenance costs, and it's an area where a small OSS project
  cannot compete with what Adobe (or an off-the-shelf open model) already
  does well. If this is wanted eventually, the right shape is integrating
  an existing open model (e.g., via an optional, separately-installed
  dependency or a plugin once the plugin runtime exists in v2.0) rather
  than building segmentation research in-house. Do not schedule
  from-scratch model training.
- **Map, People, Print, Web modules** — see the gap analysis above. Each
  is real Lightroom surface area, and each is a poor use of a small
  project's effort relative to the catalog and RAW-fidelity gaps that
  actually block adoption.
- **A real node-based compositing editor** (evolving `NodeGraphWidget`
  from a read-only diagram into an editable DAG that drives rendering).
  Interesting, but it's a fundamental re-architecture of a currently
  fixed, well-understood, easy-to-reason-about pipeline, for a feature
  aimed at a small minority of power users (Resolve/Nuke-style
  compositors). Revisit only after the catalog and RAW-fidelity gaps are
  closed and there's a specific, requested use case driving it.

---

## 4. Good first issues

These are genuinely small, scoped, verified-against-the-code tasks. Good
entry points for a new contributor who wants to ship something real
without needing architectural context first.

1. **Vignette roundness does nothing.** `src/effects/Vignette.cpp`'s
   `apply()` computes `d` from a plain circular distance
   (`dx*dx + dy*dy`) and never reads `params.roundness` — the field exists
   on `VignetteParams` in `Look.h`, is serialized, and has a UI slider, but
   the engine ignores it entirely (the file's own comment says
   "Roundness not yet implemented"). Fix: scale `dx`/`dy` asymmetrically
   based on `roundness` before computing `d`, matching Lightroom's
   round-vs-elliptical vignette behavior. Self-contained to one function,
   one file. **(S)**
2. **`ExportDialog`'s "Adobe RGB placeholder" combo entry.** In
   `ui/ExportDialog.cpp`, the color space combo box has a literal item
   labeled `"Adobe RGB placeholder"` that's selectable and does nothing on
   export. Either disable/gray it out with a tooltip explaining it's not
   implemented yet, or remove it until real color-managed export lands.
   Either fix is a one-file UI change. **(S)**
3. **`PluginManager::readPackagedPlugin` never extracts ZIP packages.**
   `src/plugins/PluginManager.cpp` copies a `.zip` file into the plugins
   folder but always reports `error = "ZIP package installed; extraction
   is not enabled yet."` and `valid = false`. Wiring in a ZIP extraction
   library (Qt doesn't ship one; would need something like KArchive,
   miniz, or QuaZip as a new optional dependency) would make installed
   packages actually usable. **(M)** — bigger than the others because it
   likely needs a new dependency decision first; worth filing as an issue
   for discussion before a PR.
4. **`RawDevelopSettings` fields are accepted but ignored.** In
   `src/io/RawImageLoader.cpp`, `load()` takes a `RawDevelopSettings` with
   `cameraProfile`, `whiteBalanceMode`, `highlightRecovery`, and
   `colorSpace`, marks the whole struct `Q_UNUSED`, and only actually
   reads `demosaicQuality` further down. A first step: wire
   `whiteBalanceMode` (e.g. "As Shot" vs "Auto" vs a manual Kelvin value)
   into LibRaw's `use_camera_wb`/`use_auto_wb`/`user_mul` fields — this is
   a contained, well-scoped piece of the larger v1.2 RAW item above.
   **(S–M)**
5. **Stale comments describing brush masks as non-functional.**
   `src/local/LocalAdjustmentEngine.h` and the `LocalAdjustment` docblock
   in `src/core/Look.h` both describe brush masks as "a placeholder"
   that's "treated as zero-weight everywhere." This is no longer true —
   `MaskGeometry.h`'s `maskWeightBrush()` and `PreviewWidget.cpp`'s brush
   painting are fully implemented. Pure documentation fix: update both
   comment blocks to describe what the code actually does. Zero risk,
   genuinely useful for the next person reading this code. **(S)**
6. **"Library" nav rail entry pops a bare message box.**
   `MainWindow::handleRailAction()` shows a generic
   `QMessageBox::information` ("This feature isn't implemented yet.") when
   the Library rail icon is clicked. Until the v1.5 catalog work lands,
   consider disabling/graying that rail entry with a tooltip instead of
   presenting it as a clickable, functioning nav item. Small UX-polish
   fix, one function. **(S)**

# Profiling hooks (cook tracing)

**Status:** Implemented 2026-09-30 (owner: hooks into the host's profiler, not a trace system of
kiln's own; whole load in scope; `CookStats` unchanged).
**Decides:** how kiln reports where the time of a load and of a cook goes.

## Why

A cold start of `kiln-gl` took 8.7 s. Finding where the time went took temporary logging in four
files. It turned out to be two separate things: 2.4 s of BC encoding capped at 3 threads, and a 5 s
driver stall on the pump thread. `CookStats` (per-stage times of one cook, `kiln-cook -v`) shows
neither the waits between stages nor the work on other threads.

## Shape

A host that profiles a shipping build already runs a profiler (Tracy, Superluminal, PIX, Perfetto).
It wants kiln's work in *that* profiler, keyed by asset, not a second trace file. So the library
only calls hooks (`kiln/profile.h`); writing a trace is a tool's job.

- `ProfileHooks { zone_begin, zone_end, interval, user }`, all optional. `ContextDesc::profiler` for
  the runtime (the cook provider takes the context's hooks), `CookEnv::profile` for a direct cook.
- **Zones** begin and end on one thread and nest: they map onto Tracy or Superluminal zones.
- **Intervals** are reported once, when finished, with two `profile_now_ns()` readings that may
  come from different threads: queue waits, the GPU copy, a whole load. A profiler shows them as
  async spans (Chrome `b`/`e`, Perfetto async tracks, Tracy messages or plots).
- `name` is a static string; `asset` is the asset name, valid during the call.
- Off by default: a null hook costs one branch. kiln allocates nothing for profiling.

## Events

| Name | Kind | Thread | What |
|---|---|---|---|
| `kiln.pump` | zone | pump | one `pump()` |
| `kiln.wait.meta`, `kiln.wait.upload` | interval | — | in kiln's queue, from queueing to submission (IO job limit, upload budget) |
| `kiln.wait.pool` | interval | — | submitted to the job system, not started yet (pool latency) |
| `kiln.meta`, `kiln.upload` | zone | worker | the meta job (read the header, or cook on a miss) and the upload job (read, Zstd decode, write staging) |
| `kiln.prepare` | zone | worker | the cook provider's `prepare` inside a meta job: a store check, or a cook on a miss |
| `kiln.gpu` | interval | — | from the upload's commit to the pump seeing it complete |
| `kiln.load` | interval | — | a load attempt, from its request (or reload) to Ready |
| `cook.unit` | zone | worker / main | one source file's cook |
| `cook.texture` › `cook.decode`, `cook.prepare`, `cook.mips`, `cook.encode`, `cook.write` | zones | | a texture's stages; `cook.write` includes Zstd |
| `cook.mesh` › `cook.import`, `cook.build`, `cook.pack`, `cook.write` | zones | | a mesh's stages; `cook.build` and `cook.pack` run once per job chunk |

## Tools

- `tools/trace_writer.h` (`cli::TraceWriter`): hooks that record every event and write a Chrome trace
  (`chrome://tracing`, ui.perfetto.dev) plus a summary per name to the log.
- `kiln-cook --trace <file>`.
- The examples (`kiln-gl*`, `kiln-vk-*`, `kiln-sokol`, `kiln-nga`, `kiln-headless`): environment
  variable `KILN_TRACE=<file>`. The examples read it; the library never does.

Forwarding to Tracy takes a few lines in the host: `zone_begin` → `___tracy_emit_zone_begin` with a
static source location per name, `zone_end` → `___tracy_emit_zone_end`, `interval` → a message or a
plot. kiln does not depend on Tracy.

## First findings (2026-09-30, i7-9700K, release)

`kiln-cook --trace` on WaterBottle at `Fast`, after the `bc7f` change: `cook.decode` 144 ms for four
2048² PNGs (up to 52 ms each, one thread), `cook.write` 78 ms, `cook.encode` 45 ms. The four images
of one glTF cook one after another inside one `cook.unit`. A cold `kiln-gl` showed `kiln.upload` up
to 100 ms per texture. Both are fixed:

- A glTF's images now cook in parallel: WaterBottle at `Fast` on all cores, 336 → 157 ms.
- The upload job decoded Zstd straight into the adapter's staging memory. That memory is
  write-combined on GL and Vulkan, and Zstd reads its output back for its matches. Decoding into
  the job's scratch buffer and copying cut a warm `kiln-gl` run's six uploads from 276 to 26 ms
  (Zstd 272 → 16 ms, plus 2 ms of copying); `kiln-vk-basic` 280 → 28 ms.

Parallel PNG decode is left: wuffs decodes one PNG as one stream.

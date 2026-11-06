# Performance optimizations (`perfopt` branch)

This fork of [MVE](https://github.com/simonfuhrmann/mve) adds a modern build
for the command-line tools and performance optimizations. The work lives on
the `perfopt` branch (the default branch of this fork); `master` tracks the
upstream repository, so it can be synchronized with it.

Timings below were measured on a 32-thread Windows machine (Clang 22) with
sample scenes of 11 views.

## Build and CI

- CMake, Ninja, Clang and vcpkg build the command-line applications (13 of
  them) and the unit tests. UMVE (the Qt GUI) and `libs/ogl` are not built.
  Presets: `windows-clang`, `linux-clang`, `macos-clang`.
- The baseline is plain x86-64 (no SSE4/AVX requirement). SIMD code uses
  [Google Highway](https://github.com/google/highway) with runtime dispatch.
- GitHub Actions builds and tests on Windows, Linux and macOS. Pushing a tag
  packages the tools (with the OpenMP runtime) and publishes a GitHub release.
- CMake options: `MVE_USE_MIMALLOC` (default ON), `MVE_USE_HIGHWAY`
  (default ON), `MVE_UNSAFE_MATH` (default ON as in the Makefiles; OFF uses
  `-ffp-contract=off`, which makes results reproducible so that refactorings
  can be verified bit-exactly).

## dmrecon (multi-view stereo)

- **mimalloc** for all C++ allocations: `PatchSampler` allocated per-patch
  vectors and maps from many threads, and about 19% of the CPU time went into
  the Windows heap. dmrecon is 16-25% faster, with bit-identical depth maps.
- **`PatchSampler`/`PatchOptimization`**: per-view `std::map` buffers became
  vectors indexed by view ID, scratch buffers are reused instead of being
  reallocated, texels are loaded once per channel, and an inline floor
  replaces a libm call. Single-threaded dmrecon is about 34% faster
  (9.9 s to 6.5 s on views 0-1 of a test scene), bit-identical with
  `-DMVE_UNSAFE_MATH=OFF`.
- **Parallel patch optimization within a view**: the queue is processed in
  batches. Up to `--batch-size` (default 32) of the best entries are
  optimized concurrently and the results are applied in queue order, so the
  output is deterministic and independent of the thread count. A batch size of
  1 gives the original sequential behavior bit-exactly. New options:
  `--batch-size`, `--threads`, `--parallel-views` (the threads are split
  between concurrent views and threads within a view). Single-view runs are
  about 4.4x faster. The depth maps differ slightly from the sequential result
  (median relative depth difference about 3e-5, fill rate +0.4%).

## fssrecon (floating scale surface reconstruction)

- `Octree::influence_query` recurses on node pointers, culls with squared
  distances (no `sqrt`) and uses the largest sample scale of each subtree for a
  tighter bound. The set of returned samples is unchanged.
- `IsoOctree` counts voxels atomically and one thread prints the progress,
  instead of an OpenMP critical section per voxel.
- The basis and weight functions and the color weights are evaluated with a
  Highway kernel. `FSSR_SCALAR=1` selects the scalar path and
  `FSSR_HWY_TARGET` limits the SIMD target.
- On a test scene, fssrecon went from 43 s to 19 s.

## Other changes

- `math/functions.h`: `_mm_popcnt` is only used with real MSVC, so plain
  x86-64 builds with Clang work.
- `ba_conjugate_gradient.h`: a null preconditioner is no longer dereferenced.
- Windows builds link the dynamic MSVC runtime (the libraries stay static),
  which saves about 1 MB per executable.

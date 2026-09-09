# IntelGfx

IntelGfx is an experimental Intel display and 3D graphics package for Haiku
x86_64. Its first hardware targets are Skylake and Kaby Lake integrated GPUs,
including HD Graphics 530 and HD Graphics 610.

The package contains a kernel rendering ABI, a Mesa 22.0.5 Iris Gallium
renderer named `Intel Gallium`, Intel Vulkan ANV with a native Haiku WSI,
diagnostics, and an updated OpenGL cube. It is the single source and package
for Haiku's `intel_extreme` kernel driver and display accelerant.

The implementation now provides the pieces Iris needs:

* one GPU address space per open device, with four-level PPGTT page tables;
* up to 16 independent logical render contexts per client;
* fixed virtual buffer bindings and atomic validation of multi-buffer submits;
* synchronous render submission with waitable userspace fence values;
* CPU/GPU cache maintenance, tiling metadata, and Gen9 PAT programming;
* Gen9 render power state, engine, context, and nonprivileged-register
  workarounds based on the i915 programming model;
* timeout quarantine, so memory reachable by a wedged GPU is retained until
  reboot instead of being freed underneath DMA;
* a Haiku HGL renderer that drives Mesa Iris and presents completed frames to
  `BGLView`.

This has run on the target hardware. On a ThinkPad P50's Skylake GT2
(8086:191b) the blitter and render engines both pass `submit-test`, the native
ABI passes `native-test`, and `intel_gfx_cube` draws with
`Mesa Intel(R) HD Graphics 530 (SKL GT2) (Iris / Haiku IntelGfx)` for thousands
of frames without faulting. It renders at roughly 1050 frames per second, and
at 59.7 with the cube's vertical sync box ticked, which is the panel. It began
at 43.

The processor is no longer what limits it. `intel_gfx_monitor` shows the GPU at
100% and the processor under 10% while the cube runs, which is the other way
round from where this started: the submission path used to write back every
byte of every buffer a batch named, twice per batch, and on a part whose GPU
shares the last level cache none of that was needed.

The GPU's frequency was the other half. Firmware leaves the software frequency
request at 100 MHz, below the 350 MHz the part reports as its own minimum, and
nothing on a Haiku system writes it; the driver now asks for the fastest
frequency the part reports, which on this one is 1050 MHz, and it runs at 1000.
That took the cube from 235 to about 1050 frames per second.

There is no governor behind that: the request is made once, at device
initialisation, and never lowered, so the GPU runs fast whenever it runs at all
rather than in proportion to what is being asked of it. What keeps that from
costing idle power is that RC6 still works, which is worth checking after any
change here: the render domain's residency counter should advance at real time
while nothing is drawing, and the monitor should say the GPU is idle.

What is left is that every submission waits for the GPU before the ioctl
returns, and every completed frame is read back and blitted by the processor,
so drawing never overlaps submission.

The shader topology comes from the driver rather than from registers the client
reads itself. The fuse registers live in a forcewake domain and answer zero
while the GPU is powered down, which is where a process that has not drawn yet
finds it, so reading them from userspace failed intermittently and took the
whole renderer down with it. The driver reads them once, during engine
initialisation, while it is already holding the domain awake. `register` is
still a raw read and is still subject to that: a GT register read while the GPU
is idle reads zero, which is why the monitor reports an idle GPU rather than
a frequency of nothing.

Still use `native-test` before the cube on a new machine, and retain the syslog
if the engine times out.

## Build

Use a configured Haiku x86_64 cross-build. Packaging also builds the Iris
renderer, downloading the exact Mesa 22.0.5 archive and checking its SHA-256.
The build expects the matching haikuports checkout beside this Haiku checkout
for Haiku's Mesa patchset.

```sh
python3 intel_gfx/build.py --haiku-build generated.x86_64 --tests --package
```

The command builds only the requested IntelGfx Jam targets and their
dependencies; it does not rebuild an image or modify the stock driver. Mesa's
standalone renderer can be rebuilt with:

```sh
python3 intel_gfx/mesa/build.py --haiku-build generated.x86_64 --clean
```

Outputs are placed below `intel_gfx/out/`. The package is
`intel_gfx-0.3.2-1-x86_64.hpkg`; the stripped renderer is also available as
`out/mesa/Intel Gallium`.

## Install

When built as the `intel_gfx` submodule of Haiku, `intel_gfx.hpkg` is included
in x86_64 images and installed by default. It owns the canonical
`intel_extreme` driver, `intel_extreme.accelerant`, Iris OpenGL renderer, and
ANV Vulkan ICD; no activation step or user override is used. The HPKG places
the Iris add-on in the system non-packaged search tier because Haiku's current
OpenGL roster otherwise selects Software Pipe before a renderer supplied by a
later package. The file is still package-owned and disappears on uninstall.

For a standalone test build, copy the HPKG to `/boot/system/packages/` and
reboot:

```sh
pkgman install ./intel_gfx-0.3.4-1-x86_64.hpkg
reboot
```

## Test

Find the actual device path first; the example path is not universal:

```sh
intel_gfx_ctl list
intel_gfx_ctl info /dev/graphics/intel_extreme_000200
intel_gfx_ctl buffer-test /dev/graphics/intel_extreme_000200
intel_gfx_ctl gtt-test /dev/graphics/intel_extreme_000200
intel_gfx_ctl native-test /dev/graphics/intel_extreme_000200
intel_gfx_cube --require-hardware --frames 600
```

The packaged `IntelGfx` discovery service is registered with Haiku's system
launch daemon and probes `/dev/graphics` automatically during boot. No
`intel_gfx_ctl` command is required to enable the driver; the tool only reports
state and runs diagnostics. The Iris binary stays in the package-owned add-on
directory. At boot the service registers it in Haiku's higher-priority system
non-packaged renderer tier, ensuring it is tried before base Mesa llvmpipe, and
removes that registration when the service exits.

`native-test` is the first hardware gate. It creates a private PPGTT, a logical
render context, two fixed virtual bindings, and a multi-object render batch. It
then checks the fence and the value written by the GPU. A timeout faults the
physical render scheduler and quarantines that client's mappings; reboot
before trying again.

The cube prints the GL vendor, version, and renderer, measures completed frames
with `glFinish`, and reads pixels from its first frame to catch a renderer that
submits without drawing. `--require-hardware` exits with failure unless the
renderer identifies itself as `Iris / Haiku IntelGfx`; `--frames N` makes a
bounded test suitable for a terminal or script.

`intel_gfx_monitor` graphs what the GPU and the processor are each doing on
one time base, sampling five times a second. The GPU line is derived from the
render domain's RC6 residency counter and is labelled with the frequency the
hardware reports, so it covers everything the device does rather than only this
driver's share; the CPU line is the average across cores, with a bar per core
beneath it so that one saturated core is visible when the average is not.
`INTEL_GFX_MONITOR_TRACE=1` makes it say which device it opened and what its
counters read, which distinguishes an idle GPU from one it could not reach.
Reading the two together is what tells a renderer short of GPU from one that is
waiting on a processor. The GPU figure can read high for a few seconds after a
client exits before the device settles back into RC6.

The older tests remain useful:

* `buffer-test` checks allocation bounds, zeroing, per-open handles, and cloned
  area lifetime without using the GPU.
* `gtt-test` checks legacy global-GTT allocation and its 64 MiB client budget.
* `submit-test DEVICE` checks the working blitter command path.
* `submit-test DEVICE render` checks the older single-buffer render path.
* `engine-status DEVICE render` prints ring, context-save, fence, and context
  status-buffer state after a failure.

An ordinary VirtIO VM can test installation, activation bookkeeping, the
memory fixture, and the service's no-device path. Intel hardware or PCI
passthrough is required for the native render test and hardware cube.

## OpenGL architecture

Mesa's Iris driver still uses its familiar GEM and syncobj vocabulary
internally. `IntelWinsys.cpp` translates those private calls into the versioned
IntelGfx ABI in process; no Linux DRM ioctl or userspace pointer crosses into
the Haiku kernel. PRIME sharing, userptr buffers, native fence file descriptors,
and memory-object import are reported unsupported.

Iris chooses stable 48-bit virtual addresses. The winsys binds every object in
an exec list at its chosen address, validates that the batch is first and has
no relocations, and sends the complete handle list to the kernel. The kernel
holds the client lock and all mappings until execution and context save have
both completed. Submissions are serialized on the physical render engine,
while each OpenGL client and Mesa hardware context keeps separate page tables
and logical context state.

CPU mappings are ordinary cloneable Haiku areas. The kernel flushes every
referenced object before submission and invalidates it after completion. X and
Y tiling are recorded and validated for Iris surface state; Gen8 and later do
not need legacy fence registers to address tiled resources.

The HGL frontend maps the completed Gallium color buffer, converts it to the
window color format, and displays it through `BGLView`. Rendering is performed
by the Intel GPU, while final presentation currently uses a CPU readback and
bitmap copy. This is intentionally a first correct path; direct scanout,
page-flip presentation, asynchronous queues, and interrupt-driven completion
remain future performance work.

## Kernel command submission

The physical render and blitter engines use Gen8+ execlist submission. A
logical ring context contains the saved register image, its ring, and the root
of its PPGTT. A submission writes the context descriptor to the execlist port,
runs the client batch, flushes the relevant GPU caches, writes a sequence
number, and waits for both that sequence number and the context-save event.

The render path programs the Gen9 render power-clock state from the fuse
configuration. Before client commands it applies the SKL/KBL context
workarounds and the required null and invalidating `PIPE_CONTROL` sequence.
The engine-level compression, HDC, clock-gating, preemption, coherent-line,
and Iris register-whitelist settings are installed when the physical render
engine starts.

Page tables use scratch mappings at every level, so an unmapped access reaches
a zero page instead of unrelated client memory. Fixed bindings reject zero,
misaligned, overlapping, upper-half, and overflowing ranges. Buffers are
zeroed locked system memory, limited to 64 MiB each and 256 MiB per open
client. The current submit path is synchronous and supports at most 256
objects per batch.

The blitter path has run on a ThinkPad P50 Skylake GT2: it fetched through its
PPGTT, executed batches, updated CPU-visible fences, filled the display, and
copied pixels back. Earlier render bring-up reached batch completion but could
wedge on a following submission. The native path adds the missing isolated
contexts, render workarounds, cache invalidation, and complete object lifetime
rules; its first real-hardware validation is the outstanding step noted above.

## Scope and limitations

The native Iris and ANV capabilities are exposed only for Gen9 Skylake
(`0x19xx`) and Kaby Lake (`0x59xx`) PCI IDs. Other generations continue to use
the unified display path but cannot select these renderers.

There is no GPU reset or replay after a hang. Interrupt-driven scheduling,
parallel engine queues, eviction, sparse unbinding, PRIME sharing, direct
presentation, suspend/resume validation, and broader Intel generations are
not implemented. The Vulkan path includes a Haiku WSI but remains experimental
and needs conformance and application testing on supported hardware.

## Roll back

Remove the package and reboot. Haiku then falls back to another applicable
graphics driver (normally the framebuffer accelerant):

```sh
pkgman uninstall intel_gfx
reboot
```

If the desktop cannot start, use a previous package state from the boot menu
or a Haiku recovery boot and remove `intel_gfx.hpkg` from the active package
set.

## Source layout

* `kernel/`: buffer objects, GGTT/PPGTT, logical engines, render clients, and
  the device entry points.
* `headers/`: fixed-width IntelGfx ABI and Gen8/Gen9 command definitions.
* `client/`: native device and mapped-buffer wrappers.
* `mesa/`: reproducible Mesa patch, Haiku Iris target, compatibility adapter,
  and standalone cross-build.
* `display/`: canonical `intel_extreme.accelerant` display component.
* `demo/`: renderer-verifying OpenGL cube.
* `tools/`, `server/`, and `input/`: diagnostics, discovery service, and
  brightness key integration.
* `package/`: standalone HPKG metadata; the Haiku tree owns default-image
  integration.

`UPSTREAM.json` records the Haiku revision and original display source paths.
Vendored display code retains its original licenses. The hardware programming
model follows Intel's Kaby Lake PRMs and Linux i915's Gen9 PPGTT, logical
context, execlist, fuse-topology, and workaround behavior.

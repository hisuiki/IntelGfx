# IntelGfx

Experimental Intel graphics package for Haiku x86_64, following RadeonGfx's
separation of kernel device access, graphics services, client transport, and
display integration. First render target: Kaby Lake GT1 / HD Graphics 610.
The same generation 9 paths cover Skylake, including the HD Graphics 530 of
a sixth generation Core ThinkPad P50.

**Current implementation:** independently built kernel driver and accelerant,
versioned device discovery, per-open buffer handles, zeroed pinned CPU buffer
objects, a native generation 8+ global GTT with real graphics addresses,
four level per-process page tables, a logical ring context on the blitter
engine, execution list submission with polled fences, client mappings, device
service, diagnostics, and an opt-in HPKG. Command submission has run on a
Skylake GT2 (8086:191b): the GPU executes a batch and the fence it writes is
visible to the CPU. **Mesa Iris/ANV integration and accelerated 3D are not
implemented, the render engine is not driven, and clients share one GPU
address space rather than being isolated from each other.**

## Build without rebuilding an ISO

From the Haiku checkout, with its x86_64 cross-build already configured:

```sh
python3 intel_gfx/build.py --haiku-build generated.x86_64 --package
```

Only the four IntelGfx targets and their build dependencies are requested.
This uses Haiku's existing compiler, private headers, and Jam rules; it does
not modify the stock driver sources or UserBuildConfig. Output is under
`intel_gfx/out/`, including the build log, staged package, and HPKG.

The source package currently lives in this checkout and depends on Haiku's
configured build tree. The binary HPKG is independently installable. A native
standalone SDK build is a later packaging task.

## Install and test

Copy `out/intel_gfx-0.1.0-1-x86_64.hpkg` to `/boot/system/packages/` on the
test system. Installation alone does not replace the running display driver.

```sh
intel_gfx_ctl list
/boot/system/servers/IntelGfx &
intel_gfx_ctl service-info
intel_gfx_activate status
intel_gfx_activate enable
# Reboot, then use the device path printed by list:
intel_gfx_ctl list
intel_gfx_ctl info /dev/graphics/intel_extreme_000200
intel_gfx_ctl buffer-test /dev/graphics/intel_extreme_000200
intel_gfx_ctl gtt-test /dev/graphics/intel_extreme_000200
intel_gfx_ctl submit-test /dev/graphics/intel_extreme_000200
```

The example PCI device path is not universal. `buffer-test` covers CPU memory
only. `gtt-test` binds buffers into the global GTT and briefly pins 64 MiB of
locked memory, so it needs a machine with memory to spare. Capability reporting
never claims GPU submission until it is implemented. An ordinary VirtIO VM can
test installation and the service's no-device path; Intel hardware or PCI
passthrough is required for Intel device tests.

## Roll back

Run `intel_gfx_activate disable`, reboot, then remove the HPKG. The activation
tool refuses to overwrite existing custom driver overrides and removes only
its own links. Do not uninstall an enabled package before disabling it.
If the desktop cannot start, use a Haiku recovery boot to remove the two links
under the installed volume's `home/config/non-packaged/add-ons/kernel/drivers/`:
`bin/intel_extreme` and `dev/graphics/intel_extreme`, after confirming they
point to IntelGfx. Reboot the installed system. Booting with "Disable user
add-ons" in the boot menu also brings the packaged driver back for one boot.

IntelGfx uses the original legacy driver name at activation, but supplies its
own accelerant signature. That prevents a second independent Intel PCI owner.
The override has to live under the home directory: the kernel ranks a driver
by the directory its path starts with and tests the system directory before
the system's non-packaged directory, which that path also starts with, so a
driver installed there ties with the packaged driver of the same name and
loses to it. Everything else the package installs is system wide.
Kernel/display replacement requires reboot; user-space tools can be rebuilt
and replaced without regenerating installation media.

## Modules and provenance

* `kernel/`: Haiku display/device foundation plus RenderClient and BufferObject.
* `display/`: the existing Intel accelerant, built with a separate signature.
* `headers/`: shared display definitions and the fixed-width, versioned new ABI.
* `client/`: device transport and mapped-buffer lifetime management.
* `server/`: native device discovery service (future submission service).
* `tools/`: diagnostics and buffer tests.
* `package/`: package metadata and reversible activation.

## Global GTT

Buffers are created as locked, cloneable system memory and are only visible to
the GPU once they are bound. Binding pins their pages into the device's global
GTT, and the graphics address handed back is a byte offset into the device's
global address space, the form the display engine and the command streamer
consume. It is page aligned, lives until the buffer is unbound or closed, and
is not stable across rebinds. Each open device pins at most 64 MiB.

IntelGfx programs the page table itself instead of going through Haiku's
`agp_gart` bus manager. That manager still writes the pre-Broadwell layout:
32 bit entries, indexed four bytes apart, in a table it looks for two megabytes
into the register BAR. From generation 8 the table holds 64 bit entries and
sits in the upper half of that BAR, so nothing `agp_gart` binds is reachable by
a Skylake or Kaby Lake GPU. It keeps working for the display because the parts
it hands out in practice are firmware-mapped stolen memory, which needs no
entries of its own.

The two allocators are kept apart by address rather than by agreement:
`agp_gart` and the display driver only ever hand out the part of the address
space the CPU aperture covers, so IntelGfx manages a range that starts where
the aperture ends. Unbound pages inside that range point at a scratch page, so
a stray access reads zeroes instead of memory belonging to something else, and
entries are always retired before the pages behind them are freed. After every
change the last entry is read back and the GPU is told to drop the entries it
had cached.

One thing the invalidation cannot promise yet: the register write assumes the
graphics engine is awake, which holds while the display driver has the device
powered but will need explicit forcewake handling once work is submitted from
a runtime-suspended state. Nothing depends on it today, since no engine reads
these entries until command submission exists.

`kGpuVirtualMemory` is reported only when a client really holds a page table,
so it cannot promise an address the device could not use; on generations
before 8 the driver reports no GPU address space at all rather than guessing
at a layout it does not implement. Cacheability is not part of a generation 8+
entry: it comes from the MOCS state of whatever accesses the page, which is a
decision for the command submission that does not exist yet.

## Command submission

Work reaches the GPU the way the hardware has expected since generation 8.
A logical ring context describes the engine's state; its address is written to
the engine's execution list submit port, and the engine loads that context,
runs the ring buffer named inside it, and saves the context back. The register
image inside the context has a fixed shape per generation and engine class,
and the one used here is the layout Linux's i915 driver uses for the
generation 9 engines that are not the render engine, in the same encoding, so
that it can be compared against its source rather than trusted from memory.

The blitter engine is driven rather than the render engine: its context is the
simplest one that still runs ordinary memory commands, and nothing else in the
system uses it while the display driver works. Interrupts stay masked and
completion is polled. A submission ends with a flush that writes its sequence
number to a status page, so a fence is a sequence number and waiting on it
means everything the commands wrote is already visible. Submissions are
serialized: the ring is only refilled once the engine has finished with what
was in it.

Every context needs valid per-process page tables even when the commands
address memory globally, because the hardware walks them when it restores a
context. The four level tables here are built out of ordinary locked pages,
and unmapped addresses resolve to a scratch page instead of faulting, so a
mistake in a command buffer cannot take the engine down with it. A bound
buffer is mapped into those tables at the same address its global mapping
uses, so one address means the same thing to commands either way.

The one context and one address space are shared by every client of a device,
which is fine for a driver whose only client is its own test tool and is the
first thing that has to change before anything else uses it.

This path has run on hardware: on a ThinkPad P50's Skylake GT2 the engine
loads the context, executes a batch fetched through the per-process page
tables, and signals a fence the processor can see, repeatedly and without
disturbing the display. `intel_gfx_ctl engine-status` reads the engine back
afterwards, which is how anything that goes wrong here gets diagnosed: the
ring registers as the engine restored them, the head it reached, the context
switch events it recorded, and how much of the context image it rewrote.

Two things had to be right before any of that worked, and both are the kind
of mistake that leaves the hardware looking healthy while doing nothing:

* Command opcodes must be compile time constants. They were being computed by
  an ordinary function at namespace scope, which a kernel add-on does not
  necessarily evaluate before the driver runs; every opcode came out zero, so
  the context image's register loads decoded as no-ops, the engine restored no
  ring at all, and it reported a clean completion having done nothing. They
  are `constexpr` now, with static assertions on the resulting values.
* `get_memory_map()` needs one more table entry than the number of pages it
  describes, for the empty entry it ends the list with. Asking it to describe
  exactly as many pages as the table holds fails with `B_BUFFER_OVERFLOW`,
  which is what binding anything larger than 31 pages used to do.

## Panel brightness

The backlight is controlled through the ordinary `BScreen` interface, so the
slider in Screen preferences works and so does anything else that asks. That
slider hides itself when the driver reports no brightness support, which is
what a laptop looked like when the device table called its chip a desktop
part; the driver now decides by asking the hardware whether a backlight
modulation period is programmed.

`intel_gfx_brightness_keys` is an input filter that turns the brightness keys
of a keyboard into backlight changes and shows the level as a notification,
which nothing in the system does on its own. It acts on the display
brightness usages of the HID consumer page, and a keyboard that reports
something else can be accommodated with `report_keys true` in
`~/config/settings/kernel/drivers/intel_gfx_brightness_keys`, which writes
every key it sees to the syslog, followed by `raise_key` and `lower_key`
there.

That covers keyboards whose brightness keys reach the system at all. A
ThinkPad's do not: on a P50 the volume keys arrive as consumer page usages
while the brightness keys produce nothing and change nothing, because the
embedded controller reports them as ACPI events on the vendor's hotkey
device, which Haiku has no driver for. Making those keys work needs that
driver, not this filter.

## The render engine

The blitter is fully working. The render engine, which is the one 3D work
needs, is not, and it is worth being precise about how far it gets: it loads
its context, executes the whole ring including the batch, writes its fence
with the pipe control that engine takes instead of a flush, saves 397 dwords
of context image back, and reports itself complete with no error. The only
thing that does not happen is the batch's store becoming visible, and a
second submission then finds the engine wedged.

Two things were ruled out along the way. The fence write proves the ring, the
context and the execution list port all work on that engine, and the batch
running to its end proves it was fetched through the per-process page tables.
The page attribute table is now programmed the way Linux programs it, because
nothing on a Haiku system did: every access this driver makes lands on entry
zero, whose meaning was whatever firmware happened to leave. That was worth
fixing on its own and did not change this symptom.

What remains to look at is the state a fresh render context starts in. Linux
initialises one from a golden image produced by running a long state setup
batch, and a render pipeline that has never been configured is a plausible
reason for a store to go nowhere and a pipe control to hang. Note that a real
3D driver sets all of that state in its own batches, so this may matter less
to Mesa than it does to a test that sets none.

## Test image

`image.py` builds a bootable Haiku image with this driver already in place, for
testing on real Intel hardware without installing anything:

```sh
python3 intel_gfx/image.py --haiku-build generated.x86_64 --write /dev/sdX
```

The image is built for debugging: the kernel's serial and syslog debug output
and its on-screen debug output are on from the first boot, so a driver problem
leaves a record without anyone having to choose anything in the boot menu, and
SSH is running with the password `haiku` for the user `user` (plus the key
from the VM setup, when there is one). It is a throwaway test image; treat
anything it can reach as public.

Everything goes into non-packaged directories, which need no package
activation and therefore survive a live boot: the kernel driver under the
legacy name in the home directory, where it outranks the packaged driver and
owns the Intel PCI function from the first boot, plus the accelerant, the
service and the tools beside the system. The HPKG
rides along on the Desktop for installing on a real system afterwards. Choose
"Disable user add-ons" in the boot menu to boot the same image with the stock
driver instead. Writing to a device erases it, so `--write` names the device
explicitly and never guesses.

On a laptop with switchable graphics the firmware has to expose the Intel GPU
at all: a ThinkPad P50 only does so with its BIOS graphics device set to
hybrid rather than discrete.

`UPSTREAM.json` records the exact Haiku revision and original file paths.
The vendored display code retains its original licenses and copyright notices.
One inherited status-page output-pointer bug is corrected in this fork.
The older display family's aliases must not be used to identify render IP:
the new ABI reports a graphics version only for the generations this driver
implements, which is 8 and later, and unknown for everything else.

Architecture references: https://github.com/X547/RadeonGfx and
https://github.com/X547/nvidia-haiku. Intel hardware implementation must follow
the Kaby Lake PRMs and the i915 logical-context, PPGTT, and execlist behavior.

#!/usr/bin/env python3
"""Build Mesa 22 Iris as a Haiku OpenGL renderer add-on."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request


VERSION = "22.0.5"
SHA256 = "5ee2dc06eff19e19b2867f12eb0db0905c9691c07974f6253f2f1443df4c7a35"
URI = f"https://archive.mesa3d.org/older-versions/22.x/mesa-{VERSION}.tar.xz"


def run(command, **kwargs):
    print("+", " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def package(build: Path, name: str) -> Path:
    matches = sorted((build / "build_packages").glob(f"{name}-[0-9]*-x86_64"))
    if not matches:
        raise RuntimeError(f"Haiku build package is missing: {name}")
    return matches[-1]


def main() -> int:
    here = Path(__file__).resolve().parent
    intel_gfx = here.parent
    haiku = intel_gfx.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--haiku-build", type=Path,
                        default=haiku / "generated.x86_64")
    parser.add_argument("-j", "--jobs", type=int, default=4)
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()
    build = args.haiku_build.resolve()
    if not (build / "build/BuildConfig").is_file():
        parser.error("Haiku build directory must be configured for x86_64")

    out = intel_gfx / "out/mesa"
    archive = out / f"mesa-{VERSION}.tar.xz"
    source = out / f"mesa-{VERSION}"
    mesa_build = out / "build"
    out.mkdir(parents=True, exist_ok=True)
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        urllib.request.urlretrieve(URI, archive)
    if args.clean:
        if source.is_dir():
            shutil.rmtree(source)
        if mesa_build.is_dir():
            shutil.rmtree(mesa_build)
    stamp = source / ".intel-gfx-overlay"
    if not stamp.is_file():
        if source.is_dir():
            shutil.rmtree(source)
        with tarfile.open(archive) as tar:
            tar.extractall(out, filter="data")
        ports_patch = (haiku.parent / "haikuports/sys-libs/mesa/patches"
                       / f"mesa-{VERSION}.patchset")
        run(["patch", "--batch", "-p1", "-i", ports_patch], cwd=source)
        run(["patch", "--batch", "-p1", "-i",
             here / f"mesa-{VERSION}-haiku-iris.patch"], cwd=source)
        stamp.write_text(SHA256 + "\n")

    # Overlay files are the part of the port edited during development. Keep
    # an existing source tree in sync so an incremental build never silently
    # packages stale winsys or ABI code.
    shutil.copytree(here / "overlay/haiku-iris",
                    source / "src/gallium/targets/haiku-iris",
                    dirs_exist_ok=True)
    shutil.copytree(here / "overlay/compat",
                    source / "src/intel/haiku/compat", dirs_exist_ok=True)
    shutil.copy2(intel_gfx / "headers/IntelGfxABI.h",
                 source / "src/intel/haiku/compat/IntelGfxABI.h")
    shutil.copy2(here / "overlay/compat/intel_haiku.h",
                 source / "src/intel/common/intel_haiku.h")
    shutil.copy2(here / "overlay/wsi/wsi_common_haiku.c",
                 source / "src/vulkan/wsi/wsi_common_haiku.c")

    regular = (build / "objects/haiku/x86_64/packaging/packages_build/regular")
    devel = regular / "hpkg_-haiku_devel.hpkg/contents"
    runtime = regular / "hpkg_-haiku.hpkg/contents"
    headers = devel / "develop/headers"
    if not headers.is_dir():
        raise RuntimeError("haiku_devel package contents are not built")
    libraries = out / "syslib"
    libraries.mkdir(exist_ok=True)
    roots = [devel / "develop/lib", runtime / "lib"]
    roots += [package(build, name) / "lib"
              for name in ("zlib", "expat", "gcc_syslibs", "mesa")]
    for root in roots:
        for item in root.glob("*"):
            if not item.is_file() or item.is_symlink():
                continue
            targets = [item.name]
            if ".so." in item.name:
                targets.append(item.name.split(".so.")[0] + ".so")
            for name in targets:
                link = libraries / name
                if not link.exists():
                    link.symlink_to(item)

    include_roots = [headers / "posix", headers, headers / "os",
                     haiku / "headers/private",
                     haiku / "headers/private/interface"]
    include_roots += sorted(path for path in (headers / "os").rglob("*")
                            if path.is_dir())
    compile_args = [f"-idirafter{path}" for path in include_roots]
    compile_args += ["-D_DEFAULT_SOURCE", "-D_BSD_SOURCE",
                     f"-B{devel / 'develop/lib'}/"]
    link_args = [f"-L{libraries}", f"-Wl,-rpath-link,{libraries}",
                 f"-B{devel / 'develop/lib'}/"]
    toolchain = build / "cross-tools-x86_64/bin/x86_64-unknown-haiku-"
    cross = out / "cross.ini"
    cross.write_text(
        "[binaries]\n"
        f"c = '{toolchain}gcc'\ncpp = '{toolchain}g++'\n"
        f"ar = '{toolchain}ar'\nstrip = '{toolchain}strip'\n"
        "pkg-config = '/usr/bin/pkg-config'\n"
        "[host_machine]\nsystem = 'haiku'\ncpu_family = 'x86_64'\n"
        "cpu = 'x86_64'\nendian = 'little'\n"
        "[properties]\nneeds_exe_wrapper = true\n"
        "[built-in options]\n"
        f"c_args = {compile_args!r}\ncpp_args = {compile_args!r}\n"
        f"c_link_args = {link_args!r}\ncpp_link_args = {link_args!r}\n")

    python = out / "venv/bin/python3"
    if not python.is_file():
        run([sys.executable, "-m", "venv", out / "venv"])
        run([python, "-m", "pip", "install", "meson==1.12.0", "mako",
             "packaging", "pyyaml"])
    meson = python.parent / "meson"
    environment = dict(os.environ)
    pkgconfig = out / "pkgconfig"
    pkgconfig.mkdir(exist_ok=True)
    (pkgconfig / "zlib.pc").write_text(
        f"Name: zlib\nDescription: Haiku zlib\nVersion: 1.3.2\n"
        f"Libs: -L{libraries} -lz\n"
        f"Cflags: -I{package(build, 'zlib') / 'develop/headers'}\n")
    (pkgconfig / "expat.pc").write_text(
        f"Name: expat\nDescription: Haiku expat\nVersion: 2.8.2\n"
        f"Libs: -L{libraries} -lexpat\n"
        f"Cflags: -I{package(build, 'expat') / 'develop/headers'}\n")
    environment["PATH"] = str(python.parent) + ":" + environment["PATH"]
    environment["PKG_CONFIG_LIBDIR"] = str(pkgconfig)
    options = [
        "-Dplatforms=haiku", "-Dgallium-drivers=iris", "-Dvulkan-drivers=intel",
        "-Ddri-drivers=", "-Dllvm=disabled", "-Dshared-llvm=disabled",
        "-Dglx=disabled", "-Degl=disabled", "-Dgles1=disabled",
        "-Dgles2=disabled", "-Dgbm=disabled", "-Dlibunwind=disabled",
        "-Dzstd=disabled", "-Dlmsensors=disabled", "-Dbuild-tests=false",
        "-Dglvnd=false", "-Dvalgrind=disabled", "-Dbuildtype=debugoptimized",
    ]
    if not (mesa_build / "build.ninja").is_file():
        run([meson, "setup", mesa_build, source, "--cross-file", cross] + options,
            env=environment)
    else:
        run([meson, "setup", "--reconfigure", mesa_build, source],
            env=environment)
    targets = [
        "src/gallium/targets/haiku-iris/libhaiku-iris.so",
        "src/intel/vulkan/libvulkan_intel.so",
        "src/intel/vulkan/intel_icd.x86_64.json",
    ]
    run(["ninja", "-C", mesa_build, f"-j{args.jobs}"] + targets, env=environment)
    strip_tool = f"{toolchain}strip"
    result = out / "Intel Gallium"
    shutil.copy2(mesa_build / "src/gallium/targets/haiku-iris/libhaiku-iris.so", result)
    run([strip_tool, str(result)])
    vk_result = out / "libvulkan_intel.so"
    shutil.copy2(mesa_build / "src/intel/vulkan/libvulkan_intel.so", vk_result)
    run([strip_tool, str(vk_result)])
    icd_result = out / "intel_icd.x86_64.json"
    icd_json = '{\n    "ICD": {\n        "api_version": "1.3.204",\n        "library_path": "libvulkan_intel.so"\n    },\n    "file_format_version": "1.0.0"\n}\n'
    icd_result.write_text(icd_json)
    print(f"Built:\n  {result}\n  {vk_result}\n  {icd_result}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

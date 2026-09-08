#!/usr/bin/env python3
"""Build IntelGfx targets against a configured Haiku tree; never build an image."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    project = Path(__file__).resolve().parent
    default_build = project.parent / 'generated.x86_64'
    if not (default_build / 'build/BuildConfig').is_file():
        if (project.parent / 'haiku/generated.x86_64/build/BuildConfig').is_file():
            default_build = project.parent / 'haiku/generated.x86_64'
        elif 'HAIKU_OUTPUT_DIR' in os.environ:
            default_build = Path(os.environ['HAIKU_OUTPUT_DIR']).resolve()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--haiku-build', type=Path,
                        default=default_build)
    parser.add_argument('-j', '--jobs', type=int, default=4)
    parser.add_argument('--package', action='store_true')
    parser.add_argument('--mesa', action='store_true',
                        help='also build the Mesa Iris OpenGL renderer')
    parser.add_argument('--tests', action='store_true', help='also build kernel memory test fixture')
    args = parser.parse_args()
    build = args.haiku_build.resolve()
    if not (build / 'build/BuildConfig').is_file():
        parser.error('Haiku build directory must already be configured for x86_64')
    out = project / 'out'
    out.mkdir(exist_ok=True)
    # Jam quoting is deliberately restricted here, rather than interpolating code.
    if any(c in str(project.parent) for c in '\n\r"$[];'):
        parser.error('unsupported characters in source directory path')
    haiku_top = project.parent
    if not (haiku_top / 'Jamfile').is_file():
        if (haiku_top / 'haiku/Jamfile').is_file():
            haiku_top = haiku_top / 'haiku'
        elif (build.parent / 'Jamfile').is_file():
            haiku_top = build.parent
    wrapper = out / 'Build.jam'
    wrapper.write_text('JAMFILE = Jamfile ;\n'
                       f'HAIKU_TOP = "{os.path.relpath(haiku_top, build)}" ;\n'
                       'HAIKU_OUTPUT_DIR = . ;\n'
                       'include [ FDirName $(HAIKU_TOP) Jamfile ] ;\n')
    command = ['jam', f'-sJAMFILE={wrapper}', '-sHAIKU_IGNORE_USER_BUILD_CONFIG=1',
               f'-j{args.jobs}', 'intel_extreme', 'intel_extreme.accelerant',
               'IntelGfx', 'intel_gfx_ctl', 'intel_gfx_brightness_keys', 'intel_gfx_cube',
               'intel_gfx_monitor']
    if args.tests:
        command.append('intel_gfx_memory_test')
    print('Building IntelGfx (log: %s)' % (out / 'build.log'), flush=True)
    # Haiku's Jamrules build absolute paths out of $(PWD), which jam takes from
    # the environment; running it in another directory does not change that by
    # itself. The C locale keeps the log the same whoever runs the build.
    environment = dict(os.environ, LC_ALL='C', PWD=str(build))
    with (out / 'build.log').open('w') as log:
        result = subprocess.run(command, cwd=build, stdout=log,
            stderr=subprocess.STDOUT, env=environment)
    if result.returncode:
        print('\n'.join((out / 'build.log').read_text().splitlines()[-100:]), file=sys.stderr)
        return result.returncode
    mesa_renderer = project / 'out/mesa/Intel Gallium'
    mesa_vulkan = project / 'out/mesa/libvulkan_intel.so'
    mesa_icd = project / 'out/mesa/intel_icd.x86_64.json'
    if args.mesa or args.package:
        subprocess.run([
            sys.executable, project / 'mesa/build.py',
            '--haiku-build', build, f'-j{args.jobs}'
        ], check=True)
    if not args.package:
        print('Build complete. Add --package to produce an HPKG.')
        return 0
    objects = build / 'objects/haiku/x86_64/release'
    stage = out / 'stage'
    if stage.exists():
        shutil.rmtree(stage)
    file_pairs = [
        (objects / 'kernel/intel_extreme',
            'add-ons/kernel/drivers/bin/intel_extreme'),
        (objects / 'display/intel_extreme.accelerant',
            'add-ons/accelerants/intel_extreme.accelerant'),
        (objects / 'server/IntelGfx', 'servers/IntelGfx'),
        (objects / 'tools/intel_gfx_ctl', 'bin/intel_gfx_ctl'),
        (objects / 'input/intel_gfx_brightness_keys',
            'add-ons/input_server/filters/intel_gfx_brightness_keys'),
        (objects / 'demo/intel_gfx_cube', 'bin/intel_gfx_cube'),
        (objects / 'monitor/intel_gfx_monitor', 'bin/intel_gfx_monitor'),
        (project / 'README.md', 'documentation/packages/intel_gfx/README.md'),
        (project / 'UPSTREAM.json', 'documentation/packages/intel_gfx/UPSTREAM.json'),
        (project / 'License.md', 'data/licenses/IntelGfx'),
        (mesa_renderer, 'add-ons/opengl/Intel Gallium'),
        (mesa_vulkan, 'lib/libvulkan_intel.so'),
        (mesa_icd, 'data/vulkan/icd.d/intel_icd.x86_64.json'),
    ]
    for source, destination in file_pairs:
        if source.is_file():
            target = stage / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    driver_link = stage / 'add-ons/kernel/drivers/dev/graphics/intel_extreme'
    driver_link.parent.mkdir(parents=True, exist_ok=True)
    driver_link.symlink_to('../../bin/intel_extreme')
    shutil.copy2(project / 'package/PackageInfo', stage / '.PackageInfo')
    package = build / 'objects/linux/x86_64/release/tools/package/package'
    if sys.platform == 'haiku1':
        package = Path('/boot/system/bin/package')
    if not package.is_file():
        parser.error('package tool not built; build the Haiku package tool first')
    env = dict(environment)
    env['LD_LIBRARY_PATH'] = str(build / 'objects/linux/lib') + ':' + env.get('LD_LIBRARY_PATH', '')
    hpkg = out / 'intel_gfx-0.3.0-1-x86_64.hpkg'
    subprocess.run([str(package), 'create', '-C', str(stage), str(hpkg)],
                   check=True, env=env)
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'],
                                       cwd=project, text=True).strip()
    (out / 'build-manifest.json').write_text(json.dumps({
        'haiku_revision': revision, 'build_directory': str(build),
        'package': str(hpkg), 'targets': command[5:],
        'mesa_renderer': str(mesa_renderer),
    }, indent=2) + '\n')
    print(hpkg)
    return 0


if __name__ == '__main__':
    sys.exit(main())

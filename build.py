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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--haiku-build', type=Path,
                        default=project.parent / 'generated.x86_64')
    parser.add_argument('-j', '--jobs', type=int, default=4)
    parser.add_argument('--package', action='store_true')
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
    wrapper = out / 'Build.jam'
    wrapper.write_text('JAMFILE = Jamfile ;\n'
                       f'HAIKU_TOP = "{os.path.relpath(project.parent, build)}" ;\n'
                       'HAIKU_OUTPUT_DIR = . ;\n'
                       'include [ FDirName $(HAIKU_TOP) Jamfile ] ;\n'
                       'SubInclude HAIKU_TOP intel_gfx ;\n')
    command = ['jam', f'-sJAMFILE={wrapper}', '-sHAIKU_IGNORE_USER_BUILD_CONFIG=1',
               f'-j{args.jobs}', 'intel_gfx', 'intel_gfx.accelerant',
               'IntelGfx', 'intel_gfx_ctl']
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
    if not args.package:
        print('Build complete. Add --package to produce an HPKG.')
        return 0
    objects = build / 'objects/haiku/x86_64/release'
    stage = out / 'stage'
    if stage.exists():
        shutil.rmtree(stage)
    files = {
        objects / 'kernel/intel_gfx': 'data/intel_gfx/kernel/intel_gfx',
        objects / 'display/intel_gfx.accelerant': 'add-ons/accelerants/intel_gfx.accelerant',
        objects / 'server/IntelGfx': 'servers/IntelGfx',
        objects / 'tools/intel_gfx_ctl': 'bin/intel_gfx_ctl',
        project / 'package/intel_gfx_activate': 'bin/intel_gfx_activate',
        project / 'README.md': 'documentation/packages/intel_gfx/README.md',
        project / 'UPSTREAM.json': 'documentation/packages/intel_gfx/UPSTREAM.json',
        project / 'License.md': 'data/licenses/IntelGfx',
    }
    for source, destination in files.items():
        target = stage / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    shutil.copy2(project / 'package/PackageInfo', stage / '.PackageInfo')
    package = build / 'objects/linux/x86_64/release/tools/package/package'
    if sys.platform == 'haiku1':
        package = Path('/boot/system/bin/package')
    if not package.is_file():
        parser.error('package tool not built; build the Haiku package tool first')
    env = dict(environment)
    env['LD_LIBRARY_PATH'] = str(build / 'objects/linux/lib') + ':' + env.get('LD_LIBRARY_PATH', '')
    hpkg = out / 'intel_gfx-0.1.0-1-x86_64.hpkg'
    subprocess.run([str(package), 'create', '-C', str(stage), str(hpkg)],
                   check=True, env=env)
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'],
                                       cwd=project, text=True).strip()
    (out / 'build-manifest.json').write_text(json.dumps({
        'haiku_revision': revision, 'build_directory': str(build),
        'package': str(hpkg), 'targets': command[5:],
    }, indent=2) + '\n')
    print(hpkg)
    return 0


if __name__ == '__main__':
    sys.exit(main())

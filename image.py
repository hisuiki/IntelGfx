#!/usr/bin/env python3
"""Build a Haiku AnyBoot image with IntelGfx already driving the Intel GPU."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

BEGIN = '# BEGIN intel_gfx/image.py'
END = '# END intel_gfx/image.py'


def shadow_line(user, password):
    """Haiku's crypt() hashes with scrypt when the salt is long: the stored
    form is $s$<log2 N>$<salt as hex>$<hash as hex>, N = 2^14, r = 8, p = 1,
    over a 32 byte salt and a 32 byte hash. A short salt would instead select
    the old DES hash, which truncates the password to eight characters."""
    salt = os.urandom(32)
    digest = hashlib.scrypt(password.encode(), salt=salt, n=1 << 14, r=8, p=1,
                            dklen=32)
    stored = f'$s${14}${salt.hex()}${digest.hex()}'
    changed = int(time.time() / (24 * 60 * 60))
    # name:password:last changed:min:max:warn:inactive:expiration:flags
    return f'{user}:{stored}:{changed}::::::0\n'


KERNEL_SETTINGS = """\
# Written by intel_gfx/image.py. Debugging is on by default in this image:
# the syslog is what a driver under development is read through, and the
# serial line is the only thing that still works once the kernel stops.
serial_debug_output true
syslog_debug_output true
debug_screen true
emergency_keys true
"""

# Kept out of the settings directory the openssh package writes, and named on
# the daemon's command line, so that installing the package cannot replace it.
# Haiku's only user has uid 0, which is what OpenSSH calls root, so password
# logins need root logins permitted. The two paths below are the ones the
# packaged configuration uses.
SSHD_CONFIG = """\
# Written by intel_gfx/image.py for a throwaway test image.
PermitRootLogin yes
PasswordAuthentication yes
PermitEmptyPasswords no
PrintMotd no
AuthorizedKeysFile config/settings/ssh/authorized_keys
Subsystem sftp /boot/system/lib/openssh/sftp-server
"""

START_SSHD = """\
#!/bin/sh
# Started by the launch daemon; see the sshd job beside this file. A live
# image ships no host keys, so they are made on first boot.
settings=$(finddir B_SYSTEM_SETTINGS_DIRECTORY)/ssh
mkdir -p "$settings"

for type in ed25519 rsa ecdsa; do
	key="$settings/ssh_host_${type}_key"
	[ -f "$key" ] || ssh-keygen -t "$type" -f "$key" -N "" >/dev/null
done

exec /boot/system/bin/sshd -D -e \\
	-f /boot/system/non-packaged/data/intel_gfx/sshd_config
"""

SSHD_JOB = """\
service x-vnd.haiku-sshd {
	launch /bin/sh /boot/system/non-packaged/data/launch/start_sshd.sh
	legacy
}
"""


def stage_settings(stage, user, password, key_file):
    """Writes the image's settings files and returns them as name, directory
    pairs."""
    files = []

    (stage / 'shadow').write_text(shadow_line(user, password))
    files.append(('shadow', 'system settings etc'))

    (stage / 'kernel').write_text(KERNEL_SETTINGS)
    files.append(('kernel', 'system settings kernel drivers'))

    (stage / 'sshd_config').write_text(SSHD_CONFIG)
    files.append(('sshd_config', 'system non-packaged data intel_gfx'))
    (stage / 'start_sshd.sh').write_text(START_SSHD)
    (stage / 'sshd').write_text(SSHD_JOB)
    files.append(('start_sshd.sh', 'system non-packaged data launch'))
    files.append(('sshd', 'system non-packaged data launch'))
    if key_file is not None and key_file.is_file():
        shutil.copy2(key_file, stage / 'authorized_keys')
        files.append(('authorized_keys', 'home config settings ssh'))

    return files


def jam_environment(build):
    """Haiku's Jamrules build absolute paths out of $(PWD), which jam reads
    from the environment rather than asking the system. Changing the working
    directory of a child process leaves PWD behind, and jam would then place
    the image scripts next to the source tree instead of in the build
    directory. The C locale keeps parsed output and diagnostics predictable."""
    return dict(os.environ, LC_ALL='C', PWD=str(build))


def configure(build, stage, package, settings):
    """Add our block to UserBuildConfig, keeping whatever else is in it."""
    config = build / 'UserBuildConfig'
    kept = []
    if config.is_file():
        skipping = False
        for line in config.read_text().splitlines():
            if line == BEGIN:
                skipping = True
            elif line == END:
                skipping = False
            elif not skipping:
                kept.append(line)
    # The VM rebuild script installs an SSH daemon of its own under the same
    # names. Only one of the two may put a launch job in the image, and this
    # one also carries the password and the configuration that goes with it,
    # so its lines give way here; that script restores them when it next runs.
    dropped = [line for line in kept
               if '<vm>' in line and ('sshd' in line or 'authorized_keys' in line)]
    if dropped:
        print(f'Replacing {len(dropped)} SSH lines from the VM rebuild script',
              flush=True)
        kept = [line for line in kept if line not in dropped]
    # Everything goes in non-packaged: a non-packaged driver hides the
    # packaged one of the same name, so only one of them owns the PCI
    # function, and the accelerant and tools need no package activation to be
    # found. The HPKG rides along on the Desktop for installing on a real
    # system later.
    #
    # The driver is the file the device path names, rather than the usual
    # symlink into a bin directory beside it: the kernel loads whatever that
    # entry resolves to, and AddSymlinkToHaikuImage cannot be called from
    # UserBuildConfig, where it writes into the image script before the
    # script's own targets have been placed.
    #
    # It goes under the home directory rather than beside the system, because
    # a driver in the system's own non-packaged directory does not actually
    # override the packaged driver of the same name: the kernel ranks drivers
    # by which directory their path starts with, and it tests the system
    # directory first, which is a prefix of the system's non-packaged
    # directory too. Both end up ranked equally and the packaged driver, being
    # scanned last, wins. The home directory is tested on its own and is
    # scanned first, so a driver there really does take precedence.
    files = {
        'intel_extreme':
            'home config non-packaged add-ons kernel drivers dev graphics',
        'intel_gfx.accelerant': 'system non-packaged add-ons accelerants',
        'intel_gfx_ctl': 'system non-packaged bin',
        'intel_gfx_activate': 'system non-packaged bin',
        'IntelGfx': 'system non-packaged servers',
    }
    block = [BEGIN]
    for name, directory in list(files.items()) + settings:
        block.append(f'SEARCH on <intel_gfx>{name} = "{stage}" ;')
        block.append(f'AddFilesToHaikuImage {directory} : <intel_gfx>{name} ;')
    block.append(f'SEARCH on <intel_gfx>{package.name} = "{package.parent}" ;')
    block.append(f'AddFilesToHaikuImage home Desktop : <intel_gfx>{package.name} ;')
    block.append(END)
    config.write_text('\n'.join(kept + block) + '\n')
    return config


def write_image(image, device):
    if not Path(device).is_block_device():
        raise SystemExit(f'{device} is not a block device')
    print(f'Writing {image} to {device}', flush=True)
    subprocess.run(['sudo', 'dd', f'if={image}', f'of={device}', 'bs=4M',
                    'oflag=direct', 'conv=fsync', 'status=progress'], check=True)
    subprocess.run(['sudo', 'blockdev', '--rereadpt', device], check=False)


def main():
    project = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--haiku-build', type=Path,
                        default=project.parent / 'generated.x86_64')
    parser.add_argument('-j', '--jobs', type=int, default=4)
    parser.add_argument('--write', metavar='DEVICE',
                        help='erase this block device and write the image to it')
    parser.add_argument('--skip-build', action='store_true',
                        help='use the image already in the build directory')
    parser.add_argument('--password', default='haiku',
                        help="password for the image's user, default haiku")
    args = parser.parse_args()

    build = args.haiku_build.resolve()
    if not (build / 'build/BuildConfig').is_file():
        parser.error('Haiku build directory must already be configured for x86_64')
    if any(c in str(project.parent) for c in '\n\r"$[];'):
        parser.error('unsupported characters in source directory path')

    image = build / 'haiku-nightly-anyboot.iso'
    if not args.skip_build:
        result = subprocess.run([sys.executable, str(project / 'build.py'),
                                 '--haiku-build', str(build), '-j', str(args.jobs),
                                 '--package'])
        if result.returncode:
            return result.returncode

        stage = project / 'out/image'
        if stage.exists():
            shutil.rmtree(stage)
        stage.mkdir(parents=True)
        objects = build / 'objects/haiku/x86_64/release'
        staged = {
            objects / 'kernel/intel_gfx': 'intel_extreme',
            objects / 'display/intel_gfx.accelerant': 'intel_gfx.accelerant',
            objects / 'server/IntelGfx': 'IntelGfx',
            objects / 'tools/intel_gfx_ctl': 'intel_gfx_ctl',
            project / 'package/intel_gfx_activate': 'intel_gfx_activate',
        }
        for source, name in staged.items():
            target = stage / name
            shutil.copy2(source, target)
            target.chmod(0o755)
        package = project / 'out/intel_gfx-0.1.0-1-x86_64.hpkg'

        settings = stage_settings(stage, 'user', args.password,
                                  build / 'vm-ssh/id_ed25519.pub')

        config = configure(build, stage, package, settings)
        print(f'Configured {config}; building the image', flush=True)
        result = subprocess.run(['jam', '-q', f'-j{args.jobs}',
                                 '@nightly-anyboot'],
                                cwd=build, env=jam_environment(build))
        if result.returncode:
            return result.returncode

    if not image.is_file() or image.stat().st_size == 0:
        parser.error(f'no image was produced at {image}')
    print(image)
    if args.write:
        write_image(image, args.write)
    return 0


if __name__ == '__main__':
    sys.exit(main())

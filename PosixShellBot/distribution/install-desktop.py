#!/usr/bin/env python3
"""Register/remove this Linux package's application menu entry for the current user."""
import argparse
import importlib.util
import os
from pathlib import Path
import stat
import sys


def entry(root):
    program = str(root / 'bin/ShellBot')
    if '=' in program or any(ord(c) < 32 for c in program):
        raise ValueError('The desktop entry requires a path without controls or an equals sign')
    # Exec quoting is followed by Desktop Entry string-value escaping.
    quoted = ''.join('\\' + c if c in '\\"`$' else c for c in program).replace('%', '%%')
    quoted = '"' + quoted.replace('\\', '\\\\') + '"'
    # GIO checks the executable before expanding %% field codes. Keep that
    # check on env, then pass the exact literal path as an argument, without a shell.
    launcher = '/usr/bin/env -- ' if '%' in program else ''
    icon = str(root / 'share/icons/hicolor/512x512/apps/shellbot.png').replace('\\', '\\\\')
    return ('[Desktop Entry]\nType=Application\nName=ShellBot\n'
            'Comment=Manage local ShellBot projects and connections\nExec=' + launcher + quoted +
            ' --desktop\nIcon=' + icon + '\nTerminal=false\nCategories=Development;Utility;\n'
            'StartupNotify=true\n').encode('utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--remove', action='store_true')
    parser.add_argument('--applications-dir', type=Path,
                        default=Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))) / 'applications')
    args = parser.parse_args()
    try:
        if sys.platform != 'linux' or os.geteuid() == 0:
            raise ValueError('Use an ordinary Linux desktop account')
        root = Path(__file__).resolve().parent
        spec = importlib.util.spec_from_file_location('shellbot_verify', root / 'verify-package.py')
        verifier = importlib.util.module_from_spec(spec)
        # Loading a bundled module must not add __pycache__ to the sealed payload.
        sys.dont_write_bytecode = True
        spec.loader.exec_module(verifier)
        verifier.verify(root)
        directory = args.applications_dir.absolute()
        if directory.resolve() != directory:
            raise ValueError('Use a canonical applications directory without symlinks')
        directory.mkdir(mode=0o755, parents=True, exist_ok=True)
        if directory.resolve() != directory:
            raise ValueError('Use a canonical applications directory without symlinks')
        info = directory.stat()
        if info.st_uid != os.geteuid() or info.st_mode & 0o022:
            raise ValueError('Applications directory must belong to this user and not be shared writable')
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            name = 'org.shellbot.desktop.desktop'
            wanted = entry(root)
            try:
                existing = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=fd)
            except FileNotFoundError:
                existing = -1
            if existing >= 0:
                try:
                    initial = os.fstat(existing)
                    if not stat.S_ISREG(initial.st_mode) or initial.st_nlink != 1 or initial.st_uid != os.geteuid() or initial.st_mode & 0o022:
                        raise ValueError('Existing desktop entry is unsafe; preserved')
                    if os.read(existing, len(wanted) + 1) != wanted:
                        raise ValueError('A different installation owns this desktop entry; preserved')
                    named = os.stat(name, dir_fd=fd, follow_symlinks=False)
                    if (initial.st_dev, initial.st_ino, initial.st_size, initial.st_mtime_ns) != (named.st_dev, named.st_ino, named.st_size, named.st_mtime_ns):
                        raise ValueError('Desktop entry changed; preserved')
                    if args.remove:
                        os.unlink(name, dir_fd=fd)
                finally:
                    os.close(existing)
            elif not args.remove:
                created = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644, dir_fd=fd)
                with os.fdopen(created, 'wb') as stream:
                    stream.write(wanted)
                    stream.flush()
                    os.fsync(stream.fileno())
            os.fsync(fd)
        finally:
            os.close(fd)
        print('Desktop entry removed; Data and services preserved.' if args.remove else
              'Desktop entry ready. Keep this package at its current path; login autostart is unchanged.')
        return 0
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())

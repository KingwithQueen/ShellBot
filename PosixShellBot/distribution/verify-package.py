#!/usr/bin/env python3
"""Verify candidate package bytes without loading code from the package."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import stat
import subprocess
import sys
import tarfile


def require(ok, message):
    if not ok:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON field: ' + key)
        result[key] = value
    return result


def read_regular(path, maximum=1024 * 1024 * 64):
    require(path.parent.resolve() == path.parent, 'Package file parent is a symlink')
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and
            not before.st_mode & 0o022 and before.st_size <= maximum,
            'Unsafe package file: ' + str(path))
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        opened = os.fstat(fd)
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mode, s.st_nlink,
                              s.st_mtime_ns, s.st_ctime_ns)
        require(identity(opened) == identity(before), 'Package file changed while opening')
        data = bytearray()
        while True:
            block = os.read(fd, 1024 * 1024)
            if not block:
                break
            data.extend(block)
            require(len(data) <= maximum, 'Package file exceeds budget')
        require(identity(os.fstat(fd)) == identity(before) == identity(path.lstat()),
                'Package file changed while reading')
        return bytes(data), stat.S_IMODE(before.st_mode)
    finally:
        os.close(fd)


def inventory(root):
    result = set()
    pending = [root]
    entries = 0
    while pending:
        directory = pending.pop()
        require(not directory.is_symlink() and directory.is_dir(), 'Unsafe package directory')
        for path in directory.iterdir():
            entries += 1
            require(entries <= 4096, 'Package entry budget exceeded')
            info = path.lstat()
            require(not stat.S_ISLNK(info.st_mode), 'Package links are not allowed: ' + str(path))
            if stat.S_ISDIR(info.st_mode):
                require(not info.st_mode & 0o022, 'Writable shared package directory')
                pending.append(path)
            else:
                require(stat.S_ISREG(info.st_mode), 'Package contains a special file')
                result.add(path.relative_to(root).as_posix())
                require(len(result) <= 4096, 'Package entry budget exceeded')
    return result


def verify(root):
    root = Path(root).absolute()
    require(root.resolve(strict=True) == root, 'Use the real package directory, without symlinks')
    raw, _ = read_regular(root / 'package-manifest.json', 4 * 1024 * 1024)
    manifest = json.loads(raw, object_pairs_hook=unique_object)
    require(manifest['schema'] == 1 and manifest['product'] == 'ShellBot' and
            manifest['channel'] == 'acceptance-candidate' and
            manifest['manual_acceptance_complete'] is False, 'Unsupported candidate manifest')
    actual = inventory(root)
    expected = {}
    total = 0
    for row in manifest['files']:
        name = row['path']
        path = PurePosixPath(name)
        require(name == str(path) and not path.is_absolute() and '..' not in path.parts and
                '\\' not in name and not any(ord(c) < 32 for c in name) and
                name not in ('package-manifest.json', 'SHA256SUMS') and name not in expected,
                'Invalid or duplicate payload path')
        require(re.fullmatch('[a-f0-9]{64}', row['sha256']), 'Invalid payload digest')
        require(type(row['bytes']) is int and row['bytes'] >= 0, 'Invalid file size')
        total += row['bytes']
        require(total <= 512 * 1024 * 1024, 'Package byte budget exceeded')
        data, mode = read_regular(root / name)
        require(len(data) == row['bytes'] and mode == row['mode'] and
                hashlib.sha256(data).hexdigest() == row['sha256'], 'Changed payload: ' + name)
        expected[name] = row['sha256']
    require(actual == inventory(root) == set(expected) | {'package-manifest.json', 'SHA256SUMS'},
            'Package has missing or unexpected files')
    sums, _ = read_regular(root / 'SHA256SUMS', 1024 * 1024)
    recorded = {}
    for line in sums.decode('utf-8').splitlines():
        match = re.fullmatch(r'([a-f0-9]{64})  (.+)', line)
        require(match is not None and match[2] not in recorded, 'Invalid checksum list')
        recorded[match[2]] = match[1]
    require(recorded == dict(expected, **{'package-manifest.json': hashlib.sha256(raw).hexdigest()}),
            'Checksum list does not match manifest')
    return manifest


def runtime_check(root, manifest):
    system = 'macos' if sys.platform == 'darwin' else 'linux' if sys.platform == 'linux' else ''
    require(manifest['platform'] == system + '-x64' and platform.machine() == 'x86_64',
            'This package requires the matching Linux or Intel Mac x86_64 system')
    if system == 'macos':
        require(tuple(map(int, platform.mac_ver()[0].split('.'))) >= (13, 0), 'macOS 13+ is required')
        if manifest['build']['signature_mode'] != 'none':
            for name in ('bin/ShellBot', 'bin/ShellBot-RestrictedProbe', 'bin/libpdfium.dylib', 'ShellBot.app'):
                subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(root / name)], check=True,
                               capture_output=True, timeout=30)
    else:
        for name in ('ShellBot', 'ShellBot-RestrictedProbe', 'libpdfium.so'):
            result = subprocess.run(['ldd', str(root / 'bin' / name)], capture_output=True,
                                    text=True, timeout=30)
            require(result.returncode == 0 and 'not found' not in result.stdout + result.stderr,
                    'Missing native libraries for ' + name + ': ' + result.stdout + result.stderr)
    result = subprocess.run([str(root / 'bin/ShellBot'), '--version'], check=True,
                            capture_output=True, text=True, timeout=15)
    require(result.stdout.startswith('ShellBot ' + manifest['version'] + ' '), 'Wrong executable version')
    return result.stdout.strip()


def verify_archive(path):
    files, raw, top, total, seen = {}, {}, None, 0, set()
    with tarfile.open(path, 'r:*') as archive:
        for count, member in enumerate(archive):
            require(count < 4096, 'Archive entry budget exceeded')
            name = member.name.rstrip('/')
            parsed = PurePosixPath(name)
            require(name not in seen, 'Duplicate archive entry')
            seen.add(name)
            require(name == str(parsed) and not parsed.is_absolute() and '..' not in parsed.parts and
                    '\\' not in name and len(parsed.parts) >= 1, 'Unsafe archive path')
            top = top or parsed.parts[0]
            require(parsed.parts[0] == top and not member.mode & 0o022, 'Unsafe archive root or permissions')
            require(not member.issym() and not member.islnk() and (member.isdir() or member.isfile()),
                    'Archive links and special files are not allowed')
            if member.isdir():
                continue
            relative = '/'.join(parsed.parts[1:])
            require(relative and relative not in files and 0 <= member.size <= 64 * 1024 * 1024, 'Invalid archive file')
            total += member.size
            require(total <= 512 * 1024 * 1024, 'Archive byte budget exceeded')
            stream = archive.extractfile(member)
            require(stream is not None, 'Missing archive bytes')
            digest, length, collected = hashlib.sha256(), 0, bytearray()
            with stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
                    length += len(block)
                    if relative in ('package-manifest.json', 'SHA256SUMS'):
                        require(length <= 4 * 1024 * 1024, 'Archive metadata budget exceeded')
                        collected.extend(block)
            require(length == member.size, 'Truncated archive file')
            files[relative] = dict(sha256=digest.hexdigest(), bytes=length, mode=member.mode)
            if collected:
                raw[relative] = bytes(collected)
    manifest = json.loads(raw['package-manifest.json'], object_pairs_hook=unique_object)
    require(manifest['schema'] == 1 and manifest['product'] == 'ShellBot' and
            manifest['channel'] == 'acceptance-candidate' and manifest['manual_acceptance_complete'] is False,
            'Unsupported archive manifest')
    expected = {row['path']: {key: row[key] for key in ('sha256', 'bytes', 'mode')} for row in manifest['files']}
    require(all(type(row['bytes']) is int and row['bytes'] >= 0 and type(row['mode']) is int and
                re.fullmatch('[a-f0-9]{64}', row['sha256']) for row in manifest['files']), 'Invalid archive file metadata')
    require(len(expected) == len(manifest['files']) and
            {name: value for name, value in files.items() if name not in raw} == expected and
            set(files) == set(expected) | {'package-manifest.json', 'SHA256SUMS'}, 'Archive manifest mismatch')
    sums = {}
    for line in raw['SHA256SUMS'].decode('utf-8').splitlines():
        match = re.fullmatch(r'([a-f0-9]{64})  (.+)', line)
        require(match is not None and match[2] not in sums, 'Invalid archive checksums')
        sums[match[2]] = match[1]
    require(sums == {name: row['sha256'] for name, row in files.items() if name != 'SHA256SUMS'}, 'Archive checksum mismatch')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument('--check-runtime', action='store_true', help='Also check native libraries and --version')
    parser.add_argument('--archive', type=Path, help='Verify a tar archive without extracting or executing it')
    args = parser.parse_args()
    try:
        require(not (args.archive and args.check_runtime), 'Archive verification does not execute native code')
        manifest = verify_archive(args.archive) if args.archive else verify(args.directory)
        version = runtime_check(args.directory, manifest) if args.check_runtime else None
        print(json.dumps(dict(status='passed', files=len(manifest['files']), platform=manifest['platform'],
                              channel=manifest['channel'], version=version, manual_acceptance_complete=False)))
        return 0
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError, subprocess.SubprocessError) as error:
        print('Package verification failed: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())

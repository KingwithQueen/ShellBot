#!/usr/bin/env python3
"""Create a native, verified acceptance candidate. Never publishes or installs services."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tarfile

sys.dont_write_bytecode = True


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    try:
        return subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True, timeout=180).stdout
    except subprocess.CalledProcessError as error:
        raise ValueError('Native command failed: ' + ' '.join(map(str, args[:2])) + '\n' + error.stderr + error.stdout) from error


def load_module(path):
    spec = importlib.util.spec_from_file_location('shellbot_package_verify', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expected_files(source, mac, signed):
    docs = ('BACKEND', 'OAUTH', 'DESKTOP', 'REMOTE_DESKTOP', 'MEDIA', 'STARTUP', 'COMMANDS',
            'PROJECTS', 'ACCEPTANCE', 'DISTRIBUTION')
    examples = ('desktop', 'projects', 'commands', 'oauth', 'media', 'restricted', 'admin')
    result = {'bin/ShellBot', 'bin/ShellBot-RestrictedProbe', 'bin/libpdfium.' + ('dylib' if mac else 'so'),
              'share/ShellBot/posix-build.json', 'README.md', 'verify-package.py'}
    result |= {'share/ShellBot/POSIX_' + name + '.md' for name in docs}
    result |= {'share/ShellBot/examples/posix-' + name + '.example.json' for name in examples}
    result |= {'share/ShellBot/licenses/' + name for name in ('LICENSE.txt', 'yyjson-LICENSE', 'civetweb-LICENSE.md', 'stb-LICENSE')}
    platform = 'mac-x64' if mac else 'linux-x64'
    licenses = {p.name for p in (source / 'third_party/pdfium' / platform / 'licenses').iterdir()}
    result |= {'share/ShellBot/licenses/pdfium/' + name for name in licenses | {'LICENSE'}}
    if mac:
        result |= {'ShellBot.app/Contents/' + name for name in ('Info.plist', 'MacOS/ShellBot',
                   'MacOS/ShellBot-RestrictedProbe', 'MacOS/libpdfium.dylib', 'Resources/ShellBot.icns', 'Resources/shellbot-icon-small.png')}
        result |= {'ShellBot.app/Contents/Resources/licenses/' + name for name in ('LICENSE.txt', 'yyjson-LICENSE', 'civetweb-LICENSE.md', 'stb-LICENSE')}
        result |= {'ShellBot.app/Contents/Resources/licenses/pdfium/' + name for name in licenses | {'LICENSE'}}
        if signed:
            result.add('ShellBot.app/Contents/_CodeSignature/CodeResources')
    else:
        result |= {'install-desktop.py', 'share/applications/org.shellbot.desktop.desktop',
                   'share/icons/hicolor/512x512/apps/shellbot.png', 'share/ShellBot/shellbot-icon-small.png'}
    return result


def linkage(stage, names, mac):
    result = []
    versions = set()
    for name in names:
        binary = stage / name
        output = run(['/usr/bin/otool', '-L', binary] if mac else ['ldd', binary])
        if mac:
            deps = [line.strip().split(' (', 1)[0] for line in output.splitlines()[1:]]
            # LC_ID_DYLIB identifies this image; it is not a dependency.
            if binary.suffix == '.dylib':
                identifiers = run(['/usr/bin/otool', '-D', binary]).splitlines()[1:]
                require(len(identifiers) == 1, 'Missing or ambiguous dylib install name: ' + name)
                deps = [dep for dep in deps if dep != identifiers[0].strip()]
            require(all(dep.startswith(('/usr/lib/', '/System/Library/')) for dep in deps),
                    'Non-system Mac linkage must be bundled explicitly: ' + output)
        else:
            require('not found' not in output, 'Unresolved native dependency: ' + output)
            deps = [line.strip().split(' (', 1)[0] for line in output.splitlines()]
            info = run(['readelf', '--version-info', binary])
            versions.update(re.findall(r'\bGLIBC_(\d+(?:\.\d+)+)', info))
        result.append(dict(path=name, libraries=deps))
    return dict(images=result, glibc_required=max(versions, key=lambda s: tuple(map(int, s.split('.')))) if versions else None,
                linux_kernel_for_commands='5.3' if not mac else None,
                macos_minimum='13.0' if mac else None,
                other_distributions_tested=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--source-revision', required=True)
    parser.add_argument('--source-dirty', choices=('true', 'false'), required=True)
    args = parser.parse_args()
    try:
        source = Path(__file__).resolve().parent.parent
        build = args.build_dir.absolute()
        require(build.resolve(strict=True) == build, 'Build directory must be canonical')
        require(sys.platform in ('darwin', 'linux'), 'Package on the native Linux or Mac host')
        mac = sys.platform == 'darwin'
        require(re.fullmatch('[a-f0-9]{40}', args.source_revision), 'A complete source commit is required')
        manifest_path = source / 'SOURCE_SHA256SUMS'
        require(manifest_path.is_file(), 'An explicit SOURCE_SHA256SUMS snapshot is required')
        for line in manifest_path.read_text(encoding='utf-8').splitlines():
            digest, name = line.split('  ', 1)
            require(re.fullmatch('[a-f0-9]{64}', digest) and not Path(name).is_absolute() and
                    '..' not in Path(name).parts and sha(source / name) == digest, 'Source snapshot changed: ' + name)
        cache = dict()
        for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
            if '=' in line and ':' in line.split('=', 1)[0]:
                key, value = line.split('=', 1)
                cache[key.split(':')[0]] = value
        require(Path(cache['CMAKE_HOME_DIRECTORY']) == source and cache['CMAKE_BUILD_TYPE'] == 'Release' and
                cache['SHELLBOT_TESTS'] == 'OFF' and cache['SHELLBOT_SANITIZERS'] == 'OFF' and
                cache['SHELLBOT_DESKTOP'] == 'ON', 'Fresh native Release desktop production build required')
        require(not any((build / name).exists() for name in ('sb_admin_contract', 'sb_broker_contract',
                    'sb_admin_ui_contract', 'sb_service_contract', 'sb_desktop_contract')), 'Test fixture in production build')
        metadata = json.loads((build / 'posix-build.json').read_text())
        require(metadata['tests'] is False and metadata['sanitizers'] is False and metadata['desktop'] is True and
                metadata['architecture'] == 'x86_64' and metadata['configuration'] == 'Release' and
                metadata['system'] == ('Darwin' if mac else 'Linux'), 'Build metadata mismatch')
        kind = 'macos-x64' if mac else 'linux-x64'
        stem = 'ShellBot-' + metadata['version'] + '-posix.' + str(metadata['posix_build']) + '-candidate-' + kind
        output = args.output_dir.absolute()
        require(output.resolve() == output and not output.is_relative_to(source) and not output.is_relative_to(build),
                'Choose a new output outside the source and build directories')
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
        stage = output / stem
        stage.mkdir(mode=0o755)
        run(['cmake', '--install', build, '--prefix', stage])
        shutil.copyfile(source / 'docs/POSIX_DISTRIBUTION.md', stage / 'README.md')
        shutil.copyfile(source / 'PosixShellBot/distribution/verify-package.py', stage / 'verify-package.py')
        if not mac:
            shutil.copyfile(source / 'PosixShellBot/distribution/install-desktop.py', stage / 'install-desktop.py')
        signed = metadata['signature_mode'] != 'none'
        verifier = load_module(stage / 'verify-package.py')
        actual = verifier.inventory(stage)
        require(actual == expected_files(source, mac, signed), 'Unexpected or missing payload: ' +
                repr(actual ^ expected_files(source, mac, signed)))
        library = 'libpdfium.dylib' if mac else 'libpdfium.so'
        vendor = source / 'third_party/pdfium' / ('mac-x64' if mac else 'linux-x64') / library
        require(sha(vendor) == metadata['pdfium_source_sha256'] and
                sha(stage / 'bin' / library) == metadata['pdfium_runtime_sha256'], 'PDFium provenance mismatch')
        images = ['bin/ShellBot', 'bin/ShellBot-RestrictedProbe', 'bin/' + library]
        signing = dict(mode=metadata['signature_mode'], notarization='not-performed', gatekeeper_acceptance='pending')
        if mac:
            require(metadata['macos_minimum'] == '13.0', 'This candidate lane requires macOS deployment target 13.0')
            info = plistlib.loads((stage / 'ShellBot.app/Contents/Info.plist').read_bytes())
            require(info['CFBundleShortVersionString'] == metadata['version'] and
                    info['CFBundleVersion'] == str(metadata['posix_build']) and info['LSMinimumSystemVersion'] == '13.0',
                    'App version or minimum system mismatch')
            require(sha(stage / 'ShellBot.app/Contents/MacOS' / library) == metadata['pdfium_runtime_sha256'], 'App PDFium pin mismatch')
            images += ['ShellBot.app/Contents/MacOS/' + name for name in ('ShellBot', 'ShellBot-RestrictedProbe', library)]
            if signed:
                for name in images + ['ShellBot.app']:
                    run(['/usr/bin/codesign', '--verify', '--strict', stage / name])
                run(['/usr/bin/codesign', '--verify', '--deep', '--strict', stage / 'ShellBot.app'])
                signing['hardened_runtime'] = metadata['signature_mode'] == 'identity'
        for name in ('bin/ShellBot', 'ShellBot.app/Contents/MacOS/ShellBot') if mac else ('bin/ShellBot',):
            require(run([stage / name, '--version']).startswith('ShellBot ' + metadata['version'] + ' '), 'Executable version mismatch')
        requirements = linkage(stage, images, mac)
        files = []
        for name in sorted(actual):
            path = stage / name
            mode = stat.S_IMODE(path.stat().st_mode)
            require(not mode & 0o022, 'Payload is shared writable')
            if name in images and not name.endswith(('.so', '.dylib')):
                require(mode & 0o111, 'Executable mode missing')
            files.append(dict(path=name, sha256=sha(path), bytes=path.stat().st_size, mode=mode))
        manifest = dict(schema=1, product='ShellBot', version=metadata['version'], posix_build=metadata['posix_build'],
                        platform=kind, channel='acceptance-candidate', manual_acceptance_complete=False,
                        source=dict(commit=args.source_revision, working_tree_changes=args.source_dirty == 'true',
                                    manifest_sha256=sha(manifest_path)), build=metadata, signing=signing,
                        requirements=requirements, files=files)
        encoded = json.dumps(manifest, ensure_ascii=False, indent=2).encode('utf-8') + b'\n'
        (stage / 'package-manifest.json').write_bytes(encoded)
        sums = {row['path']: row['sha256'] for row in files}
        sums['package-manifest.json'] = sha(stage / 'package-manifest.json')
        (stage / 'SHA256SUMS').write_bytes(''.join(sums[name] + '  ' + name + '\n' for name in sorted(sums)).encode('utf-8'))
        verifier.verify(stage)
        verifier.runtime_check(stage, manifest)
        archive = output / (stem + '.tar.gz')
        with tarfile.open(archive, 'w:gz', format=tarfile.PAX_FORMAT) as tar:
            for path in sorted(stage.rglob('*')):
                require(not path.is_symlink(), 'Package link refused')
                tar.add(path, arcname=stem + '/' + path.relative_to(stage).as_posix(), recursive=False)
        verifier.verify_archive(archive)
        digest = sha(archive)
        (output / (archive.name + '.sha256')).write_bytes((digest + '  ' + archive.name + '\n').encode('ascii'))
        receipt = dict(status='passed', package=archive.name, sha256=digest, payload_directory=stem,
                       payload_files=len(files), manifest_sha256=sha(stage / 'package-manifest.json'),
                       source=manifest['source'], platform=kind, signing=signing, manual_acceptance_complete=False)
        (output / 'package-receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
        print(json.dumps(receipt))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('Candidate packaging failed; partial output preserved: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())

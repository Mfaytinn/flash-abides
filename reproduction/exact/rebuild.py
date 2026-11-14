#!/usr/bin/env python3
"""Rebuild the October 2026 Final binary using the preserved source and profiles.

Requires the original absolute workspace and Ubuntu host inputs; see README.md.
Never retrains PGO or packages team claims.
"""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import tarfile
from pathlib import Path, PurePosixPath

WORKSPACE = Path('/home/verisalis003_gmail_com/compiler-pgo-gokturk-20261013')
EXPECTED = 'a0ffe98228ae20d8915843516ca970a3fb39287ccaa83fd83f3613be96351369'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def canonical_sources(metadata, archive_path):
    """Verify the archive digest, exact file set, safe names/types and source hashes."""
    expected = metadata['source_sha256']
    for name in expected:
        path = PurePosixPath(name)
        if (str(path) != name or '..' in path.parts or
                not (name == 'engine/CMakeLists.txt' or
                     name.startswith(('engine/src/', 'engine/include/')))):
            raise ValueError(f'unsafe manifest source path: {name}')
    sources = {}
    # Hash this same open file BEFORE parsing or extracting anything from it.
    with archive_path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != metadata['canonical_source_archive']['sha256']:
            raise ValueError('canonical source archive SHA-256 mismatch')
        stream.seek(0)
        with tarfile.open(fileobj=stream, mode='r:gz') as archive:
            for member in archive:
                if (member.name not in expected or member.name in sources or
                        member.type != tarfile.REGTYPE or member.sparse is not None):
                    raise ValueError(f'unexpected source archive member: {member.name}')
                with archive.extractfile(member) as source:
                    data = source.read()
                if hashlib.sha256(data).hexdigest() != expected[member.name]:
                    raise ValueError(f'archived source mismatch: {member.name}')
                sources[member.name] = data
    if sources.keys() != expected.keys():
        raise ValueError('canonical source archive membership mismatch')
    return sources


def restore_canonical_sources(metadata, archive_path):
    sources = canonical_sources(metadata, archive_path)
    destination = WORKSPACE / 'engine'
    # Refuse a pre-existing tree (including symlinks or unlisted source files).
    destination.mkdir()
    for name, data in sources.items():
        path = destination / name.removeprefix('engine/')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', type=Path, required=True,
                        help='reproduction/manifest.json; canonical archive must be alongside it')
    args = parser.parse_args()
    if platform.machine() != 'x86_64':
        parser.error('requires an x86_64 Linux build host, not an ARM native build')
    metadata = json.loads(args.metadata.read_text())
    if metadata['variants']['gcc']['binary_sha256'] != EXPECTED:
        parser.error('metadata does not identify the submitted binary')
    archive_path = args.metadata.parent / metadata['canonical_source_archive']['path']
    try:
        restore_canonical_sources(metadata, archive_path)
    except (OSError, ValueError, tarfile.TarError) as error:
        parser.error(str(error))
    for name, digest in metadata['source_sha256'].items():
        if sha(WORKSPACE / name) != digest:
            parser.error(f'source mismatch: {name}')
    for name, digest in metadata['variants']['gcc']['profile_files'].items():
        if sha(WORKSPACE / 'gcc' / name) != digest:
            parser.error(f'PGO profile mismatch: {name}')
    gcc = WORKSPACE / 'toolchains/gcc/usr'
    # Installed AFTER the submission was built on the original VM. Its presence
    # silently selects a different static runtime despite the GCC 13 header flags.
    if (gcc / 'lib/gcc/x86_64-linux-gnu/16/libstdc++.a').exists():
        parser.error('GCC 16 libstdc++.a must be absent; use the preserved toolchain archive')
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = str(WORKSPACE / 'toolchains/binutils/usr/lib/x86_64-linux-gnu')
    env['COMPILER_PATH'] = str(WORKSPACE / 'toolchains/binutils/usr/bin')
    env['PATH'] = env['COMPILER_PATH'] + os.pathsep + env['PATH']
    compiler = gcc / 'bin/x86_64-linux-gnu-g++-16'
    version = subprocess.check_output([str(compiler), '--version'], env=env, text=True)
    if version != metadata['compilers']['gcc']:
        parser.error('compiler version does not match submission metadata')
    flags = ('-O3 -g -DNDEBUG -fno-fast-math -ffp-contract=off -fno-pie '
             '-fcf-protection=none -fno-stack-protector -nostdinc++ '
             '-isystem /usr/include/c++/13 -isystem /usr/include/x86_64-linux-gnu/c++/13 '
             '-isystem /usr/include/c++/13/backward '
             f'-B{WORKSPACE}/toolchains/binutils/usr/bin/ -flto=4 ')
    profile = (f'-fprofile-use={WORKSPACE}/gcc/raw -fprofile-correction '
               '-Werror=missing-profile -Werror=coverage-mismatch -Wno-error=missing-profile')
    link = (f'-no-pie -L{gcc}/lib/gcc/x86_64-linux-gnu/16 '
            '-L/usr/lib/gcc/x86_64-linux-gnu/13 -flto=4 ' + profile)
    build = WORKSPACE / 'gcc/build'
    subprocess.run(['cmake', '-G', 'Unix Makefiles', '-S', str(WORKSPACE / 'engine'),
                    '-B', str(build), f'-DCMAKE_CXX_COMPILER={compiler}',
                    f'-DCMAKE_AR={gcc}/bin/x86_64-linux-gnu-gcc-ar-16',
                    f'-DCMAKE_RANLIB={gcc}/bin/x86_64-linux-gnu-gcc-ranlib-16',
                    '-DCMAKE_BUILD_TYPE=Release', '-DT3_STATIC=ON', '-DT3_NATIVE=OFF',
                    '-DCMAKE_CXX_FLAGS_RELEASE=' + flags + profile,
                    '-DCMAKE_EXE_LINKER_FLAGS=' + link], env=env, check=True)
    subprocess.run(['cmake', '--build', str(build), '--target', 'simulate',
                    '--clean-first', '--parallel', '4'], env=env, check=True)
    actual = sha(build / 'simulate')
    if actual != EXPECTED:
        raise SystemExit(f'NOT byte-identical: {actual}; expected {EXPECTED}')
    print(f'Byte-identical: {actual}  {build / "simulate"}')


if __name__ == '__main__':
    main()

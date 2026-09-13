"""Build an isolated, fingerprinted reader package without deploying to an engine.

Build only the shared reader target. The package is marked development/unvalidated
until the engine parity checks pass. Source changes during a build reject staging.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_inventory(root):
    paths = [root / 'CMakeLists.txt']
    for folder in ('src', 'include', 'cmake'):
        for directory, names, leaves in os.walk(root / folder, followlinks=False):
            names[:] = sorted(n for n in names if not (Path(directory) / n).is_symlink()
                              and not (Path(directory) / n).is_junction())
            paths.extend(Path(directory) / name for name in leaves
                         if not (Path(directory) / name).is_symlink())
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(paths)}


def run(command, log):
    with log.open('w', encoding='utf-8') as stream:
        stream.write(json.dumps([str(x) for x in command]) + '\n')
        stream.flush()
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}). See {log}\n'
                           + '\n'.join(log.read_text(encoding='utf-8', errors='replace').splitlines()[-25:]))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--out', type=Path, required=True, help='New run directory; existing directories are rejected')
    p.add_argument('--cmake', default='cmake')
    p.add_argument('--generator', default='Visual Studio 18 2026')
    p.add_argument('--parallel', type=int, default=2)
    args = p.parse_args()
    root, out = args.source.resolve(), args.out.resolve()
    if not (root / 'include/bf6_core.h').is_file():
        raise ValueError('Not a reader source directory')
    if args.parallel < 1:
        raise ValueError('--parallel must be positive')
    out.mkdir(parents=True, exist_ok=False)
    before = source_inventory(root)
    (out / 'source-before.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
    build = out / 'build'
    run([args.cmake, '-S', str(root), '-B', str(build), '-G', args.generator,
         '-A', 'x64', '-DBUILD_TESTING=OFF'], out / 'configure.log')
    print('Configured; building bf6_core only.', flush=True)
    run([args.cmake, '--build', str(build), '--config', 'Release', '--target', 'bf6_core',
         '--parallel', str(args.parallel)], out / 'build.log')
    after = source_inventory(root)
    (out / 'source-after.json').write_text(json.dumps(after, indent=2), encoding='utf-8')
    if before != after:
        raise RuntimeError('Reader source changed during build. Output retained for diagnosis; package not staged.')
    package = out / 'package'
    (package / 'bin').mkdir(parents=True)
    (package / 'include').mkdir()
    (package / 'lib').mkdir()
    for source, target in ((build / 'Release/bf6_core.dll', package / 'bin/bf6_core.dll'),
                           (build / 'Release/bf6_core.lib', package / 'lib/bf6_core.lib'),
                           (root / 'include/bf6_core.h', package / 'include/bf6_core.h')):
        shutil.copy2(source, target)
    # Header and source may be edited while staging, too.
    if before != source_inventory(root):
        raise RuntimeError('Source changed during staging. No valid package manifest was written.')
    header = (package / 'include/bf6_core.h').read_text(encoding='utf-8-sig')
    header = re.sub(r'/\*.*?\*/|//[^\n]*', '', header, flags=re.S)
    abi = int(re.search(r'#define\s+BF6_ABI_VERSION\s+(\d+)', header)[1])
    declared = sorted(set(re.findall(r'BF6_API\s+[^;{}]*?\b(bf6_\w+)\s*\(', header)))
    # Probe only the ABI getter and symbol presence in a separate process.
    # Do not call game-reading or struct-bearing APIs during package validation.
    probe = ('import ctypes,json,sys; d=ctypes.CDLL(sys.argv[1]); '
             'd.bf6_abi_version.restype=ctypes.c_int; '
             'print(json.dumps({"abi":d.bf6_abi_version(), '
             '"missing":[n for n in json.loads(sys.argv[2]) if not hasattr(d,n)]}))')
    result = subprocess.run([sys.executable, '-c', probe, str(package / 'bin/bf6_core.dll'),
                             json.dumps(declared)], capture_output=True, text=True, timeout=30, check=True)
    verified = json.loads(result.stdout)
    if verified['abi'] != abi or verified['missing']:
        raise RuntimeError(f'Header/binary verification failed: {verified}')
    contents = {f.relative_to(package).as_posix(): {'bytes': f.stat().st_size, 'sha256': sha(f)}
                for f in sorted(package.rglob('*')) if f.is_file()}
    manifest = {'schema': 1, 'created_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'status': 'development-engine-parity-unvalidated', 'abi': abi,
                'architecture': 'x64', 'configuration': 'Release', 'source_root': str(root),
                'source_files': before, 'files': contents, 'declared_exports_checked': len(declared),
                'validation': {'abi_getter_matches_header': True, 'all_declared_symbols_present': True,
                               'engine_parity': 'not-run', 'struct_layout_parity': 'not-run'},
                'deployment': 'none'}
    (package / 'reader-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'package': str(package), 'abi': abi, 'exports_checked': len(declared),
                      'engine_parity': 'not-run', 'deployed': False}), flush=True)


if __name__ == '__main__':
    main()

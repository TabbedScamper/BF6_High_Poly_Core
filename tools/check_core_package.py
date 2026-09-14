"""Check a reader package against the exact header an adapter will compile with.

Exit 2 means incompatible/incomplete. This is a development consistency check,
not a package signature verifier or engine parity certification. Never deploys.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check(package, consumer_header):
    errors = []
    manifest = json.loads((package / 'reader-manifest.json').read_text(encoding='utf-8'))
    required = {'bin/bf6_core.dll', 'lib/bf6_core.lib', 'include/bf6_core.h'}
    listed = set(manifest.get('files', {}))
    # The binary, its import library and the public headers; nothing else.
    expected = required | {n for n in listed if re.fullmatch(r'include/bf6_\w+\.h', n)}
    if listed != expected:
        errors.append('Package file manifest does not match the required allowlist.')
    for name in sorted(expected):
        item, record = package / name, manifest.get('files', {}).get(name, {})
        if not item.is_file() or item.is_symlink():
            errors.append(f'Missing or linked package file: {name}')
        elif item.stat().st_size != record.get('bytes') or digest(item) != record.get('sha256'):
            errors.append(f'Package hash/size mismatch: {name}')
    source = consumer_header.read_text(encoding='utf-8-sig')
    abi = re.search(r'^\s*#define\s+BF6_ABI_VERSION\s+(\d+)', source, re.M)
    consumer_abi = int(abi[1]) if abi else None
    if consumer_abi != manifest.get('abi'):
        errors.append(f'ABI mismatch: consumer {consumer_abi}, package {manifest.get("abi")}.')
    if digest(consumer_header) != manifest.get('files', {}).get('include/bf6_core.h', {}).get('sha256'):
        errors.append('Consumer header is not the exact packaged header; rebuild the adapter against the pinned package.')
    validation = manifest.get('validation', {})
    if not validation.get('abi_getter_matches_header') or not validation.get('all_declared_symbols_present'):
        errors.append('Package has no successful binary/header verification record.')
    return {'consistent': not errors, 'package': str(package), 'consumer_header': str(consumer_header),
            'errors': errors, 'engine_parity': validation.get('engine_parity', 'not-run'),
            'release_ready': False, 'note': 'Consistency alone does not approve deployment or release.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--consumer-header', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = check(args.package.resolve(), args.consumer_header.resolve())
    except (OSError, ValueError, TypeError, KeyError) as error:
        result = {'consistent': False, 'errors': [str(error)], 'release_ready': False}
    print(json.dumps(result, indent=2))
    return 0 if result['consistent'] else 2


if __name__ == '__main__':
    raise SystemExit(main())

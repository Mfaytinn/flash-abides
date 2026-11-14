#!/usr/bin/env python3
"""Reuse a signed submission only if its immutable image contains our rebuilt binary."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--submission', type=Path, required=True)
    parser.add_argument('--binary', type=Path, default=Path('build/cross-final/build/simulate'))
    parser.add_argument('--out', type=Path, default=Path('dist/submission-final.zip'))
    parser.add_argument('--container', default='docker')
    args = parser.parse_args()
    if args.out.exists():
        parser.error(f'refusing to overwrite {args.out}')
    with zipfile.ZipFile(args.submission) as archive:
        if sorted(archive.namelist()) != ['submission.json', 'team-claim.json']:
            parser.error('expected exactly submission.json and team-claim.json')
        descriptor = json.loads(archive.read('submission.json'))
        json.loads(archive.read('team-claim.json'))
    image = descriptor['image']
    digest = image['digest']
    if not digest.startswith('sha256:') or len(digest) != 71:
        parser.error('submission must reference an immutable sha256 image digest')
    reference = f"{image['registry']}/{image['repository']}@{digest}"
    subprocess.run([args.container, 'pull', '--platform', 'linux/amd64', reference], check=True)
    container = subprocess.check_output(
        [args.container, 'create', '--platform', 'linux/amd64', reference], text=True).strip()
    try:
        with tempfile.TemporaryDirectory() as directory:
            published = Path(directory) / 'simulate'
            subprocess.run([args.container, 'cp', f'{container}:/simulate', str(published)], check=True)
            if sha(args.binary) != sha(published):
                parser.error('rebuilt binary differs from the signed submission image; '
                             'publish the new image and seal a new descriptor/claim first')
    finally:
        subprocess.run([args.container, 'rm', container], check=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Preserve the signed claim and ZIP bytes; never invent credentials or claims.
    shutil.copyfile(args.submission, args.out)
    print(f'{sha(args.out)}  {args.out}')


if __name__ == '__main__':
    main()

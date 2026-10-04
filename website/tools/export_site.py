#!/usr/bin/env python3
"""Copy the successfully published CMake site to its distribution directory."""
import argparse
from pathlib import Path
import shutil
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--destination', required=True, type=Path)
    args = parser.parse_args()
    source, destination = args.source.resolve(), args.destination.resolve()
    if not (source / 'site-manifest.json').is_file() or not (source / 'index.html').is_file():
        raise SystemExit('The source is not a published website')
    if source == destination or source in destination.parents or destination in source.parents:
        raise SystemExit('Source and export directories must not overlap')
    if destination.exists() and any(destination.iterdir()) and not (destination / 'site-manifest.json').is_file():
        raise SystemExit('Refusing to replace a directory that is not a website export')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.website-export-', dir=destination.parent) as temporary:
        staged = Path(temporary) / 'site'
        shutil.copytree(source, staged)
        previous = Path(temporary) / 'previous'
        if destination.exists():
            destination.rename(previous)
        try:
            staged.rename(destination)
        except BaseException:
            if previous.exists():
                previous.rename(destination)
            raise
    print(f'Website exported to {destination}')


if __name__ == '__main__':
    main()

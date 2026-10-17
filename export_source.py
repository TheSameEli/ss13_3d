# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 TheSameEli
"""Export a new, source-only directory for a separate repository (never publishes)."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parent
# Deliberate allowlist: no bin/, generated artwork, old packages or user audio.
SOURCE_FILES = (
    'AUTHORS.md', '.gitignore', '.clang-format', '.editorconfig', 'README.md', 'LICENSE', 'THIRD_PARTY.md', 'requirements.txt',
    'compatibility.json', 'abi-516.1688.json', 'byond_abi.h', 'volume.h', 'volume.cpp',
    'first_person.h', 'first_person.cpp', 'lighting_shader.h', 'win_handle.h',
    'probe.cpp', 'launch.cpp', 'asset_utils.py', 'compile_geometry.py',
    'check_asset_geometry.py', 'check_first_person.cpp', 'check_first_person.cmd',
    'test_tools.py', 'decode_scene.py', 'inspect_live_assets.py', 'export_source.py',
    'build.cmd', 'msvc-env.cmd', 'Start-Mod.cmd', 'Start-NativeClient.ps1',
    'docs/ARCHITECTURE.md', 'docs/RENDERING.md', 'docs/KNOWN_ISSUES.md',
    'docs/DEVELOPMENT.md', 'docs/REVIEW.md', 'docs/VALIDATION.md',
)


def export_source(output: Path) -> dict[str, str]:
    output = output.resolve()
    # Refuse existing destinations, including symlinks. No deletion or git action.
    if output.exists() or output.is_symlink():
        raise FileExistsError(f'Refusing to overwrite {output}; choose a new directory')
    sources = [(name, ROOT / name) for name in SOURCE_FILES]
    for _, source in sources:
        if not source.is_file() or source.is_symlink():
            raise ValueError(f'Missing or symlinked source file: {source}')
    output.mkdir(parents=True, exist_ok=False)
    manifest = {}
    for name, source in sources:
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        manifest[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    (output / 'SOURCE_MANIFEST.json').write_text(
        json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'publish/ss13-first-person')
    args = parser.parse_args()
    manifest = export_source(args.output)
    print(f'Exported {len(manifest)} source files to {args.output.resolve()}')
    print('No Git repository was created and nothing was uploaded.')


if __name__ == '__main__':
    main()

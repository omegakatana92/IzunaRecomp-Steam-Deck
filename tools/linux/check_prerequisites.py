#!/usr/bin/env python3
"""Report blockers without downloading dependencies or changing game files."""
import pathlib
import shutil
import sys

root = pathlib.Path(__file__).resolve().parents[2]
checks = {
    'CMake': shutil.which('cmake'),
    'Ninja': shutil.which('ninja'),
    'Clang': shutil.which('clang++-19') or shutil.which('clang++-20') or shutil.which('clang++'),
    'Git': shutil.which('git'),
    'Patched ReXGlue checkout': (root / 'third_party/rexglue-sdk/CMakeLists.txt').is_file(),
    'Own game executable (assets/default.xex)': (root / 'assets/default.xex').is_file(),
    'Generated build integration': (root / 'generated/rexglue.cmake').is_file(),
}
for name, found in checks.items():
    print(f"{'OK' if found else 'MISSING'}: {name}")
print('\nNative desktop port: NOT IMPLEMENTED/VALIDATED.')
print('Existing Linux presets and NG2_PS5_HOST_TEST do not establish Steam Deck compatibility.')
print('See docs/LINUX_STEAM_DECK.md for source blockers and validation requirements.')
sys.exit(0 if all(checks.values()) else 2)

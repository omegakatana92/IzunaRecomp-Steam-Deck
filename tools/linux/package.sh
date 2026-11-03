#!/usr/bin/env bash
# Stage only redistributable inputs. Requires a successfully linked native ELF.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
exe=${1:-$root/out/linux-desktop/ng2}
: "${DXC_SO:?Set DXC_SO to the Linux x86-64 libdxcompiler.so (DXC 1.8)}"
: "${LINUXDEPLOY:?Set LINUXDEPLOY to a local linuxdeploy-x86_64.AppImage}"
[[ -f "$exe" && -f "$DXC_SO" && -f "$LINUXDEPLOY" ]] || { echo 'Missing executable or packaging dependency' >&2; exit 2; }
python3 - "$exe" "$DXC_SO" <<'PY'
import sys,struct
for p in sys.argv[1:]:
    with open(p,'rb') as f: h=f.read(20)
    if h[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H',h,18)[0] != 62:
        raise SystemExit(f'{p}: expected Linux x86-64 ELF, not a Windows or other-platform binary')
PY
version=$(tr -d ' \r\n' < "$root/IZUNA_VERSION")
name="IzunaRecomp-${version}-experimental-linux-x86_64"
dest="$root/out/release"
app="$dest/$name.AppDir"
# Refuse to overwrite an existing package; review or move it before rebuilding.
[[ ! -e "$app" ]] || { echo "$app already exists" >&2; exit 2; }
mkdir -p "$app/usr/bin" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/256x256/apps" "$app/usr/share/doc/izunarecomp"
cp "$exe" "$app/usr/bin/ng2"
cp "$DXC_SO" "$app/usr/bin/libdxcompiler.so"
cp "$root/resources/izuna_256.png" "$app/usr/share/icons/hicolor/256x256/apps/izunarecomp.png"
cp "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$root/docs/LINUX_STEAM_DECK.md" "$app/usr/share/doc/izunarecomp/"
mkdir -p "$app/usr/bin/hub"
cp -r "$root/ps5/package/hub/button_prompts" "$app/usr/bin/hub/"
cat > "$app/usr/share/applications/izunarecomp.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=IzunaRecomp (Experimental)
Comment=Native Ninja Gaiden II recompilation; requires your game data
Exec=ng2
Icon=izunarecomp
Categories=Game;
Terminal=false
DESKTOP
cat > "$app/AppRun" <<'RUN'
#!/bin/sh
set -eu
app=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$app/usr/bin:$app/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$app/usr/bin/ng2" "$@"
RUN
chmod +x "$app/AppRun" "$app/usr/bin/ng2"
# Collect the exact vendored dependency licences; fail rather than silently omit.
python3 - "$root" "$app/usr/share/doc/izunarecomp/licenses" <<'PY'
import pathlib,shutil,sys
root,dest=map(pathlib.Path,sys.argv[1:]);dest.mkdir()
components=[root/'third_party/lua',root/'third_party/zstd',root/'third_party/smol-v',root/'third_party/xenosrecomp',root/'third_party/rexglue-sdk']
components += [p for p in (root/'third_party/rexglue-sdk/thirdparty').iterdir() if p.is_dir() and (p/'.git').exists()]
for p in components:
    files=[f for f in p.iterdir() if f.is_file() and f.name.upper().startswith(('LICENSE','COPYING','COPYRIGHT'))]
    if p.name == 'libmspack' and not files:
        files=[p/'libmspack'/'COPYING.LIB'] if (p/'libmspack'/'COPYING.LIB').is_file() else []
    if not files: raise SystemExit(f'Missing licence files: {p}')
    out=dest/p.name;out.mkdir(exist_ok=True)
    for f in files: shutil.copy2(f,out/f.name)
shutil.copy2(root/'tools/windows/release/dxcompiler-license.txt',dest/'dxcompiler.txt')
PY
# linuxdeploy bundles ELF dependencies, excluding system libc and driver stacks.
# Both packaging tools use extraction mode so the build host does not need FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1
export OUTPUT="$dest/$name.AppImage"
"$LINUXDEPLOY" --appdir "$app" --executable "$app/usr/bin/ng2" \
  --library "$app/usr/bin/libdxcompiler.so" --output appimage
# The portable Linux archive uses the same dependency-complete directory.
tar -C "$dest" -czf "$dest/$name.tar.gz" "$name.AppDir"
(cd "$dest" && sha256sum "$name.AppImage" "$name.tar.gz") > "$dest/$name.sha256"
printf '%s\n' "$dest/$name.AppImage" "$dest/$name.tar.gz"

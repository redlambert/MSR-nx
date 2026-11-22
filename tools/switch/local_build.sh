#!/bin/bash
# MSR-nx: builds msr.nro on your own PC from your own game files and the runtime kit.
#
# Run in devkitPro's MSYS2 terminal (Start, devkitPro, MSYS2), from the repository folder:
#
#     bash tools/switch/local_build.sh <game folder> <kit folder>
#
#   <game folder>  holds code.bin and exheader.bin (made by extract_3ds.py)
#   <kit folder>   the unzipped "msr-kit" download from the "Switch runtime kit" workflow
#
# Shader pack: put the official release's msr.nro in the game folder as official.nro and its
# precompiled shaders are copied into this build, so effects do not compile on the Switch the
# first time they appear. (The renderer and shader tools are unchanged since 0.6.1.)
#
# Needs: devkitPro with switch-dev, and Python 3 with capstone (py -3 -m pip install capstone).
# Game files and everything made from them stay in build/local/ on this PC. Never commit it.
#
# The first build translates and compiles the game's code (about 15 to 60 minutes, depending on
# the PC). After that, a build with a new kit only links, which takes seconds.

set -e

GAME="$1"
KIT="$2"
if [ -z "$GAME" ] || [ -z "$KIT" ]; then
    echo "usage: bash tools/switch/local_build.sh <game folder> <kit folder>"
    exit 1
fi

if [ ! -f runtime/switch/main_nx.c ]; then
    echo "error: run this from the MSR-nx repository folder"
    exit 1
fi

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH
B=build/local

fail() {
    echo
    echo "error: $1"
    exit 1
}

# ---- tools -----------------------------------------------------------------------------------
echo "[1/6] Checking tools"
command -v aarch64-none-elf-gcc > /dev/null || fail "devkitA64 not found. In this terminal run: pacman -S --needed switch-dev"
command -v nacptool > /dev/null || fail "nacptool not found. In this terminal run: pacman -S --needed switch-dev"
command -v elf2nro > /dev/null || fail "elf2nro not found. In this terminal run: pacman -S --needed switch-dev"
[ -f "$DEVKITPRO/libnx/lib/libdeko3d.a" ] || fail "deko3d not found. In this terminal run: pacman -S --needed switch-dev"

PY=""
for p in py /c/Windows/py.exe python3 python; do
    if command -v "$p" > /dev/null 2>&1 && "$p" -c "import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)" > /dev/null 2>&1; then
        PY="$p"
        break
    fi
done
[ -n "$PY" ] || fail "Python 3 not found. Install it from python.org (tick 'Add python.exe to PATH'), then reopen this terminal"
"$PY" -c "import capstone" > /dev/null 2>&1 || fail "capstone not installed. In PowerShell run: py -3 -m pip install capstone"

# ---- inputs ----------------------------------------------------------------------------------
echo "[2/6] Checking the game files and the kit"
[ -f "$GAME/code.bin" ] || fail "no code.bin in $GAME"
[ -f "$GAME/exheader.bin" ] || fail "no exheader.bin in $GAME"
[ -f "$KIT/libuam.a" ] || fail "no libuam.a in $KIT (unzip the msr-kit download there)"
[ -f "$KIT/analyze.py" ] || fail "no analyze.py in $KIT"
ls "$KIT"/nxrt/*.o > /dev/null 2>&1 || fail "no nxrt/*.o in $KIT"

mkdir -p "$B/extract" "$B/gen" "$B/nxobj" "$B/out"
cp "$GAME/code.bin" "$GAME/exheader.bin" "$B/extract/"

"$PY" - "$B/extract" <<'PY'
import hashlib
import importlib.util
import json
import struct
import sys

d = sys.argv[1]
spec = importlib.util.spec_from_file_location('x', 'tools/extract_3ds.py')
x = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x)
for name in ('code.bin', 'exheader.bin'):
    got = hashlib.sha256(open(d + '/' + name, 'rb').read()).hexdigest()
    if got != x.EXPECTED[name]:
        sys.exit('error: ' + name + ' is not from the supported game version (USA)')

exh = open(d + '/exheader.bin', 'rb').read()
code = open(d + '/code.bin', 'rb').read()


def segment(offset):
    address, pages, size = struct.unpack_from('<III', exh, offset)
    return {'address': address, 'pages': pages, 'size': size}


layout = {
    'title': exh[:8].rstrip(b'\0').decode('ascii', 'replace'),
    'program_id': '00040000001BB200',
    'text': segment(0x10),
    'rodata': segment(0x20),
    'data': segment(0x30),
    'bss_size': struct.unpack_from('<I', exh, 0x3C)[0],
    'code_size': len(code),
}
open(d + '/layout.json', 'w').write(json.dumps(layout, indent=2))
print('    game files OK')
PY

# ---- translate (only when the generator or the CPU headers changed) --------------------------
# extra_entries.txt in the game folder (optional): functions the receipt said ran on the interpreter,
# one "0xADDRESS" per line (odd = Thumb), translated like the rest from the next build on
EXTRA=""
if [ -f "$GAME/extra_entries.txt" ]; then EXTRA="$GAME/extra_entries.txt"; fi
STAMP=$(cat tools/gen/*.py runtime/cpu/*.h runtime/os/*.h "$KIT/analyze.py" $EXTRA | sha256sum | cut -c1-16)
if [ -f "$B/gen/table.c" ] && [ "$(cat "$B/stamp" 2>/dev/null)" = "$STAMP" ]; then
    echo "[3/6] Game code already translated: skipping"
else
    echo "[3/6] Translating the game code to C (a few minutes)"
    rm -rf "$B/gen" "$B/nxobj"
    mkdir -p "$B/gen" "$B/nxobj"
    "$PY" "$KIT/analyze.py" "$B/extract" > "$B/analyze.log" || fail "analyze.py failed (see $B/analyze.log)"
    "$PY" tools/gen/generate.py "$B/extract" "$B/extract/functions.csv" "$B/gen" $EXTRA || fail "generate.py failed"
    echo "$STAMP" > "$B/stamp"
fi

# ---- compile the game code (only units not yet compiled) -------------------------------------
echo "[4/6] Compiling the game code (the first time is the long part)"
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
FLAGS="-O2 $ARCH -ffunction-sections -fdata-sections -w -fno-strict-aliasing -ffp-contract=off -D__SWITCH__ -Iruntime/cpu -Iruntime/os -I$B/gen"
export FLAGS B
JOBS=$(nproc)
todo=0
for f in "$B"/gen/unit_*.c "$B/gen/table.c"; do
    o="$B/nxobj/$(basename "$f" .c).o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
        echo "$f"
    fi
done > "$B/todo.txt"
todo=$(wc -l < "$B/todo.txt")
total=$(ls "$B"/gen/unit_*.c "$B/gen/table.c" | wc -l)
echo "    $todo of $total units to compile, $JOBS at a time"
rm -f "$B/failed.txt"
xargs -a "$B/todo.txt" -P "$JOBS" -I{} sh -c '
    f="{}"
    b=$(basename "$f" .c)
    if aarch64-none-elf-gcc $FLAGS -c "$f" -o "$B/nxobj/$b.o.tmp"; then
        mv "$B/nxobj/$b.o.tmp" "$B/nxobj/$b.o"
        echo "    built $b"
    else
        echo "$b" >> "$B/failed.txt"
    fi'
[ ! -f "$B/failed.txt" ] || fail "these units did not compile: $(tr '\n' ' ' < "$B/failed.txt")"

# ---- link ------------------------------------------------------------------------------------
echo "[5/6] Linking msr.nro"
PACKOBJ=""
rm -f "$B/shaders.pack"
if [ -f "$GAME/official.nro" ]; then
    "$PY" - "$GAME/official.nro" "$B/shaders.pack" <<'PY'
import struct
import sys

nro = open(sys.argv[1], 'rb').read()
found = None
start = 0
while True:
    i = nro.find(b'MSRVSPK1', start)
    if i < 0:
        break
    start = i + 1
    if i % 256:
        continue
    count = struct.unpack_from('<I', nro, i + 8)[0]
    if not 0 < count < 100000:
        continue
    end = 0
    ok = True
    last_key = -1
    for k in range(count):
        key, pos, size = struct.unpack_from('<QII', nro, i + 16 + 16 * k)
        if key <= last_key or pos % 256 or size == 0 or i + pos + size > len(nro) or nro[i + pos:i + pos + 4] != b'DKSH':
            ok = False
            break
        last_key = key
        end = max(end, pos + size)
    if ok:
        found = (i, end, count)
        break
if not found:
    print('    no shader pack found in official.nro: building without one')
    sys.exit(0)
i, end, count = found
end = (end + 255) & ~255
open(sys.argv[2], "wb").write(nro[i:i + end])
print('    shader pack copied from official.nro: %d shaders' % count)
PY
fi
if [ -f "$B/shaders.pack" ]; then
    printf '.section .rodata.msr_pack,"a"\n.global msr_pack_data\n.global msr_pack_end\n.balign 256\nmsr_pack_data:\n.incbin "%s"\nmsr_pack_end:\n' "$B/shaders.pack" > "$B/out/pack_blob.S"
    aarch64-none-elf-gcc $ARCH -c "$B/out/pack_blob.S" -o "$B/out/pack_blob.o"
    PACKOBJ="$B/out/pack_blob.o"
else
    echo "    no shader pack (put the official msr.nro in the game folder as official.nro to add one)"
fi
aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs $ARCH -Wl,--gc-sections \
    -o "$B/out/msr.elf" \
    "$KIT"/nxrt/*.o \
    "$B"/nxobj/*.o \
    $PACKOBJ \
    "$KIT/libuam.a" \
    -L$DEVKITPRO/libnx/lib -ldeko3d -lnx -lm -lstdc++
nacptool --create "MSR-nx" "FurryLilManChild" "0.7.0-local" "$B/out/msr.nacp"
elf2nro "$B/out/msr.elf" "$B/out/msr.nro" --nacp="$B/out/msr.nacp" --icon=runtime/switch/res/icon.jpg > /dev/null

echo "[6/6] Done"
echo
echo "    $(pwd)/$B/out/msr.nro"
echo
echo "Copy it to sdmc:/switch/msr/ on the Switch, replacing the old msr.nro."

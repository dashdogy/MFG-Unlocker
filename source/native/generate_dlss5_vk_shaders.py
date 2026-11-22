"""Compile the DLSS 5 Vulkan stage's compute passes (V1.7.0 vkdlss5.1) into RCDATA payloads.

The encode, meter and decode passes are dlss5.cpp's own HLSL (kShader, the D3D12 stage's source): it is read from
dlss5.cpp, so both APIs run one source. Only Vulkan bindings change: the constant buffer becomes a push-constant
struct (its members keep their names through #defines; a clash with any other name fails to compile), the storage
image gets an explicit format per variant (no shaderStorageImage*WithoutFormat feature is needed) and registers map
to bindings t0..t3 -> 0..3, u0 -> 4, u1 -> 5. CopyIn (Vulkan only) copies the game's DLSS output, a storage image of
binding 6 in its own format, into the stage's copy at binding 4 before Encode reads it.

The compiler is the Vulkan SDK 1.4.363.0 dxc (SPIR-V code generation; the Windows SDK dxc has none), pinned by its
version line, and every output is pinned by SHA-256, so a different compiler or an edited source fails the build.
dlss5_vk_spirv.rc lists the passes for pack_embedded_payloads.py (the dlss5_vk payload group; each pass its own
uncompressed resource since V1.7.2 nolzma.1), and
dlss5_vk_spirv.h names each pass's resource id.
"""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess

PINNED_DXC = "dxcompiler.dll: 1.9(5512-01b62ad4)(1.9.0.5512) - 1.9.0.5512 (01b62ad47)"
FIRST_RESOURCE_ID = 7500  # the XeFG payloads use 7000..7482
FLAGS = ["-spirv", "-HV", "2018", "-T", "cs_6_0", "-O3", "-fspv-target-env=vulkan1.1",
         "-fvk-t-shift", "0", "0", "-fvk-u-shift", "4", "0"]

COPY_IN = """
struct CData { uint2 origin; uint2 size; };
[[vk::push_constant]] CData C;
[[vk::binding(6)]] [[vk::image_format("GAME")]] RWTexture2D<float4> game;
[[vk::binding(4)]] [[vk::image_format("OURS")]] RWTexture2D<float4> ours;
// The rect of the game's output into the same texels of ours.
[numthreads(8,8,1)] void CopyIn(uint3 id:SV_DispatchThreadID){
 if(id.x<C.size.x&&id.y<C.size.y){uint2 p=C.origin+id.xy;ours[p]=game[p];}
}
"""

# name -> (entry, source, storage format of binding 4, game format of binding 6, pinned SPIR-V SHA-256)
VARIANTS = {
    "encode": ("Encode", "stage", "rgba16f", None, "95DEF5C58F01340D6C01FB817821F6DE37F8D559E0EE3166DB00F89A19D8FD24"),
    "meter": ("Meter", "stage", "rgba16f", None, "75DA0250347C865C4E90253207B015779CC4061218D6A9AA5F37AE69969F213B"),
    "decode_rgba16f": ("Decode", "stage", "rgba16f", None, "F55ED74C1801B2468169025D21AED2EED359A04487555F669086966AEA41A3A3"),
    "decode_rgba32f": ("Decode", "stage", "rgba32f", None, "6CCA3FDAD2DA0A12D338C9D91D4BFF6F5456F25CAECF9CAF567DC595F6755C2D"),
    "decode_rgba8": ("Decode", "stage", "rgba8", None, "BB853BC7A4022E00D23A41104E10C6993AB60F44A7B94E57BF4A8CFAE79B3A92"),
    "copy_in_rgba16f": ("CopyIn", "copy", "rgba16f", "rgba16f", "7C0DB81DA79311B461364433C631FEC0F3D9D08AFCA248ADAEFCA995CBCB17B0"),
    "copy_in_rgba32f": ("CopyIn", "copy", "rgba32f", "rgba32f", "8780DDAD2B45B844F130BF6068DA7212A1902CE209D4811A2FB76B55D385463A"),
    "copy_in_rgba8": ("CopyIn", "copy", "rgba16f", "rgba8", "EEC51BB968929D378C29CA8085ED021AF179FC628B2DCC286C6F974C2EF5B6E8"),
}

parser = argparse.ArgumentParser()
parser.add_argument("--dxc", type=Path, required=True, help="Vulkan SDK 1.4.363.0 Bin/dxc.exe")
parser.add_argument("--spirv-val", type=Path, help="spirv-val.exe, run on every output when given")
parser.add_argument("--source", type=Path, required=True, help="source/native directory")
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--print-digests", action="store_true", help="report digests instead of enforcing the pins")
args = parser.parse_args()

version = subprocess.run([str(args.dxc), "--version"], capture_output=True, text=True, check=True).stdout.strip()
if version != PINNED_DXC:
    raise SystemExit(f"Unpinned dxc: {version!r} (expected {PINNED_DXC!r})")

text = (args.source / "dlss5.cpp").read_text(encoding="utf-8")
opening = 'const char kShader[]=R"('
start = text.index(opening) + len(opening)
stage = text[start:text.index(')";', start)]
# Issue 169: Wine's D3DCompile (vkd3d-shader) has no isfinite, isnan or isinf, and the D3D12 stage compiles these
# texts at run time. They test ranges with plain comparisons instead (a NaN fails every comparison).
boost_opening = 'const char kBoostShader[]=R"('
boost_start = text.index(boost_opening) + len(boost_opening)
for label, body in (("kShader", stage), ("kBoostShader", text[boost_start:text.index(')";', boost_start)])):
    for intrinsic in ("isfinite(", "isnan(", "isinf("):
        if intrinsic in body:
            raise SystemExit(f"dlss5.cpp {label}: {intrinsic}...) does not compile under Wine's D3DCompile; "
                             "use comparisons")
block = re.search(r"cbuffer C:register\(b0\) \{(.*?)\};", stage, re.S)
if not block:
    raise SystemExit("dlss5.cpp kShader: no cbuffer C:register(b0)")
members = re.findall(r"\b([A-Za-z_]\w*)\s*(?=[,;])", block.group(1))
stage = (stage[:block.start()] + "struct CData {" + block.group(1) + "};\n[[vk::push_constant]] CData C;\n"
         + "".join(f"#define {name} C.{name}\n" for name in members) + stage[block.end():])
target = "RWTexture2D<float4> dst:register(u0);"
if stage.count(target) != 1:
    raise SystemExit("dlss5.cpp kShader: expected one dst:register(u0)")

out = args.output / "dlss5_vk"
out.mkdir(parents=True, exist_ok=True)
header = ["#pragma once", "// Generated by generate_dlss5_vk_shaders.py from dlss5.cpp's kShader; do not edit.",
          "// Resource ids of the passes in the packed dlss5_vk group (dlss5_vk.packed.h).",
          "#include <cstdint>", "namespace dlss5::vulkan::spirv {"]
rc = ["#include <windows.h>", "LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL"]
failures = []
for resource, (name, (entry, kind, storage, game, pinned)) in enumerate(VARIANTS.items(), FIRST_RESOURCE_ID):
    if kind == "stage":
        source = stage.replace(target, f'[[vk::image_format("{storage}")]] ' + target, 1)
    else:
        source = COPY_IN.replace("GAME", game).replace("OURS", storage)
    hlsl = out / f"{name}.hlsl"
    hlsl.write_text(source, encoding="utf-8", newline="\n")
    spv = out / f"{name}.spv"
    subprocess.run([str(args.dxc), *FLAGS, "-E", entry, "-Fo", str(spv), str(hlsl)], check=True)
    if args.spirv_val:
        subprocess.run([str(args.spirv_val), "--target-env", "vulkan1.1", str(spv)], check=True)
    raw = spv.read_bytes()
    digest = hashlib.sha256(raw).hexdigest().upper()
    if args.print_digests:
        print(f"{name} {digest} {len(raw)}")
    elif digest != pinned:
        failures.append(f"{name}: {digest} (pinned {pinned})")
    if not raw or len(raw) % 4:
        failures.append(f"{name}: {len(raw)} bytes is not whole SPIR-V words")
    header.append(f"inline constexpr uint32_t {name} = {resource};")
    rc.append(f'{resource} RCDATA "{spv.resolve().as_posix()}"')
if failures:
    raise SystemExit("DLSS 5 Vulkan SPIR-V drifted; update a pin only with a re-validated shader "
                     "(tests\\dlss5_vk_harness):\n  " + "\n  ".join(failures))
header.append("}")
for name, lines in (("dlss5_vk_spirv.h", header), ("dlss5_vk_spirv.rc", rc)):
    text = "\n".join(lines) + "\n"
    target = args.output / name
    if not target.exists() or target.read_text(encoding="utf-8") != text:
        target.write_text(text, encoding="utf-8", newline="\n")

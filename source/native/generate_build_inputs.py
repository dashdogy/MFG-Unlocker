"""Generate the fixed proxy ABI, Vulkan color shaders and pinned ImGui DX12 and DX11 adaptations."""
from pathlib import Path
import argparse, hashlib, json, shutil, struct, subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--imgui', type=Path, required=True)
parser.add_argument('--glslang', type=Path, required=True)
args = parser.parse_args()
output, imgui, glslang = args.output, args.imgui, args.glslang
source_dir = Path(__file__).resolve().parent
output.mkdir(parents=True, exist_ok=True)
(output / 'proxy_generated').mkdir(exist_ok=True)
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()
if digest(imgui / 'backends/imgui_impl_dx12.cpp') != '4932F938C6D48EDF91B45078CF0BDD0BF9AD46CC5D4B9A48873528046DA7C580':
    raise RuntimeError('Expected the pinned ImGui DX12 backend')
if digest(imgui / 'backends/imgui_impl_dx11.cpp') != '0167B94EA9732A4185C203C9BED5ECB4020BB30EE12C34025866A9ADFCC6A906':
    raise RuntimeError('Expected the pinned ImGui DX11 backend')
if digest(glslang) != '8F0838D3FD981D4086D81675C4CFA4381691AEAFC3651D12D62F61EF0CEBB33E':
    raise RuntimeError('Expected glslangValidator from Vulkan SDK 1.2.176.1')
families = []
for line in (source_dir / 'proxy_exports.txt').read_text(encoding='utf-8').splitlines():
    if not line or line.startswith('#'):
        continue
    if line.startswith('@'):
        families.append(dict(name=line[1:], exports=[]))
    else:
        ordinal, *names = line.split()
        families[-1]['exports'].append(dict(ordinal=int(ordinal), names=names))
assert len(families) == 19
names = sorted({name for family in families for entry in family["exports"] for name in entry["names"]})
ordinals = sorted({entry["ordinal"] for family in families for entry in family["exports"]})
assert min(ordinals) >= 1 and max(ordinals) < 1024
named_base = 1024
public_base = 4096
assert named_base + len(names) < public_base
public = ["MfgUnlockCoreLoaded", "MfgUnlockSampleFrameTelemetry", "MfgUnlockSingleModuleQuery", "InitializeASI",
          "XeFGModelName", "OptiPluginInit"]
defs = ["LIBRARY RTXMFG", "EXPORTS"]
asm = ["; Generated Windows x64 ABI-preserving named/ordinal tail dispatch.",
       "option casemap:none", "extern MfgProxyResolveRenamed:proc", ".code"]

def thunk(symbol, kind, key):
    asm.extend([f"{symbol} proc frame", "    sub rsp, 0A8h", "    .allocstack 0A8h", "    .endprolog",
                "    mov [rsp+20h], rcx", "    mov [rsp+28h], rdx", "    mov [rsp+30h], r8", "    mov [rsp+38h], r9",
                "    movdqu [rsp+40h], xmm0", "    movdqu [rsp+50h], xmm1", "    movdqu [rsp+60h], xmm2", "    movdqu [rsp+70h], xmm3",
                f"    mov ecx, {kind}", f"    mov edx, {key}", "    call MfgProxyResolveRenamed",
                "    movdqu xmm0, [rsp+40h]", "    movdqu xmm1, [rsp+50h]", "    movdqu xmm2, [rsp+60h]", "    movdqu xmm3, [rsp+70h]",
                "    mov rcx, [rsp+20h]", "    mov rdx, [rsp+28h]", "    mov r8, [rsp+30h]", "    mov r9, [rsp+38h]",
                "    add rsp, 0A8h", "    jmp rax", f"{symbol} endp"])

for ordinal in ordinals:
    symbol = f"MfgRenamedOrdinal_{ordinal}"
    defs.append(f"{symbol} @{ordinal} NONAME")
    thunk(symbol, 0, ordinal)
for index, name in enumerate(names):
    symbol = f"MfgRenamedName_{index}"
    defs.append(f"{name}={symbol} @{named_base + index} PRIVATE")
    thunk(symbol, 1, index)
for index, name in enumerate(public):
    defs.append(f"{name} @{public_base + index}")
asm.append("end")
header = ["#pragma once", "#include <cstdint>",
          f"inline constexpr uint32_t kMfgRenamedNameCount = {len(names)};",
          f"inline constexpr uint16_t kMfgRenamedOrdinals[19][{len(names)}] = {{"]
for family in families:
    by_name = {name: entry["ordinal"] for entry in family["exports"] for name in entry["names"]}
    header.append("    {" + ",".join(str(by_name.get(name, 0)) for name in names) + "},")
header += ["};", "inline bool MfgProxyKnownOrdinal(uint32_t family, uint32_t ordinal) noexcept {", "    switch (family) {"]
for index, family in enumerate(families):
    header.append(f"    case {index}: switch (ordinal) {{")
    header.extend(f"        case {entry['ordinal']}:" for entry in family["exports"])
    header.append("            return true; default: return false; }")
header += ["    default: return false;", "    }", "}"]
proxy = ["#pragma once", "#include <cstdint>",
    "struct MfgProxyFamily { const wchar_t* original; const wchar_t* chained; bool systemFallback; };",
    "inline constexpr MfgProxyFamily kMfgProxyFamilies[] = {"]
for family in families:
    name = family['name']
    fallback = 'false' if name in ('binkw64', 'bink2w64') else 'true'
    proxy.append(f'    {{ L"{name}.dll", L"{name}Hooked.dll", {fallback} }},')
proxy += ["};", "inline constexpr uint32_t kMfgProxyFamilyCount = sizeof(kMfgProxyFamilies) / sizeof(kMfgProxyFamilies[0]);",
    "inline constexpr uint32_t kMfgProxyOrdinalLimit = 1024;",
    "inline const char* MfgProxyExportName(uint32_t family, uint32_t ordinal) noexcept {", "    switch (family) {"]
for index, family in enumerate(families):
    proxy.append(f"    case {index}: switch (ordinal) {{")
    for entry in family['exports']:
        # Candidate's private XInput entries use ordinal dispatch without a name fallback.
        if entry['names'] and not (family['name'].startswith('xinput') and entry['ordinal'] in (100,101,102,103,104,108)):
            proxy.append(f"        case {entry['ordinal']}: return \"{entry['names'][0]}\";")
    proxy.append("        default: return nullptr; }")
proxy += ["    default: return nullptr;", "    }", "}"]
for name, lines in [("renamable.def", defs), ("renamable_exports.asm", asm),
                    ("renamable_names.h", header), ("proxy_export_names.h", proxy)]:
    (output / "proxy_generated" / name).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

def replace(text, old, new):
    assert text.count(old) == 1, (old, text.count(old))
    return text.replace(old, new)

source = imgui / 'backends/imgui_impl_dx12.cpp'
text = source.read_text(encoding='utf-8')
text = replace(text, '    float   mvp[4][4];', '    float   mvp[4][4];\n    float   outputColorSpace;\n    float   paperWhiteNits;')
text = replace(text, '    bool                        LegacySingleDescriptorUsed;',
               '    bool                        LegacySingleDescriptorUsed;\n    int                         OutputColorSpace; // RTX40MFG: 0 SDR, 1 scRGB, 2 HDR10')
marker = '// Buffers used during the rendering of a frame'
text = replace(text, marker, '''// RTX40MFG: per-context output encoding; changes constants, not live pipelines.
void ImGui_ImplDX12_SetOutputColorSpace(int mode)
{
    if (auto* data = ImGui_ImplDX12_GetBackendData()) data->OutputColorSpace = mode;
}
bool ImGui_ImplDX12_IsReady()
{
    const auto* data = ImGui_ImplDX12_GetBackendData();
    return data && data->pPipelineState && data->pRootSignature;
}

''' + marker)
text = replace(text, '    VERTEX_CONSTANT_BUFFER_DX12 vertex_constant_buffer;',
               '    VERTEX_CONSTANT_BUFFER_DX12 vertex_constant_buffer;\n    vertex_constant_buffer.outputColorSpace = float(bd->OutputColorSpace);\n    vertex_constant_buffer.paperWhiteNits = 203.0f;')
text = replace(text, 'SetGraphicsRoot32BitConstants(0, 16,', 'SetGraphicsRoot32BitConstants(0, 18,')
text = replace(text, 'param[0].Constants.Num32BitValues = 16;', 'param[0].Constants.Num32BitValues = 18;')
text = replace(text, 'param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;', 'param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;')
start = text.index('        static const char* pixelShader =')
end = text.index('        if (FAILED(D3DCompile(pixelShader', start)
shader = r'''cbuffer UiColor : register(b0) { float4x4 Projection; float OutputColorSpace; float PaperWhiteNits; };
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
SamplerState sampler0 : register(s0);
Texture2D texture0 : register(t0);
float3 Linearize(float3 c) {
    return lerp(pow(max((c+0.055)/1.055,0),2.4), c/12.92, step(c,0.04045));
}
float3 Pq(float3 nits) {
    float3 p = pow(max(nits/10000.0,0),2610.0/16384.0);
    return pow((3424.0/4096.0+(2413.0/128.0)*p)/(1.0+(2392.0/128.0)*p),2523.0/32.0);
}
float4 main(PS_INPUT input) : SV_Target {
    float4 color = input.col * texture0.Sample(sampler0,input.uv);
    if (OutputColorSpace > 0.5) {
        float3 linearColor = Linearize(color.rgb);
        if (OutputColorSpace < 1.5) color.rgb = linearColor * PaperWhiteNits/80.0;
        else {
            float3 rec2020 = float3(dot(linearColor,float3(0.627404,0.329282,0.0433136)),
                dot(linearColor,float3(0.069097,0.919540,0.0113612)),
                dot(linearColor,float3(0.0163916,0.0880132,0.895595)));
            color.rgb = Pq(rec2020*PaperWhiteNits);
        }
    }
    return color;
}'''
text = text[:start] + '        static const char* pixelShader = R"MFG(' + shader + ')MFG";\n\n' + text[end:]
(output / 'imgui_impl_dx12.cpp').write_text(text, encoding='utf-8', newline='\n')
shutil.copyfile(imgui / 'backends/imgui_impl_dx12.h', output / 'imgui_impl_dx12.h')

# dx11menu.1 (V1.7.1): the DX11 backend with the same output encoding, for the menu in DirectX 11 games. Mode 3 is
# an sRGB render target (a legacy DirectX 11 swap chain format), which encodes the linear colour itself, as the Vulkan
# menu's shader 3 does. The pixel shader reads the vertex constant buffer, so it is bound and kept for both stages.
source = imgui / 'backends/imgui_impl_dx11.cpp'
text = source.read_text(encoding='utf-8')
text = replace(text, '    float   mvp[4][4];', '    float   mvp[4][4];\n    float   outputColorSpace;\n'
               '    float   paperWhiteNits;\n    float   padding[2]; // RTX40MFG: a constant buffer is a multiple of 16 bytes')
text = replace(text, '    int                         IndexBufferSize;',
               '    int                         IndexBufferSize;\n'
               '    int                         OutputColorSpace; // RTX40MFG: 0 SDR, 1 scRGB, 2 HDR10, 3 sRGB target')
marker = '// Functions'
text = replace(text, marker, '''// RTX40MFG: per-context output encoding; changes constants, not device objects.
void ImGui_ImplDX11_SetOutputColorSpace(int mode)
{
    if (auto* data = ImGui_ImplDX11_GetBackendData()) data->OutputColorSpace = mode;
}
bool ImGui_ImplDX11_IsReady()
{
    const auto* data = ImGui_ImplDX11_GetBackendData();
    return data && data->pVertexShader && data->pPixelShader && data->pInputLayout && data->pVertexConstantBuffer
        && data->pBlendState && data->pRasterizerState && data->pDepthStencilState && data->pTexSamplerLinear;
}

''' + marker)
text = replace(text, '        memcpy(&constant_buffer->mvp, mvp, sizeof(mvp));',
               '        memcpy(&constant_buffer->mvp, mvp, sizeof(mvp));\n'
               '        constant_buffer->outputColorSpace = float(bd->OutputColorSpace);\n'
               '        constant_buffer->paperWhiteNits = 203.0f;')
text = replace(text, '    device_ctx->VSSetConstantBuffers(0, 1, &bd->pVertexConstantBuffer);',
               '    device_ctx->VSSetConstantBuffers(0, 1, &bd->pVertexConstantBuffer);\n'
               '    device_ctx->PSSetConstantBuffers(0, 1, &bd->pVertexConstantBuffer);')
text = replace(text, '*VertexBuffer, *VSConstantBuffer;', '*VertexBuffer, *VSConstantBuffer, *PSConstantBuffer;')
text = replace(text, '    device->VSGetConstantBuffers(0, 1, &old.VSConstantBuffer);',
               '    device->VSGetConstantBuffers(0, 1, &old.VSConstantBuffer);\n'
               '    device->PSGetConstantBuffers(0, 1, &old.PSConstantBuffer);')
restore = '    device->VSSetConstantBuffers(0, 1, &old.VSConstantBuffer); if (old.VSConstantBuffer) old.VSConstantBuffer->Release();'
text = replace(text, restore, restore + '\n    device->PSSetConstantBuffers(0, 1, &old.PSConstantBuffer);'
               ' if (old.PSConstantBuffer) old.PSConstantBuffer->Release();')
shader11 = replace(shader, '        if (OutputColorSpace < 1.5) color.rgb',
                   '        if (OutputColorSpace > 2.5) color.rgb = linearColor;\n'
                   '        else if (OutputColorSpace < 1.5) color.rgb')
start = text.index('        static const char* pixelShader =')
end = text.index('        ID3DBlob* pixelShaderBlob;', start)
text = text[:start] + '        static const char* pixelShader = R"MFG(' + shader11 + ')MFG";\n\n' + text[end:]
(output / 'imgui_impl_dx11.cpp').write_text(text, encoding='utf-8', newline='\n')
shutil.copyfile(imgui / 'backends/imgui_impl_dx11.h', output / 'imgui_impl_dx11.h')

header = ['#pragma once', '#include <cstdint>', 'namespace single_overlay::color {']
shader_rows = []
for mode in (1,2,3):
    spv = output / f'ui_color_{mode}.spv'
    subprocess.run([str(glslang), '-V', f'-DMFG_COLOR={mode}',
                    '-o', str(spv), str(source_dir / 'ui_color.frag')], check=True)
    raw = spv.read_bytes()
    words = struct.unpack(f'<{len(raw)//4}I', raw)
    header.append(f'inline constexpr uint32_t shader{mode}[] = {{')
    for at in range(0, len(words), 8): header.append('    '+','.join(f'0x{word:08x}u' for word in words[at:at+8])+',')
    header.append('};')
    shader_rows.append({'mode': mode, 'sha256': hashlib.sha256(raw).hexdigest().upper(), 'bytes': len(raw)})
header.append('}')
(output / 'overlay_color_spv.h').write_text('\n'.join(header)+'\n', encoding='ascii')

# Refuse ABI, shader or backend drift from the frozen release candidate.
for name, expected in json.loads((source_dir / 'generated_sha256.json').read_text(encoding='utf-8')).items():
    actual = hashlib.sha256((output / name).read_text(encoding='utf-8').encode()).hexdigest().upper()
    if actual != expected:
        raise RuntimeError(f'Generated output mismatch: {name} ({actual})')
print('Verified all seven generated outputs against their pins')

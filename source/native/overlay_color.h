#pragma once
// Uses the current ImGui context. No device or pipeline recreation is needed.
void ImGui_ImplDX12_SetOutputColorSpace(int mode);
bool ImGui_ImplDX12_IsReady();
// dx11menu.1: the same for the DX11 backend; mode 3 is an sRGB render target.
void ImGui_ImplDX11_SetOutputColorSpace(int mode);
bool ImGui_ImplDX11_IsReady();

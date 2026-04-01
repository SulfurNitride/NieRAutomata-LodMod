#pragma once

#include <cstdint>
#include <filesystem>

struct ID3D11Device; // forward declare to avoid pulling in d3d11.h

// CRC32-C (Castagnoli) - hardware accelerated when SSE4.2 is available
uint32_t texinj_crc32c(uint32_t crc, const void* buf, size_t len);

struct TextureInjectionSettings
{
  bool Enable = false;
  bool DebugLog = false;

  // Search paths (relative to game dir, scanned in order - last wins)
  // 1. SpecialK-style:  SpecialK/inject/textures/{hash}.dds
  // 2. WAX-style:       WAX/mods/{ModName}/textures/{hash}.dds
  // 3. LodMod-style:    LodMod/textures/{hash}.dds
};

extern TextureInjectionSettings TexInjSettings;

// Initialize the texture injection system
void TextureInjection_Init();

// Scan all texture source folders and build the hash->filepath map
void TextureInjection_ScanFolders(const std::filesystem::path& gameDir);

// Hook entry point - called from DXGI factory hook to capture the D3D11 device
void TextureInjection_OnDeviceCreated(ID3D11Device* pDevice);

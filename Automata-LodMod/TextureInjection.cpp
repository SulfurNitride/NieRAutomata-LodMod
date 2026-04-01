#include "pch.h"
#include "TextureInjection.h"

#include <d3d11.h>
#include <dxgi.h>
#include <intrin.h>
#include "DirectXTex.h"

TextureInjectionSettings TexInjSettings;

// ============================================================================
// CRC32-C Implementation (Castagnoli)
// Uses SSE4.2 hardware intrinsics when available, software fallback otherwise.
// Compatible with Special K's hashing for texture pack interoperability.
// ============================================================================

static bool s_has_sse42 = false;

static void texinj_detect_sse42()
{
  int cpuInfo[4] = { 0 };
  __cpuidex(cpuInfo, 1, 0);
  s_has_sse42 = (cpuInfo[2] & (1 << 20)) != 0;
}

// Software CRC32-C table
static uint32_t s_crc32c_table[256];
static bool s_crc32c_table_init = false;

static void texinj_init_crc32c_table()
{
  if (s_crc32c_table_init) return;
  for (uint32_t i = 0; i < 256; i++)
  {
    uint32_t crc = i;
    for (int j = 0; j < 8; j++)
      crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78 : 0);
    s_crc32c_table[i] = crc;
  }
  s_crc32c_table_init = true;
}

static uint32_t texinj_crc32c_sw(uint32_t crc, const void* buf, size_t len)
{
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  crc = ~crc;
  for (size_t i = 0; i < len; i++)
    crc = s_crc32c_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

__attribute__((target("sse4.2")))
static uint32_t texinj_crc32c_hw(uint32_t crc, const void* buf, size_t len)
{
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  crc = ~crc;

#ifdef _M_X64
  // Process 8 bytes at a time
  while (len >= 8)
  {
    crc = static_cast<uint32_t>(_mm_crc32_u64(crc, *reinterpret_cast<const uint64_t*>(p)));
    p += 8;
    len -= 8;
  }
#endif

  // Process remaining bytes
  while (len > 0)
  {
    crc = _mm_crc32_u8(crc, *p);
    p++;
    len--;
  }

  return ~crc;
}

uint32_t texinj_crc32c(uint32_t crc, const void* buf, size_t len)
{
  if (s_has_sse42)
    return texinj_crc32c_hw(crc, buf, len);
  return texinj_crc32c_sw(crc, buf, len);
}

// ============================================================================
// Texture Hash Map
// Maps CRC32-C top_crc32 -> filepath for DDS replacement textures
// Supports dual-hash filenames: {top_crc32}_{full_checksum}.dds
// ============================================================================

struct TextureEntry
{
  std::wstring filepath;
  uint32_t top_crc32 = 0;
  uint32_t full_checksum = 0; // 0 = single-hash (matches any full checksum)
};

static std::unordered_map<uint32_t, std::vector<TextureEntry>>& GetTextureMap()
{
  static auto* m = new std::unordered_map<uint32_t, std::vector<TextureEntry>>();
  return *m;
}

static CRITICAL_SECTION& GetTexCS()
{
  static CRITICAL_SECTION* cs = nullptr;
  if (!cs) { cs = new CRITICAL_SECTION; InitializeCriticalSection(cs); }
  return *cs;
}

static size_t s_total_textures = 0;

// Parse a hex hash from a filename component
static bool ParseHex(const std::wstring& str, uint32_t& out)
{
  if (str.empty()) return false;
  try
  {
    out = static_cast<uint32_t>(std::stoul(str, nullptr, 16));
    return true;
  }
  catch (...) { return false; }
}

// Try to parse a DDS filename into hash(es)
// Supports: "A1B2C3D4.dds" or "A1B2C3D4_E5F6G7H8.dds"
static bool ParseTextureFilename(const std::filesystem::path& filepath, uint32_t& top_crc32, uint32_t& full_checksum)
{
  auto stem = filepath.stem().wstring();
  auto ext = filepath.extension().wstring();

  // Must be .dds
  if (_wcsicmp(ext.c_str(), L".dds") != 0)
    return false;

  auto underscore = stem.find(L'_');
  if (underscore != std::wstring::npos)
  {
    // Dual hash: top_full.dds
    auto topStr = stem.substr(0, underscore);
    auto fullStr = stem.substr(underscore + 1);
    if (!ParseHex(topStr, top_crc32) || !ParseHex(fullStr, full_checksum))
      return false;
  }
  else
  {
    // Single hash: top.dds
    if (!ParseHex(stem, top_crc32))
      return false;
    full_checksum = 0;
  }

  return true;
}

// Scan a single directory for hash-named DDS files
static int ScanTextureDirectory(const std::filesystem::path& dir, const char* sourceName)
{
  if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
    return 0;

  int count = 0;
  std::error_code ec;

  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
  {
    if (entry.is_directory())
      continue;

    uint32_t top_crc32 = 0, full_checksum = 0;
    if (!ParseTextureFilename(entry.path(), top_crc32, full_checksum))
      continue;

    TextureEntry tex;
    tex.filepath = entry.path().wstring();
    tex.top_crc32 = top_crc32;
    tex.full_checksum = full_checksum;

    {
      EnterCriticalSection(&GetTexCS());

      auto& entries = GetTextureMap()[top_crc32];

      // Check for existing entry with same hashes - replace it (later sources win)
      bool replaced = false;
      for (auto& existing : entries)
      {
        if (existing.full_checksum == full_checksum)
        {
          if (TexInjSettings.DebugLog)
          {
            auto oldPath = utf8_encode(existing.filepath);
            auto newPath = utf8_encode(tex.filepath);
            dlog("[TexInject] Replacing %s with %s (source: %s)\n",
              oldPath.c_str(), newPath.c_str(), sourceName);
          }
          existing = tex;
          replaced = true;
          break;
        }
      }

      if (!replaced)
        entries.push_back(tex);

      LeaveCriticalSection(&GetTexCS());
    }

    count++;
  }

  return count;
}

void TextureInjection_ScanFolders(const std::filesystem::path& gameDir)
{
  {
    EnterCriticalSection(&GetTexCS());
    GetTextureMap().clear();
    s_total_textures = 0;
    LeaveCriticalSection(&GetTexCS());
  }

  // Source 1 (lowest priority): SpecialK-style
  // SpecialK/inject/textures/
  auto skPath = gameDir / "SpecialK" / "inject" / "textures";
  int skCount = ScanTextureDirectory(skPath, "SpecialK");
  if (skCount > 0)
    dlog("[TexInject] Loaded %d textures from SpecialK folder\n", skCount);

  // Source 2 (medium priority): WAX-style
  // WAX/mods/{ModName}/textures/
  auto waxModsPath = gameDir / "WAX" / "mods";
  int waxCount = 0;
  std::error_code ec;
  if (std::filesystem::exists(waxModsPath, ec) && std::filesystem::is_directory(waxModsPath, ec))
  {
    for (const auto& modDir : std::filesystem::directory_iterator(waxModsPath, ec))
    {
      if (!modDir.is_directory())
        continue;

      auto texDir = modDir.path() / "textures";
      std::string modName = modDir.path().filename().string();
      int modCount = ScanTextureDirectory(texDir, modName.c_str());
      if (modCount > 0)
      {
        dlog("[TexInject] Loaded %d textures from WAX mod: %s\n", modCount, modName.c_str());
        waxCount += modCount;
      }
    }
  }

  // Source 3 (highest priority): LodMod-style
  // LodMod/textures/
  auto lodmodPath = gameDir / "LodMod" / "textures";
  int lodmodCount = ScanTextureDirectory(lodmodPath, "LodMod");
  if (lodmodCount > 0)
    dlog("[TexInject] Loaded %d textures from LodMod folder\n", lodmodCount);

  {
    EnterCriticalSection(&GetTexCS());
    s_total_textures = 0;
    for (const auto& [hash, entries] : GetTextureMap())
      s_total_textures += entries.size();
    LeaveCriticalSection(&GetTexCS());
  }

  dlog("[TexInject] Total unique texture entries indexed: %zu (SK: %d, WAX: %d, LodMod: %d)\n",
    s_total_textures, skCount, waxCount, lodmodCount);
}

// ============================================================================
// Texture Hashing
// Computes CRC32-C of texture pixel data at creation time.
// Mirrors Special K's crc32_tex algorithm for compatibility.
// ============================================================================

static bool IsCompressedFormat(DXGI_FORMAT fmt)
{
  switch (fmt)
  {
  case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
  case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
  case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
  case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
    return true;
  default:
    return false;
  }
}

// BC1/BC4 = 8 bytes per block, all others = 16
static int GetBCBlockSize(DXGI_FORMAT fmt)
{
  switch (fmt)
  {
  case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
    return 8;
  default:
    return 16;
  }
}

static UINT GetBytesPerPixel(DXGI_FORMAT fmt)
{
  switch (fmt)
  {
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    return 16;
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
  case DXGI_FORMAT_R32G32B32_TYPELESS:
    return 12;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
  case DXGI_FORMAT_R32G32_TYPELESS:
    return 8;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R11G11B10_FLOAT:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SNORM:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R16G16_TYPELESS:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
  case DXGI_FORMAT_R32_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT:
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
  case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    return 4;
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SNORM:
  case DXGI_FORMAT_R8G8_SINT:
  case DXGI_FORMAT_R8G8_TYPELESS:
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SNORM:
  case DXGI_FORMAT_R16_SINT:
  case DXGI_FORMAT_R16_TYPELESS:
  case DXGI_FORMAT_D16_UNORM:
  case DXGI_FORMAT_B5G6R5_UNORM:
  case DXGI_FORMAT_B5G5R5A1_UNORM:
  case DXGI_FORMAT_B4G4R4A4_UNORM:
    return 2;
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_A8_UNORM:
    return 1;
  default:
    return 0;
  }
}

static uint32_t CalcMipmapLODs(uint32_t width, uint32_t height)
{
  uint32_t lods = 1;
  while (width > 1 || height > 1)
  {
    if (width > 1)  width >>= 1;
    if (height > 1) height >>= 1;
    lods++;
  }
  return lods;
}

// Compute CRC32-C of texture data, matching Special K's crc32_tex algorithm
// Returns full checksum, sets *pLOD0_CRC32 to the base mip hash (used for lookup)
static uint32_t HashTexture(const D3D11_TEXTURE2D_DESC* pDesc,
  const D3D11_SUBRESOURCE_DATA* pInitialData,
  uint32_t* pLOD0_CRC32)
{
  if (!pInitialData || !pDesc)
    return 0;

  if (pLOD0_CRC32)
    *pLOD0_CRC32 = 0;

  // Skip cubemaps and array textures (same as SK)
  if ((pDesc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) == D3D11_RESOURCE_MISC_TEXTURECUBE)
    return 0;

  if (pDesc->ArraySize > 1)
    return 0;

  // Skip textures without CPU-readable initial data
  if (pDesc->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)
    return 0;

  uint32_t checksum = 0;
  bool compressed = IsCompressedFormat(pDesc->Format);

  uint32_t width = pDesc->Width;
  uint32_t height = pDesc->Height;
  uint32_t mip_levels = pDesc->MipLevels;
  if (mip_levels == 0)
    mip_levels = CalcMipmapLODs(width, height);

  if (compressed)
  {
    int blockSize = GetBCBlockSize(pDesc->Format);
    // bpp flag: 0 for BC1/BC4 (8-byte blocks), 1 for others (16-byte blocks)
    int bpp = (blockSize == 8) ? 0 : 1;

    for (uint32_t i = 0; i < mip_levels; i++)
    {
      const uint8_t* pData = static_cast<const uint8_t*>(pInitialData[i].pSysMem);
      if (!pData) break;

      UINT stride = bpp == 0
        ? (std::max(1u, ((width + 3u) / 4u)) * 8u)
        : (std::max(1u, ((width + 3u) / 4u)) * 16u);

      if (stride == pInitialData[i].SysMemPitch)
      {
        // Tightly packed - hash entire mip at once
        uint32_t lodSize = stride * (height / 4 + height % 4);
        checksum = texinj_crc32c(checksum, pData, lodSize);
      }
      else
      {
        // Padded rows - hash row by row
        for (uint32_t j = 0; j < height; j += 4)
        {
          uint32_t row_crc = texinj_crc32c(checksum, pData, stride);
          if (row_crc != 0)
            checksum = row_crc;
          pData += pInitialData[i].SysMemPitch;
        }
      }

      if (i == 0 && pLOD0_CRC32)
        *pLOD0_CRC32 = checksum;

      if (width > 1)  width >>= 1;
      if (height > 1) height >>= 1;
    }
  }
  else
  {
    UINT bytesPerPixel = GetBytesPerPixel(pDesc->Format);
    if (bytesPerPixel == 0)
      return 0;

    for (uint32_t i = 0; i < mip_levels; i++)
    {
      const uint8_t* pData = static_cast<const uint8_t*>(pInitialData[i].pSysMem);
      if (!pData) break;

      UINT scanlength = bytesPerPixel * width;

      if (scanlength == pInitialData[i].SysMemPitch)
      {
        uint32_t lodSize = scanlength * height;
        checksum = texinj_crc32c(checksum, pData, lodSize);
      }
      else
      {
        for (uint32_t j = 0; j < height; j++)
        {
          uint32_t row_crc = texinj_crc32c(checksum, pData, scanlength);
          if (row_crc != 0)
            checksum = row_crc;
          pData += pInitialData[i].SysMemPitch;
        }
      }

      if (i == 0 && pLOD0_CRC32)
        *pLOD0_CRC32 = checksum;

      if (width > 1)  width >>= 1;
      if (height > 1) height >>= 1;
    }
  }

  return checksum;
}

// ============================================================================
// Lookup & Load
// ============================================================================

// Find a replacement texture file for the given hashes
static const TextureEntry* FindReplacement(uint32_t top_crc32, uint32_t full_checksum)
{
  EnterCriticalSection(&GetTexCS());

  auto it = GetTextureMap().find(top_crc32);
  if (it == GetTextureMap().end())
  {
    LeaveCriticalSection(&GetTexCS());
    return nullptr;
  }

  const TextureEntry* singleHashMatch = nullptr;

  for (const auto& entry : it->second)
  {
    // Exact dual-hash match takes priority
    if (entry.full_checksum != 0 && entry.full_checksum == full_checksum)
    {
      LeaveCriticalSection(&GetTexCS());
      return &entry;
    }

    // Single-hash match (full_checksum == 0) is fallback
    if (entry.full_checksum == 0)
      singleHashMatch = &entry;
  }

  LeaveCriticalSection(&GetTexCS());
  return singleHashMatch;
}

// ============================================================================
// Minimal DDS Loader
// ============================================================================

#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
  uint32_t size;
  uint32_t flags;
  uint32_t fourCC;
  uint32_t RGBBitCount;
  uint32_t RBitMask, GBitMask, BBitMask, ABitMask;
};

struct DDS_HEADER {
  uint32_t size;
  uint32_t flags;
  uint32_t height;
  uint32_t width;
  uint32_t pitchOrLinearSize;
  uint32_t depth;
  uint32_t mipMapCount;
  uint32_t reserved1[11];
  DDS_PIXELFORMAT ddspf;
  uint32_t caps;
  uint32_t caps2;
  uint32_t caps3;
  uint32_t caps4;
  uint32_t reserved2;
};

struct DDS_HEADER_DXT10 {
  DXGI_FORMAT dxgiFormat;
  uint32_t resourceDimension;
  uint32_t miscFlag;
  uint32_t arraySize;
  uint32_t miscFlags2;
};
#pragma pack(pop)

#define DDS_MAGIC 0x20534444 // "DDS "
#define DDPF_FOURCC 0x4
#define DDPF_RGB    0x40
#define DDPF_LUMINANCE 0x20000
#define DDPF_ALPHA  0x2

static DXGI_FORMAT DDS_GetFormat(const DDS_PIXELFORMAT& pf, bool& hasDXT10)
{
  hasDXT10 = false;
  if (pf.flags & DDPF_FOURCC)
  {
    switch (pf.fourCC)
    {
    case '1TXD': return DXGI_FORMAT_BC1_UNORM; // DXT1
    case '3TXD': return DXGI_FORMAT_BC2_UNORM; // DXT3
    case '5TXD': return DXGI_FORMAT_BC3_UNORM; // DXT5
    case '1ITA': return DXGI_FORMAT_BC4_UNORM; // ATI1/BC4
    case '2ITA': return DXGI_FORMAT_BC5_UNORM; // ATI2/BC5
    case 'U4CB': return DXGI_FORMAT_BC4_UNORM;
    case 'U5CB': return DXGI_FORMAT_BC5_UNORM;
    case '01XD': hasDXT10 = true; return DXGI_FORMAT_UNKNOWN; // read from DXT10 header
    case 111:    return DXGI_FORMAT_R16_FLOAT;
    case 112:    return DXGI_FORMAT_R16G16_FLOAT;
    case 113:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 114:    return DXGI_FORMAT_R32_FLOAT;
    case 115:    return DXGI_FORMAT_R32G32_FLOAT;
    case 116:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:     return DXGI_FORMAT_UNKNOWN;
    }
  }
  if (pf.flags & DDPF_RGB)
  {
    switch (pf.RGBBitCount)
    {
    case 32:
      if (pf.RBitMask == 0xFF && pf.GBitMask == 0xFF00 && pf.BBitMask == 0xFF0000 && pf.ABitMask == 0xFF000000)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
      if (pf.BBitMask == 0xFF && pf.GBitMask == 0xFF00 && pf.RBitMask == 0xFF0000 && pf.ABitMask == 0xFF000000)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
      break;
    case 16:
      if (pf.RBitMask == 0xF800)
        return DXGI_FORMAT_B5G6R5_UNORM;
      break;
    case 8:
      return DXGI_FORMAT_R8_UNORM;
    }
  }
  if (pf.flags & DDPF_LUMINANCE)
  {
    if (pf.RGBBitCount == 8) return DXGI_FORMAT_R8_UNORM;
    if (pf.RGBBitCount == 16) return DXGI_FORMAT_R8G8_UNORM;
  }
  if (pf.flags & DDPF_ALPHA)
  {
    if (pf.RGBBitCount == 8) return DXGI_FORMAT_A8_UNORM;
  }
  return DXGI_FORMAT_UNKNOWN;
}

static UINT DDS_CalcRowPitch(DXGI_FORMAT fmt, uint32_t width)
{
  if (IsCompressedFormat(fmt))
  {
    int blockSize = GetBCBlockSize(fmt);
    return std::max(1u, ((width + 3u) / 4u)) * blockSize;
  }
  return GetBytesPerPixel(fmt) * width;
}

static UINT DDS_CalcSlicePitch(DXGI_FORMAT fmt, uint32_t width, uint32_t height)
{
  if (IsCompressedFormat(fmt))
  {
    int blockSize = GetBCBlockSize(fmt);
    return std::max(1u, ((width + 3u) / 4u)) * blockSize * std::max(1u, ((height + 3u) / 4u));
  }
  return GetBytesPerPixel(fmt) * width * height;
}

static HRESULT LoadAndCreateTexture(ID3D11Device* pDevice,
  const std::wstring& filepath,
  ID3D11Texture2D** ppTexture)
{
  // Try DirectXTex first
  DirectX::TexMetadata metadata;
  HRESULT hr = DirectX::GetMetadataFromDDSFile(filepath.c_str(), DirectX::DDS_FLAGS_NONE, metadata);
  if (SUCCEEDED(hr))
  {
    DirectX::ScratchImage image;
    hr = DirectX::LoadFromDDSFile(filepath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
    if (SUCCEEDED(hr))
    {
      hr = DirectX::CreateTexture(pDevice,
        image.GetImages(), image.GetImageCount(), metadata,
        reinterpret_cast<ID3D11Resource**>(ppTexture));
      if (SUCCEEDED(hr))
        return S_OK;
    }
  }

  // Fallback: manual DDS loader
  HANDLE hFile = CreateFileW(filepath.c_str(), GENERIC_READ, FILE_SHARE_READ,
    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return E_FAIL;

  DWORD fileSize = GetFileSize(hFile, nullptr);
  if (fileSize < sizeof(uint32_t) + sizeof(DDS_HEADER))
  {
    CloseHandle(hFile);
    return E_FAIL;
  }

  auto fileData = std::make_unique<uint8_t[]>(fileSize);
  DWORD bytesRead;
  if (!ReadFile(hFile, fileData.get(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize)
  {
    CloseHandle(hFile);
    return E_FAIL;
  }
  CloseHandle(hFile);

  // Parse DDS
  const uint8_t* ptr = fileData.get();
  uint32_t magic = *reinterpret_cast<const uint32_t*>(ptr);
  if (magic != DDS_MAGIC)
    return E_FAIL;
  ptr += 4;

  const DDS_HEADER* header = reinterpret_cast<const DDS_HEADER*>(ptr);
  ptr += sizeof(DDS_HEADER);

  bool hasDXT10 = false;
  DXGI_FORMAT format = DDS_GetFormat(header->ddspf, hasDXT10);

  if (hasDXT10)
  {
    const DDS_HEADER_DXT10* dxt10 = reinterpret_cast<const DDS_HEADER_DXT10*>(ptr);
    format = dxt10->dxgiFormat;
    ptr += sizeof(DDS_HEADER_DXT10);
  }

  if (format == DXGI_FORMAT_UNKNOWN)
    return E_FAIL;

  uint32_t width = header->width;
  uint32_t height = header->height;
  uint32_t mipCount = header->mipMapCount;
  if (mipCount == 0) mipCount = 1;

  // Build subresource data for each mip
  std::vector<D3D11_SUBRESOURCE_DATA> initData(mipCount);
  const uint8_t* dataPtr = ptr;
  uint32_t w = width, h = height;

  for (uint32_t i = 0; i < mipCount; i++)
  {
    UINT rowPitch = DDS_CalcRowPitch(format, w);
    UINT slicePitch = DDS_CalcSlicePitch(format, w, h);

    // Bounds check
    if (dataPtr + slicePitch > fileData.get() + fileSize)
    {
      mipCount = i; // truncate
      break;
    }

    initData[i].pSysMem = dataPtr;
    initData[i].SysMemPitch = rowPitch;
    initData[i].SysMemSlicePitch = slicePitch;

    dataPtr += slicePitch;
    if (w > 1) w >>= 1;
    if (h > 1) h >>= 1;
  }

  if (mipCount == 0)
    return E_FAIL;

  // Create texture
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = mipCount;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  return pDevice->CreateTexture2D(&desc, initData.data(), ppTexture);
}

// ============================================================================
// D3D11 CreateTexture2D Hook
// ============================================================================

// Original function pointer
typedef HRESULT(STDMETHODCALLTYPE* CreateTexture2D_fn)(
  ID3D11Device* pDevice,
  const D3D11_TEXTURE2D_DESC* pDesc,
  const D3D11_SUBRESOURCE_DATA* pInitialData,
  ID3D11Texture2D** ppTexture2D);

static CreateTexture2D_fn s_CreateTexture2D_Original = nullptr;
static ID3D11Device* s_pDevice = nullptr;

// Flag to prevent recursive injection (set per-call, not thread-local to avoid TLS init issues)
static volatile bool s_inInjection = false;

static HRESULT STDMETHODCALLTYPE CreateTexture2D_Hook(
  ID3D11Device* pDevice,
  const D3D11_TEXTURE2D_DESC* pDesc,
  const D3D11_SUBRESOURCE_DATA* pInitialData,
  ID3D11Texture2D** ppTexture2D)
{
  if (s_inInjection || !pInitialData || !ppTexture2D || !pDesc)
    return s_CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);

  // Lazy rescan for VFS
  static bool s_rescanned = false;
  if (!s_rescanned && s_total_textures == 0)
  {
    s_rescanned = true;
    extern WCHAR GameDir[4096];
    extern bool GotGameDir;
    if (GotGameDir)
      TextureInjection_ScanFolders(std::filesystem::path(GameDir));
  }

  if (pDesc->Usage == D3D11_USAGE_STAGING || s_total_textures == 0)
    return s_CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);

  if (!pInitialData[0].pSysMem)
    return s_CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);

  // Skip render targets, depth stencils, and other non-texture resources
  if (pDesc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))
    return s_CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);

  // Skip tiny textures (likely lookup tables, noise, etc.)
  if (pDesc->Width < 16 || pDesc->Height < 16)
    return s_CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);

  // Only hash compressed textures (BC1-BC7) - that's what texture packs use
  if (!IsCompressedFormat(pDesc->Format))
    return s_CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);

  // Hash LOD0 only
  uint32_t top_crc32 = 0;
  uint32_t full_checksum = HashTexture(pDesc, pInitialData, &top_crc32);

  // Dump hashes to a separate file for diagnostics
  static FILE* s_hashDump = nullptr;
  static int s_hashCount = 0;
  if (!s_hashDump && TexInjSettings.DebugLog && full_checksum != 0)
  {
    extern WCHAR GameDir[4096];
    WCHAR dumpPath[4096];
    swprintf_s(dumpPath, L"%sLodMod_hashes.txt", GameDir);
    s_hashDump = _wfopen(dumpPath, L"w");
  }
  if (s_hashDump && full_checksum != 0 && s_hashCount < 2000)
  {
    fprintf(s_hashDump, "%08X fmt=%d %ux%u mips=%u pitch=%u\n",
      top_crc32, (int)pDesc->Format, pDesc->Width, pDesc->Height,
      pDesc->MipLevels, pInitialData[0].SysMemPitch);
    s_hashCount++;
    if (s_hashCount % 100 == 0) fflush(s_hashDump);
  }

  if (full_checksum != 0)
  {
    const TextureEntry* replacement = FindReplacement(top_crc32, full_checksum);
    if (replacement)
    {
      // Cache: store loaded textures so we don't reload DDS every time
      static std::unordered_map<uint32_t, ID3D11Texture2D*>* s_cache = nullptr;
      if (!s_cache) s_cache = new std::unordered_map<uint32_t, ID3D11Texture2D*>();

      auto cached = s_cache->find(top_crc32);
      if (cached != s_cache->end())
      {
        // Return cached texture (AddRef since game will Release)
        cached->second->AddRef();
        *ppTexture2D = cached->second;
        return S_OK;
      }

      s_inInjection = true;
      ID3D11Texture2D* pReplacementTex = nullptr;
      HRESULT hr = LoadAndCreateTexture(pDevice, replacement->filepath, &pReplacementTex);
      s_inInjection = false;

      if (SUCCEEDED(hr) && pReplacementTex)
      {
        if (s_hashDump)
        {
          D3D11_TEXTURE2D_DESC newDesc;
          pReplacementTex->GetDesc(&newDesc);
          auto p = utf8_encode(replacement->filepath);
          fprintf(s_hashDump, "  INJECT %08X: game fmt=%d %ux%u -> dds fmt=%d %ux%u | %s\n",
            top_crc32, (int)pDesc->Format, pDesc->Width, pDesc->Height,
            (int)newDesc.Format, newDesc.Width, newDesc.Height, p.c_str());
          fflush(s_hashDump);
        }

        // Cache it (AddRef for cache, another for the return)
        pReplacementTex->AddRef();
        (*s_cache)[top_crc32] = pReplacementTex;

        *ppTexture2D = pReplacementTex;
        return S_OK;
      }
    }
  }

  return s_CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);
}

// ============================================================================
// Device capture - hook CreateTexture2D when we get the real device
// ============================================================================

static bool s_deviceHooked = false;

static void HookCreateTexture2D(ID3D11Device* pDevice)
{
  if (s_deviceHooked || !pDevice)
    return;
  s_deviceHooked = true;

  // Direct vtable swap - simpler and safer than MinHook for COM vtables
  void** vtable = *reinterpret_cast<void***>(pDevice);

  s_CreateTexture2D_Original = reinterpret_cast<CreateTexture2D_fn>(vtable[5]);

  DWORD oldProtect;
  VirtualProtect(&vtable[5], sizeof(void*), PAGE_READWRITE, &oldProtect);
  vtable[5] = &CreateTexture2D_Hook;
  VirtualProtect(&vtable[5], sizeof(void*), oldProtect, &oldProtect);

  dlog("[TexInject] Hooked CreateTexture2D via vtable swap\n");
}

typedef HRESULT(WINAPI* D3D11CreateDevice_fn)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static D3D11CreateDevice_fn s_D3D11CreateDevice_Original = nullptr;

static HRESULT WINAPI D3D11CreateDevice_Hook(
  IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
  ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
  HRESULT hr = s_D3D11CreateDevice_Original(pAdapter, DriverType, Software, Flags,
    pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
  if (SUCCEEDED(hr) && ppDevice && *ppDevice)
    HookCreateTexture2D(*ppDevice);
  return hr;
}

typedef HRESULT(WINAPI* D3D11CreateDeviceAndSwapChain_fn)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static D3D11CreateDeviceAndSwapChain_fn s_D3D11CreateDeviceAndSwapChain_Original = nullptr;

static HRESULT WINAPI D3D11CreateDeviceAndSwapChain_Hook(
  IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
  const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
  ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
  HRESULT hr = s_D3D11CreateDeviceAndSwapChain_Original(pAdapter, DriverType, Software, Flags,
    pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
    ppDevice, pFeatureLevel, ppImmediateContext);
  if (SUCCEEDED(hr) && ppDevice && *ppDevice)
    HookCreateTexture2D(*ppDevice);
  return hr;
}

// ============================================================================
// Initialization
// ============================================================================

void TextureInjection_Init()
{
  // Read settings from INI
  TexInjSettings.Enable = INI_GetBool(IniPath, L"TextureInjection", L"Enable", false);
  TexInjSettings.DebugLog = INI_GetBool(IniPath, L"TextureInjection", L"DebugLog", false) || Settings.DebugLog;

  if (!TexInjSettings.Enable)
    return;

  // Initialize CRC32-C
  texinj_detect_sse42();
  texinj_init_crc32c_table();
  dlog("[TexInject] CRC32-C: %s\n", s_has_sse42 ? "HW" : "SW");

  // Scan texture folders (may find nothing if VFS isn't mounted yet - that's OK, we rescan lazily)
  extern WCHAR GameDir[4096];
  extern bool GotGameDir;
  if (GotGameDir)
    TextureInjection_ScanFolders(std::filesystem::path(GameDir));

  // Hook D3D11CreateDevice to capture the real device
  HMODULE d3d11Module = GetModuleHandleA("d3d11.dll");
  if (!d3d11Module)
    d3d11Module = LoadLibraryA("d3d11.dll");

  if (d3d11Module)
  {
    auto pCreateDevice = GetProcAddress(d3d11Module, "D3D11CreateDevice");
    if (pCreateDevice)
      MH_CreateHook(pCreateDevice, &D3D11CreateDevice_Hook,
        reinterpret_cast<LPVOID*>(&s_D3D11CreateDevice_Original));

    auto pCreateDeviceSwap = GetProcAddress(d3d11Module, "D3D11CreateDeviceAndSwapChain");
    if (pCreateDeviceSwap)
      MH_CreateHook(pCreateDeviceSwap, &D3D11CreateDeviceAndSwapChain_Hook,
        reinterpret_cast<LPVOID*>(&s_D3D11CreateDeviceAndSwapChain_Original));

    dlog("[TexInject] D3D11 hooks created\n");
  }
}

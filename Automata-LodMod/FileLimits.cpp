#include "pch.h"

// Wolf Limit Break equivalent - increases the game's internal file I/O limits
// All patches target the file system initialization function
// Original values -> Wolf defaults -> LodMod defaults (configurable via INI)

// Function that initializes the CRI file system pools
// All patches are immediate values within this function
const uint32_t FileSystemInit_Addr[] = { 0x86B550, 0x863460, 0x879F10, 0x5D0C80, 0xA32C50 };

// Pool size #4 - mov edx, imm32 (4 bytes at offset +0x95 from func start)
// Original: 0x21EA0000 (570MB), Wolf: 0x29E20000 (711MB)
const uint32_t PoolSize4_Addr[] = { 0x86B5E8, 0x863498, 0x879FA8, 0x5D0D18, 0xA32CE8 };

// Pool size #5 - mov edx, imm32 (4 bytes at offset +0xBB from func start)
// Original: 0xBACC0000 (2.9GB), Wolf: 0xF74C0000 (3.9GB)
const uint32_t PoolSize5_Addr[] = { 0x86B60E, 0x8634BE, 0x879FCE, 0x5D0D3E, 0xA32D0E };

// Resource multiplier A - single byte immediate
// Original: 3, Wolf: 6
const uint32_t ResourceMultA_Addr[] = { 0x86B809, 0x8636B9, 0x87A1C9, 0x5D0F39, 0xA32F09 };

// Resource multiplier B - single byte immediate
// Original: 4, Wolf: 8
const uint32_t ResourceMultB_Addr[] = { 0x86B857, 0x863707, 0x87A217, 0x5D0F87, 0xA32F57 };

// Buffer entries A - single byte immediate
// Original: 0x80 (128), Wolf: 0xF0 (240)
const uint32_t BufEntriesA_Addr[] = { 0x86B919, 0x8637C9, 0x87A2D9, 0x5D1049, 0xA33019 };

// Buffer entries B - single byte immediate
// Original: 0x50 (80), Wolf: 0xA0 (160)
const uint32_t BufEntriesB_Addr[] = { 0x86B940, 0x8637F0, 0x87A300, 0x5D1070, 0xA33040 };

// Max concurrent file handles A - single byte immediate
// Original: 0x0C (12), Wolf: 0x2A (42)
const uint32_t MaxFilesA_Addr[] = { 0x86BAA0, 0x863950, 0x87A460, 0x5D11D0, 0xA331A0 };

// Max concurrent file handles B - single byte immediate
// Original: 0x37 (55), Wolf: 0x47 (71)
const uint32_t MaxFilesB_Addr[] = { 0x86BAEE, 0x86399E, 0x87A4AE, 0x5D121E, 0xA331EE };

// Pool count A - single byte immediate
// Original: 0x06 (6), Wolf: 0x0F (15)
const uint32_t PoolCountA_Addr[] = { 0x86BBD8, 0x863A88, 0x87A598, 0x5D1308, 0xA332D8 };

// Pool count B - single byte immediate
// Original: 0x12 (18), Wolf: 0x15 (21)
const uint32_t PoolCountB_Addr[] = { 0x86BBFF, 0x863AAF, 0x87A5BF, 0x5D132F, 0xA332FF };

struct FileLimitSettings
{
  bool Enable;

  // Pool sizes (32-bit values, represent bytes of virtual memory)
  uint32_t PoolSize4;
  uint32_t PoolSize5;

  // Resource multipliers (single byte each)
  uint8_t ResourceMultA;
  uint8_t ResourceMultB;

  // Buffer entry counts (single byte each)
  uint8_t BufEntriesA;
  uint8_t BufEntriesB;

  // Max concurrent file handles (single byte each, max 255)
  uint8_t MaxFilesA;
  uint8_t MaxFilesB;

  // Pool counts (single byte each)
  uint8_t PoolCountA;
  uint8_t PoolCountB;
};

static FileLimitSettings s_fileLimits = {
  .Enable = false,
  // Defaults: significantly higher than Wolf Limit Break
  .PoolSize4 = 0x40000000,     // 1GB (original: 570MB, Wolf: 711MB)
  .PoolSize5 = 0xFFFF0000,     // ~4GB (original: 2.9GB, Wolf: 3.9GB) - near max for 32-bit immediate
  .ResourceMultA = 16,          // original: 3, Wolf: 6
  .ResourceMultB = 16,          // original: 4, Wolf: 8
  .BufEntriesA = 0xFF,          // 255 (original: 128, Wolf: 240)
  .BufEntriesB = 0xFF,          // 255 (original: 80, Wolf: 160)
  .MaxFilesA = 0xFF,            // 255 (original: 12, Wolf: 42)
  .MaxFilesB = 0xFF,            // 255 (original: 55, Wolf: 71)
  .PoolCountA = 30,             // original: 6, Wolf: 15
  .PoolCountB = 30,             // original: 18, Wolf: 21
};

void FileLimits_Init()
{
  s_fileLimits.Enable = INI_GetBool(IniPath, L"FileLimits", L"Enable", false);

  if (!s_fileLimits.Enable)
    return;

  // Read configurable values from INI (all optional, defaults are above)
  s_fileLimits.PoolSize4 = static_cast<uint32_t>(
    GetPrivateProfileIntW(L"FileLimits", L"PoolSize4MB", s_fileLimits.PoolSize4 / (1024 * 1024), IniPath))
    * 1024 * 1024;
  s_fileLimits.PoolSize5 = static_cast<uint32_t>(
    GetPrivateProfileIntW(L"FileLimits", L"PoolSize5MB", s_fileLimits.PoolSize5 / (1024 * 1024), IniPath))
    * 1024 * 1024;

  s_fileLimits.ResourceMultA = static_cast<uint8_t>(
    GetPrivateProfileIntW(L"FileLimits", L"ResourceMultiplierA", s_fileLimits.ResourceMultA, IniPath));
  s_fileLimits.ResourceMultB = static_cast<uint8_t>(
    GetPrivateProfileIntW(L"FileLimits", L"ResourceMultiplierB", s_fileLimits.ResourceMultB, IniPath));

  s_fileLimits.BufEntriesA = static_cast<uint8_t>(
    GetPrivateProfileIntW(L"FileLimits", L"BufferEntriesA", s_fileLimits.BufEntriesA, IniPath));
  s_fileLimits.BufEntriesB = static_cast<uint8_t>(
    GetPrivateProfileIntW(L"FileLimits", L"BufferEntriesB", s_fileLimits.BufEntriesB, IniPath));

  s_fileLimits.MaxFilesA = static_cast<uint8_t>(
    GetPrivateProfileIntW(L"FileLimits", L"MaxConcurrentFilesA", s_fileLimits.MaxFilesA, IniPath));
  s_fileLimits.MaxFilesB = static_cast<uint8_t>(
    GetPrivateProfileIntW(L"FileLimits", L"MaxConcurrentFilesB", s_fileLimits.MaxFilesB, IniPath));

  s_fileLimits.PoolCountA = static_cast<uint8_t>(
    GetPrivateProfileIntW(L"FileLimits", L"PoolCountA", s_fileLimits.PoolCountA, IniPath));
  s_fileLimits.PoolCountB = static_cast<uint8_t>(
    GetPrivateProfileIntW(L"FileLimits", L"PoolCountB", s_fileLimits.PoolCountB, IniPath));

  // Apply patches
  if (GameAddress(PoolSize4_Addr))
    SafeWrite(GameAddress(PoolSize4_Addr), s_fileLimits.PoolSize4);

  if (GameAddress(PoolSize5_Addr))
    SafeWrite(GameAddress(PoolSize5_Addr), s_fileLimits.PoolSize5);

  if (GameAddress(ResourceMultA_Addr))
    SafeWrite(GameAddress(ResourceMultA_Addr), s_fileLimits.ResourceMultA);

  if (GameAddress(ResourceMultB_Addr))
    SafeWrite(GameAddress(ResourceMultB_Addr), s_fileLimits.ResourceMultB);

  if (GameAddress(BufEntriesA_Addr))
    SafeWrite(GameAddress(BufEntriesA_Addr), s_fileLimits.BufEntriesA);

  if (GameAddress(BufEntriesB_Addr))
    SafeWrite(GameAddress(BufEntriesB_Addr), s_fileLimits.BufEntriesB);

  if (GameAddress(MaxFilesA_Addr))
    SafeWrite(GameAddress(MaxFilesA_Addr), s_fileLimits.MaxFilesA);

  if (GameAddress(MaxFilesB_Addr))
    SafeWrite(GameAddress(MaxFilesB_Addr), s_fileLimits.MaxFilesB);

  if (GameAddress(PoolCountA_Addr))
    SafeWrite(GameAddress(PoolCountA_Addr), s_fileLimits.PoolCountA);

  if (GameAddress(PoolCountB_Addr))
    SafeWrite(GameAddress(PoolCountB_Addr), s_fileLimits.PoolCountB);

  dlog("[FileLimits] Patches applied:\n");
  dlog("  PoolSize4: %u MB\n", s_fileLimits.PoolSize4 / (1024 * 1024));
  dlog("  PoolSize5: %u MB\n", s_fileLimits.PoolSize5 / (1024 * 1024));
  dlog("  ResourceMultA: %d, ResourceMultB: %d\n", s_fileLimits.ResourceMultA, s_fileLimits.ResourceMultB);
  dlog("  BufEntriesA: %d, BufEntriesB: %d\n", s_fileLimits.BufEntriesA, s_fileLimits.BufEntriesB);
  dlog("  MaxFilesA: %d, MaxFilesB: %d\n", s_fileLimits.MaxFilesA, s_fileLimits.MaxFilesB);
  dlog("  PoolCountA: %d, PoolCountB: %d\n", s_fileLimits.PoolCountA, s_fileLimits.PoolCountB);
}

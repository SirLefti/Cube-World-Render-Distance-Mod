// Cube World Alpha - Render Distance Mod
//
// Four separate limits decide how far you can see, and the view ends at the
// SMALLEST of them:
//
//   1. zone creation - the world thread only generates zones within a circle of
//                      768 blocks (5 patches: 1 to increase the limit, 4 to
//                      increase the square boxing the circle)
//   2. zone eviction - zones further than 4 zones from a player are released
//                      once a second (1 patch to increase the limit)
//   3. chunk window  - window size * 32 blocks wide, fog capped at half of
//                      it: 40 / 2 * 32 = 640 blocks (1 patch to increase the
//                      window size)
//   4. residency     - the render distance slider from the options, a budget of
//                      how much chunk data may stay resident, tuned so 100%
//                      roughly fills limit #3 (1 patch to increase the budget)
//
// One square on the world map is a zone. A zone contains 8x8 chunks, a chunk
// contains 32x32 blocks. The unit of the limit we are raising is chunks, and it
// is always half of it in any direction.
//
// Fog is drawn as a circle around the player to the nearest MISSING chunk, so
// it grows outwards while the world is loading, hiding where chunks would pop
// out of nowhere otherwise.
//
// A chunk cannot exist without its zone. #1 and #2 decide which zones can
// exist. #3 decides which chunks inside those zones are generated. #4 decides
// which stay loaded, nearest first. This is why we patch multiple locations.

// ---- build requirements -----------------------------------------------------
#include <stdint.h>   // UINTPTR_MAX - pointer width, checked portably

#if !defined(_WIN32)
#  error "Target must be Windows: this is a DLL injected into Cube.exe. Cross-compile, e.g. i686-w64-mingw32-g++."
#endif
#if UINTPTR_MAX != 0xFFFFFFFFu
#  error "Target must be 32-bit: Cube.exe is PE32."
#endif

#include <windows.h>

// ---- tuning -----------------------------------------------------------------

// max increased window size that feel safe to use
static const uint32_t WINDOW_SIZE_2GB_ADDRESS_SPACE = 46;
static const uint32_t WINDOW_SIZE_4GB_ADDRESS_SPACE = 80;

// ---- game facts -------------------------------------------------------------

static const uint32_t VANILLA_ZONE_RADIUS = 3;          // zones
static const uint32_t VANILLA_ZONE_CUTOFF = 768;        // blocks
static const uint32_t VANILLA_ZONE_EVICT  = 4;          // zones
static const uint32_t VANILLA_WINDOW_SIZE = 40;         // chunks
static const uint32_t VANILLA_BUDGET      = 0x1800000;  // cost units at 100%
static const uint32_t BLOCKS_PER_CHUNK    = 32;
static const uint32_t BLOCKS_PER_ZONE     = 256;

// half the chunk window, in blocks (the fog caps here)
static constexpr uint32_t ViewBlocks(uint32_t windowSize)
{
    return windowSize / 2 * BLOCKS_PER_CHUNK;
}

// margin required because a chunk in the last row requires the neighbour zone
// half of what vanilla implied, but still enough
static const uint32_t ZONE_MARGIN_BLOCKS = 64;

// how many zones are needed to cover the view, rounded up to whole zones
static constexpr uint32_t ZoneRadius(uint32_t windowSize)
{
    return (ViewBlocks(windowSize) + ZONE_MARGIN_BLOCKS + BLOCKS_PER_ZONE - 1) / BLOCKS_PER_ZONE;
}

// the creation scan's circular cutoff, in blocks
static constexpr uint32_t ZoneCutoff(uint32_t windowSize)
{
    return ZoneRadius(windowSize) * BLOCKS_PER_ZONE;
}

// zone eviction keeps one additional zone of hysteresis
static constexpr uint32_t ZoneEvict(uint32_t windowSize)
{
    return ZoneRadius(windowSize) + 1;
}

// the residency budget has to follow the area of the window
static constexpr uint32_t ChunkBudget(uint32_t windowSize)
{
    return (uint32_t)((uint64_t)VANILLA_BUDGET * windowSize * windowSize
                      / ((uint64_t)VANILLA_WINDOW_SIZE * VANILLA_WINDOW_SIZE));
}

// assert that our recalculations reproduce vanilla results with vanilla inputs
static_assert(ZoneRadius(VANILLA_WINDOW_SIZE) == VANILLA_ZONE_RADIUS
              && ZoneCutoff(VANILLA_WINDOW_SIZE) == VANILLA_ZONE_CUTOFF
              && ZoneEvict(VANILLA_WINDOW_SIZE) == VANILLA_ZONE_EVICT,
              "the derivation no longer reproduces the vanilla constants");

// changed values must still fit into an imm8
static_assert(ZoneEvict(WINDOW_SIZE_2GB_ADDRESS_SPACE) * ZoneEvict(WINDOW_SIZE_2GB_ADDRESS_SPACE) <= 127
              && ZoneEvict(WINDOW_SIZE_4GB_ADDRESS_SPACE) * ZoneEvict(WINDOW_SIZE_4GB_ADDRESS_SPACE) <= 127,
              "window size too large: the zone eviction limit does not fit in an imm8");
static_assert(ZoneRadius(WINDOW_SIZE_2GB_ADDRESS_SPACE) <= 127
              && ZoneRadius(WINDOW_SIZE_4GB_ADDRESS_SPACE) <= 127,
              "window size too large: the zone radius does not fit in an imm8");

// ---- patch sites ------------------------------------------------------------
struct PatchSite
{
    uint32_t    rva;        // start of the instruction
    uint32_t    length;     // bytes compared against the vanilla encoding
    const BYTE* expected;   // refuses to touch any other build
    uint32_t    immOffset;  // where the immediate sits inside the instruction
    uint32_t    immSize;    // 1 or 4 bytes
    uint32_t    value;      // what the immediate becomes
};


// #1 p1: squared cutoff of the zone scan
static const BYTE EXPECTED_CUTOFF[] = { 0xBF, 0x00, 0x00, 0x09, 0x00 };
// #1 p2-5: the four zone scan bounds
static const BYTE EXPECTED_X_LO[] = { 0x8D, 0x78, 0xFD };
static const BYTE EXPECTED_X_HI[] = { 0x8D, 0x58, 0x03 };
static const BYTE EXPECTED_Y_LO[] = { 0x8D, 0x50, 0xFD };
static const BYTE EXPECTED_Y_HI[] = { 0x83, 0xC0, 0x03 };
// #2: zone eviction, squared distance in zones
static const BYTE EXPECTED_EVICT[] = { 0x83, 0xFA, 0x10 };
// #3: chunk window side
static const BYTE EXPECTED_GRID[] = { 0xC7, 0x83, 0xDC, 0x02, 0x00, 0x00,
                                      0x28, 0x00, 0x00, 0x00 };
// #4: distance slider 100% budget
static const BYTE EXPECTED_BUDGET[] = { 0xC7, 0x45, 0x94, 0x00, 0x00, 0x80, 
                                        0x01 };

// actual patch sites are declared in DllMain because the values are only known
// at runtime

// ---- address space ----------------------------------------------------------

// check if LARGE_ADDRESS_AWARE flag is set, which allows 32-bit executables on
// 64-bit OS to use up to 4 GB of memory
static bool IsLargeAddressAware(uint32_t base)
{
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const IMAGE_NT_HEADERS32* nt =
        (const IMAGE_NT_HEADERS32*)(base + (uint32_t)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

// check if OS is 64-bit
static bool IsWow64(void)
{
    typedef BOOL (WINAPI *IsWow64ProcessFn)(HANDLE, PBOOL);

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32 == NULL) {
        return false;
    }
    IsWow64ProcessFn isWow64 =
        (IsWow64ProcessFn)GetProcAddress(kernel32, "IsWow64Process");
    if (isWow64 == NULL) {
        return false;
    }
    BOOL wow = FALSE;
    if (!isWow64(GetCurrentProcess(), &wow)) {
        return false;
    }
    return wow != FALSE;
}

// ---- patching ---------------------------------------------------------------
static bool WriteImmediate(BYTE* location, uint32_t value, uint32_t size)
{
    DWORD oldProtection;
    if (!VirtualProtect(location, size, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return false;
    }
    for (uint32_t i = 0; i < size; i++) {
        location[i] = (BYTE)(value >> (i * 8));
    }
    VirtualProtect(location, size, oldProtection, &oldProtection);
    FlushInstructionCache(GetCurrentProcess(), location, size);
    return true;
}

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    uint32_t base = (uint32_t)GetModuleHandle(NULL);

    const bool has4GBAddressSpace = IsLargeAddressAware(base) && IsWow64();
    const uint32_t windowSize = has4GBAddressSpace ? WINDOW_SIZE_4GB_ADDRESS_SPACE
                                                   : WINDOW_SIZE_2GB_ADDRESS_SPACE;

    const uint32_t zoneRadius = ZoneRadius(windowSize);
    const uint32_t negRadius  = (uint32_t)(-(int32_t)zoneRadius);
    const uint32_t zoneCutoff = ZoneCutoff(windowSize);
    const uint32_t zoneEvict  = ZoneEvict(windowSize);

    const PatchSite patches[] = {
        { 0x0006AA97,  5, EXPECTED_CUTOFF, 1, 4, zoneCutoff * zoneCutoff },  // zone cutoff (sq)  [#1 p1]
        { 0x0006AACF,  3, EXPECTED_X_LO,   2, 1, negRadius               },  // zone x low        [#1 p2]
        { 0x0006AAD2,  3, EXPECTED_X_HI,   2, 1, zoneRadius              },  // zone x high       [#1 p3]
        { 0x0006AAD8,  3, EXPECTED_Y_LO,   2, 1, negRadius               },  // zone y low        [#1 p4]
        { 0x0006AADB,  3, EXPECTED_Y_HI,   2, 1, zoneRadius              },  // zone y high       [#1 p5]
        { 0x0006AE04,  3, EXPECTED_EVICT,  2, 1, zoneEvict * zoneEvict   },  // zone evict (sq)   [#2]
        { 0x00059F39, 10, EXPECTED_GRID,   6, 4, windowSize              },  // chunk window side [#3]
        { 0x0006938F,  7, EXPECTED_BUDGET, 3, 4, ChunkBudget(windowSize) },  // residency budget  [#4]
    };

    // Refuse to patch anything that is not the expected Cube.exe build, and
    // check every site before writing any of them to avoid a half-applied mod.
    for (const PatchSite& p : patches) {
        if (memcmp((BYTE*)(base + p.rva), p.expected, p.length) != 0) {
            return TRUE;
        }
    }

    for (const PatchSite& p : patches) {
        WriteImmediate((BYTE*)(base + p.rva + p.immOffset), p.value, p.immSize);
    }

    return TRUE;
}

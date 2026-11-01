// Ace Cull - entity culling + RAM optimizer for Minecraft Bedrock 1.26.60
// (Android arm64, LeviLaunchroid preload-native mod).
//
// How the entity cull works:
//   LevelRendererCamera::queueRenderEntities runs a per-entity callback
//   (a std::function lambda taking Actor&). We find that callback by byte
//   signature, read the three struct offsets it uses straight out of its own
//   machine code (so nothing is hardcoded to one build), then hook it. In the
//   hook we compare the entity's distance to the camera and simply do not call
//   the original for far entities, so they are never queued for drawing.
//   If anything cannot be verified, the cull hook is NOT installed.

#include "AceConfig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <malloc.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

// pl::memory::hook(void* target, void* detour, void** original, HookPriority)
// Imported by symbol name so we do not depend on the header's exact types.
extern "C" int acecull_pl_hook(void *target, void *detour, void **original,
                               int priority)
    __asm__("_ZN2pl6memory4hookEPvS1_PS1_NS0_12HookPriorityE");

namespace {

using acecull::AceConfig;

constexpr const char *kModuleId = "ace_cull.main";
constexpr const char *kCullKey = "cullEnabled";
constexpr const char *kHeavyKey = "heavy";
constexpr const char *kDistanceKey = "distance";
constexpr const char *kBelowKey = "belowBlocks";
constexpr const char *kHeavyPctKey = "heavyPercent";
constexpr const char *kRamAdaptiveKey = "ramAdaptive";
constexpr const char *kRamKey = "ramEnabled";
constexpr const char *kRamIntervalKey = "ramInterval";

// ---------------------------------------------------------------------------
// Signature of the per-entity callback: std::function<void(Actor&)>::operator()
// of the lambda inside LevelRendererCamera::queueRenderEntities (1.26.60).
// Branch/adrp immediates are masked so the pattern survives code movement.
// ---------------------------------------------------------------------------
constexpr uint8_t kSig[] = {
    0xFF,0x83,0x02,0xD1,0xE8,0x2B,0x00,0xFD,0xFD,0x7B,0x06,0xA9,0xF8,0x5F,0x07,0xA9,
    0xF6,0x57,0x08,0xA9,0xF4,0x4F,0x09,0xA9,0xFD,0x83,0x01,0x91,0x57,0xD0,0x3B,0xD5,
    0xF3,0x03,0x00,0xAA,0xF4,0x03,0x01,0xAA,0xE8,0x16,0x40,0xF9,0xA8,0x83,0x1E,0xF8,
    0x08,0x04,0x40,0xF9,0x15,0x10,0x40,0xF9,0x08,0x31,0x41,0x39,0x08,0x00,0x00,0x34,
    0x68,0x0A,0x40,0xF9,0x09,0x41,0x40,0x39,0x09,0x00,0x00,0x34,0x08,0x45,0x40,0x39};
constexpr uint8_t kMask[] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x1F,0x00,0x00,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x1F,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF};
static_assert(sizeof(kSig) == sizeof(kMask), "sig/mask size mismatch");

// ---------------------------------------------------------------------------
// Shared state read by the hot hook (all relaxed atomics, no locks).
// ---------------------------------------------------------------------------
std::atomic_bool gCullOn{false};
std::atomic<float> gCullDist2{64.0f * 64.0f};
std::atomic<float> gBelow{0.0f}; // 0 = off; else cull entities this far below the camera
std::atomic_bool gStop{false};

struct Offsets {
  uint32_t closureToCamera = 0; // closure -> LevelRendererCamera*
  uint32_t cameraPos = 0;       // LevelRendererCamera + this = float[3] camera
  uint32_t actorPosPtr = 0;     // Actor + this = float* (x,y,z)
} gOff;

using CallbackFn = void (*)(void *closure, void *actor);
CallbackFn gOriginal = nullptr;

// The hook. x0 = std::function closure, x1 = Actor*.
void cullDetour(void *closure, void *actor) {
  if (gCullOn.load(std::memory_order_relaxed) && closure && actor) {
    auto *camObj = *reinterpret_cast<char **>(static_cast<char *>(closure) +
                                              gOff.closureToCamera);
    auto *pos = *reinterpret_cast<const float **>(static_cast<char *>(actor) +
                                                  gOff.actorPosPtr);
    if (camObj && pos) {
      const float *cam = reinterpret_cast<const float *>(camObj + gOff.cameraPos);
      const float dx = cam[0] - pos[0];
      const float dy = cam[1] - pos[1];
      const float dz = cam[2] - pos[2];
      const float below = gBelow.load(std::memory_order_relaxed);
      if (below > 0.0f && dy > below) {
        return; // far underneath us: never queued for rendering
      }
      if (dx * dx + dy * dy + dz * dz >
          gCullDist2.load(std::memory_order_relaxed)) {
        return; // too far: never queued for rendering
      }
    }
  }
  gOriginal(closure, actor);
}

// ---------------------------------------------------------------------------
// Memory scanning
// ---------------------------------------------------------------------------
struct Region {
  uintptr_t begin;
  uintptr_t end;
};

std::vector<Region> executableRegions(const char *libName) {
  std::vector<Region> out;
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    if (line.find(libName) == std::string::npos) continue;
    unsigned long long a = 0, b = 0;
    char perms[8] = {};
    if (std::sscanf(line.c_str(), "%llx-%llx %7s", &a, &b, perms) != 3) continue;
    if (perms[0] == 'r' && perms[2] == 'x') {
      out.push_back({static_cast<uintptr_t>(a), static_cast<uintptr_t>(b)});
    }
  }
  return out;
}

std::vector<uintptr_t> scan(const std::vector<Region> &regions) {
  std::vector<uintptr_t> hits;
  const size_t n = sizeof(kSig);
  for (const auto &r : regions) {
    if (r.end - r.begin < n) continue;
    const auto *base = reinterpret_cast<const uint8_t *>(r.begin);
    const size_t len = r.end - r.begin;
    const size_t last = len - n;
    for (size_t i = 0; i <= last; i += 4) { // instructions are 4-byte aligned
      if (base[i] != kSig[0]) continue;
      bool ok = true;
      for (size_t k = 1; k < n; ++k) {
        if ((base[i + k] & kMask[k]) != kSig[k]) { ok = false; break; }
      }
      if (ok) hits.push_back(r.begin + i);
    }
  }
  return hits;
}

// ---------------------------------------------------------------------------
// Read the struct offsets out of the function's own instructions.
// ---------------------------------------------------------------------------
inline int64_t signExt(uint32_t v, int bits) {
  const uint32_t m = 1u << (bits - 1);
  return static_cast<int64_t>((v ^ m)) - static_cast<int64_t>(m);
}

bool deriveOffsets(uintptr_t fn, Offsets &out) {
  const auto *w = reinterpret_cast<const uint32_t *>(fn);
  constexpr int kN = 120;

  auto ldrS = [](uint32_t x, uint32_t &rn, uint32_t &imm) {
    if ((x & 0xFFC00000u) != 0xBD400000u) return false; // ldr s?, [xN, #imm]
    rn = (x >> 5) & 31;
    imm = ((x >> 10) & 0xFFF) * 4;
    return true;
  };

  // 1) three consecutive float loads from the same register: camera x,y,z
  uint32_t camReg = 0, camOff = 0;
  bool haveCam = false;
  for (int i = 0; i < kN - 4 && !haveCam; ++i) {
    uint32_t r0, o0;
    if (!ldrS(w[i], r0, o0)) continue;
    int stage = 1;
    for (int j = i + 1; j < i + 5 && j < kN && stage < 3; ++j) {
      uint32_t r1, o1;
      if (!ldrS(w[j], r1, o1) || r1 != r0) continue;
      if (stage == 1 && o1 == o0 + 4) stage = 2;
      else if (stage == 2 && o1 == o0 + 8) stage = 3;
    }
    if (stage == 3) { camReg = r0; camOff = o0; haveCam = true; }
  }
  if (!haveCam) return false;

  // 2) the closure field that holds that register: ldr xN, [x0, #imm]
  bool haveLrc = false;
  uint32_t lrcOff = 0;
  for (int i = 0; i < kN; ++i) {
    if ((w[i] & 0xFFC003E0u) == 0xF9400000u && (w[i] & 31) == camReg) {
      lrcOff = ((w[i] >> 10) & 0xFFF) * 8;
      haveLrc = true;
      break;
    }
  }
  if (!haveLrc) return false;

  // 3) entity position: "bl getter; ldp s,s,[x0]" -> getter is "ldr x0,[x0,#o]; ret"
  uint32_t posOff = 0;
  bool havePos = false;
  for (int i = 1; i < kN; ++i) {
    if ((w[i] & 0xFFFF83E0u) != 0x2D400000u) continue; // ldp s,s,[x0]
    if ((w[i - 1] >> 26) != 0x25) continue;             // preceded by bl
    const int64_t rel = signExt(w[i - 1] & 0x3FFFFFFu, 26) * 4;
    const auto *target = reinterpret_cast<const uint32_t *>(
        reinterpret_cast<intptr_t>(w + (i - 1)) + rel);
    if ((target[0] & 0xFFC003FFu) == 0xF9400000u && target[1] == 0xD65F03C0u) {
      posOff = ((target[0] >> 10) & 0xFFF) * 8;
      havePos = true;
      break;
    }
  }
  if (!havePos) return false;

  // sanity bounds
  if (camOff < 0x40 || camOff > 0x4000) return false;
  if (lrcOff > 0x200) return false;
  if (posOff < 0x40 || posOff > 0x2000) return false;

  out.cameraPos = camOff;
  out.closureToCamera = lrcOff;
  out.actorPosPtr = posOff;
  return true;
}

// ---------------------------------------------------------------------------
// Crash guard: two early deaths in a row with culling armed => stay disabled.
// ---------------------------------------------------------------------------
std::string guardPath() {
  std::ifstream f("/proc/self/cmdline");
  std::string pkg;
  std::getline(f, pkg, '\0');
  if (pkg.empty()) return {};
  return "/data/data/" + pkg + "/files/acecull.guard";
}
int readGuard(const std::string &p) {
  if (p.empty()) return 0;
  std::ifstream f(p);
  int v = 0;
  f >> v;
  return v;
}
void writeGuard(const std::string &p, int v) {
  if (p.empty()) return;
  std::ofstream f(p, std::ios::trunc);
  f << v;
}

// ---------------------------------------------------------------------------
// Mod
// ---------------------------------------------------------------------------
class AceCull {
public:
  static AceCull &instance() {
    static AceCull mod;
    return mod;
  }

  AceCull() : mSelf(*ll::mod::NativeMod::current()) {}

  bool load() {
    std::lock_guard lock(mMutex);
    mConfigFile.emplace();
    if (!mConfigFile->load()) {
      mSelf.getLogger().error("Failed to load config");
      mConfigFile.reset();
      return false;
    }
    normalize(mConfigFile->value());
    mConfigFile->save();
    return true;
  }

  bool enable() {
    gStop = false;
    mModuleEnabled = true;
    const AceConfig cfg = snapshot();
    applyConfig(cfg);

    const bool ok =
        pl::modmenu::ModuleBuilder(kModuleId, "Ace Cull")
            .modId(mSelf.getId())
            .description("Entity culling + RAM optimizer for 1.26.60.")
            .defaultEnabled(true)
            .onToggle(onToggle)
            .config(kCullKey, "Entity Cull", pl::modmenu::ConfigType::Toggle,
                    cfg.cullEnabled ? "true" : "false")
            .config(kHeavyKey, "Heavy Culling", pl::modmenu::ConfigType::Toggle,
                    cfg.heavy ? "true" : "false")
            .config(kHeavyPctKey, "Heavy Strength (% of distance)",
                    pl::modmenu::ConfigType::SliderInt,
                    std::to_string(cfg.heavyPercent),
                    std::to_string(acecull::kMinHeavyPercent),
                    std::to_string(acecull::kMaxHeavyPercent))
            .config(kDistanceKey, "Cull Distance (blocks)",
                    pl::modmenu::ConfigType::SliderInt,
                    std::to_string(cfg.distance),
                    std::to_string(acecull::kMinDistance),
                    std::to_string(acecull::kMaxDistance))
            .config(kBelowKey, "Cull Below (blocks, 0 = off)",
                    pl::modmenu::ConfigType::SliderInt,
                    std::to_string(cfg.belowBlocks), "0",
                    std::to_string(acecull::kMaxBelow))
            .config(kRamKey, "RAM Optimizer", pl::modmenu::ConfigType::Toggle,
                    cfg.ramEnabled ? "true" : "false")
            .config(kRamAdaptiveKey, "Adaptive RAM (purge only when needed)",
                    pl::modmenu::ConfigType::Toggle,
                    cfg.ramAdaptive ? "true" : "false")
            .config(kRamIntervalKey, "RAM Interval (seconds)",
                    pl::modmenu::ConfigType::SliderInt,
                    std::to_string(cfg.ramInterval),
                    std::to_string(acecull::kMinRamInterval),
                    std::to_string(acecull::kMaxRamInterval))
            .onConfigChanged(onConfigChanged)
            .registerModule();
    if (!ok) mSelf.getLogger().error("Mod menu registration failed");

    if (!mInstaller.joinable()) {
      mInstaller = std::thread([this] { installLoop(); });
    }
    if (!mRamThread.joinable()) {
      mRamThread = std::thread([this] { ramLoop(); });
    }
    return true; // keep running even if the menu failed
  }

  bool disable() {
    gCullOn = false; // hook stays installed but becomes a pass-through
    mModuleEnabled = false;
    pl::modmenu::unregisterModule(kModuleId);
    return true;
  }

  bool unload() {
    gCullOn = false;
    gStop = true;
    pl::modmenu::unregisterModule(kModuleId);
    if (mInstaller.joinable()) mInstaller.join();
    if (mRamThread.joinable()) mRamThread.join();
    std::lock_guard lock(mMutex);
    mConfigFile.reset();
    return true;
  }

private:
  ll::mod::NativeMod &mSelf;
  std::mutex mMutex;
  std::optional<pl::config::ConfigFile<AceConfig>> mConfigFile;
  std::thread mInstaller;
  std::thread mRamThread;
  std::atomic_bool mModuleEnabled{true};
  std::atomic_bool mHookInstalled{false};
  std::atomic_bool mRamOn{true};
  std::atomic_int mRamSeconds{30};
  std::atomic_bool mRamAdaptive{true};

  static void normalize(AceConfig &c) {
    const AceConfig d;
    c.version = d.version;
    c.distance = std::clamp(c.distance, acecull::kMinDistance, acecull::kMaxDistance);
    c.ramInterval = std::clamp(c.ramInterval, acecull::kMinRamInterval,
                               acecull::kMaxRamInterval);
    c.heavyPercent = std::clamp(c.heavyPercent, acecull::kMinHeavyPercent,
                                acecull::kMaxHeavyPercent);
    if (c.belowBlocks <= 0) c.belowBlocks = 0;
    else c.belowBlocks = std::clamp(c.belowBlocks, acecull::kMinBelow, acecull::kMaxBelow);
  }

  AceConfig snapshot() {
    std::lock_guard lock(mMutex);
    if (!mConfigFile) return AceConfig{};
    AceConfig c = mConfigFile->value();
    normalize(c);
    return c;
  }

  // push config into the lock-free globals used by the hook / RAM thread
  void applyConfig(const AceConfig &c) {
    float d = static_cast<float>(c.distance);
    if (c.heavy)
      d = std::max(static_cast<float>(acecull::kMinDistance),
                   d * static_cast<float>(c.heavyPercent) / 100.0f);
    gCullDist2 = d * d;
    gBelow = static_cast<float>(c.belowBlocks);
    gCullOn = mHookInstalled && mModuleEnabled && c.cullEnabled;
    mRamOn = c.ramEnabled;
    mRamSeconds = c.ramInterval;
    mRamAdaptive = c.ramAdaptive;
  }

  static void onToggle(std::string_view id, bool enabled) {
    if (id != kModuleId) return;
    auto &self = instance();
    self.mModuleEnabled = enabled;
    if (enabled && !self.mHookInstalled) {
      // user explicitly re-enabled: clear the crash guard and try again
      writeGuard(guardPath(), 0);
      std::thread([] { instance().tryInstall(true); }).detach();
    }
    self.applyConfig(self.snapshot());
  }

  static void onConfigChanged(std::string_view id, std::string_view key,
                              std::string_view value) {
    if (id != kModuleId) return;
    instance().handleConfig(key, value);
  }

  void handleConfig(std::string_view key, std::string_view value) {
    {
      std::lock_guard lock(mMutex);
      if (!mConfigFile) return;
      auto &c = mConfigFile->value();
      const bool b = (value == "true" || value == "1");
      const int n = std::atoi(std::string(value).c_str());
      if (key == kCullKey) c.cullEnabled = b;
      else if (key == kHeavyKey) c.heavy = b;
      else if (key == kDistanceKey) c.distance = n;
      else if (key == kHeavyPctKey) c.heavyPercent = n;
      else if (key == kRamAdaptiveKey) c.ramAdaptive = b;
      else if (key == kBelowKey) c.belowBlocks = n;
      else if (key == kRamKey) c.ramEnabled = b;
      else if (key == kRamIntervalKey) c.ramInterval = n;
      else return;
      normalize(c);
      mConfigFile->save();
    }
    applyConfig(snapshot());
  }

  // Wait for libminecraftpe.so, then find + verify + hook.
  void installLoop() {
    const std::string guard = guardPath();
    const int strikes = readGuard(guard);
    if (strikes >= 2) {
      mSelf.getLogger().warn(
          "Entity cull is OFF: the game closed early twice with it armed. "
          "Toggle the Ace Cull module off and on to try again.");
      return;
    }
    for (int i = 0; i < 240 && !gStop; ++i) { // up to ~2 minutes
      if (tryInstall(false)) {
        // armed: if we survive 25 s, forgive earlier strikes
        writeGuard(guard, strikes + 1);
        for (int s = 0; s < 25 && !gStop; ++s)
          std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!gStop) writeGuard(guard, 0);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    mSelf.getLogger().error("Entity cull not installed (game library or "
                            "callback not found)");
  }

  bool tryInstall(bool verbose) {
    if (mHookInstalled) return true;
    const auto regions = executableRegions("libminecraftpe.so");
    if (regions.empty()) return false; // game not loaded yet

    const auto hits = scan(regions);
    if (hits.size() != 1) {
      if (verbose || !hits.empty())
        mSelf.getLogger().error("Signature matched {} places (need exactly 1)",
                                hits.size());
      return false;
    }
    Offsets off;
    if (!deriveOffsets(hits[0], off)) {
      mSelf.getLogger().error(
          "Found the entity callback but could not verify its layout; cull "
          "hook NOT installed (RAM optimizer still works).");
      return false;
    }
    gOff = off;

    void *orig = nullptr;
    acecull_pl_hook(reinterpret_cast<void *>(hits[0]),
                    reinterpret_cast<void *>(&cullDetour), &orig, 200);
    if (!orig) {
      mSelf.getLogger().error("pl::memory::hook did not return a trampoline");
      return false;
    }
    gOriginal = reinterpret_cast<CallbackFn>(orig);
    mHookInstalled = true;
    mSelf.getLogger().info(
        "Entity cull armed: camera+{:#x} closure+{:#x} actorPos+{:#x}",
        off.cameraPos, off.closureToCamera, off.actorPosPtr);
    applyConfig(snapshot());
    return true;
  }

  static long readKb(const char *file, const char *key) {
    std::ifstream f(file);
    std::string line;
    const size_t n = std::strlen(key);
    while (std::getline(f, line)) {
      if (line.compare(0, n, key) == 0)
        return std::atol(line.c_str() + n);
    }
    return 0;
  }

  // RAM optimizer: hand freed allocator memory back to Android.
  // Adaptive mode only purges when the phone is low on RAM (<20% free) or the
  // game grew by 200 MB since the last purge, so it does not cause stutters.
  void ramLoop() {
    using MallocTrim = int (*)(size_t);
    auto trim = reinterpret_cast<MallocTrim>(dlsym(RTLD_DEFAULT, "malloc_trim"));
    constexpr int kMPurge = -101;     // M_PURGE
    constexpr int kMPurgeAll = -104;  // M_PURGE_ALL
    long lastRss = readKb("/proc/self/status", "VmRSS:");
    while (!gStop) {
      const int secs = std::max(acecull::kMinRamInterval, mRamSeconds.load());
      for (int s = 0; s < secs && !gStop; ++s)
        std::this_thread::sleep_for(std::chrono::seconds(1));
      if (gStop) break;
      if (!mRamOn || !mModuleEnabled) continue;
      if (mRamAdaptive) {
        const long avail = readKb("/proc/meminfo", "MemAvailable:");
        const long total = readKb("/proc/meminfo", "MemTotal:");
        const long rss = readKb("/proc/self/status", "VmRSS:");
        const bool low = total > 0 && avail > 0 && avail * 100 < total * 20;
        const bool grew = rss - lastRss > 200 * 1024;
        if (!low && !grew) { lastRss = std::min(lastRss, rss); continue; }
      }
      mallopt(kMPurgeAll, 0);
      mallopt(kMPurge, 0);
      if (trim) trim(0);
      lastRss = readKb("/proc/self/status", "VmRSS:");
    }
  }
};

} // namespace

PL_REGISTER_MOD(AceCull, AceCull::instance())

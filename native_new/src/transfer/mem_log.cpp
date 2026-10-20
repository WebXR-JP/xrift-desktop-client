// mem_log の実体。Windows 専用であるため、非 Windows では何もしない
// スタブにしてビルドを通す（転送自体が Windows のみのため）。
#ifdef _WIN32

#include "transfer/mem_log.h"

#include <windows.h>

#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#include "common/log.h"

namespace xrift::transfer {
namespace {

// 生存数の計数。確保・破棄の箇所で更新する。
std::atomic<int64_t> g_liveImages{0};
std::atomic<int64_t> g_liveImageBytes{0};
std::atomic<int64_t> g_liveMemories{0};
std::atomic<int64_t> g_liveMemoryBytes{0};
std::atomic<int64_t> g_imports{0};
std::atomic<int64_t> g_frees{0};

uint64_t SteadyMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

}  // namespace

void TrackVkImageCreate(uint64_t bytes) {
  g_liveImages.fetch_add(1, std::memory_order_relaxed);
  g_liveImageBytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void TrackVkImageDestroy(uint64_t bytes) {
  g_liveImages.fetch_sub(1, std::memory_order_relaxed);
  g_liveImageBytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void TrackVkMemoryAlloc(uint64_t bytes) {
  g_liveMemories.fetch_add(1, std::memory_order_relaxed);
  g_liveMemoryBytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void TrackVkMemoryFree(uint64_t bytes) {
  g_liveMemories.fetch_sub(1, std::memory_order_relaxed);
  g_liveMemoryBytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void TrackImport() { g_imports.fetch_add(1, std::memory_order_relaxed); }

void TrackImportFree() { g_frees.fetch_add(1, std::memory_order_relaxed); }

VkLive GetVkLive() {
  VkLive snapshot{};
  snapshot.images = g_liveImages.load(std::memory_order_relaxed);
  snapshot.imageBytes = g_liveImageBytes.load(std::memory_order_relaxed);
  snapshot.memories = g_liveMemories.load(std::memory_order_relaxed);
  snapshot.memoryBytes = g_liveMemoryBytes.load(std::memory_order_relaxed);
  snapshot.imports = g_imports.load(std::memory_order_relaxed);
  snapshot.frees = g_frees.load(std::memory_order_relaxed);
  return snapshot;
}

namespace {

// プロセスメモリの観測点。取得失敗時は 0 を出す（計測の失敗は転送に影響させない）。
void SampleProcess(uint64_t* privateBytes, uint64_t* workingSetBytes, uint64_t* handleCount) {
  *privateBytes = 0;
  *workingSetBytes = 0;
  *handleCount = 0;
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) != 0) {
    *privateBytes = static_cast<uint64_t>(counters.PrivateUsage);
    *workingSetBytes = static_cast<uint64_t>(counters.WorkingSetSize);
  }
  DWORD handles = 0;
  if (::GetProcessHandleCount(::GetCurrentProcess(), &handles) != 0) {
    *handleCount = handles;
  }
}

}  // namespace

// MemLogger のスレッド状態。ヘッダに置くと <thread> 等が漏れるため pImpl 相当を
// ここに置く。公開ヘッダは path / mode / 参照 atomic だけを持つ。
class MemLoggerWorker {
 public:
  MemLoggerWorker(const std::string& path, const std::string& mode,
                  const std::atomic<uint64_t>* framesOk,
                  const std::atomic<uint64_t>* framesFailed)
      : path_(path), mode_(mode), framesOk_(framesOk), framesFailed_(framesFailed) {}

  void Start() {
    if (path_.empty()) {
      return;
    }
    out_.open(path_, std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
      Error("メモリログを開けない: %s（計測なしで続行する）", path_.c_str());
      return;
    }
    out_ << "t_ms,mode,privateBytes,workingSetBytes,handleCount,liveVkImages,"
            "liveVkImageBytes,liveVkMemories,liveVkMemoryBytes,imports,frees,framesOk,"
            "framesFailed\n";
    out_.flush();
    t0Ms_ = SteadyMs();
    running_.store(true, std::memory_order_release);
    // 開始直後の 1 行を同期的に出す（「開始」値の証拠にする）。
    WriteSample();
    worker_ = std::thread([this] {
      while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_.load(std::memory_order_acquire)) {
          break;
        }
        WriteSample();
      }
    });
    Info("メモリログ開始: %s（1 秒周期）", path_.c_str());
  }

  void Stop() {
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    if (worker_.joinable()) {
      worker_.join();
    } else if (!wasRunning) {
      return;
    }
    // 終了直後の 1 行を出す（「終了」値の証拠にする）。
    if (out_.is_open()) {
      WriteSample();
      out_.flush();
      out_.close();
    }
  }

 private:
  void WriteSample() {
    uint64_t privateBytes = 0;
    uint64_t workingSetBytes = 0;
    uint64_t handleCount = 0;
    SampleProcess(&privateBytes, &workingSetBytes, &handleCount);
    const VkLive live = GetVkLive();
    const uint64_t ok = framesOk_ != nullptr ? framesOk_->load(std::memory_order_relaxed) : 0;
    const uint64_t failed =
        framesFailed_ != nullptr ? framesFailed_->load(std::memory_order_relaxed) : 0;
    const uint64_t tMs = SteadyMs() - t0Ms_;
    out_ << tMs << ',' << mode_ << ',' << privateBytes << ',' << workingSetBytes << ','
         << handleCount << ',' << live.images << ',' << live.imageBytes << ',' << live.memories
         << ',' << live.memoryBytes << ',' << live.imports << ',' << live.frees << ',' << ok
         << ',' << failed << '\n';
    // 1 秒周期のため毎行 flush する（異常終了時にも行を残す）。
    out_.flush();
  }

  std::string path_;
  std::string mode_;
  const std::atomic<uint64_t>* framesOk_ = nullptr;
  const std::atomic<uint64_t>* framesFailed_ = nullptr;
  std::ofstream out_;
  std::thread worker_;
  std::atomic<bool> running_{false};
  uint64_t t0Ms_ = 0;
};

MemLogger::MemLogger(const std::string& path, const std::string& mode,
                     const std::atomic<uint64_t>* framesOk,
                     const std::atomic<uint64_t>* framesFailed)
    : path_(path), mode_(mode), framesOk_(framesOk), framesFailed_(framesFailed) {}

MemLogger::~MemLogger() { Stop(); }

void MemLogger::Start() {
  workerStorage_ = std::make_unique<MemLoggerWorker>(path_, mode_, framesOk_, framesFailed_);
  workerStorage_->Start();
}

void MemLogger::Stop() {
  if (workerStorage_) {
    workerStorage_->Stop();
    workerStorage_.reset();
  }
}

}  // namespace xrift::transfer

#else

#include "transfer/mem_log.h"

namespace xrift::transfer {

// ヘッダの unique_ptr を完結させるための空定義（Windows 側は実体を持つ）。
class MemLoggerWorker {};

void TrackVkImageCreate(uint64_t) {}
void TrackVkImageDestroy(uint64_t) {}
void TrackVkMemoryAlloc(uint64_t) {}
void TrackVkMemoryFree(uint64_t) {}
void TrackImport() {}
void TrackImportFree() {}
VkLive GetVkLive() { return VkLive{}; }

MemLogger::MemLogger(const std::string&, const std::string&, const std::atomic<uint64_t>*,
                     const std::atomic<uint64_t>*) {}
MemLogger::~MemLogger() = default;
void MemLogger::Start() {}
void MemLogger::Stop() {}

}  // namespace xrift::transfer

#endif

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// GPU メモリ枯渇の切り分け用の計測。修正ではなく観測だけを行う。
//  - 生存 VkImage / VkDeviceMemory の数と要求バイト数は、確保・破棄の箇所で
//    カウンタを更新して出す（実際の確保 API を数える）。
//  - 1 秒周期の CSV ログ（exe 側の Private Bytes / RSS / ハンドル数と合わせる）。
// Windows 以外では何もしない（転送自体が Windows のみのため）。

namespace xrift::transfer {

// 確保・破棄の箇所から呼ぶ。呼び出しは単一の転送スレッド、読みはログスレッドのため
// atomic にする。image と memory は対で数える（import 1 件 = image 1 + memory 1）。
void TrackVkImageCreate(uint64_t bytes);  // vkCreateImage の成功時（要求バイト数付き）。
void TrackVkImageDestroy(uint64_t bytes);
void TrackVkMemoryAlloc(uint64_t bytes);  // vkAllocateMemory の成功時（確保バイト数付き）。
void TrackVkMemoryFree(uint64_t bytes);
void TrackImport();      // import の確保対（bind 完了）の累計。
void TrackImportFree();  // import の確保対の破棄の累計。

struct VkLive {
  int64_t images = 0;
  int64_t imageBytes = 0;
  int64_t memories = 0;
  int64_t memoryBytes = 0;
  int64_t imports = 0;
  int64_t frees = 0;
};

// 現在の生存数と累計の複写を返す。
VkLive GetVkLive();

// 1 秒周期で CSV に出す。列は
// t_ms,mode,privateBytes,workingSetBytes,handleCount,liveVkImages,liveVkImageBytes,
// liveVkMemories,liveVkMemoryBytes,imports,frees,framesOk,framesFailed。
// framesOk / framesFailed は転送ループ側の atomic を参照する（所有しない）。
// 空 path では何もしない（通常経路の挙動は変えない）。
class MemLoggerWorker;

class MemLogger {
 public:
  MemLogger(const std::string& path, const std::string& mode,
            const std::atomic<uint64_t>* framesOk, const std::atomic<uint64_t>* framesFailed);
  ~MemLogger();

  MemLogger(const MemLogger&) = delete;
  MemLogger& operator=(const MemLogger&) = delete;

  void Start();  // ヘッダ書き・スレッド開始。失敗時は Error を出して無効化する。
  void Stop();   // 停止・合流（二重呼び出し可。デストラクタでも呼ぶ）。

 private:
  std::string path_;
  std::string mode_;
  const std::atomic<uint64_t>* framesOk_ = nullptr;
  const std::atomic<uint64_t>* framesFailed_ = nullptr;
  // スレッド状態は .cpp 側に置き、ヘッダから <thread> 等を漏らさない。
  std::unique_ptr<MemLoggerWorker> workerStorage_;
};

}  // namespace xrift::transfer

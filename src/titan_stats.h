#pragma once

#include "rocksdb/statistics.h"
#include "titan/options.h"

#include <atomic>
#include <map>
#include <string>
#include <unordered_map>

namespace rocksdb {
namespace titandb {

// Titan internal stats does NOT optimize race
// condition by making thread local copies of
// data.
class TitanInternalStats {
 public:
  enum StatsType {
    LIVE_BLOB_SIZE,
    NUM_LIVE_BLOB_FILE,
    NUM_OBSOLETE_BLOB_FILE,
    LIVE_BLOB_FILE_SIZE,
    OBSOLETE_BLOB_FILE_SIZE,
    INTERNAL_STATS_ENUM_MAX,
  };
  void Clear() {
    for (int i = 0; i < INTERNAL_STATS_ENUM_MAX; i++) {
      stats_[i].store(0, std::memory_order_relaxed);
    }
  }
  void ResetStats(StatsType type) {
    stats_[type].store(0, std::memory_order_relaxed);
  }
  void AddStats(StatsType type, uint64_t value) {
    auto& v = stats_[type];
    v.fetch_add(value, std::memory_order_relaxed);
  }
  void SubStats(StatsType type, uint64_t value) {
    auto& v = stats_[type];
    v.fetch_sub(value, std::memory_order_relaxed);
  }
  bool GetIntProperty(const Slice& property, uint64_t* value) const {
    auto p = stats_type_string_map.find(property.ToString());
    if (p != stats_type_string_map.end()) {
      *value = stats_[p->second].load(std::memory_order_relaxed);
      return true;
    }
    return false;
  }
  bool GetStringProperty(const Slice& property, std::string* value) const {
    uint64_t int_value;
    if (GetIntProperty(property, &int_value)) {
      *value = std::to_string(int_value);
      return true;
    }
    return false;
  }

 private:
  static const std::unordered_map<std::string, TitanInternalStats::StatsType>
      stats_type_string_map;
  std::atomic<uint64_t> stats_[INTERNAL_STATS_ENUM_MAX];
};

class TitanStats {
 public:
  TitanStats(Statistics* stats) : stats_(stats) {}
  Status Initialize(std::map<uint32_t, TitanCFOptions> cf_options,
                    uint32_t default_cf) {
    for (auto& opts : cf_options) {
      internal_stats_[opts.first] = NewTitanInternalStats(opts.second);
    }
    default_cf_ = default_cf;
    return Status::OK();
  }
  Statistics* statistics() { return stats_; }
  TitanInternalStats* internal_stats(uint32_t cf_id) {
    auto p = internal_stats_.find(cf_id);
    if (p == internal_stats_.end()) {
      return nullptr;
    } else {
      return p->second.get();
    }
  }

 private:
  Statistics* stats_ = nullptr;
  uint32_t default_cf_ = 0;
  std::unordered_map<uint32_t, std::shared_ptr<TitanInternalStats>>
      internal_stats_;
  std::shared_ptr<TitanInternalStats> NewTitanInternalStats(
      TitanCFOptions& opts) {
    return std::make_shared<TitanInternalStats>();
  }
};

// Utility functions
inline Statistics* statistics(TitanStats* stats) {
  return (stats) ? stats->statistics() : nullptr;
}

inline void RecordTick(TitanStats* stats, uint32_t ticker_type,
                       uint64_t count = 1) {
  if (stats && stats->statistics()) {
    stats->statistics()->recordTick(ticker_type, count);
  }
}

inline void MeasureTime(TitanStats* stats, uint32_t histogram_type,
                        uint64_t time) {
  if (stats && stats->statistics()) {
    stats->statistics()->measureTime(histogram_type, time);
  }
}

inline void SetTickerCount(TitanStats* stats, uint32_t ticker_type,
                           uint64_t count) {
  if (stats && stats->statistics()) {
    stats->statistics()->setTickerCount(ticker_type, count);
  }
}

inline void ResetStats(TitanStats* stats, uint32_t cf_id,
                       TitanInternalStats::StatsType type) {
  if (stats) {
    auto p = stats->internal_stats(cf_id);
    if (p) {
      p->ResetStats(type);
    }
  }
}

inline void AddStats(TitanStats* stats, uint32_t cf_id,
                     TitanInternalStats::StatsType type, uint64_t value) {
  if (stats) {
    auto p = stats->internal_stats(cf_id);
    if (p) {
      p->AddStats(type, value);
    }
  }
}

inline void SubStats(TitanStats* stats, uint32_t cf_id,
                     TitanInternalStats::StatsType type, uint64_t value) {
  if (stats) {
    auto p = stats->internal_stats(cf_id);
    if (p) {
      p->SubStats(type, value);
    }
  }
}

}  // namespace titandb
}  // namespace rocksdb

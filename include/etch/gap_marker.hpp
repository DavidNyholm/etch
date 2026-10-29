// etch - the async sink's gap markers: the warning the worker writes for records the drop
// policy lost, and the exclusion set it carries so a marker cannot restart the chain the
// dropped records came from (see Sink and delivery.hpp).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <source_location>
#include <thread>
#include <utility>

#include "delivery.hpp"
#include "level.hpp"
#include "record.hpp"

namespace etch::detail {

// The sinks that the nested records dropped since the last marker came from inside of, merged
// under a small mutex on the drop path (ordinary records have no lineage and never take it).
// The set only grows: extra exclusions are harmless, missing ones are not.
class GapMarkers {
 public:
  // Adds the record's lineage and ancestry. False when memory for it ran out (the caller
  // counts a failure; the next merge may complete the set).
  bool merge(const Record& r) noexcept {
    if (r.lineage == nullptr && r.ancestry == nullptr) return true;
    try {
      std::lock_guard lock(mutex_);
      for (const auto& source : {r.lineage, r.ancestry}) {
        if (source == nullptr) continue;
        for (const Sink* sink : *source) {
          if (std::find(lineage_.begin(), lineage_.end(), sink) == lineage_.end()) {
            lineage_.push_back(sink);
          }
        }
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  // The set as a marker's ancestry: what is logged in response to the marker inherits it.
  // Null while the set is empty.
  std::shared_ptr<const Lineage> ancestry() const {
    std::lock_guard lock(mutex_);
    if (lineage_.empty()) return nullptr;
    return std::make_shared<Lineage>(lineage_);
  }

 private:
  mutable std::mutex mutex_;
  Lineage lineage_;
};

// One warning from logger "etch" that says how many records were dropped since the previous
// report. The marker goes wherever the inner sink and anything behind it send it (it is the
// queue's own word about itself).
inline Record gap_marker_record(std::size_t gap, Level level,
                                std::shared_ptr<const Lineage> ancestry) {
  Record marker;
  marker.level = level;
  marker.time = std::chrono::system_clock::now();
  marker.thread = std::this_thread::get_id();
  marker.thread_index = this_thread_index();
  marker.where = std::source_location::current();
  marker.logger_name = "etch";
  marker.message =
      std::format("{} records dropped by the async queue since the previous report", gap);
  marker.fields.emplace_back("dropped", gap);
  marker.fields.emplace_back(kEtchField, "gap");  // after `dropped`, which 0.4 readers index
  marker.ancestry = std::move(ancestry);
  return marker;
}

}  // namespace etch::detail

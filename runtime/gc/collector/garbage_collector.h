/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_GC_COLLECTOR_GARBAGE_COLLECTOR_H_
#define ART_RUNTIME_GC_COLLECTOR_GARBAGE_COLLECTOR_H_

#include <stdint.h>
#include <list>

#include "base/histogram.h"
#include "base/macros.h"
#include "base/metrics/metrics.h"
#include "base/mutex.h"
#include "base/timing_logger.h"
#include "gc/collector_type.h"
#include "gc/gc_cause.h"
#include "gc_root.h"
#include "gc_type.h"
#include "iteration.h"
#include "object_byte_pair.h"
#include "object_callbacks.h"

namespace art HIDDEN {

namespace mirror {
class Class;
class Object;
class Reference;
}  // namespace mirror

namespace gc {

class Heap;

namespace accounting {
template <typename T>
class AtomicStack;
using ObjectStack = AtomicStack<mirror::Object>;
}  // namespace accounting

namespace space {
class ContinuousSpace;
}  // namespace space

namespace collector {
class GarbageCollector : public RootVisitor, public IsMarkedVisitor, public MarkObjectVisitor {
 public:
  class SCOPED_LOCKABLE ScopedPause {
   public:
    explicit ScopedPause(GarbageCollector* collector, bool with_reporting = true)
        EXCLUSIVE_LOCK_FUNCTION(Locks::mutator_lock_);
    ~ScopedPause() UNLOCK_FUNCTION();

   private:
    const uint64_t start_time_;
    GarbageCollector* const collector_;
    bool with_reporting_;
  };

  GarbageCollector(Heap* heap, const std::string& name);
  virtual ~GarbageCollector() { }
  const char* GetName() const {
    return name_.c_str();
  }
  virtual GcType GetGcType() const = 0;
  virtual CollectorType GetCollectorType() const = 0;
  // Run the garbage collector.
  void Run(GcCause gc_cause, bool clear_soft_references) REQUIRES(!pause_histogram_lock_);
  Heap* GetHeap() const {
    return heap_;
  }
  void RegisterPause(uint64_t nano_length);
  const CumulativeLogger& GetCumulativeTimings() const {
    return cumulative_timings_;
  }
  // Swap the live and mark bitmaps of spaces that are active for the collector. For partial GC,
  // this is the allocation space, for full GC then we swap the zygote bitmaps too.
  void SwapBitmaps()
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  uint64_t GetTotalCpuTime() const {
    return total_thread_cpu_time_ns_;
  }
  uint64_t GetTotalPausedTimeNs() REQUIRES(!pause_histogram_lock_);
  int64_t GetTotalFreedBytes() const {
    return total_freed_bytes_;
  }
  uint64_t GetTotalFreedObjects() const {
    return total_freed_objects_;
  }
  uint64_t GetTotalScannedBytes() const {
    return total_scanned_bytes_;
  }
  // Reset the cumulative timings and pause histogram.
  void ResetMeasurements() REQUIRES(!pause_histogram_lock_);
  // Returns the estimated throughput in bytes / second.
  uint64_t GetEstimatedMeanThroughput() const;
  // Returns how many GC iterations have been run.
  size_t NumberOfIterations() const {
    return GetCumulativeTimings().GetIterations();
  }
  // Returns the current GC iteration and assocated info.
  Iteration* GetCurrentIteration();
  const Iteration* GetCurrentIteration() const;
  TimingLogger* GetTimings() {
    return &GetCurrentIteration()->timings_;
  }
  // Record a free of normal objects.
  void RecordFree(const ObjectBytePair& freed);
  // Record a free of large objects.
  void RecordFreeLOS(const ObjectBytePair& freed);
  virtual void DumpPerformanceInfo(std::ostream& os) REQUIRES(!pause_histogram_lock_);

  // Extract RSS for GC-specific memory ranges using mincore().
  uint64_t ExtractRssFromMincore(std::list<std::pair<void*, void*>>* gc_ranges);

  // Helper functions for querying if objects are marked. These are used for processing references,
  // and will be used for reading system weaks while the GC is running.
  virtual mirror::Object* IsMarked(mirror::Object* obj)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  // Returns true if the given heap reference is null or is already marked. If it's already marked,
  // update the reference (uses a CAS if do_atomic_update is true). Otherwise, returns false.
  virtual bool IsNullOrMarkedHeapReference(mirror::HeapReference<mirror::Object>* obj,
                                           bool do_atomic_update)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  // Used by reference processor.
  virtual void ProcessMarkStack() REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  // Force mark an object.
  virtual mirror::Object* MarkObject(mirror::Object* obj)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  virtual void MarkHeapReference(mirror::HeapReference<mirror::Object>* obj,
                                 bool do_atomic_update)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  virtual void DelayReferenceReferent(ObjPtr<mirror::Class> klass,
                                      ObjPtr<mirror::Reference> reference)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  bool IsTransactionActive() const {
    return is_transaction_active_;
  }

  bool ShouldEagerlyReleaseMemoryToOS() const;

 protected:
  // Run all of the GC phases.
  virtual void RunPhases() REQUIRES(!Locks::mutator_lock_) = 0;
  // Revoke all the thread-local buffers.
  virtual void RevokeAllThreadLocalBuffers() = 0;
  // Deallocates unmarked objects referenced by 'obj_arr' that reside either in the
  // given continuous-spaces or in large-object space. WARNING: Trashes objects.
  void SweepArray(accounting::ObjectStack* obj_arr,
                  bool swap_bitmaps,
                  std::vector<space::ContinuousSpace*>* sweep_spaces)
      REQUIRES(Locks::heap_bitmap_lock_) REQUIRES_SHARED(Locks::mutator_lock_);

  static constexpr size_t kPauseBucketSize = 500;
  static constexpr size_t kPauseBucketCount = 32;
  static constexpr size_t kMemBucketSize = 10;
  static constexpr size_t kMemBucketCount = 16;

  Heap* const heap_;
  std::string name_;
  // Cumulative statistics.
  Histogram<uint64_t> pause_histogram_ GUARDED_BY(pause_histogram_lock_);
  Histogram<uint64_t> rss_histogram_;
  Histogram<size_t> freed_bytes_histogram_;
  metrics::MetricsBase<int64_t>* gc_time_histogram_;
  metrics::MetricsBase<uint64_t>* metrics_gc_count_;
  metrics::MetricsBase<uint64_t>* metrics_gc_count_delta_;
  metrics::MetricsBase<int64_t>* gc_throughput_histogram_;
  metrics::MetricsBase<int64_t>* gc_tracing_throughput_hist_;
  metrics::MetricsBase<uint64_t>* gc_throughput_avg_;
  metrics::MetricsBase<uint64_t>* gc_tracing_throughput_avg_;
  metrics::MetricsBase<uint64_t>* gc_scanned_bytes_;
  metrics::MetricsBase<uint64_t>* gc_scanned_bytes_delta_;
  metrics::MetricsBase<uint64_t>* gc_freed_bytes_;
  metrics::MetricsBase<uint64_t>* gc_freed_bytes_delta_;
  metrics::MetricsBase<uint64_t>* gc_duration_;
  metrics::MetricsBase<uint64_t>* gc_duration_delta_;
  metrics::MetricsBase<uint64_t>* gc_app_slow_path_during_gc_duration_delta_;
  uint64_t total_thread_cpu_time_ns_;
  uint64_t total_time_ns_;
  uint64_t total_freed_objects_;
  int64_t total_freed_bytes_;
  uint64_t total_scanned_bytes_;
  CumulativeLogger cumulative_timings_;
  mutable Mutex pause_histogram_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  bool is_transaction_active_;
  // The garbage collector algorithms will either have all the metrics pointers
  // (above) initialized, or none of them. So instead of checking each time, we
  // use this flag.
  bool are_metrics_initialized_;

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(GarbageCollector);
};

}  // namespace collector
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_COLLECTOR_GARBAGE_COLLECTOR_H_

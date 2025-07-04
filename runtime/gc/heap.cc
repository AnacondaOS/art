/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "heap.h"

#include <sys/types.h>
#include <unistd.h>

#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <vector>

#include "allocation_listener.h"
#include "android-base/stringprintf.h"
#include "android-base/thread_annotations.h"
#include "art_field-inl.h"
#include "backtrace_helper.h"
#include "base/allocator.h"
#include "base/arena_allocator.h"
#include "base/dumpable.h"
#include "base/file_utils.h"
#include "base/histogram-inl.h"
#include "base/logging.h"  // For VLOG.
#include "base/memory_tool.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/utils.h"
#include "class_root-inl.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex/dex_file-inl.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table-inl.h"
#include "gc/accounting/read_barrier_table.h"
#include "gc/accounting/remembered_set.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/collector/concurrent_copying.h"
#include "gc/collector/mark_compact.h"
#include "gc/collector/mark_sweep.h"
#include "gc/collector/partial_mark_sweep.h"
#include "gc/collector/semi_space.h"
#include "gc/collector/sticky_mark_sweep.h"
#include "gc/racing_check.h"
#include "gc/reference_processor.h"
#include "gc/scoped_gc_critical_section.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/space/dlmalloc_space-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/region_space.h"
#include "gc/space/rosalloc_space-inl.h"
#include "gc/space/space-inl.h"
#include "gc/space/zygote_space.h"
#include "gc/task_processor.h"
#include "gc/verification.h"
#include "gc_pause_listener.h"
#include "gc_root.h"
#include "handle_scope-inl.h"
#include "heap-inl.h"
#include "heap-visit-objects-inl.h"
#include "intern_table.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jni/java_vm_ext.h"
#include "mirror/class-inl.h"
#include "mirror/executable-inl.h"
#include "mirror/field.h"
#include "mirror/method_handle_impl.h"
#include "mirror/object-inl.h"
#include "mirror/object-refvisitor-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/reference-inl.h"
#include "mirror/var_handle.h"
#include "nativehelper/scoped_local_ref.h"
#include "oat/image.h"
#include "obj_ptr-inl.h"
#ifdef ART_TARGET_ANDROID
#include "perfetto/heap_profile.h"
#endif
#include "reflection.h"
#include "runtime.h"
#include "javaheapprof/javaheapsampler.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "verify_object-inl.h"
#include "well_known_classes.h"

#if defined(__BIONIC__) || defined(__GLIBC__) || defined(ANDROID_HOST_MUSL)
#include <malloc.h>  // For mallinfo()
#endif

namespace art HIDDEN {

#ifdef ART_TARGET_ANDROID
namespace {

// Enable the heap sampler Callback function used by Perfetto.
void EnableHeapSamplerCallback(void* enable_ptr,
                               const AHeapProfileEnableCallbackInfo* enable_info_ptr) {
  HeapSampler* sampler_self = reinterpret_cast<HeapSampler*>(enable_ptr);
  // Set the ART profiler sampling interval to the value from Perfetto.
  uint64_t interval = AHeapProfileEnableCallbackInfo_getSamplingInterval(enable_info_ptr);
  if (interval > 0) {
    sampler_self->SetSamplingInterval(interval);
  }
  // Else default is 4K sampling interval. However, default case shouldn't happen for Perfetto API.
  // AHeapProfileEnableCallbackInfo_getSamplingInterval should always give the requested
  // (non-negative) sampling interval. It is a uint64_t and gets checked for != 0
  // Do not call heap as a temp here, it will build but test run will silently fail.
  // Heap is not fully constructed yet in some cases.
  sampler_self->EnableHeapSampler();
}

// Disable the heap sampler Callback function used by Perfetto.
void DisableHeapSamplerCallback(void* disable_ptr,
                                [[maybe_unused]] const AHeapProfileDisableCallbackInfo* info_ptr) {
  HeapSampler* sampler_self = reinterpret_cast<HeapSampler*>(disable_ptr);
  sampler_self->DisableHeapSampler();
}

}  // namespace
#endif

namespace gc {

DEFINE_RUNTIME_DEBUG_FLAG(Heap, kStressCollectorTransition);

// Minimum amount of remaining bytes before a concurrent GC is triggered.
static constexpr size_t kMinConcurrentRemainingBytes = 128 * KB;
static constexpr size_t kMaxConcurrentRemainingBytes = 512 * KB;
// Sticky GC throughput adjustment. Increasing this causes sticky GC to occur more
// relative to partial/full GC. This may be desirable since sticky GCs interfere less
// with mutator threads (lower pauses, use less memory bandwidth). The value
// (1.0) for non-generational GC case is fixed and shall never change.
static double GetStickyGcThroughputAdjustment(bool use_generational_gc) {
  return use_generational_gc ? 0.5 : 1.0;
}
// Whether or not we compact the zygote in PreZygoteFork.
static constexpr bool kCompactZygote = kMovingCollector;
// How many reserve entries are at the end of the allocation stack, these are only needed if the
// allocation stack overflows.
static constexpr size_t kAllocationStackReserveSize = 1024;
// Default mark stack size in bytes.
static const size_t kDefaultMarkStackSize = 64 * KB;
// Define space name.
static const char* kDlMallocSpaceName[2] = {"main dlmalloc space", "main dlmalloc space 1"};
static const char* kRosAllocSpaceName[2] = {"main rosalloc space", "main rosalloc space 1"};
static const char* kMemMapSpaceName[2] = {"main space", "main space 1"};
static const char* kNonMovingSpaceName = "non moving space";
static const char* kZygoteSpaceName = "zygote space";
static constexpr bool kGCALotMode = false;
// GC alot mode uses a small allocation stack to stress test a lot of GC.
static constexpr size_t kGcAlotAllocationStackSize = 4 * KB /
    sizeof(mirror::HeapReference<mirror::Object>);
// Verify objet has a small allocation stack size since searching the allocation stack is slow.
static constexpr size_t kVerifyObjectAllocationStackSize = 16 * KB /
    sizeof(mirror::HeapReference<mirror::Object>);
static constexpr size_t kDefaultAllocationStackSize = 8 * MB /
    sizeof(mirror::HeapReference<mirror::Object>);

// If we violate BOTH of the following constraints, we throw OOME.
// They differ due to concurrent allocation.
// After a GC (due to allocation failure) we should retrieve at least this
// fraction of the current max heap size.
static constexpr double kMinFreedHeapAfterGcForAlloc = 0.05;
// After a GC (due to allocation failure), at least this fraction of the
// heap should be available.
static constexpr double kMinFreeHeapAfterGcForAlloc = 0.01;

// For deterministic compilation, we need the heap to be at a well-known address.
static constexpr uint32_t kAllocSpaceBeginForDeterministicAoT = 0x40000000;
// Dump the rosalloc stats on SIGQUIT.
static constexpr bool kDumpRosAllocStatsOnSigQuit = false;

static const char* kRegionSpaceName = "main space (region space)";

// If true, we log all GCs in the both the foreground and background. Used for debugging.
static constexpr bool kLogAllGCs = false;

// Use Max heap for 2 seconds, this is smaller than the usual 5s window since we don't want to leave
// allocate with relaxed ergonomics for that long.
static constexpr size_t kPostForkMaxHeapDurationMS = 2000;

#if defined(__LP64__) || !defined(ADDRESS_SANITIZER)
// 32 MB (0x2000000) is picked to ensure it is aligned to the largest supported PMD
// size, which is 32mb with a 16k page size on AArch64.
uint8_t* const Heap::kPreferredAllocSpaceBegin = reinterpret_cast<uint8_t*>(([]() constexpr {
  constexpr size_t kBegin = 32 * MB;
  constexpr int kMaxPMDSize = (kMaxPageSize / sizeof(uint64_t)) * kMaxPageSize;
  static_assert(IsAligned<kMaxPMDSize>(kBegin),
                "Moving-space's begin should be aligned to the maximum supported PMD size.");
  return kBegin;
})());
#else
#ifdef __ANDROID__
// For 32-bit Android, use 0x20000000 because asan reserves 0x04000000 - 0x20000000.
uint8_t* const Heap::kPreferredAllocSpaceBegin = reinterpret_cast<uint8_t*>(0x20000000);
#else
// For 32-bit host, use 0x40000000 because asan uses most of the space below this.
uint8_t* const Heap::kPreferredAllocSpaceBegin = reinterpret_cast<uint8_t*>(0x40000000);
#endif
#endif

// Log GC on regular (but fairly large) intervals during GC stress mode.
// It is expected that the other runtime options will be used to reduce the usual logging.
// This allows us to make the logging much less verbose while still reporting some
// progress (biased towards expensive GCs), and while still reporting pathological cases.
static constexpr int64_t kGcStressModeGcLogSampleFrequencyNs = MsToNs(10000);

static inline bool CareAboutPauseTimes() {
  return Runtime::Current()->InJankPerceptibleProcessState();
}

static void VerifyBootImagesContiguity(const std::vector<gc::space::ImageSpace*>& image_spaces) {
  uint32_t boot_image_size = 0u;
  for (size_t i = 0u, num_spaces = image_spaces.size(); i != num_spaces; ) {
    const ImageHeader& image_header = image_spaces[i]->GetImageHeader();
    uint32_t reservation_size = image_header.GetImageReservationSize();
    uint32_t image_count = image_header.GetImageSpaceCount();

    CHECK_NE(image_count, 0u);
    CHECK_LE(image_count, num_spaces - i);
    CHECK_NE(reservation_size, 0u);
    for (size_t j = 1u; j != image_count; ++j) {
      CHECK_EQ(image_spaces[i + j]->GetImageHeader().GetComponentCount(), 0u);
      CHECK_EQ(image_spaces[i + j]->GetImageHeader().GetImageReservationSize(), 0u);
    }

    // Check the start of the heap.
    CHECK_EQ(image_spaces[0]->Begin() + boot_image_size, image_spaces[i]->Begin());
    // Check contiguous layout of images and oat files.
    const uint8_t* current_heap = image_spaces[i]->Begin();
    const uint8_t* current_oat = image_spaces[i]->GetImageHeader().GetOatFileBegin();
    for (size_t j = 0u; j != image_count; ++j) {
      const ImageHeader& current_header = image_spaces[i + j]->GetImageHeader();
      CHECK_EQ(current_heap, image_spaces[i + j]->Begin());
      CHECK_EQ(current_oat, current_header.GetOatFileBegin());
      current_heap += RoundUp(current_header.GetImageSize(), kElfSegmentAlignment);
      CHECK_GT(current_header.GetOatFileEnd(), current_header.GetOatFileBegin());
      current_oat = current_header.GetOatFileEnd();
    }
    // Check that oat files start at the end of images.
    CHECK_EQ(current_heap, image_spaces[i]->GetImageHeader().GetOatFileBegin());
    // Check that the reservation size equals the size of images and oat files.
    CHECK_EQ(reservation_size, static_cast<size_t>(current_oat - image_spaces[i]->Begin()));

    boot_image_size += reservation_size;
    i += image_count;
  }
}

Heap::Heap(size_t initial_size,
           size_t growth_limit,
           size_t min_free,
           size_t max_free,
           double target_utilization,
           double foreground_heap_growth_multiplier,
           size_t stop_for_native_allocs,
           size_t capacity,
           size_t non_moving_space_capacity,
           const std::vector<std::string>& boot_class_path,
           const std::vector<std::string>& boot_class_path_locations,
           ArrayRef<File> boot_class_path_files,
           ArrayRef<File> boot_class_path_image_files,
           ArrayRef<File> boot_class_path_vdex_files,
           ArrayRef<File> boot_class_path_oat_files,
           const std::vector<std::string>& image_file_names,
           const InstructionSet image_instruction_set,
           CollectorType foreground_collector_type,
           CollectorType background_collector_type,
           space::LargeObjectSpaceType large_object_space_type,
           size_t large_object_threshold,
           size_t parallel_gc_threads,
           size_t conc_gc_threads,
           bool low_memory_mode,
           size_t long_pause_log_threshold,
           size_t long_gc_log_threshold,
           bool ignore_target_footprint,
           bool always_log_explicit_gcs,
           bool use_tlab,
           bool verify_pre_gc_heap,
           bool verify_pre_sweeping_heap,
           bool verify_post_gc_heap,
           bool verify_pre_gc_rosalloc,
           bool verify_pre_sweeping_rosalloc,
           bool verify_post_gc_rosalloc,
           bool gc_stress_mode,
           bool measure_gc_performance,
           bool use_homogeneous_space_compaction_for_oom,
           bool use_generational_gc,
           uint64_t min_interval_homogeneous_space_compaction_by_oom,
           bool dump_region_info_before_gc,
           bool dump_region_info_after_gc)
    : non_moving_space_(nullptr),
      rosalloc_space_(nullptr),
      dlmalloc_space_(nullptr),
      main_space_(nullptr),
      collector_type_(kCollectorTypeNone),
      foreground_collector_type_(foreground_collector_type),
      background_collector_type_(background_collector_type),
      desired_collector_type_(foreground_collector_type_),
      pending_task_lock_(nullptr),
      parallel_gc_threads_(parallel_gc_threads),
      conc_gc_threads_(conc_gc_threads),
      low_memory_mode_(low_memory_mode),
      long_pause_log_threshold_(long_pause_log_threshold),
      long_gc_log_threshold_(long_gc_log_threshold),
      process_cpu_start_time_ns_(ProcessCpuNanoTime()),
      pre_gc_last_process_cpu_time_ns_(process_cpu_start_time_ns_),
      post_gc_last_process_cpu_time_ns_(process_cpu_start_time_ns_),
      pre_gc_weighted_allocated_bytes_(0.0),
      post_gc_weighted_allocated_bytes_(0.0),
      ignore_target_footprint_(ignore_target_footprint),
      always_log_explicit_gcs_(always_log_explicit_gcs),
      zygote_creation_lock_("zygote creation lock", kZygoteCreationLock),
      zygote_space_(nullptr),
      large_object_threshold_(large_object_threshold),
      disable_thread_flip_count_(0),
      thread_flip_running_(false),
      collector_type_running_(kCollectorTypeNone),
      last_gc_cause_(kGcCauseNone),
      thread_running_gc_(nullptr),
      last_gc_type_(collector::kGcTypeNone),
      next_gc_type_(collector::kGcTypePartial),
      capacity_(capacity),
      growth_limit_(growth_limit),
      initial_heap_size_(initial_size),
      target_footprint_(initial_size),
      // Using kPostMonitorLock as a lock at kDefaultMutexLevel is acquired after
      // this one.
      process_state_update_lock_("process state update lock", kPostMonitorLock),
      min_foreground_target_footprint_(0),
      min_foreground_concurrent_start_bytes_(0),
      concurrent_start_bytes_(std::numeric_limits<size_t>::max()),
      total_bytes_freed_ever_(0),
      total_objects_freed_ever_(0),
      num_bytes_allocated_(0),
      native_bytes_registered_(0),
      old_native_bytes_allocated_(0),
      native_objects_notified_(0),
      num_bytes_freed_revoke_(0),
      num_bytes_alive_after_gc_(0),
      verify_missing_card_marks_(false),
      verify_system_weaks_(false),
      verify_pre_gc_heap_(verify_pre_gc_heap),
      verify_pre_sweeping_heap_(verify_pre_sweeping_heap),
      verify_post_gc_heap_(verify_post_gc_heap),
      verify_mod_union_table_(false),
      verify_pre_gc_rosalloc_(verify_pre_gc_rosalloc),
      verify_pre_sweeping_rosalloc_(verify_pre_sweeping_rosalloc),
      verify_post_gc_rosalloc_(verify_post_gc_rosalloc),
      gc_stress_mode_(gc_stress_mode),
      /* For GC a lot mode, we limit the allocation stacks to be kGcAlotInterval allocations. This
       * causes a lot of GC since we do a GC for alloc whenever the stack is full. When heap
       * verification is enabled, we limit the size of allocation stacks to speed up their
       * searching.
       */
      max_allocation_stack_size_(kGCALotMode ? kGcAlotAllocationStackSize
                                 : (kVerifyObjectSupport > kVerifyObjectModeFast)
                                     ? kVerifyObjectAllocationStackSize
                                     : kDefaultAllocationStackSize),
      current_allocator_(kAllocatorTypeDlMalloc),
      current_non_moving_allocator_(kAllocatorTypeNonMoving),
      bump_pointer_space_(nullptr),
      temp_space_(nullptr),
      region_space_(nullptr),
      min_free_(min_free),
      max_free_(max_free),
      target_utilization_(target_utilization),
      foreground_heap_growth_multiplier_(foreground_heap_growth_multiplier),
      stop_for_native_allocs_(stop_for_native_allocs),
      total_wait_time_(0),
      verify_object_mode_(kVerifyObjectModeDisabled),
      disable_moving_gc_count_(0),
      semi_space_collector_(nullptr),
      active_concurrent_copying_collector_(nullptr),
      young_concurrent_copying_collector_(nullptr),
      concurrent_copying_collector_(nullptr),
      is_running_on_memory_tool_(Runtime::Current()->IsRunningOnMemoryTool()),
      use_tlab_(use_tlab),
      main_space_backup_(nullptr),
      min_interval_homogeneous_space_compaction_by_oom_(
          min_interval_homogeneous_space_compaction_by_oom),
      last_time_homogeneous_space_compaction_by_oom_(NanoTime()),
      gcs_completed_(0u),
      max_gc_requested_(0u),
      pending_collector_transition_(nullptr),
      pending_heap_trim_(nullptr),
      use_homogeneous_space_compaction_for_oom_(use_homogeneous_space_compaction_for_oom),
      use_generational_gc_(use_generational_gc),
      running_collection_is_blocking_(false),
      blocking_gc_count_(0U),
      blocking_gc_time_(0U),
      last_update_time_gc_count_rate_histograms_(  // Round down by the window duration.
          (NanoTime() / kGcCountRateHistogramWindowDuration) * kGcCountRateHistogramWindowDuration),
      gc_count_last_window_(0U),
      blocking_gc_count_last_window_(0U),
      gc_count_rate_histogram_("gc count rate histogram", 1U, kGcCountRateMaxBucketCount),
      blocking_gc_count_rate_histogram_(
          "blocking gc count rate histogram", 1U, kGcCountRateMaxBucketCount),
      alloc_tracking_enabled_(false),
      alloc_record_depth_(AllocRecordObjectMap::kDefaultAllocStackDepth),
      backtrace_lock_(nullptr),
      seen_backtrace_count_(0u),
      unique_backtrace_count_(0u),
      gc_disabled_for_shutdown_(false),
      dump_region_info_before_gc_(dump_region_info_before_gc),
      dump_region_info_after_gc_(dump_region_info_after_gc),
      boot_image_spaces_(),
      boot_images_start_address_(0u),
      boot_images_size_(0u),
      pre_oome_gc_count_(0u) {
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() entering";
  }

  LOG(INFO) << "Using " << foreground_collector_type_ << " GC.";
  if (gUseUserfaultfd) {
    CHECK_EQ(foreground_collector_type_, kCollectorTypeCMC);
    CHECK_EQ(background_collector_type_, kCollectorTypeCMCBackground);
  } else {
    // This ensures that userfaultfd syscall is done before any seccomp filter is installed.
    // TODO(b/266731037): Remove this when we no longer need to collect metric on userfaultfd
    // support.
    auto [uffd_supported, minor_fault_supported] = collector::MarkCompact::GetUffdAndMinorFault();
    // The check is just to ensure that compiler doesn't eliminate the function call above.
    // Userfaultfd support is certain to be there if its minor-fault feature is supported.
    CHECK_IMPLIES(minor_fault_supported, uffd_supported);
  }

  if (gUseReadBarrier) {
    CHECK_EQ(foreground_collector_type_, kCollectorTypeCC);
    CHECK_EQ(background_collector_type_, kCollectorTypeCCBackground);
  } else if (background_collector_type_ != gc::kCollectorTypeHomogeneousSpaceCompact) {
    CHECK_EQ(IsMovingGc(foreground_collector_type_), IsMovingGc(background_collector_type_))
        << "Changing from " << foreground_collector_type_ << " to "
        << background_collector_type_ << " (or visa versa) is not supported.";
  }
  verification_.reset(new Verification(this));
  CHECK_GE(large_object_threshold, kMinLargeObjectThreshold);
  ScopedTrace trace(__FUNCTION__);
  Runtime* const runtime = Runtime::Current();
  // If we aren't the zygote, switch to the default non zygote allocator. This may update the
  // entrypoints.
  const bool is_zygote = runtime->IsZygote();
  if (!is_zygote) {
    // Background compaction is currently not supported for command line runs.
    if (background_collector_type_ != foreground_collector_type_) {
      VLOG(heap) << "Disabling background compaction for non zygote";
      background_collector_type_ = foreground_collector_type_;
    }
  }
  ChangeCollector(desired_collector_type_);
  live_bitmap_.reset(new accounting::HeapBitmap(this));
  mark_bitmap_.reset(new accounting::HeapBitmap(this));

  // We don't have hspace compaction enabled with CC.
  if (foreground_collector_type_ == kCollectorTypeCC
      || foreground_collector_type_ == kCollectorTypeCMC) {
    use_homogeneous_space_compaction_for_oom_ = false;
  }
  bool support_homogeneous_space_compaction =
      background_collector_type_ == gc::kCollectorTypeHomogeneousSpaceCompact ||
      use_homogeneous_space_compaction_for_oom_;
  // We may use the same space the main space for the non moving space if we don't need to compact
  // from the main space.
  // This is not the case if we support homogeneous compaction or have a moving background
  // collector type.
  bool separate_non_moving_space = is_zygote ||
      support_homogeneous_space_compaction || IsMovingGc(foreground_collector_type_) ||
      IsMovingGc(background_collector_type_);

  // Requested begin for the alloc space, to follow the mapped image and oat files
  uint8_t* request_begin = nullptr;
  // Calculate the extra space required after the boot image, see allocations below.
  size_t heap_reservation_size = 0u;
  if (separate_non_moving_space) {
    heap_reservation_size = non_moving_space_capacity;
  } else if (foreground_collector_type_ != kCollectorTypeCC && is_zygote) {
    heap_reservation_size = capacity_;
  }
  heap_reservation_size = RoundUp(heap_reservation_size, gPageSize);
  // Load image space(s).
  std::vector<std::unique_ptr<space::ImageSpace>> boot_image_spaces;
  MemMap heap_reservation;
  if (space::ImageSpace::LoadBootImage(boot_class_path,
                                       boot_class_path_locations,
                                       boot_class_path_files,
                                       boot_class_path_image_files,
                                       boot_class_path_vdex_files,
                                       boot_class_path_oat_files,
                                       image_file_names,
                                       image_instruction_set,
                                       runtime->ShouldRelocate(),
                                       /*executable=*/!runtime->IsAotCompiler(),
                                       heap_reservation_size,
                                       runtime->AllowInMemoryCompilation(),
                                       runtime->GetApexVersions(),
                                       &boot_image_spaces,
                                       &heap_reservation)) {
    DCHECK_EQ(heap_reservation_size, heap_reservation.IsValid() ? heap_reservation.Size() : 0u);
    DCHECK(!boot_image_spaces.empty());
    request_begin = boot_image_spaces.back()->GetImageHeader().GetOatFileEnd();
    DCHECK_IMPLIES(heap_reservation.IsValid(), request_begin == heap_reservation.Begin())
        << "request_begin=" << static_cast<const void*>(request_begin)
        << " heap_reservation.Begin()=" << static_cast<const void*>(heap_reservation.Begin());
    for (std::unique_ptr<space::ImageSpace>& space : boot_image_spaces) {
      boot_image_spaces_.push_back(space.get());
      AddSpace(space.release());
    }
    boot_images_start_address_ = PointerToLowMemUInt32(boot_image_spaces_.front()->Begin());
    uint32_t boot_images_end =
        PointerToLowMemUInt32(boot_image_spaces_.back()->GetImageHeader().GetOatFileEnd());
    boot_images_size_ = boot_images_end - boot_images_start_address_;
    if (kIsDebugBuild) {
      VerifyBootImagesContiguity(boot_image_spaces_);
    }
  } else {
    if (foreground_collector_type_ == kCollectorTypeCC) {
      // Need to use a low address so that we can allocate a contiguous 2 * Xmx space
      // when there's no image (dex2oat for target).
      request_begin = kPreferredAllocSpaceBegin;
    }
    // Gross hack to make dex2oat deterministic.
    if (foreground_collector_type_ == kCollectorTypeMS && Runtime::Current()->IsAotCompiler()) {
      // Currently only enabled for MS collector since that is what the deterministic dex2oat uses.
      // b/26849108
      request_begin = reinterpret_cast<uint8_t*>(kAllocSpaceBeginForDeterministicAoT);
    }
  }

  /*
  requested_alloc_space_begin ->     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-  nonmoving space (non_moving_space_capacity)+-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-????????????????????????????????????????????+-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-main alloc space / bump space 1 (capacity_) +-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-????????????????????????????????????????????+-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                                     +-main alloc space2 / bump space 2 (capacity_)+-
                                     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
  */

  MemMap main_mem_map_1;
  MemMap main_mem_map_2;

  std::string error_str;
  MemMap non_moving_space_mem_map;
  if (separate_non_moving_space) {
    ScopedTrace trace2("Create separate non moving space");
    // If we are the zygote, the non moving space becomes the zygote space when we run
    // PreZygoteFork the first time. In this case, call the map "zygote space" since we can't
    // rename the mem map later.
    const char* space_name = is_zygote ? kZygoteSpaceName : kNonMovingSpaceName;
    // Reserve the non moving mem map before the other two since it needs to be at a specific
    // address.
    DCHECK_EQ(heap_reservation.IsValid(), !boot_image_spaces_.empty());
    if (heap_reservation.IsValid()) {
      non_moving_space_mem_map = heap_reservation.RemapAtEnd(
          heap_reservation.Begin(), space_name, PROT_READ | PROT_WRITE, &error_str);
    } else {
      non_moving_space_mem_map = MapAnonymousPreferredAddress(
          space_name, request_begin, non_moving_space_capacity, &error_str);
    }
    CHECK(non_moving_space_mem_map.IsValid()) << error_str;
    DCHECK(!heap_reservation.IsValid());
    // Try to reserve virtual memory at a lower address if we have a separate non moving space.
    request_begin = non_moving_space_mem_map.Begin() == kPreferredAllocSpaceBegin
                        ? non_moving_space_mem_map.End()
                        : kPreferredAllocSpaceBegin;
  }
  // Attempt to create 2 mem maps at or after the requested begin.
  if (foreground_collector_type_ != kCollectorTypeCC) {
    ScopedTrace trace2("Create main mem map");
    if (separate_non_moving_space || !is_zygote) {
      main_mem_map_1 = MapAnonymousPreferredAddress(
          kMemMapSpaceName[0], request_begin, capacity_, &error_str);
    } else {
      // If no separate non-moving space and we are the zygote, the main space must come right after
      // the image space to avoid a gap. This is required since we want the zygote space to be
      // adjacent to the image space.
      DCHECK_EQ(heap_reservation.IsValid(), !boot_image_spaces_.empty());
      main_mem_map_1 = MemMap::MapAnonymous(
          kMemMapSpaceName[0],
          request_begin,
          capacity_,
          PROT_READ | PROT_WRITE,
          /* low_4gb= */ true,
          /* reuse= */ false,
          heap_reservation.IsValid() ? &heap_reservation : nullptr,
          &error_str);
    }
    CHECK(main_mem_map_1.IsValid()) << error_str;
    DCHECK(!heap_reservation.IsValid());
  }
  if (support_homogeneous_space_compaction ||
      background_collector_type_ == kCollectorTypeSS ||
      foreground_collector_type_ == kCollectorTypeSS) {
    ScopedTrace trace2("Create main mem map 2");
    main_mem_map_2 = MapAnonymousPreferredAddress(
        kMemMapSpaceName[1], main_mem_map_1.End(), capacity_, &error_str);
    CHECK(main_mem_map_2.IsValid()) << error_str;
  }

  // Create the non moving space first so that bitmaps don't take up the address range.
  if (separate_non_moving_space) {
    ScopedTrace trace2("Add non moving space");
    // Non moving space is always dlmalloc since we currently don't have support for multiple
    // active rosalloc spaces.
    const size_t size = non_moving_space_mem_map.Size();
    const void* non_moving_space_mem_map_begin = non_moving_space_mem_map.Begin();
    non_moving_space_ = space::DlMallocSpace::CreateFromMemMap(std::move(non_moving_space_mem_map),
                                                               "zygote / non moving space",
                                                               GetDefaultStartingSize(),
                                                               initial_size,
                                                               size,
                                                               size,
                                                               /* can_move_objects= */ false);
    CHECK(non_moving_space_ != nullptr) << "Failed creating non moving space "
        << non_moving_space_mem_map_begin;
    non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
    AddSpace(non_moving_space_);
  }
  // Create other spaces based on whether or not we have a moving GC.
  if (foreground_collector_type_ == kCollectorTypeCC) {
    CHECK(separate_non_moving_space);
    // Reserve twice the capacity, to allow evacuating every region for explicit GCs.
    MemMap region_space_mem_map =
        space::RegionSpace::CreateMemMap(kRegionSpaceName, capacity_ * 2, request_begin);
    CHECK(region_space_mem_map.IsValid()) << "No region space mem map";
    region_space_ = space::RegionSpace::Create(
        kRegionSpaceName, std::move(region_space_mem_map), use_generational_gc_);
    AddSpace(region_space_);
  } else if (IsMovingGc(foreground_collector_type_)) {
    // Create bump pointer spaces.
    // We only to create the bump pointer if the foreground collector is a compacting GC.
    // TODO: Place bump-pointer spaces somewhere to minimize size of card table.
    bump_pointer_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 1",
                                                                    std::move(main_mem_map_1));
    CHECK(bump_pointer_space_ != nullptr) << "Failed to create bump pointer space";
    AddSpace(bump_pointer_space_);
    // For Concurrent Mark-compact GC we don't need the temp space to be in
    // lower 4GB. So its temp space will be created by the GC itself.
    if (foreground_collector_type_ != kCollectorTypeCMC) {
      temp_space_ = space::BumpPointerSpace::CreateFromMemMap("Bump pointer space 2",
                                                              std::move(main_mem_map_2));
      CHECK(temp_space_ != nullptr) << "Failed to create bump pointer space";
      AddSpace(temp_space_);
    }
    CHECK(separate_non_moving_space);
  } else {
    CreateMainMallocSpace(std::move(main_mem_map_1), initial_size, growth_limit_, capacity_);
    CHECK(main_space_ != nullptr);
    AddSpace(main_space_);
    if (!separate_non_moving_space) {
      non_moving_space_ = main_space_;
      CHECK(!non_moving_space_->CanMoveObjects());
    }
    if (main_mem_map_2.IsValid()) {
      const char* name = kUseRosAlloc ? kRosAllocSpaceName[1] : kDlMallocSpaceName[1];
      main_space_backup_.reset(CreateMallocSpaceFromMemMap(std::move(main_mem_map_2),
                                                           initial_size,
                                                           growth_limit_,
                                                           capacity_,
                                                           name,
                                                           /* can_move_objects= */ true));
      CHECK(main_space_backup_.get() != nullptr);
      // Add the space so its accounted for in the heap_begin and heap_end.
      AddSpace(main_space_backup_.get());
    }
  }
  CHECK(non_moving_space_ != nullptr);
  CHECK(!non_moving_space_->CanMoveObjects());
  // Allocate the large object space.
  if (large_object_space_type == space::LargeObjectSpaceType::kFreeList) {
    large_object_space_ = space::FreeListSpace::Create("free list large object space", capacity_);
    CHECK(large_object_space_ != nullptr) << "Failed to create large object space";
  } else if (large_object_space_type == space::LargeObjectSpaceType::kMap) {
    large_object_space_ = space::LargeObjectMapSpace::Create("mem map large object space");
    CHECK(large_object_space_ != nullptr) << "Failed to create large object space";
  } else {
    // Disable the large object space by making the cutoff excessively large.
    large_object_threshold_ = std::numeric_limits<size_t>::max();
    large_object_space_ = nullptr;
  }
  if (large_object_space_ != nullptr) {
    AddSpace(large_object_space_);
  }
  // Compute heap capacity. Continuous spaces are sorted in order of Begin().
  CHECK(!continuous_spaces_.empty());
  // Relies on the spaces being sorted.
  uint8_t* heap_begin = continuous_spaces_.front()->Begin();
  uint8_t* heap_end = continuous_spaces_.back()->Limit();
  size_t heap_capacity = heap_end - heap_begin;
  // Remove the main backup space since it slows down the GC to have unused extra spaces.
  // TODO: Avoid needing to do this.
  if (main_space_backup_.get() != nullptr) {
    RemoveSpace(main_space_backup_.get());
  }
  // Allocate the card table.
  // We currently don't support dynamically resizing the card table.
  // Since we don't know where in the low_4gb the app image will be located, make the card table
  // cover the whole low_4gb. TODO: Extend the card table in AddSpace.
  UNUSED(heap_capacity);
  // Start at 4 KB, we can be sure there are no spaces mapped this low since the address range is
  // reserved by the kernel.
  static constexpr size_t kMinHeapAddress = 4 * KB;
  card_table_.reset(accounting::CardTable::Create(reinterpret_cast<uint8_t*>(kMinHeapAddress),
                                                  4 * GB - kMinHeapAddress));
  CHECK(card_table_.get() != nullptr) << "Failed to create card table";
  if (foreground_collector_type_ == kCollectorTypeCC && kUseTableLookupReadBarrier) {
    rb_table_.reset(new accounting::ReadBarrierTable());
    DCHECK(rb_table_->IsAllCleared());
  }
  if (HasBootImageSpace()) {
    // Don't add the image mod union table if we are running without an image, this can crash if
    // we use the CardCache implementation.
    for (space::ImageSpace* image_space : GetBootImageSpaces()) {
      accounting::ModUnionTable* mod_union_table = new accounting::ModUnionTableToZygoteAllocspace(
          "Image mod-union table", this, image_space);
      CHECK(mod_union_table != nullptr) << "Failed to create image mod-union table";
      AddModUnionTable(mod_union_table);
    }
  }
  if (collector::SemiSpace::kUseRememberedSet && non_moving_space_ != main_space_) {
    accounting::RememberedSet* non_moving_space_rem_set =
        new accounting::RememberedSet("Non-moving space remembered set", this, non_moving_space_);
    CHECK(non_moving_space_rem_set != nullptr) << "Failed to create non-moving space remembered set";
    AddRememberedSet(non_moving_space_rem_set);
  }
  // TODO: Count objects in the image space here?
  num_bytes_allocated_.store(0, std::memory_order_relaxed);
  mark_stack_.reset(accounting::ObjectStack::Create("mark stack", kDefaultMarkStackSize,
                                                    kDefaultMarkStackSize));
  const size_t alloc_stack_capacity = max_allocation_stack_size_ + kAllocationStackReserveSize;
  allocation_stack_.reset(accounting::ObjectStack::Create(
      "allocation stack", max_allocation_stack_size_, alloc_stack_capacity));
  live_stack_.reset(accounting::ObjectStack::Create(
      "live stack", max_allocation_stack_size_, alloc_stack_capacity));
  // It's still too early to take a lock because there are no threads yet, but we can create locks
  // now. We don't create it earlier to make it clear that you can't use locks during heap
  // initialization.
  gc_complete_lock_ = new Mutex("GC complete lock");
  gc_complete_cond_.reset(new ConditionVariable("GC complete condition variable",
                                                *gc_complete_lock_));

  thread_flip_lock_ = new Mutex("GC thread flip lock");
  thread_flip_cond_.reset(new ConditionVariable("GC thread flip condition variable",
                                                *thread_flip_lock_));
  task_processor_.reset(new TaskProcessor());
  reference_processor_.reset(new ReferenceProcessor());
  pending_task_lock_ = new Mutex("Pending task lock");
  if (ignore_target_footprint_) {
    SetIdealFootprint(std::numeric_limits<size_t>::max());
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  }
  CHECK_NE(target_footprint_.load(std::memory_order_relaxed), 0U);
  CreateGarbageCollectors(measure_gc_performance);
  if (!GetBootImageSpaces().empty() && non_moving_space_ != nullptr &&
      (is_zygote || separate_non_moving_space)) {
    // Check that there's no gap between the image space and the non moving space so that the
    // immune region won't break (eg. due to a large object allocated in the gap). This is only
    // required when we're the zygote.
    // Space with smallest Begin().
    space::ImageSpace* first_space = nullptr;
    for (space::ImageSpace* space : boot_image_spaces_) {
      if (first_space == nullptr || space->Begin() < first_space->Begin()) {
        first_space = space;
      }
    }
    bool no_gap = MemMap::CheckNoGaps(*first_space->GetMemMap(), *non_moving_space_->GetMemMap());
    if (!no_gap) {
      PrintFileToLog("/proc/self/maps", LogSeverity::ERROR);
      MemMap::DumpMaps(LOG_STREAM(ERROR), /* terse= */ true);
      LOG(FATAL) << "There's a gap between the image space and the non-moving space";
    }
  }
  // Perfetto Java Heap Profiler Support.
  if (runtime->IsPerfettoJavaHeapStackProfEnabled()) {
    // Perfetto Plugin is loaded and enabled, initialize the Java Heap Profiler.
    InitPerfettoJavaHeapProf();
  } else {
    // Disable the Java Heap Profiler.
    GetHeapSampler().DisableHeapSampler();
  }

  instrumentation::Instrumentation* const instrumentation = runtime->GetInstrumentation();
  if (gc_stress_mode_) {
    backtrace_lock_ = new Mutex("GC complete lock");
  }
  if (is_running_on_memory_tool_ || gc_stress_mode_) {
    instrumentation->InstrumentQuickAllocEntryPoints();
  }
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "Heap() exiting";
  }
}

void Heap::CreateGarbageCollectors(bool measure_gc_performance) {
  for (size_t i = 0; i < 2; ++i) {
    const bool concurrent = (i != 0);
    if ((MayUseCollector(kCollectorTypeCMS) && concurrent) ||
        (MayUseCollector(kCollectorTypeMS) && !concurrent)) {
      garbage_collectors_.push_back(new collector::MarkSweep(this, concurrent));
      garbage_collectors_.push_back(new collector::PartialMarkSweep(this, concurrent));
      garbage_collectors_.push_back(new collector::StickyMarkSweep(this, concurrent));
    }
  }
  if (kMovingCollector) {
    if (MayUseCollector(kCollectorTypeSS) ||
        MayUseCollector(kCollectorTypeHomogeneousSpaceCompact) ||
        use_homogeneous_space_compaction_for_oom_) {
      semi_space_collector_ = new collector::SemiSpace(this);
      garbage_collectors_.push_back(semi_space_collector_);
    }
    if (MayUseCollector(kCollectorTypeCMC)) {
      mark_compact_ = new collector::MarkCompact(this);
      garbage_collectors_.push_back(mark_compact_);
      if (use_generational_gc_) {
        young_mark_compact_ = new collector::YoungMarkCompact(this, mark_compact_);
        garbage_collectors_.push_back(young_mark_compact_);
      }
    }
    if (MayUseCollector(kCollectorTypeCC)) {
      concurrent_copying_collector_ = new collector::ConcurrentCopying(this,
                                                                       /*young_gen=*/false,
                                                                       use_generational_gc_,
                                                                       "",
                                                                       measure_gc_performance);
      if (use_generational_gc_) {
        young_concurrent_copying_collector_ =
            new collector::ConcurrentCopying(this,
                                             /*young_gen=*/true,
                                             use_generational_gc_,
                                             "young",
                                             measure_gc_performance);
      }
      active_concurrent_copying_collector_.store(concurrent_copying_collector_,
                                                 std::memory_order_relaxed);
      DCHECK(region_space_ != nullptr);
      concurrent_copying_collector_->SetRegionSpace(region_space_);
      if (use_generational_gc_) {
        young_concurrent_copying_collector_->SetRegionSpace(region_space_);
        // At this point, non-moving space should be created.
        DCHECK(non_moving_space_ != nullptr);
        concurrent_copying_collector_->CreateInterRegionRefBitmaps();
      }
      garbage_collectors_.push_back(concurrent_copying_collector_);
      if (use_generational_gc_) {
        garbage_collectors_.push_back(young_concurrent_copying_collector_);
      }
    }
  }
}

MemMap Heap::MapAnonymousPreferredAddress(const char* name,
                                          uint8_t* request_begin,
                                          size_t capacity,
                                          std::string* out_error_str) {
  while (true) {
    MemMap map = MemMap::MapAnonymous(name,
                                      request_begin,
                                      capacity,
                                      PROT_READ | PROT_WRITE,
                                      /*low_4gb=*/ true,
                                      /*reuse=*/ false,
                                      /*reservation=*/ nullptr,
                                      out_error_str);
    if (map.IsValid() || request_begin == nullptr) {
      return map;
    }
    // Retry a  second time with no specified request begin.
    request_begin = nullptr;
  }
}

bool Heap::MayUseCollector(CollectorType type) const {
  return foreground_collector_type_ == type || background_collector_type_ == type;
}

space::MallocSpace* Heap::CreateMallocSpaceFromMemMap(MemMap&& mem_map,
                                                      size_t initial_size,
                                                      size_t growth_limit,
                                                      size_t capacity,
                                                      const char* name,
                                                      bool can_move_objects) {
  space::MallocSpace* malloc_space = nullptr;
  if (kUseRosAlloc) {
    // Create rosalloc space.
    malloc_space = space::RosAllocSpace::CreateFromMemMap(std::move(mem_map),
                                                          name,
                                                          GetDefaultStartingSize(),
                                                          initial_size,
                                                          growth_limit,
                                                          capacity,
                                                          low_memory_mode_,
                                                          can_move_objects);
  } else {
    malloc_space = space::DlMallocSpace::CreateFromMemMap(std::move(mem_map),
                                                          name,
                                                          GetDefaultStartingSize(),
                                                          initial_size,
                                                          growth_limit,
                                                          capacity,
                                                          can_move_objects);
  }
  if (collector::SemiSpace::kUseRememberedSet) {
    accounting::RememberedSet* rem_set  =
        new accounting::RememberedSet(std::string(name) + " remembered set", this, malloc_space);
    CHECK(rem_set != nullptr) << "Failed to create main space remembered set";
    AddRememberedSet(rem_set);
  }
  CHECK(malloc_space != nullptr) << "Failed to create " << name;
  malloc_space->SetFootprintLimit(malloc_space->Capacity());
  return malloc_space;
}

void Heap::CreateMainMallocSpace(MemMap&& mem_map,
                                 size_t initial_size,
                                 size_t growth_limit,
                                 size_t capacity) {
  // Is background compaction is enabled?
  bool can_move_objects = IsMovingGc(background_collector_type_) !=
      IsMovingGc(foreground_collector_type_) || use_homogeneous_space_compaction_for_oom_;
  // If we are the zygote and don't yet have a zygote space, it means that the zygote fork will
  // happen in the future. If this happens and we have kCompactZygote enabled we wish to compact
  // from the main space to the zygote space. If background compaction is enabled, always pass in
  // that we can move objets.
  if (kCompactZygote && Runtime::Current()->IsZygote() && !can_move_objects) {
    // After the zygote we want this to be false if we don't have background compaction enabled so
    // that getting primitive array elements is faster.
    can_move_objects = !HasZygoteSpace();
  }
  if (collector::SemiSpace::kUseRememberedSet && main_space_ != nullptr) {
    RemoveRememberedSet(main_space_);
  }
  const char* name = kUseRosAlloc ? kRosAllocSpaceName[0] : kDlMallocSpaceName[0];
  main_space_ = CreateMallocSpaceFromMemMap(std::move(mem_map),
                                            initial_size,
                                            growth_limit,
                                            capacity, name,
                                            can_move_objects);
  SetSpaceAsDefault(main_space_);
  VLOG(heap) << "Created main space " << main_space_;
}

void Heap::ChangeAllocator(AllocatorType allocator) {
  if (current_allocator_ != allocator) {
    // These two allocators are only used internally and don't have any entrypoints.
    CHECK_NE(allocator, kAllocatorTypeLOS);
    CHECK_NE(allocator, kAllocatorTypeNonMoving);
    current_allocator_ = allocator;
    MutexLock mu(nullptr, *Locks::runtime_shutdown_lock_);
    SetQuickAllocEntryPointsAllocator(current_allocator_);
    Runtime::Current()->GetInstrumentation()->ResetQuickAllocEntryPoints();
  }
}

bool Heap::IsCompilingBoot() const {
  if (!Runtime::Current()->IsAotCompiler()) {
    return false;
  }
  ScopedObjectAccess soa(Thread::Current());
  for (const auto& space : continuous_spaces_) {
    if (space->IsImageSpace() || space->IsZygoteSpace()) {
      return false;
    }
  }
  return true;
}

void Heap::IncrementDisableMovingGC(Thread* self) {
  // Need to do this holding the lock to prevent races where the GC is about to run / running when
  // we attempt to disable it.
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  ++disable_moving_gc_count_;
  if (IsMovingGc(collector_type_running_)) {
    WaitForGcToCompleteLocked(kGcCauseDisableMovingGc, self);
  }
}

void Heap::DecrementDisableMovingGC(Thread* self) {
  MutexLock mu(self, *gc_complete_lock_);
  CHECK_GT(disable_moving_gc_count_, 0U);
  --disable_moving_gc_count_;
}

void Heap::IncrementDisableThreadFlip(Thread* self) {
  // Supposed to be called by mutators. If thread_flip_running_ is true, block. Otherwise, go ahead.
  bool is_nested = self->GetDisableThreadFlipCount() > 0;
  self->IncrementDisableThreadFlipCount();
  if (is_nested) {
    // If this is a nested JNI critical section enter, we don't need to wait or increment the global
    // counter. The global counter is incremented only once for a thread for the outermost enter.
    return;
  }
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcThreadFlip);
  MutexLock mu(self, *thread_flip_lock_);
  thread_flip_cond_->CheckSafeToWait(self);
  bool has_waited = false;
  uint64_t wait_start = 0;
  if (thread_flip_running_) {
    wait_start = NanoTime();
    ScopedTrace trace("IncrementDisableThreadFlip");
    while (thread_flip_running_) {
      has_waited = true;
      thread_flip_cond_->Wait(self);
    }
  }
  ++disable_thread_flip_count_;
  if (has_waited) {
    uint64_t wait_time = NanoTime() - wait_start;
    total_wait_time_ += wait_time;
    if (wait_time > long_pause_log_threshold_) {
      LOG(INFO) << __FUNCTION__ << " blocked for " << PrettyDuration(wait_time);
    }
  }
}

void Heap::EnsureObjectUserfaulted(ObjPtr<mirror::Object> obj) {
  if (gUseUserfaultfd) {
    // Use volatile to ensure that compiler loads from memory to trigger userfaults, if required.
    const uint8_t* start = reinterpret_cast<uint8_t*>(obj.Ptr());
    const uint8_t* end = AlignUp(start + obj->SizeOf(), gPageSize);
    // The first page is already touched by SizeOf().
    start += gPageSize;
    while (start < end) {
      ForceRead(start);
      start += gPageSize;
    }
  }
}

void Heap::DecrementDisableThreadFlip(Thread* self) {
  // Supposed to be called by mutators. Decrement disable_thread_flip_count_ and potentially wake up
  // the GC waiting before doing a thread flip.
  self->DecrementDisableThreadFlipCount();
  bool is_outermost = self->GetDisableThreadFlipCount() == 0;
  if (!is_outermost) {
    // If this is not an outermost JNI critical exit, we don't need to decrement the global counter.
    // The global counter is decremented only once for a thread for the outermost exit.
    return;
  }
  MutexLock mu(self, *thread_flip_lock_);
  CHECK_GT(disable_thread_flip_count_, 0U);
  --disable_thread_flip_count_;
  if (disable_thread_flip_count_ == 0) {
    // Potentially notify the GC thread blocking to begin a thread flip.
    thread_flip_cond_->Broadcast(self);
  }
}

void Heap::ThreadFlipBegin(Thread* self) {
  // Supposed to be called by GC. Set thread_flip_running_ to be true. If disable_thread_flip_count_
  // > 0, block. Otherwise, go ahead.
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcThreadFlip);
  MutexLock mu(self, *thread_flip_lock_);
  thread_flip_cond_->CheckSafeToWait(self);
  bool has_waited = false;
  uint64_t wait_start = NanoTime();
  CHECK(!thread_flip_running_);
  // Set this to true before waiting so that frequent JNI critical enter/exits won't starve
  // GC. This like a writer preference of a reader-writer lock.
  thread_flip_running_ = true;
  while (disable_thread_flip_count_ > 0) {
    has_waited = true;
    thread_flip_cond_->Wait(self);
  }
  if (has_waited) {
    uint64_t wait_time = NanoTime() - wait_start;
    total_wait_time_ += wait_time;
    if (wait_time > long_pause_log_threshold_) {
      LOG(INFO) << __FUNCTION__ << " blocked for " << PrettyDuration(wait_time);
    }
  }
}

void Heap::ThreadFlipEnd(Thread* self) {
  // Supposed to be called by GC. Set thread_flip_running_ to false and potentially wake up mutators
  // waiting before doing a JNI critical.
  MutexLock mu(self, *thread_flip_lock_);
  CHECK(thread_flip_running_);
  thread_flip_running_ = false;
  // Potentially notify mutator threads blocking to enter a JNI critical section.
  thread_flip_cond_->Broadcast(self);
}

void Heap::GrowHeapOnJankPerceptibleSwitch() {
  MutexLock mu(Thread::Current(), process_state_update_lock_);
  size_t orig_target_footprint = target_footprint_.load(std::memory_order_relaxed);
  if (orig_target_footprint < min_foreground_target_footprint_) {
    target_footprint_.compare_exchange_strong(orig_target_footprint,
                                              min_foreground_target_footprint_,
                                              std::memory_order_relaxed);
  }
  if (IsGcConcurrent() && concurrent_start_bytes_ < min_foreground_concurrent_start_bytes_) {
    concurrent_start_bytes_ = min_foreground_concurrent_start_bytes_;
  }
}

void Heap::UpdateProcessState(ProcessState old_process_state, ProcessState new_process_state) {
  if (old_process_state != new_process_state) {
    const bool jank_perceptible = new_process_state == kProcessStateJankPerceptible;
    if (jank_perceptible) {
      // Transition back to foreground right away to prevent jank.
      RequestCollectorTransition(foreground_collector_type_, 0);
      GrowHeapOnJankPerceptibleSwitch();
    } else {
      // If background_collector_type_ is kCollectorTypeHomogeneousSpaceCompact then we have
      // special handling which does a homogenous space compaction once but then doesn't transition
      // the collector. Similarly, we invoke a full compaction for kCollectorTypeCC but don't
      // transition the collector.
      RequestCollectorTransition(background_collector_type_, 0);
    }
  }
}

void Heap::CreateThreadPool(size_t num_threads) {
  if (num_threads == 0) {
    num_threads = std::max(parallel_gc_threads_, conc_gc_threads_);
  }
  if (num_threads != 0) {
    thread_pool_.reset(ThreadPool::Create("Heap thread pool", num_threads));
  }
}

void Heap::WaitForWorkersToBeCreated() {
  DCHECK(!Runtime::Current()->IsShuttingDown(Thread::Current()))
      << "Cannot create new threads during runtime shutdown";
  if (thread_pool_ != nullptr) {
    thread_pool_->WaitForWorkersToBeCreated();
  }
}

void Heap::MarkAllocStackAsLive(accounting::ObjectStack* stack) {
  space::ContinuousSpace* space1 = main_space_ != nullptr ? main_space_ : non_moving_space_;
  space::ContinuousSpace* space2 = non_moving_space_;
  // TODO: Generalize this to n bitmaps?
  CHECK(space1 != nullptr);
  CHECK(space2 != nullptr);
  MarkAllocStack(space1->GetLiveBitmap(), space2->GetLiveBitmap(),
                 (large_object_space_ != nullptr ? large_object_space_->GetLiveBitmap() : nullptr),
                 stack);
}

void Heap::DeleteThreadPool() {
  thread_pool_.reset(nullptr);
}

void Heap::AddSpace(space::Space* space) {
  CHECK(space != nullptr);
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (space->IsContinuousSpace()) {
    DCHECK(!space->IsDiscontinuousSpace());
    space::ContinuousSpace* continuous_space = space->AsContinuousSpace();
    // Continuous spaces don't necessarily have bitmaps.
    accounting::ContinuousSpaceBitmap* live_bitmap = continuous_space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = continuous_space->GetMarkBitmap();
    // The region space bitmap is not added since VisitObjects visits the region space objects with
    // special handling.
    if (live_bitmap != nullptr && !space->IsRegionSpace()) {
      CHECK(mark_bitmap != nullptr);
      live_bitmap_->AddContinuousSpaceBitmap(live_bitmap);
      mark_bitmap_->AddContinuousSpaceBitmap(mark_bitmap);
    }
    continuous_spaces_.push_back(continuous_space);
    // Ensure that spaces remain sorted in increasing order of start address.
    std::sort(continuous_spaces_.begin(), continuous_spaces_.end(),
              [](const space::ContinuousSpace* a, const space::ContinuousSpace* b) {
      return a->Begin() < b->Begin();
    });
  } else {
    CHECK(space->IsDiscontinuousSpace());
    space::DiscontinuousSpace* discontinuous_space = space->AsDiscontinuousSpace();
    live_bitmap_->AddLargeObjectBitmap(discontinuous_space->GetLiveBitmap());
    mark_bitmap_->AddLargeObjectBitmap(discontinuous_space->GetMarkBitmap());
    discontinuous_spaces_.push_back(discontinuous_space);
  }
  if (space->IsAllocSpace()) {
    alloc_spaces_.push_back(space->AsAllocSpace());
  }
}

void Heap::SetSpaceAsDefault(space::ContinuousSpace* continuous_space) {
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (continuous_space->IsDlMallocSpace()) {
    dlmalloc_space_ = continuous_space->AsDlMallocSpace();
  } else if (continuous_space->IsRosAllocSpace()) {
    rosalloc_space_ = continuous_space->AsRosAllocSpace();
  }
}

void Heap::RemoveSpace(space::Space* space) {
  DCHECK(space != nullptr);
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  if (space->IsContinuousSpace()) {
    DCHECK(!space->IsDiscontinuousSpace());
    space::ContinuousSpace* continuous_space = space->AsContinuousSpace();
    // Continuous spaces don't necessarily have bitmaps.
    accounting::ContinuousSpaceBitmap* live_bitmap = continuous_space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = continuous_space->GetMarkBitmap();
    if (live_bitmap != nullptr && !space->IsRegionSpace()) {
      DCHECK(mark_bitmap != nullptr);
      live_bitmap_->RemoveContinuousSpaceBitmap(live_bitmap);
      mark_bitmap_->RemoveContinuousSpaceBitmap(mark_bitmap);
    }
    auto it = std::find(continuous_spaces_.begin(), continuous_spaces_.end(), continuous_space);
    DCHECK(it != continuous_spaces_.end());
    continuous_spaces_.erase(it);
  } else {
    DCHECK(space->IsDiscontinuousSpace());
    space::DiscontinuousSpace* discontinuous_space = space->AsDiscontinuousSpace();
    live_bitmap_->RemoveLargeObjectBitmap(discontinuous_space->GetLiveBitmap());
    mark_bitmap_->RemoveLargeObjectBitmap(discontinuous_space->GetMarkBitmap());
    auto it = std::find(discontinuous_spaces_.begin(), discontinuous_spaces_.end(),
                        discontinuous_space);
    DCHECK(it != discontinuous_spaces_.end());
    discontinuous_spaces_.erase(it);
  }
  if (space->IsAllocSpace()) {
    auto it = std::find(alloc_spaces_.begin(), alloc_spaces_.end(), space->AsAllocSpace());
    DCHECK(it != alloc_spaces_.end());
    alloc_spaces_.erase(it);
  }
}

double Heap::CalculateGcWeightedAllocatedBytes(uint64_t gc_last_process_cpu_time_ns,
                                               uint64_t current_process_cpu_time) const {
  uint64_t bytes_allocated = GetBytesAllocated();
  double weight = current_process_cpu_time - gc_last_process_cpu_time_ns;
  return weight * bytes_allocated;
}

void Heap::CalculatePreGcWeightedAllocatedBytes() {
  uint64_t current_process_cpu_time = ProcessCpuNanoTime();
  pre_gc_weighted_allocated_bytes_ +=
    CalculateGcWeightedAllocatedBytes(pre_gc_last_process_cpu_time_ns_, current_process_cpu_time);
  pre_gc_last_process_cpu_time_ns_ = current_process_cpu_time;
}

void Heap::CalculatePostGcWeightedAllocatedBytes() {
  uint64_t current_process_cpu_time = ProcessCpuNanoTime();
  post_gc_weighted_allocated_bytes_ +=
    CalculateGcWeightedAllocatedBytes(post_gc_last_process_cpu_time_ns_, current_process_cpu_time);
  post_gc_last_process_cpu_time_ns_ = current_process_cpu_time;
}

uint64_t Heap::GetTotalGcCpuTime() {
  uint64_t sum = 0;
  for (auto* collector : garbage_collectors_) {
    sum += collector->GetTotalCpuTime();
  }
  return sum;
}

void Heap::DumpGcPerformanceInfo(std::ostream& os) {
  // Dump cumulative timings.
  os << "Dumping cumulative Gc timings\n";
  uint64_t total_duration = 0;
  // Dump cumulative loggers for each GC type.
  uint64_t total_paused_time = 0;
  for (auto* collector : garbage_collectors_) {
    total_duration += collector->GetCumulativeTimings().GetTotalNs();
    total_paused_time += collector->GetTotalPausedTimeNs();
    collector->DumpPerformanceInfo(os);
  }
  if (total_duration != 0) {
    const double total_seconds = total_duration / 1.0e9;
    const double total_cpu_seconds = GetTotalGcCpuTime() / 1.0e9;
    os << "Total time spent in GC: " << PrettyDuration(total_duration) << "\n";
    os << "Mean GC size throughput: "
       << PrettySize(GetBytesFreedEver() / total_seconds) << "/s"
       << " per cpu-time: "
       << PrettySize(GetBytesFreedEver() / total_cpu_seconds) << "/s\n";
  }
  os << "Total bytes allocated " << PrettySize(GetBytesAllocatedEver()) << "\n";
  os << "Total bytes freed " << PrettySize(GetBytesFreedEver()) << "\n";
  os << "Free memory " << PrettySize(GetFreeMemory()) << "\n";
  os << "Free memory until GC " << PrettySize(GetFreeMemoryUntilGC()) << "\n";
  os << "Free memory until OOME " << PrettySize(GetFreeMemoryUntilOOME()) << "\n";
  os << "Total memory " << PrettySize(GetTotalMemory()) << "\n";
  os << "Max memory " << PrettySize(GetMaxMemory()) << "\n";
  if (HasZygoteSpace()) {
    os << "Zygote space size " << PrettySize(zygote_space_->Size()) << "\n";
  }
  os << "Total mutator paused time: " << PrettyDuration(total_paused_time) << "\n";
  os << "Total time waiting for GC to complete: " << PrettyDuration(total_wait_time_) << "\n";
  os << "Total GC count: " << GetGcCount() << "\n";
  os << "Total GC time: " << PrettyDuration(GetGcTime()) << "\n";
  os << "Total blocking GC count: " << GetBlockingGcCount() << "\n";
  os << "Total blocking GC time: " << PrettyDuration(GetBlockingGcTime()) << "\n";
  os << "Total pre-OOME GC count: " << GetPreOomeGcCount() << "\n";
  {
    MutexLock mu(Thread::Current(), *gc_complete_lock_);
    if (gc_count_rate_histogram_.SampleSize() > 0U) {
      os << "Histogram of GC count per " << NsToMs(kGcCountRateHistogramWindowDuration) << " ms: ";
      gc_count_rate_histogram_.DumpBins(os);
      os << "\n";
    }
    if (blocking_gc_count_rate_histogram_.SampleSize() > 0U) {
      os << "Histogram of blocking GC count per "
         << NsToMs(kGcCountRateHistogramWindowDuration) << " ms: ";
      blocking_gc_count_rate_histogram_.DumpBins(os);
      os << "\n";
    }
  }

  if (kDumpRosAllocStatsOnSigQuit && rosalloc_space_ != nullptr) {
    rosalloc_space_->DumpStats(os);
  }

  os << "Native bytes total: " << GetNativeBytes()
     << " registered: " << native_bytes_registered_.load(std::memory_order_relaxed) << "\n";

  os << "Total native bytes at last GC: "
     << old_native_bytes_allocated_.load(std::memory_order_relaxed) << "\n";

  BaseMutex::DumpAll(os);
}

void Heap::ResetGcPerformanceInfo() {
  for (auto* collector : garbage_collectors_) {
    collector->ResetMeasurements();
  }

  process_cpu_start_time_ns_ = ProcessCpuNanoTime();

  pre_gc_last_process_cpu_time_ns_ = process_cpu_start_time_ns_;
  pre_gc_weighted_allocated_bytes_ = 0u;

  post_gc_last_process_cpu_time_ns_ = process_cpu_start_time_ns_;
  post_gc_weighted_allocated_bytes_ = 0u;

  total_bytes_freed_ever_.store(0);
  total_objects_freed_ever_.store(0);
  total_wait_time_ = 0;
  blocking_gc_count_ = 0;
  blocking_gc_time_ = 0;
  pre_oome_gc_count_.store(0, std::memory_order_relaxed);
  gc_count_last_window_ = 0;
  blocking_gc_count_last_window_ = 0;
  last_update_time_gc_count_rate_histograms_ =  // Round down by the window duration.
      (NanoTime() / kGcCountRateHistogramWindowDuration) * kGcCountRateHistogramWindowDuration;
  {
    MutexLock mu(Thread::Current(), *gc_complete_lock_);
    gc_count_rate_histogram_.Reset();
    blocking_gc_count_rate_histogram_.Reset();
  }
}

uint64_t Heap::GetGcCount() const {
  uint64_t gc_count = 0U;
  for (auto* collector : garbage_collectors_) {
    gc_count += collector->GetCumulativeTimings().GetIterations();
  }
  return gc_count;
}

uint64_t Heap::GetGcTime() const {
  uint64_t gc_time = 0U;
  for (auto* collector : garbage_collectors_) {
    gc_time += collector->GetCumulativeTimings().GetTotalNs();
  }
  return gc_time;
}

uint64_t Heap::GetBlockingGcCount() const {
  return blocking_gc_count_;
}

uint64_t Heap::GetBlockingGcTime() const {
  return blocking_gc_time_;
}

void Heap::DumpGcCountRateHistogram(std::ostream& os) const {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  if (gc_count_rate_histogram_.SampleSize() > 0U) {
    gc_count_rate_histogram_.DumpBins(os);
  }
}

void Heap::DumpBlockingGcCountRateHistogram(std::ostream& os) const {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  if (blocking_gc_count_rate_histogram_.SampleSize() > 0U) {
    blocking_gc_count_rate_histogram_.DumpBins(os);
  }
}

uint64_t Heap::GetPreOomeGcCount() const {
  return pre_oome_gc_count_.load(std::memory_order_relaxed);
}

ALWAYS_INLINE
static inline AllocationListener* GetAndOverwriteAllocationListener(
    Atomic<AllocationListener*>* storage, AllocationListener* new_value) {
  return storage->exchange(new_value);
}

Heap::~Heap() {
  VLOG(heap) << "Starting ~Heap()";
  STLDeleteElements(&garbage_collectors_);
  // If we don't reset then the mark stack complains in its destructor.
  allocation_stack_->Reset();
  allocation_records_.reset();
  live_stack_->Reset();
  STLDeleteValues(&mod_union_tables_);
  STLDeleteValues(&remembered_sets_);
  STLDeleteElements(&continuous_spaces_);
  STLDeleteElements(&discontinuous_spaces_);
  delete gc_complete_lock_;
  delete thread_flip_lock_;
  delete pending_task_lock_;
  delete backtrace_lock_;
  uint64_t unique_count = unique_backtrace_count_.load();
  uint64_t seen_count = seen_backtrace_count_.load();
  if (unique_count != 0 || seen_count != 0) {
    LOG(INFO) << "gc stress unique=" << unique_count << " total=" << (unique_count + seen_count);
  }
  VLOG(heap) << "Finished ~Heap()";
}


space::ContinuousSpace* Heap::FindContinuousSpaceFromAddress(const mirror::Object* addr) const {
  for (const auto& space : continuous_spaces_) {
    if (space->Contains(addr)) {
      return space;
    }
  }
  return nullptr;
}

space::ContinuousSpace* Heap::FindContinuousSpaceFromObject(ObjPtr<mirror::Object> obj,
                                                            bool fail_ok) const {
  space::ContinuousSpace* space = FindContinuousSpaceFromAddress(obj.Ptr());
  if (space != nullptr) {
    return space;
  }
  if (!fail_ok) {
    LOG(FATAL) << "object " << obj << " not inside any spaces!";
  }
  return nullptr;
}

space::DiscontinuousSpace* Heap::FindDiscontinuousSpaceFromObject(ObjPtr<mirror::Object> obj,
                                                                  bool fail_ok) const {
  for (const auto& space : discontinuous_spaces_) {
    if (space->Contains(obj.Ptr())) {
      return space;
    }
  }
  if (!fail_ok) {
    LOG(FATAL) << "object " << obj << " not inside any spaces!";
  }
  return nullptr;
}

space::Space* Heap::FindSpaceFromObject(ObjPtr<mirror::Object> obj, bool fail_ok) const {
  space::Space* result = FindContinuousSpaceFromObject(obj, true);
  if (result != nullptr) {
    return result;
  }
  return FindDiscontinuousSpaceFromObject(obj, fail_ok);
}

space::Space* Heap::FindSpaceFromAddress(const void* addr) const {
  for (const auto& space : continuous_spaces_) {
    if (space->Contains(reinterpret_cast<const mirror::Object*>(addr))) {
      return space;
    }
  }
  for (const auto& space : discontinuous_spaces_) {
    if (space->Contains(reinterpret_cast<const mirror::Object*>(addr))) {
      return space;
    }
  }
  return nullptr;
}

std::string Heap::DumpSpaceNameFromAddress(const void* addr) const {
  space::Space* space = FindSpaceFromAddress(addr);
  return (space != nullptr) ? space->GetName() : "no space";
}

void Heap::ThrowOutOfMemoryError(Thread* self, size_t byte_count, AllocatorType allocator_type) {
  // If we're in a stack overflow, do not create a new exception. It would require running the
  // constructor, which will of course still be in a stack overflow. Note: we only care if the
  // native stack has overflowed. If the simulated stack overflows, it is still possible that the
  // native stack has room to create a new exception.
  if (self->IsHandlingStackOverflow<kNativeStackType>()) {
    self->SetException(
        Runtime::Current()->GetPreAllocatedOutOfMemoryErrorWhenHandlingStackOverflow());
    return;
  }
  // Allow plugins to intercept out of memory errors.
  Runtime::Current()->OutOfMemoryErrorHook();

  std::ostringstream oss;
  size_t total_bytes_free = GetFreeMemory();
  oss << "Failed to allocate a " << byte_count << " byte allocation with " << total_bytes_free
      << " free bytes and " << PrettySize(GetFreeMemoryUntilOOME()) << " until OOM,"
      << " target footprint " << target_footprint_.load(std::memory_order_relaxed)
      << ", growth limit "
      << growth_limit_;
  // If the allocation failed due to fragmentation, print out the largest continuous allocation.
  if (total_bytes_free >= byte_count) {
    space::AllocSpace* space = nullptr;
    if (allocator_type == kAllocatorTypeNonMoving) {
      space = non_moving_space_;
    } else if (allocator_type == kAllocatorTypeRosAlloc ||
               allocator_type == kAllocatorTypeDlMalloc) {
      space = main_space_;
    } else if (allocator_type == kAllocatorTypeBumpPointer ||
               allocator_type == kAllocatorTypeTLAB) {
      space = bump_pointer_space_;
    } else if (allocator_type == kAllocatorTypeRegion ||
               allocator_type == kAllocatorTypeRegionTLAB) {
      space = region_space_;
    }

    // There is no fragmentation info to log for large-object space.
    if (allocator_type != kAllocatorTypeLOS) {
      CHECK(space != nullptr) << "allocator_type:" << allocator_type
                              << " byte_count:" << byte_count
                              << " total_bytes_free:" << total_bytes_free;
      // LogFragmentationAllocFailure returns true if byte_count is greater than
      // the largest free contiguous chunk in the space. Return value false
      // means that we are throwing OOME because the amount of free heap after
      // GC is less than kMinFreeHeapAfterGcForAlloc in proportion of the heap-size.
      // Log an appropriate message in that case.
      if (!space->LogFragmentationAllocFailure(oss, byte_count)) {
        oss << "; giving up on allocation because <"
            << kMinFreeHeapAfterGcForAlloc * 100
            << "% of heap free after GC.";
      }
    }
  }
  self->ThrowOutOfMemoryError(oss.str().c_str());
}

void Heap::DoPendingCollectorTransition() {
  CollectorType desired_collector_type = desired_collector_type_;

  if (collector_type_ == kCollectorTypeCC || collector_type_ == kCollectorTypeCMC) {
    // App's allocations (since last GC) more than the threshold then do TransitionGC
    // when the app was in background. If not then don't do TransitionGC.
    // num_bytes_allocated_since_gc should always be positive even if initially
    // num_bytes_alive_after_gc_ is coming from Zygote. This gives positive or zero value.
    size_t num_bytes_allocated_since_gc =
        UnsignedDifference(GetBytesAllocated(), num_bytes_alive_after_gc_);
    if (num_bytes_allocated_since_gc <
        (UnsignedDifference(target_footprint_.load(std::memory_order_relaxed),
                            num_bytes_alive_after_gc_)/4)
        && !kStressCollectorTransition
        && !IsLowMemoryMode()) {
      return;
    }
  }

  // Launch homogeneous space compaction if it is desired.
  if (desired_collector_type == kCollectorTypeHomogeneousSpaceCompact) {
    if (!CareAboutPauseTimes()) {
      PerformHomogeneousSpaceCompact();
    } else {
      VLOG(gc) << "Homogeneous compaction ignored due to jank perceptible process state";
    }
  } else if (desired_collector_type == kCollectorTypeCCBackground ||
             desired_collector_type == kCollectorTypeCMCBackground) {
    if (!CareAboutPauseTimes()) {
      // Invoke full compaction.
      CollectGarbageInternal(collector::kGcTypeFull,
                             kGcCauseCollectorTransition,
                             /*clear_soft_references=*/false, GetCurrentGcNum() + 1);
    } else {
      VLOG(gc) << "background compaction ignored due to jank perceptible process state";
    }
  } else {
    CHECK_EQ(desired_collector_type, collector_type_) << "Unsupported collector transition";
  }
}

void Heap::Trim(Thread* self) {
  Runtime* const runtime = Runtime::Current();
  if (!CareAboutPauseTimes()) {
    // Deflate the monitors, this can cause a pause but shouldn't matter since we don't care
    // about pauses.
    ScopedTrace trace("Deflating monitors");
    // Avoid race conditions on the lock word for CC.
    ScopedGCCriticalSection gcs(self, kGcCauseTrim, kCollectorTypeHeapTrim);
    ScopedSuspendAll ssa(__FUNCTION__);
    uint64_t start_time = NanoTime();
    size_t count = runtime->GetMonitorList()->DeflateMonitors();
    VLOG(heap) << "Deflating " << count << " monitors took "
        << PrettyDuration(NanoTime() - start_time);
  }
  TrimIndirectReferenceTables(self);
  TrimSpaces(self);
  // Trim arenas that may have been used by JIT or verifier.
  runtime->GetArenaPool()->TrimMaps();
}

class TrimIndirectReferenceTableClosure : public Closure {
 public:
  explicit TrimIndirectReferenceTableClosure(Barrier* barrier) : barrier_(barrier) {
  }
  void Run(Thread* thread) override REQUIRES_SHARED(Locks::mutator_lock_) {
    thread->GetJniEnv()->TrimLocals();
    // If thread is a running mutator, then act on behalf of the trim thread.
    // See the code in ThreadList::RunCheckpoint.
    barrier_->Pass(Thread::Current());
  }

 private:
  Barrier* const barrier_;
};

void Heap::TrimIndirectReferenceTables(Thread* self) {
  ScopedObjectAccess soa(self);
  ScopedTrace trace(__PRETTY_FUNCTION__);
  JavaVMExt* vm = soa.Vm();
  // Trim globals indirect reference table.
  vm->TrimGlobals();
  // Trim locals indirect reference tables.
  // TODO: May also want to look for entirely empty pages maintained by SmallIrtAllocator.
  Barrier barrier(0);
  TrimIndirectReferenceTableClosure closure(&barrier);
  size_t barrier_count = Runtime::Current()->GetThreadList()->RunCheckpoint(&closure);
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForCheckPointsToRun);
  if (barrier_count != 0) {
    barrier.Increment(self, barrier_count);
  }
}

void Heap::StartGC(Thread* self, GcCause cause, CollectorType collector_type) {
  // This can be called in either kRunnable or suspended states.
  // TODO: Consider fixing that?
  ThreadState old_thread_state = self->GetState();
  if (old_thread_state == ThreadState::kRunnable) {
    Locks::mutator_lock_->AssertSharedHeld(self);
    // Manually inlining the following call breaks thread-safety analysis.
    StartGCRunnable(self, cause, collector_type);
    return;
  }
  Locks::mutator_lock_->AssertNotHeld(self);
  self->SetState(ThreadState::kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  WaitForGcToCompleteLocked(cause, self);
  collector_type_running_ = collector_type;
  last_gc_cause_ = cause;
  thread_running_gc_ = self;
  self->SetState(old_thread_state);
}

void Heap::StartGCRunnable(Thread* self, GcCause cause, CollectorType collector_type) {
  Locks::mutator_lock_->AssertSharedHeld(self);
  while (true) {
    self->TransitionFromRunnableToSuspended(ThreadState::kWaitingForGcToComplete);
    {
      MutexLock mu(self, *gc_complete_lock_);
      // Ensure there is only one GC at a time.
      WaitForGcToCompleteLocked(cause, self);
      collector_type_running_ = collector_type;
      last_gc_cause_ = cause;
      thread_running_gc_ = self;
    }
    // We have to be careful returning to runnable state, since that could cause us to block.
    // That would be bad, since collector_type_running_ is set, and hence no GC is possible in this
    // state, allowing deadlock.
    if (LIKELY(self->TryTransitionFromSuspendedToRunnable())) {
      return;
    }
    {
      MutexLock mu(self, *gc_complete_lock_);
      collector_type_running_ = kCollectorTypeNone;
      thread_running_gc_ = nullptr;
    }
    self->TransitionFromSuspendedToRunnable();  // Will handle suspension request and block.
  }
}

void Heap::TrimSpaces(Thread* self) {
  // Pretend we are doing a GC to prevent background compaction from deleting the space we are
  // trimming.
  StartGC(self, kGcCauseTrim, kCollectorTypeHeapTrim);
  ScopedTrace trace(__PRETTY_FUNCTION__);
  const uint64_t start_ns = NanoTime();
  // Trim the managed spaces.
  uint64_t total_alloc_space_allocated = 0;
  uint64_t total_alloc_space_size = 0;
  uint64_t managed_reclaimed = 0;
  {
    ScopedObjectAccess soa(self);
    for (const auto& space : continuous_spaces_) {
      if (space->IsMallocSpace()) {
        gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
        if (malloc_space->IsRosAllocSpace() || !CareAboutPauseTimes()) {
          // Don't trim dlmalloc spaces if we care about pauses since this can hold the space lock
          // for a long period of time.
          managed_reclaimed += malloc_space->Trim();
        }
        total_alloc_space_size += malloc_space->Size();
      }
    }
  }
  total_alloc_space_allocated = GetBytesAllocated();
  if (large_object_space_ != nullptr) {
    total_alloc_space_allocated -= large_object_space_->GetBytesAllocated();
  }
  if (bump_pointer_space_ != nullptr) {
    total_alloc_space_allocated -= bump_pointer_space_->Size();
  }
  if (region_space_ != nullptr) {
    total_alloc_space_allocated -= region_space_->GetBytesAllocated();
  }
  const float managed_utilization = static_cast<float>(total_alloc_space_allocated) /
      static_cast<float>(total_alloc_space_size);
  uint64_t gc_heap_end_ns = NanoTime();
  // We never move things in the native heap, so we can finish the GC at this point.
  FinishGC(self, collector::kGcTypeNone);

  VLOG(heap) << "Heap trim of managed (duration=" << PrettyDuration(gc_heap_end_ns - start_ns)
      << ", advised=" << PrettySize(managed_reclaimed) << ") heap. Managed heap utilization of "
      << static_cast<int>(100 * managed_utilization) << "%.";
}

bool Heap::IsValidObjectAddress(const void* addr) const {
  if (addr == nullptr) {
    return true;
  }
  return IsAligned<kObjectAlignment>(addr) && FindSpaceFromAddress(addr) != nullptr;
}

bool Heap::IsNonDiscontinuousSpaceHeapAddress(const void* addr) const {
  return FindContinuousSpaceFromAddress(reinterpret_cast<const mirror::Object*>(addr)) != nullptr;
}

bool Heap::IsLiveObjectLocked(ObjPtr<mirror::Object> obj,
                              bool search_allocation_stack,
                              bool search_live_stack,
                              bool sorted) {
  if (UNLIKELY(!IsAligned<kObjectAlignment>(obj.Ptr()))) {
    return false;
  }
  if (bump_pointer_space_ != nullptr && bump_pointer_space_->HasAddress(obj.Ptr())) {
    mirror::Class* klass = obj->GetClass<kVerifyNone>();
    if (obj == klass) {
      // This case happens for java.lang.Class.
      return true;
    }
    return VerifyClassClass(klass) && IsLiveObjectLocked(klass);
  } else if (temp_space_ != nullptr && temp_space_->HasAddress(obj.Ptr())) {
    // If we are in the allocated region of the temp space, then we are probably live (e.g. during
    // a GC). When a GC isn't running End() - Begin() is 0 which means no objects are contained.
    return temp_space_->Contains(obj.Ptr());
  }
  if (region_space_ != nullptr && region_space_->HasAddress(obj.Ptr())) {
    return true;
  }
  space::ContinuousSpace* c_space = FindContinuousSpaceFromObject(obj, true);
  space::DiscontinuousSpace* d_space = nullptr;
  if (c_space != nullptr) {
    if (c_space->GetLiveBitmap()->Test(obj.Ptr())) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != nullptr) {
      if (d_space->GetLiveBitmap()->Test(obj.Ptr())) {
        return true;
      }
    }
  }
  // This is covering the allocation/live stack swapping that is done without mutators suspended.
  for (size_t i = 0; i < (sorted ? 1 : 5); ++i) {
    if (i > 0) {
      NanoSleep(MsToNs(10));
    }
    if (search_allocation_stack) {
      if (sorted) {
        if (allocation_stack_->ContainsSorted(obj.Ptr())) {
          return true;
        }
      } else if (allocation_stack_->Contains(obj.Ptr())) {
        return true;
      }
    }

    if (search_live_stack) {
      if (sorted) {
        if (live_stack_->ContainsSorted(obj.Ptr())) {
          return true;
        }
      } else if (live_stack_->Contains(obj.Ptr())) {
        return true;
      }
    }
  }
  // We need to check the bitmaps again since there is a race where we mark something as live and
  // then clear the stack containing it.
  if (c_space != nullptr) {
    if (c_space->GetLiveBitmap()->Test(obj.Ptr())) {
      return true;
    }
  } else {
    d_space = FindDiscontinuousSpaceFromObject(obj, true);
    if (d_space != nullptr && d_space->GetLiveBitmap()->Test(obj.Ptr())) {
      return true;
    }
  }
  return false;
}

std::string Heap::DumpSpaces() const {
  std::ostringstream oss;
  DumpSpaces(oss);
  return oss.str();
}

void Heap::DumpSpaces(std::ostream& stream) const {
  for (const auto& space : continuous_spaces_) {
    accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    stream << space << " " << *space << "\n";
    if (live_bitmap != nullptr) {
      stream << live_bitmap << " " << *live_bitmap << "\n";
    }
    if (mark_bitmap != nullptr) {
      stream << mark_bitmap << " " << *mark_bitmap << "\n";
    }
  }
  for (const auto& space : discontinuous_spaces_) {
    stream << space << " " << *space << "\n";
  }
}

void Heap::VerifyObjectBody(ObjPtr<mirror::Object> obj) {
  if (verify_object_mode_ == kVerifyObjectModeDisabled) {
    return;
  }

  // Ignore early dawn of the universe verifications.
  if (UNLIKELY(num_bytes_allocated_.load(std::memory_order_relaxed) < 10 * KB)) {
    return;
  }
  CHECK_ALIGNED(obj.Ptr(), kObjectAlignment) << "Object isn't aligned";
  mirror::Class* c = obj->GetFieldObject<mirror::Class, kVerifyNone>(mirror::Object::ClassOffset());
  CHECK(c != nullptr) << "Null class in object " << obj;
  CHECK_ALIGNED(c, kObjectAlignment) << "Class " << c << " not aligned in object " << obj;
  CHECK(VerifyClassClass(c));

  if (verify_object_mode_ > kVerifyObjectModeFast) {
    // Note: the bitmap tests below are racy since we don't hold the heap bitmap lock.
    CHECK(IsLiveObjectLocked(obj)) << "Object is dead " << obj << "\n" << DumpSpaces();
  }
}

void Heap::VerifyHeap() {
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  auto visitor = [&](mirror::Object* obj) NO_THREAD_SAFETY_ANALYSIS {
    VerifyObjectBody(obj);
  };
  // Technically we need the mutator lock here to call Visit. However, VerifyObjectBody is already
  // NO_THREAD_SAFETY_ANALYSIS.
  auto no_thread_safety_analysis = [&]() NO_THREAD_SAFETY_ANALYSIS {
    GetLiveBitmap()->Visit(visitor);
  };
  no_thread_safety_analysis();
}

void Heap::RecordFree(uint64_t freed_objects, int64_t freed_bytes) {
  // Use signed comparison since freed bytes can be negative when background compaction foreground
  // transitions occurs. This is typically due to objects moving from a bump pointer space to a
  // free list backed space, which may increase memory footprint due to padding and binning.
  RACING_DCHECK_LE(freed_bytes,
                   static_cast<int64_t>(num_bytes_allocated_.load(std::memory_order_relaxed)));
  // Note: This relies on 2s complement for handling negative freed_bytes.
  num_bytes_allocated_.fetch_sub(static_cast<ssize_t>(freed_bytes), std::memory_order_relaxed);
  if (Runtime::Current()->HasStatsEnabled()) {
    RuntimeStats* thread_stats = Thread::Current()->GetStats();
    thread_stats->freed_objects += freed_objects;
    thread_stats->freed_bytes += freed_bytes;
    // TODO: Do this concurrently.
    RuntimeStats* global_stats = Runtime::Current()->GetStats();
    global_stats->freed_objects += freed_objects;
    global_stats->freed_bytes += freed_bytes;
  }
}

void Heap::RecordFreeRevoke() {
  // Subtract num_bytes_freed_revoke_ from num_bytes_allocated_ to cancel out the
  // ahead-of-time, bulk counting of bytes allocated in rosalloc thread-local buffers.
  // If there's a concurrent revoke, ok to not necessarily reset num_bytes_freed_revoke_
  // all the way to zero exactly as the remainder will be subtracted at the next GC.
  size_t bytes_freed = num_bytes_freed_revoke_.load(std::memory_order_relaxed);
  CHECK_GE(num_bytes_freed_revoke_.fetch_sub(bytes_freed, std::memory_order_relaxed),
           bytes_freed) << "num_bytes_freed_revoke_ underflow";
  CHECK_GE(num_bytes_allocated_.fetch_sub(bytes_freed, std::memory_order_relaxed),
           bytes_freed) << "num_bytes_allocated_ underflow";
  GetCurrentGcIteration()->SetFreedRevoke(bytes_freed);
}

space::RosAllocSpace* Heap::GetRosAllocSpace(gc::allocator::RosAlloc* rosalloc) const {
  if (rosalloc_space_ != nullptr && rosalloc_space_->GetRosAlloc() == rosalloc) {
    return rosalloc_space_;
  }
  for (const auto& space : continuous_spaces_) {
    if (space->AsContinuousSpace()->IsRosAllocSpace()) {
      if (space->AsContinuousSpace()->AsRosAllocSpace()->GetRosAlloc() == rosalloc) {
        return space->AsContinuousSpace()->AsRosAllocSpace();
      }
    }
  }
  return nullptr;
}

static inline bool EntrypointsInstrumented() REQUIRES_SHARED(Locks::mutator_lock_) {
  instrumentation::Instrumentation* const instrumentation =
      Runtime::Current()->GetInstrumentation();
  return instrumentation != nullptr && instrumentation->AllocEntrypointsInstrumented();
}

mirror::Object* Heap::AllocateInternalWithGc(Thread* self,
                                             AllocatorType allocator,
                                             bool instrumented,
                                             size_t alloc_size,
                                             size_t* bytes_allocated,
                                             size_t* usable_size,
                                             size_t* bytes_tl_bulk_allocated,
                                             ObjPtr<mirror::Class>* klass) {
  bool was_default_allocator = allocator == GetCurrentAllocator();
  // Make sure there is no pending exception since we may need to throw an OOME.
  self->AssertNoPendingException();
  DCHECK(klass != nullptr);

  StackHandleScope<1> hs(self);
  HandleWrapperObjPtr<mirror::Class> h_klass(hs.NewHandleWrapper(klass));

  auto send_object_pre_alloc =
      [&]() REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_) {
        if (UNLIKELY(instrumented)) {
          AllocationListener* l = alloc_listener_.load(std::memory_order_seq_cst);
          if (UNLIKELY(l != nullptr) && UNLIKELY(l->HasPreAlloc())) {
            l->PreObjectAllocated(self, h_klass, &alloc_size);
          }
        }
      };
#define PERFORM_SUSPENDING_OPERATION(op)                                          \
  [&]() REQUIRES(Roles::uninterruptible_) REQUIRES_SHARED(Locks::mutator_lock_) { \
    ScopedAllowThreadSuspension ats;                                              \
    auto res = (op);                                                              \
    send_object_pre_alloc();                                                      \
    return res;                                                                   \
  }()

  // The allocation failed. If the GC is running, block until it completes, and then retry the
  // allocation.
  collector::GcType last_gc =
      PERFORM_SUSPENDING_OPERATION(WaitForGcToComplete(kGcCauseForAlloc, self));
  // If we were the default allocator but the allocator changed while we were suspended,
  // abort the allocation.
  if ((was_default_allocator && allocator != GetCurrentAllocator()) ||
      (!instrumented && EntrypointsInstrumented())) {
    return nullptr;
  }
  uint32_t starting_gc_num = GetCurrentGcNum();
  if (last_gc != collector::kGcTypeNone) {
    // A GC was in progress and we blocked, retry allocation now that memory has been freed.
    mirror::Object* ptr = TryToAllocate<true, false>(self, allocator, alloc_size, bytes_allocated,
                                                     usable_size, bytes_tl_bulk_allocated);
    if (ptr != nullptr) {
      return ptr;
    }
  }
  if (IsGCDisabledForShutdown()) {
    // We're just shutting down and GCs don't work anymore. Try a different allocator.
    mirror::Object* ptr = TryToAllocate<true, false>(self,
                                                     kAllocatorTypeNonMoving,
                                                     alloc_size,
                                                     bytes_allocated,
                                                     usable_size,
                                                     bytes_tl_bulk_allocated);
    if (ptr != nullptr) {
      return ptr;
    }
  }

  int64_t bytes_freed_before = GetBytesFreedEver();
  auto have_reclaimed_enough = [&]() {
    size_t curr_bytes_allocated = GetBytesAllocated();
    size_t free_heap = UnsignedDifference(growth_limit_, curr_bytes_allocated);
    int64_t newly_freed = GetBytesFreedEver() - bytes_freed_before;
    double free_heap_ratio = static_cast<double>(free_heap) / growth_limit_;
    double newly_freed_ratio = static_cast<double>(newly_freed) / growth_limit_;
    return free_heap_ratio >= kMinFreeHeapAfterGcForAlloc ||
           newly_freed_ratio >= kMinFreedHeapAfterGcForAlloc;
  };
  // We perform one GC as per the next_gc_type_ (chosen in GrowForUtilization),
  // if it's not already tried. If that doesn't succeed then go for the most
  // exhaustive option. Perform a full-heap collection including clearing
  // SoftReferences. In case of ConcurrentCopying, it will also ensure that
  // all regions are evacuated. If allocation doesn't succeed even after that
  // then there is no hope, so we throw OOME.
  collector::GcType tried_type = next_gc_type_;
  if (last_gc < tried_type) {
    VLOG(gc) << "Starting a blocking GC " << kGcCauseForAlloc;
    PERFORM_SUSPENDING_OPERATION(
        CollectGarbageInternal(tried_type, kGcCauseForAlloc, false, starting_gc_num + 1));

    if ((was_default_allocator && allocator != GetCurrentAllocator()) ||
        (!instrumented && EntrypointsInstrumented())) {
      return nullptr;
    }
    // Check this even if we didn't actually run a GC; if we didn't someone else probably did.
    if (have_reclaimed_enough()) {
      mirror::Object* ptr = TryToAllocate<true, false>(self, allocator,
                                                       alloc_size, bytes_allocated,
                                                       usable_size, bytes_tl_bulk_allocated);
      if (ptr != nullptr) {
        return ptr;
      }
    }
  }
  // Most allocations should have succeeded by now, so the heap is really full, really fragmented,
  // or the requested size is really big. Do another GC, collecting SoftReferences this time. The
  // VM spec requires that all SoftReferences have been collected and cleared before throwing
  // OOME.
  VLOG(gc) << "Forcing collection of SoftReferences for " << PrettySize(alloc_size)
           << " allocation";
  // TODO: Run finalization, but this may cause more allocations to occur.
  // We don't need a WaitForGcToComplete here either.
  // TODO: Should check whether another thread already just ran a GC with soft
  // references.

  DCHECK(!gc_plan_.empty());

  int64_t min_freed_to_continue =
      static_cast<int64_t>(kMinFreedHeapAfterGcForAlloc * growth_limit_ + alloc_size);
  // Repeatedly collect the entire heap until either
  // (a) this was insufficiently productive at reclaiming memory and we should give upt to avoid
  // "GC thrashing", or
  // (b) GC was sufficiently productive (reclaimed min_freed_to_continue bytes) AND allowed us to
  // satisfy the allocation request.
  bool gc_ran;
  int gc_attempts = 0;
  // A requested GC can fail to run because either someone else beat us to it, or because we can't
  // run a GC in this state. In the latter case, we return quickly. Just try a small number of
  // times.
  static constexpr int kMaxGcAttempts = 5;
  do {
    bytes_freed_before = GetBytesFreedEver();
    pre_oome_gc_count_.fetch_add(1, std::memory_order_relaxed);
    // TODO(b/353333767): Do this only if nobody else beats us to it. If we're having trouble
    // allocating, probably other threads are in the same boat.
    starting_gc_num = GetCurrentGcNum();
    gc_ran = PERFORM_SUSPENDING_OPERATION(
        CollectGarbageInternal(gc_plan_.back(), kGcCauseForAlloc, true, starting_gc_num + 1) !=
        collector::kGcTypeNone);
    ++gc_attempts;
    if ((was_default_allocator && allocator != GetCurrentAllocator()) ||
        (!instrumented && EntrypointsInstrumented())) {
      return nullptr;
    }
    bool ran_homogeneous_space_compaction = false;
    bool immediately_reclaimed_enough = have_reclaimed_enough();
    if (!immediately_reclaimed_enough) {
      const uint64_t current_time = NanoTime();
      if (allocator == kAllocatorTypeRosAlloc || allocator == kAllocatorTypeDlMalloc) {
        if (use_homogeneous_space_compaction_for_oom_ &&
            current_time - last_time_homogeneous_space_compaction_by_oom_ >
            min_interval_homogeneous_space_compaction_by_oom_) {
          last_time_homogeneous_space_compaction_by_oom_ = current_time;
          ran_homogeneous_space_compaction =
              (PERFORM_SUSPENDING_OPERATION(PerformHomogeneousSpaceCompact()) ==
               HomogeneousSpaceCompactResult::kSuccess);
          // Thread suspension could have occurred.
          if ((was_default_allocator && allocator != GetCurrentAllocator()) ||
              (!instrumented && EntrypointsInstrumented())) {
            return nullptr;
          }
          // Always print that we ran homogeneous space compation since this can cause jank.
          VLOG(heap) << "Ran heap homogeneous space compaction, "
                    << " requested defragmentation "
                    << count_requested_homogeneous_space_compaction_.load()
                    << " performed defragmentation "
                    << count_performed_homogeneous_space_compaction_.load()
                    << " ignored homogeneous space compaction "
                    << count_ignored_homogeneous_space_compaction_.load()
                    << " delayed count = "
                    << count_delayed_oom_.load();
        }
      }
    }
    if (immediately_reclaimed_enough ||
        (ran_homogeneous_space_compaction && have_reclaimed_enough())) {
      mirror::Object* ptr = TryToAllocate<true, true>(
          self, allocator, alloc_size, bytes_allocated, usable_size, bytes_tl_bulk_allocated);
      if (ptr != nullptr) {
        if (ran_homogeneous_space_compaction) {
          count_delayed_oom_++;
        }
        return ptr;
      }
    }
    // This loops only if we reclaimed plenty of memory, but presumably some other thread beat us
    // to allocating it. In the very unlikely case that we're running into a serious fragmentation
    // issue, and there is no other thread allocating, GCs will quickly become unsuccessful, and we
    // will stop then. If another thread is allocating aggressively, this may go on for a while,
    // but we are still making progress somewhere.
  } while ((!gc_ran && gc_attempts < kMaxGcAttempts) ||
           GetBytesFreedEver() - bytes_freed_before > min_freed_to_continue);
#undef PERFORM_SUSPENDING_OPERATION
  // Throw an OOM error.
  {
    ScopedAllowThreadSuspension ats;
    ThrowOutOfMemoryError(self, alloc_size, allocator);
  }
  return nullptr;
}

void Heap::SetTargetHeapUtilization(float target) {
  DCHECK_GT(target, 0.1f);  // asserted in Java code
  DCHECK_LT(target, 1.0f);
  target_utilization_ = target;
}

size_t Heap::GetObjectsAllocated() const {
  Thread* const self = Thread::Current();
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGetObjectsAllocated);
  // Prevent GC running during GetObjectsAllocated since we may get a checkpoint request that tells
  // us to suspend while we are doing SuspendAll. b/35232978
  gc::ScopedGCCriticalSection gcs(Thread::Current(),
                                  gc::kGcCauseGetObjectsAllocated,
                                  gc::kCollectorTypeGetObjectsAllocated);
  // Need SuspendAll here to prevent lock violation if RosAlloc does it during InspectAll.
  ScopedSuspendAll ssa(__FUNCTION__);
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  size_t total = 0;
  for (space::AllocSpace* space : alloc_spaces_) {
    total += space->GetObjectsAllocated();
  }
  return total;
}

uint64_t Heap::GetBytesAllocatedEver() const {
  // Force the returned value to be monotonically increasing, in the sense that if this is called
  // at A and B, such that A happens-before B, then the call at B returns a value no smaller than
  // that at A. This is not otherwise guaranteed, since num_bytes_allocated_ is decremented first,
  // and total_bytes_freed_ever_ is incremented later.
  static std::atomic<uint64_t> max_bytes_so_far(0);
  uint64_t so_far = max_bytes_so_far.load(std::memory_order_relaxed);
  uint64_t current_bytes = GetBytesFreedEver(std::memory_order_acquire) + GetBytesAllocated();
  DCHECK(current_bytes < (static_cast<uint64_t>(1) << 63));  // result is "positive".
  do {
    if (current_bytes <= so_far) {
      return so_far;
    }
  } while (!max_bytes_so_far.compare_exchange_weak(so_far /* updated */,
                                                   current_bytes, std::memory_order_relaxed));
  return current_bytes;
}

// Check whether the given object is an instance of the given class.
static bool MatchesClass(mirror::Object* obj,
                         Handle<mirror::Class> h_class,
                         bool use_is_assignable_from) REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Class* instance_class = obj->GetClass();
  CHECK(instance_class != nullptr);
  ObjPtr<mirror::Class> klass = h_class.Get();
  if (use_is_assignable_from) {
    return klass != nullptr && klass->IsAssignableFrom(instance_class);
  }
  return instance_class == klass;
}

void Heap::CountInstances(const std::vector<Handle<mirror::Class>>& classes,
                          bool use_is_assignable_from,
                          uint64_t* counts) {
  auto instance_counter = [&](mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
    for (size_t i = 0; i < classes.size(); ++i) {
      if (MatchesClass(obj, classes[i], use_is_assignable_from)) {
        ++counts[i];
      }
    }
  };
  VisitObjects(instance_counter);
}

void Heap::CollectGarbage(bool clear_soft_references, GcCause cause) {
  // Even if we waited for a GC we still need to do another GC since weaks allocated during the
  // last GC will not have necessarily been cleared.
  CollectGarbageInternal(gc_plan_.back(), cause, clear_soft_references, GC_NUM_ANY);
}

bool Heap::SupportHomogeneousSpaceCompactAndCollectorTransitions() const {
  return main_space_backup_.get() != nullptr && main_space_ != nullptr &&
      foreground_collector_type_ == kCollectorTypeCMS;
}

HomogeneousSpaceCompactResult Heap::PerformHomogeneousSpaceCompact() {
  Thread* self = Thread::Current();
  // Inc requested homogeneous space compaction.
  count_requested_homogeneous_space_compaction_++;
  // Store performed homogeneous space compaction at a new request arrival.
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  {
    ScopedThreadStateChange tsc2(self, ThreadState::kWaitingForGcToComplete);
    MutexLock mu(self, *gc_complete_lock_);
    // Ensure there is only one GC at a time.
    WaitForGcToCompleteLocked(kGcCauseHomogeneousSpaceCompact, self);
    // Homogeneous space compaction is a copying transition, can't run it if the moving GC disable
    // count is non zero.
    // If the collector type changed to something which doesn't benefit from homogeneous space
    // compaction, exit.
    if (disable_moving_gc_count_ != 0 || IsMovingGc(collector_type_) ||
        !main_space_->CanMoveObjects()) {
      return kErrorReject;
    }
    if (!SupportHomogeneousSpaceCompactAndCollectorTransitions()) {
      return kErrorUnsupported;
    }
    collector_type_running_ = kCollectorTypeHomogeneousSpaceCompact;
  }
  if (Runtime::Current()->IsShuttingDown(self)) {
    // Don't allow heap transitions to happen if the runtime is shutting down since these can
    // cause objects to get finalized.
    FinishGC(self, collector::kGcTypeNone);
    return HomogeneousSpaceCompactResult::kErrorVMShuttingDown;
  }
  collector::GarbageCollector* collector;
  {
    ScopedSuspendAll ssa(__FUNCTION__);
    uint64_t start_time = NanoTime();
    // Launch compaction.
    space::MallocSpace* to_space = main_space_backup_.release();
    space::MallocSpace* from_space = main_space_;
    to_space->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
    const uint64_t space_size_before_compaction = from_space->Size();
    AddSpace(to_space);
    // Make sure that we will have enough room to copy.
    CHECK_GE(to_space->GetFootprintLimit(), from_space->GetFootprintLimit());
    collector = Compact(to_space, from_space, kGcCauseHomogeneousSpaceCompact);
    const uint64_t space_size_after_compaction = to_space->Size();
    main_space_ = to_space;
    main_space_backup_.reset(from_space);
    RemoveSpace(from_space);
    SetSpaceAsDefault(main_space_);  // Set as default to reset the proper dlmalloc space.
    // Update performed homogeneous space compaction count.
    count_performed_homogeneous_space_compaction_++;
    // Print statics log and resume all threads.
    uint64_t duration = NanoTime() - start_time;
    VLOG(heap) << "Heap homogeneous space compaction took " << PrettyDuration(duration) << " size: "
               << PrettySize(space_size_before_compaction) << " -> "
               << PrettySize(space_size_after_compaction) << " compact-ratio: "
               << std::fixed << static_cast<double>(space_size_after_compaction) /
               static_cast<double>(space_size_before_compaction);
  }
  // Finish GC.
  // Get the references we need to enqueue.
  SelfDeletingTask* clear = reference_processor_->CollectClearedReferences(self);
  GrowForUtilization(semi_space_collector_);
  LogGC(kGcCauseHomogeneousSpaceCompact, collector);
  FinishGC(self, collector::kGcTypeFull);
  // Enqueue any references after losing the GC locks.
  clear->Run(self);
  clear->Finalize();
  {
    ScopedObjectAccess soa(self);
    soa.Vm()->UnloadNativeLibraries();
  }
  return HomogeneousSpaceCompactResult::kSuccess;
}

void Heap::SetDefaultConcurrentStartBytes() {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  if (collector_type_running_ != kCollectorTypeNone) {
    // If a collector is already running, just let it set concurrent_start_bytes_ .
    return;
  }
  SetDefaultConcurrentStartBytesLocked();
}

void Heap::SetDefaultConcurrentStartBytesLocked() {
  if (IsGcConcurrent()) {
    size_t target_footprint = target_footprint_.load(std::memory_order_relaxed);
    size_t reserve_bytes = target_footprint / 4;
    reserve_bytes = std::min(reserve_bytes, kMaxConcurrentRemainingBytes);
    reserve_bytes = std::max(reserve_bytes, kMinConcurrentRemainingBytes);
    concurrent_start_bytes_ = UnsignedDifference(target_footprint, reserve_bytes);
  } else {
    concurrent_start_bytes_ = std::numeric_limits<size_t>::max();
  }
}

void Heap::ChangeCollector(CollectorType collector_type) {
  // TODO: Only do this with all mutators suspended to avoid races.
  if (collector_type != collector_type_) {
    collector_type_ = collector_type;
    gc_plan_.clear();
    switch (collector_type_) {
      case kCollectorTypeCC: {
        if (use_generational_gc_) {
          gc_plan_.push_back(collector::kGcTypeSticky);
        }
        gc_plan_.push_back(collector::kGcTypeFull);
        if (use_tlab_) {
          ChangeAllocator(kAllocatorTypeRegionTLAB);
        } else {
          ChangeAllocator(kAllocatorTypeRegion);
        }
        break;
      }
      case kCollectorTypeCMC: {
        if (use_generational_gc_) {
          gc_plan_.push_back(collector::kGcTypeSticky);
        }
        gc_plan_.push_back(collector::kGcTypeFull);
        if (use_tlab_) {
          ChangeAllocator(kAllocatorTypeTLAB);
        } else {
          ChangeAllocator(kAllocatorTypeBumpPointer);
        }
        break;
      }
      case kCollectorTypeSS: {
        gc_plan_.push_back(collector::kGcTypeFull);
        if (use_tlab_) {
          ChangeAllocator(kAllocatorTypeTLAB);
        } else {
          ChangeAllocator(kAllocatorTypeBumpPointer);
        }
        break;
      }
      case kCollectorTypeMS: {
        gc_plan_.push_back(collector::kGcTypeSticky);
        gc_plan_.push_back(collector::kGcTypePartial);
        gc_plan_.push_back(collector::kGcTypeFull);
        ChangeAllocator(kUseRosAlloc ? kAllocatorTypeRosAlloc : kAllocatorTypeDlMalloc);
        break;
      }
      case kCollectorTypeCMS: {
        gc_plan_.push_back(collector::kGcTypeSticky);
        gc_plan_.push_back(collector::kGcTypePartial);
        gc_plan_.push_back(collector::kGcTypeFull);
        ChangeAllocator(kUseRosAlloc ? kAllocatorTypeRosAlloc : kAllocatorTypeDlMalloc);
        break;
      }
      default: {
        UNIMPLEMENTED(FATAL);
        UNREACHABLE();
      }
    }
    SetDefaultConcurrentStartBytesLocked();
  }
}

// Special compacting collector which uses sub-optimal bin packing to reduce zygote space size.
class ZygoteCompactingCollector final : public collector::SemiSpace {
 public:
  ZygoteCompactingCollector(gc::Heap* heap, bool is_running_on_memory_tool)
      : SemiSpace(heap, "zygote collector"),
        bin_live_bitmap_(nullptr),
        bin_mark_bitmap_(nullptr),
        is_running_on_memory_tool_(is_running_on_memory_tool) {}

  void BuildBins(space::ContinuousSpace* space) REQUIRES_SHARED(Locks::mutator_lock_) {
    bin_live_bitmap_ = space->GetLiveBitmap();
    bin_mark_bitmap_ = space->GetMarkBitmap();
    uintptr_t prev = reinterpret_cast<uintptr_t>(space->Begin());
    WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    // Note: This requires traversing the space in increasing order of object addresses.
    auto visitor = [&](mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
      uintptr_t object_addr = reinterpret_cast<uintptr_t>(obj);
      size_t bin_size = object_addr - prev;
      // Add the bin consisting of the end of the previous object to the start of the current object.
      AddBin(bin_size, prev);
      prev = object_addr + RoundUp(obj->SizeOf<kDefaultVerifyFlags>(), kObjectAlignment);
    };
    bin_live_bitmap_->Walk(visitor);
    // Add the last bin which spans after the last object to the end of the space.
    AddBin(reinterpret_cast<uintptr_t>(space->End()) - prev, prev);
  }

 private:
  // Maps from bin sizes to locations.
  std::multimap<size_t, uintptr_t> bins_;
  // Live bitmap of the space which contains the bins.
  accounting::ContinuousSpaceBitmap* bin_live_bitmap_;
  // Mark bitmap of the space which contains the bins.
  accounting::ContinuousSpaceBitmap* bin_mark_bitmap_;
  const bool is_running_on_memory_tool_;

  void AddBin(size_t size, uintptr_t position) {
    if (is_running_on_memory_tool_) {
      MEMORY_TOOL_MAKE_DEFINED(reinterpret_cast<void*>(position), size);
    }
    if (size != 0) {
      bins_.insert(std::make_pair(size, position));
    }
  }

  bool ShouldSweepSpace([[maybe_unused]] space::ContinuousSpace* space) const override {
    // Don't sweep any spaces since we probably blasted the internal accounting of the free list
    // allocator.
    return false;
  }

  mirror::Object* MarkNonForwardedObject(mirror::Object* obj) override
      REQUIRES(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    size_t obj_size = obj->SizeOf<kDefaultVerifyFlags>();
    size_t alloc_size = RoundUp(obj_size, kObjectAlignment);
    mirror::Object* forward_address;
    // Find the smallest bin which we can move obj in.
    auto it = bins_.lower_bound(alloc_size);
    if (it == bins_.end()) {
      // No available space in the bins, place it in the target space instead (grows the zygote
      // space).
      size_t bytes_allocated, unused_bytes_tl_bulk_allocated;
      forward_address = to_space_->Alloc(
          self_, alloc_size, &bytes_allocated, nullptr, &unused_bytes_tl_bulk_allocated);
      if (to_space_live_bitmap_ != nullptr) {
        to_space_live_bitmap_->Set(forward_address);
      } else {
        GetHeap()->GetNonMovingSpace()->GetLiveBitmap()->Set(forward_address);
        GetHeap()->GetNonMovingSpace()->GetMarkBitmap()->Set(forward_address);
      }
    } else {
      size_t size = it->first;
      uintptr_t pos = it->second;
      bins_.erase(it);  // Erase the old bin which we replace with the new smaller bin.
      forward_address = reinterpret_cast<mirror::Object*>(pos);
      // Set the live and mark bits so that sweeping system weaks works properly.
      bin_live_bitmap_->Set(forward_address);
      bin_mark_bitmap_->Set(forward_address);
      DCHECK_GE(size, alloc_size);
      // Add a new bin with the remaining space.
      AddBin(size - alloc_size, pos + alloc_size);
    }
    // Copy the object over to its new location.
    // Historical note: We did not use `alloc_size` to avoid a Valgrind error.
    memcpy(reinterpret_cast<void*>(forward_address), obj, obj_size);
    if (kUseBakerReadBarrier) {
      obj->AssertReadBarrierState();
      forward_address->AssertReadBarrierState();
    }
    return forward_address;
  }
};

void Heap::UnBindBitmaps() {
  TimingLogger::ScopedTiming t("UnBindBitmaps", GetCurrentGcIteration()->GetTimings());
  for (const auto& space : GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (alloc_space->GetLiveBitmap() != nullptr && alloc_space->HasBoundBitmaps()) {
        alloc_space->UnBindBitmaps();
      }
    }
  }
}

void Heap::IncrementFreedEver() {
  // Counters are updated only by us, but may be read concurrently.
  // The updates should become visible after the corresponding live object info.
  total_objects_freed_ever_.store(total_objects_freed_ever_.load(std::memory_order_relaxed)
                                  + GetCurrentGcIteration()->GetFreedObjects()
                                  + GetCurrentGcIteration()->GetFreedLargeObjects(),
                                  std::memory_order_release);
  total_bytes_freed_ever_.store(total_bytes_freed_ever_.load(std::memory_order_relaxed)
                                + GetCurrentGcIteration()->GetFreedBytes()
                                + GetCurrentGcIteration()->GetFreedLargeObjectBytes(),
                                std::memory_order_release);
}

#pragma clang diagnostic push
#if !ART_USE_FUTEXES
// Frame gets too large, perhaps due to Bionic pthread_mutex_lock size. We don't care.
#  pragma clang diagnostic ignored "-Wframe-larger-than="
#endif
// This has a large frame, but shouldn't be run anywhere near the stack limit.
// FIXME: BUT it did exceed... http://b/197647048
#  pragma clang diagnostic ignored "-Wframe-larger-than="
void Heap::PreZygoteFork() {
  if (!HasZygoteSpace()) {
    // We still want to GC in case there is some unreachable non moving objects that could cause a
    // suboptimal bin packing when we compact the zygote space.
    CollectGarbageInternal(collector::kGcTypeFull, kGcCauseBackground, false, GC_NUM_ANY);
    // Trim the pages at the end of the non moving space. Trim while not holding zygote lock since
    // the trim process may require locking the mutator lock.
    non_moving_space_->Trim();
  }
  // We need to close userfaultfd fd for app/webview zygotes to avoid getattr
  // (stat) on the fd during fork.
  Thread* self = Thread::Current();
  MutexLock mu(self, zygote_creation_lock_);
  // Try to see if we have any Zygote spaces.
  if (HasZygoteSpace()) {
    return;
  }
  Runtime* runtime = Runtime::Current();
  // Setup linear-alloc pool for post-zygote fork allocations before freezing
  // snapshots of intern-table and class-table.
  runtime->SetupLinearAllocForPostZygoteFork(self);
  runtime->GetInternTable()->AddNewTable();
  runtime->GetClassLinker()->MoveClassTableToPreZygote();
  VLOG(heap) << "Starting PreZygoteFork";
  // The end of the non-moving space may be protected, unprotect it so that we can copy the zygote
  // there.
  non_moving_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
  const bool same_space = non_moving_space_ == main_space_;
  if (kCompactZygote) {
    // Temporarily disable rosalloc verification because the zygote
    // compaction will mess up the rosalloc internal metadata.
    ScopedDisableRosAllocVerification disable_rosalloc_verif(this);
    ZygoteCompactingCollector zygote_collector(this, is_running_on_memory_tool_);
    zygote_collector.BuildBins(non_moving_space_);
    // Create a new bump pointer space which we will compact into.
    space::BumpPointerSpace target_space("zygote bump space", non_moving_space_->End(),
                                         non_moving_space_->Limit());
    // Compact the bump pointer space to a new zygote bump pointer space.
    bool reset_main_space = false;
    if (IsMovingGc(collector_type_)) {
      if (collector_type_ == kCollectorTypeCC) {
        zygote_collector.SetFromSpace(region_space_);
      } else {
        zygote_collector.SetFromSpace(bump_pointer_space_);
      }
    } else {
      CHECK(main_space_ != nullptr);
      CHECK_NE(main_space_, non_moving_space_)
          << "Does not make sense to compact within the same space";
      // Copy from the main space.
      zygote_collector.SetFromSpace(main_space_);
      reset_main_space = true;
    }
    zygote_collector.SetToSpace(&target_space);
    zygote_collector.SetSwapSemiSpaces(false);
    zygote_collector.Run(kGcCauseCollectorTransition, false);
    if (reset_main_space) {
      main_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
      madvise(main_space_->Begin(), main_space_->Capacity(), MADV_DONTNEED);
      MemMap mem_map = main_space_->ReleaseMemMap();
      RemoveSpace(main_space_);
      space::Space* old_main_space = main_space_;
      CreateMainMallocSpace(std::move(mem_map),
                            kDefaultInitialSize,
                            std::min(mem_map.Size(), growth_limit_),
                            mem_map.Size());
      delete old_main_space;
      AddSpace(main_space_);
    } else {
      if (collector_type_ == kCollectorTypeCC) {
        region_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
        // Evacuated everything out of the region space, clear the mark bitmap.
        region_space_->GetMarkBitmap()->Clear();
      } else {
        bump_pointer_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
        if (gUseUserfaultfd && use_generational_gc_) {
          MarkCompactCollector()->ResetGenerationalState();
        }
      }
    }
    if (temp_space_ != nullptr) {
      CHECK(temp_space_->IsEmpty());
    }
    IncrementFreedEver();
    // Update the end and write out image.
    non_moving_space_->SetEnd(target_space.End());
    non_moving_space_->SetLimit(target_space.Limit());
    VLOG(heap) << "Create zygote space with size=" << non_moving_space_->Size() << " bytes";
  }
  // Change the collector to the post zygote one.
  ChangeCollector(foreground_collector_type_);
  // Save the old space so that we can remove it after we complete creating the zygote space.
  space::MallocSpace* old_alloc_space = non_moving_space_;
  // Turn the current alloc space into a zygote space and obtain the new alloc space composed of
  // the remaining available space.
  // Remove the old space before creating the zygote space since creating the zygote space sets
  // the old alloc space's bitmaps to null.
  RemoveSpace(old_alloc_space);
  if (collector::SemiSpace::kUseRememberedSet) {
    // Consistency bound check.
    FindRememberedSetFromSpace(old_alloc_space)->AssertAllDirtyCardsAreWithinSpace();
    // Remove the remembered set for the now zygote space (the old
    // non-moving space). Note now that we have compacted objects into
    // the zygote space, the data in the remembered set is no longer
    // needed. The zygote space will instead have a mod-union table
    // from this point on.
    RemoveRememberedSet(old_alloc_space);
  }
  // Remaining space becomes the new non moving space.
  zygote_space_ = old_alloc_space->CreateZygoteSpace(kNonMovingSpaceName, low_memory_mode_,
                                                     &non_moving_space_);
  CHECK(!non_moving_space_->CanMoveObjects());
  if (same_space) {
    main_space_ = non_moving_space_;
    SetSpaceAsDefault(main_space_);
  }
  delete old_alloc_space;
  CHECK(HasZygoteSpace()) << "Failed creating zygote space";
  AddSpace(zygote_space_);
  non_moving_space_->SetFootprintLimit(non_moving_space_->Capacity());
  AddSpace(non_moving_space_);
  constexpr bool set_mark_bit = kUseBakerReadBarrier
                                && gc::collector::ConcurrentCopying::kGrayDirtyImmuneObjects;
  if (set_mark_bit) {
    // Treat all of the objects in the zygote as marked to avoid unnecessary dirty pages. This is
    // safe since we mark all of the objects that may reference non immune objects as gray.
    zygote_space_->SetMarkBitInLiveObjects();
  }

  // Create the zygote space mod union table.
  accounting::ModUnionTable* mod_union_table =
      new accounting::ModUnionTableCardCache("zygote space mod-union table", this, zygote_space_);
  CHECK(mod_union_table != nullptr) << "Failed to create zygote space mod-union table";

  if (collector_type_ != kCollectorTypeCC && collector_type_ != kCollectorTypeCMC) {
    // Set all the cards in the mod-union table since we don't know which objects contain references
    // to large objects.
    mod_union_table->SetCards();
  } else {
    // Make sure to clear the zygote space cards so that we don't dirty pages in the next GC. There
    // may be dirty cards from the zygote compaction or reference processing. These cards are not
    // necessary to have marked since the zygote space may not refer to any objects not in the
    // zygote or image spaces at this point.
    mod_union_table->ProcessCards();
    mod_union_table->ClearTable();

    // For CC and CMC we never collect zygote large objects. This means we do not need to set the
    // cards for the zygote mod-union table and we can also clear all of the existing image
    // mod-union tables. The existing mod-union tables are only for image spaces and may only
    // reference zygote and image objects.
    for (auto& pair : mod_union_tables_) {
      CHECK(pair.first->IsImageSpace());
      CHECK(!pair.first->AsImageSpace()->GetImageHeader().IsAppImage());
      accounting::ModUnionTable* table = pair.second;
      table->ClearTable();
    }
  }
  AddModUnionTable(mod_union_table);
  large_object_space_->SetAllLargeObjectsAsZygoteObjects(self, set_mark_bit);
  if (collector::SemiSpace::kUseRememberedSet) {
    // Add a new remembered set for the post-zygote non-moving space.
    accounting::RememberedSet* post_zygote_non_moving_space_rem_set =
        new accounting::RememberedSet("Post-zygote non-moving space remembered set", this,
                                      non_moving_space_);
    CHECK(post_zygote_non_moving_space_rem_set != nullptr)
        << "Failed to create post-zygote non-moving space remembered set";
    AddRememberedSet(post_zygote_non_moving_space_rem_set);
  }
}
#pragma clang diagnostic pop

void Heap::FlushAllocStack() {
  MarkAllocStackAsLive(allocation_stack_.get());
  allocation_stack_->Reset();
}

void Heap::MarkAllocStack(accounting::ContinuousSpaceBitmap* bitmap1,
                          accounting::ContinuousSpaceBitmap* bitmap2,
                          accounting::LargeObjectBitmap* large_objects,
                          accounting::ObjectStack* stack) {
  DCHECK(bitmap1 != nullptr);
  DCHECK(bitmap2 != nullptr);
  const auto* limit = stack->End();
  for (auto* it = stack->Begin(); it != limit; ++it) {
    const mirror::Object* obj = it->AsMirrorPtr();
    if (obj != nullptr) {
      if (bitmap1->HasAddress(obj)) {
        bitmap1->Set(obj);
      } else if (bitmap2->HasAddress(obj)) {
        bitmap2->Set(obj);
      } else {
        DCHECK(large_objects != nullptr);
        large_objects->Set(obj);
      }
    }
  }
}

void Heap::SwapSemiSpaces() {
  CHECK(bump_pointer_space_ != nullptr);
  CHECK(temp_space_ != nullptr);
  std::swap(bump_pointer_space_, temp_space_);
}

collector::GarbageCollector* Heap::Compact(space::ContinuousMemMapAllocSpace* target_space,
                                           space::ContinuousMemMapAllocSpace* source_space,
                                           GcCause gc_cause) {
  CHECK(kMovingCollector);
  if (target_space != source_space) {
    // Don't swap spaces since this isn't a typical semi space collection.
    semi_space_collector_->SetSwapSemiSpaces(false);
    semi_space_collector_->SetFromSpace(source_space);
    semi_space_collector_->SetToSpace(target_space);
    semi_space_collector_->Run(gc_cause, false);
    return semi_space_collector_;
  }
  LOG(FATAL) << "Unsupported";
  UNREACHABLE();
}

void Heap::TraceHeapSize(size_t heap_size) {
  ATraceIntegerValue("Heap size (KB)", heap_size / KB);
}

#if defined(__GLIBC__)
# define IF_GLIBC(x) x
#else
# define IF_GLIBC(x)
#endif

size_t Heap::GetNativeBytes() {
  size_t malloc_bytes;
#if defined(__BIONIC__) || defined(__GLIBC__) || defined(ANDROID_HOST_MUSL)
  IF_GLIBC(size_t mmapped_bytes;)
  struct mallinfo mi = mallinfo();
  // In spite of the documentation, the jemalloc version of this call seems to do what we want,
  // and it is thread-safe.
  if (sizeof(size_t) > sizeof(mi.uordblks) && sizeof(size_t) > sizeof(mi.hblkhd)) {
    // Shouldn't happen, but glibc declares uordblks as int.
    // Avoiding sign extension gets us correct behavior for another 2 GB.
    malloc_bytes = (unsigned int)mi.uordblks;
    IF_GLIBC(mmapped_bytes = (unsigned int)mi.hblkhd;)
  } else {
    malloc_bytes = mi.uordblks;
    IF_GLIBC(mmapped_bytes = mi.hblkhd;)
  }
  // From the spec, it appeared mmapped_bytes <= malloc_bytes. Reality was sometimes
  // dramatically different. (b/119580449 was an early bug.) If so, we try to fudge it.
  // However, malloc implementations seem to interpret hblkhd differently, namely as
  // mapped blocks backing the entire heap (e.g. jemalloc) vs. large objects directly
  // allocated via mmap (e.g. glibc). Thus we now only do this for glibc, where it
  // previously helped, and which appears to use a reading of the spec compatible
  // with our adjustment.
#if defined(__GLIBC__)
  if (mmapped_bytes > malloc_bytes) {
    malloc_bytes = mmapped_bytes;
  }
#endif  // GLIBC
#else  // Neither Bionic nor Glibc
  // We should hit this case only in contexts in which GC triggering is not critical. Effectively
  // disable GC triggering based on malloc().
  malloc_bytes = 1000;
#endif
  return malloc_bytes + native_bytes_registered_.load(std::memory_order_relaxed);
  // An alternative would be to get RSS from /proc/self/statm. Empirically, that's no
  // more expensive, and it would allow us to count memory allocated by means other than malloc.
  // However it would change as pages are unmapped and remapped due to memory pressure, among
  // other things. It seems risky to trigger GCs as a result of such changes.
}

static inline bool GCNumberLt(uint32_t gc_num1, uint32_t gc_num2) {
  // unsigned comparison, assuming a non-huge difference, but dealing correctly with wrapping.
  uint32_t difference = gc_num2 - gc_num1;
  bool completed_more_than_requested = difference > 0x80000000;
  return difference > 0 && !completed_more_than_requested;
}


collector::GcType Heap::CollectGarbageInternal(collector::GcType gc_type,
                                               GcCause gc_cause,
                                               bool clear_soft_references,
                                               uint32_t requested_gc_num) {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  // If the heap can't run the GC, silently fail and return that no GC was run.
  switch (gc_type) {
    case collector::kGcTypePartial: {
      if (!HasZygoteSpace()) {
        // Do not increment gcs_completed_ . We should retry with kGcTypeFull.
        return collector::kGcTypeNone;
      }
      break;
    }
    default: {
      // Other GC types don't have any special cases which makes them not runnable. The main case
      // here is full GC.
    }
  }
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingPerformingGc);
  Locks::mutator_lock_->AssertNotHeld(self);
  SelfDeletingTask* clear;  // Unconditionally set below.
  {
    // We should not ever become runnable and re-suspend while executing a GC.
    // This would likely cause a deadlock if we acted on a suspension request.
    // TODO: We really want to assert that we don't transition to kRunnable.
    ScopedAssertNoThreadSuspension scoped_assert("Performing GC");
    if (self->IsHandlingStackOverflow<kNativeStackType>()) {
      // If we are throwing a stack overflow error we probably don't have enough remaining stack
      // space to run the GC. Note: we only care if the native stack has overflowed. If the
      // simulated stack overflows it is still possible that the native stack has room to run the
      // GC.

      // Count this as a GC in case someone is waiting for it to complete.
      gcs_completed_.fetch_add(1, std::memory_order_release);
      return collector::kGcTypeNone;
    }
    bool compacting_gc;
    {
      gc_complete_lock_->AssertNotHeld(self);
      // Already not runnable; just switch suspended states. We remain in a suspended state until
      // FinishGC(). This avoids the complicated dance in StartGC().
      ScopedThreadStateChange tsc2(self, ThreadState::kWaitingForGcToComplete);
      MutexLock mu(self, *gc_complete_lock_);
      // Ensure there is only one GC at a time.
      WaitForGcToCompleteLocked(gc_cause, self);
      if (requested_gc_num != GC_NUM_ANY && !GCNumberLt(GetCurrentGcNum(), requested_gc_num)) {
        // The appropriate GC was already triggered elsewhere.
        return collector::kGcTypeNone;
      }
      compacting_gc = IsMovingGc(collector_type_);
      // GC can be disabled if someone has a used GetPrimitiveArrayCritical.
      if (compacting_gc && disable_moving_gc_count_ != 0) {
        LOG(WARNING) << "Skipping GC due to disable moving GC count " << disable_moving_gc_count_;
        // Again count this as a GC.
        gcs_completed_.fetch_add(1, std::memory_order_release);
        return collector::kGcTypeNone;
      }
      if (gc_disabled_for_shutdown_) {
        gcs_completed_.fetch_add(1, std::memory_order_release);
        return collector::kGcTypeNone;
      }
      collector_type_running_ = collector_type_;
      last_gc_cause_ = gc_cause;
    }
    if (gc_cause == kGcCauseForAlloc && runtime->HasStatsEnabled()) {
      ++runtime->GetStats()->gc_for_alloc_count;
      ++self->GetStats()->gc_for_alloc_count;
    }
    const size_t bytes_allocated_before_gc = GetBytesAllocated();

    DCHECK_LT(gc_type, collector::kGcTypeMax);
    DCHECK_NE(gc_type, collector::kGcTypeNone);

    collector::GarbageCollector* collector = nullptr;
    // TODO: Clean this up.
    if (compacting_gc) {
      DCHECK(current_allocator_ == kAllocatorTypeBumpPointer ||
             current_allocator_ == kAllocatorTypeTLAB ||
             current_allocator_ == kAllocatorTypeRegion ||
             current_allocator_ == kAllocatorTypeRegionTLAB);
      switch (collector_type_) {
        case kCollectorTypeSS:
          semi_space_collector_->SetFromSpace(bump_pointer_space_);
          semi_space_collector_->SetToSpace(temp_space_);
          semi_space_collector_->SetSwapSemiSpaces(true);
          collector = semi_space_collector_;
          break;
        case kCollectorTypeCMC:
          collector = mark_compact_;
          if (use_generational_gc_ && gc_type == collector::kGcTypeSticky) {
            collector = young_mark_compact_;
          }
          break;
        case kCollectorTypeCC:
          collector::ConcurrentCopying* active_cc_collector;
          if (use_generational_gc_) {
            // TODO: Other threads must do the flip checkpoint before they start poking at
            // active_concurrent_copying_collector_. So we should not concurrency here.
            active_cc_collector = (gc_type == collector::kGcTypeSticky) ?
                                      young_concurrent_copying_collector_ :
                                      concurrent_copying_collector_;
            active_concurrent_copying_collector_.store(active_cc_collector,
                                                       std::memory_order_relaxed);
            DCHECK(active_cc_collector->RegionSpace() == region_space_);
            collector = active_cc_collector;
          } else {
            collector = active_concurrent_copying_collector_.load(std::memory_order_relaxed);
          }
          break;
        default:
          LOG(FATAL) << "Invalid collector type " << static_cast<size_t>(collector_type_);
      }
      // temp_space_ will be null for kCollectorTypeCMC.
      if (temp_space_ != nullptr &&
          collector != active_concurrent_copying_collector_.load(std::memory_order_relaxed)) {
        temp_space_->GetMemMap()->Protect(PROT_READ | PROT_WRITE);
        if (kIsDebugBuild) {
          // Try to read each page of the memory map in case mprotect didn't work properly
          // b/19894268.
          temp_space_->GetMemMap()->TryReadable();
        }
        CHECK(temp_space_->IsEmpty());
      }
    } else if (current_allocator_ == kAllocatorTypeRosAlloc ||
               current_allocator_ == kAllocatorTypeDlMalloc) {
      collector = FindCollectorByGcType(gc_type);
    } else {
      LOG(FATAL) << "Invalid current allocator " << current_allocator_;
    }

    CHECK(collector != nullptr) << "Could not find garbage collector with collector_type="
                                << static_cast<size_t>(collector_type_)
                                << " and gc_type=" << gc_type;
    collector->Run(gc_cause, clear_soft_references || runtime->IsZygote());
    IncrementFreedEver();
    RequestTrim(self);
    // Collect cleared references.
    clear = reference_processor_->CollectClearedReferences(self);
    // Grow the heap so that we know when to perform the next GC.
    GrowForUtilization(collector, bytes_allocated_before_gc);
    old_native_bytes_allocated_.store(GetNativeBytes());
    LogGC(gc_cause, collector);
    FinishGC(self, gc_type);
    // We're suspended up to this point.
  }
  // Actually enqueue all cleared references. Do this after the GC has officially finished since
  // otherwise we can deadlock.
  clear->Run(self);
  clear->Finalize();
  // Inform DDMS that a GC completed.
  Dbg::GcDidFinish();

  // Unload native libraries for class unloading. We do this after calling FinishGC to prevent
  // deadlocks in case the JNI_OnUnload function does allocations.
  {
    ScopedObjectAccess soa(self);
    soa.Vm()->UnloadNativeLibraries();
  }
  return gc_type;
}

void Heap::LogGC(GcCause gc_cause, collector::GarbageCollector* collector) {
  const size_t duration = GetCurrentGcIteration()->GetDurationNs();
  const std::vector<uint64_t>& pause_times = GetCurrentGcIteration()->GetPauseTimes();
  // Print the GC if it is an explicit GC (e.g. Runtime.gc()) or a slow GC
  // (mutator time blocked >= long_pause_log_threshold_).
  bool log_gc = kLogAllGCs || (gc_cause == kGcCauseExplicit && always_log_explicit_gcs_);
  if (!log_gc && CareAboutPauseTimes()) {
    // GC for alloc pauses the allocating thread, so consider it as a pause.
    log_gc = duration > long_gc_log_threshold_ ||
        (gc_cause == kGcCauseForAlloc && duration > long_pause_log_threshold_);
    for (uint64_t pause : pause_times) {
      log_gc = log_gc || pause >= long_pause_log_threshold_;
    }
  }
  bool is_sampled = false;
  if (UNLIKELY(gc_stress_mode_)) {
    static std::atomic_int64_t accumulated_duration_ns = 0;
    accumulated_duration_ns += duration;
    if (accumulated_duration_ns >= kGcStressModeGcLogSampleFrequencyNs) {
      accumulated_duration_ns -= kGcStressModeGcLogSampleFrequencyNs;
      log_gc = true;
      is_sampled = true;
    }
  }
  if (log_gc) {
    const size_t percent_free = GetPercentFree();
    const size_t current_heap_size = GetBytesAllocated();
    const size_t total_memory = GetTotalMemory();
    std::ostringstream pause_string;
    for (size_t i = 0; i < pause_times.size(); ++i) {
      pause_string << PrettyDuration((pause_times[i] / 1000) * 1000)
                   << ((i != pause_times.size() - 1) ? "," : "");
    }
    LOG(INFO) << gc_cause << " " << collector->GetName()
              << (is_sampled ? " (sampled)" : "")
              << " GC freed "
              << PrettySize(current_gc_iteration_.GetFreedBytes()) << " AllocSpace bytes, "
              << current_gc_iteration_.GetFreedLargeObjects() << "("
              << PrettySize(current_gc_iteration_.GetFreedLargeObjectBytes()) << ") LOS objects, "
              << percent_free << "% free, " << PrettySize(current_heap_size) << "/"
              << PrettySize(total_memory) << ", " << "paused " << pause_string.str()
              << " total " << PrettyDuration((duration / 1000) * 1000);
    VLOG(heap) << Dumpable<TimingLogger>(*current_gc_iteration_.GetTimings());
  }
}

void Heap::FinishGC(Thread* self, collector::GcType gc_type) {
  MutexLock mu(self, *gc_complete_lock_);
  collector_type_running_ = kCollectorTypeNone;
  if (gc_type != collector::kGcTypeNone) {
    last_gc_type_ = gc_type;

    // Update stats.
    ++gc_count_last_window_;
    if (running_collection_is_blocking_) {
      // If the currently running collection was a blocking one,
      // increment the counters and reset the flag.
      ++blocking_gc_count_;
      blocking_gc_time_ += GetCurrentGcIteration()->GetDurationNs();
      ++blocking_gc_count_last_window_;
    }
    // Update the gc count rate histograms if due.
    UpdateGcCountRateHistograms();
  }
  // Reset.
  running_collection_is_blocking_ = false;
  thread_running_gc_ = nullptr;
  if (gc_type != collector::kGcTypeNone) {
    gcs_completed_.fetch_add(1, std::memory_order_release);
  }
  // Wake anyone who may have been waiting for the GC to complete.
  gc_complete_cond_->Broadcast(self);
}

void Heap::UpdateGcCountRateHistograms() {
  // Invariant: if the time since the last update includes more than
  // one windows, all the GC runs (if > 0) must have happened in first
  // window because otherwise the update must have already taken place
  // at an earlier GC run. So, we report the non-first windows with
  // zero counts to the histograms.
  DCHECK_EQ(last_update_time_gc_count_rate_histograms_ % kGcCountRateHistogramWindowDuration, 0U);
  uint64_t now = NanoTime();
  DCHECK_GE(now, last_update_time_gc_count_rate_histograms_);
  uint64_t time_since_last_update = now - last_update_time_gc_count_rate_histograms_;
  uint64_t num_of_windows = time_since_last_update / kGcCountRateHistogramWindowDuration;

  // The computed number of windows can be incoherently high if NanoTime() is not monotonic.
  // Setting a limit on its maximum value reduces the impact on CPU time in such cases.
  if (num_of_windows > kGcCountRateHistogramMaxNumMissedWindows) {
    LOG(WARNING) << "Reducing the number of considered missed Gc histogram windows from "
                 << num_of_windows << " to " << kGcCountRateHistogramMaxNumMissedWindows;
    num_of_windows = kGcCountRateHistogramMaxNumMissedWindows;
  }

  if (time_since_last_update >= kGcCountRateHistogramWindowDuration) {
    // Record the first window.
    gc_count_rate_histogram_.AddValue(gc_count_last_window_ - 1);  // Exclude the current run.
    blocking_gc_count_rate_histogram_.AddValue(running_collection_is_blocking_ ?
        blocking_gc_count_last_window_ - 1 : blocking_gc_count_last_window_);
    // Record the other windows (with zero counts).
    for (uint64_t i = 0; i < num_of_windows - 1; ++i) {
      gc_count_rate_histogram_.AddValue(0);
      blocking_gc_count_rate_histogram_.AddValue(0);
    }
    // Update the last update time and reset the counters.
    last_update_time_gc_count_rate_histograms_ =
        (now / kGcCountRateHistogramWindowDuration) * kGcCountRateHistogramWindowDuration;
    gc_count_last_window_ = 1;  // Include the current run.
    blocking_gc_count_last_window_ = running_collection_is_blocking_ ? 1 : 0;
  }
  DCHECK_EQ(last_update_time_gc_count_rate_histograms_ % kGcCountRateHistogramWindowDuration, 0U);
}

class RootMatchesObjectVisitor : public SingleRootVisitor {
 public:
  explicit RootMatchesObjectVisitor(const mirror::Object* obj) : obj_(obj) { }

  void VisitRoot(mirror::Object* root, const RootInfo& info)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    if (root == obj_) {
      LOG(INFO) << "Object " << obj_ << " is a root " << info.ToString();
    }
  }

 private:
  const mirror::Object* const obj_;
};


class ScanVisitor {
 public:
  void operator()(const mirror::Object* obj) const {
    LOG(ERROR) << "Would have rescanned object " << obj;
  }
};

// Verify a reference from an object.
class VerifyReferenceVisitor : public SingleRootVisitor {
 public:
  VerifyReferenceVisitor(Thread* self, Heap* heap, size_t* fail_count, bool verify_referent)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : self_(self), heap_(heap), fail_count_(fail_count), verify_referent_(verify_referent) {
    CHECK_EQ(self_, Thread::Current());
  }

  void operator()([[maybe_unused]] ObjPtr<mirror::Class> klass, ObjPtr<mirror::Reference> ref) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (verify_referent_) {
      VerifyReference(ref.Ptr(), ref->GetReferent(), mirror::Reference::ReferentOffset());
    }
  }

  void operator()(ObjPtr<mirror::Object> obj,
                  MemberOffset offset,
                  [[maybe_unused]] bool is_static) const REQUIRES_SHARED(Locks::mutator_lock_) {
    VerifyReference(obj.Ptr(), obj->GetFieldObject<mirror::Object>(offset), offset);
  }

  bool IsLive(ObjPtr<mirror::Object> obj) const NO_THREAD_SAFETY_ANALYSIS {
    return heap_->IsLiveObjectLocked(obj, true, false, true);
  }

  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }
  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const_cast<VerifyReferenceVisitor*>(this)->VisitRoot(
        root->AsMirrorPtr(), RootInfo(kRootVMInternal));
  }

  void VisitRoot(mirror::Object* root, const RootInfo& root_info) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (root == nullptr) {
      LOG(ERROR) << "Root is null with info " << root_info.GetType();
    } else if (!VerifyReference(nullptr, root, MemberOffset(0))) {
      LOG(ERROR) << "Root " << root << " is dead with type " << mirror::Object::PrettyTypeOf(root)
          << " thread_id= " << root_info.GetThreadId() << " root_type= " << root_info.GetType();
    }
  }

 private:
  // TODO: Fix the no thread safety analysis.
  // Returns false on failure.
  bool VerifyReference(mirror::Object* obj, mirror::Object* ref, MemberOffset offset) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (ref == nullptr || IsLive(ref)) {
      // Verify that the reference is live.
      return true;
    }
    CHECK_EQ(self_, Thread::Current());  // fail_count_ is private to the calling thread.
    *fail_count_ += 1;
    if (*fail_count_ == 1) {
      // Only print message for the first failure to prevent spam.
      LOG(ERROR) << "!!!!!!!!!!!!!!Heap corruption detected!!!!!!!!!!!!!!!!!!!";
    }
    if (obj != nullptr) {
      // Only do this part for non roots.
      accounting::CardTable* card_table = heap_->GetCardTable();
      accounting::ObjectStack* alloc_stack = heap_->allocation_stack_.get();
      accounting::ObjectStack* live_stack = heap_->live_stack_.get();
      uint8_t* card_addr = card_table->CardFromAddr(obj);
      LOG(ERROR) << "Object " << obj << " references dead object " << ref << " at offset "
                 << offset << "\n card value = " << static_cast<int>(*card_addr);
      if (heap_->IsValidObjectAddress(obj->GetClass())) {
        LOG(ERROR) << "Obj type " << obj->PrettyTypeOf();
      } else {
        LOG(ERROR) << "Object " << obj << " class(" << obj->GetClass() << ") not a heap address";
      }

      // Attempt to find the class inside of the recently freed objects.
      space::ContinuousSpace* ref_space = heap_->FindContinuousSpaceFromObject(ref, true);
      if (ref_space != nullptr && ref_space->IsMallocSpace()) {
        space::MallocSpace* space = ref_space->AsMallocSpace();
        mirror::Class* ref_class = space->FindRecentFreedObject(ref);
        if (ref_class != nullptr) {
          LOG(ERROR) << "Reference " << ref << " found as a recently freed object with class "
                     << ref_class->PrettyClass();
        } else {
          LOG(ERROR) << "Reference " << ref << " not found as a recently freed object";
        }
      }

      if (ref->GetClass() != nullptr && heap_->IsValidObjectAddress(ref->GetClass()) &&
          ref->GetClass()->IsClass()) {
        LOG(ERROR) << "Ref type " << ref->PrettyTypeOf();
      } else {
        LOG(ERROR) << "Ref " << ref << " class(" << ref->GetClass()
                   << ") is not a valid heap address";
      }

      card_table->CheckAddrIsInCardTable(reinterpret_cast<const uint8_t*>(obj));
      void* cover_begin = card_table->AddrFromCard(card_addr);
      void* cover_end = reinterpret_cast<void*>(reinterpret_cast<size_t>(cover_begin) +
          accounting::CardTable::kCardSize);
      LOG(ERROR) << "Card " << reinterpret_cast<void*>(card_addr) << " covers " << cover_begin
          << "-" << cover_end;
      accounting::ContinuousSpaceBitmap* bitmap =
          heap_->GetLiveBitmap()->GetContinuousSpaceBitmap(obj);

      if (bitmap == nullptr) {
        LOG(ERROR) << "Object " << obj << " has no bitmap";
        if (!VerifyClassClass(obj->GetClass())) {
          LOG(ERROR) << "Object " << obj << " failed class verification!";
        }
      } else {
        // Print out how the object is live.
        if (bitmap->Test(obj)) {
          LOG(ERROR) << "Object " << obj << " found in live bitmap";
        }
        if (alloc_stack->Contains(const_cast<mirror::Object*>(obj))) {
          LOG(ERROR) << "Object " << obj << " found in allocation stack";
        }
        if (live_stack->Contains(const_cast<mirror::Object*>(obj))) {
          LOG(ERROR) << "Object " << obj << " found in live stack";
        }
        if (alloc_stack->Contains(const_cast<mirror::Object*>(ref))) {
          LOG(ERROR) << "Ref " << ref << " found in allocation stack";
        }
        if (live_stack->Contains(const_cast<mirror::Object*>(ref))) {
          LOG(ERROR) << "Ref " << ref << " found in live stack";
        }
        // Attempt to see if the card table missed the reference.
        ScanVisitor scan_visitor;
        uint8_t* byte_cover_begin = reinterpret_cast<uint8_t*>(card_table->AddrFromCard(card_addr));
        card_table->Scan<false>(bitmap, byte_cover_begin,
                                byte_cover_begin + accounting::CardTable::kCardSize, scan_visitor);
      }

      // Search to see if any of the roots reference our object.
      RootMatchesObjectVisitor visitor1(obj);
      Runtime::Current()->VisitRoots(&visitor1);
      // Search to see if any of the roots reference our reference.
      RootMatchesObjectVisitor visitor2(ref);
      Runtime::Current()->VisitRoots(&visitor2);
    }
    return false;
  }

  Thread* const self_;
  Heap* const heap_;
  size_t* const fail_count_;
  const bool verify_referent_;
};

// Verify all references within an object, for use with HeapBitmap::Visit.
class VerifyObjectVisitor {
 public:
  VerifyObjectVisitor(Thread* self, Heap* heap, size_t* fail_count, bool verify_referent)
      : self_(self), heap_(heap), fail_count_(fail_count), verify_referent_(verify_referent) {}

  void operator()(mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
    // Note: we are verifying the references in obj but not obj itself, this is because obj must
    // be live or else how did we find it in the live bitmap?
    VerifyReferenceVisitor visitor(self_, heap_, fail_count_, verify_referent_);
    // The class doesn't count as a reference but we should verify it anyways.
    obj->VisitReferences(visitor, visitor);
  }

  void VerifyRoots() REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Locks::heap_bitmap_lock_) {
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    VerifyReferenceVisitor visitor(self_, heap_, fail_count_, verify_referent_);
    Runtime::Current()->VisitRoots(&visitor);
  }

  uint32_t GetFailureCount() const REQUIRES(Locks::mutator_lock_) {
    CHECK_EQ(self_, Thread::Current());
    return *fail_count_;
  }

 private:
  Thread* const self_;
  Heap* const heap_;
  size_t* const fail_count_;
  const bool verify_referent_;
};

void Heap::PushOnAllocationStackWithInternalGC(Thread* self, ObjPtr<mirror::Object>* obj) {
  // Slow path, the allocation stack push back must have already failed.
  DCHECK(!allocation_stack_->AtomicPushBack(obj->Ptr()));
  do {
    // TODO: Add handle VerifyObject.
    StackHandleScope<1> hs(self);
    HandleWrapperObjPtr<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
    // Push our object into the reserve region of the allocation stack. This is only required due
    // to heap verification requiring that roots are live (either in the live bitmap or in the
    // allocation stack).
    CHECK(allocation_stack_->AtomicPushBackIgnoreGrowthLimit(obj->Ptr()));
    CollectGarbageInternal(collector::kGcTypeSticky,
                           kGcCauseForAlloc,
                           false,
                           GetCurrentGcNum() + 1);
  } while (!allocation_stack_->AtomicPushBack(obj->Ptr()));
}

void Heap::PushOnThreadLocalAllocationStackWithInternalGC(Thread* self,
                                                          ObjPtr<mirror::Object>* obj) {
  // Slow path, the allocation stack push back must have already failed.
  DCHECK(!self->PushOnThreadLocalAllocationStack(obj->Ptr()));
  StackReference<mirror::Object>* start_address;
  StackReference<mirror::Object>* end_address;
  while (!allocation_stack_->AtomicBumpBack(kThreadLocalAllocationStackSize, &start_address,
                                            &end_address)) {
    // TODO: Add handle VerifyObject.
    StackHandleScope<1> hs(self);
    HandleWrapperObjPtr<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
    // Push our object into the reserve region of the allocaiton stack. This is only required due
    // to heap verification requiring that roots are live (either in the live bitmap or in the
    // allocation stack).
    CHECK(allocation_stack_->AtomicPushBackIgnoreGrowthLimit(obj->Ptr()));
    // Push into the reserve allocation stack.
    CollectGarbageInternal(collector::kGcTypeSticky,
                           kGcCauseForAlloc,
                           false,
                           GetCurrentGcNum() + 1);
  }
  self->SetThreadLocalAllocationStack(start_address, end_address);
  // Retry on the new thread-local allocation stack.
  CHECK(self->PushOnThreadLocalAllocationStack(obj->Ptr()));  // Must succeed.
}

// Must do this with mutators suspended since we are directly accessing the allocation stacks.
size_t Heap::VerifyHeapReferences(bool verify_referents) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  // Lets sort our allocation stacks so that we can efficiently binary search them.
  allocation_stack_->Sort();
  live_stack_->Sort();
  // Since we sorted the allocation stack content, need to revoke all
  // thread-local allocation stacks.
  RevokeAllThreadLocalAllocationStacks(self);
  size_t fail_count = 0;
  VerifyObjectVisitor visitor(self, this, &fail_count, verify_referents);
  // Verify objects in the allocation stack since these will be objects which were:
  // 1. Allocated prior to the GC (pre GC verification).
  // 2. Allocated during the GC (pre sweep GC verification).
  // We don't want to verify the objects in the live stack since they themselves may be
  // pointing to dead objects if they are not reachable.
  VisitObjectsPaused(visitor);
  // Verify the roots:
  visitor.VerifyRoots();
  if (visitor.GetFailureCount() > 0) {
    // Dump mod-union tables.
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      mod_union_table->Dump(LOG_STREAM(ERROR) << mod_union_table->GetName() << ": ");
    }
    // Dump remembered sets.
    for (const auto& table_pair : remembered_sets_) {
      accounting::RememberedSet* remembered_set = table_pair.second;
      remembered_set->Dump(LOG_STREAM(ERROR) << remembered_set->GetName() << ": ");
    }
    DumpSpaces(LOG_STREAM(ERROR));
  }
  return visitor.GetFailureCount();
}

class VerifyReferenceCardVisitor {
 public:
  VerifyReferenceCardVisitor(Heap* heap, bool* failed)
      REQUIRES_SHARED(Locks::mutator_lock_,
                            Locks::heap_bitmap_lock_)
      : heap_(heap), failed_(failed) {
  }

  // There is no card marks for native roots on a class.
  void VisitRootIfNonNull(
      [[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}
  void VisitRoot([[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}

  // TODO: Fix lock analysis to not use NO_THREAD_SAFETY_ANALYSIS, requires support for
  // annotalysis on visitors.
  void operator()(mirror::Object* obj, MemberOffset offset, bool is_static) const
      NO_THREAD_SAFETY_ANALYSIS {
    mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
    // Filter out class references since changing an object's class does not mark the card as dirty.
    // Also handles large objects, since the only reference they hold is a class reference.
    if (ref != nullptr && !ref->IsClass()) {
      accounting::CardTable* card_table = heap_->GetCardTable();
      // If the object is not dirty and it is referencing something in the live stack other than
      // class, then it must be on a dirty card.
      if (!card_table->AddrIsInCardTable(obj)) {
        LOG(ERROR) << "Object " << obj << " is not in the address range of the card table";
        *failed_ = true;
      } else if (!card_table->IsDirty(obj)) {
        // TODO: Check mod-union tables.
        // Card should be either kCardDirty if it got re-dirtied after we aged it, or
        // kCardDirty - 1 if it didnt get touched since we aged it.
        accounting::ObjectStack* live_stack = heap_->live_stack_.get();
        if (live_stack->ContainsSorted(ref)) {
          if (live_stack->ContainsSorted(obj)) {
            LOG(ERROR) << "Object " << obj << " found in live stack";
          }
          if (heap_->GetLiveBitmap()->Test(obj)) {
            LOG(ERROR) << "Object " << obj << " found in live bitmap";
          }
          LOG(ERROR) << "Object " << obj << " " << mirror::Object::PrettyTypeOf(obj)
                    << " references " << ref << " " << mirror::Object::PrettyTypeOf(ref)
                    << " in live stack";

          // Print which field of the object is dead.
          if (!obj->IsObjectArray()) {
            ObjPtr<mirror::Class> klass = is_static ? obj->AsClass() : obj->GetClass();
            CHECK(klass != nullptr);
            for (ArtField& field : klass->GetFields()) {
              if (is_static == field.IsStatic() &&
                  field.GetOffset().Int32Value() == offset.Int32Value()) {
                LOG(ERROR) << (is_static ? "Static " : "") << "field in the live stack is "
                           << field.PrettyField();
                break;
              }
            }
          } else {
            ObjPtr<mirror::ObjectArray<mirror::Object>> object_array =
                obj->AsObjectArray<mirror::Object>();
            for (int32_t i = 0; i < object_array->GetLength(); ++i) {
              if (object_array->Get(i) == ref) {
                LOG(ERROR) << (is_static ? "Static " : "") << "obj[" << i << "] = ref";
              }
            }
          }

          *failed_ = true;
        }
      }
    }
  }

 private:
  Heap* const heap_;
  bool* const failed_;
};

class VerifyLiveStackReferences {
 public:
  explicit VerifyLiveStackReferences(Heap* heap)
      : heap_(heap),
        failed_(false) {}

  void operator()(mirror::Object* obj) const
      REQUIRES_SHARED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    VerifyReferenceCardVisitor visitor(heap_, const_cast<bool*>(&failed_));
    obj->VisitReferences(visitor, VoidFunctor());
  }

  bool Failed() const {
    return failed_;
  }

 private:
  Heap* const heap_;
  bool failed_;
};

bool Heap::VerifyMissingCardMarks() {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  // We need to sort the live stack since we binary search it.
  live_stack_->Sort();
  // Since we sorted the allocation stack content, need to revoke all
  // thread-local allocation stacks.
  RevokeAllThreadLocalAllocationStacks(self);
  VerifyLiveStackReferences visitor(this);
  GetLiveBitmap()->Visit(visitor);
  // We can verify objects in the live stack since none of these should reference dead objects.
  for (auto* it = live_stack_->Begin(); it != live_stack_->End(); ++it) {
    if (!kUseThreadLocalAllocationStack || it->AsMirrorPtr() != nullptr) {
      visitor(it->AsMirrorPtr());
    }
  }
  return !visitor.Failed();
}

void Heap::SwapStacks() {
  if (kUseThreadLocalAllocationStack) {
    live_stack_->AssertAllZero();
  }
  allocation_stack_.swap(live_stack_);
}

void Heap::RevokeAllThreadLocalAllocationStacks(Thread* self) {
  // This must be called only during the pause.
  DCHECK(Locks::mutator_lock_->IsExclusiveHeld(self));
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* t : thread_list) {
    t->RevokeThreadLocalAllocationStack();
  }
}

void Heap::AssertThreadLocalBuffersAreRevoked(Thread* thread) {
  if (kIsDebugBuild) {
    if (rosalloc_space_ != nullptr) {
      rosalloc_space_->AssertThreadLocalBuffersAreRevoked(thread);
    }
    if (bump_pointer_space_ != nullptr) {
      bump_pointer_space_->AssertThreadLocalBuffersAreRevoked(thread);
    }
  }
}

void Heap::AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked() {
  if (kIsDebugBuild) {
    if (bump_pointer_space_ != nullptr) {
      bump_pointer_space_->AssertAllThreadLocalBuffersAreRevoked();
    }
  }
}

accounting::ModUnionTable* Heap::FindModUnionTableFromSpace(space::Space* space) {
  auto it = mod_union_tables_.find(space);
  if (it == mod_union_tables_.end()) {
    return nullptr;
  }
  return it->second;
}

accounting::RememberedSet* Heap::FindRememberedSetFromSpace(space::Space* space) {
  auto it = remembered_sets_.find(space);
  if (it == remembered_sets_.end()) {
    return nullptr;
  }
  return it->second;
}

void Heap::ProcessCards(TimingLogger* timings,
                        bool use_rem_sets,
                        bool process_alloc_space_cards,
                        bool clear_alloc_space_cards) {
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  // Clear cards and keep track of cards cleared in the mod-union table.
  for (const auto& space : continuous_spaces_) {
    accounting::ModUnionTable* table = FindModUnionTableFromSpace(space);
    accounting::RememberedSet* rem_set = FindRememberedSetFromSpace(space);
    if (table != nullptr) {
      const char* name = space->IsZygoteSpace() ? "ZygoteModUnionClearCards" :
          "ImageModUnionClearCards";
      TimingLogger::ScopedTiming t2(name, timings);
      table->ProcessCards();
    } else if (use_rem_sets && rem_set != nullptr) {
      DCHECK(collector::SemiSpace::kUseRememberedSet) << static_cast<int>(collector_type_);
      TimingLogger::ScopedTiming t2("AllocSpaceRemSetClearCards", timings);
      rem_set->ClearCards();
    } else if (process_alloc_space_cards) {
      TimingLogger::ScopedTiming t2("AllocSpaceClearCards", timings);
      if (clear_alloc_space_cards) {
        uint8_t* end = space->End();
        if (space->IsImageSpace()) {
          // Image space end is the end of the mirror objects, it is not necessarily page or card
          // aligned. Align up so that the check in ClearCardRange does not fail.
          end = AlignUp(end, accounting::CardTable::kCardSize);
        }
        card_table_->ClearCardRange(space->Begin(), end);
      } else {
        // No mod union table for the AllocSpace. Age the cards so that the GC knows that these
        // cards were dirty before the GC started.
        // TODO: Need to use atomic for the case where aged(cleaning thread) -> dirty(other thread)
        // -> clean(cleaning thread).
        // The races are we either end up with: Aged card, unaged card. Since we have the
        // checkpoint roots and then we scan / update mod union tables after. We will always
        // scan either card. If we end up with the non aged card, we scan it it in the pause.
        card_table_->ModifyCardsAtomic(space->Begin(), space->End(), AgeCardVisitor(),
                                       VoidFunctor());
      }
    }
  }
}

struct IdentityMarkHeapReferenceVisitor : public MarkObjectVisitor {
  mirror::Object* MarkObject(mirror::Object* obj) override {
    return obj;
  }
  void MarkHeapReference(mirror::HeapReference<mirror::Object>*, bool) override {
  }
};

void Heap::PreGcVerificationPaused(collector::GarbageCollector* gc) {
  Thread* const self = Thread::Current();
  TimingLogger* const timings = current_gc_iteration_.GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  if (verify_pre_gc_heap_) {
    TimingLogger::ScopedTiming t2("(Paused)PreGcVerifyHeapReferences", timings);
    size_t failures = VerifyHeapReferences();
    if (failures > 0) {
      LOG(FATAL) << "Pre " << gc->GetName() << " heap verification failed with " << failures
          << " failures";
    }
  }
  // Check that all objects which reference things in the live stack are on dirty cards.
  if (verify_missing_card_marks_) {
    TimingLogger::ScopedTiming t2("(Paused)PreGcVerifyMissingCardMarks", timings);
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    SwapStacks();
    // Sort the live stack so that we can quickly binary search it later.
    CHECK(VerifyMissingCardMarks()) << "Pre " << gc->GetName()
                                    << " missing card mark verification failed\n" << DumpSpaces();
    SwapStacks();
  }
  if (verify_mod_union_table_) {
    TimingLogger::ScopedTiming t2("(Paused)PreGcVerifyModUnionTables", timings);
    ReaderMutexLock reader_lock(self, *Locks::heap_bitmap_lock_);
    for (const auto& table_pair : mod_union_tables_) {
      accounting::ModUnionTable* mod_union_table = table_pair.second;
      IdentityMarkHeapReferenceVisitor visitor;
      mod_union_table->UpdateAndMarkReferences(&visitor);
      mod_union_table->Verify();
    }
  }
}

void Heap::PreGcVerification(collector::GarbageCollector* gc) {
  if (verify_pre_gc_heap_ || verify_missing_card_marks_ || verify_mod_union_table_) {
    collector::GarbageCollector::ScopedPause pause(gc, false);
    PreGcVerificationPaused(gc);
  }
}

void Heap::PrePauseRosAllocVerification([[maybe_unused]] collector::GarbageCollector* gc) {
  // TODO: Add a new runtime option for this?
  if (verify_pre_gc_rosalloc_) {
    RosAllocVerification(current_gc_iteration_.GetTimings(), "PreGcRosAllocVerification");
  }
}

void Heap::PreSweepingGcVerification(collector::GarbageCollector* gc) {
  Thread* const self = Thread::Current();
  TimingLogger* const timings = current_gc_iteration_.GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  // Called before sweeping occurs since we want to make sure we are not going so reclaim any
  // reachable objects.
  if (verify_pre_sweeping_heap_) {
    TimingLogger::ScopedTiming t2("(Paused)PostSweepingVerifyHeapReferences", timings);
    CHECK_NE(self->GetState(), ThreadState::kRunnable);
    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      // Swapping bound bitmaps does nothing.
      gc->SwapBitmaps();
    }
    // Pass in false since concurrent reference processing can mean that the reference referents
    // may point to dead objects at the point which PreSweepingGcVerification is called.
    size_t failures = VerifyHeapReferences(false);
    if (failures > 0) {
      LOG(FATAL) << "Pre sweeping " << gc->GetName() << " GC verification failed with " << failures
          << " failures";
    }
    {
      WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
      gc->SwapBitmaps();
    }
  }
  if (verify_pre_sweeping_rosalloc_) {
    RosAllocVerification(timings, "PreSweepingRosAllocVerification");
  }
}

void Heap::PostGcVerificationPaused(collector::GarbageCollector* gc) {
  // Only pause if we have to do some verification.
  Thread* const self = Thread::Current();
  TimingLogger* const timings = GetCurrentGcIteration()->GetTimings();
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  if (verify_system_weaks_) {
    ReaderMutexLock mu2(self, *Locks::heap_bitmap_lock_);
    collector::MarkSweep* mark_sweep = down_cast<collector::MarkSweep*>(gc);
    mark_sweep->VerifySystemWeaks();
  }
  if (verify_post_gc_rosalloc_) {
    RosAllocVerification(timings, "(Paused)PostGcRosAllocVerification");
  }
  if (verify_post_gc_heap_) {
    TimingLogger::ScopedTiming t2("(Paused)PostGcVerifyHeapReferences", timings);
    size_t failures = VerifyHeapReferences();
    if (failures > 0) {
      LOG(FATAL) << "Pre " << gc->GetName() << " heap verification failed with " << failures
          << " failures";
    }
  }
}

void Heap::PostGcVerification(collector::GarbageCollector* gc) {
  if (verify_system_weaks_ || verify_post_gc_rosalloc_ || verify_post_gc_heap_) {
    collector::GarbageCollector::ScopedPause pause(gc, false);
    PostGcVerificationPaused(gc);
  }
}

void Heap::RosAllocVerification(TimingLogger* timings, const char* name) {
  TimingLogger::ScopedTiming t(name, timings);
  for (const auto& space : continuous_spaces_) {
    if (space->IsRosAllocSpace()) {
      VLOG(heap) << name << " : " << space->GetName();
      space->AsRosAllocSpace()->Verify();
    }
  }
}

collector::GcType Heap::WaitForGcToComplete(GcCause cause, Thread* self) {
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingForGcToComplete);
  MutexLock mu(self, *gc_complete_lock_);
  return WaitForGcToCompleteLocked(cause, self, /* only_one= */ true);
}

collector::GcType Heap::WaitForGcToCompleteLocked(GcCause cause, Thread* self, bool only_one) {
  gc_complete_cond_->CheckSafeToWait(self);
  collector::GcType last_gc_type = collector::kGcTypeNone;
  GcCause last_gc_cause = kGcCauseNone;
  if (collector_type_running_ != kCollectorTypeNone) {
    uint64_t wait_start = NanoTime();
    uint32_t starting_gc_num = GetCurrentGcNum();
    while (collector_type_running_ != kCollectorTypeNone &&
           (!only_one || GCNumberLt(GetCurrentGcNum(), starting_gc_num + 1))) {
      if (!task_processor_->IsRunningThread(self)) {
        // The current thread is about to wait for a currently running
        // collection to finish. If the waiting thread is not the heap
        // task daemon thread, the currently running collection is
        // considered as a blocking GC.
        running_collection_is_blocking_ = true;
        VLOG(gc) << "Waiting for a blocking GC " << cause;
      }
      SCOPED_TRACE << "GC: Wait For Completion " << cause;
      // We must wait, change thread state then sleep on gc_complete_cond_;
      gc_complete_cond_->Wait(self);
      last_gc_type = last_gc_type_;
      last_gc_cause = last_gc_cause_;
    }
    uint64_t wait_time = NanoTime() - wait_start;
    total_wait_time_ += wait_time;
    if (wait_time > long_pause_log_threshold_) {
      LOG(INFO) << "WaitForGcToComplete blocked " << cause << " on " << last_gc_cause << " for "
                << PrettyDuration(wait_time);
    }
  }
  if (!task_processor_->IsRunningThread(self)) {
    // The current thread is about to run a collection. If the thread
    // is not the heap task daemon thread, it's considered as a
    // blocking GC (i.e., blocking itself).
    running_collection_is_blocking_ = true;
  }
  DCHECK(only_one || collector_type_running_ == kCollectorTypeNone);
  return last_gc_type;
}

void Heap::DumpForSigQuit(std::ostream& os) {
  os << "Heap: " << GetPercentFree() << "% free, " << PrettySize(GetBytesAllocated()) << "/"
     << PrettySize(GetTotalMemory()) << "\n";
  {
    os << "Image spaces:\n";
    ScopedObjectAccess soa(Thread::Current());
    for (const auto& space : continuous_spaces_) {
      if (space->IsImageSpace()) {
        os << space->GetName() << "\n";
      }
    }
  }
  DumpGcPerformanceInfo(os);
}

size_t Heap::GetPercentFree() {
  return static_cast<size_t>(100.0f * static_cast<float>(
      GetFreeMemory()) / target_footprint_.load(std::memory_order_relaxed));
}

void Heap::SetIdealFootprint(size_t target_footprint) {
  if (target_footprint > GetMaxMemory()) {
    VLOG(gc) << "Clamp target GC heap from " << PrettySize(target_footprint) << " to "
             << PrettySize(GetMaxMemory());
    target_footprint = GetMaxMemory();
  }
  target_footprint_.store(target_footprint, std::memory_order_relaxed);
}

bool Heap::IsMovableObject(ObjPtr<mirror::Object> obj) const {
  if (kMovingCollector) {
    space::Space* space = FindContinuousSpaceFromObject(obj.Ptr(), true);
    if (space != nullptr) {
      // TODO: Check large object?
      return space->CanMoveObjects();
    }
  }
  return false;
}

collector::GarbageCollector* Heap::FindCollectorByGcType(collector::GcType gc_type) {
  for (auto* collector : garbage_collectors_) {
    if (collector->GetCollectorType() == collector_type_ &&
        collector->GetGcType() == gc_type) {
      return collector;
    }
  }
  return nullptr;
}

double Heap::HeapGrowthMultiplier() const {
  // If we don't care about pause times we are background, so return 1.0.
  if (!CareAboutPauseTimes()) {
    return 1.0;
  }
  return foreground_heap_growth_multiplier_;
}

void Heap::GrowForUtilization(collector::GarbageCollector* collector_ran,
                              size_t bytes_allocated_before_gc) {
  // We're running in the thread that set collector_type_running_ to something other than none,
  // thus ensuring that there is only one of us running. Thus
  // collector_type_running_ != kCollectorTypeNone, but that's a little tricky to turn into a
  // DCHECK.

  // We know what our utilization is at this moment.
  // This doesn't actually resize any memory. It just lets the heap grow more when necessary.
  const size_t bytes_allocated = GetBytesAllocated();
  // Trace the new heap size after the GC is finished.
  TraceHeapSize(bytes_allocated);
  uint64_t target_size, grow_bytes;
  collector::GcType gc_type = collector_ran->GetGcType();
  MutexLock mu(Thread::Current(), process_state_update_lock_);
  // Use the multiplier to grow more for foreground.
  const double multiplier = HeapGrowthMultiplier();
  if (gc_type != collector::kGcTypeSticky) {
    // Grow the heap for non sticky GC.
    uint64_t delta = bytes_allocated * (1.0 / GetTargetHeapUtilization() - 1.0);
    DCHECK_LE(delta, std::numeric_limits<size_t>::max()) << "bytes_allocated=" << bytes_allocated
        << " target_utilization_=" << target_utilization_;
    grow_bytes = std::min(delta, static_cast<uint64_t>(max_free_));
    grow_bytes = std::max(grow_bytes, static_cast<uint64_t>(min_free_));
    target_size = bytes_allocated + static_cast<uint64_t>(grow_bytes * multiplier);
    next_gc_type_ = collector::kGcTypeSticky;
  } else {
    collector::GcType non_sticky_gc_type = NonStickyGcType();
    // Find what the next non sticky collector will be.
    collector::GarbageCollector* non_sticky_collector = FindCollectorByGcType(non_sticky_gc_type);
    if (use_generational_gc_) {
      if (non_sticky_collector == nullptr) {
        non_sticky_collector = FindCollectorByGcType(collector::kGcTypePartial);
      }
      CHECK(non_sticky_collector != nullptr);
    }
    double sticky_gc_throughput_adjustment = GetStickyGcThroughputAdjustment(use_generational_gc_);

    // If the throughput of the current sticky GC >= throughput of the non sticky collector, then
    // do another sticky collection next.
    // We also check that the bytes allocated aren't over the target_footprint, or
    // concurrent_start_bytes in case of concurrent GCs, in order to prevent a
    // pathological case where dead objects which aren't reclaimed by sticky could get accumulated
    // if the sticky GC throughput always remained >= the full/partial throughput.
    size_t target_footprint = target_footprint_.load(std::memory_order_relaxed);
    if (current_gc_iteration_.GetEstimatedThroughput() * sticky_gc_throughput_adjustment >=
        non_sticky_collector->GetEstimatedMeanThroughput() &&
        non_sticky_collector->NumberOfIterations() > 0 &&
        bytes_allocated <= (IsGcConcurrent() ? concurrent_start_bytes_ : target_footprint)) {
      next_gc_type_ = collector::kGcTypeSticky;
    } else {
      next_gc_type_ = non_sticky_gc_type;
    }
    // If we have freed enough memory, shrink the heap back down.
    const size_t adjusted_max_free = static_cast<size_t>(max_free_ * multiplier);
    if (bytes_allocated + adjusted_max_free < target_footprint) {
      target_size = bytes_allocated + adjusted_max_free;
      grow_bytes = max_free_;
    } else {
      target_size = std::max(bytes_allocated, target_footprint);
      // The same whether jank perceptible or not; just avoid the adjustment.
      grow_bytes = 0;
    }
  }
  CHECK_LE(target_size, std::numeric_limits<size_t>::max())
      << " bytes_allocated:" << bytes_allocated
      << " bytes_freed:" << current_gc_iteration_.GetFreedBytes()
      << " large_obj_bytes_freed:" << current_gc_iteration_.GetFreedLargeObjectBytes();
  if (!ignore_target_footprint_) {
    SetIdealFootprint(target_size);
    // Store target size (computed with foreground heap growth multiplier) for updating
    // target_footprint_ when process state switches to foreground.
    // target_size = 0 ensures that target_footprint_ is not updated on
    // process-state switch.
    min_foreground_target_footprint_ =
        (multiplier <= 1.0 && grow_bytes > 0)
        ? std::min(
          bytes_allocated + static_cast<size_t>(grow_bytes * foreground_heap_growth_multiplier_),
          GetMaxMemory())
        : 0;

    if (IsGcConcurrent()) {
      const uint64_t freed_bytes = current_gc_iteration_.GetFreedBytes() +
          current_gc_iteration_.GetFreedLargeObjectBytes() +
          current_gc_iteration_.GetFreedRevokeBytes();
      // Records the number of bytes allocated at the time of GC finish,excluding the number of
      // bytes allocated during GC.
      num_bytes_alive_after_gc_ = UnsignedDifference(bytes_allocated_before_gc, freed_bytes);
      // Bytes allocated will shrink by freed_bytes after the GC runs, so if we want to figure out
      // how many bytes were allocated during the GC we need to add freed_bytes back on.
      // Almost always bytes_allocated + freed_bytes >= bytes_allocated_before_gc.
      const size_t bytes_allocated_during_gc =
          UnsignedDifference(bytes_allocated + freed_bytes, bytes_allocated_before_gc);
      // Calculate when to perform the next ConcurrentGC.
      // Estimate how many remaining bytes we will have when we need to start the next GC.
      size_t remaining_bytes = bytes_allocated_during_gc;
      remaining_bytes = std::min(remaining_bytes, kMaxConcurrentRemainingBytes);
      remaining_bytes = std::max(remaining_bytes, kMinConcurrentRemainingBytes);
      size_t target_footprint = target_footprint_.load(std::memory_order_relaxed);
      if (UNLIKELY(remaining_bytes > target_footprint)) {
        // A never going to happen situation that from the estimated allocation rate we will exceed
        // the applications entire footprint with the given estimated allocation rate. Schedule
        // another GC nearly straight away.
        remaining_bytes = std::min(kMinConcurrentRemainingBytes, target_footprint);
      }
      DCHECK_LE(target_footprint_.load(std::memory_order_relaxed), GetMaxMemory());
      // Start a concurrent GC when we get close to the estimated remaining bytes. When the
      // allocation rate is very high, remaining_bytes could tell us that we should start a GC
      // right away.
      concurrent_start_bytes_ = std::max(target_footprint - remaining_bytes, bytes_allocated);
      // Store concurrent_start_bytes_ (computed with foreground heap growth multiplier) for update
      // itself when process state switches to foreground.
      min_foreground_concurrent_start_bytes_ =
          min_foreground_target_footprint_ != 0
          ? std::max(min_foreground_target_footprint_ - remaining_bytes, bytes_allocated)
          : 0;
    }
  }
}

void Heap::ClampGrowthLimit() {
  // Use heap bitmap lock to guard against races with BindLiveToMarkBitmap.
  ScopedObjectAccess soa(Thread::Current());
  WriterMutexLock mu(soa.Self(), *Locks::heap_bitmap_lock_);
  capacity_ = growth_limit_;
  for (const auto& space : continuous_spaces_) {
    if (space->IsMallocSpace()) {
      gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
      malloc_space->ClampGrowthLimit();
    }
  }
  if (large_object_space_ != nullptr) {
    large_object_space_->ClampGrowthLimit(capacity_);
  }
  if (collector_type_ == kCollectorTypeCC) {
    DCHECK(region_space_ != nullptr);
    // Twice the capacity as CC needs extra space for evacuating objects.
    region_space_->ClampGrowthLimit(2 * capacity_);
  } else if (collector_type_ == kCollectorTypeCMC) {
    DCHECK(gUseUserfaultfd);
    DCHECK_NE(mark_compact_, nullptr);
    DCHECK_NE(bump_pointer_space_, nullptr);
    mark_compact_->ClampGrowthLimit(capacity_);
  }
  // This space isn't added for performance reasons.
  if (main_space_backup_.get() != nullptr) {
    main_space_backup_->ClampGrowthLimit();
  }
}

void Heap::ClearGrowthLimit() {
  if (target_footprint_.load(std::memory_order_relaxed) == growth_limit_
      && growth_limit_ < capacity_) {
    target_footprint_.store(capacity_, std::memory_order_relaxed);
    SetDefaultConcurrentStartBytes();
  }
  growth_limit_ = capacity_;
  ScopedObjectAccess soa(Thread::Current());
  for (const auto& space : continuous_spaces_) {
    if (space->IsMallocSpace()) {
      gc::space::MallocSpace* malloc_space = space->AsMallocSpace();
      malloc_space->ClearGrowthLimit();
      malloc_space->SetFootprintLimit(malloc_space->Capacity());
    }
  }
  // This space isn't added for performance reasons.
  if (main_space_backup_.get() != nullptr) {
    main_space_backup_->ClearGrowthLimit();
    main_space_backup_->SetFootprintLimit(main_space_backup_->Capacity());
  }
}

void Heap::AddFinalizerReference(Thread* self, ObjPtr<mirror::Object>* object) {
  ScopedObjectAccess soa(self);
  StackHandleScope<1u> hs(self);
  // Use handle wrapper to update the `*object` if the object gets moved.
  HandleWrapperObjPtr<mirror::Object> h_object = hs.NewHandleWrapper(object);
  WellKnownClasses::java_lang_ref_FinalizerReference_add->InvokeStatic<'V', 'L'>(
      self, h_object.Get());
}

void Heap::RequestConcurrentGCAndSaveObject(Thread* self,
                                            bool force_full,
                                            uint32_t observed_gc_num,
                                            ObjPtr<mirror::Object>* obj) {
  StackHandleScope<1> hs(self);
  HandleWrapperObjPtr<mirror::Object> wrapper(hs.NewHandleWrapper(obj));
  RequestConcurrentGC(self, kGcCauseBackground, force_full, observed_gc_num);
}

class Heap::ConcurrentGCTask : public HeapTask {
 public:
  ConcurrentGCTask(uint64_t target_time, GcCause cause, bool force_full, uint32_t gc_num)
      : HeapTask(target_time), cause_(cause), force_full_(force_full), my_gc_num_(gc_num) {}
  void Run(Thread* self) override {
    Runtime* runtime = Runtime::Current();
    gc::Heap* heap = runtime->GetHeap();
    DCHECK(GCNumberLt(my_gc_num_, heap->GetCurrentGcNum() + 2));  // <= current_gc_num + 1
    heap->ConcurrentGC(self, cause_, force_full_, my_gc_num_);
    CHECK_IMPLIES(GCNumberLt(heap->GetCurrentGcNum(), my_gc_num_), runtime->IsShuttingDown(self));
  }

 private:
  const GcCause cause_;
  const bool force_full_;  // If true, force full (or partial) collection.
  const uint32_t my_gc_num_;  // Sequence number of requested GC.
};

static bool CanAddHeapTask(Thread* self) REQUIRES(!Locks::runtime_shutdown_lock_) {
  Runtime* runtime = Runtime::Current();
  // We only care if the native stack has overflowed. If the simulated stack overflows, it is still
  // possible that the native stack has room to add a heap task.
  return runtime != nullptr && runtime->IsFinishedStarting() && !runtime->IsShuttingDown(self) &&
      !self->IsHandlingStackOverflow<kNativeStackType>();
}

bool Heap::RequestConcurrentGC(Thread* self,
                               GcCause cause,
                               bool force_full,
                               uint32_t observed_gc_num) {
  uint32_t max_gc_requested = max_gc_requested_.load(std::memory_order_relaxed);
  if (!GCNumberLt(observed_gc_num, max_gc_requested)) {
    // observed_gc_num >= max_gc_requested: Nobody beat us to requesting the next gc.
    if (CanAddHeapTask(self)) {
      // Since observed_gc_num >= max_gc_requested, this increases max_gc_requested_, if successful.
      if (max_gc_requested_.CompareAndSetStrongRelaxed(max_gc_requested, observed_gc_num + 1)) {
        task_processor_->AddTask(self, new ConcurrentGCTask(NanoTime(),  // Start straight away.
                                                            cause,
                                                            force_full,
                                                            observed_gc_num + 1));
      }
      DCHECK(GCNumberLt(observed_gc_num, max_gc_requested_.load(std::memory_order_relaxed)));
      // If we increased max_gc_requested_, then we added a task that will eventually cause
      // gcs_completed_ to be incremented (to at least observed_gc_num + 1).
      // If the CAS failed, somebody else did.
      return true;
    }
    return false;
  }
  return true;  // Vacuously.
}

void Heap::ConcurrentGC(Thread* self, GcCause cause, bool force_full, uint32_t requested_gc_num) {
  if (!Runtime::Current()->IsShuttingDown(self)) {
    // Wait for any GCs currently running to finish. If this incremented GC number, we're done.
    WaitForGcToComplete(cause, self);
    if (GCNumberLt(GetCurrentGcNum(), requested_gc_num)) {
      collector::GcType next_gc_type = next_gc_type_;
      // If forcing full and next gc type is sticky, override with a non-sticky type.
      if (force_full && next_gc_type == collector::kGcTypeSticky) {
        next_gc_type = NonStickyGcType();
      }
      // If we can't run the GC type we wanted to run, find the next appropriate one and try
      // that instead. E.g. can't do partial, so do full instead.
      // We must ensure that we run something that ends up incrementing gcs_completed_.
      // In the kGcTypePartial case, the initial CollectGarbageInternal call may not have that
      // effect, but the subsequent KGcTypeFull call will.
      if (CollectGarbageInternal(next_gc_type, cause, false, requested_gc_num)
          == collector::kGcTypeNone) {
        for (collector::GcType gc_type : gc_plan_) {
          if (!GCNumberLt(GetCurrentGcNum(), requested_gc_num)) {
            // Somebody did it for us.
            break;
          }
          // Attempt to run the collector, if we succeed, we are done.
          if (gc_type > next_gc_type &&
              CollectGarbageInternal(gc_type, cause, false, requested_gc_num)
              != collector::kGcTypeNone) {
            break;
          }
        }
      }
    }
  }
}

class Heap::CollectorTransitionTask : public HeapTask {
 public:
  explicit CollectorTransitionTask(uint64_t target_time) : HeapTask(target_time) {}

  void Run(Thread* self) override {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    heap->DoPendingCollectorTransition();
    heap->ClearPendingCollectorTransition(self);
  }
};

void Heap::ClearPendingCollectorTransition(Thread* self) {
  MutexLock mu(self, *pending_task_lock_);
  pending_collector_transition_ = nullptr;
}

void Heap::RequestCollectorTransition(CollectorType desired_collector_type, uint64_t delta_time) {
  Thread* self = Thread::Current();
  desired_collector_type_ = desired_collector_type;
  if (desired_collector_type_ == collector_type_ || !CanAddHeapTask(self)) {
    return;
  }
  if (collector_type_ == kCollectorTypeCC) {
    // For CC, we invoke a full compaction when going to the background, but the collector type
    // doesn't change.
    DCHECK_EQ(desired_collector_type_, kCollectorTypeCCBackground);
  }
  if (collector_type_ == kCollectorTypeCMC) {
    // For CMC collector type doesn't change.
    DCHECK_EQ(desired_collector_type_, kCollectorTypeCMCBackground);
  }
  DCHECK_NE(collector_type_, kCollectorTypeCCBackground);
  DCHECK_NE(collector_type_, kCollectorTypeCMCBackground);
  CollectorTransitionTask* added_task = nullptr;
  const uint64_t target_time = NanoTime() + delta_time;
  {
    MutexLock mu(self, *pending_task_lock_);
    // If we have an existing collector transition, update the target time to be the new target.
    if (pending_collector_transition_ != nullptr) {
      task_processor_->UpdateTargetRunTime(self, pending_collector_transition_, target_time);
      return;
    }
    added_task = new CollectorTransitionTask(target_time);
    pending_collector_transition_ = added_task;
  }
  task_processor_->AddTask(self, added_task);
}

class Heap::HeapTrimTask : public HeapTask {
 public:
  explicit HeapTrimTask(uint64_t delta_time) : HeapTask(NanoTime() + delta_time) { }
  void Run(Thread* self) override {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    heap->Trim(self);
    heap->ClearPendingTrim(self);
  }
};

void Heap::ClearPendingTrim(Thread* self) {
  MutexLock mu(self, *pending_task_lock_);
  pending_heap_trim_ = nullptr;
}

void Heap::RequestTrim(Thread* self) {
  if (!CanAddHeapTask(self)) {
    return;
  }
  // GC completed and now we must decide whether to request a heap trim (advising pages back to the
  // kernel) or not. Issuing a request will also cause trimming of the libc heap. As a trim scans
  // a space it will hold its lock and can become a cause of jank.
  // Note, the large object space self trims and the Zygote space was trimmed and unchanging since
  // forking.

  // We don't have a good measure of how worthwhile a trim might be. We can't use the live bitmap
  // because that only marks object heads, so a large array looks like lots of empty space. We
  // don't just call dlmalloc all the time, because the cost of an _attempted_ trim is proportional
  // to utilization (which is probably inversely proportional to how much benefit we can expect).
  // We could try mincore(2) but that's only a measure of how many pages we haven't given away,
  // not how much use we're making of those pages.
  HeapTrimTask* added_task = nullptr;
  {
    MutexLock mu(self, *pending_task_lock_);
    if (pending_heap_trim_ != nullptr) {
      // Already have a heap trim request in task processor, ignore this request.
      return;
    }
    added_task = new HeapTrimTask(kHeapTrimWait);
    pending_heap_trim_ = added_task;
  }
  task_processor_->AddTask(self, added_task);
}

void Heap::IncrementNumberOfBytesFreedRevoke(size_t freed_bytes_revoke) {
  size_t previous_num_bytes_freed_revoke =
      num_bytes_freed_revoke_.fetch_add(freed_bytes_revoke, std::memory_order_relaxed);
  // Check the updated value is less than the number of bytes allocated. There is a risk of
  // execution being suspended between the increment above and the CHECK below, leading to
  // the use of previous_num_bytes_freed_revoke in the comparison.
  CHECK_GE(num_bytes_allocated_.load(std::memory_order_relaxed),
           previous_num_bytes_freed_revoke + freed_bytes_revoke);
}

void Heap::RevokeThreadLocalBuffers(Thread* thread) {
  if (rosalloc_space_ != nullptr) {
    size_t freed_bytes_revoke = rosalloc_space_->RevokeThreadLocalBuffers(thread);
    if (freed_bytes_revoke > 0U) {
      IncrementNumberOfBytesFreedRevoke(freed_bytes_revoke);
    }
  }
  if (bump_pointer_space_ != nullptr) {
    CHECK_EQ(bump_pointer_space_->RevokeThreadLocalBuffers(thread), 0U);
  }
  if (region_space_ != nullptr) {
    CHECK_EQ(region_space_->RevokeThreadLocalBuffers(thread), 0U);
  }
}

void Heap::RevokeRosAllocThreadLocalBuffers(Thread* thread) {
  if (rosalloc_space_ != nullptr) {
    size_t freed_bytes_revoke = rosalloc_space_->RevokeThreadLocalBuffers(thread);
    if (freed_bytes_revoke > 0U) {
      IncrementNumberOfBytesFreedRevoke(freed_bytes_revoke);
    }
  }
}

void Heap::RevokeAllThreadLocalBuffers() {
  if (rosalloc_space_ != nullptr) {
    size_t freed_bytes_revoke = rosalloc_space_->RevokeAllThreadLocalBuffers();
    if (freed_bytes_revoke > 0U) {
      IncrementNumberOfBytesFreedRevoke(freed_bytes_revoke);
    }
  }
  if (bump_pointer_space_ != nullptr) {
    CHECK_EQ(bump_pointer_space_->RevokeAllThreadLocalBuffers(), 0U);
  }
  if (region_space_ != nullptr) {
    CHECK_EQ(region_space_->RevokeAllThreadLocalBuffers(), 0U);
  }
}

// For GC triggering purposes, we count old (pre-last-GC) and new native allocations as
// different fractions of Java allocations.
// For now, we essentially do not count old native allocations at all, so that we can preserve the
// existing behavior of not limiting native heap size. If we seriously considered it, we would
// have to adjust collection thresholds when we encounter large amounts of old native memory,
// and handle native out-of-memory situations.

static constexpr size_t kOldNativeDiscountFactor = 65536;  // Approximately infinite for now.
static constexpr size_t kNewNativeDiscountFactor = 2;

// If weighted java + native memory use exceeds our target by kStopForNativeFactor, and
// newly allocated memory exceeds stop_for_native_allocs_, we wait for GC to complete to avoid
// running out of memory.
static constexpr float kStopForNativeFactor = 4.0;

// Return the ratio of the weighted native + java allocated bytes to its target value.
// A return value > 1.0 means we should collect. Significantly larger values mean we're falling
// behind.
inline float Heap::NativeMemoryOverTarget(size_t current_native_bytes, bool is_gc_concurrent) {
  // Collection check for native allocation. Does not enforce Java heap bounds.
  // With adj_start_bytes defined below, effectively checks
  // <java bytes allocd> + c1*<old native allocd> + c2*<new native allocd) >= adj_start_bytes,
  // where c3 > 1, and currently c1 and c2 are 1 divided by the values defined above.
  size_t old_native_bytes = old_native_bytes_allocated_.load(std::memory_order_relaxed);
  if (old_native_bytes > current_native_bytes) {
    // Net decrease; skip the check, but update old value.
    // It's OK to lose an update if two stores race.
    old_native_bytes_allocated_.store(current_native_bytes, std::memory_order_relaxed);
    return 0.0;
  } else {
    size_t new_native_bytes = UnsignedDifference(current_native_bytes, old_native_bytes);
    size_t weighted_native_bytes = new_native_bytes / kNewNativeDiscountFactor
        + old_native_bytes / kOldNativeDiscountFactor;
    size_t add_bytes_allowed = static_cast<size_t>(
        NativeAllocationGcWatermark() * HeapGrowthMultiplier());
    size_t java_gc_start_bytes = is_gc_concurrent
        ? concurrent_start_bytes_
        : target_footprint_.load(std::memory_order_relaxed);
    size_t adj_start_bytes = UnsignedSum(java_gc_start_bytes,
                                         add_bytes_allowed / kNewNativeDiscountFactor);
    return static_cast<float>(GetBytesAllocated() + weighted_native_bytes)
         / static_cast<float>(adj_start_bytes);
  }
}

inline void Heap::CheckGCForNative(Thread* self) {
  bool is_gc_concurrent = IsGcConcurrent();
  uint32_t starting_gc_num = GetCurrentGcNum();
  size_t current_native_bytes = GetNativeBytes();
  float gc_urgency = NativeMemoryOverTarget(current_native_bytes, is_gc_concurrent);
  if (UNLIKELY(gc_urgency >= 1.0)) {
    if (is_gc_concurrent) {
      bool requested =
          RequestConcurrentGC(self, kGcCauseForNativeAlloc, /*force_full=*/true, starting_gc_num);
      if (requested && gc_urgency > kStopForNativeFactor
          && current_native_bytes > stop_for_native_allocs_) {
        // We're in danger of running out of memory due to rampant native allocation.
        if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
          LOG(INFO) << "Stopping for native allocation, urgency: " << gc_urgency;
        }
        // Count how many times we do this, so we can warn if this becomes excessive.
        // Stop after a while, out of excessive caution.
        static constexpr int kGcWaitIters = 20;
        for (int i = 1; i <= kGcWaitIters; ++i) {
          if (!GCNumberLt(GetCurrentGcNum(), max_gc_requested_.load(std::memory_order_relaxed))
              || WaitForGcToComplete(kGcCauseForNativeAlloc, self) != collector::kGcTypeNone) {
            break;
          }
          CHECK(GCNumberLt(starting_gc_num, max_gc_requested_.load(std::memory_order_relaxed)));
          if (i % 10 == 0) {
            LOG(WARNING) << "Slept " << i << " times in native allocation, waiting for GC";
          }
          static constexpr int kGcWaitSleepMicros = 2000;
          usleep(kGcWaitSleepMicros);  // Encourage our requested GC to start.
        }
      }
    } else {
      CollectGarbageInternal(NonStickyGcType(), kGcCauseForNativeAlloc, false, starting_gc_num + 1);
    }
  }
}

// About kNotifyNativeInterval allocations have occurred. Check whether we should garbage collect.
void Heap::NotifyNativeAllocations(JNIEnv* env) {
  native_objects_notified_.fetch_add(kNotifyNativeInterval, std::memory_order_relaxed);
  CheckGCForNative(Thread::ForEnv(env));
}

// Register a native allocation with an explicit size.
// This should only be done for large allocations of non-malloc memory, which we wouldn't
// otherwise see.
void Heap::RegisterNativeAllocation(JNIEnv* env, size_t bytes) {
  // Cautiously check for a wrapped negative bytes argument.
  DCHECK(sizeof(size_t) < 8 || bytes < (std::numeric_limits<size_t>::max() / 2));
  native_bytes_registered_.fetch_add(bytes, std::memory_order_relaxed);
  uint32_t objects_notified =
      native_objects_notified_.fetch_add(1, std::memory_order_relaxed);
  if (objects_notified % kNotifyNativeInterval == kNotifyNativeInterval - 1
      || bytes > kCheckImmediatelyThreshold) {
    CheckGCForNative(Thread::ForEnv(env));
  }
  // Heap profiler treats this as a Java allocation with a null object.
  if (GetHeapSampler().IsEnabled()) {
    JHPCheckNonTlabSampleAllocation(Thread::Current(), nullptr, bytes);
  }
}

void Heap::RegisterNativeFree(JNIEnv*, size_t bytes) {
  size_t allocated;
  size_t new_freed_bytes;
  do {
    allocated = native_bytes_registered_.load(std::memory_order_relaxed);
    new_freed_bytes = std::min(allocated, bytes);
    // We should not be registering more free than allocated bytes.
    // But correctly keep going in non-debug builds.
    DCHECK_EQ(new_freed_bytes, bytes);
  } while (!native_bytes_registered_.CompareAndSetWeakRelaxed(allocated,
                                                              allocated - new_freed_bytes));
}

size_t Heap::GetTotalMemory() const {
  return std::max(target_footprint_.load(std::memory_order_relaxed), GetBytesAllocated());
}

void Heap::AddModUnionTable(accounting::ModUnionTable* mod_union_table) {
  DCHECK(mod_union_table != nullptr);
  mod_union_tables_.Put(mod_union_table->GetSpace(), mod_union_table);
}

void Heap::CheckPreconditionsForAllocObject(ObjPtr<mirror::Class> c, size_t byte_count) {
  // Compare rounded sizes since the allocation may have been retried after rounding the size.
  // See b/37885600
  CHECK(c == nullptr || (c->IsClassClass() && byte_count >= sizeof(mirror::Class)) ||
        (c->IsVariableSize() ||
            RoundUp(c->GetObjectSize(), kObjectAlignment) ==
                RoundUp(byte_count, kObjectAlignment)))
      << "ClassFlags=" << c->GetClassFlags()
      << " IsClassClass=" << c->IsClassClass()
      << " byte_count=" << byte_count
      << " IsVariableSize=" << c->IsVariableSize()
      << " ObjectSize=" << c->GetObjectSize()
      << " sizeof(Class)=" << sizeof(mirror::Class)
      << " " << verification_->DumpObjectInfo(c.Ptr(), /*tag=*/ "klass");
  CHECK_GE(byte_count, sizeof(mirror::Object));
}

void Heap::AddRememberedSet(accounting::RememberedSet* remembered_set) {
  CHECK(remembered_set != nullptr);
  space::Space* space = remembered_set->GetSpace();
  CHECK(space != nullptr);
  CHECK(remembered_sets_.find(space) == remembered_sets_.end()) << space;
  remembered_sets_.Put(space, remembered_set);
  CHECK(remembered_sets_.find(space) != remembered_sets_.end()) << space;
}

void Heap::RemoveRememberedSet(space::Space* space) {
  CHECK(space != nullptr);
  auto it = remembered_sets_.find(space);
  CHECK(it != remembered_sets_.end());
  delete it->second;
  remembered_sets_.erase(it);
  CHECK(remembered_sets_.find(space) == remembered_sets_.end());
}

void Heap::ClearMarkedObjects(bool release_eagerly) {
  // Clear all of the spaces' mark bitmaps.
  for (const auto& space : GetContinuousSpaces()) {
    if (space->GetLiveBitmap() != nullptr && !space->HasBoundBitmaps()) {
      space->GetMarkBitmap()->Clear(release_eagerly);
    }
  }
  // Clear the marked objects in the discontinous space object sets.
  for (const auto& space : GetDiscontinuousSpaces()) {
    space->GetMarkBitmap()->Clear(release_eagerly);
  }
}

void Heap::SetAllocationRecords(AllocRecordObjectMap* records) {
  allocation_records_.reset(records);
}

void Heap::VisitAllocationRecords(RootVisitor* visitor) const {
  if (IsAllocTrackingEnabled()) {
    MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
    if (IsAllocTrackingEnabled()) {
      GetAllocationRecords()->VisitRoots(visitor);
    }
  }
}

void Heap::SweepAllocationRecords(IsMarkedVisitor* visitor) const {
  if (IsAllocTrackingEnabled()) {
    MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
    if (IsAllocTrackingEnabled()) {
      GetAllocationRecords()->SweepAllocationRecords(visitor);
    }
  }
}

void Heap::AllowNewAllocationRecords() const {
  CHECK(!gUseReadBarrier);
  MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
  AllocRecordObjectMap* allocation_records = GetAllocationRecords();
  if (allocation_records != nullptr) {
    allocation_records->AllowNewAllocationRecords();
  }
}

void Heap::DisallowNewAllocationRecords() const {
  CHECK(!gUseReadBarrier);
  MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
  AllocRecordObjectMap* allocation_records = GetAllocationRecords();
  if (allocation_records != nullptr) {
    allocation_records->DisallowNewAllocationRecords();
  }
}

void Heap::BroadcastForNewAllocationRecords() const {
  // Always broadcast without checking IsAllocTrackingEnabled() because IsAllocTrackingEnabled() may
  // be set to false while some threads are waiting for system weak access in
  // AllocRecordObjectMap::RecordAllocation() and we may fail to wake them up. b/27467554.
  MutexLock mu(Thread::Current(), *Locks::alloc_tracker_lock_);
  AllocRecordObjectMap* allocation_records = GetAllocationRecords();
  if (allocation_records != nullptr) {
    allocation_records->BroadcastForNewAllocationRecords();
  }
}

// Perfetto Java Heap Profiler Support.

// Perfetto initialization.
void Heap::InitPerfettoJavaHeapProf() {
  // Initialize Perfetto Heap info and Heap id.
  uint32_t heap_id = 1;  // Initialize to 1, to be overwritten by Perfetto heap id.
#ifdef ART_TARGET_ANDROID
  // Register the heap and create the heapid.
  // Use a Perfetto heap name = "com.android.art" for the Java Heap Profiler.
  AHeapInfo* info = AHeapInfo_create("com.android.art");
  // Set the Enable Callback, there is no callback data ("nullptr").
  AHeapInfo_setEnabledCallback(info, &EnableHeapSamplerCallback, &heap_sampler_);
  // Set the Disable Callback.
  AHeapInfo_setDisabledCallback(info, &DisableHeapSamplerCallback, &heap_sampler_);
  heap_id = AHeapProfile_registerHeap(info);
  // Do not enable the Java Heap Profiler in this case, wait for Perfetto to enable it through
  // the callback function.
#else
  // This is the host case, enable the Java Heap Profiler for host testing.
  // Perfetto API is currently not available on host.
  heap_sampler_.EnableHeapSampler();
#endif
  heap_sampler_.SetHeapID(heap_id);
  VLOG(heap) << "Java Heap Profiler Initialized";
}

void Heap::JHPCheckNonTlabSampleAllocation(Thread* self, mirror::Object* obj, size_t alloc_size) {
  bool take_sample = false;
  size_t bytes_until_sample = 0;
  HeapSampler& prof_heap_sampler = GetHeapSampler();
  // An allocation occurred, sample it, even if non-Tlab.
  // In case take_sample is already set from the previous GetSampleOffset
  // because we tried the Tlab allocation first, we will not use this value.
  // A new value is generated below. Also bytes_until_sample will be updated.
  // Note that we are not using the return value from the GetSampleOffset in
  // the NonTlab case here.
  prof_heap_sampler.GetSampleOffset(
      alloc_size, self->GetTlabPosOffset(), &take_sample, &bytes_until_sample);
  prof_heap_sampler.SetBytesUntilSample(bytes_until_sample);
  if (take_sample) {
    prof_heap_sampler.ReportSample(obj, alloc_size);
  }
  VLOG(heap) << "JHP:NonTlab Non-moving or Large Allocation or RegisterNativeAllocation";
}

size_t Heap::JHPCalculateNextTlabSize(Thread* self,
                                      size_t jhp_def_tlab_size,
                                      size_t alloc_size,
                                      bool* take_sample,
                                      size_t* bytes_until_sample) {
  size_t next_sample_point = GetHeapSampler().GetSampleOffset(
      alloc_size, self->GetTlabPosOffset(), take_sample, bytes_until_sample);
  return std::min(next_sample_point, jhp_def_tlab_size);
}

void Heap::AdjustSampleOffset(size_t adjustment) {
  GetHeapSampler().AdjustSampleOffset(adjustment);
}

void Heap::CheckGcStressMode(Thread* self, ObjPtr<mirror::Object>* obj) {
  DCHECK(gc_stress_mode_);
  auto* const runtime = Runtime::Current();
  if (runtime->GetClassLinker()->IsInitialized() && !runtime->IsActiveTransaction()) {
    // Check if we should GC.
    bool new_backtrace = false;
    {
      static constexpr size_t kMaxFrames = 16u;
      MutexLock mu(self, *backtrace_lock_);
      FixedSizeBacktrace<kMaxFrames> backtrace;
      backtrace.Collect(/* skip_count= */ 2);
      uint64_t hash = backtrace.Hash();
      new_backtrace = seen_backtraces_.find(hash) == seen_backtraces_.end();
      if (new_backtrace) {
        seen_backtraces_.insert(hash);
      }
    }
    if (new_backtrace) {
      StackHandleScope<1> hs(self);
      auto h = hs.NewHandleWrapper(obj);
      CollectGarbage(/* clear_soft_references= */ false);
      unique_backtrace_count_.fetch_add(1);
    } else {
      seen_backtrace_count_.fetch_add(1);
    }
  }
}

void Heap::DisableGCForShutdown() {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  gc_disabled_for_shutdown_ = true;
}

bool Heap::IsGCDisabledForShutdown() const {
  MutexLock mu(Thread::Current(), *gc_complete_lock_);
  return gc_disabled_for_shutdown_;
}

bool Heap::ObjectIsInBootImageSpace(ObjPtr<mirror::Object> obj) const {
  DCHECK_EQ(IsBootImageAddress(obj.Ptr()),
            any_of(boot_image_spaces_.begin(),
                   boot_image_spaces_.end(),
                   [obj](gc::space::ImageSpace* space) REQUIRES_SHARED(Locks::mutator_lock_) {
                     return space->HasAddress(obj.Ptr());
                   }));
  return IsBootImageAddress(obj.Ptr());
}

bool Heap::IsInBootImageOatFile(const void* p) const {
  DCHECK_EQ(IsBootImageAddress(p),
            any_of(boot_image_spaces_.begin(),
                   boot_image_spaces_.end(),
                   [p](gc::space::ImageSpace* space) REQUIRES_SHARED(Locks::mutator_lock_) {
                     return space->GetOatFile()->Contains(p);
                   }));
  return IsBootImageAddress(p);
}

void Heap::SetAllocationListener(AllocationListener* l) {
  AllocationListener* old = GetAndOverwriteAllocationListener(&alloc_listener_, l);

  if (old == nullptr) {
    Runtime::Current()->GetInstrumentation()->InstrumentQuickAllocEntryPoints();
  }
}

void Heap::RemoveAllocationListener() {
  AllocationListener* old = GetAndOverwriteAllocationListener(&alloc_listener_, nullptr);

  if (old != nullptr) {
    Runtime::Current()->GetInstrumentation()->UninstrumentQuickAllocEntryPoints();
  }
}

void Heap::SetGcPauseListener(GcPauseListener* l) {
  gc_pause_listener_.store(l, std::memory_order_relaxed);
}

void Heap::RemoveGcPauseListener() {
  gc_pause_listener_.store(nullptr, std::memory_order_relaxed);
}

mirror::Object* Heap::AllocWithNewTLAB(Thread* self,
                                       AllocatorType allocator_type,
                                       size_t alloc_size,
                                       bool grow,
                                       size_t* bytes_allocated,
                                       size_t* usable_size,
                                       size_t* bytes_tl_bulk_allocated) {
  mirror::Object* ret = nullptr;
  bool take_sample = false;
  size_t bytes_until_sample = 0;
  bool jhp_enabled = GetHeapSampler().IsEnabled();

  if (kUsePartialTlabs && alloc_size <= self->TlabRemainingCapacity()) {
    DCHECK_GT(alloc_size, self->TlabSize());
    // There is enough space if we grow the TLAB. Lets do that. This increases the
    // TLAB bytes.
    const size_t min_expand_size = alloc_size - self->TlabSize();
    size_t next_tlab_size =
        jhp_enabled ? JHPCalculateNextTlabSize(
                          self, kPartialTlabSize, alloc_size, &take_sample, &bytes_until_sample) :
                      kPartialTlabSize;
    const size_t expand_bytes = std::max(
        min_expand_size,
        std::min(self->TlabRemainingCapacity() - self->TlabSize(), next_tlab_size));
    if (UNLIKELY(IsOutOfMemoryOnAllocation(allocator_type, expand_bytes, grow))) {
      return nullptr;
    }
    *bytes_tl_bulk_allocated = expand_bytes;
    self->ExpandTlab(expand_bytes);
    DCHECK_LE(alloc_size, self->TlabSize());
  } else if (allocator_type == kAllocatorTypeTLAB) {
    DCHECK(bump_pointer_space_ != nullptr);
    // Try to allocate a page-aligned TLAB (not necessary though).
    // TODO: for large allocations, which are rare, maybe we should allocate
    // that object and return. There is no need to revoke the current TLAB,
    // particularly if it's mostly unutilized.
    size_t next_tlab_size = RoundDown(alloc_size + kDefaultTLABSize, gPageSize) - alloc_size;
    if (jhp_enabled) {
      next_tlab_size = JHPCalculateNextTlabSize(
          self, next_tlab_size, alloc_size, &take_sample, &bytes_until_sample);
    }
    const size_t new_tlab_size = alloc_size + next_tlab_size;
    if (UNLIKELY(IsOutOfMemoryOnAllocation(allocator_type, new_tlab_size, grow))) {
      return nullptr;
    }
    // Try allocating a new thread local buffer, if the allocation fails the space must be
    // full so return null.
    if (!bump_pointer_space_->AllocNewTlab(self, new_tlab_size, bytes_tl_bulk_allocated)) {
      return nullptr;
    }
    if (jhp_enabled) {
      VLOG(heap) << "JHP:kAllocatorTypeTLAB, New Tlab bytes allocated= " << new_tlab_size;
    }
  } else {
    DCHECK(allocator_type == kAllocatorTypeRegionTLAB);
    DCHECK(region_space_ != nullptr);
    if (space::RegionSpace::kRegionSize >= alloc_size) {
      // Non-large. Check OOME for a tlab.
      if (LIKELY(!IsOutOfMemoryOnAllocation(allocator_type,
                                            space::RegionSpace::kRegionSize,
                                            grow))) {
        size_t next_pr_tlab_size =
            kUsePartialTlabs ? kPartialTlabSize : gc::space::RegionSpace::kRegionSize;
        if (jhp_enabled) {
          next_pr_tlab_size = JHPCalculateNextTlabSize(
              self, next_pr_tlab_size, alloc_size, &take_sample, &bytes_until_sample);
        }
        const size_t new_tlab_size = kUsePartialTlabs
            ? std::max(alloc_size, next_pr_tlab_size)
            : next_pr_tlab_size;
        // Try to allocate a tlab.
        if (!region_space_->AllocNewTlab(self, new_tlab_size, bytes_tl_bulk_allocated)) {
          // Failed to allocate a tlab. Try non-tlab.
          ret = region_space_->AllocNonvirtual<false>(alloc_size,
                                                      bytes_allocated,
                                                      usable_size,
                                                      bytes_tl_bulk_allocated);
          if (jhp_enabled) {
            JHPCheckNonTlabSampleAllocation(self, ret, alloc_size);
          }
          return ret;
        }
        // Fall-through to using the TLAB below.
      } else {
        // Check OOME for a non-tlab allocation.
        if (!IsOutOfMemoryOnAllocation(allocator_type, alloc_size, grow)) {
          ret = region_space_->AllocNonvirtual<false>(alloc_size,
                                                      bytes_allocated,
                                                      usable_size,
                                                      bytes_tl_bulk_allocated);
          if (jhp_enabled) {
            JHPCheckNonTlabSampleAllocation(self, ret, alloc_size);
          }
          return ret;
        }
        // Neither tlab or non-tlab works. Give up.
        return nullptr;
      }
    } else {
      // Large. Check OOME.
      if (LIKELY(!IsOutOfMemoryOnAllocation(allocator_type, alloc_size, grow))) {
        ret = region_space_->AllocNonvirtual<false>(alloc_size,
                                                    bytes_allocated,
                                                    usable_size,
                                                    bytes_tl_bulk_allocated);
        if (jhp_enabled) {
          JHPCheckNonTlabSampleAllocation(self, ret, alloc_size);
        }
        return ret;
      }
      return nullptr;
    }
  }
  // Refilled TLAB, return.
  ret = self->AllocTlab(alloc_size);
  DCHECK(ret != nullptr);
  *bytes_allocated = alloc_size;
  *usable_size = alloc_size;

  // JavaHeapProfiler: Send the thread information about this allocation in case a sample is
  // requested.
  // This is the fallthrough from both the if and else if above cases => Cases that use TLAB.
  if (jhp_enabled) {
    if (take_sample) {
      GetHeapSampler().ReportSample(ret, alloc_size);
      // Update the bytes_until_sample now that the allocation is already done.
      GetHeapSampler().SetBytesUntilSample(bytes_until_sample);
    }
    VLOG(heap) << "JHP:Fallthrough Tlab allocation";
  }

  return ret;
}

const Verification* Heap::GetVerification() const {
  return verification_.get();
}

void Heap::VlogHeapGrowth(size_t old_footprint, size_t new_footprint, size_t alloc_size) {
  VLOG(heap) << "Growing heap from " << PrettySize(old_footprint) << " to "
             << PrettySize(new_footprint) << " for a " << PrettySize(alloc_size) << " allocation";
}

// Run a gc if we haven't run one since initial_gc_num. This forces processes to
// reclaim memory allocated during startup, even if they don't do much
// allocation post startup. If the process is actively allocating and triggering
// GCs, or has moved to the background and hence forced a GC, this does nothing.
class Heap::TriggerPostForkCCGcTask : public HeapTask {
 public:
  explicit TriggerPostForkCCGcTask(uint64_t target_time, uint32_t initial_gc_num) :
      HeapTask(target_time), initial_gc_num_(initial_gc_num) {}
  void Run(Thread* self) override {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->GetCurrentGcNum() == initial_gc_num_) {
      if (kLogAllGCs) {
        LOG(INFO) << "Forcing GC for allocation-inactive process";
      }
      heap->RequestConcurrentGC(self, kGcCauseBackground, false, initial_gc_num_);
    }
  }
 private:
  uint32_t initial_gc_num_;
};

// Reduce target footprint, if no GC has occurred since initial_gc_num.
// If a GC already occurred, it will have done this for us.
class Heap::ReduceTargetFootprintTask : public HeapTask {
 public:
  explicit ReduceTargetFootprintTask(uint64_t target_time, size_t new_target_sz,
                                     uint32_t initial_gc_num) :
      HeapTask(target_time), new_target_sz_(new_target_sz), initial_gc_num_(initial_gc_num) {}
  void Run(Thread* self) override {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    MutexLock mu(self, *(heap->gc_complete_lock_));
    if (heap->GetCurrentGcNum() == initial_gc_num_
        && heap->collector_type_running_ == kCollectorTypeNone) {
      size_t target_footprint = heap->target_footprint_.load(std::memory_order_relaxed);
      if (target_footprint > new_target_sz_) {
        if (heap->target_footprint_.CompareAndSetStrongRelaxed(target_footprint, new_target_sz_)) {
          heap->SetDefaultConcurrentStartBytesLocked();
        }
      }
    }
  }
 private:
  size_t new_target_sz_;
  uint32_t initial_gc_num_;
};

// Return a pseudo-random integer between 0 and 19999, using the uid as a seed.  We want this to
// be deterministic for a given process, but to vary randomly across processes. Empirically, the
// uids for processes for which this matters are distinct.
static uint32_t GetPseudoRandomFromUid() {
  std::default_random_engine rng(getuid());
  std::uniform_int_distribution<int> dist(0, 19999);
  return dist(rng);
}

void Heap::PostForkChildAction(Thread* self) {
  uint32_t starting_gc_num = GetCurrentGcNum();
  uint64_t last_adj_time = NanoTime();
  next_gc_type_ = NonStickyGcType();  // Always start with a full gc.

  LOG(INFO) << "Using " << foreground_collector_type_ << " GC.";
  if (gUseUserfaultfd) {
    DCHECK_NE(mark_compact_, nullptr);
    mark_compact_->CreateUserfaultfd(/*post_fork*/true);
  }

  // Temporarily increase target_footprint_ and concurrent_start_bytes_ to
  // max values to avoid GC during app launch.
  // Set target_footprint_ to the largest allowed value.
  SetIdealFootprint(growth_limit_);
  SetDefaultConcurrentStartBytes();

  // Shrink heap after kPostForkMaxHeapDurationMS, to force a memory hog process to GC.
  // This remains high enough that many processes will continue without a GC.
  if (initial_heap_size_ < growth_limit_) {
    size_t first_shrink_size = std::max(growth_limit_ / 4, initial_heap_size_);
    last_adj_time += MsToNs(kPostForkMaxHeapDurationMS);
    GetTaskProcessor()->AddTask(
        self, new ReduceTargetFootprintTask(last_adj_time, first_shrink_size, starting_gc_num));
    // Shrink to a small value after a substantial time period. This will typically force a
    // GC if none has occurred yet. Has no effect if there was a GC before this anyway, which
    // is commonly the case, e.g. because of a process transition.
    if (initial_heap_size_ < first_shrink_size) {
      last_adj_time += MsToNs(4 * kPostForkMaxHeapDurationMS);
      GetTaskProcessor()->AddTask(
          self,
          new ReduceTargetFootprintTask(last_adj_time, initial_heap_size_, starting_gc_num));
    }
  }
  // Schedule a GC after a substantial period of time. This will become a no-op if another GC is
  // scheduled in the interim. If not, we want to avoid holding onto start-up garbage.
  uint64_t post_fork_gc_time = last_adj_time
      + MsToNs(4 * kPostForkMaxHeapDurationMS + GetPseudoRandomFromUid());
  GetTaskProcessor()->AddTask(self,
                              new TriggerPostForkCCGcTask(post_fork_gc_time, starting_gc_num));
}

void Heap::VisitReflectiveTargets(ReflectiveValueVisitor *visit) {
  VisitObjectsPaused([&visit](mirror::Object* ref) NO_THREAD_SAFETY_ANALYSIS {
    art::ObjPtr<mirror::Class> klass(ref->GetClass());
    // All these classes are in the BootstrapClassLoader.
    if (!klass->IsBootStrapClassLoaded()) {
      return;
    }
    if (GetClassRoot<mirror::Method>()->IsAssignableFrom(klass) ||
        GetClassRoot<mirror::Constructor>()->IsAssignableFrom(klass)) {
      down_cast<mirror::Executable*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::Field>() == klass) {
      down_cast<mirror::Field*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::MethodHandle>()->IsAssignableFrom(klass)) {
      down_cast<mirror::MethodHandle*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::StaticFieldVarHandle>()->IsAssignableFrom(klass)) {
      down_cast<mirror::StaticFieldVarHandle*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::FieldVarHandle>()->IsAssignableFrom(klass)) {
      down_cast<mirror::FieldVarHandle*>(ref)->VisitTarget(visit);
    } else if (art::GetClassRoot<art::mirror::DexCache>()->IsAssignableFrom(klass)) {
      down_cast<mirror::DexCache*>(ref)->VisitReflectiveTargets(visit);
    }
  });
}

bool Heap::AddHeapTask(gc::HeapTask* task) {
  Thread* const self = Thread::Current();
  if (!CanAddHeapTask(self)) {
    return false;
  }
  GetTaskProcessor()->AddTask(self, task);
  return true;
}

std::string Heap::GetForegroundCollectorName() {
  std::ostringstream oss;
  oss << foreground_collector_type_;
  return oss.str();
}

bool Heap::HasAppImageSpaceFor(const std::string& dex_location) const {
  ScopedObjectAccess soa(Thread::Current());
  for (space::ContinuousSpace* space : continuous_spaces_) {
    // An image space is either a boot image space or an app image space.
    if (space->IsImageSpace() &&
        !IsBootImageAddress(space->Begin()) &&
        (space->AsImageSpace()->GetOatFile()->GetOatDexFiles()[0]->GetDexFileLocation() ==
              dex_location)) {
      return true;
    }
  }
  return false;
}

}  // namespace gc
}  // namespace art

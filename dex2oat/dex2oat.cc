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

#include <inttypes.h>
#include <log/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <algorithm>
#include <forward_list>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include "aot_class_linker.h"
#include "arch/instruction_set_features.h"
#include "art_method-inl.h"
#include "base/callee_save_type.h"
#include "base/dumpable.h"
#include "base/fast_exit.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "base/leb128.h"
#include "base/macros.h"
#include "base/memory_tool.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "base/zip_archive.h"
#include "class_linker.h"
#include "class_loader_context.h"
#include "class_root-inl.h"
#include "cmdline_parser.h"
#include "compiler.h"
#include "compiler_callbacks.h"
#include "debug/elf_debug_writer.h"
#include "debug/method_debug_info.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_loader.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/verification_results.h"
#include "dex2oat_options.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "driver/compiler_options_map-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "gc/verification.h"
#include "interpreter/unstarted_runtime.h"
#include "jni/java_vm_ext.h"
#include "linker/elf_writer.h"
#include "linker/elf_writer_quick.h"
#include "linker/image_writer.h"
#include "linker/multi_oat_relative_patcher.h"
#include "linker/oat_writer.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "oat/elf_file.h"
#include "oat/oat.h"
#include "oat/oat_file.h"
#include "oat/oat_file_assistant.h"
#include "palette/palette.h"
#include "profile/profile_compilation_info.h"
#include "runtime.h"
#include "runtime_intrinsics.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "stream/buffered_output_stream.h"
#include "stream/file_output_stream.h"
#include "vdex_file.h"
#include "verifier/verifier_deps.h"

namespace art {

namespace dex2oat {
  enum class ReturnCode : int {
    kNoFailure = 0,          // No failure, execution completed successfully.
    kOther = 1,              // Some other not closer specified error occurred.
    kCreateRuntime = 2,      // Dex2oat failed creating a runtime.
  };
}  // namespace dex2oat

using android::base::StringAppendV;
using android::base::StringPrintf;
using gc::space::ImageSpace;

static constexpr size_t kDefaultMinDexFilesForSwap = 2;
static constexpr size_t kDefaultMinDexFileCumulativeSizeForSwap = 20 * MB;

// Compiler filter override for very large apps.
static constexpr CompilerFilter::Filter kLargeAppFilter = CompilerFilter::kVerify;

static int original_argc;
static char** original_argv;

static std::string CommandLine() {
  std::vector<std::string> command;
  command.reserve(original_argc);
  for (int i = 0; i < original_argc; ++i) {
    command.push_back(original_argv[i]);
  }
  return android::base::Join(command, ' ');
}

// A stripped version. Remove some less essential parameters. If we see a "--zip-fd=" parameter, be
// even more aggressive. There won't be much reasonable data here for us in that case anyways (the
// locations are all staged).
static std::string StrippedCommandLine() {
  std::vector<std::string> command;

  // Do a pre-pass to look for zip-fd and the compiler filter.
  bool saw_zip_fd = false;
  bool saw_compiler_filter = false;
  for (int i = 0; i < original_argc; ++i) {
    std::string_view arg(original_argv[i]);
    if (arg.starts_with("--zip-fd=")) {
      saw_zip_fd = true;
    }
    if (arg.starts_with("--compiler-filter=")) {
      saw_compiler_filter = true;
    }
  }

  // Now filter out things.
  for (int i = 0; i < original_argc; ++i) {
    std::string_view arg(original_argv[i]);
    // All runtime-arg parameters are dropped.
    if (arg == "--runtime-arg") {
      i++;  // Drop the next part, too.
      continue;
    }

    // Any instruction-setXXX is dropped.
    if (arg.starts_with("--instruction-set")) {
      continue;
    }

    // The boot image is dropped.
    if (arg.starts_with("--boot-image=")) {
      continue;
    }

    // The image format is dropped.
    if (arg.starts_with("--image-format=")) {
      continue;
    }

    // This should leave any dex-file and oat-file options, describing what we compiled.

    // However, we prefer to drop this when we saw --zip-fd.
    if (saw_zip_fd) {
      // Drop anything --zip-X, --dex-X, --oat-X, --swap-X, or --app-image-X
      if (arg.starts_with("--zip-") ||
          arg.starts_with("--dex-") ||
          arg.starts_with("--oat-") ||
          arg.starts_with("--swap-") ||
          arg.starts_with("--app-image-")) {
        continue;
      }
    }

    command.push_back(std::string(arg));
  }

  if (!saw_compiler_filter) {
    command.push_back("--compiler-filter=" +
        CompilerFilter::NameOfFilter(CompilerFilter::kDefaultCompilerFilter));
  }

  // Construct the final output.
  if (command.size() <= 1U) {
    // It seems only "/apex/com.android.art/bin/dex2oat" is left, or not
    // even that. Use a pretty line.
    return "Starting dex2oat.";
  }
  return android::base::Join(command, ' ');
}

static void UsageErrorV(const char* fmt, va_list ap) {
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(ERROR) << error;
}

static void UsageError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);
}

NO_RETURN static void Usage(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);

  UsageError("Command: %s", CommandLine().c_str());

  UsageError("Usage: dex2oat [options]...");
  UsageError("");

  std::stringstream oss;
  VariableIndentationOutputStream vios(&oss);
  auto parser = CreateDex2oatArgumentParser();
  parser.DumpHelp(vios);
  UsageError(oss.str().c_str());
  std::cerr << "See log for usage error information\n";
  exit(EXIT_FAILURE);
}


// Set CPU affinity from a string containing a comma-separated list of numeric CPU identifiers.
static void SetCpuAffinity(const std::vector<int32_t>& cpu_list) {
#ifdef __linux__
  int cpu_count = sysconf(_SC_NPROCESSORS_CONF);
  cpu_set_t target_cpu_set;
  CPU_ZERO(&target_cpu_set);

  for (int32_t cpu : cpu_list) {
    if (cpu >= 0 && cpu < cpu_count) {
      CPU_SET(cpu, &target_cpu_set);
    } else {
      // Argument error is considered fatal, suggests misconfigured system properties.
      Usage("Invalid cpu \"d\" specified in --cpu-set argument (nprocessors = %d)",
            cpu, cpu_count);
    }
  }

  if (sched_setaffinity(getpid(), sizeof(target_cpu_set), &target_cpu_set) == -1) {
    // Failure to set affinity may be outside control of requestor, log warning rather than
    // treating as fatal.
    PLOG(WARNING) << "Failed to set CPU affinity.";
  }
#else
  LOG(WARNING) << "--cpu-set not supported on this platform.";
#endif  // __linux__
}



// The primary goal of the watchdog is to prevent stuck build servers
// during development when fatal aborts lead to a cascade of failures
// that result in a deadlock.
class WatchDog {
// WatchDog defines its own CHECK_PTHREAD_CALL to avoid using LOG which uses locks
#undef CHECK_PTHREAD_CALL
#define CHECK_WATCH_DOG_PTHREAD_CALL(call, args, what) \
  do { \
    int rc = call args; \
    if (rc != 0) { \
      errno = rc; \
      std::string message(# call); \
      message += " failed for "; \
      message += reason; \
      Fatal(message); \
    } \
  } while (false)

 public:
  explicit WatchDog(int64_t timeout_in_milliseconds)
      : timeout_in_milliseconds_(timeout_in_milliseconds),
        shutting_down_(false) {
    const char* reason = "dex2oat watch dog thread startup";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_init, (&mutex_, nullptr), reason);
#ifndef __APPLE__
    pthread_condattr_t condattr;
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_condattr_init, (&condattr), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_condattr_setclock, (&condattr, CLOCK_MONOTONIC), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_init, (&cond_, &condattr), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_condattr_destroy, (&condattr), reason);
#endif
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_attr_init, (&attr_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_create, (&pthread_, &attr_, &CallBack, this), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_attr_destroy, (&attr_), reason);
  }
  ~WatchDog() {
    const char* reason = "dex2oat watch dog thread shutdown";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&mutex_), reason);
    shutting_down_ = true;
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_signal, (&cond_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&mutex_), reason);

    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_join, (pthread_, nullptr), reason);

    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_destroy, (&cond_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_destroy, (&mutex_), reason);
  }

  static void SetRuntime(Runtime* runtime) {
    const char* reason = "dex2oat watch dog set runtime";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&runtime_mutex_), reason);
    runtime_ = runtime;
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&runtime_mutex_), reason);
  }

  // TODO: tune the multiplier for GC verification, the following is just to make the timeout
  //       large.
  static constexpr int64_t kWatchdogVerifyMultiplier =
      kVerifyObjectSupport > kVerifyObjectModeFast ? 100 : 1;

  // When setting timeouts, keep in mind that the build server may not be as fast as your
  // desktop. Debug builds are slower so they have larger timeouts.
  static constexpr int64_t kWatchdogSlowdownFactor = kIsDebugBuild ? 5U : 1U;

  // 9.5 minutes scaled by kSlowdownFactor. This is slightly smaller than the Package Manager
  // watchdog (PackageManagerService.WATCHDOG_TIMEOUT, 10 minutes), so that dex2oat will abort
  // itself before that watchdog would take down the system server.
  static constexpr int64_t kWatchDogTimeoutSeconds = kWatchdogSlowdownFactor * (9 * 60 + 30);

  static constexpr int64_t kDefaultWatchdogTimeoutInMS =
      kWatchdogVerifyMultiplier * kWatchDogTimeoutSeconds * 1000;

 private:
  static void* CallBack(void* arg) {
    WatchDog* self = reinterpret_cast<WatchDog*>(arg);
    ::art::SetThreadName("dex2oat watch dog");
    self->Wait();
    return nullptr;
  }

  NO_RETURN static void Fatal(const std::string& message) {
    // TODO: When we can guarantee it won't prevent shutdown in error cases, move to LOG. However,
    //       it's rather easy to hang in unwinding.
    //       LogLine also avoids ART logging lock issues, as it's really only a wrapper around
    //       logcat logging or stderr output.
    LogHelper::LogLineLowStack(__FILE__, __LINE__, LogSeverity::FATAL, message.c_str());

    // If we're on the host, try to dump all threads to get a sense of what's going on. This is
    // restricted to the host as the dump may itself go bad.
    // TODO: Use a double watchdog timeout, so we can enable this on-device.
    Runtime* runtime = GetRuntime();
    if (!kIsTargetBuild && runtime != nullptr) {
      runtime->AttachCurrentThread("Watchdog thread attached for dumping",
                                   true,
                                   nullptr,
                                   false);
      runtime->DumpForSigQuit(std::cerr);
    }
    exit(static_cast<int>(dex2oat::ReturnCode::kOther));
  }

  void Wait() {
    timespec timeout_ts;
#if defined(__APPLE__)
    InitTimeSpec(true, CLOCK_REALTIME, timeout_in_milliseconds_, 0, &timeout_ts);
#else
    InitTimeSpec(true, CLOCK_MONOTONIC, timeout_in_milliseconds_, 0, &timeout_ts);
#endif
    const char* reason = "dex2oat watch dog thread waiting";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&mutex_), reason);
    while (!shutting_down_) {
      int rc = pthread_cond_timedwait(&cond_, &mutex_, &timeout_ts);
      if (rc == EINTR) {
        continue;
      } else if (rc == ETIMEDOUT) {
        Fatal(StringPrintf("dex2oat did not finish after %" PRId64 " milliseconds",
                           timeout_in_milliseconds_));
      } else if (rc != 0) {
        std::string message(StringPrintf("pthread_cond_timedwait failed: %s", strerror(rc)));
        Fatal(message);
      }
    }
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&mutex_), reason);
  }

  static Runtime* GetRuntime() {
    const char* reason = "dex2oat watch dog get runtime";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&runtime_mutex_), reason);
    Runtime* runtime = runtime_;
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&runtime_mutex_), reason);
    return runtime;
  }

  static pthread_mutex_t runtime_mutex_;
  static Runtime* runtime_;

  // TODO: Switch to Mutex when we can guarantee it won't prevent shutdown in error cases.
  pthread_mutex_t mutex_;
  pthread_cond_t cond_;
  pthread_attr_t attr_;
  pthread_t pthread_;

  const int64_t timeout_in_milliseconds_;
  bool shutting_down_;
};

pthread_mutex_t WatchDog::runtime_mutex_ = PTHREAD_MUTEX_INITIALIZER;
Runtime* WatchDog::runtime_ = nullptr;

// Helper class for overriding `java.lang.ThreadLocal.nextHashCode`.
//
// The class ThreadLocal has a static field nextHashCode used for assigning hash codes to
// new ThreadLocal objects. Since the class and the object referenced by the field are
// in the boot image, they cannot be modified under normal rules for AOT compilation.
// However, since this is a private detail that's used only for assigning hash codes and
// everything should work fine with different hash codes, we override the field for the
// compilation, providing another object that the AOT class initialization can modify.
class ThreadLocalHashOverride {
 public:
  ThreadLocalHashOverride(bool apply, int32_t initial_value) {
    Thread* self = Thread::Current();
    ScopedObjectAccess soa(self);
    hs_.emplace(self);  // While holding the mutator lock.
    Runtime* runtime = Runtime::Current();
    klass_ = hs_->NewHandle(apply
        ? runtime->GetClassLinker()->LookupClass(self,
                                                 "Ljava/lang/ThreadLocal;",
                                                 /*class_loader=*/ nullptr)
        : nullptr);
    field_ = ((klass_ != nullptr) && klass_->IsVisiblyInitialized())
        ? klass_->FindDeclaredStaticField("nextHashCode",
                                          "Ljava/util/concurrent/atomic/AtomicInteger;")
        : nullptr;
    old_field_value_ =
        hs_->NewHandle(field_ != nullptr ? field_->GetObject(klass_.Get()) : nullptr);
    if (old_field_value_ != nullptr) {
      gc::AllocatorType allocator_type = runtime->GetHeap()->GetCurrentAllocator();
      StackHandleScope<1u> hs2(self);
      Handle<mirror::Object> new_field_value = hs2.NewHandle(
          old_field_value_->GetClass()->Alloc(self, allocator_type));
      PointerSize pointer_size = runtime->GetClassLinker()->GetImagePointerSize();
      ArtMethod* constructor = old_field_value_->GetClass()->FindConstructor("(I)V", pointer_size);
      CHECK(constructor != nullptr);
      uint32_t args[] = {
          reinterpret_cast32<uint32_t>(new_field_value.Get()),
          static_cast<uint32_t>(initial_value)
      };
      JValue result;
      constructor->Invoke(self, args, sizeof(args), &result, /*shorty=*/ "VI");
      CHECK(!self->IsExceptionPending());
      field_->SetObject</*kTransactionActive=*/ false>(klass_.Get(), new_field_value.Get());
    }
    if (apply && old_field_value_ == nullptr) {
      if ((klass_ != nullptr) && klass_->IsVisiblyInitialized()) {
        // This would mean that the implementation of ThreadLocal has changed
        // and the code above is no longer applicable.
        LOG(ERROR) << "Failed to override ThreadLocal.nextHashCode";
      } else {
        VLOG(compiler) << "ThreadLocal is not initialized in the primary boot image.";
      }
    }
  }

  ~ThreadLocalHashOverride() {
    ScopedObjectAccess soa(hs_->Self());
    if (old_field_value_ != nullptr) {
      // Allow the overriding object to be collected.
      field_->SetObject</*kTransactionActive=*/ false>(klass_.Get(), old_field_value_.Get());
    }
    hs_.reset();  // While holding the mutator lock.
  }

 private:
  std::optional<StackHandleScope<2u>> hs_;
  Handle<mirror::Class> klass_;
  ArtField* field_;
  Handle<mirror::Object> old_field_value_;
};

class Dex2Oat final {
 public:
  explicit Dex2Oat(TimingLogger* timings)
      : key_value_store_(nullptr),
        verification_results_(nullptr),
        runtime_(nullptr),
        thread_count_(sysconf(_SC_NPROCESSORS_CONF)),
        start_ns_(NanoTime()),
        start_cputime_ns_(ProcessCpuNanoTime()),
        strip_(false),
        oat_fd_(-1),
        input_vdex_fd_(-1),
        output_vdex_fd_(-1),
        input_vdex_file_(nullptr),
        dm_fd_(-1),
        zip_fd_(-1),
        image_fd_(-1),
        have_multi_image_arg_(false),
        image_base_(0U),
        image_storage_mode_(ImageHeader::kStorageModeUncompressed),
        passes_to_run_filename_(nullptr),
        is_host_(false),
        elf_writers_(),
        oat_writers_(),
        rodata_(),
        image_writer_(nullptr),
        driver_(nullptr),
        opened_dex_files_maps_(),
        opened_dex_files_(),
        avoid_storing_invocation_(false),
        swap_fd_(File::kInvalidFd),
        app_image_fd_(File::kInvalidFd),
        timings_(timings),
        force_determinism_(false),
        check_linkage_conditions_(false),
        crash_on_linkage_violation_(false),
        compile_individually_(false),
        profile_load_attempted_(false),
        should_report_dex2oat_compilation_(false) {}

  ~Dex2Oat() {
    // Log completion time before deleting the runtime_, because this accesses
    // the runtime.
    LogCompletionTime();

    if (!kIsDebugBuild && !(kRunningOnMemoryTool && kMemoryToolDetectsLeaks)) {
      // We want to just exit on non-debug builds, not bringing the runtime down
      // in an orderly fashion. So release the following fields.
      if (!compiler_options_->GetDumpStats()) {
        // The --dump-stats get logged when the optimizing compiler gets destroyed, so we can't
        // release the driver_.
        driver_.release();              // NOLINT
      }
      image_writer_.release();          // NOLINT
      for (std::unique_ptr<const DexFile>& dex_file : opened_dex_files_) {
        dex_file.release();             // NOLINT
      }
      new std::vector<MemMap>(std::move(opened_dex_files_maps_));  // Leak MemMaps.
      for (std::unique_ptr<File>& vdex_file : vdex_files_) {
        vdex_file.release();            // NOLINT
      }
      for (std::unique_ptr<File>& oat_file : oat_files_) {
        oat_file.release();             // NOLINT
      }
      runtime_.release();               // NOLINT
      verification_results_.release();  // NOLINT
      key_value_store_.release();       // NOLINT
    }

    // Remind the user if they passed testing only flags.
    if (!kIsTargetBuild && force_allow_oj_inlines_) {
      LOG(ERROR) << "Inlines allowed from core-oj! FOR TESTING USE ONLY! DO NOT DISTRIBUTE"
                  << " BINARIES BUILT WITH THIS OPTION!";
    }
  }

  struct ParserOptions {
    std::vector<std::string> oat_symbols;
    std::string boot_image_filename;
    int64_t watch_dog_timeout_in_ms = -1;
    bool watch_dog_enabled = true;
    bool requested_specific_compiler = false;
    std::string error_msg;
  };

  void ParseBase(const std::string& option) {
    char* end;
    image_base_ = strtoul(option.c_str(), &end, 16);
    if (end == option.c_str() || *end != '\0') {
      Usage("Failed to parse hexadecimal value for option %s", option.data());
    }
  }

  bool VerifyProfileData() {
    return profile_compilation_info_->VerifyProfileData(compiler_options_->dex_files_for_oat_file_);
  }

  void ParseInstructionSetVariant(const std::string& option, ParserOptions* parser_options) {
    if (kIsTargetBuild) {
      compiler_options_->instruction_set_features_ = InstructionSetFeatures::FromVariantAndHwcap(
          compiler_options_->instruction_set_, option, &parser_options->error_msg);
    } else {
      compiler_options_->instruction_set_features_ = InstructionSetFeatures::FromVariant(
          compiler_options_->instruction_set_, option, &parser_options->error_msg);
    }
    if (compiler_options_->instruction_set_features_ == nullptr) {
      Usage("%s", parser_options->error_msg.c_str());
    }
  }

  void ParseInstructionSetFeatures(const std::string& option, ParserOptions* parser_options) {
    if (compiler_options_->instruction_set_features_ == nullptr) {
      compiler_options_->instruction_set_features_ = InstructionSetFeatures::FromVariant(
          compiler_options_->instruction_set_, "default", &parser_options->error_msg);
      if (compiler_options_->instruction_set_features_ == nullptr) {
        Usage("Problem initializing default instruction set features variant: %s",
              parser_options->error_msg.c_str());
      }
    }
    compiler_options_->instruction_set_features_ =
        compiler_options_->instruction_set_features_->AddFeaturesFromString(
            option, &parser_options->error_msg);
    if (compiler_options_->instruction_set_features_ == nullptr) {
      Usage("Error parsing '%s': %s", option.c_str(), parser_options->error_msg.c_str());
    }
  }

  void ProcessOptions(ParserOptions* parser_options) {
    compiler_options_->compiler_type_ = CompilerOptions::CompilerType::kAotCompiler;
    compiler_options_->compile_pic_ = true;  // All AOT compilation is PIC.

    // TODO: This should be a command line option for cross-compilation. b/289805127
    compiler_options_->emit_read_barrier_ = gUseReadBarrier;

    if (android_root_.empty()) {
      const char* android_root_env_var = getenv("ANDROID_ROOT");
      if (android_root_env_var == nullptr) {
        Usage("--android-root unspecified and ANDROID_ROOT not set");
      }
      android_root_ += android_root_env_var;
    }

    if (!parser_options->boot_image_filename.empty()) {
      boot_image_filename_ = parser_options->boot_image_filename;
    }

    DCHECK(compiler_options_->image_type_ == CompilerOptions::ImageType::kNone);
    if (!image_filenames_.empty() || image_fd_ != -1) {
      // If no boot image is provided, then dex2oat is compiling the primary boot image,
      // otherwise it is compiling the boot image extension.
      compiler_options_->image_type_ = boot_image_filename_.empty()
          ? CompilerOptions::ImageType::kBootImage
          : CompilerOptions::ImageType::kBootImageExtension;
    }
    if (app_image_fd_ != -1 || !app_image_file_name_.empty()) {
      if (compiler_options_->IsBootImage() || compiler_options_->IsBootImageExtension()) {
        Usage("Can't have both (--image or --image-fd) and (--app-image-fd or --app-image-file)");
      }
      if (profile_files_.empty() && profile_file_fds_.empty()) {
        LOG(WARNING) << "Generating an app image without a profile. This will result in an app "
                        "image with no classes. Did you forget to add the profile with either "
                        "--profile-file-fd or --profile-file?";
      }
      compiler_options_->image_type_ = CompilerOptions::ImageType::kAppImage;
    }

    if (!image_filenames_.empty() && image_fd_ != -1) {
      Usage("Can't have both --image and --image-fd");
    }

    if (oat_filenames_.empty() && oat_fd_ == -1) {
      Usage("Output must be supplied with either --oat-file or --oat-fd");
    }

    if (input_vdex_fd_ != -1 && !input_vdex_.empty()) {
      Usage("Can't have both --input-vdex-fd and --input-vdex");
    }

    if (output_vdex_fd_ != -1 && !output_vdex_.empty()) {
      Usage("Can't have both --output-vdex-fd and --output-vdex");
    }

    if (!oat_filenames_.empty() && oat_fd_ != -1) {
      Usage("--oat-file should not be used with --oat-fd");
    }

    if ((output_vdex_fd_ == -1) != (oat_fd_ == -1)) {
      Usage("VDEX and OAT output must be specified either with one --oat-file "
            "or with --oat-fd and --output-vdex-fd file descriptors");
    }

    if ((image_fd_ != -1) && (oat_fd_ == -1)) {
      Usage("--image-fd must be used with --oat_fd and --output_vdex_fd");
    }

    if (!parser_options->oat_symbols.empty() && oat_fd_ != -1) {
      Usage("--oat-symbols should not be used with --oat-fd");
    }

    if (!parser_options->oat_symbols.empty() && is_host_) {
      Usage("--oat-symbols should not be used with --host");
    }

    if (output_vdex_fd_ != -1 && !image_filenames_.empty()) {
      Usage("--output-vdex-fd should not be used with --image");
    }

    if (oat_fd_ != -1 && !image_filenames_.empty()) {
      Usage("--oat-fd should not be used with --image");
    }

    if (!parser_options->oat_symbols.empty() &&
        parser_options->oat_symbols.size() != oat_filenames_.size()) {
      Usage("--oat-file arguments do not match --oat-symbols arguments");
    }

    if (!image_filenames_.empty() && image_filenames_.size() != oat_filenames_.size()) {
      Usage("--oat-file arguments do not match --image arguments");
    }

    if (!IsBootImage() && boot_image_filename_.empty()) {
      DCHECK(!IsBootImageExtension());
      if (std::any_of(runtime_args_.begin(), runtime_args_.end(), [](std::string_view arg) {
            return arg.starts_with("-Xbootclasspath:");
          })) {
        LOG(WARNING) << "--boot-image is not specified while -Xbootclasspath is specified. Running "
                        "dex2oat in imageless mode";
      } else {
        boot_image_filename_ =
            GetDefaultBootImageLocation(android_root_, /*deny_art_apex_data_files=*/false);
      }
    }

    if (dex_filenames_.empty() && zip_fd_ == -1) {
      Usage("Input must be supplied with either --dex-file or --zip-fd");
    }

    if (!dex_filenames_.empty() && zip_fd_ != -1) {
      Usage("--dex-file should not be used with --zip-fd");
    }

    if (!dex_filenames_.empty() && !zip_location_.empty()) {
      Usage("--dex-file should not be used with --zip-location");
    }

    if (dex_locations_.empty()) {
      dex_locations_ = dex_filenames_;
    } else if (dex_locations_.size() != dex_filenames_.size()) {
      Usage("--dex-location arguments do not match --dex-file arguments");
    }

    if (!dex_filenames_.empty() && !oat_filenames_.empty()) {
      if (oat_filenames_.size() != 1 && oat_filenames_.size() != dex_filenames_.size()) {
        Usage("--oat-file arguments must be singular or match --dex-file arguments");
      }
    }

    if (!dex_fds_.empty() && dex_fds_.size() != dex_filenames_.size()) {
      Usage("--dex-fd arguments do not match --dex-file arguments");
    }

    if (zip_fd_ != -1 && zip_location_.empty()) {
      Usage("--zip-location should be supplied with --zip-fd");
    }

    if (boot_image_filename_.empty()) {
      if (image_base_ == 0) {
        Usage("Non-zero --base not specified for boot image");
      }
    } else {
      if (image_base_ != 0) {
        Usage("Non-zero --base specified for app image or boot image extension");
      }
    }

    if (have_multi_image_arg_) {
      if (!IsImage()) {
        Usage("--multi-image or --single-image specified for non-image compilation");
      }
    } else {
      // Use the default, i.e. multi-image for boot image and boot image extension.
      // This shall pass the checks below.
      compiler_options_->multi_image_ = IsBootImage() || IsBootImageExtension();
    }
    // On target we support generating a single image for the primary boot image.
    if (!kIsTargetBuild && !force_allow_oj_inlines_) {
      if (IsBootImage() && !compiler_options_->multi_image_) {
        Usage(
            "--single-image specified for primary boot image on host. Please "
            "use the flag --force-allow-oj-inlines and do not distribute "
            "binaries.");
      }
    }
    if (IsAppImage() && compiler_options_->multi_image_) {
      Usage("--multi-image specified for app image");
    }

    if (image_fd_ != -1 && compiler_options_->multi_image_) {
      Usage("--single-image not specified for --image-fd");
    }

    const bool have_profile_file = !profile_files_.empty();
    const bool have_profile_fd = !profile_file_fds_.empty();
    if (have_profile_file && have_profile_fd) {
      Usage("Profile files should not be specified with both --profile-file-fd and --profile-file");
    }

    if (!parser_options->oat_symbols.empty()) {
      oat_unstripped_ = std::move(parser_options->oat_symbols);
    }

    if (compiler_options_->instruction_set_features_ == nullptr) {
      // '--instruction-set-features/--instruction-set-variant' were not used.
      // Use features for the 'default' variant.
      compiler_options_->instruction_set_features_ = InstructionSetFeatures::FromVariant(
          compiler_options_->instruction_set_, "default", &parser_options->error_msg);
      if (compiler_options_->instruction_set_features_ == nullptr) {
        Usage("Problem initializing default instruction set features variant: %s",
              parser_options->error_msg.c_str());
      }
    }

    if (compiler_options_->instruction_set_ == kRuntimeISA) {
      std::unique_ptr<const InstructionSetFeatures> runtime_features(
          InstructionSetFeatures::FromCppDefines());
      if (kRuntimeISA == InstructionSet::kArm64) {
         std::unique_ptr<const InstructionSetFeatures> arm64_runtime_features(
             InstructionSetFeatures::FromRuntimeDetection());
         if (arm64_runtime_features != nullptr) {
           runtime_features = std::move(arm64_runtime_features);
         }
      }
      if (!compiler_options_->GetInstructionSetFeatures()->Equals(runtime_features.get())) {
        LOG(WARNING) << "Mismatch between dex2oat instruction set features to use ("
            << *compiler_options_->GetInstructionSetFeatures()
            << ") and those from CPP defines (" << *runtime_features
            << ") for the command line:\n" << CommandLine();
      }
    }

    if (!dirty_image_objects_filenames_.empty() && !dirty_image_objects_fds_.empty()) {
      Usage("--dirty-image-objects and --dirty-image-objects-fd should not be both specified");
    }

    if (!preloaded_classes_files_.empty() && !preloaded_classes_fds_.empty()) {
      Usage("--preloaded-classes and --preloaded-classes-fds should not be both specified");
    }

    if (!cpu_set_.empty()) {
      SetCpuAffinity(cpu_set_);
    }

    if (compiler_options_->inline_max_code_units_ == CompilerOptions::kUnsetInlineMaxCodeUnits) {
      compiler_options_->inline_max_code_units_ = CompilerOptions::kDefaultInlineMaxCodeUnits;
    }

    // Checks are all explicit until we know the architecture.
    // Set the compilation target's implicit checks options.
    switch (compiler_options_->GetInstructionSet()) {
      case InstructionSet::kArm64:
        compiler_options_->implicit_suspend_checks_ = true;
        FALLTHROUGH_INTENDED;
      case InstructionSet::kArm:
      case InstructionSet::kThumb2:
      case InstructionSet::kRiscv64:
      case InstructionSet::kX86:
      case InstructionSet::kX86_64:
        compiler_options_->implicit_null_checks_ = true;
        compiler_options_->implicit_so_checks_ = true;
        break;

      default:
        // Defaults are correct.
        break;
    }

#ifdef ART_USE_RESTRICTED_MODE
    // TODO(Simulator): support signal handling and implicit checks.
    compiler_options_->implicit_suspend_checks_ = false;
    compiler_options_->implicit_null_checks_ = false;
#endif  // ART_USE_RESTRICTED_MODE

    // Done with usage checks, enable watchdog if requested
    if (parser_options->watch_dog_enabled) {
      int64_t timeout = parser_options->watch_dog_timeout_in_ms > 0
                            ? parser_options->watch_dog_timeout_in_ms
                            : WatchDog::kDefaultWatchdogTimeoutInMS;
      watchdog_.reset(new WatchDog(timeout));
    }

    // Fill some values into the key-value store for the oat header.
    key_value_store_.reset(new linker::OatKeyValueStore());

    // Automatically force determinism for the boot image and boot image extensions in a host build.
    if (!kIsTargetBuild && (IsBootImage() || IsBootImageExtension())) {
      force_determinism_ = true;
    }
    compiler_options_->force_determinism_ = force_determinism_;

    compiler_options_->check_linkage_conditions_ = check_linkage_conditions_;
    compiler_options_->crash_on_linkage_violation_ = crash_on_linkage_violation_;

    if (passes_to_run_filename_ != nullptr) {
      passes_to_run_ = ReadCommentedInputFromFile<std::vector<std::string>>(
          passes_to_run_filename_,
          nullptr);         // No post-processing.
      if (passes_to_run_.get() == nullptr) {
        Usage("Failed to read list of passes to run.");
      }
    }

    // Prune profile specifications of the boot image location.
    std::vector<std::string> boot_images =
        android::base::Split(boot_image_filename_, {ImageSpace::kComponentSeparator});
    bool boot_image_filename_pruned = false;
    for (std::string& boot_image : boot_images) {
      size_t profile_separator_pos = boot_image.find(ImageSpace::kProfileSeparator);
      if (profile_separator_pos != std::string::npos) {
        boot_image.resize(profile_separator_pos);
        boot_image_filename_pruned = true;
      }
    }
    if (boot_image_filename_pruned) {
      std::string new_boot_image_filename =
          android::base::Join(boot_images, ImageSpace::kComponentSeparator);
      VLOG(compiler) << "Pruning profile specifications of the boot image location. Before: "
                     << boot_image_filename_ << ", After: " << new_boot_image_filename;
      boot_image_filename_ = std::move(new_boot_image_filename);
    }

    compiler_options_->passes_to_run_ = passes_to_run_.get();
  }

  void ExpandOatAndImageFilenames() {
    ArrayRef<const std::string> locations(dex_locations_);
    if (!compiler_options_->multi_image_) {
      locations = locations.SubArray(/*pos=*/ 0u, /*length=*/ 1u);
    }
    if (image_fd_ == -1) {
      if (image_filenames_[0].rfind('/') == std::string::npos) {
        Usage("Unusable boot image filename %s", image_filenames_[0].c_str());
      }
      image_filenames_ = ImageSpace::ExpandMultiImageLocations(
          locations, image_filenames_[0], IsBootImageExtension());

      if (oat_filenames_[0].rfind('/') == std::string::npos) {
        Usage("Unusable boot image oat filename %s", oat_filenames_[0].c_str());
      }
      oat_filenames_ = ImageSpace::ExpandMultiImageLocations(
          locations, oat_filenames_[0], IsBootImageExtension());
    } else {
      DCHECK(!compiler_options_->multi_image_);
      std::vector<std::string> oat_locations = ImageSpace::ExpandMultiImageLocations(
          locations, oat_location_, IsBootImageExtension());
      DCHECK_EQ(1u, oat_locations.size());
      oat_location_ = oat_locations[0];
    }

    if (!oat_unstripped_.empty()) {
      if (oat_unstripped_[0].rfind('/') == std::string::npos) {
        Usage("Unusable boot image symbol filename %s", oat_unstripped_[0].c_str());
      }
      oat_unstripped_ = ImageSpace::ExpandMultiImageLocations(
           locations, oat_unstripped_[0], IsBootImageExtension());
    }
  }

  void InsertCompileOptions(int argc, char** argv) {
    if (!avoid_storing_invocation_) {
      std::ostringstream oss;
      for (int i = 0; i < argc; ++i) {
        if (i > 0) {
          oss << ' ';
        }
        oss << argv[i];
      }
      key_value_store_->PutNonDeterministic(
          OatHeader::kDex2OatCmdLineKey, oss.str(), /*allow_truncation=*/true);
    }
    key_value_store_->Put(OatHeader::kDebuggableKey, compiler_options_->debuggable_);
    key_value_store_->Put(OatHeader::kNativeDebuggableKey,
                          compiler_options_->GetNativeDebuggable());
    key_value_store_->Put(OatHeader::kCompilerFilter,
                          CompilerFilter::NameOfFilter(compiler_options_->GetCompilerFilter()));
    key_value_store_->Put(OatHeader::kConcurrentCopying, compiler_options_->EmitReadBarrier());
    if (invocation_file_.get() != -1) {
      std::ostringstream oss;
      for (int i = 0; i < argc; ++i) {
        if (i > 0) {
          oss << std::endl;
        }
        oss << argv[i];
      }
      std::string invocation(oss.str());
      if (TEMP_FAILURE_RETRY(write(invocation_file_.get(),
                                   invocation.c_str(),
                                   invocation.size())) == -1) {
        Usage("Unable to write invocation file");
      }
    }
  }

  // This simple forward is here so the string specializations below don't look out of place.
  template <typename T, typename U>
  void AssignIfExists(Dex2oatArgumentMap& map,
                      const Dex2oatArgumentMap::Key<T>& key,
                      U* out) {
    map.AssignIfExists(key, out);
  }

  // Specializations to handle const char* vs std::string.
  void AssignIfExists(Dex2oatArgumentMap& map,
                      const Dex2oatArgumentMap::Key<std::string>& key,
                      const char** out) {
    if (map.Exists(key)) {
      char_backing_storage_.push_front(std::move(*map.Get(key)));
      *out = char_backing_storage_.front().c_str();
    }
  }
  void AssignIfExists(Dex2oatArgumentMap& map,
                      const Dex2oatArgumentMap::Key<std::vector<std::string>>& key,
                      std::vector<const char*>* out) {
    if (map.Exists(key)) {
      for (auto& val : *map.Get(key)) {
        char_backing_storage_.push_front(std::move(val));
        out->push_back(char_backing_storage_.front().c_str());
      }
    }
  }

  template <typename T>
  void AssignTrueIfExists(Dex2oatArgumentMap& map,
                          const Dex2oatArgumentMap::Key<T>& key,
                          bool* out) {
    if (map.Exists(key)) {
      *out = true;
    }
  }

  void AssignIfExists(Dex2oatArgumentMap& map,
                      const Dex2oatArgumentMap::Key<std::string>& key,
                      std::vector<std::string>* out) {
    DCHECK(out->empty());
    if (map.Exists(key)) {
      out->push_back(*map.Get(key));
    }
  }

  // Parse the arguments from the command line. In case of an unrecognized option or impossible
  // values/combinations, a usage error will be displayed and exit() is called. Thus, if the method
  // returns, arguments have been successfully parsed.
  void ParseArgs(int argc, char** argv) {
    original_argc = argc;
    original_argv = argv;

    Locks::Init();
    InitLogging(argv, Runtime::Abort);

    compiler_options_.reset(new CompilerOptions());

    using M = Dex2oatArgumentMap;
    std::string error_msg;
    std::unique_ptr<M> args_uptr = M::Parse(argc, const_cast<const char**>(argv), &error_msg);
    if (args_uptr == nullptr) {
      Usage("Failed to parse command line: %s", error_msg.c_str());
      UNREACHABLE();
    }

    M& args = *args_uptr;

    std::string compact_dex_level;
    std::unique_ptr<ParserOptions> parser_options(new ParserOptions());

    AssignIfExists(args, M::CompactDexLevel, &compact_dex_level);
    AssignIfExists(args, M::DexFiles, &dex_filenames_);
    AssignIfExists(args, M::DexLocations, &dex_locations_);
    AssignIfExists(args, M::DexFds, &dex_fds_);
    AssignIfExists(args, M::OatFile, &oat_filenames_);
    AssignIfExists(args, M::OatSymbols, &parser_options->oat_symbols);
    AssignTrueIfExists(args, M::Strip, &strip_);
    AssignIfExists(args, M::ImageFilename, &image_filenames_);
    AssignIfExists(args, M::ImageFd, &image_fd_);
    AssignIfExists(args, M::ZipFd, &zip_fd_);
    AssignIfExists(args, M::ZipLocation, &zip_location_);
    AssignIfExists(args, M::InputVdexFd, &input_vdex_fd_);
    AssignIfExists(args, M::OutputVdexFd, &output_vdex_fd_);
    AssignIfExists(args, M::InputVdex, &input_vdex_);
    AssignIfExists(args, M::OutputVdex, &output_vdex_);
    AssignIfExists(args, M::DmFd, &dm_fd_);
    AssignIfExists(args, M::DmFile, &dm_file_location_);
    AssignIfExists(args, M::OatFd, &oat_fd_);
    AssignIfExists(args, M::OatLocation, &oat_location_);
    AssignIfExists(args, M::Watchdog, &parser_options->watch_dog_enabled);
    AssignIfExists(args, M::WatchdogTimeout, &parser_options->watch_dog_timeout_in_ms);
    AssignIfExists(args, M::Threads, &thread_count_);
    AssignIfExists(args, M::CpuSet, &cpu_set_);
    AssignIfExists(args, M::Passes, &passes_to_run_filename_);
    AssignIfExists(args, M::BootImage, &parser_options->boot_image_filename);
    AssignIfExists(args, M::AndroidRoot, &android_root_);
    AssignIfExists(args, M::Profile, &profile_files_);
    AssignIfExists(args, M::ProfileFd, &profile_file_fds_);
    AssignIfExists(args, M::PreloadedClasses, &preloaded_classes_files_);
    AssignIfExists(args, M::PreloadedClassesFds, &preloaded_classes_fds_);
    AssignIfExists(args, M::RuntimeOptions, &runtime_args_);
    AssignIfExists(args, M::SwapFile, &swap_file_name_);
    AssignIfExists(args, M::SwapFileFd, &swap_fd_);
    AssignIfExists(args, M::SwapDexSizeThreshold, &min_dex_file_cumulative_size_for_swap_);
    AssignIfExists(args, M::SwapDexCountThreshold, &min_dex_files_for_swap_);
    AssignIfExists(args, M::VeryLargeAppThreshold, &very_large_threshold_);
    AssignIfExists(args, M::AppImageFile, &app_image_file_name_);
    AssignIfExists(args, M::AppImageFileFd, &app_image_fd_);
    AssignIfExists(args, M::NoInlineFrom, &no_inline_from_string_);
    AssignIfExists(args, M::ClasspathDir, &classpath_dir_);
    AssignIfExists(args, M::DirtyImageObjects, &dirty_image_objects_filenames_);
    AssignIfExists(args, M::DirtyImageObjectsFd, &dirty_image_objects_fds_);
    AssignIfExists(args, M::ImageFormat, &image_storage_mode_);
    AssignIfExists(args, M::CompilationReason, &compilation_reason_);
    AssignTrueIfExists(args, M::CheckLinkageConditions, &check_linkage_conditions_);
    AssignTrueIfExists(args, M::CrashOnLinkageViolation, &crash_on_linkage_violation_);
    AssignTrueIfExists(args, M::ForceAllowOjInlines, &force_allow_oj_inlines_);
    AssignIfExists(args, M::PublicSdk, &public_sdk_);
    AssignIfExists(args, M::ApexVersions, &apex_versions_argument_);

    if (!compact_dex_level.empty()) {
      LOG(WARNING) << "Obsolete flag --compact-dex-level ignored";
    }

    AssignIfExists(args, M::TargetInstructionSet, &compiler_options_->instruction_set_);
    // arm actually means thumb2.
    if (compiler_options_->instruction_set_ == InstructionSet::kArm) {
      compiler_options_->instruction_set_ = InstructionSet::kThumb2;
    }

    AssignTrueIfExists(args, M::Host, &is_host_);
    AssignTrueIfExists(args, M::AvoidStoringInvocation, &avoid_storing_invocation_);
    if (args.Exists(M::InvocationFile)) {
      invocation_file_.reset(open(args.Get(M::InvocationFile)->c_str(),
                                  O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,
                                  S_IRUSR|S_IWUSR));
      if (invocation_file_.get() == -1) {
        int err = errno;
        Usage("Unable to open invocation file '%s' for writing due to %s.",
              args.Get(M::InvocationFile)->c_str(), strerror(err));
      }
    }
    AssignIfExists(args, M::CopyDexFiles, &copy_dex_files_);

    AssignTrueIfExists(args, M::MultiImage, &have_multi_image_arg_);
    AssignIfExists(args, M::MultiImage, &compiler_options_->multi_image_);

    if (args.Exists(M::ForceDeterminism)) {
      force_determinism_ = true;
    }
    AssignTrueIfExists(args, M::CompileIndividually, &compile_individually_);

    if (args.Exists(M::Base)) {
      ParseBase(*args.Get(M::Base));
    }
    if (args.Exists(M::TargetInstructionSetVariant)) {
      ParseInstructionSetVariant(*args.Get(M::TargetInstructionSetVariant), parser_options.get());
    }
    if (args.Exists(M::TargetInstructionSetFeatures)) {
      ParseInstructionSetFeatures(*args.Get(M::TargetInstructionSetFeatures), parser_options.get());
    }
    if (args.Exists(M::ClassLoaderContext)) {
      std::string class_loader_context_arg = *args.Get(M::ClassLoaderContext);
      class_loader_context_ = ClassLoaderContext::Create(class_loader_context_arg);
      if (class_loader_context_ == nullptr) {
        Usage("Option --class-loader-context has an incorrect format: %s",
              class_loader_context_arg.c_str());
      }
      if (args.Exists(M::ClassLoaderContextFds)) {
        std::string str_fds_arg = *args.Get(M::ClassLoaderContextFds);
        std::vector<std::string> str_fds = android::base::Split(str_fds_arg, ":");
        for (const std::string& str_fd : str_fds) {
          class_loader_context_fds_.push_back(std::stoi(str_fd, nullptr, 0));
          if (class_loader_context_fds_.back() < 0) {
            Usage("Option --class-loader-context-fds has incorrect format: %s",
                str_fds_arg.c_str());
          }
        }
      }
      if (args.Exists(M::StoredClassLoaderContext)) {
        const std::string stored_context_arg = *args.Get(M::StoredClassLoaderContext);
        stored_class_loader_context_ = ClassLoaderContext::Create(stored_context_arg);
        if (stored_class_loader_context_ == nullptr) {
          Usage("Option --stored-class-loader-context has an incorrect format: %s",
                stored_context_arg.c_str());
        } else if (class_loader_context_->VerifyClassLoaderContextMatch(
            stored_context_arg,
            /*verify_names*/ false,
            /*verify_checksums*/ false) != ClassLoaderContext::VerificationResult::kVerifies) {
          Usage(
              "Option --stored-class-loader-context '%s' mismatches --class-loader-context '%s'",
              stored_context_arg.c_str(),
              class_loader_context_arg.c_str());
        }
      }
    } else if (args.Exists(M::StoredClassLoaderContext)) {
      Usage("Option --stored-class-loader-context should only be used if "
            "--class-loader-context is also specified");
    }

    if (args.Exists(M::UpdatableBcpPackagesFile)) {
      LOG(WARNING)
          << "Option --updatable-bcp-packages-file is deprecated and no longer takes effect";
    }

    if (args.Exists(M::UpdatableBcpPackagesFd)) {
      LOG(WARNING) << "Option --updatable-bcp-packages-fd is deprecated and no longer takes effect";
    }

    if (args.Exists(M::ForceJitZygote)) {
      if (!parser_options->boot_image_filename.empty()) {
        Usage("Option --boot-image and --force-jit-zygote cannot be specified together");
      }
      parser_options->boot_image_filename = GetJitZygoteBootImageLocation();
    }

    // If we have a profile, change the default compiler filter to speed-profile
    // before reading compiler options.
    static_assert(CompilerFilter::kDefaultCompilerFilter == CompilerFilter::kSpeed);
    DCHECK_EQ(compiler_options_->GetCompilerFilter(), CompilerFilter::kSpeed);
    if (HasProfileInput()) {
      compiler_options_->SetCompilerFilter(CompilerFilter::kSpeedProfile);
    }

    if (!ReadCompilerOptions(args, compiler_options_.get(), &error_msg)) {
      Usage(error_msg.c_str());
    }

    if (!compiler_options_->GetDumpCfgFileName().empty() && thread_count_ != 1) {
      LOG(INFO) << "Since we are dumping the CFG to " << compiler_options_->GetDumpCfgFileName()
                << ", we override thread number to 1 to have determinism. It was " << thread_count_
                << ".";
      thread_count_ = 1;
    }

    PaletteShouldReportDex2oatCompilation(&should_report_dex2oat_compilation_);
    AssignTrueIfExists(args, M::ForcePaletteCompilationHooks, &should_report_dex2oat_compilation_);

    ProcessOptions(parser_options.get());
  }

  // Check whether the oat output files are writable, and open them for later. Also open a swap
  // file, if a name is given.
  bool OpenFile() {
    // Prune non-existent dex files now so that we don't create empty oat files for multi-image.
    PruneNonExistentDexFiles();

    // Expand oat and image filenames for boot image and boot image extension.
    // This is mostly for multi-image but single-image also needs some processing.
    if (IsBootImage() || IsBootImageExtension()) {
      ExpandOatAndImageFilenames();
    }

    // OAT and VDEX file handling
    if (oat_fd_ == -1) {
      DCHECK(!oat_filenames_.empty());
      for (const std::string& oat_filename : oat_filenames_) {
        std::unique_ptr<File> oat_file(OS::CreateEmptyFile(oat_filename.c_str()));
        if (oat_file == nullptr) {
          PLOG(ERROR) << "Failed to create oat file: " << oat_filename;
          return false;
        }
        if (fchmod(oat_file->Fd(), 0644) != 0) {
          PLOG(ERROR) << "Failed to make oat file world readable: " << oat_filename;
          oat_file->Erase();
          return false;
        }
        oat_files_.push_back(std::move(oat_file));
        DCHECK_EQ(input_vdex_fd_, -1);
        if (!input_vdex_.empty()) {
          std::string error_msg;
          input_vdex_file_ = VdexFile::Open(input_vdex_,
                                            /*low_4gb=*/false,
                                            &error_msg);
        }

        DCHECK_EQ(output_vdex_fd_, -1);
        std::string vdex_filename = output_vdex_.empty() ?
                                        ReplaceFileExtension(oat_filename, kVdexExtension) :
                                        output_vdex_;
        if (vdex_filename == input_vdex_ && output_vdex_.empty()) {
          use_existing_vdex_ = true;
          std::unique_ptr<File> vdex_file(OS::OpenFileForReading(vdex_filename.c_str()));
          vdex_files_.push_back(std::move(vdex_file));
        } else {
          std::unique_ptr<File> vdex_file(OS::CreateEmptyFile(vdex_filename.c_str()));
          if (vdex_file == nullptr) {
            PLOG(ERROR) << "Failed to open vdex file: " << vdex_filename;
            return false;
          }
          if (fchmod(vdex_file->Fd(), 0644) != 0) {
            PLOG(ERROR) << "Failed to make vdex file world readable: " << vdex_filename;
            vdex_file->Erase();
            return false;
          }
          vdex_files_.push_back(std::move(vdex_file));
        }
      }
    } else {
      std::unique_ptr<File> oat_file(
          new File(DupCloexec(oat_fd_), oat_location_, /* check_usage */ true));
      if (!oat_file->IsOpened()) {
        PLOG(ERROR) << "Failed to create oat file: " << oat_location_;
        return false;
      }
      if (oat_file->SetLength(0) != 0) {
        PLOG(WARNING) << "Truncating oat file " << oat_location_ << " failed.";
        oat_file->Erase();
        return false;
      }
      oat_files_.push_back(std::move(oat_file));

      if (input_vdex_fd_ != -1) {
        struct stat s;
        int rc = TEMP_FAILURE_RETRY(fstat(input_vdex_fd_, &s));
        if (rc == -1) {
          PLOG(WARNING) << "Failed getting length of vdex file";
        } else {
          std::string error_msg;
          input_vdex_file_ = VdexFile::Open(input_vdex_fd_,
                                            s.st_size,
                                            "vdex",
                                            /*low_4gb=*/false,
                                            &error_msg);
          // If there's any problem with the passed vdex, just warn and proceed
          // without it.
          if (input_vdex_file_ == nullptr) {
            PLOG(WARNING) << "Failed opening vdex file: " << error_msg;
          }
        }
      }

      DCHECK_NE(output_vdex_fd_, -1);
      std::string vdex_location = ReplaceFileExtension(oat_location_, kVdexExtension);
      if (input_vdex_file_ != nullptr && output_vdex_fd_ == input_vdex_fd_) {
        use_existing_vdex_ = true;
      }

      std::unique_ptr<File> vdex_file(new File(DupCloexec(output_vdex_fd_),
                                               vdex_location,
                                               /* check_usage= */ true,
                                               /* read_only_mode= */ use_existing_vdex_));
      if (!vdex_file->IsOpened()) {
        PLOG(ERROR) << "Failed to create vdex file: " << vdex_location;
        return false;
      }

      if (!use_existing_vdex_) {
        if (vdex_file->SetLength(0) != 0) {
          PLOG(ERROR) << "Truncating vdex file " << vdex_location << " failed.";
          vdex_file->Erase();
          return false;
        }
      }
      vdex_files_.push_back(std::move(vdex_file));

      oat_filenames_.push_back(oat_location_);
    }

    if (dm_fd_ != -1 || !dm_file_location_.empty()) {
      std::string error_msg;
      if (dm_fd_ != -1) {
        dm_file_.reset(ZipArchive::OpenFromFd(dm_fd_, "DexMetadata", &error_msg));
      } else {
        dm_file_.reset(ZipArchive::Open(dm_file_location_.c_str(), &error_msg));
      }
      if (dm_file_ == nullptr) {
        LOG(WARNING) << "Could not open DexMetadata archive " << error_msg;
      }
    }

    // If we have a dm file and a vdex file, we (arbitrarily) pick the vdex file.
    // In theory the files should be the same.
    if (dm_file_ != nullptr) {
      if (input_vdex_file_ == nullptr) {
        std::string error_msg;
        input_vdex_file_ = VdexFile::OpenFromDm(dm_file_location_, *dm_file_, &error_msg);
        if (input_vdex_file_ != nullptr) {
          VLOG(verifier) << "Doing fast verification with vdex from DexMetadata archive";
        } else {
          LOG(WARNING) << error_msg;
        }
      } else {
        LOG(INFO) << "Ignoring vdex file in dex metadata due to vdex file already being passed";
      }
    }

    // Swap file handling
    //
    // If the swap fd is not -1, we assume this is the file descriptor of an open but unlinked file
    // that we can use for swap.
    //
    // If the swap fd is -1 and we have a swap-file string, open the given file as a swap file. We
    // will immediately unlink to satisfy the swap fd assumption.
    if (swap_fd_ == -1 && !swap_file_name_.empty()) {
      std::unique_ptr<File> swap_file(OS::CreateEmptyFile(swap_file_name_.c_str()));
      if (swap_file.get() == nullptr) {
        PLOG(ERROR) << "Failed to create swap file: " << swap_file_name_;
        return false;
      }
      swap_fd_ = swap_file->Release();
      unlink(swap_file_name_.c_str());
    }

    return true;
  }

  void EraseOutputFiles() {
    for (auto& files : { &vdex_files_, &oat_files_ }) {
      for (size_t i = 0; i < files->size(); ++i) {
        auto& file = (*files)[i];
        if (file != nullptr) {
          if (!file->ReadOnlyMode()) {
            file->Erase();
          }
          file.reset();
        }
      }
    }
  }

  void LoadImageClassDescriptors() {
    if (!IsImage()) {
      return;
    }
    HashSet<std::string> image_classes;
    if (DoProfileGuidedOptimizations()) {
      // TODO: The following comment looks outdated or misplaced.
      // Filter out class path classes since we don't want to include these in the image.
      image_classes = profile_compilation_info_->GetClassDescriptors(
          compiler_options_->dex_files_for_oat_file_);
      VLOG(compiler) << "Loaded " << image_classes.size()
                     << " image class descriptors from profile";
    } else if (compiler_options_->IsBootImage() || compiler_options_->IsBootImageExtension()) {
      // If we are compiling a boot image but no profile is provided, include all classes in the
      // image. This is to match pre-boot image extension work where we would load all boot image
      // extension classes at startup.
      for (const DexFile* dex_file : compiler_options_->dex_files_for_oat_file_) {
        for (uint32_t i = 0; i < dex_file->NumClassDefs(); i++) {
          const dex::ClassDef& class_def = dex_file->GetClassDef(i);
          const char* descriptor = dex_file->GetClassDescriptor(class_def);
          image_classes.insert(descriptor);
        }
      }
    }
    if (VLOG_IS_ON(compiler)) {
      for (const std::string& s : image_classes) {
        LOG(INFO) << "Image class " << s;
      }
    }
    compiler_options_->image_classes_ = std::move(image_classes);
  }

  // Set up the environment for compilation. Includes starting the runtime and loading/opening the
  // boot class path.
  dex2oat::ReturnCode Setup() {
    TimingLogger::ScopedTiming t("dex2oat Setup", timings_);

    if (!PrepareDirtyObjects()) {
      return dex2oat::ReturnCode::kOther;
    }

    if (!PreparePreloadedClasses()) {
      return dex2oat::ReturnCode::kOther;
    }

    callbacks_.reset(new QuickCompilerCallbacks(
        // For class verification purposes, boot image extension is the same as boot image.
        (IsBootImage() || IsBootImageExtension())
            ? CompilerCallbacks::CallbackMode::kCompileBootImage
            : CompilerCallbacks::CallbackMode::kCompileApp));

    RuntimeArgumentMap runtime_options;
    if (!PrepareRuntimeOptions(&runtime_options, callbacks_.get())) {
      return dex2oat::ReturnCode::kOther;
    }

    CreateOatWriters();
    if (!AddDexFileSources()) {
      return dex2oat::ReturnCode::kOther;
    }

    {
      TimingLogger::ScopedTiming t_dex("Writing and opening dex files", timings_);
      for (size_t i = 0, size = oat_writers_.size(); i != size; ++i) {
        // Unzip or copy dex files straight to the oat file.
        std::vector<MemMap> opened_dex_files_map;
        std::vector<std::unique_ptr<const DexFile>> opened_dex_files;
        // No need to verify the dex file when we have a vdex file, which means it was already
        // verified.
        const bool verify =
            (input_vdex_file_ == nullptr) && !compiler_options_->AssumeDexFilesAreVerified();
        if (!oat_writers_[i]->WriteAndOpenDexFiles(
            vdex_files_[i].get(),
            verify,
            use_existing_vdex_,
            copy_dex_files_,
            &opened_dex_files_map,
            &opened_dex_files)) {
          return dex2oat::ReturnCode::kOther;
        }
        dex_files_per_oat_file_.push_back(MakeNonOwningPointerVector(opened_dex_files));
        for (MemMap& map : opened_dex_files_map) {
          opened_dex_files_maps_.push_back(std::move(map));
        }
        for (std::unique_ptr<const DexFile>& dex_file : opened_dex_files) {
          dex_file_oat_index_map_.insert(std::make_pair(dex_file.get(), i));
          opened_dex_files_.push_back(std::move(dex_file));
        }
      }
    }

    compiler_options_->dex_files_for_oat_file_ = MakeNonOwningPointerVector(opened_dex_files_);
    const std::vector<const DexFile*>& dex_files = compiler_options_->dex_files_for_oat_file_;

    if (!ValidateInputVdexChecksums()) {
       return dex2oat::ReturnCode::kOther;
    }

    // Check if we need to downgrade the compiler-filter for size reasons.
    // Note: This does not affect the compiler filter already stored in the key-value
    //       store which is used for determining whether the oat file is up to date,
    //       together with the boot class path locations and checksums stored below.
    CompilerFilter::Filter original_compiler_filter = compiler_options_->GetCompilerFilter();
    if (!IsBootImage() && !IsBootImageExtension() && IsVeryLarge(dex_files)) {
      // Disable app image to make sure dex2oat unloading is enabled.
      compiler_options_->image_type_ = CompilerOptions::ImageType::kNone;

      // If we need to downgrade the compiler-filter for size reasons, do that early before we read
      // it below for creating verification callbacks.
      if (!CompilerFilter::IsAsGoodAs(kLargeAppFilter, compiler_options_->GetCompilerFilter())) {
        LOG(INFO) << "Very large app, downgrading to verify.";
        compiler_options_->SetCompilerFilter(kLargeAppFilter);
      }
    }

    if (CompilerFilter::IsAnyCompilationEnabled(compiler_options_->GetCompilerFilter()) ||
        IsImage()) {
      // Only modes with compilation or image generation require verification results.
      verification_results_.reset(new VerificationResults());
      callbacks_->SetVerificationResults(verification_results_.get());
    }

    if (IsBootImage() || IsBootImageExtension()) {
      // For boot image or boot image extension, pass opened dex files to the Runtime::Create().
      // Note: Runtime acquires ownership of these dex files.
      runtime_options.Set(RuntimeArgumentMap::BootClassPathDexList, &opened_dex_files_);
    }
    if (!CreateRuntime(std::move(runtime_options))) {
      return dex2oat::ReturnCode::kCreateRuntime;
    }
    if (runtime_->GetHeap()->GetBootImageSpaces().empty() &&
        (IsBootImageExtension() || IsAppImage())) {
      LOG(WARNING) << "Cannot create "
                   << (IsBootImageExtension() ? "boot image extension" : "app image")
                   << " without a primary boot image.";
      compiler_options_->image_type_ = CompilerOptions::ImageType::kNone;
    }
    ArrayRef<const DexFile* const> bcp_dex_files(runtime_->GetClassLinker()->GetBootClassPath());
    if (IsBootImage() || IsBootImageExtension()) {
      // Check boot class path dex files and, if compiling an extension, the images it depends on.
      if ((IsBootImage() && bcp_dex_files.size() != dex_files.size()) ||
          (IsBootImageExtension() && bcp_dex_files.size() <= dex_files.size())) {
        LOG(ERROR) << "Unexpected number of boot class path dex files for boot image or extension, "
            << bcp_dex_files.size() << (IsBootImage() ? " != " : " <= ") << dex_files.size();
        return dex2oat::ReturnCode::kOther;
      }
      if (!std::equal(dex_files.begin(), dex_files.end(), bcp_dex_files.end() - dex_files.size())) {
        LOG(ERROR) << "Boot class path dex files do not end with the compiled dex files.";
        return dex2oat::ReturnCode::kOther;
      }
      size_t bcp_df_pos = 0u;
      size_t bcp_df_end = bcp_dex_files.size();
      for (const std::string& bcp_location : runtime_->GetBootClassPathLocations()) {
        if (bcp_df_pos == bcp_df_end || bcp_dex_files[bcp_df_pos]->GetLocation() != bcp_location) {
          LOG(ERROR) << "Missing dex file for boot class component " << bcp_location;
          return dex2oat::ReturnCode::kOther;
        }
        CHECK(!DexFileLoader::IsMultiDexLocation(bcp_dex_files[bcp_df_pos]->GetLocation()));
        ++bcp_df_pos;
        while (bcp_df_pos != bcp_df_end &&
            DexFileLoader::IsMultiDexLocation(bcp_dex_files[bcp_df_pos]->GetLocation())) {
          ++bcp_df_pos;
        }
      }
      if (bcp_df_pos != bcp_df_end) {
        LOG(ERROR) << "Unexpected dex file in boot class path "
            << bcp_dex_files[bcp_df_pos]->GetLocation();
        return dex2oat::ReturnCode::kOther;
      }
      auto lacks_image = [](const DexFile* df) {
        if (kIsDebugBuild && df->GetOatDexFile() != nullptr) {
          const OatFile* oat_file = df->GetOatDexFile()->GetOatFile();
          CHECK(oat_file != nullptr);
          const auto& image_spaces = Runtime::Current()->GetHeap()->GetBootImageSpaces();
          CHECK(std::any_of(image_spaces.begin(),
                            image_spaces.end(),
                            [=](const ImageSpace* space) {
                              return oat_file == space->GetOatFile();
                            }));
        }
        return df->GetOatDexFile() == nullptr;
      };
      if (std::any_of(bcp_dex_files.begin(), bcp_dex_files.end() - dex_files.size(), lacks_image)) {
        LOG(ERROR) << "Missing required boot image(s) for boot image extension.";
        return dex2oat::ReturnCode::kOther;
      }
    }

    if (!compilation_reason_.empty()) {
      key_value_store_->Put(OatHeader::kCompilationReasonKey, compilation_reason_);
    }

    Runtime* runtime = Runtime::Current();

    if (IsBootImage()) {
      // If we're compiling the boot image, store the boot classpath into the Key-Value store.
      // We use this when loading the boot image.
      key_value_store_->Put(OatHeader::kBootClassPathKey, android::base::Join(dex_locations_, ':'));
    } else if (IsBootImageExtension()) {
      // Validate the boot class path and record the dependency on the loaded boot images.
      TimingLogger::ScopedTiming t3("Loading image checksum", timings_);
      std::string full_bcp = android::base::Join(runtime->GetBootClassPathLocations(), ':');
      std::string extension_part = ":" + android::base::Join(dex_locations_, ':');
      if (!full_bcp.ends_with(extension_part)) {
        LOG(ERROR) << "Full boot class path does not end with extension parts, full: " << full_bcp
            << ", extension: " << extension_part.substr(1u);
        return dex2oat::ReturnCode::kOther;
      }
      std::string bcp_dependency = full_bcp.substr(0u, full_bcp.size() - extension_part.size());
      key_value_store_->Put(OatHeader::kBootClassPathKey, bcp_dependency);
      ArrayRef<const DexFile* const> bcp_dex_files_dependency =
          bcp_dex_files.SubArray(/*pos=*/ 0u, bcp_dex_files.size() - dex_files.size());
      ArrayRef<ImageSpace* const> image_spaces(runtime->GetHeap()->GetBootImageSpaces());
      key_value_store_->Put(
          OatHeader::kBootClassPathChecksumsKey,
          gc::space::ImageSpace::GetBootClassPathChecksums(image_spaces, bcp_dex_files_dependency));
    } else {
      if (CompilerFilter::DependsOnImageChecksum(original_compiler_filter)) {
        TimingLogger::ScopedTiming t3("Loading image checksum", timings_);
        key_value_store_->Put(OatHeader::kBootClassPathKey,
                              android::base::Join(runtime->GetBootClassPathLocations(), ':'));
        ArrayRef<ImageSpace* const> image_spaces(runtime->GetHeap()->GetBootImageSpaces());
        key_value_store_->Put(
            OatHeader::kBootClassPathChecksumsKey,
            gc::space::ImageSpace::GetBootClassPathChecksums(image_spaces, bcp_dex_files));
      }

      // Open dex files for class path.

      if (class_loader_context_ == nullptr) {
        // If no context was specified use the default one (which is an empty PathClassLoader).
        class_loader_context_ = ClassLoaderContext::Default();
      }

      DCHECK_EQ(oat_writers_.size(), 1u);

      // Note: Ideally we would reject context where the source dex files are also
      // specified in the classpath (as it doesn't make sense). However this is currently
      // needed for non-prebuild tests and benchmarks which expects on the fly compilation.
      // Also, for secondary dex files we do not have control on the actual classpath.
      // Instead of aborting, remove all the source location from the context classpaths.
      if (class_loader_context_->RemoveLocationsFromClassPaths(
            oat_writers_[0]->GetSourceLocations())) {
        LOG(WARNING) << "The source files to be compiled are also in the classpath.";
      }

      // We need to open the dex files before encoding the context in the oat file.
      // (because the encoding adds the dex checksum...)
      // TODO(calin): consider redesigning this so we don't have to open the dex files before
      // creating the actual class loader.
      if (!class_loader_context_->OpenDexFiles(classpath_dir_,
                                               class_loader_context_fds_)) {
        // Do not abort if we couldn't open files from the classpath. They might be
        // apks without dex files and right now are opening flow will fail them.
        LOG(WARNING) << "Failed to open classpath dex files";
      }

      // Store the class loader context in the oat header.
      // TODO: deprecate this since store_class_loader_context should be enough to cover the users
      // of classpath_dir as well.
      std::string class_path_key =
          class_loader_context_->EncodeContextForOatFile(classpath_dir_,
                                                         stored_class_loader_context_.get());
      key_value_store_->Put(OatHeader::kClassPathKey, class_path_key);
    }

    if (IsBootImage() ||
        IsBootImageExtension() ||
        CompilerFilter::DependsOnImageChecksum(original_compiler_filter)) {
      std::string versions =
          apex_versions_argument_.empty() ? runtime->GetApexVersions() : apex_versions_argument_;
      if (!key_value_store_->PutNonDeterministic(OatHeader::kApexVersionsKey, versions)) {
        LOG(ERROR) << "Cannot store apex versions string because it's too long";
        return dex2oat::ReturnCode::kOther;
      }
    }

    // Now that we have adjusted whether we generate an image, encode it in the
    // key/value store.
    key_value_store_->Put(OatHeader::kRequiresImage, compiler_options_->IsGeneratingImage());

    // Now that we have finalized key_value_store_, start writing the .rodata section.
    // Among other things, this creates type lookup tables that speed up the compilation.
    {
      TimingLogger::ScopedTiming t_dex("Starting .rodata", timings_);
      rodata_.reserve(oat_writers_.size());
      for (size_t i = 0, size = oat_writers_.size(); i != size; ++i) {
        rodata_.push_back(elf_writers_[i]->StartRoData());
        if (!oat_writers_[i]->StartRoData(dex_files_per_oat_file_[i],
                                          rodata_.back(),
                                          (i == 0u) ? key_value_store_.get() : nullptr)) {
          return dex2oat::ReturnCode::kOther;
        }
      }
    }

    // We had to postpone the swap decision till now, as this is the point when we actually
    // know about the dex files we're going to use.

    // Make sure that we didn't create the driver, yet.
    CHECK(driver_ == nullptr);
    // If we use a swap file, ensure we are above the threshold to make it necessary.
    if (swap_fd_ != -1) {
      if (!UseSwap(IsBootImage() || IsBootImageExtension(), dex_files)) {
        close(swap_fd_);
        swap_fd_ = -1;
        VLOG(compiler) << "Decided to run without swap.";
      } else {
        LOG(INFO) << "Large app, accepted running with swap.";
      }
    }
    // Note that dex2oat won't close the swap_fd_. The compiler driver's swap space will do that.

    if (!IsBootImage() && !IsBootImageExtension()) {
      constexpr bool kSaveDexInput = false;
      if (kSaveDexInput) {
        SaveDexInput();
      }
    }

    // Setup VerifierDeps for compilation and report if we fail to parse the data.
    if (input_vdex_file_ != nullptr) {
      TimingLogger::ScopedTiming t_dex("Parse Verifier Deps", timings_);
      std::unique_ptr<verifier::VerifierDeps> verifier_deps(
          new verifier::VerifierDeps(dex_files, /*output_only=*/ false));
      if (!verifier_deps->ParseStoredData(dex_files, input_vdex_file_->GetVerifierDepsData())) {
        return dex2oat::ReturnCode::kOther;
      }
      // We can do fast verification.
      callbacks_->SetVerifierDeps(verifier_deps.release());
    } else {
      // Create the main VerifierDeps, here instead of in the compiler since we want to aggregate
      // the results for all the dex files, not just the results for the current dex file.
      callbacks_->SetVerifierDeps(new verifier::VerifierDeps(dex_files));
    }

    return dex2oat::ReturnCode::kNoFailure;
  }

  // Validates that the input vdex checksums match the source dex checksums.
  // Note that this is only effective and relevant if the input_vdex_file does not
  // contain a dex section (e.g. when they come from .dm files).
  // If the input vdex does contain dex files, the dex files will be opened from there
  // and so this check is redundant.
  bool ValidateInputVdexChecksums() {
    if (input_vdex_file_ == nullptr) {
      // Nothing to validate
      return true;
    }
    if (input_vdex_file_->GetNumberOfDexFiles()
          != compiler_options_->dex_files_for_oat_file_.size()) {
      LOG(ERROR) << "Vdex file contains a different number of dex files than the source. "
          << " vdex_num=" << input_vdex_file_->GetNumberOfDexFiles()
          << " dex_source_num=" << compiler_options_->dex_files_for_oat_file_.size();
      return false;
    }

    for (size_t i = 0; i < compiler_options_->dex_files_for_oat_file_.size(); i++) {
      uint32_t dex_source_checksum =
          compiler_options_->dex_files_for_oat_file_[i]->GetLocationChecksum();
      uint32_t vdex_checksum = input_vdex_file_->GetLocationChecksum(i);
      if (dex_source_checksum != vdex_checksum) {
        LOG(ERROR) << "Vdex file checksum different than source dex checksum for position " << i
          << std::hex
          << " vdex_checksum=0x" << vdex_checksum
          << " dex_source_checksum=0x" << dex_source_checksum
          << std::dec;
        return false;
      }
    }
    return true;
  }

  // If we need to keep the oat file open for the image writer.
  bool ShouldKeepOatFileOpen() const {
    return IsImage() && oat_fd_ != File::kInvalidFd;
  }

  // Doesn't return the class loader since it's not meant to be used for image compilation.
  void CompileDexFilesIndividually() {
    CHECK(!IsImage()) << "Not supported with image";
    for (const DexFile* dex_file : compiler_options_->dex_files_for_oat_file_) {
      std::vector<const DexFile*> dex_files(1u, dex_file);
      VLOG(compiler) << "Compiling " << dex_file->GetLocation();
      jobject class_loader = CompileDexFiles(dex_files);
      CHECK(class_loader != nullptr);
      ScopedObjectAccess soa(Thread::Current());
      // Unload class loader to free RAM.
      jweak weak_class_loader = soa.Env()->GetVm()->AddWeakGlobalRef(
          soa.Self(),
          soa.Decode<mirror::ClassLoader>(class_loader));
      soa.Env()->GetVm()->DeleteGlobalRef(soa.Self(), class_loader);
      runtime_->GetHeap()->CollectGarbage(/* clear_soft_references */ true);
      ObjPtr<mirror::ClassLoader> decoded_weak = soa.Decode<mirror::ClassLoader>(weak_class_loader);
      if (decoded_weak != nullptr) {
        LOG(FATAL) << "Failed to unload class loader, path from root set: "
                   << runtime_->GetHeap()->GetVerification()->FirstPathFromRootSet(decoded_weak);
      }
      VLOG(compiler) << "Unloaded classloader";
    }
  }

  bool ShouldCompileDexFilesIndividually() const {
    // Compile individually if we are allowed to, and
    // 1. not building an image, and
    // 2. not verifying a vdex file, and
    // 3. using multidex, and
    // 4. not doing any AOT compilation.
    // This means no-vdex verify will use the individual compilation
    // mode (to reduce RAM used by the compiler).
    return compile_individually_ &&
           (!IsImage() && !use_existing_vdex_ &&
            compiler_options_->dex_files_for_oat_file_.size() > 1 &&
            !CompilerFilter::IsAotCompilationEnabled(compiler_options_->GetCompilerFilter()));
  }

  uint32_t GetCombinedChecksums() const {
    uint32_t combined_checksums = 0u;
    for (const DexFile* dex_file : compiler_options_->GetDexFilesForOatFile()) {
      combined_checksums ^= dex_file->GetLocationChecksum();
    }
    return combined_checksums;
  }

  // Set up and create the compiler driver and then invoke it to compile all the dex files.
  jobject Compile() REQUIRES(!Locks::mutator_lock_) {
    ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();

    TimingLogger::ScopedTiming t("dex2oat Compile", timings_);

    // Find the dex files we should not inline from.
    std::vector<std::string> no_inline_filters;
    Split(no_inline_from_string_, ',', &no_inline_filters);

    // For now, on the host always have core-oj removed.
    const std::string core_oj = "core-oj";
    if (!kIsTargetBuild && !ContainsElement(no_inline_filters, core_oj)) {
      if (force_allow_oj_inlines_) {
        LOG(ERROR) << "Inlines allowed from core-oj! FOR TESTING USE ONLY! DO NOT DISTRIBUTE"
                   << " BINARIES BUILT WITH THIS OPTION!";
      } else {
        no_inline_filters.push_back(core_oj);
      }
    }

    if (!no_inline_filters.empty()) {
      std::vector<const DexFile*> class_path_files;
      if (!IsBootImage() && !IsBootImageExtension()) {
        // The class loader context is used only for apps.
        class_path_files = class_loader_context_->FlattenOpenedDexFiles();
      }

      const std::vector<const DexFile*>& dex_files = compiler_options_->dex_files_for_oat_file_;
      std::vector<const DexFile*> no_inline_from_dex_files;
      const std::vector<const DexFile*>* dex_file_vectors[] = {
          &class_linker->GetBootClassPath(),
          &class_path_files,
          &dex_files
      };
      for (const std::vector<const DexFile*>* dex_file_vector : dex_file_vectors) {
        for (const DexFile* dex_file : *dex_file_vector) {
          for (const std::string& filter : no_inline_filters) {
            // Use dex_file->GetLocation() rather than dex_file->GetBaseLocation(). This
            // allows tests to specify <test-dexfile>!classes2.dex if needed but if the
            // base location passes the `starts_with()` test, so do all extra locations.
            std::string dex_location = dex_file->GetLocation();
            if (filter.find('/') == std::string::npos) {
              // The filter does not contain the path. Remove the path from dex_location as well.
              size_t last_slash = dex_file->GetLocation().rfind('/');
              if (last_slash != std::string::npos) {
                dex_location = dex_location.substr(last_slash + 1);
              }
            }

            if (dex_location.starts_with(filter)) {
              VLOG(compiler) << "Disabling inlining from " << dex_file->GetLocation();
              no_inline_from_dex_files.push_back(dex_file);
              break;
            }
          }
        }
      }
      if (!no_inline_from_dex_files.empty()) {
        compiler_options_->no_inline_from_.swap(no_inline_from_dex_files);
      }
    }
    compiler_options_->profile_compilation_info_ = profile_compilation_info_.get();

    driver_.reset(new CompilerDriver(compiler_options_.get(),
                                     verification_results_.get(),
                                     thread_count_,
                                     swap_fd_));

    driver_->PrepareDexFilesForOatFile(timings_);

    if (!IsBootImage() && !IsBootImageExtension()) {
      driver_->SetClasspathDexFiles(class_loader_context_->FlattenOpenedDexFiles());
    }

    const bool compile_individually = ShouldCompileDexFilesIndividually();
    if (compile_individually) {
      // Set the compiler driver in the callbacks so that we can avoid re-verification.
      // Only set the compiler filter if we are doing separate compilation since there is a bit
      // of overhead when checking if a class was previously verified.
      callbacks_->SetDoesClassUnloading(true, driver_.get());
    }

    // Setup vdex for compilation.
    const std::vector<const DexFile*>& dex_files = compiler_options_->dex_files_for_oat_file_;
    // To allow initialization of classes that construct ThreadLocal objects in class initializer,
    // re-initialize the ThreadLocal.nextHashCode to a new object that's not in the boot image.
    ThreadLocalHashOverride thread_local_hash_override(
        /*apply=*/ !IsBootImage(), /*initial_value=*/ 123456789u ^ GetCombinedChecksums());

    // Invoke the compilation.
    if (compile_individually) {
      CompileDexFilesIndividually();
      // Return a null classloader since we already freed released it.
      return nullptr;
    }
    return CompileDexFiles(dex_files);
  }

  // Create the class loader, use it to compile, and return.
  jobject CompileDexFiles(const std::vector<const DexFile*>& dex_files) {
    ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();

    jobject class_loader = nullptr;
    if (!IsBootImage() && !IsBootImageExtension()) {
      class_loader =
          class_loader_context_->CreateClassLoader(compiler_options_->GetDexFilesForOatFile());
    }
    if (!IsBootImage()) {
      callbacks_->SetDexFiles(&dex_files);

      // We need to set this after we create the class loader so that the runtime can access
      // the hidden fields of the well known class loaders.
      if (!public_sdk_.empty()) {
        std::string error_msg;
        std::unique_ptr<SdkChecker> sdk_checker(SdkChecker::Create(public_sdk_, &error_msg));
        if (sdk_checker != nullptr) {
          AotClassLinker* aot_class_linker = down_cast<AotClassLinker*>(class_linker);
          aot_class_linker->SetSdkChecker(std::move(sdk_checker));
        } else {
          LOG(FATAL) << "Failed to create SdkChecker with dex files "
              << public_sdk_ << " Error: " << error_msg;
          UNREACHABLE();
        }
      }
    }
    if (IsAppImage()) {
      AotClassLinker::SetAppImageDexFiles(&compiler_options_->GetDexFilesForOatFile());
    }

    // Register dex caches and key them to the class loader so that they only unload when the
    // class loader unloads.
    for (const auto& dex_file : dex_files) {
      ScopedObjectAccess soa(Thread::Current());
      // Registering the dex cache adds a strong root in the class loader that prevents the dex
      // cache from being unloaded early.
      ObjPtr<mirror::DexCache> dex_cache = class_linker->RegisterDexFile(
          *dex_file,
          soa.Decode<mirror::ClassLoader>(class_loader));
      if (dex_cache == nullptr) {
        soa.Self()->AssertPendingException();
        LOG(FATAL) << "Failed to register dex file " << dex_file->GetLocation() << " "
                   << soa.Self()->GetException()->Dump();
      }
    }
    driver_->InitializeThreadPools();
    driver_->PreCompile(class_loader,
                        dex_files,
                        timings_,
                        &compiler_options_->image_classes_);
    driver_->CompileAll(class_loader, dex_files, timings_);
    driver_->FreeThreadPools();
    return class_loader;
  }

  // Notes on the interleaving of creating the images and oat files to
  // ensure the references between the two are correct.
  //
  // Currently we have a memory layout that looks something like this:
  //
  // +--------------+
  // | images       |
  // +--------------+
  // | oat files    |
  // +--------------+
  // | alloc spaces |
  // +--------------+
  //
  // There are several constraints on the loading of the images and oat files.
  //
  // 1. The images are expected to be loaded at an absolute address and
  // contain Objects with absolute pointers within the images.
  //
  // 2. There are absolute pointers from Methods in the images to their
  // code in the oat files.
  //
  // 3. There are absolute pointers from the code in the oat files to Methods
  // in the images.
  //
  // 4. There are absolute pointers from code in the oat files to other code
  // in the oat files.
  //
  // To get this all correct, we go through several steps.
  //
  // 1. We prepare offsets for all data in the oat files and calculate
  // the oat data size and code size. During this stage, we also set
  // oat code offsets in methods for use by the image writer.
  //
  // 2. We prepare offsets for the objects in the images and calculate
  // the image sizes.
  //
  // 3. We create the oat files. Originally this was just our own proprietary
  // file but now it is contained within an ELF dynamic object (aka an .so
  // file). Since we know the image sizes and oat data sizes and code sizes we
  // can prepare the ELF headers and we then know the ELF memory segment
  // layout and we can now resolve all references. The compiler provides
  // LinkerPatch information in each CompiledMethod and we resolve these,
  // using the layout information and image object locations provided by
  // image writer, as we're writing the method code.
  //
  // 4. We create the image files. They need to know where the oat files
  // will be loaded after itself. Originally oat files were simply
  // memory mapped so we could predict where their contents were based
  // on the file size. Now that they are ELF files, we need to inspect
  // the ELF files to understand the in memory segment layout including
  // where the oat header is located within.
  // TODO: We could just remember this information from step 3.
  //
  // 5. We fixup the ELF program headers so that dlopen will try to
  // load the .so at the desired location at runtime by offsetting the
  // Elf32_Phdr.p_vaddr values by the desired base address.
  // TODO: Do this in step 3. We already know the layout there.
  //
  // Steps 1.-3. are done by the CreateOatFile() above, steps 4.-5.
  // are done by the CreateImageFile() below.

  // Write out the generated code part. Calls the OatWriter and ElfBuilder. Also prepares the
  // ImageWriter, if necessary.
  // Note: Flushing (and closing) the file is the caller's responsibility, except for the failure
  //       case (when the file will be explicitly erased).
  bool WriteOutputFiles(jobject class_loader) {
    TimingLogger::ScopedTiming t("dex2oat Oat", timings_);

    // Sync the data to the file, in case we did dex2dex transformations.
    for (MemMap& map : opened_dex_files_maps_) {
      if (!map.Sync()) {
        PLOG(ERROR) << "Failed to Sync() dex2dex output. Map: " << map.GetName();
        return false;
      }
    }

    if (IsImage()) {
      if (!IsBootImage()) {
        DCHECK_EQ(image_base_, 0u);
        gc::Heap* const heap = Runtime::Current()->GetHeap();
        image_base_ = heap->GetBootImagesStartAddress() + heap->GetBootImagesSize();
      }
      VLOG(compiler) << "Image base=" << reinterpret_cast<void*>(image_base_);

      image_writer_.reset(new linker::ImageWriter(*compiler_options_,
                                                  image_base_,
                                                  image_storage_mode_,
                                                  oat_filenames_,
                                                  dex_file_oat_index_map_,
                                                  class_loader,
                                                  dirty_image_objects_.get()));

      // We need to prepare method offsets in the image address space for resolving linker patches.
      TimingLogger::ScopedTiming t2("dex2oat Prepare image address space", timings_);
      if (!image_writer_->PrepareImageAddressSpace(timings_)) {
        LOG(ERROR) << "Failed to prepare image address space.";
        return false;
      }
    }

    // Initialize the writers with the compiler driver, image writer, and their
    // dex files. The writers were created without those being there yet.
    for (size_t i = 0, size = oat_files_.size(); i != size; ++i) {
      std::unique_ptr<linker::OatWriter>& oat_writer = oat_writers_[i];
      std::vector<const DexFile*>& dex_files = dex_files_per_oat_file_[i];
      oat_writer->Initialize(
          driver_.get(), verification_results_.get(), image_writer_.get(), dex_files);
    }

    if (!use_existing_vdex_) {
      TimingLogger::ScopedTiming t2("dex2oat Write VDEX", timings_);
      DCHECK(IsBootImage() || IsBootImageExtension() || oat_files_.size() == 1u);
      verifier::VerifierDeps* verifier_deps = callbacks_->GetVerifierDeps();
      for (size_t i = 0, size = oat_files_.size(); i != size; ++i) {
        File* vdex_file = vdex_files_[i].get();
        if (!oat_writers_[i]->FinishVdexFile(vdex_file, verifier_deps)) {
          LOG(ERROR) << "Failed to finish VDEX file " << vdex_file->GetPath();
          return false;
        }
      }
    }

    {
      TimingLogger::ScopedTiming t2("dex2oat Write ELF", timings_);
      linker::MultiOatRelativePatcher patcher(compiler_options_->GetInstructionSet(),
                                              compiler_options_->GetInstructionSetFeatures(),
                                              driver_->GetCompiledMethodStorage());
      for (size_t i = 0, size = oat_files_.size(); i != size; ++i) {
        std::unique_ptr<linker::ElfWriter>& elf_writer = elf_writers_[i];
        std::unique_ptr<linker::OatWriter>& oat_writer = oat_writers_[i];

        oat_writer->PrepareLayout(&patcher);
        elf_writer->PrepareDynamicSection(oat_writer->GetOatHeader().GetExecutableOffset(),
                                          oat_writer->GetCodeSize(),
                                          oat_writer->GetDataImgRelRoSize(),
                                          oat_writer->GetDataImgRelRoAppImageOffset(),
                                          oat_writer->GetBssSize(),
                                          oat_writer->GetBssMethodsOffset(),
                                          oat_writer->GetBssRootsOffset(),
                                          oat_writer->GetVdexSize());
        if (IsImage()) {
          // Update oat layout.
          DCHECK(image_writer_ != nullptr);
          DCHECK_LT(i, oat_filenames_.size());
          image_writer_->UpdateOatFileLayout(i,
                                             elf_writer->GetLoadedSize(),
                                             oat_writer->GetOatDataOffset(),
                                             oat_writer->GetOatSize());
        }
      }

      for (size_t i = 0, size = oat_files_.size(); i != size; ++i) {
        std::unique_ptr<File>& oat_file = oat_files_[i];
        std::unique_ptr<linker::ElfWriter>& elf_writer = elf_writers_[i];
        std::unique_ptr<linker::OatWriter>& oat_writer = oat_writers_[i];

        // We need to mirror the layout of the ELF file in the compressed debug-info.
        // Therefore PrepareDebugInfo() relies on the SetLoadedSectionSizes() call further above.
        debug::DebugInfo debug_info = oat_writer->GetDebugInfo();  // Keep the variable alive.
        // This will perform the compression on background thread while we do other I/O below.
        // If we hit any ERROR path below, the destructor of this variable will wait for the
        // task to finish (since it accesses the 'debug_info' above and other 'Dex2Oat' data).
        std::unique_ptr<ThreadPool> compression_job = elf_writer->PrepareDebugInfo(debug_info);

        OutputStream* rodata = rodata_[i];
        DCHECK(rodata != nullptr);
        if (!oat_writer->WriteRodata(rodata)) {
          LOG(ERROR) << "Failed to write .rodata section to the ELF file " << oat_file->GetPath();
          return false;
        }
        elf_writer->EndRoData(rodata);
        rodata = nullptr;

        OutputStream* text = elf_writer->StartText();
        if (!oat_writer->WriteCode(text)) {
          LOG(ERROR) << "Failed to write .text section to the ELF file " << oat_file->GetPath();
          return false;
        }
        elf_writer->EndText(text);

        if (oat_writer->GetDataImgRelRoSize() != 0u) {
          OutputStream* data_img_rel_ro = elf_writer->StartDataImgRelRo();
          if (!oat_writer->WriteDataImgRelRo(data_img_rel_ro)) {
            LOG(ERROR) << "Failed to write .data.img.rel.ro section to the ELF file "
                << oat_file->GetPath();
            return false;
          }
          elf_writer->EndDataImgRelRo(data_img_rel_ro);
        }

        if (!oat_writer->WriteHeader(elf_writer->GetStream())) {
          LOG(ERROR) << "Failed to write oat header to the ELF file " << oat_file->GetPath();
          return false;
        }

        if (IsImage()) {
          // Update oat header information.
          DCHECK(image_writer_ != nullptr);
          DCHECK_LT(i, oat_filenames_.size());
          image_writer_->UpdateOatFileHeader(i, oat_writer->GetOatHeader());
        }

        elf_writer->WriteDynamicSection();
        {
          TimingLogger::ScopedTiming t_wdi("Write DebugInfo", timings_);
          elf_writer->WriteDebugInfo(oat_writer->GetDebugInfo());
        }

        {
          TimingLogger::ScopedTiming t_end("Write ELF End", timings_);
          if (!elf_writer->End()) {
            LOG(ERROR) << "Failed to write ELF file " << oat_file->GetPath();
            return false;
          }
        }

        if (!FlushOutputFile(&vdex_files_[i]) || !FlushOutputFile(&oat_files_[i])) {
          return false;
        }

        VLOG(compiler) << "Oat file written successfully: " << oat_filenames_[i];

        {
          TimingLogger::ScopedTiming t_dow("Destroy OatWriter", timings_);
          oat_writer.reset();
        }
        // We may still need the ELF writer later for stripping.
      }
    }

    return true;
  }

  // If we are compiling an image, invoke the image creation routine. Else just skip.
  bool HandleImage() {
    if (IsImage()) {
      TimingLogger::ScopedTiming t("dex2oat ImageWriter", timings_);
      if (!CreateImageFile()) {
        return false;
      }
      VLOG(compiler) << "Images written successfully";
    }
    return true;
  }

  // Copy the full oat files to symbols directory and then strip the originals.
  bool CopyOatFilesToSymbolsDirectoryAndStrip() {
    for (size_t i = 0; i < oat_unstripped_.size(); ++i) {
      // If we don't want to strip in place, copy from stripped location to unstripped location.
      // We need to strip after image creation because FixupElf needs to use .strtab.
      if (oat_unstripped_[i] != oat_filenames_[i]) {
        DCHECK(oat_files_[i].get() != nullptr && oat_files_[i]->IsOpened());

        TimingLogger::ScopedTiming t("dex2oat OatFile copy", timings_);
        std::unique_ptr<File>& in = oat_files_[i];
        int64_t in_length = in->GetLength();
        if (in_length < 0) {
          PLOG(ERROR) << "Failed to get the length of oat file: " << in->GetPath();
          return false;
        }
        std::unique_ptr<File> out(OS::CreateEmptyFile(oat_unstripped_[i].c_str()));
        if (out == nullptr) {
          PLOG(ERROR) << "Failed to open oat file for writing: " << oat_unstripped_[i];
          return false;
        }
        if (!out->Copy(in.get(), 0, in_length)) {
          PLOG(ERROR) << "Failed to copy oat file to file: " << out->GetPath();
          return false;
        }
        if (out->FlushCloseOrErase() != 0) {
          PLOG(ERROR) << "Failed to flush and close copied oat file: " << oat_unstripped_[i];
          return false;
        }
        VLOG(compiler) << "Oat file copied successfully (unstripped): " << oat_unstripped_[i];

        if (strip_) {
          TimingLogger::ScopedTiming t2("dex2oat OatFile strip", timings_);
          if (!elf_writers_[i]->StripDebugInfo()) {
            PLOG(ERROR) << "Failed strip oat file: " << in->GetPath();
            return false;
          }
        }
      }
    }
    return true;
  }

  bool FlushOutputFile(std::unique_ptr<File>* file) {
    if ((file->get() != nullptr) && !file->get()->ReadOnlyMode()) {
      if (file->get()->Flush() != 0) {
        PLOG(ERROR) << "Failed to flush output file: " << file->get()->GetPath();
        return false;
      }
    }
    return true;
  }

  bool FlushCloseOutputFile(File* file) {
    if ((file != nullptr) && !file->ReadOnlyMode()) {
      if (file->FlushCloseOrErase() != 0) {
        PLOG(ERROR) << "Failed to flush and close output file: " << file->GetPath();
        return false;
      }
    }
    return true;
  }

  bool FlushOutputFiles() {
    TimingLogger::ScopedTiming t2("dex2oat Flush Output Files", timings_);
    for (auto& files : { &vdex_files_, &oat_files_ }) {
      for (size_t i = 0; i < files->size(); ++i) {
        if (!FlushOutputFile(&(*files)[i])) {
          return false;
        }
      }
    }
    return true;
  }

  bool FlushCloseOutputFiles() {
    bool result = true;
    for (auto& files : { &vdex_files_, &oat_files_ }) {
      for (size_t i = 0; i < files->size(); ++i) {
        result &= FlushCloseOutputFile((*files)[i].get());
      }
    }
    return result;
  }

  void DumpTiming() {
    if (compiler_options_->GetDumpTimings() ||
        (kIsDebugBuild && timings_->GetTotalNs() > MsToNs(1000))) {
      LOG(INFO) << Dumpable<TimingLogger>(*timings_);
    }
  }

  bool IsImage() const {
    return IsAppImage() || IsBootImage() || IsBootImageExtension();
  }

  bool IsAppImage() const {
    return compiler_options_->IsAppImage();
  }

  bool IsBootImage() const {
    return compiler_options_->IsBootImage();
  }

  bool IsBootImageExtension() const {
    return compiler_options_->IsBootImageExtension();
  }

  bool IsHost() const {
    return is_host_;
  }

  bool HasProfileInput() const { return !profile_file_fds_.empty() || !profile_files_.empty(); }

  // Must be called after the profile is loaded.
  bool DoProfileGuidedOptimizations() const {
    DCHECK(!HasProfileInput() || profile_load_attempted_)
        << "The profile has to be loaded before we can decided "
        << "if we do profile guided optimizations";
    return profile_compilation_info_ != nullptr && !profile_compilation_info_->IsEmpty();
  }

  bool DoOatLayoutOptimizations() const {
    return DoProfileGuidedOptimizations();
  }

  bool LoadProfile() {
    DCHECK(HasProfileInput());
    profile_load_attempted_ = true;
    // TODO(calin): We should be using the runtime arena pool (instead of the
    // default profile arena). However the setup logic is messy and needs
    // cleaning up before that (e.g. the oat writers are created before the
    // runtime).
    bool for_boot_image = IsBootImage() || IsBootImageExtension();
    profile_compilation_info_.reset(new ProfileCompilationInfo(for_boot_image));

    // Cleanup profile compilation info if we encounter any error when reading profiles.
    auto cleanup = android::base::ScopeGuard([&]() { profile_compilation_info_.reset(nullptr); });

    // Dex2oat only uses the reference profile and that is not updated concurrently by the app or
    // other processes. So we don't need to lock (as we have to do in profman or when writing the
    // profile info).
    std::vector<std::unique_ptr<File>> profile_files;
    if (!profile_file_fds_.empty()) {
      for (int fd : profile_file_fds_) {
        profile_files.push_back(std::make_unique<File>(DupCloexec(fd),
                                                       "profile",
                                                       /*check_usage=*/ false,
                                                       /*read_only_mode=*/ true));
      }
    } else {
      for (const std::string& file : profile_files_) {
        profile_files.emplace_back(OS::OpenFileForReading(file.c_str()));
        if (profile_files.back().get() == nullptr) {
          PLOG(ERROR) << "Cannot open profiles";
          return false;
        }
      }
    }

    std::map<std::string, uint32_t> old_profile_keys, new_profile_keys;
    auto filter_fn = [&](const std::string& profile_key, uint32_t checksum) {
      auto it = old_profile_keys.find(profile_key);
      if (it != old_profile_keys.end() && it->second != checksum) {
        // Filter out this entry. We have already loaded data for the same profile key with a
        // different checksum from an earlier profile file.
        return false;
      }
      // Insert the new profile key and checksum.
      // Note: If the profile contains the same key with different checksums, this insertion fails
      // but we still return `true` and let the `ProfileCompilationInfo::Load()` report an error.
      new_profile_keys.insert(std::make_pair(profile_key, checksum));
      return true;
    };
    for (const std::unique_ptr<File>& profile_file : profile_files) {
      if (!profile_compilation_info_->Load(profile_file->Fd(),
                                           /*merge_classes=*/ true,
                                           filter_fn)) {
        return false;
      }
      old_profile_keys.merge(new_profile_keys);
      new_profile_keys.clear();
    }

    cleanup.Disable();
    return true;
  }

  // If we're asked to speed-profile the app but we have no profile, or the profile
  // is empty, change the filter to verify, and the image_type to none.
  // A speed-profile compilation without profile data is equivalent to verify and
  // this change will increase the precision of the telemetry data.
  void UpdateCompilerOptionsBasedOnProfile() {
    if (!DoProfileGuidedOptimizations() &&
        compiler_options_->GetCompilerFilter() == CompilerFilter::kSpeedProfile) {
      VLOG(compiler) << "Changing compiler filter to verify from speed-profile "
          << "because of empty or non existing profile";

      compiler_options_->SetCompilerFilter(CompilerFilter::kVerify);

      // Note that we could reset the image_type to CompilerOptions::ImageType::kNone
      // to prevent an app image generation.
      // However, if we were pass an image file we would essentially leave the image
      // file empty (possibly triggering some harmless errors when we try to load it).
      //
      // Letting the image_type_ be determined by whether or not we passed an image
      // file will at least write the appropriate header making it an empty but valid
      // image.
    }
  }

  class ScopedDex2oatReporting {
   public:
    explicit ScopedDex2oatReporting(const Dex2Oat& dex2oat) :
        should_report_(dex2oat.should_report_dex2oat_compilation_) {
      if (should_report_) {
        if (dex2oat.zip_fd_ != -1) {
          zip_dup_fd_.reset(DupCloexecOrError(dex2oat.zip_fd_));
          if (zip_dup_fd_ < 0) {
            return;
          }
        }
        int image_fd = dex2oat.IsAppImage() ? dex2oat.app_image_fd_ : dex2oat.image_fd_;
        if (image_fd != -1) {
          image_dup_fd_.reset(DupCloexecOrError(image_fd));
          if (image_dup_fd_ < 0) {
            return;
          }
        }
        oat_dup_fd_.reset(DupCloexecOrError(dex2oat.oat_fd_));
        if (oat_dup_fd_ < 0) {
          return;
        }
        vdex_dup_fd_.reset(DupCloexecOrError(dex2oat.output_vdex_fd_));
        if (vdex_dup_fd_ < 0) {
          return;
        }
        PaletteNotifyStartDex2oatCompilation(zip_dup_fd_,
                                             image_dup_fd_,
                                             oat_dup_fd_,
                                             vdex_dup_fd_);
      }
      error_reporting_ = false;
    }

    ~ScopedDex2oatReporting() {
      if (!error_reporting_) {
        if (should_report_) {
          PaletteNotifyEndDex2oatCompilation(zip_dup_fd_,
                                             image_dup_fd_,
                                             oat_dup_fd_,
                                             vdex_dup_fd_);
        }
      }
    }

    bool ErrorReporting() const { return error_reporting_; }

   private:
    int DupCloexecOrError(int fd) {
      int dup_fd = DupCloexec(fd);
      if (dup_fd < 0) {
        LOG(ERROR) << "Error dup'ing a file descriptor " << strerror(errno);
        error_reporting_ = true;
      }
      return dup_fd;
    }
    android::base::unique_fd oat_dup_fd_;
    android::base::unique_fd vdex_dup_fd_;
    android::base::unique_fd zip_dup_fd_;
    android::base::unique_fd image_dup_fd_;
    bool error_reporting_ = false;
    bool should_report_;
  };

 private:
  bool UseSwap(bool is_image, const std::vector<const DexFile*>& dex_files) {
    if (is_image) {
      // Don't use swap, we know generation should succeed, and we don't want to slow it down.
      return false;
    }
    if (dex_files.size() < min_dex_files_for_swap_) {
      // If there are less dex files than the threshold, assume it's gonna be fine.
      return false;
    }
    size_t dex_files_size = 0;
    for (const auto* dex_file : dex_files) {
      dex_files_size += dex_file->GetHeader().file_size_;
    }
    return dex_files_size >= min_dex_file_cumulative_size_for_swap_;
  }

  bool IsVeryLarge(const std::vector<const DexFile*>& dex_files) {
    size_t dex_files_size = 0;
    for (const auto* dex_file : dex_files) {
      dex_files_size += dex_file->GetHeader().file_size_;
    }
    return dex_files_size >= very_large_threshold_;
  }

  bool PrepareDirtyObjects() {
    if (!dirty_image_objects_fds_.empty()) {
      dirty_image_objects_ = std::make_unique<std::vector<std::string>>();
      for (int fd : dirty_image_objects_fds_) {
        if (!ReadCommentedInputFromFd(fd, nullptr, dirty_image_objects_.get())) {
          LOG(ERROR) << "Failed to create list of dirty objects from fd " << fd;
          return false;
        }
      }
      // Close since we won't need it again.
      for (int fd : dirty_image_objects_fds_) {
        close(fd);
      }
      dirty_image_objects_fds_.clear();
    } else if (!dirty_image_objects_filenames_.empty()) {
      dirty_image_objects_ = std::make_unique<std::vector<std::string>>();
      for (const std::string& file : dirty_image_objects_filenames_) {
        if (!ReadCommentedInputFromFile(file.c_str(), nullptr, dirty_image_objects_.get())) {
          LOG(ERROR) << "Failed to create list of dirty objects from '" << file << "'";
          return false;
        }
      }
    }
    return true;
  }

  bool PreparePreloadedClasses() {
    if (!preloaded_classes_fds_.empty()) {
      for (int fd : preloaded_classes_fds_) {
        if (!ReadCommentedInputFromFd(fd, nullptr, &compiler_options_->preloaded_classes_)) {
          return false;
        }
      }
    } else {
      for (const std::string& file : preloaded_classes_files_) {
        if (!ReadCommentedInputFromFile(
                file.c_str(), nullptr, &compiler_options_->preloaded_classes_)) {
          return false;
        }
      }
    }
    return true;
  }

  void PruneNonExistentDexFiles() {
    DCHECK_EQ(dex_filenames_.size(), dex_locations_.size());
    size_t kept = 0u;
    for (size_t i = 0, size = dex_filenames_.size(); i != size; ++i) {
      // Keep if the file exist, or is passed as FD.
      if (!OS::FileExists(dex_filenames_[i].c_str()) && i >= dex_fds_.size()) {
        LOG(WARNING) << "Skipping non-existent dex file '" << dex_filenames_[i] << "'";
      } else {
        if (kept != i) {
          dex_filenames_[kept] = dex_filenames_[i];
          dex_locations_[kept] = dex_locations_[i];
        }
        ++kept;
      }
    }
    dex_filenames_.resize(kept);
    dex_locations_.resize(kept);
  }

  bool AddDexFileSources() {
    TimingLogger::ScopedTiming t2("AddDexFileSources", timings_);
    if (input_vdex_file_ != nullptr && input_vdex_file_->HasDexSection()) {
      DCHECK_EQ(oat_writers_.size(), 1u);
      const std::string& name = zip_location_.empty() ? dex_locations_[0] : zip_location_;
      DCHECK(!name.empty());
      if (!oat_writers_[0]->AddVdexDexFilesSource(*input_vdex_file_.get(), name.c_str())) {
        return false;
      }
    } else if (zip_fd_ != -1) {
      DCHECK_EQ(oat_writers_.size(), 1u);
      if (!oat_writers_[0]->AddDexFileSource(File(zip_fd_, /* check_usage */ false),
                                             zip_location_.c_str())) {
        return false;
      }
    } else {
      DCHECK_EQ(dex_filenames_.size(), dex_locations_.size());
      DCHECK_GE(oat_writers_.size(), 1u);

      bool use_dex_fds = !dex_fds_.empty();
      if (use_dex_fds) {
        DCHECK_EQ(dex_fds_.size(), dex_filenames_.size());
      }

      bool is_multi_image = oat_writers_.size() > 1u;
      if (is_multi_image) {
        DCHECK_EQ(oat_writers_.size(), dex_filenames_.size());
      }

      for (size_t i = 0; i != dex_filenames_.size(); ++i) {
        int oat_index = is_multi_image ? i : 0;
        auto oat_writer = oat_writers_[oat_index].get();

        if (use_dex_fds) {
          if (!oat_writer->AddDexFileSource(File(dex_fds_[i], /* check_usage */ false),
                                            dex_locations_[i].c_str())) {
            return false;
          }
        } else {
          if (!oat_writer->AddDexFileSource(dex_filenames_[i].c_str(),
                                            dex_locations_[i].c_str())) {
            return false;
          }
        }
      }
    }
    return true;
  }

  void CreateOatWriters() {
    TimingLogger::ScopedTiming t2("CreateOatWriters", timings_);
    elf_writers_.reserve(oat_files_.size());
    oat_writers_.reserve(oat_files_.size());
    for (const std::unique_ptr<File>& oat_file : oat_files_) {
      elf_writers_.emplace_back(linker::CreateElfWriterQuick(*compiler_options_, oat_file.get()));
      elf_writers_.back()->Start();
      bool do_oat_writer_layout = DoOatLayoutOptimizations();
      oat_writers_.emplace_back(new linker::OatWriter(
          *compiler_options_,
          timings_,
          do_oat_writer_layout ? profile_compilation_info_.get() : nullptr));
    }
  }

  void SaveDexInput() {
    const std::vector<const DexFile*>& dex_files = compiler_options_->dex_files_for_oat_file_;
    for (size_t i = 0, size = dex_files.size(); i != size; ++i) {
      const DexFile* dex_file = dex_files[i];
      std::string tmp_file_name(StringPrintf("/data/local/tmp/dex2oat.%d.%zd.dex",
                                             getpid(), i));
      std::unique_ptr<File> tmp_file(OS::CreateEmptyFile(tmp_file_name.c_str()));
      if (tmp_file.get() == nullptr) {
        PLOG(ERROR) << "Failed to open file " << tmp_file_name
            << ". Try: adb shell chmod 777 /data/local/tmp";
        continue;
      }
      // This is just dumping files for debugging. Ignore errors, and leave remnants.
      UNUSED(tmp_file->WriteFully(dex_file->Begin(), dex_file->Size()));
      UNUSED(tmp_file->Flush());
      UNUSED(tmp_file->Close());
      LOG(INFO) << "Wrote input to " << tmp_file_name;
    }
  }

  bool PrepareRuntimeOptions(RuntimeArgumentMap* runtime_options,
                             QuickCompilerCallbacks* callbacks) {
    RuntimeOptions raw_options;
    if (IsBootImage()) {
      std::string boot_class_path = "-Xbootclasspath:";
      boot_class_path += android::base::Join(dex_filenames_, ':');
      raw_options.push_back(std::make_pair(boot_class_path, nullptr));
      std::string boot_class_path_locations = "-Xbootclasspath-locations:";
      boot_class_path_locations += android::base::Join(dex_locations_, ':');
      raw_options.push_back(std::make_pair(boot_class_path_locations, nullptr));
    } else {
      std::string boot_image_option = "-Ximage:";
      boot_image_option += boot_image_filename_;
      raw_options.push_back(std::make_pair(boot_image_option, nullptr));
    }
    for (size_t i = 0; i < runtime_args_.size(); i++) {
      raw_options.push_back(std::make_pair(runtime_args_[i], nullptr));
    }

    raw_options.push_back(std::make_pair("compilercallbacks", callbacks));
    raw_options.push_back(
        std::make_pair("imageinstructionset",
                       GetInstructionSetString(compiler_options_->GetInstructionSet())));

    // Never allow implicit image compilation.
    raw_options.push_back(std::make_pair("-Xnoimage-dex2oat", nullptr));
    // Disable libsigchain. We don't don't need it during compilation and it prevents us
    // from getting a statically linked version of dex2oat (because of dlsym and RTLD_NEXT).
    raw_options.push_back(std::make_pair("-Xno-sig-chain", nullptr));
    // Disable Hspace compaction to save heap size virtual space.
    // Only need disable Hspace for OOM becasue background collector is equal to
    // foreground collector by default for dex2oat.
    raw_options.push_back(std::make_pair("-XX:DisableHSpaceCompactForOOM", nullptr));

    if (!Runtime::ParseOptions(raw_options, false, runtime_options)) {
      LOG(ERROR) << "Failed to parse runtime options";
      return false;
    }
    return true;
  }

  // Create a runtime necessary for compilation.
  bool CreateRuntime(RuntimeArgumentMap&& runtime_options) {
    // To make identity hashcode deterministic, set a seed based on the dex file checksums.
    // That makes the seed also most likely different for different inputs, for example
    // for primary boot image and different extensions that could be loaded together.
    mirror::Object::SetHashCodeSeed(987654321u ^ GetCombinedChecksums());

    TimingLogger::ScopedTiming t_runtime("Create runtime", timings_);
    if (!Runtime::Create(std::move(runtime_options))) {
      LOG(ERROR) << "Failed to create runtime";
      return false;
    }

    // Runtime::Init will rename this thread to be "main". Prefer "dex2oat" so that "top" and
    // "ps -a" don't change to non-descript "main."
    SetThreadName(kIsDebugBuild ? "dex2oatd" : "dex2oat");

    runtime_.reset(Runtime::Current());
    runtime_->SetInstructionSet(compiler_options_->GetInstructionSet());
    for (uint32_t i = 0; i < static_cast<uint32_t>(CalleeSaveType::kLastCalleeSaveType); ++i) {
      CalleeSaveType type = CalleeSaveType(i);
      if (!runtime_->HasCalleeSaveMethod(type)) {
        runtime_->SetCalleeSaveMethod(runtime_->CreateCalleeSaveMethod(), type);
      }
    }

    // Initialize maps for unstarted runtime. This needs to be here, as running clinits needs this
    // set up.
    interpreter::UnstartedRuntime::Initialize();

    Thread* self = Thread::Current();
    runtime_->GetClassLinker()->RunEarlyRootClinits(self);
    InitializeIntrinsics();
    runtime_->RunRootClinits(self);

    // Runtime::Create acquired the mutator_lock_ that is normally given away when we
    // Runtime::Start, give it away now so that we don't starve GC.
    self->TransitionFromRunnableToSuspended(ThreadState::kNative);

    WatchDog::SetRuntime(runtime_.get());

    return true;
  }

  // Let the ImageWriter write the image files. If we do not compile PIC, also fix up the oat files.
  bool CreateImageFile()
      REQUIRES(!Locks::mutator_lock_) {
    CHECK(image_writer_ != nullptr);
    if (IsAppImage()) {
      DCHECK(image_filenames_.empty());
      if (app_image_fd_ != -1) {
        image_filenames_.push_back(StringPrintf("FileDescriptor[%d]", app_image_fd_));
      } else {
        image_filenames_.push_back(app_image_file_name_);
      }
    }
    if (image_fd_ != -1) {
      DCHECK(image_filenames_.empty());
      image_filenames_.push_back(StringPrintf("FileDescriptor[%d]", image_fd_));
    }
    if (!image_writer_->Write(IsAppImage() ? app_image_fd_ : image_fd_,
                              image_filenames_,
                              IsAppImage() ? 1u : dex_locations_.size())) {
      LOG(ERROR) << "Failure during image file creation";
      return false;
    }

    // We need the OatDataBegin entries.
    dchecked_vector<uintptr_t> oat_data_begins;
    for (size_t i = 0, size = oat_filenames_.size(); i != size; ++i) {
      oat_data_begins.push_back(image_writer_->GetOatDataBegin(i));
    }
    // Destroy ImageWriter.
    image_writer_.reset();

    return true;
  }

  template <typename T>
  static bool ReadCommentedInputFromFile(
      const char* input_filename, std::function<std::string(const char*)>* process, T* output) {
    auto input_file = std::unique_ptr<FILE, decltype(&fclose)>{fopen(input_filename, "re"), fclose};
    if (!input_file) {
      LOG(ERROR) << "Failed to open input file " << input_filename;
      return false;
    }
    ReadCommentedInputStream<T>(input_file.get(), process, output);
    return true;
  }

  template <typename T>
  static bool ReadCommentedInputFromFd(
      int input_fd, std::function<std::string(const char*)>* process, T* output) {
    auto input_file = std::unique_ptr<FILE, decltype(&fclose)>{fdopen(input_fd, "r"), fclose};
    if (!input_file) {
      LOG(ERROR) << "Failed to re-open input fd from /prof/self/fd/" << input_fd;
      return false;
    }
    ReadCommentedInputStream<T>(input_file.get(), process, output);
    return true;
  }

  // Read lines from the given file, dropping comments and empty lines. Post-process each line with
  // the given function.
  template <typename T>
  static std::unique_ptr<T> ReadCommentedInputFromFile(
      const char* input_filename, std::function<std::string(const char*)>* process) {
    std::unique_ptr<T> output(new T());
    ReadCommentedInputFromFile(input_filename, process, output.get());
    return output;
  }

  // Read lines from the given fd, dropping comments and empty lines. Post-process each line with
  // the given function.
  template <typename T>
  static std::unique_ptr<T> ReadCommentedInputFromFd(
      int input_fd, std::function<std::string(const char*)>* process) {
    std::unique_ptr<T> output(new T());
    ReadCommentedInputFromFd(input_fd, process, output.get());
    return output;
  }

  // Read lines from the given stream, dropping comments and empty lines. Post-process each line
  // with the given function.
  template <typename T> static void ReadCommentedInputStream(
      std::FILE* in_stream,
      std::function<std::string(const char*)>* process,
      T* output) {
    char* line = nullptr;
    size_t line_alloc = 0;
    ssize_t len = 0;
    while ((len = getline(&line, &line_alloc, in_stream)) > 0) {
      if (line[0] == '\0' || line[0] == '#' || line[0] == '\n') {
        continue;
      }
      if (line[len - 1] == '\n') {
        line[len - 1] = '\0';
      }
      if (process != nullptr) {
        std::string descriptor((*process)(line));
        output->insert(output->end(), descriptor);
      } else {
        output->insert(output->end(), line);
      }
    }
    free(line);
  }

  void LogCompletionTime() {
    // Note: when creation of a runtime fails, e.g., when trying to compile an app but when there
    //       is no image, there won't be a Runtime::Current().
    // Note: driver creation can fail when loading an invalid dex file.
    LOG(INFO) << "dex2oat took "
              << PrettyDuration(NanoTime() - start_ns_)
              << " (" << PrettyDuration(ProcessCpuNanoTime() - start_cputime_ns_) << " cpu)"
              << " (threads: " << thread_count_ << ") "
              << ((Runtime::Current() != nullptr && driver_ != nullptr) ?
                  driver_->GetMemoryUsageString(kIsDebugBuild || VLOG_IS_ON(compiler)) :
                  "");
  }

  std::string StripIsaFrom(const char* image_filename, InstructionSet isa) {
    std::string res(image_filename);
    size_t last_slash = res.rfind('/');
    if (last_slash == std::string::npos || last_slash == 0) {
      return res;
    }
    size_t penultimate_slash = res.rfind('/', last_slash - 1);
    if (penultimate_slash == std::string::npos) {
      return res;
    }
    // Check that the string in-between is the expected one.
    if (res.substr(penultimate_slash + 1, last_slash - penultimate_slash - 1) !=
            GetInstructionSetString(isa)) {
      LOG(WARNING) << "Unexpected string when trying to strip isa: " << res;
      return res;
    }
    return res.substr(0, penultimate_slash) + res.substr(last_slash);
  }

  std::unique_ptr<CompilerOptions> compiler_options_;

  std::unique_ptr<linker::OatKeyValueStore> key_value_store_;

  std::unique_ptr<VerificationResults> verification_results_;

  std::unique_ptr<QuickCompilerCallbacks> callbacks_;

  std::unique_ptr<Runtime> runtime_;

  // The spec describing how the class loader should be setup for compilation.
  std::unique_ptr<ClassLoaderContext> class_loader_context_;

  // Optional list of file descriptors corresponding to dex file locations in
  // flattened `class_loader_context_`.
  std::vector<int> class_loader_context_fds_;

  // The class loader context stored in the oat file. May be equal to class_loader_context_.
  std::unique_ptr<ClassLoaderContext> stored_class_loader_context_;

  size_t thread_count_;
  std::vector<int32_t> cpu_set_;
  uint64_t start_ns_;
  uint64_t start_cputime_ns_;
  std::unique_ptr<WatchDog> watchdog_;
  std::vector<std::unique_ptr<File>> oat_files_;
  std::vector<std::unique_ptr<File>> vdex_files_;
  std::string oat_location_;
  std::vector<std::string> oat_filenames_;
  std::vector<std::string> oat_unstripped_;
  bool strip_;
  int oat_fd_;
  int input_vdex_fd_;
  int output_vdex_fd_;
  std::string input_vdex_;
  std::string output_vdex_;
  std::unique_ptr<VdexFile> input_vdex_file_;
  int dm_fd_;
  std::string dm_file_location_;
  std::unique_ptr<ZipArchive> dm_file_;
  std::vector<std::string> dex_filenames_;
  std::vector<std::string> dex_locations_;
  std::vector<int> dex_fds_;
  int zip_fd_;
  std::string zip_location_;
  std::string boot_image_filename_;
  std::vector<const char*> runtime_args_;
  std::vector<std::string> image_filenames_;
  int image_fd_;
  bool have_multi_image_arg_;
  uintptr_t image_base_;
  ImageHeader::StorageMode image_storage_mode_;
  const char* passes_to_run_filename_;
  std::vector<std::string> dirty_image_objects_filenames_;
  std::vector<int> dirty_image_objects_fds_;
  std::unique_ptr<std::vector<std::string>> dirty_image_objects_;
  std::unique_ptr<std::vector<std::string>> passes_to_run_;
  bool is_host_;
  std::string android_root_;
  std::string no_inline_from_string_;
  bool force_allow_oj_inlines_ = false;

  std::vector<std::unique_ptr<linker::ElfWriter>> elf_writers_;
  std::vector<std::unique_ptr<linker::OatWriter>> oat_writers_;
  std::vector<OutputStream*> rodata_;
  std::vector<std::unique_ptr<OutputStream>> vdex_out_;
  std::unique_ptr<linker::ImageWriter> image_writer_;
  std::unique_ptr<CompilerDriver> driver_;

  std::vector<MemMap> opened_dex_files_maps_;
  std::vector<std::unique_ptr<const DexFile>> opened_dex_files_;

  bool avoid_storing_invocation_;
  android::base::unique_fd invocation_file_;
  std::string swap_file_name_;
  int swap_fd_;
  size_t min_dex_files_for_swap_ = kDefaultMinDexFilesForSwap;
  size_t min_dex_file_cumulative_size_for_swap_ = kDefaultMinDexFileCumulativeSizeForSwap;
  size_t very_large_threshold_ = std::numeric_limits<size_t>::max();
  std::string app_image_file_name_;
  int app_image_fd_;
  std::vector<std::string> profile_files_;
  std::vector<int> profile_file_fds_;
  std::vector<std::string> preloaded_classes_files_;
  std::vector<int> preloaded_classes_fds_;
  std::unique_ptr<ProfileCompilationInfo> profile_compilation_info_;
  TimingLogger* timings_;
  std::vector<std::vector<const DexFile*>> dex_files_per_oat_file_;
  HashMap<const DexFile*, size_t> dex_file_oat_index_map_;

  // Backing storage.
  std::forward_list<std::string> char_backing_storage_;

  // See CompilerOptions.force_determinism_.
  bool force_determinism_;
  // See CompilerOptions.crash_on_linkage_violation_.
  bool check_linkage_conditions_;
  // See CompilerOptions.crash_on_linkage_violation_.
  bool crash_on_linkage_violation_;

  // Directory of relative classpaths.
  std::string classpath_dir_;

  // Whether the given input vdex is also the output.
  bool use_existing_vdex_ = false;

  // By default, copy the dex to the vdex file only if dex files are
  // compressed in APK.
  linker::CopyOption copy_dex_files_ = linker::CopyOption::kOnlyIfCompressed;

  // The reason for invoking the compiler.
  std::string compilation_reason_;

  // Whether to force individual compilation.
  bool compile_individually_;

  // The classpath that determines if a given symbol should be resolved at compile time or not.
  std::string public_sdk_;

  // The apex versions of jars in the boot classpath. Set through command line
  // argument.
  std::string apex_versions_argument_;

  // Whether or we attempted to load the profile (if given).
  bool profile_load_attempted_;

  // Whether PaletteNotify{Start,End}Dex2oatCompilation should be called.
  bool should_report_dex2oat_compilation_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Dex2Oat);
};

class ScopedGlobalRef {
 public:
  explicit ScopedGlobalRef(jobject obj) : obj_(obj) {}
  ~ScopedGlobalRef() {
    if (obj_ != nullptr) {
      ScopedObjectAccess soa(Thread::Current());
      soa.Env()->GetVm()->DeleteGlobalRef(soa.Self(), obj_);
    }
  }

 private:
  jobject obj_;
};

static dex2oat::ReturnCode DoCompilation(Dex2Oat& dex2oat) REQUIRES(!Locks::mutator_lock_) {
  Locks::mutator_lock_->AssertNotHeld(Thread::Current());
  dex2oat.LoadImageClassDescriptors();
  jobject class_loader = dex2oat.Compile();
  // Keep the class loader that was used for compilation live for the rest of the compilation
  // process.
  ScopedGlobalRef global_ref(class_loader);

  if (!dex2oat.WriteOutputFiles(class_loader)) {
    dex2oat.EraseOutputFiles();
    return dex2oat::ReturnCode::kOther;
  }

  // Flush output files.  Keep them open as we might still modify them later (strip them).
  if (!dex2oat.FlushOutputFiles()) {
    dex2oat.EraseOutputFiles();
    return dex2oat::ReturnCode::kOther;
  }

  // Creates the boot.art and patches the oat files.
  if (!dex2oat.HandleImage()) {
    return dex2oat::ReturnCode::kOther;
  }

  // When given --host, finish early without stripping.
  if (dex2oat.IsHost()) {
    if (!dex2oat.FlushCloseOutputFiles()) {
      return dex2oat::ReturnCode::kOther;
    }
    dex2oat.DumpTiming();
    return dex2oat::ReturnCode::kNoFailure;
  }

  // Copy stripped to unstripped location, if necessary. This will implicitly flush & close the
  // stripped versions. If this is given, we expect to be able to open writable files by name.
  if (!dex2oat.CopyOatFilesToSymbolsDirectoryAndStrip()) {
    return dex2oat::ReturnCode::kOther;
  }

  // FlushClose again, as stripping might have re-opened the oat files.
  if (!dex2oat.FlushCloseOutputFiles()) {
    return dex2oat::ReturnCode::kOther;
  }

  dex2oat.DumpTiming();
  return dex2oat::ReturnCode::kNoFailure;
}

static dex2oat::ReturnCode Dex2oat(int argc, char** argv) {
  TimingLogger timings("compiler", false, false);

  // Allocate `dex2oat` on the heap instead of on the stack, as Clang
  // might produce a stack frame too large for this function or for
  // functions inlining it (such as main), that would not fit the
  // requirements of the `-Wframe-larger-than` option.
  std::unique_ptr<Dex2Oat> dex2oat = std::make_unique<Dex2Oat>(&timings);

  // Parse arguments. Argument mistakes will lead to exit(EXIT_FAILURE) in UsageError.
  dex2oat->ParseArgs(argc, argv);

  art::MemMap::Init();  // For ZipEntry::ExtractToMemMap, vdex and profiles.

  // If needed, process profile information for profile guided compilation.
  // This operation involves I/O.
  if (dex2oat->HasProfileInput()) {
    if (!dex2oat->LoadProfile()) {
      LOG(ERROR) << "Failed to process profile file";
      return dex2oat::ReturnCode::kOther;
    }
  }

  // Check if we need to update any of the compiler options (such as the filter)
  // and do it before anything else (so that the other operations have a true
  // view of the state).
  dex2oat->UpdateCompilerOptionsBasedOnProfile();

  // Insert the compiler options in the key value store.
  // We have to do this after we altered any incoming arguments
  // (such as the compiler filter).
  dex2oat->InsertCompileOptions(argc, argv);

  // Check early that the result of compilation can be written
  if (!dex2oat->OpenFile()) {
    // Flush close so that the File Guard checks don't fail the assertions.
    dex2oat->FlushCloseOutputFiles();
    return dex2oat::ReturnCode::kOther;
  }

  // Print the complete line when any of the following is true:
  //   1) Debug build
  //   2) Compiling an image
  //   3) Compiling with --host
  //   4) Compiling on the host (not a target build)
  // Otherwise, print a stripped command line.
  if (kIsDebugBuild ||
      dex2oat->IsBootImage() || dex2oat->IsBootImageExtension() ||
      dex2oat->IsHost() ||
      !kIsTargetBuild) {
    LOG(INFO) << CommandLine();
  } else {
    LOG(INFO) << StrippedCommandLine();
  }

  Dex2Oat::ScopedDex2oatReporting sdr(*dex2oat.get());

  if (sdr.ErrorReporting()) {
    dex2oat->EraseOutputFiles();
    return dex2oat::ReturnCode::kOther;
  }

  dex2oat::ReturnCode setup_code = dex2oat->Setup();
  if (setup_code != dex2oat::ReturnCode::kNoFailure) {
    dex2oat->EraseOutputFiles();
    return setup_code;
  }

  // TODO: Due to the cyclic dependencies, profile loading and verifying are
  // being done separately. Refactor and place the two next to each other.
  // If verification fails, we don't abort the compilation and instead log an
  // error.
  // TODO(b/62602192, b/65260586): We should consider aborting compilation when
  // the profile verification fails.
  // Note: If dex2oat fails, installd will remove the oat files causing the app
  // to fallback to apk with possible in-memory extraction. We want to avoid
  // that, and thus we're lenient towards profile corruptions.
  if (dex2oat->DoProfileGuidedOptimizations()) {
    dex2oat->VerifyProfileData();
  }

  // Helps debugging on device. Can be used to determine which dalvikvm instance invoked a dex2oat
  // instance. Used by tools/bisection_search/bisection_search.py.
  VLOG(compiler) << "Running dex2oat (parent PID = " << getppid() << ")";

  dex2oat::ReturnCode result = DoCompilation(*dex2oat);

  return result;
}
}  // namespace art

int main(int argc, char** argv) {
  int result = static_cast<int>(art::Dex2oat(argc, argv));
  // Everything was done, do an explicit exit here to avoid running Runtime destructors that take
  // time (bug 10645725) unless we're a debug or instrumented build or running on a memory tool.
  // Note: The Dex2Oat class should not destruct the runtime in this case.
  if (!art::kIsDebugBuild && !art::kIsPGOInstrumentation && !art::kRunningOnMemoryTool) {
    art::FastExit(result);
  }
  return result;
}

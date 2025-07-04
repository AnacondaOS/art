/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "signal_catcher.h"

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <optional>
#include <sstream>

#include "arch/instruction_set.h"
#include "base/debugstore.h"
#include "base/logging.h"  // For GetCmdLine.
#include "base/os.h"
#include "base/time_utils.h"
#include "base/utils.h"
#include "class_linker.h"
#include "com_android_art_flags.h"
#include "gc/heap.h"
#include "jit/profile_saver.h"
#include "palette/palette.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "signal_set.h"
#include "thread.h"
#include "thread_list.h"
#include "trace_profile.h"

namespace art_flags = com::android::art::flags;

namespace art HIDDEN {

static void DumpCmdLine(std::ostream& os) {
#if defined(__linux__)
  // Show the original command line, and the current command line too if it's changed.
  // On Android, /proc/self/cmdline will have been rewritten to something like "system_server".
  // Note: The string "Cmd line:" is chosen to match the format used by debuggerd.
  std::string current_cmd_line;
  if (android::base::ReadFileToString("/proc/self/cmdline", &current_cmd_line)) {
    current_cmd_line.resize(current_cmd_line.find_last_not_of('\0') + 1);  // trim trailing '\0's
    std::replace(current_cmd_line.begin(), current_cmd_line.end(), '\0', ' ');

    os << "Cmd line: " << current_cmd_line << "\n";
    const char* stashed_cmd_line = GetCmdLine();
    if (stashed_cmd_line != nullptr && current_cmd_line != stashed_cmd_line
            && strcmp(stashed_cmd_line, "<unset>") != 0) {
      os << "Original command line: " << stashed_cmd_line << "\n";
    }
  }
#else
  os << "Cmd line: " << GetCmdLine() << "\n";
#endif
}

SignalCatcher::SignalCatcher()
    : lock_("SignalCatcher lock"),
      cond_("SignalCatcher::cond_", lock_),
      thread_(nullptr) {
  SetHaltFlag(false);

  // Create a raw pthread; its start routine will attach to the runtime.
  CHECK_PTHREAD_CALL(pthread_create, (&pthread_, nullptr, &Run, this), "signal catcher thread");

  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  while (thread_ == nullptr) {
    cond_.Wait(self);
  }
}

SignalCatcher::~SignalCatcher() {
  // Since we know the thread is just sitting around waiting for signals
  // to arrive, send it one.
  SetHaltFlag(true);
  CHECK_PTHREAD_CALL(pthread_kill,
                     (pthread_, SIGQUIT),
                     android::base::StringPrintf("signal catcher shutdown: %lu", pthread_));
  CHECK_PTHREAD_CALL(pthread_join,
                     (pthread_, nullptr),
                     android::base::StringPrintf("signal catcher shutdown: %lu", pthread_));
}

void SignalCatcher::SetHaltFlag(bool new_value) {
  MutexLock mu(Thread::Current(), lock_);
  halt_ = new_value;
}

bool SignalCatcher::ShouldHalt() {
  MutexLock mu(Thread::Current(), lock_);
  return halt_;
}

void SignalCatcher::Output(const std::string& s) {
  ScopedThreadStateChange tsc(Thread::Current(), ThreadState::kWaitingForSignalCatcherOutput);
  palette_status_t status = PaletteWriteCrashThreadStacks(s.data(), s.size());
  if (status == PALETTE_STATUS_OK) {
    LOG(INFO) << "Wrote stack traces to tombstoned";
  } else {
    CHECK(status == PALETTE_STATUS_FAILED_CHECK_LOG);
    LOG(ERROR) << "Failed to write stack traces to tombstoned";
  }
}

void SignalCatcher::HandleSigQuit() {
  sigquit_nanotime_ = NanoTime();
  Runtime* runtime = Runtime::Current();
  std::ostringstream os;
  os << "\n"
      << "----- pid " << getpid() << " at " << GetIsoDate() << " -----\n";

  DumpCmdLine(os);

  // Note: The strings "Build fingerprint:" and "ABI:" are chosen to match the format used by
  // debuggerd. This allows, for example, the stack tool to work.
  std::string fingerprint = runtime->GetFingerprint();
  os << "Build fingerprint: '" << (fingerprint.empty() ? "unknown" : fingerprint) << "'\n";
  os << "ABI: '" << GetInstructionSetString(runtime->GetInstructionSet()) << "'\n";

  os << "Build type: " << (kIsDebugBuild ? "debug" : "optimized") << "\n";

  os << "Debug Store: " << DebugStoreGetString() << "\n";

  if (art_flags::always_enable_profile_code()) {
    os << "LongRunningMethods: " << TraceProfiler::GetLongRunningMethodsString() << "\n";
  }

  runtime->DumpForSigQuit(os);

  if ((false)) {
    std::string maps;
    if (android::base::ReadFileToString("/proc/self/maps", &maps)) {
      os << "/proc/self/maps:\n" << maps;
    }
  }
  os << "----- end " << getpid() << " -----\n";
  Output(os.str());
  sigquit_nanotime_ = std::nullopt;
}

void SignalCatcher::HandleSigUsr1() {
  LOG(INFO) << "SIGUSR1 forcing GC (no HPROF) and profile save";
  Runtime::Current()->GetHeap()->CollectGarbage(/* clear_soft_references= */ false);
  ProfileSaver::ForceProcessProfiles();
}

int SignalCatcher::WaitForSignal(Thread* self, SignalSet& signals) {
  ScopedThreadStateChange tsc(self, ThreadState::kWaitingInMainSignalCatcherLoop);

  // Signals for sigwait() must be blocked but not ignored.  We
  // block signals like SIGQUIT for all threads, so the condition
  // is met.  When the signal hits, we wake up, without any signal
  // handlers being invoked.
  int signal_number = signals.Wait();
  if (!ShouldHalt()) {
    // Let the user know we got the signal, just in case the system's too screwed for us to
    // actually do what they want us to do...
    LOG(INFO) << *self << ": reacting to signal " << signal_number;

    // If anyone's holding locks (which might prevent us from getting back into state Runnable), say so...
    Runtime::Current()->DumpLockHolders(LOG_STREAM(INFO));
  }

  return signal_number;
}

void* SignalCatcher::Run(void* arg) {
  SignalCatcher* signal_catcher = reinterpret_cast<SignalCatcher*>(arg);
  CHECK(signal_catcher != nullptr);

  Runtime* runtime = Runtime::Current();
  CHECK(runtime->AttachCurrentThread("Signal Catcher", true, runtime->GetSystemThreadGroup(),
                                     !runtime->IsAotCompiler()));

  Thread* self = Thread::Current();
  DCHECK_NE(self->GetState(), ThreadState::kRunnable);
  {
    MutexLock mu(self, signal_catcher->lock_);
    signal_catcher->thread_ = self;
    signal_catcher->cond_.Broadcast(self);
  }

  // Set up mask with signals we want to handle.
  SignalSet signals;
  signals.Add(SIGQUIT);
  signals.Add(SIGUSR1);

  while (true) {
    int signal_number = signal_catcher->WaitForSignal(self, signals);
    if (signal_catcher->ShouldHalt()) {
      runtime->DetachCurrentThread();
      return nullptr;
    }

    switch (signal_number) {
    case SIGQUIT:
      signal_catcher->HandleSigQuit();
      break;
    case SIGUSR1:
      signal_catcher->HandleSigUsr1();
      break;
    default:
      LOG(ERROR) << "Unexpected signal %d" << signal_number;
      break;
    }
  }
}

}  // namespace art

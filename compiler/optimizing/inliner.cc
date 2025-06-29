/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "inliner.h"

#include "art_method-inl.h"
#include "base/logging.h"
#include "base/pointer_size.h"
#include "builder.h"
#include "class_linker.h"
#include "class_root-inl.h"
#include "compiler_callbacks.h"
#include "constant_folding.h"
#include "data_type-inl.h"
#include "dead_code_elimination.h"
#include "dex/inline_method_analyser.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "handle_cache-inl.h"
#include "instruction_simplifier.h"
#include "intrinsics.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-alloc-inl.h"
#include "mirror/object_array-inl.h"
#include "nodes.h"
#include "profiling_info_builder.h"
#include "reference_type_propagation.h"
#include "register_allocator_linear_scan.h"
#include "scoped_thread_state_change-inl.h"
#include "sharpening.h"
#include "ssa_builder.h"
#include "ssa_phi_elimination.h"
#include "thread.h"
#include "verifier/verifier_compiler_binding.h"

namespace art HIDDEN {

// Instruction limit to control memory.
static constexpr size_t kMaximumNumberOfTotalInstructions = 1024;

// Maximum number of instructions for considering a method small,
// which we will always try to inline if the other non-instruction limits
// are not reached.
static constexpr size_t kMaximumNumberOfInstructionsForSmallMethod = 3;

// Limit the number of dex registers that we accumulate while inlining
// to avoid creating large amount of nested environments.
static constexpr size_t kMaximumNumberOfCumulatedDexRegisters = 32;

// Limit recursive call inlining, which do not benefit from too
// much inlining compared to code locality.
static constexpr size_t kMaximumNumberOfRecursiveCalls = 4;

// Limit recursive polymorphic call inlining to prevent code bloat, since it can quickly get out of
// hand in the presence of multiple Wrapper classes. We set this to 0 to disallow polymorphic
// recursive calls at all.
static constexpr size_t kMaximumNumberOfPolymorphicRecursiveCalls = 0;

// Controls the use of inline caches in AOT mode.
static constexpr bool kUseAOTInlineCaches = true;

// Controls the use of inlining try catches.
static constexpr bool kInlineTryCatches = true;

// We check for line numbers to make sure the DepthString implementation
// aligns the output nicely.
#define LOG_INTERNAL(msg) \
  static_assert(__LINE__ > 10, "Unhandled line number"); \
  static_assert(__LINE__ < 10000, "Unhandled line number"); \
  VLOG(compiler) << DepthString(__LINE__) << msg

#define LOG_TRY() LOG_INTERNAL("Try inlinining call: ")
#define LOG_NOTE() LOG_INTERNAL("Note: ")
#define LOG_SUCCESS() LOG_INTERNAL("Success: ")
#define LOG_FAIL(stats_ptr, stat) MaybeRecordStat(stats_ptr, stat); LOG_INTERNAL("Fail: ")
#define LOG_FAIL_NO_STAT() LOG_INTERNAL("Fail: ")

std::string HInliner::DepthString(int line) const {
  std::string value;
  // Indent according to the inlining depth.
  size_t count = depth_;
  // Line numbers get printed in the log, so add a space if the log's line is less
  // than 1000, and two if less than 100. 10 cannot be reached as it's the copyright.
  if (!kIsTargetBuild) {
    if (line < 100) {
      value += " ";
    }
    if (line < 1000) {
      value += " ";
    }
    // Safeguard if this file reaches more than 10000 lines.
    DCHECK_LT(line, 10000);
  }
  for (size_t i = 0; i < count; ++i) {
    value += "  ";
  }
  return value;
}

static size_t CountNumberOfInstructions(HGraph* graph) {
  size_t number_of_instructions = 0;
  for (HBasicBlock* block : graph->GetReversePostOrderSkipEntryBlock()) {
    for (HInstructionIterator instr_it(block->GetInstructions());
         !instr_it.Done();
         instr_it.Advance()) {
      ++number_of_instructions;
    }
  }
  return number_of_instructions;
}

void HInliner::UpdateInliningBudget() {
  if (total_number_of_instructions_ >= kMaximumNumberOfTotalInstructions) {
    // Always try to inline small methods.
    inlining_budget_ = kMaximumNumberOfInstructionsForSmallMethod;
  } else {
    inlining_budget_ = std::max(
        kMaximumNumberOfInstructionsForSmallMethod,
        kMaximumNumberOfTotalInstructions - total_number_of_instructions_);
  }
}

bool HInliner::Run() {
  if (codegen_->GetCompilerOptions().GetInlineMaxCodeUnits() == 0) {
    // Inlining effectively disabled.
    return false;
  } else if (graph_->IsDebuggable()) {
    // For simplicity, we currently never inline when the graph is debuggable. This avoids
    // doing some logic in the runtime to discover if a method could have been inlined.
    return false;
  }

  bool did_inline = false;

  // Initialize the number of instructions for the method being compiled. Recursive calls
  // to HInliner::Run have already updated the instruction count.
  if (outermost_graph_ == graph_) {
    total_number_of_instructions_ = CountNumberOfInstructions(graph_);
  }

  UpdateInliningBudget();
  DCHECK_NE(total_number_of_instructions_, 0u);
  DCHECK_NE(inlining_budget_, 0u);

  // If we're compiling tests, honor inlining directives in method names:
  // - if a method's name contains the substring "$noinline$", do not
  //   inline that method;
  // - if a method's name contains the substring "$inline$", ensure
  //   that this method is actually inlined.
  // We limit the latter to AOT compilation, as the JIT may or may not inline
  // depending on the state of classes at runtime.
  const bool honor_noinline_directives = codegen_->GetCompilerOptions().CompileArtTest();
  const bool honor_inline_directives =
      honor_noinline_directives &&
      Runtime::Current()->IsAotCompiler() &&
      !graph_->IsCompilingBaseline();

  // Keep a copy of all blocks when starting the visit.
  ArenaVector<HBasicBlock*> blocks = graph_->GetReversePostOrder();
  DCHECK(!blocks.empty());
  // Because we are changing the graph when inlining,
  // we just iterate over the blocks of the outer method.
  // This avoids doing the inlining work again on the inlined blocks.
  for (HBasicBlock* block : blocks) {
    for (HInstruction* instruction = block->GetFirstInstruction(); instruction != nullptr;) {
      HInstruction* next = instruction->GetNext();
      HInvoke* call = instruction->AsInvokeOrNull();
      // As long as the call is not intrinsified, it is worth trying to inline.
      if (call != nullptr && !codegen_->IsImplementedIntrinsic(call)) {
        if (honor_noinline_directives) {
          // Debugging case: directives in method names control or assert on inlining.
          std::string callee_name =
              call->GetMethodReference().PrettyMethod(/* with_signature= */ false);
          // Tests prevent inlining by having $noinline$ in their method names.
          if (callee_name.find("$noinline$") == std::string::npos) {
            if (TryInline(call)) {
              did_inline = true;
            } else if (honor_inline_directives) {
              bool should_have_inlined = (callee_name.find("$inline$") != std::string::npos);
              CHECK(!should_have_inlined) << "Could not inline " << callee_name;
            }
          }
        } else {
          DCHECK(!honor_inline_directives);
          // Normal case: try to inline.
          if (TryInline(call)) {
            did_inline = true;
          }
        }
      }
      instruction = next;
    }
  }

  if (run_extra_type_propagation_) {
    ReferenceTypePropagation rtp_fixup(graph_,
                                       outer_compilation_unit_.GetDexCache(),
                                       /* is_first_run= */ false);
    rtp_fixup.Run();
  }

  // We return true if we either inlined at least one method, or we marked one of our methods as
  // always throwing.
  // To check if we added an always throwing method we can either:
  //   1) Pass a boolean throughout the pipeline and get an accurate result, or
  //   2) Just check that the `HasAlwaysThrowingInvokes()` flag is true now. This is not 100%
  //     accurate but the only other part where we set `HasAlwaysThrowingInvokes` is constant
  //     folding the DivideUnsigned intrinsics for when the divisor is known to be 0. This case is
  //     rare enough that changing the pipeline for this is not worth it. In the case of the false
  //     positive (i.e. A) we didn't inline at all, B) the graph already had an always throwing
  //     invoke, and C) we didn't set any new always throwing invokes), we will be running constant
  //     folding, instruction simplifier, and dead code elimination one more time even though it
  //     shouldn't change things. There's no false negative case.
  return did_inline || graph_->HasAlwaysThrowingInvokes();
}

static bool IsMethodOrDeclaringClassFinal(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return method->IsFinal() || method->GetDeclaringClass()->IsFinal();
}

/**
 * Given the `resolved_method` looked up in the dex cache, try to find
 * the actual runtime target of an interface or virtual call.
 * Return nullptr if the runtime target cannot be proven.
 */
static ArtMethod* FindVirtualOrInterfaceTarget(HInvoke* invoke, ReferenceTypeInfo info)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* resolved_method = invoke->GetResolvedMethod();
  if (IsMethodOrDeclaringClassFinal(resolved_method)) {
    // No need to lookup further, the resolved method will be the target.
    return resolved_method;
  }

  if (info.GetTypeHandle()->IsInterface()) {
    // Statically knowing that the receiver has an interface type cannot
    // help us find what is the target method.
    return nullptr;
  } else if (!resolved_method->GetDeclaringClass()->IsAssignableFrom(info.GetTypeHandle().Get())) {
    // The method that we're trying to call is not in the receiver's class or super classes.
    return nullptr;
  } else if (info.GetTypeHandle()->IsErroneous()) {
    // If the type is erroneous, do not go further, as we are going to query the vtable or
    // imt table, that we can only safely do on non-erroneous classes.
    return nullptr;
  }

  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  PointerSize pointer_size = cl->GetImagePointerSize();
  if (invoke->IsInvokeInterface()) {
    resolved_method = info.GetTypeHandle()->FindVirtualMethodForInterface(
        resolved_method, pointer_size);
  } else {
    DCHECK(invoke->IsInvokeVirtual());
    resolved_method = info.GetTypeHandle()->FindVirtualMethodForVirtual(
        resolved_method, pointer_size);
  }

  if (resolved_method == nullptr) {
    // The information we had on the receiver was not enough to find
    // the target method. Since we check above the exact type of the receiver,
    // the only reason this can happen is an IncompatibleClassChangeError.
    return nullptr;
  } else if (!resolved_method->IsInvokable()) {
    // The information we had on the receiver was not enough to find
    // the target method. Since we check above the exact type of the receiver,
    // the only reason this can happen is an IncompatibleClassChangeError.
    return nullptr;
  } else if (IsMethodOrDeclaringClassFinal(resolved_method)) {
    // A final method has to be the target method.
    return resolved_method;
  } else if (info.IsExact()) {
    // If we found a method and the receiver's concrete type is statically
    // known, we know for sure the target.
    return resolved_method;
  } else {
    // Even if we did find a method, the receiver type was not enough to
    // statically find the runtime target.
    return nullptr;
  }
}

static uint32_t FindMethodIndexIn(ArtMethod* method,
                                  const DexFile& dex_file,
                                  uint32_t name_and_signature_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (IsSameDexFile(*method->GetDexFile(), dex_file)) {
    return method->GetDexMethodIndex();
  } else {
    return method->FindDexMethodIndexInOtherDexFile(dex_file, name_and_signature_index);
  }
}

static dex::TypeIndex FindClassIndexIn(ObjPtr<mirror::Class> cls,
                                       const DexCompilationUnit& compilation_unit)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = *compilation_unit.GetDexFile();
  dex::TypeIndex index;
  if (cls->GetDexCache() == nullptr) {
    DCHECK(cls->IsArrayClass()) << cls->PrettyClass();
    index = cls->FindTypeIndexInOtherDexFile(dex_file);
  } else if (!cls->GetDexTypeIndex().IsValid()) {
    DCHECK(cls->IsProxyClass()) << cls->PrettyClass();
    // TODO: deal with proxy classes.
  } else if (IsSameDexFile(cls->GetDexFile(), dex_file)) {
    DCHECK_EQ(cls->GetDexCache(), compilation_unit.GetDexCache().Get());
    index = cls->GetDexTypeIndex();
  } else {
    index = cls->FindTypeIndexInOtherDexFile(dex_file);
    // We cannot guarantee the entry will resolve to the same class,
    // as there may be different class loaders. So only return the index if it's
    // the right class already resolved with the class loader.
    if (index.IsValid()) {
      ObjPtr<mirror::Class> resolved = compilation_unit.GetClassLinker()->LookupResolvedType(
          index, compilation_unit.GetDexCache().Get(), compilation_unit.GetClassLoader().Get());
      if (resolved != cls) {
        index = dex::TypeIndex::Invalid();
      }
    }
  }

  return index;
}

HInliner::InlineCacheType HInliner::GetInlineCacheType(
    const StackHandleScope<InlineCache::kIndividualCacheSize>& classes) {
  DCHECK_EQ(classes.Capacity(), InlineCache::kIndividualCacheSize);
  uint8_t number_of_types = classes.Size();
  if (number_of_types == 0) {
    return kInlineCacheUninitialized;
  } else if (number_of_types == 1) {
    return kInlineCacheMonomorphic;
  } else if (number_of_types == InlineCache::kIndividualCacheSize) {
    return kInlineCacheMegamorphic;
  } else {
    return kInlineCachePolymorphic;
  }
}

static inline ObjPtr<mirror::Class> GetMonomorphicType(
    const StackHandleScope<InlineCache::kIndividualCacheSize>& classes)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(classes.GetReference(0) != nullptr);
  return classes.GetReference(0)->AsClass();
}

ArtMethod* HInliner::FindMethodFromCHA(ArtMethod* resolved_method) {
  if (!resolved_method->HasSingleImplementation()) {
    return nullptr;
  }
  if (Runtime::Current()->IsAotCompiler()) {
    // No CHA-based devirtulization for AOT compiler (yet).
    return nullptr;
  }
  if (Runtime::Current()->IsZygote()) {
    // No CHA-based devirtulization for Zygote, as it compiles with
    // offline information.
    return nullptr;
  }
  if (outermost_graph_->IsCompilingOsr()) {
    // We do not support HDeoptimize in OSR methods.
    return nullptr;
  }
  PointerSize pointer_size = caller_compilation_unit_.GetClassLinker()->GetImagePointerSize();
  ArtMethod* single_impl = resolved_method->GetSingleImplementation(pointer_size);
  if (single_impl == nullptr) {
    return nullptr;
  }
  if (single_impl->IsProxyMethod()) {
    // Proxy method is a generic invoker that's not worth
    // devirtualizing/inlining. It also causes issues when the proxy
    // method is in another dex file if we try to rewrite invoke-interface to
    // invoke-virtual because a proxy method doesn't have a real dex file.
    return nullptr;
  }
  if (!single_impl->GetDeclaringClass()->IsResolved()) {
    // There's a race with the class loading, which updates the CHA info
    // before setting the class to resolved. So we just bail for this
    // rare occurence.
    return nullptr;
  }
  return single_impl;
}

static bool IsMethodVerified(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (method->GetDeclaringClass()->IsVerified()) {
    return true;
  }
  // For AOT, we check if the class has a verification status that allows us to
  // inline / analyze.
  // At runtime, we know this is cold code if the class is not verified, so don't
  // bother analyzing.
  if (Runtime::Current()->IsAotCompiler()) {
    if (method->GetDeclaringClass()->IsVerifiedNeedsAccessChecks()) {
      DCHECK(!Runtime::Current()->GetCompilerCallbacks()->IsUncompilableMethod(
                  MethodReference(method->GetDexFile(), method->GetDexMethodIndex())));
      return true;
    }
    if (method->GetDeclaringClass()->ShouldVerifyAtRuntime()) {
      return !Runtime::Current()->GetCompilerCallbacks()->IsUncompilableMethod(
          MethodReference(method->GetDexFile(), method->GetDexMethodIndex()));
    }
  }
  return false;
}

static bool AlwaysThrows(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(method != nullptr);
  // Skip non-compilable and unverified methods.
  if (!method->IsCompilable() || !IsMethodVerified(method)) {
    return false;
  }

  // Skip native methods, methods with try blocks, and methods that are too large.
  // TODO(solanes): We could correctly mark methods with try/catch blocks as always throwing as long
  // as we can get rid of the infinite loop cases. These cases (e.g. `void foo() {while (true) {}}`)
  // are the only ones that can have no return instruction and still not be an "always throwing
  // method". Unfortunately, we need to construct the graph to know there's an infinite loop and
  // therefore not worth the trouble.
  CodeItemDataAccessor accessor(method->DexInstructionData());
  if (!accessor.HasCodeItem() ||
      accessor.TriesSize() != 0 ||
      accessor.InsnsSizeInCodeUnits() > kMaximumNumberOfTotalInstructions) {
    return false;
  }
  // Scan for exits.
  bool throw_seen = false;
  for (const DexInstructionPcPair& pair : accessor) {
    switch (pair.Inst().Opcode()) {
      case Instruction::RETURN:
      case Instruction::RETURN_VOID:
      case Instruction::RETURN_WIDE:
      case Instruction::RETURN_OBJECT:
        return false;  // found regular control flow back
      case Instruction::THROW:
        throw_seen = true;
        break;
      default:
        break;
    }
  }
  return throw_seen;
}

bool HInliner::TryInline(HInvoke* invoke_instruction) {
  MaybeRecordStat(stats_, MethodCompilationStat::kTryInline);

  // Don't bother to move further if we know the method is unresolved or the invocation is
  // polymorphic (invoke-{polymorphic,custom}).
  if (invoke_instruction->IsInvokeUnresolved()) {
    MaybeRecordStat(stats_, MethodCompilationStat::kNotInlinedUnresolved);
    return false;
  } else if (invoke_instruction->IsInvokePolymorphic()) {
    MaybeRecordStat(stats_, MethodCompilationStat::kNotInlinedPolymorphic);
    return false;
  } else if (invoke_instruction->IsInvokeCustom()) {
    MaybeRecordStat(stats_, MethodCompilationStat::kNotInlinedCustom);
    return false;
  }

  ScopedObjectAccess soa(Thread::Current());
  LOG_TRY() << invoke_instruction->GetMethodReference().PrettyMethod();

  ArtMethod* resolved_method = invoke_instruction->GetResolvedMethod();
  if (resolved_method == nullptr) {
    DCHECK(invoke_instruction->IsInvokeStaticOrDirect());
    DCHECK(invoke_instruction->AsInvokeStaticOrDirect()->IsStringInit());
    LOG_FAIL_NO_STAT() << "Not inlining a String.<init> method";
    return false;
  }

  ArtMethod* actual_method = nullptr;
  ReferenceTypeInfo receiver_info = ReferenceTypeInfo::CreateInvalid();
  if (invoke_instruction->GetInvokeType() == kStatic) {
    actual_method = invoke_instruction->GetResolvedMethod();
  } else {
    HInstruction* receiver = invoke_instruction->InputAt(0);
    while (receiver->IsNullCheck()) {
      // Due to multiple levels of inlining within the same pass, it might be that
      // null check does not have the reference type of the actual receiver.
      receiver = receiver->InputAt(0);
    }
    receiver_info = receiver->GetReferenceTypeInfo();
    if (!receiver_info.IsValid()) {
      // We have to run the extra type propagation now as we are requiring the RTI.
      DCHECK(run_extra_type_propagation_);
      run_extra_type_propagation_ = false;
      ReferenceTypePropagation rtp_fixup(graph_,
                                         outer_compilation_unit_.GetDexCache(),
                                         /* is_first_run= */ false);
      rtp_fixup.Run();
      receiver_info = receiver->GetReferenceTypeInfo();
    }

    DCHECK(receiver_info.IsValid()) << "Invalid RTI for " << receiver->DebugName();
    if (invoke_instruction->IsInvokeStaticOrDirect()) {
      actual_method = invoke_instruction->GetResolvedMethod();
    } else {
      actual_method = FindVirtualOrInterfaceTarget(invoke_instruction, receiver_info);
    }
  }

  if (actual_method != nullptr) {
    // Single target.
    bool result = TryInlineAndReplace(invoke_instruction,
                                      actual_method,
                                      receiver_info,
                                      /* do_rtp= */ true,
                                      /* is_speculative= */ false);
    if (result) {
      MaybeRecordStat(stats_, MethodCompilationStat::kInlinedInvokeVirtualOrInterface);
      if (outermost_graph_ == graph_) {
        MaybeRecordStat(stats_, MethodCompilationStat::kInlinedLastInvokeVirtualOrInterface);
      }
    } else {
      HInvoke* invoke_to_analyze = nullptr;
      if (TryDevirtualize(invoke_instruction, actual_method, &invoke_to_analyze)) {
        // Consider devirtualization as inlining.
        result = true;
        MaybeRecordStat(stats_, MethodCompilationStat::kDevirtualized);
      } else {
        invoke_to_analyze = invoke_instruction;
      }
      // Set always throws property for non-inlined method call with single target.
      if (invoke_instruction->AlwaysThrows() || AlwaysThrows(actual_method)) {
        invoke_to_analyze->SetAlwaysThrows(/* always_throws= */ true);
        graph_->SetHasAlwaysThrowingInvokes(/* value= */ true);
      }
    }
    return result;
  }

  if (graph_->IsCompilingBaseline()) {
    LOG_FAIL_NO_STAT() << "Call to " << invoke_instruction->GetMethodReference().PrettyMethod()
                       << " not inlined because we are compiling baseline and we could not"
                       << " statically resolve the target";
    // For baseline compilation, we will collect inline caches, so we should not
    // try to inline using them.
    outermost_graph_->SetUsefulOptimizing();
    return false;
  }

  DCHECK(!invoke_instruction->IsInvokeStaticOrDirect());

  // No try catch inlining allowed here, or recursively. For try catch inlining we are banking on
  // the fact that we have a unique dex pc list. We cannot guarantee that for some TryInline methods
  // e.g. `TryInlinePolymorphicCall`.
  // TODO(solanes): Setting `try_catch_inlining_allowed_` to false here covers all cases from
  // `TryInlineFromCHA` and from `TryInlineFromInlineCache` as well (e.g.
  // `TryInlinePolymorphicCall`). Reassess to see if we can inline inline catch blocks in
  // `TryInlineFromCHA`, `TryInlineMonomorphicCall` and `TryInlinePolymorphicCallToSameTarget`.

  // We store the value to restore it since we will use the same HInliner instance for other inlinee
  // candidates.
  const bool previous_value = try_catch_inlining_allowed_;
  try_catch_inlining_allowed_ = false;

  if (TryInlineFromCHA(invoke_instruction)) {
    try_catch_inlining_allowed_ = previous_value;
    return true;
  }

  const bool result = TryInlineFromInlineCache(invoke_instruction);
  try_catch_inlining_allowed_ = previous_value;
  return result;
}

bool HInliner::TryInlineFromCHA(HInvoke* invoke_instruction) {
  ArtMethod* method = FindMethodFromCHA(invoke_instruction->GetResolvedMethod());
  if (method == nullptr) {
    return false;
  }
  LOG_NOTE() << "Try CHA-based inlining of " << method->PrettyMethod();

  uint32_t dex_pc = invoke_instruction->GetDexPc();
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();
  Handle<mirror::Class> cls = graph_->GetHandleCache()->NewHandle(method->GetDeclaringClass());
  if (!TryInlineAndReplace(invoke_instruction,
                           method,
                           ReferenceTypeInfo::Create(cls),
                           /* do_rtp= */ true,
                           /* is_speculative= */ true)) {
    return false;
  }
  AddCHAGuard(invoke_instruction, dex_pc, cursor, bb_cursor);
  // Add dependency due to devirtualization: we are assuming the resolved method
  // has a single implementation.
  outermost_graph_->AddCHASingleImplementationDependency(invoke_instruction->GetResolvedMethod());
  MaybeRecordStat(stats_, MethodCompilationStat::kCHAInline);
  return true;
}

bool HInliner::UseOnlyPolymorphicInliningWithNoDeopt() {
  // If we are compiling AOT or OSR, pretend the call using inline caches is polymorphic and
  // do not generate a deopt.
  //
  // For AOT:
  //    Generating a deopt does not ensure that we will actually capture the new types;
  //    and the danger is that we could be stuck in a loop with "forever" deoptimizations.
  //    Take for example the following scenario:
  //      - we capture the inline cache in one run
  //      - the next run, we deoptimize because we miss a type check, but the method
  //        never becomes hot again
  //    In this case, the inline cache will not be updated in the profile and the AOT code
  //    will keep deoptimizing.
  //    Another scenario is if we use profile compilation for a process which is not allowed
  //    to JIT (e.g. system server). If we deoptimize we will run interpreted code for the
  //    rest of the lifetime.
  // TODO(calin):
  //    This is a compromise because we will most likely never update the inline cache
  //    in the profile (unless there's another reason to deopt). So we might be stuck with
  //    a sub-optimal inline cache.
  //    We could be smarter when capturing inline caches to mitigate this.
  //    (e.g. by having different thresholds for new and old methods).
  //
  // For OSR:
  //     We may come from the interpreter and it may have seen different receiver types.
  return Runtime::Current()->IsAotCompiler() || outermost_graph_->IsCompilingOsr();
}
bool HInliner::TryInlineFromInlineCache(HInvoke* invoke_instruction)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (Runtime::Current()->IsAotCompiler() && !kUseAOTInlineCaches) {
    return false;
  }

  StackHandleScope<InlineCache::kIndividualCacheSize> classes(Thread::Current());
  // The Zygote JIT compiles based on a profile, so we shouldn't use runtime inline caches
  // for it.
  InlineCacheType inline_cache_type =
      (Runtime::Current()->IsAotCompiler() || Runtime::Current()->IsZygote())
          ? GetInlineCacheAOT(invoke_instruction, &classes)
          : GetInlineCacheJIT(invoke_instruction, &classes);

  switch (inline_cache_type) {
    case kInlineCacheNoData: {
      LOG_FAIL_NO_STAT()
          << "No inline cache information for call to "
          << invoke_instruction->GetMethodReference().PrettyMethod();
      return false;
    }

    case kInlineCacheUninitialized: {
      LOG_FAIL_NO_STAT()
          << "Interface or virtual call to "
          << invoke_instruction->GetMethodReference().PrettyMethod()
          << " is not hit and not inlined";
      return false;
    }

    case kInlineCacheMonomorphic: {
      MaybeRecordStat(stats_, MethodCompilationStat::kMonomorphicCall);
      if (UseOnlyPolymorphicInliningWithNoDeopt()) {
        return TryInlinePolymorphicCall(invoke_instruction, classes);
      } else {
        return TryInlineMonomorphicCall(invoke_instruction, classes);
      }
    }

    case kInlineCachePolymorphic: {
      MaybeRecordStat(stats_, MethodCompilationStat::kPolymorphicCall);
      return TryInlinePolymorphicCall(invoke_instruction, classes);
    }

    case kInlineCacheMegamorphic: {
      LOG_FAIL_NO_STAT()
          << "Interface or virtual call to "
          << invoke_instruction->GetMethodReference().PrettyMethod()
          << " is megamorphic and not inlined";
      MaybeRecordStat(stats_, MethodCompilationStat::kMegamorphicCall);
      return false;
    }

    case kInlineCacheMissingTypes: {
      LOG_FAIL_NO_STAT()
          << "Interface or virtual call to "
          << invoke_instruction->GetMethodReference().PrettyMethod()
          << " is missing types and not inlined";
      return false;
    }
  }
}

HInliner::InlineCacheType HInliner::GetInlineCacheJIT(
    HInvoke* invoke_instruction,
    /*out*/StackHandleScope<InlineCache::kIndividualCacheSize>* classes) {
  DCHECK(codegen_->GetCompilerOptions().IsJitCompiler());

  ArtMethod* caller = graph_->GetArtMethod();
  // Under JIT, we should always know the caller.
  DCHECK(caller != nullptr);

  InlineCache* cache = nullptr;
  // Start with the outer graph profiling info.
  ProfilingInfo* profiling_info = outermost_graph_->GetProfilingInfo();
  if (profiling_info != nullptr) {
    if (depth_ == 0) {
      cache = profiling_info->GetInlineCache(invoke_instruction->GetDexPc());
    } else {
      uint32_t dex_pc = ProfilingInfoBuilder::EncodeInlinedDexPc(
          this, codegen_->GetCompilerOptions(), invoke_instruction);
      if (dex_pc != kNoDexPc) {
        cache = profiling_info->GetInlineCache(dex_pc);
      }
    }
  }

  if (cache == nullptr) {
    // Check the current graph profiling info.
    profiling_info = graph_->GetProfilingInfo();
    if (profiling_info == nullptr) {
      return kInlineCacheNoData;
    }

    cache = profiling_info->GetInlineCache(invoke_instruction->GetDexPc());
  }

  if (cache == nullptr) {
    // Either we never hit this invoke and we never compiled the callee,
    // or the method wasn't resolved when we performed baseline compilation.
    // Bail for now.
    return kInlineCacheNoData;
  }
  Runtime::Current()->GetJit()->GetCodeCache()->CopyInlineCacheInto(*cache, classes);
  return GetInlineCacheType(*classes);
}

HInliner::InlineCacheType HInliner::GetInlineCacheAOT(
    HInvoke* invoke_instruction,
    /*out*/StackHandleScope<InlineCache::kIndividualCacheSize>* classes) {
  DCHECK_EQ(classes->Capacity(), InlineCache::kIndividualCacheSize);
  DCHECK_EQ(classes->Size(), 0u);

  const ProfileCompilationInfo* pci = codegen_->GetCompilerOptions().GetProfileCompilationInfo();
  if (pci == nullptr) {
    return kInlineCacheNoData;
  }

  ProfileCompilationInfo::MethodHotness hotness = pci->GetMethodHotness(MethodReference(
      caller_compilation_unit_.GetDexFile(), caller_compilation_unit_.GetDexMethodIndex()));
  if (!hotness.IsHot()) {
    return kInlineCacheNoData;  // no profile information for this invocation.
  }

  const ProfileCompilationInfo::InlineCacheMap* inline_caches = hotness.GetInlineCacheMap();
  DCHECK(inline_caches != nullptr);

  // Inlined inline caches are not supported in AOT, so we use the dex pc directly, and don't
  // call `InlineCache::EncodeDexPc`.
  // To support it, we would need to ensure `inline_max_code_units` remain the
  // same between dex2oat and runtime, for example by adding it to the boot
  // image oat header.
  const auto it = inline_caches->find(invoke_instruction->GetDexPc());
  if (it == inline_caches->end()) {
    return kInlineCacheUninitialized;
  }

  const ProfileCompilationInfo::DexPcData& dex_pc_data = it->second;
  if (dex_pc_data.is_missing_types) {
    return kInlineCacheMissingTypes;
  }
  if (dex_pc_data.is_megamorphic) {
    return kInlineCacheMegamorphic;
  }
  DCHECK_LE(dex_pc_data.classes.size(), InlineCache::kIndividualCacheSize);

  // Walk over the class descriptors and look up the actual classes.
  // If we cannot find a type we return kInlineCacheMissingTypes.
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  Thread* self = Thread::Current();
  for (const dex::TypeIndex& type_index : dex_pc_data.classes) {
    const DexFile* dex_file = caller_compilation_unit_.GetDexFile();
    size_t descriptor_length;
    const char* descriptor = pci->GetTypeDescriptor(dex_file, type_index, &descriptor_length);
    ObjPtr<mirror::Class> clazz = class_linker->FindClass(
        self, descriptor, descriptor_length, caller_compilation_unit_.GetClassLoader());
    if (clazz == nullptr) {
      self->ClearException();  // Clean up the exception left by type resolution.
      VLOG(compiler) << "Could not find class from inline cache in AOT mode "
          << invoke_instruction->GetMethodReference().PrettyMethod()
          << " : "
          << descriptor;
      return kInlineCacheMissingTypes;
    }
    DCHECK_LT(classes->Size(), classes->Capacity());
    classes->NewHandle(clazz);
  }

  return GetInlineCacheType(*classes);
}

HInstanceFieldGet* HInliner::BuildGetReceiverClass(HInstruction* receiver,
                                                   uint32_t dex_pc) const {
  ArtField* field = WellKnownClasses::java_lang_Object_shadowKlass;
  HInstanceFieldGet* result = new (graph_->GetAllocator()) HInstanceFieldGet(
      receiver,
      field,
      DataType::Type::kReference,
      field->GetOffset(),
      field->IsVolatile(),
      field->GetDexFieldIndex(),
      field->GetDeclaringClass()->GetDexClassDefIndex(),
      *field->GetDexFile(),
      dex_pc);
  // The class of a field is effectively final, and does not have any memory dependencies.
  result->SetSideEffects(SideEffects::None());
  return result;
}

static ArtMethod* ResolveMethodFromInlineCache(Handle<mirror::Class> klass,
                                               HInvoke* invoke_instruction,
                                               PointerSize pointer_size)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* resolved_method = invoke_instruction->GetResolvedMethod();
  if (Runtime::Current()->IsAotCompiler()) {
    // We can get unrelated types when working with profiles (corruption,
    // systme updates, or anyone can write to it). So first check if the class
    // actually implements the declaring class of the method that is being
    // called in bytecode.
    // Note: the lookup methods used below require to have assignable types.
    if (!resolved_method->GetDeclaringClass()->IsAssignableFrom(klass.Get())) {
      return nullptr;
    }

    // Also check whether the type in the inline cache is an interface or an
    // abstract class. We only expect concrete classes in inline caches, so this
    // means the class was changed.
    if (klass->IsAbstract() || klass->IsInterface()) {
      return nullptr;
    }
  }

  if (invoke_instruction->IsInvokeInterface()) {
    resolved_method = klass->FindVirtualMethodForInterface(resolved_method, pointer_size);
  } else {
    DCHECK(invoke_instruction->IsInvokeVirtual());
    resolved_method = klass->FindVirtualMethodForVirtual(resolved_method, pointer_size);
  }
  // Even if the class exists we can still not have the function the
  // inline-cache targets if the profile is from far enough in the past/future.
  // We need to allow this since we don't update boot-profiles very often. This
  // can occur in boot-profiles with inline-caches.
  DCHECK(Runtime::Current()->IsAotCompiler() || resolved_method != nullptr);
  return resolved_method;
}

bool HInliner::TryInlineMonomorphicCall(
    HInvoke* invoke_instruction,
    const StackHandleScope<InlineCache::kIndividualCacheSize>& classes) {
  DCHECK(invoke_instruction->IsInvokeVirtual() || invoke_instruction->IsInvokeInterface())
      << invoke_instruction->DebugName();

  dex::TypeIndex class_index = FindClassIndexIn(
      GetMonomorphicType(classes), caller_compilation_unit_);
  if (!class_index.IsValid()) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedDexCacheInaccessibleToCaller)
        << "Call to " << ArtMethod::PrettyMethod(invoke_instruction->GetResolvedMethod())
        << " from inline cache is not inlined because its class is not"
        << " accessible to the caller";
    return false;
  }

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  PointerSize pointer_size = class_linker->GetImagePointerSize();
  Handle<mirror::Class> monomorphic_type =
      graph_->GetHandleCache()->NewHandle(GetMonomorphicType(classes));
  ArtMethod* resolved_method = ResolveMethodFromInlineCache(
      monomorphic_type, invoke_instruction, pointer_size);
  if (resolved_method == nullptr) {
    // Bogus AOT profile, bail.
    DCHECK(Runtime::Current()->IsAotCompiler());
    return false;
  }

  LOG_NOTE() << "Try inline monomorphic call to " << resolved_method->PrettyMethod();
  HInstruction* receiver = invoke_instruction->InputAt(0);
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();
  if (!TryInlineAndReplace(invoke_instruction,
                           resolved_method,
                           ReferenceTypeInfo::Create(monomorphic_type, /* is_exact= */ true),
                           /* do_rtp= */ false,
                           /* is_speculative= */ true)) {
    return false;
  }

  // We successfully inlined, now add a guard.
  AddTypeGuard(receiver,
               cursor,
               bb_cursor,
               class_index,
               monomorphic_type,
               invoke_instruction,
               /* with_deoptimization= */ true);

  // Lazily run type propagation to get the guard typed, and eventually propagate the
  // type of the receiver.
  run_extra_type_propagation_ = true;

  MaybeRecordStat(stats_, MethodCompilationStat::kInlinedMonomorphicCall);
  return true;
}

void HInliner::AddCHAGuard(HInstruction* invoke_instruction,
                           uint32_t dex_pc,
                           HInstruction* cursor,
                           HBasicBlock* bb_cursor) {
  HShouldDeoptimizeFlag* deopt_flag = new (graph_->GetAllocator())
      HShouldDeoptimizeFlag(graph_->GetAllocator(), dex_pc);
  // ShouldDeoptimizeFlag is used to perform a deoptimization because of a CHA
  // invalidation or for debugging reasons. It is OK to just check for non-zero
  // value here instead of the specific CHA value. When a debugging deopt is
  // requested we deoptimize before we execute any code and hence we shouldn't
  // see that case here.
  HInstruction* compare = new (graph_->GetAllocator()) HNotEqual(
      deopt_flag, graph_->GetIntConstant(0));
  HInstruction* deopt = new (graph_->GetAllocator()) HDeoptimize(
      graph_->GetAllocator(), compare, DeoptimizationKind::kCHA, dex_pc);

  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(deopt_flag, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(deopt_flag, bb_cursor->GetFirstInstruction());
  }
  bb_cursor->InsertInstructionAfter(compare, deopt_flag);
  bb_cursor->InsertInstructionAfter(deopt, compare);

  // Add receiver as input to aid CHA guard optimization later.
  deopt_flag->AddInput(invoke_instruction->InputAt(0));
  DCHECK_EQ(deopt_flag->InputCount(), 1u);
  deopt->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
  outermost_graph_->IncrementNumberOfCHAGuards();
}

HInstruction* HInliner::AddTypeGuard(HInstruction* receiver,
                                     HInstruction* cursor,
                                     HBasicBlock* bb_cursor,
                                     dex::TypeIndex class_index,
                                     Handle<mirror::Class> klass,
                                     HInstruction* invoke_instruction,
                                     bool with_deoptimization) {
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  HInstanceFieldGet* receiver_class = BuildGetReceiverClass(
      receiver, invoke_instruction->GetDexPc());
  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(receiver_class, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(receiver_class, bb_cursor->GetFirstInstruction());
  }

  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  bool is_referrer;
  ArtMethod* outermost_art_method = outermost_graph_->GetArtMethod();
  if (outermost_art_method == nullptr) {
    DCHECK(Runtime::Current()->IsAotCompiler());
    // We are in AOT mode and we don't have an ART method to determine
    // if the inlined method belongs to the referrer. Assume it doesn't.
    is_referrer = false;
  } else {
    is_referrer = klass.Get() == outermost_art_method->GetDeclaringClass();
  }

  // Note that we will just compare the classes, so we don't need Java semantics access checks.
  // Note that the type index and the dex file are relative to the method this type guard is
  // inlined into.
  HLoadClass* load_class = new (graph_->GetAllocator()) HLoadClass(graph_->GetCurrentMethod(),
                                                                   class_index,
                                                                   caller_dex_file,
                                                                   klass,
                                                                   is_referrer,
                                                                   invoke_instruction->GetDexPc(),
                                                                   /* needs_access_check= */ false);
  HLoadClass::LoadKind kind = HSharpening::ComputeLoadClassKind(
      load_class, codegen_, caller_compilation_unit_);
  DCHECK(kind != HLoadClass::LoadKind::kInvalid)
      << "We should always be able to reference a class for inline caches";
  // Load kind must be set before inserting the instruction into the graph.
  load_class->SetLoadKind(kind);
  bb_cursor->InsertInstructionAfter(load_class, receiver_class);
  // In AOT mode, we will most likely load the class from BSS, which will involve a call
  // to the runtime. In this case, the load instruction will need an environment so copy
  // it from the invoke instruction.
  if (load_class->NeedsEnvironment()) {
    DCHECK(Runtime::Current()->IsAotCompiler());
    load_class->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
  }

  HNotEqual* compare = new (graph_->GetAllocator()) HNotEqual(load_class, receiver_class);
  bb_cursor->InsertInstructionAfter(compare, load_class);
  if (with_deoptimization) {
    HDeoptimize* deoptimize = new (graph_->GetAllocator()) HDeoptimize(
        graph_->GetAllocator(),
        compare,
        receiver,
        Runtime::Current()->IsAotCompiler()
            ? DeoptimizationKind::kAotInlineCache
            : DeoptimizationKind::kJitInlineCache,
        invoke_instruction->GetDexPc());
    bb_cursor->InsertInstructionAfter(deoptimize, compare);
    deoptimize->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
    DCHECK_EQ(invoke_instruction->InputAt(0), receiver);
    receiver->ReplaceUsesDominatedBy(deoptimize, deoptimize);
    deoptimize->SetReferenceTypeInfo(receiver->GetReferenceTypeInfo());
  }
  return compare;
}

static void MaybeReplaceAndRemove(HInstruction* new_instruction, HInstruction* old_instruction) {
  DCHECK(new_instruction != old_instruction);
  if (new_instruction != nullptr) {
    old_instruction->ReplaceWith(new_instruction);
  }
  old_instruction->GetBlock()->RemoveInstruction(old_instruction);
}

bool HInliner::TryInlinePolymorphicCall(
    HInvoke* invoke_instruction,
    const StackHandleScope<InlineCache::kIndividualCacheSize>& classes) {
  DCHECK(invoke_instruction->IsInvokeVirtual() || invoke_instruction->IsInvokeInterface())
      << invoke_instruction->DebugName();

  if (TryInlinePolymorphicCallToSameTarget(invoke_instruction, classes)) {
    return true;
  }

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  PointerSize pointer_size = class_linker->GetImagePointerSize();

  bool all_targets_inlined = true;
  bool one_target_inlined = false;
  DCHECK_EQ(classes.Capacity(), InlineCache::kIndividualCacheSize);
  uint8_t number_of_types = classes.Size();
  for (size_t i = 0; i != number_of_types; ++i) {
    DCHECK(classes.GetReference(i) != nullptr);
    Handle<mirror::Class> handle =
        graph_->GetHandleCache()->NewHandle(classes.GetReference(i)->AsClass());
    ArtMethod* method = ResolveMethodFromInlineCache(handle, invoke_instruction, pointer_size);
    if (method == nullptr) {
      DCHECK(Runtime::Current()->IsAotCompiler());
      // AOT profile is bogus. This loop expects to iterate over all entries,
      // so just just continue.
      all_targets_inlined = false;
      continue;
    }

    HInstruction* receiver = invoke_instruction->InputAt(0);
    HInstruction* cursor = invoke_instruction->GetPrevious();
    HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

    dex::TypeIndex class_index = FindClassIndexIn(handle.Get(), caller_compilation_unit_);
    HInstruction* return_replacement = nullptr;

    // In monomorphic cases when UseOnlyPolymorphicInliningWithNoDeopt() is true, we call
    // `TryInlinePolymorphicCall` even though we are monomorphic.
    const bool actually_monomorphic = number_of_types == 1;
    DCHECK_IMPLIES(actually_monomorphic, UseOnlyPolymorphicInliningWithNoDeopt());

    // We only want to limit recursive polymorphic cases, not monomorphic ones.
    const bool too_many_polymorphic_recursive_calls =
        !actually_monomorphic &&
        CountRecursiveCallsOf(method) > kMaximumNumberOfPolymorphicRecursiveCalls;
    if (too_many_polymorphic_recursive_calls) {
      LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedPolymorphicRecursiveBudget)
          << "Method " << method->PrettyMethod()
          << " is not inlined because it has reached its polymorphic recursive call budget.";
    } else if (class_index.IsValid()) {
      LOG_NOTE() << "Try inline polymorphic call to " << method->PrettyMethod();
    }

    if (too_many_polymorphic_recursive_calls ||
        !class_index.IsValid() ||
        !TryBuildAndInline(invoke_instruction,
                           method,
                           ReferenceTypeInfo::Create(handle, /* is_exact= */ true),
                           &return_replacement,
                           /* is_speculative= */ true)) {
      all_targets_inlined = false;
    } else {
      one_target_inlined = true;

      LOG_SUCCESS() << "Polymorphic call to "
                    << invoke_instruction->GetMethodReference().PrettyMethod()
                    << " has inlined " << ArtMethod::PrettyMethod(method);

      // If we have inlined all targets before, and this receiver is the last seen,
      // we deoptimize instead of keeping the original invoke instruction.
      bool deoptimize = !UseOnlyPolymorphicInliningWithNoDeopt() &&
          all_targets_inlined &&
          (i + 1 == number_of_types);

      HInstruction* compare = AddTypeGuard(receiver,
                                           cursor,
                                           bb_cursor,
                                           class_index,
                                           handle,
                                           invoke_instruction,
                                           deoptimize);
      if (deoptimize) {
        MaybeReplaceAndRemove(return_replacement, invoke_instruction);
      } else {
        CreateDiamondPatternForPolymorphicInline(compare, return_replacement, invoke_instruction);
      }
    }
  }

  if (!one_target_inlined) {
    LOG_FAIL_NO_STAT()
        << "Call to " << invoke_instruction->GetMethodReference().PrettyMethod()
        << " from inline cache is not inlined because none"
        << " of its targets could be inlined";
    return false;
  }

  MaybeRecordStat(stats_, MethodCompilationStat::kInlinedPolymorphicCall);

  // Lazily run type propagation to get the guards typed.
  run_extra_type_propagation_ = true;
  return true;
}

void HInliner::CreateDiamondPatternForPolymorphicInline(HInstruction* compare,
                                                        HInstruction* return_replacement,
                                                        HInstruction* invoke_instruction) {
  uint32_t dex_pc = invoke_instruction->GetDexPc();
  HBasicBlock* cursor_block = compare->GetBlock();
  HBasicBlock* original_invoke_block = invoke_instruction->GetBlock();
  ArenaAllocator* allocator = graph_->GetAllocator();

  // Spit the block after the compare: `cursor_block` will now be the start of the diamond,
  // and the returned block is the start of the then branch (that could contain multiple blocks).
  HBasicBlock* then = cursor_block->SplitAfterForInlining(compare);

  // Split the block containing the invoke before and after the invoke. The returned block
  // of the split before will contain the invoke and will be the otherwise branch of
  // the diamond. The returned block of the split after will be the merge block
  // of the diamond.
  HBasicBlock* end_then = invoke_instruction->GetBlock();
  HBasicBlock* otherwise = end_then->SplitBeforeForInlining(invoke_instruction);
  HBasicBlock* merge = otherwise->SplitAfterForInlining(invoke_instruction);

  // If the methods we are inlining return a value, we create a phi in the merge block
  // that will have the `invoke_instruction and the `return_replacement` as inputs.
  if (return_replacement != nullptr) {
    HPhi* phi = new (allocator) HPhi(
        allocator, kNoRegNumber, 0, HPhi::ToPhiType(invoke_instruction->GetType()), dex_pc);
    merge->AddPhi(phi);
    invoke_instruction->ReplaceWith(phi);
    phi->AddInput(return_replacement);
    phi->AddInput(invoke_instruction);
  }

  // Add the control flow instructions.
  otherwise->AddInstruction(new (allocator) HGoto(dex_pc));
  end_then->AddInstruction(new (allocator) HGoto(dex_pc));
  cursor_block->AddInstruction(new (allocator) HIf(compare, dex_pc));

  // Add the newly created blocks to the graph.
  graph_->AddBlock(then);
  graph_->AddBlock(otherwise);
  graph_->AddBlock(merge);

  // Set up successor (and implictly predecessor) relations.
  cursor_block->AddSuccessor(otherwise);
  cursor_block->AddSuccessor(then);
  end_then->AddSuccessor(merge);
  otherwise->AddSuccessor(merge);

  // Set up dominance information.
  then->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(then);
  otherwise->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(otherwise);
  merge->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(merge);

  // Update the revert post order.
  size_t index = IndexOfElement(graph_->reverse_post_order_, cursor_block);
  MakeRoomFor(&graph_->reverse_post_order_, 1, index);
  graph_->reverse_post_order_[++index] = then;
  index = IndexOfElement(graph_->reverse_post_order_, end_then);
  MakeRoomFor(&graph_->reverse_post_order_, 2, index);
  graph_->reverse_post_order_[++index] = otherwise;
  graph_->reverse_post_order_[++index] = merge;


  graph_->UpdateLoopAndTryInformationOfNewBlock(
      then, original_invoke_block, /* replace_if_back_edge= */ false);
  graph_->UpdateLoopAndTryInformationOfNewBlock(
      otherwise, original_invoke_block, /* replace_if_back_edge= */ false);

  // In case the original invoke location was a back edge, we need to update
  // the loop to now have the merge block as a back edge.
  graph_->UpdateLoopAndTryInformationOfNewBlock(
      merge, original_invoke_block, /* replace_if_back_edge= */ true);
}

bool HInliner::TryInlinePolymorphicCallToSameTarget(
    HInvoke* invoke_instruction,
    const StackHandleScope<InlineCache::kIndividualCacheSize>& classes) {
  // This optimization only works under JIT for now.
  if (!codegen_->GetCompilerOptions().IsJitCompiler()) {
    return false;
  }

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  PointerSize pointer_size = class_linker->GetImagePointerSize();

  ArtMethod* actual_method = nullptr;
  size_t method_index = invoke_instruction->IsInvokeVirtual()
      ? invoke_instruction->AsInvokeVirtual()->GetVTableIndex()
      : invoke_instruction->AsInvokeInterface()->GetImtIndex();

  // Check whether we are actually calling the same method among
  // the different types seen.
  DCHECK_EQ(classes.Capacity(), InlineCache::kIndividualCacheSize);
  uint8_t number_of_types = classes.Size();
  for (size_t i = 0; i != number_of_types; ++i) {
    DCHECK(classes.GetReference(i) != nullptr);
    ArtMethod* new_method = nullptr;
    if (invoke_instruction->IsInvokeInterface()) {
      new_method = classes.GetReference(i)->AsClass()->GetImt(pointer_size)->Get(
          method_index, pointer_size);
      if (new_method->IsRuntimeMethod()) {
        // Bail out as soon as we see a conflict trampoline in one of the target's
        // interface table.
        return false;
      }
    } else {
      DCHECK(invoke_instruction->IsInvokeVirtual());
      new_method =
          classes.GetReference(i)->AsClass()->GetEmbeddedVTableEntry(method_index, pointer_size);
    }
    DCHECK(new_method != nullptr);
    if (actual_method == nullptr) {
      actual_method = new_method;
    } else if (actual_method != new_method) {
      // Different methods, bailout.
      return false;
    }
  }

  HInstruction* receiver = invoke_instruction->InputAt(0);
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

  HInstruction* return_replacement = nullptr;
  Handle<mirror::Class> cls =
      graph_->GetHandleCache()->NewHandle(actual_method->GetDeclaringClass());
  if (!TryBuildAndInline(invoke_instruction,
                         actual_method,
                         ReferenceTypeInfo::Create(cls),
                         &return_replacement,
                         /* is_speculative= */ true)) {
    return false;
  }

  // We successfully inlined, now add a guard.
  HInstanceFieldGet* receiver_class = BuildGetReceiverClass(
      receiver, invoke_instruction->GetDexPc());

  DataType::Type type = Is64BitInstructionSet(graph_->GetInstructionSet())
      ? DataType::Type::kInt64
      : DataType::Type::kInt32;
  HClassTableGet* class_table_get = new (graph_->GetAllocator()) HClassTableGet(
      receiver_class,
      type,
      invoke_instruction->IsInvokeVirtual() ? HClassTableGet::TableKind::kVTable
                                            : HClassTableGet::TableKind::kIMTable,
      method_index,
      invoke_instruction->GetDexPc());

  HConstant* constant;
  if (type == DataType::Type::kInt64) {
    constant = graph_->GetLongConstant(reinterpret_cast<intptr_t>(actual_method));
  } else {
    constant = graph_->GetIntConstant(reinterpret_cast<intptr_t>(actual_method));
  }

  HNotEqual* compare = new (graph_->GetAllocator()) HNotEqual(class_table_get, constant);
  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(receiver_class, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(receiver_class, bb_cursor->GetFirstInstruction());
  }
  bb_cursor->InsertInstructionAfter(class_table_get, receiver_class);
  bb_cursor->InsertInstructionAfter(compare, class_table_get);

  if (outermost_graph_->IsCompilingOsr()) {
    CreateDiamondPatternForPolymorphicInline(compare, return_replacement, invoke_instruction);
  } else {
    HDeoptimize* deoptimize = new (graph_->GetAllocator()) HDeoptimize(
        graph_->GetAllocator(),
        compare,
        receiver,
        DeoptimizationKind::kJitSameTarget,
        invoke_instruction->GetDexPc());
    bb_cursor->InsertInstructionAfter(deoptimize, compare);
    deoptimize->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
    MaybeReplaceAndRemove(return_replacement, invoke_instruction);
    receiver->ReplaceUsesDominatedBy(deoptimize, deoptimize);
    deoptimize->SetReferenceTypeInfo(receiver->GetReferenceTypeInfo());
  }

  // Lazily run type propagation to get the guard typed.
  run_extra_type_propagation_ = true;
  MaybeRecordStat(stats_, MethodCompilationStat::kInlinedPolymorphicCall);

  LOG_SUCCESS() << "Inlined same polymorphic target " << actual_method->PrettyMethod();
  return true;
}

void HInliner::MaybeRunReferenceTypePropagation(HInstruction* replacement,
                                                HInvoke* invoke_instruction) {
  if (ReturnTypeMoreSpecific(replacement, invoke_instruction)) {
    // Actual return value has a more specific type than the method's declared
    // return type. Run RTP again on the outer graph to propagate it.
    ReferenceTypePropagation(graph_,
                             outer_compilation_unit_.GetDexCache(),
                             /* is_first_run= */ false).Run();
  }
}

bool HInliner::TryDevirtualize(HInvoke* invoke_instruction,
                               ArtMethod* method,
                               HInvoke** replacement) {
  DCHECK(invoke_instruction != *replacement);
  if (!invoke_instruction->IsInvokeInterface() && !invoke_instruction->IsInvokeVirtual()) {
    return false;
  }

  // Don't devirtualize to an intrinsic invalid after the builder phase. The ArtMethod might be an
  // intrinsic even when the HInvoke isn't e.g. java.lang.CharSequence.isEmpty (not an intrinsic)
  // can get devirtualized into java.lang.String.isEmpty (which is an intrinsic).
  if (method->IsIntrinsic() && !IsValidIntrinsicAfterBuilder(method->GetIntrinsic())) {
    return false;
  }

  // Don't bother trying to call directly a default conflict method. It
  // doesn't have a proper MethodReference, but also `GetCanonicalMethod`
  // will return an actual default implementation.
  if (method->IsDefaultConflicting()) {
    return false;
  }
  DCHECK(!method->IsProxyMethod());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  PointerSize pointer_size = cl->GetImagePointerSize();
  // The sharpening logic assumes the caller isn't passing a copied method.
  method = method->GetCanonicalMethod(pointer_size);
  uint32_t dex_method_index = FindMethodIndexIn(
      method,
      *invoke_instruction->GetMethodReference().dex_file,
      invoke_instruction->GetMethodReference().index);
  if (dex_method_index == dex::kDexNoIndex) {
    return false;
  }
  HInvokeStaticOrDirect::DispatchInfo dispatch_info =
      HSharpening::SharpenLoadMethod(method,
                                     /* has_method_id= */ true,
                                     /* for_interface_call= */ false,
                                     codegen_);
  DCHECK_NE(dispatch_info.code_ptr_location, CodePtrLocation::kCallCriticalNative);
  if (dispatch_info.method_load_kind == MethodLoadKind::kRuntimeCall) {
    // If sharpening returns that we need to load the method at runtime, keep
    // the virtual/interface call which will be faster.
    // Also, the entrypoints for runtime calls do not handle devirtualized
    // calls.
    return false;
  }

  HInvokeStaticOrDirect* new_invoke = new (graph_->GetAllocator()) HInvokeStaticOrDirect(
      graph_->GetAllocator(),
      invoke_instruction->GetNumberOfArguments(),
      invoke_instruction->GetNumberOfOutVRegs(),
      invoke_instruction->GetType(),
      invoke_instruction->GetDexPc(),
      MethodReference(invoke_instruction->GetMethodReference().dex_file, dex_method_index),
      method,
      dispatch_info,
      kDirect,
      MethodReference(method->GetDexFile(), method->GetDexMethodIndex()),
      HInvokeStaticOrDirect::ClinitCheckRequirement::kNone,
      !graph_->IsDebuggable());
  HInputsRef inputs = invoke_instruction->GetInputs();
  DCHECK_EQ(inputs.size(), invoke_instruction->GetNumberOfArguments());
  for (size_t index = 0; index != inputs.size(); ++index) {
    new_invoke->SetArgumentAt(index, inputs[index]);
  }
  if (HInvokeStaticOrDirect::NeedsCurrentMethodInput(dispatch_info)) {
    new_invoke->SetRawInputAt(new_invoke->GetCurrentMethodIndexUnchecked(),
                              graph_->GetCurrentMethod());
  }
  invoke_instruction->GetBlock()->InsertInstructionBefore(new_invoke, invoke_instruction);
  new_invoke->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
  if (invoke_instruction->GetType() == DataType::Type::kReference) {
    new_invoke->SetReferenceTypeInfoIfValid(invoke_instruction->GetReferenceTypeInfo());
  }
  *replacement = new_invoke;

  MaybeReplaceAndRemove(*replacement, invoke_instruction);
  // No need to call MaybeRunReferenceTypePropagation, as we know the return type
  // cannot be more specific.
  DCHECK(!ReturnTypeMoreSpecific(*replacement, invoke_instruction));
  return true;
}


bool HInliner::TryInlineAndReplace(HInvoke* invoke_instruction,
                                   ArtMethod* method,
                                   ReferenceTypeInfo receiver_type,
                                   bool do_rtp,
                                   bool is_speculative) {
  DCHECK(!codegen_->IsImplementedIntrinsic(invoke_instruction));
  HInstruction* return_replacement = nullptr;

  if (!TryBuildAndInline(
          invoke_instruction, method, receiver_type, &return_replacement, is_speculative)) {
    return false;
  }

  MaybeReplaceAndRemove(return_replacement, invoke_instruction);
  FixUpReturnReferenceType(method, return_replacement);
  if (do_rtp) {
    MaybeRunReferenceTypePropagation(return_replacement, invoke_instruction);
  }
  return true;
}

size_t HInliner::CountRecursiveCallsOf(ArtMethod* method) const {
  const HInliner* current = this;
  size_t count = 0;
  do {
    if (current->graph_->GetArtMethod() == method) {
      ++count;
    }
    current = current->parent_;
  } while (current != nullptr);
  return count;
}

static inline bool MayInline(const CompilerOptions& compiler_options,
                             const DexFile& inlined_from,
                             const DexFile& inlined_into) {
  // We're not allowed to inline across dex files if we're the no-inline-from dex file.
  if (!IsSameDexFile(inlined_from, inlined_into) &&
      ContainsElement(compiler_options.GetNoInlineFromDexFile(), &inlined_from)) {
    return false;
  }

  return true;
}

// Returns whether inlining is allowed based on ART semantics.
bool HInliner::IsInliningAllowed(ArtMethod* method, const CodeItemDataAccessor& accessor) const {
  if (!accessor.HasCodeItem()) {
    LOG_FAIL_NO_STAT()
        << "Method " << method->PrettyMethod() << " is not inlined because it is native";
    return false;
  }

  if (!method->IsCompilable()) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedNotCompilable)
        << "Method " << method->PrettyMethod()
        << " has soft failures un-handled by the compiler, so it cannot be inlined";
    return false;
  }

  if (!IsMethodVerified(method)) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedNotVerified)
        << "Method " << method->PrettyMethod()
        << " couldn't be verified, so it cannot be inlined";
    return false;
  }

  if (annotations::MethodIsNeverInline(*method->GetDexFile(),
                                       method->GetClassDef(),
                                       method->GetDexMethodIndex())) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedNeverInlineAnnotation)
        << "Method " << method->PrettyMethod()
        << " has the @NeverInline annotation so it won't be inlined";
    return false;
  }

  return true;
}

// Returns whether ART supports inlining this method.
//
// Some methods are not supported because they have features for which inlining
// is not implemented. For example, we do not currently support inlining throw
// instructions into a try block.
bool HInliner::IsInliningSupported(const HInvoke* invoke_instruction,
                                   ArtMethod* method,
                                   const CodeItemDataAccessor& accessor) const {
  if (method->IsProxyMethod()) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedProxy)
        << "Method " << method->PrettyMethod()
        << " is not inlined because of unimplemented inline support for proxy methods.";
    return false;
  }

  if (accessor.TriesSize() != 0) {
    if (!kInlineTryCatches) {
      LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedTryCatchDisabled)
          << "Method " << method->PrettyMethod()
          << " is not inlined because inlining try catches is disabled globally";
      return false;
    }
    const bool disallowed_try_catch_inlining =
        // Direct parent is a try block.
        invoke_instruction->GetBlock()->IsTryBlock() ||
        // Indirect parent disallows try catch inlining.
        !try_catch_inlining_allowed_;
    if (disallowed_try_catch_inlining) {
      LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedTryCatchCallee)
          << "Method " << method->PrettyMethod()
          << " is not inlined because it has a try catch and we are not supporting it for this"
          << " particular call. This is could be because e.g. it would be inlined inside another"
          << " try block, we arrived here from TryInlinePolymorphicCall, etc.";
      return false;
    }
  }

  if (invoke_instruction->IsInvokeStaticOrDirect() &&
      invoke_instruction->AsInvokeStaticOrDirect()->IsStaticWithImplicitClinitCheck()) {
    // Case of a static method that cannot be inlined because it implicitly
    // requires an initialization check of its declaring class.
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedDexCacheClinitCheck)
        << "Method " << method->PrettyMethod()
        << " is not inlined because it is static and requires a clinit"
        << " check that cannot be emitted due to Dex cache limitations";
    return false;
  }

  return true;
}

bool HInliner::IsInliningEncouraged(const HInvoke* invoke_instruction,
                                    ArtMethod* method,
                                    const CodeItemDataAccessor& accessor) const {
  if (CountRecursiveCallsOf(method) > kMaximumNumberOfRecursiveCalls) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedRecursiveBudget)
        << "Method "
        << method->PrettyMethod()
        << " is not inlined because it has reached its recursive call budget.";
    return false;
  }

  size_t inline_max_code_units = codegen_->GetCompilerOptions().GetInlineMaxCodeUnits();
  if (accessor.InsnsSizeInCodeUnits() > inline_max_code_units) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedCodeItem)
        << "Method " << method->PrettyMethod()
        << " is not inlined because its code item is too big: "
        << accessor.InsnsSizeInCodeUnits()
        << " > "
        << inline_max_code_units;
    return false;
  }

  if (graph_->IsCompilingBaseline() &&
      accessor.InsnsSizeInCodeUnits() > CompilerOptions::kBaselineInlineMaxCodeUnits) {
    LOG_FAIL_NO_STAT() << "Reached baseline maximum code unit for inlining  "
                       << method->PrettyMethod();
    outermost_graph_->SetUsefulOptimizing();
    return false;
  }

  if (invoke_instruction->GetBlock()->GetLastInstruction()->IsThrow()) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedEndsWithThrow)
        << "Method " << method->PrettyMethod()
        << " is not inlined because its block ends with a throw";
    return false;
  }

  return true;
}

bool HInliner::TryBuildAndInline(HInvoke* invoke_instruction,
                                 ArtMethod* method,
                                 ReferenceTypeInfo receiver_type,
                                 HInstruction** return_replacement,
                                 bool is_speculative) {
  DCHECK_IMPLIES(method->IsStatic(), !receiver_type.IsValid());
  DCHECK_IMPLIES(!method->IsStatic(), receiver_type.IsValid());
  // If invoke_instruction is devirtualized to a different method, give intrinsics
  // another chance before we try to inline it.
  if (invoke_instruction->GetResolvedMethod() != method &&
      method->IsIntrinsic() &&
      IsValidIntrinsicAfterBuilder(method->GetIntrinsic())) {
    MaybeRecordStat(stats_, MethodCompilationStat::kIntrinsicRecognized);
    // For simplicity, always create a new instruction to replace the existing
    // invoke.
    HInvokeVirtual* new_invoke = new (graph_->GetAllocator()) HInvokeVirtual(
        graph_->GetAllocator(),
        invoke_instruction->GetNumberOfArguments(),
        invoke_instruction->GetNumberOfOutVRegs(),
        invoke_instruction->GetType(),
        invoke_instruction->GetDexPc(),
        invoke_instruction->GetMethodReference(),  // Use existing invoke's method's reference.
        method,
        MethodReference(method->GetDexFile(), method->GetDexMethodIndex()),
        method->GetMethodIndex(),
        !graph_->IsDebuggable());
    DCHECK_NE(new_invoke->GetIntrinsic(), Intrinsics::kNone);
    HInputsRef inputs = invoke_instruction->GetInputs();
    for (size_t index = 0; index != inputs.size(); ++index) {
      new_invoke->SetArgumentAt(index, inputs[index]);
    }
    invoke_instruction->GetBlock()->InsertInstructionBefore(new_invoke, invoke_instruction);
    new_invoke->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
    if (invoke_instruction->GetType() == DataType::Type::kReference) {
      new_invoke->SetReferenceTypeInfoIfValid(invoke_instruction->GetReferenceTypeInfo());
    }
    *return_replacement = new_invoke;
    return true;
  }

  CodeItemDataAccessor accessor(method->DexInstructionData());

  if (!IsInliningAllowed(method, accessor)) {
    return false;
  }

  // We have checked above that inlining is "allowed" to make sure that the method has bytecode
  // (is not native), is compilable and verified and to enforce the @NeverInline annotation.
  // However, the pattern substitution is always preferable, so we do it before the check if
  // inlining is "encouraged". It also has an exception to the `MayInline()` restriction.
  if (TryPatternSubstitution(invoke_instruction, method, accessor, return_replacement)) {
    LOG_SUCCESS() << "Successfully replaced pattern of invoke "
                  << method->PrettyMethod();
    MaybeRecordStat(stats_, MethodCompilationStat::kReplacedInvokeWithSimplePattern);
    return true;
  }

  // Check whether we're allowed to inline. The outermost compilation unit is the relevant
  // dex file here (though the transitivity of an inline chain would allow checking the caller).
  if (!MayInline(codegen_->GetCompilerOptions(),
                 *method->GetDexFile(),
                 *outer_compilation_unit_.GetDexFile())) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedWont)
        << "Won't inline " << method->PrettyMethod() << " in "
        << outer_compilation_unit_.GetDexFile()->GetLocation() << " ("
        << caller_compilation_unit_.GetDexFile()->GetLocation() << ") from "
        << method->GetDexFile()->GetLocation();
    return false;
  }

  if (!IsInliningSupported(invoke_instruction, method, accessor)) {
    return false;
  }

  if (!IsInliningEncouraged(invoke_instruction, method, accessor)) {
    return false;
  }

  if (!TryBuildAndInlineHelper(
          invoke_instruction, method, receiver_type, return_replacement, is_speculative)) {
    return false;
  }

  LOG_SUCCESS() << method->PrettyMethod();
  MaybeRecordStat(stats_, MethodCompilationStat::kInlinedInvoke);
  if (outermost_graph_ == graph_) {
    MaybeRecordStat(stats_, MethodCompilationStat::kInlinedLastInvoke);
  }
  return true;
}

static HInstruction* GetInvokeInputForArgVRegIndex(HInvoke* invoke_instruction,
                                                   size_t arg_vreg_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  size_t input_index = 0;
  for (size_t i = 0; i < arg_vreg_index; ++i, ++input_index) {
    DCHECK_LT(input_index, invoke_instruction->GetNumberOfArguments());
    if (DataType::Is64BitType(invoke_instruction->InputAt(input_index)->GetType())) {
      ++i;
      DCHECK_NE(i, arg_vreg_index);
    }
  }
  DCHECK_LT(input_index, invoke_instruction->GetNumberOfArguments());
  return invoke_instruction->InputAt(input_index);
}

// Try to recognize known simple patterns and replace invoke call with appropriate instructions.
bool HInliner::TryPatternSubstitution(HInvoke* invoke_instruction,
                                      ArtMethod* method,
                                      const CodeItemDataAccessor& accessor,
                                      HInstruction** return_replacement) {
  InlineMethod inline_method;
  if (!InlineMethodAnalyser::AnalyseMethodCode(method, &accessor, &inline_method)) {
    return false;
  }

  size_t number_of_instructions = 0u;  // Note: We do not count constants.
  switch (inline_method.opcode) {
    case kInlineOpNop:
      DCHECK_EQ(invoke_instruction->GetType(), DataType::Type::kVoid);
      *return_replacement = nullptr;
      break;
    case kInlineOpReturnArg:
      *return_replacement = GetInvokeInputForArgVRegIndex(invoke_instruction,
                                                          inline_method.d.return_data.arg);
      break;
    case kInlineOpNonWideConst: {
      char shorty0 = method->GetShorty()[0];
      if (shorty0 == 'L') {
        DCHECK_EQ(inline_method.d.data, 0u);
        *return_replacement = graph_->GetNullConstant();
      } else if (shorty0 == 'F') {
        *return_replacement = graph_->GetFloatConstant(
            bit_cast<float, int32_t>(static_cast<int32_t>(inline_method.d.data)));
      } else {
        *return_replacement = graph_->GetIntConstant(static_cast<int32_t>(inline_method.d.data));
      }
      break;
    }
    case kInlineOpIGet: {
      const InlineIGetIPutData& data = inline_method.d.ifield_data;
      if (data.method_is_static || data.object_arg != 0u) {
        // TODO: Needs null check.
        return false;
      }
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, data.object_arg);
      HInstanceFieldGet* iget = CreateInstanceFieldGet(data.field_idx, method, obj);
      DCHECK_EQ(iget->GetFieldOffset().Uint32Value(), data.field_offset);
      DCHECK_EQ(iget->IsVolatile() ? 1u : 0u, data.is_volatile);
      invoke_instruction->GetBlock()->InsertInstructionBefore(iget, invoke_instruction);
      *return_replacement = iget;
      number_of_instructions = 1u;
      break;
    }
    case kInlineOpIPut: {
      const InlineIGetIPutData& data = inline_method.d.ifield_data;
      if (data.method_is_static || data.object_arg != 0u) {
        // TODO: Needs null check.
        return false;
      }
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, data.object_arg);
      HInstruction* value = GetInvokeInputForArgVRegIndex(invoke_instruction, data.src_arg);
      HInstanceFieldSet* iput = CreateInstanceFieldSet(data.field_idx, method, obj, value);
      DCHECK_EQ(iput->GetFieldOffset().Uint32Value(), data.field_offset);
      DCHECK_EQ(iput->IsVolatile() ? 1u : 0u, data.is_volatile);
      invoke_instruction->GetBlock()->InsertInstructionBefore(iput, invoke_instruction);
      if (data.return_arg_plus1 != 0u) {
        size_t return_arg = data.return_arg_plus1 - 1u;
        *return_replacement = GetInvokeInputForArgVRegIndex(invoke_instruction, return_arg);
      }
      number_of_instructions = 1u;
      break;
    }
    case kInlineOpConstructor: {
      const InlineConstructorData& data = inline_method.d.constructor_data;
      // Get the indexes to arrays for easier processing.
      uint16_t iput_field_indexes[] = {
          data.iput0_field_index, data.iput1_field_index, data.iput2_field_index
      };
      uint16_t iput_args[] = { data.iput0_arg, data.iput1_arg, data.iput2_arg };
      static_assert(arraysize(iput_args) == arraysize(iput_field_indexes), "Size mismatch");
      // Count valid field indexes.
      for (size_t i = 0, end = data.iput_count; i < end; i++) {
        // Check that there are no duplicate valid field indexes.
        DCHECK_EQ(0, std::count(iput_field_indexes + i + 1,
                                iput_field_indexes + end,
                                iput_field_indexes[i]));
      }
      // Check that there are no valid field indexes in the rest of the array.
      DCHECK_EQ(0, std::count_if(iput_field_indexes + data.iput_count,
                                 iput_field_indexes + arraysize(iput_field_indexes),
                                 [](uint16_t index) { return index != DexFile::kDexNoIndex16; }));

      // Create HInstanceFieldSet for each IPUT that stores non-zero data.
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction,
                                                        /* arg_vreg_index= */ 0u);
      bool needs_constructor_barrier = false;
      for (size_t i = 0, end = data.iput_count; i != end; ++i) {
        HInstruction* value = GetInvokeInputForArgVRegIndex(invoke_instruction, iput_args[i]);
        if (!IsZeroBitPattern(value)) {
          uint16_t field_index = iput_field_indexes[i];
          bool is_final;
          HInstanceFieldSet* iput =
              CreateInstanceFieldSet(field_index, method, obj, value, &is_final);
          invoke_instruction->GetBlock()->InsertInstructionBefore(iput, invoke_instruction);

          // Check whether the field is final. If it is, we need to add a barrier.
          if (is_final) {
            needs_constructor_barrier = true;
          }
        }
      }
      if (needs_constructor_barrier) {
        // See DexCompilationUnit::RequiresConstructorBarrier for more details.
        DCHECK(obj != nullptr) << "only non-static methods can have a constructor fence";

        HConstructorFence* constructor_fence =
            new (graph_->GetAllocator()) HConstructorFence(obj, kNoDexPc, graph_->GetAllocator());
        invoke_instruction->GetBlock()->InsertInstructionBefore(constructor_fence,
                                                                invoke_instruction);
      }
      *return_replacement = nullptr;
      number_of_instructions = data.iput_count + (needs_constructor_barrier ? 1u : 0u);
      break;
    }
  }
  if (number_of_instructions != 0u) {
    total_number_of_instructions_ += number_of_instructions;
    UpdateInliningBudget();
  }
  return true;
}

HInstanceFieldGet* HInliner::CreateInstanceFieldGet(uint32_t field_index,
                                                    ArtMethod* referrer,
                                                    HInstruction* obj)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ArtField* resolved_field =
      class_linker->LookupResolvedField(field_index, referrer, /* is_static= */ false);
  DCHECK(resolved_field != nullptr);
  HInstanceFieldGet* iget = new (graph_->GetAllocator()) HInstanceFieldGet(
      obj,
      resolved_field,
      DataType::FromShorty(resolved_field->GetTypeDescriptor()[0]),
      resolved_field->GetOffset(),
      resolved_field->IsVolatile(),
      field_index,
      resolved_field->GetDeclaringClass()->GetDexClassDefIndex(),
      *referrer->GetDexFile(),
      // Read barrier generates a runtime call in slow path and we need a valid
      // dex pc for the associated stack map. 0 is bogus but valid. Bug: 26854537.
      /* dex_pc= */ 0);
  if (iget->GetType() == DataType::Type::kReference) {
    // Use the same dex_cache that we used for field lookup as the hint_dex_cache.
    Handle<mirror::DexCache> dex_cache =
        graph_->GetHandleCache()->NewHandle(referrer->GetDexCache());
    ReferenceTypePropagation rtp(graph_,
                                 dex_cache,
                                 /* is_first_run= */ false);
    rtp.Visit(iget);
  }
  return iget;
}

HInstanceFieldSet* HInliner::CreateInstanceFieldSet(uint32_t field_index,
                                                    ArtMethod* referrer,
                                                    HInstruction* obj,
                                                    HInstruction* value,
                                                    bool* is_final)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ArtField* resolved_field =
      class_linker->LookupResolvedField(field_index, referrer, /* is_static= */ false);
  DCHECK(resolved_field != nullptr);
  if (is_final != nullptr) {
    // This information is needed only for constructors.
    DCHECK(referrer->IsConstructor());
    *is_final = resolved_field->IsFinal();
  }
  HInstanceFieldSet* iput = new (graph_->GetAllocator()) HInstanceFieldSet(
      obj,
      value,
      resolved_field,
      DataType::FromShorty(resolved_field->GetTypeDescriptor()[0]),
      resolved_field->GetOffset(),
      resolved_field->IsVolatile(),
      field_index,
      resolved_field->GetDeclaringClass()->GetDexClassDefIndex(),
      *referrer->GetDexFile(),
      // Read barrier generates a runtime call in slow path and we need a valid
      // dex pc for the associated stack map. 0 is bogus but valid. Bug: 26854537.
      /* dex_pc= */ 0);
  return iput;
}

template <typename T>
static inline Handle<T> NewHandleIfDifferent(ObjPtr<T> object, Handle<T> hint, HGraph* graph)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return (object != hint.Get()) ? graph->GetHandleCache()->NewHandle(object) : hint;
}

static bool CanEncodeInlinedMethodInStackMap(const DexFile& outer_dex_file,
                                             ArtMethod* callee,
                                             const CodeGenerator* codegen,
                                             bool* out_needs_bss_check)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!Runtime::Current()->IsAotCompiler()) {
    // JIT can always encode methods in stack maps.
    return true;
  }

  const DexFile* dex_file = callee->GetDexFile();
  if (IsSameDexFile(outer_dex_file, *dex_file)) {
    return true;
  }

  // Inline across dexfiles if the callee's DexFile is:
  // 1) in the bootclasspath, or
  if (callee->GetDeclaringClass()->IsBootStrapClassLoaded()) {
    // In multi-image, each BCP DexFile has their own OatWriter. Since they don't cooperate with
    // each other, we request the BSS check for them.
    // TODO(solanes, 154012332): Add .bss support for BCP multi-image.
    *out_needs_bss_check = codegen->GetCompilerOptions().IsMultiImage();
    return true;
  }

  // 2) is a non-BCP dexfile with the OatFile we are compiling.
  if (codegen->GetCompilerOptions().WithinOatFile(dex_file)) {
    return true;
  }

  // TODO(solanes): Support more AOT cases for inlining:
  // - methods in class loader context's DexFiles
  return false;
}

  // Substitutes parameters in the callee graph with their values from the caller.
void HInliner::SubstituteArguments(HGraph* callee_graph,
                                   HInvoke* invoke_instruction,
                                   ReferenceTypeInfo receiver_type,
                                   const DexCompilationUnit& dex_compilation_unit) {
  ArtMethod* const resolved_method = callee_graph->GetArtMethod();
  size_t parameter_index = 0;
  bool run_rtp = false;
  for (HInstructionIterator instructions(callee_graph->GetEntryBlock()->GetInstructions());
       !instructions.Done();
       instructions.Advance()) {
    HInstruction* current = instructions.Current();
    if (current->IsParameterValue()) {
      HInstruction* argument = invoke_instruction->InputAt(parameter_index);
      if (argument->IsNullConstant()) {
        current->ReplaceWith(callee_graph->GetNullConstant());
      } else if (argument->IsIntConstant()) {
        current->ReplaceWith(callee_graph->GetIntConstant(argument->AsIntConstant()->GetValue()));
      } else if (argument->IsLongConstant()) {
        current->ReplaceWith(callee_graph->GetLongConstant(argument->AsLongConstant()->GetValue()));
      } else if (argument->IsFloatConstant()) {
        current->ReplaceWith(
            callee_graph->GetFloatConstant(argument->AsFloatConstant()->GetValue()));
      } else if (argument->IsDoubleConstant()) {
        current->ReplaceWith(
            callee_graph->GetDoubleConstant(argument->AsDoubleConstant()->GetValue()));
      } else if (argument->GetType() == DataType::Type::kReference) {
        if (!resolved_method->IsStatic() && parameter_index == 0 && receiver_type.IsValid()) {
          run_rtp = true;
          current->SetReferenceTypeInfo(receiver_type);
        } else {
          current->SetReferenceTypeInfoIfValid(argument->GetReferenceTypeInfo());
        }
        current->AsParameterValue()->SetCanBeNull(argument->CanBeNull());
      }
      ++parameter_index;
    }
  }

  // We have replaced formal arguments with actual arguments. If actual types
  // are more specific than the declared ones, run RTP again on the inner graph.
  if (run_rtp || ArgumentTypesMoreSpecific(invoke_instruction, resolved_method)) {
    ReferenceTypePropagation(callee_graph,
                             dex_compilation_unit.GetDexCache(),
                             /* is_first_run= */ false).Run();
  }
}

// Returns whether we can inline the callee_graph into the target_block.
//
// This performs a combination of semantics checks, compiler support checks, and
// resource limit checks.
//
// If this function returns true, it will also set out_number_of_instructions to
// the number of instructions in the inlined body.
bool HInliner::CanInlineBody(const HGraph* callee_graph,
                             HInvoke* invoke,
                             size_t* out_number_of_instructions,
                             bool is_speculative) const {
  ArtMethod* const resolved_method = callee_graph->GetArtMethod();

  HBasicBlock* exit_block = callee_graph->GetExitBlock();
  if (exit_block == nullptr) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedInfiniteLoop)
        << "Method " << resolved_method->PrettyMethod()
        << " could not be inlined because it has an infinite loop";
    return false;
  }

  bool has_one_return = false;
  bool has_try_catch = false;
  for (HBasicBlock* predecessor : exit_block->GetPredecessors()) {
    const HInstruction* last_instruction = predecessor->GetLastInstruction();
    // On inlinees, we can have Return/ReturnVoid/Throw -> TryBoundary -> Exit. To check for the
    // actual last instruction, we have to skip the TryBoundary instruction.
    if (last_instruction->IsTryBoundary()) {
      has_try_catch = true;
      predecessor = predecessor->GetSinglePredecessor();
      last_instruction = predecessor->GetLastInstruction();

      // If the last instruction chain is Return/ReturnVoid -> TryBoundary -> Exit we will have to
      // split a critical edge in InlineInto and might recompute loop information, which is
      // unsupported for irreducible loops.
      if (!last_instruction->IsThrow() && graph_->HasIrreducibleLoops()) {
        DCHECK(last_instruction->IsReturn() || last_instruction->IsReturnVoid());
        // TODO(ngeoffray): Support re-computing loop information to graphs with
        // irreducible loops?
        LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedIrreducibleLoopCaller)
            << "Method " << resolved_method->PrettyMethod()
            << " could not be inlined because we will have to recompute the loop information and"
            << " the caller has irreducible loops";
        return false;
      }
    }

    if (last_instruction->IsThrow()) {
      if (graph_->GetExitBlock() == nullptr) {
        // TODO(ngeoffray): Support adding HExit in the caller graph.
        LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedInfiniteLoop)
            << "Method " << resolved_method->PrettyMethod()
            << " could not be inlined because one branch always throws and"
            << " caller does not have an exit block";
        return false;
      } else if (graph_->HasIrreducibleLoops()) {
        // TODO(ngeoffray): Support re-computing loop information to graphs with
        // irreducible loops?
        LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedIrreducibleLoopCaller)
            << "Method " << resolved_method->PrettyMethod()
            << " could not be inlined because one branch always throws and"
            << " the caller has irreducible loops";
        return false;
      }
    } else {
      has_one_return = true;
    }
  }

  if (!has_one_return) {
    // If a method has a try catch, all throws are potentially caught. We are conservative and
    // don't assume a method always throws unless we can guarantee that.
    if (!is_speculative && !has_try_catch) {
      // If we know that the method always throws with the particular parameters, set it as such.
      // This is better than using the dex instructions as we have more information about this
      // particular call. We don't mark speculative inlines (e.g. the ones from the inline cache) as
      // always throwing since they might not throw when executed.
      invoke->SetAlwaysThrows(/* always_throws= */ true);
      graph_->SetHasAlwaysThrowingInvokes(/* value= */ true);
    }

    // Methods that contain infinite loops with try catches fall into this line too as we construct
    // an Exit block for them. This will mean that the stat `kNotInlinedAlwaysThrows` might not be
    // 100% correct but:
    // 1) This is a very small fraction of methods, and
    // 2) It is not easy to disambiguate between those.
    // Since we want to avoid inlining methods with infinite loops anyway, we return false for these
    // cases too.
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedAlwaysThrows)
        << "Method " << resolved_method->PrettyMethod()
        << " could not be inlined because it always throws";
    return false;
  }

  const bool too_many_registers =
      total_number_of_dex_registers_ > kMaximumNumberOfCumulatedDexRegisters;
  bool needs_bss_check = false;
  const bool can_encode_in_stack_map = CanEncodeInlinedMethodInStackMap(
      *outer_compilation_unit_.GetDexFile(), resolved_method, codegen_, &needs_bss_check);
  size_t number_of_instructions = 0;
  // Skip the entry block, it does not contain instructions that prevent inlining.
  for (HBasicBlock* block : callee_graph->GetReversePostOrderSkipEntryBlock()) {
    if (block->IsLoopHeader()) {
      if (block->GetLoopInformation()->IsIrreducible()) {
        // Don't inline methods with irreducible loops, they could prevent some
        // optimizations to run.
        LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedIrreducibleLoopCallee)
            << "Method " << resolved_method->PrettyMethod()
            << " could not be inlined because it contains an irreducible loop";
        return false;
      }
      if (!block->GetLoopInformation()->HasExitEdge()) {
        // Don't inline methods with loops without exit, since they cause the
        // loop information to be computed incorrectly when updating after
        // inlining.
        LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedLoopWithoutExit)
            << "Method " << resolved_method->PrettyMethod()
            << " could not be inlined because it contains a loop with no exit";
        return false;
      }
    }

    for (HInstructionIterator instr_it(block->GetInstructions());
         !instr_it.Done();
         instr_it.Advance()) {
      if (++number_of_instructions > inlining_budget_) {
        LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedInstructionBudget)
            << "Method " << resolved_method->PrettyMethod()
            << " is not inlined because the outer method has reached"
            << " its instruction budget limit.";
        return false;
      }
      HInstruction* current = instr_it.Current();
      if (current->NeedsEnvironment()) {
        if (too_many_registers) {
          LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedEnvironmentBudget)
              << "Method " << resolved_method->PrettyMethod()
              << " is not inlined because its caller has reached"
              << " its environment budget limit.";
          return false;
        }

        if (!can_encode_in_stack_map) {
          LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedStackMaps)
              << "Method " << resolved_method->PrettyMethod() << " could not be inlined because "
              << current->DebugName() << " needs an environment, is in a different dex file"
              << ", and cannot be encoded in the stack maps.";
          return false;
        }
      }

      if (current->IsUnresolvedStaticFieldGet() ||
          current->IsUnresolvedInstanceFieldGet() ||
          current->IsUnresolvedStaticFieldSet() ||
          current->IsUnresolvedInstanceFieldSet() ||
          current->IsInvokeUnresolved()) {
        // Unresolved invokes / field accesses are expensive at runtime when decoding inlining info,
        // so don't inline methods that have them.
        LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedUnresolvedEntrypoint)
            << "Method " << resolved_method->PrettyMethod()
            << " could not be inlined because it is using an unresolved"
            << " entrypoint";
        return false;
      }

      // We currently don't have support for inlining across dex files if we are:
      // 1) In AoT,
      // 2) cross-dex inlining,
      // 3) the callee is a BCP DexFile,
      // 4) we are compiling multi image, and
      // 5) have an instruction that needs a bss entry, which will always be
      // 5)b) an instruction that needs an environment.
      // 1) - 4) are encoded in `needs_bss_check` (see CanEncodeInlinedMethodInStackMap).
      if (needs_bss_check && current->NeedsBss()) {
        DCHECK(current->NeedsEnvironment());
        LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedBss)
            << "Method " << resolved_method->PrettyMethod()
            << " could not be inlined because it needs a BSS check";
        return false;
      }

      if (outermost_graph_->IsCompilingBaseline() &&
          (current->IsInvokeVirtual() || current->IsInvokeInterface()) &&
          ProfilingInfoBuilder::IsInlineCacheUseful(current->AsInvoke(), codegen_)) {
        uint32_t maximum_inlining_depth_for_baseline =
            InlineCache::MaxDexPcEncodingDepth(
                outermost_graph_->GetArtMethod(),
                codegen_->GetCompilerOptions().GetInlineMaxCodeUnits());
        if (depth_ + 1 > maximum_inlining_depth_for_baseline) {
          LOG_FAIL_NO_STAT() << "Reached maximum depth for inlining in baseline compilation: "
                             << depth_ << " for " << callee_graph->GetArtMethod()->PrettyMethod();
          outermost_graph_->SetUsefulOptimizing();
          return false;
        }
      }
    }
  }

  *out_number_of_instructions = number_of_instructions;
  return true;
}

bool HInliner::TryBuildAndInlineHelper(HInvoke* invoke_instruction,
                                       ArtMethod* resolved_method,
                                       ReferenceTypeInfo receiver_type,
                                       HInstruction** return_replacement,
                                       bool is_speculative) {
  DCHECK_IMPLIES(resolved_method->IsStatic(), !receiver_type.IsValid());
  DCHECK_IMPLIES(!resolved_method->IsStatic(), receiver_type.IsValid());
  const dex::CodeItem* code_item = resolved_method->GetCodeItem();
  const DexFile& callee_dex_file = *resolved_method->GetDexFile();
  uint32_t method_index = resolved_method->GetDexMethodIndex();
  CodeItemDebugInfoAccessor code_item_accessor(resolved_method->DexInstructionDebugInfo());
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  Handle<mirror::DexCache> dex_cache = NewHandleIfDifferent(resolved_method->GetDexCache(),
                                                            caller_compilation_unit_.GetDexCache(),
                                                            graph_);
  Handle<mirror::ClassLoader> class_loader =
      NewHandleIfDifferent(resolved_method->GetDeclaringClass()->GetClassLoader(),
                           caller_compilation_unit_.GetClassLoader(),
                           graph_);

  Handle<mirror::Class> compiling_class =
      graph_->GetHandleCache()->NewHandle(resolved_method->GetDeclaringClass());
  DexCompilationUnit dex_compilation_unit(
      class_loader,
      class_linker,
      callee_dex_file,
      code_item,
      resolved_method->GetDeclaringClass()->GetDexClassDefIndex(),
      method_index,
      resolved_method->GetAccessFlags(),
      /* verified_method= */ nullptr,
      dex_cache,
      compiling_class);

  InvokeType invoke_type = invoke_instruction->GetInvokeType();
  if (invoke_type == kInterface) {
    // We have statically resolved the dispatch. To please the class linker
    // at runtime, we change this call as if it was a virtual call.
    invoke_type = kVirtual;
  }

  bool caller_dead_reference_safe = graph_->IsDeadReferenceSafe();
  const dex::ClassDef& callee_class = resolved_method->GetClassDef();
  // MethodContainsRSensitiveAccess is currently slow, but HasDeadReferenceSafeAnnotation()
  // is currently rarely true.
  bool callee_dead_reference_safe =
      annotations::HasDeadReferenceSafeAnnotation(callee_dex_file, callee_class)
      && !annotations::MethodContainsRSensitiveAccess(callee_dex_file, callee_class, method_index);

  const int32_t caller_instruction_counter = graph_->GetCurrentInstructionId();
  HGraph* callee_graph = new (graph_->GetAllocator()) HGraph(
      graph_->GetAllocator(),
      graph_->GetArenaStack(),
      graph_->GetHandleCache()->GetHandles(),
      callee_dex_file,
      method_index,
      codegen_->GetCompilerOptions().GetInstructionSet(),
      invoke_type,
      callee_dead_reference_safe,
      graph_->IsDebuggable(),
      graph_->GetCompilationKind(),
      /* start_instruction_id= */ caller_instruction_counter);
  callee_graph->SetArtMethod(resolved_method);

  ScopedProfilingInfoUse spiu(Runtime::Current()->GetJit(), resolved_method, Thread::Current());
  if (Runtime::Current()->GetJit() != nullptr) {
    callee_graph->SetProfilingInfo(spiu.GetProfilingInfo());
  }

  // When they are needed, allocate `inline_stats_` on the Arena instead
  // of on the stack, as Clang might produce a stack frame too large
  // for this function, that would not fit the requirements of the
  // `-Wframe-larger-than` option.
  if (stats_ != nullptr) {
    // Reuse one object for all inline attempts from this caller to keep Arena memory usage low.
    if (inline_stats_ == nullptr) {
      void* storage = graph_->GetAllocator()->Alloc<OptimizingCompilerStats>(kArenaAllocMisc);
      inline_stats_ = new (storage) OptimizingCompilerStats;
    } else {
      inline_stats_->Reset();
    }
  }
  HGraphBuilder builder(callee_graph,
                        code_item_accessor,
                        &dex_compilation_unit,
                        &outer_compilation_unit_,
                        codegen_,
                        inline_stats_);

  if (builder.BuildGraph() != kAnalysisSuccess) {
    LOG_FAIL(stats_, MethodCompilationStat::kNotInlinedCannotBuild)
        << "Method " << callee_dex_file.PrettyMethod(method_index)
        << " could not be built, so cannot be inlined";
    return false;
  }

  SubstituteArguments(callee_graph, invoke_instruction, receiver_type, dex_compilation_unit);

  const bool try_catch_inlining_allowed_for_recursive_inline =
      // It was allowed previously.
      try_catch_inlining_allowed_ &&
      // The current invoke is not a try block.
      !invoke_instruction->GetBlock()->IsTryBlock();
  RunOptimizations(callee_graph,
                   invoke_instruction->GetEnvironment(),
                   code_item,
                   dex_compilation_unit,
                   try_catch_inlining_allowed_for_recursive_inline);

  size_t number_of_instructions = 0;
  if (!CanInlineBody(callee_graph, invoke_instruction, &number_of_instructions, is_speculative)) {
    return false;
  }

  DCHECK_EQ(caller_instruction_counter, graph_->GetCurrentInstructionId())
      << "No instructions can be added to the outer graph while inner graph is being built";

  // Inline the callee graph inside the caller graph.
  const int32_t callee_instruction_counter = callee_graph->GetCurrentInstructionId();
  graph_->SetCurrentInstructionId(callee_instruction_counter);
  *return_replacement = callee_graph->InlineInto(graph_, invoke_instruction);
  // Update our budget for other inlining attempts in `caller_graph`.
  total_number_of_instructions_ += number_of_instructions;
  UpdateInliningBudget();

  DCHECK_EQ(callee_instruction_counter, callee_graph->GetCurrentInstructionId())
      << "No instructions can be added to the inner graph during inlining into the outer graph";

  if (stats_ != nullptr) {
    DCHECK(inline_stats_ != nullptr);
    inline_stats_->AddTo(stats_);
  }

  if (caller_dead_reference_safe && !callee_dead_reference_safe) {
    // Caller was dead reference safe, but is not anymore, since we inlined dead
    // reference unsafe code. Prior transformations remain valid, since they did not
    // affect the inlined code.
    graph_->MarkDeadReferenceUnsafe();
  }

  return true;
}

void HInliner::RunOptimizations(HGraph* callee_graph,
                                HEnvironment* caller_environment,
                                const dex::CodeItem* code_item,
                                const DexCompilationUnit& dex_compilation_unit,
                                bool try_catch_inlining_allowed_for_recursive_inline) {
  // Note: if the outermost_graph_ is being compiled OSR, we should not run any
  // optimization that could lead to a HDeoptimize. The following optimizations do not.
  HDeadCodeElimination dce(callee_graph, inline_stats_, "dead_code_elimination$inliner");
  HConstantFolding fold(callee_graph, inline_stats_, "constant_folding$inliner");
  InstructionSimplifier simplify(callee_graph, codegen_, inline_stats_);

  HOptimization* optimizations[] = {
    &fold,
    &simplify,
    &dce,
  };

  for (size_t i = 0; i < arraysize(optimizations); ++i) {
    HOptimization* optimization = optimizations[i];
    optimization->Run();
  }

  // Bail early for pathological cases on the environment (for example recursive calls,
  // or too large environment).
  if (total_number_of_dex_registers_ > kMaximumNumberOfCumulatedDexRegisters) {
    LOG_NOTE() << "Calls in " << callee_graph->GetArtMethod()->PrettyMethod()
             << " will not be inlined because the outer method has reached"
             << " its environment budget limit.";
    return;
  }

  // Bail early if we know we already are over the limit.
  size_t number_of_instructions = CountNumberOfInstructions(callee_graph);
  if (number_of_instructions > inlining_budget_) {
    LOG_NOTE() << "Calls in " << callee_graph->GetArtMethod()->PrettyMethod()
             << " will not be inlined because the outer method has reached"
             << " its instruction budget limit. " << number_of_instructions;
    return;
  }

  CodeItemDataAccessor accessor(callee_graph->GetDexFile(), code_item);
  HInliner inliner(callee_graph,
                   outermost_graph_,
                   codegen_,
                   outer_compilation_unit_,
                   dex_compilation_unit,
                   inline_stats_,
                   total_number_of_dex_registers_ + accessor.RegistersSize(),
                   total_number_of_instructions_ + number_of_instructions,
                   this,
                   caller_environment,
                   depth_ + 1,
                   try_catch_inlining_allowed_for_recursive_inline);
  inliner.Run();
}

static bool IsReferenceTypeRefinement(ObjPtr<mirror::Class> declared_class,
                                      bool declared_is_exact,
                                      bool declared_can_be_null,
                                      HInstruction* actual_obj)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (declared_can_be_null && !actual_obj->CanBeNull()) {
    return true;
  }

  ReferenceTypeInfo actual_rti = actual_obj->GetReferenceTypeInfo();
  if (!actual_rti.IsValid()) {
    return false;
  }

  ObjPtr<mirror::Class> actual_class = actual_rti.GetTypeHandle().Get();
  return (actual_rti.IsExact() && !declared_is_exact) ||
         (declared_class != actual_class && declared_class->IsAssignableFrom(actual_class));
}

static bool IsReferenceTypeRefinement(ObjPtr<mirror::Class> declared_class,
                                      bool declared_can_be_null,
                                      HInstruction* actual_obj)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  bool admissible = ReferenceTypePropagation::IsAdmissible(declared_class);
  return IsReferenceTypeRefinement(
      admissible ? declared_class : GetClassRoot<mirror::Class>(),
      /*declared_is_exact=*/ admissible && declared_class->CannotBeAssignedFromOtherTypes(),
      declared_can_be_null,
      actual_obj);
}

bool HInliner::ArgumentTypesMoreSpecific(HInvoke* invoke_instruction, ArtMethod* resolved_method) {
  // If this is an instance call, test whether the type of the `this` argument
  // is more specific than the class which declares the method.
  if (!resolved_method->IsStatic()) {
    if (IsReferenceTypeRefinement(resolved_method->GetDeclaringClass(),
                                  /*declared_can_be_null=*/ false,
                                  invoke_instruction->InputAt(0u))) {
      return true;
    }
  }

  // Iterate over the list of parameter types and test whether any of the
  // actual inputs has a more specific reference type than the type declared in
  // the signature.
  const dex::TypeList* param_list = resolved_method->GetParameterTypeList();
  for (size_t param_idx = 0,
              input_idx = resolved_method->IsStatic() ? 0 : 1,
              e = (param_list == nullptr ? 0 : param_list->Size());
       param_idx < e;
       ++param_idx, ++input_idx) {
    HInstruction* input = invoke_instruction->InputAt(input_idx);
    if (input->GetType() == DataType::Type::kReference) {
      ObjPtr<mirror::Class> param_cls = resolved_method->LookupResolvedClassFromTypeIndex(
          param_list->GetTypeItem(param_idx).type_idx_);
      if (IsReferenceTypeRefinement(param_cls, /*declared_can_be_null=*/ true, input)) {
        return true;
      }
    }
  }

  return false;
}

bool HInliner::ReturnTypeMoreSpecific(HInstruction* return_replacement,
                                      HInvoke* invoke_instruction) {
  // Check the integrity of reference types and run another type propagation if needed.
  if (return_replacement != nullptr) {
    if (return_replacement->GetType() == DataType::Type::kReference) {
      // Test if the return type is a refinement of the declared return type.
      ReferenceTypeInfo invoke_rti = invoke_instruction->GetReferenceTypeInfo();
      if (IsReferenceTypeRefinement(invoke_rti.GetTypeHandle().Get(),
                                    invoke_rti.IsExact(),
                                    invoke_instruction->CanBeNull(),
                                    return_replacement)) {
        return true;
      } else if (return_replacement->IsInstanceFieldGet()) {
        HInstanceFieldGet* field_get = return_replacement->AsInstanceFieldGet();
        ArtField* cls_field = WellKnownClasses::java_lang_Object_shadowKlass;
        if (field_get->GetFieldInfo().GetField() == cls_field) {
          return true;
        }
      }
    } else if (return_replacement->IsInstanceOf()) {
      // Inlining InstanceOf into an If may put a tighter bound on reference types.
      return true;
    }
  }

  return false;
}

void HInliner::FixUpReturnReferenceType(ArtMethod* resolved_method,
                                        HInstruction* return_replacement) {
  if (return_replacement != nullptr) {
    if (return_replacement->GetType() == DataType::Type::kReference) {
      if (!return_replacement->GetReferenceTypeInfo().IsValid()) {
        // Make sure that we have a valid type for the return. We may get an invalid one when
        // we inline invokes with multiple branches and create a Phi for the result.
        // TODO: we could be more precise by merging the phi inputs but that requires
        // some functionality from the reference type propagation.
        DCHECK(return_replacement->IsPhi());
        ObjPtr<mirror::Class> cls = resolved_method->LookupResolvedReturnType();
        ReferenceTypeInfo rti = ReferenceTypePropagation::IsAdmissible(cls)
            ? ReferenceTypeInfo::Create(graph_->GetHandleCache()->NewHandle(cls))
            : graph_->GetInexactObjectRti();
        return_replacement->SetReferenceTypeInfo(rti);
      }
    }
  }
}

}  // namespace art

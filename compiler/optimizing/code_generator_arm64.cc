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

#include "code_generator_arm64.h"

#include "aarch64/assembler-aarch64.h"
#include "aarch64/registers-aarch64.h"
#include "arch/arm64/asm_support_arm64.h"
#include "arch/arm64/instruction_set_features_arm64.h"
#include "arch/arm64/jni_frame_arm64.h"
#include "art_method-inl.h"
#include "base/bit_utils.h"
#include "base/bit_utils_iterator.h"
#include "class_root-inl.h"
#include "class_table.h"
#include "code_generator_utils.h"
#include "com_android_art_flags.h"
#include "dex/dex_file_types.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "gc/accounting/card_table.h"
#include "gc/space/image_space.h"
#include "heap_poisoning.h"
#include "interpreter/mterp/nterp.h"
#include "intrinsics.h"
#include "intrinsics_arm64.h"
#include "intrinsics_list.h"
#include "intrinsics_utils.h"
#include "jit/profiling_info.h"
#include "linker/linker_patch.h"
#include "lock_word.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/var_handle.h"
#include "offsets.h"
#include "optimizing/common_arm64.h"
#include "optimizing/nodes.h"
#include "profiling_info_builder.h"
#include "thread.h"
#include "trace.h"
#include "utils/arm64/assembler_arm64.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"

using namespace vixl::aarch64;  // NOLINT(build/namespaces)
using vixl::ExactAssemblyScope;
using vixl::CodeBufferCheckScope;
using vixl::EmissionCheckScope;

namespace art_flags = com::android::art::flags;

#ifdef __
#error "ARM64 Codegen VIXL macro-assembler macro already defined."
#endif

namespace art HIDDEN {

template<class MirrorType>
class GcRoot;

namespace arm64 {

using helpers::ARM64EncodableConstantOrRegister;
using helpers::ArtVixlRegCodeCoherentForRegSet;
using helpers::CPURegisterFrom;
using helpers::DRegisterFrom;
using helpers::FPRegisterFrom;
using helpers::HeapOperand;
using helpers::HeapOperandFrom;
using helpers::InputCPURegisterOrZeroRegAt;
using helpers::InputFPRegisterAt;
using helpers::InputOperandAt;
using helpers::InputRegisterAt;
using helpers::Int64FromLocation;
using helpers::LocationFrom;
using helpers::OperandFromMemOperand;
using helpers::OutputCPURegister;
using helpers::OutputFPRegister;
using helpers::OutputRegister;
using helpers::RegisterFrom;
using helpers::StackOperandFrom;
using helpers::VIXLRegCodeFromART;
using helpers::WRegisterFrom;
using helpers::XRegisterFrom;

// TODO(mythria): Expand SystemRegister in vixl to include this value.
uint16_t SYS_CNTVCT_EL0 = SystemRegisterEncoder<1, 3, 14, 0, 2>::value;

// The compare/jump sequence will generate about (1.5 * num_entries + 3) instructions. While jump
// table version generates 7 instructions and num_entries literals. Compare/jump sequence will
// generates less code/data with a small num_entries.
static constexpr uint32_t kPackedSwitchCompareJumpThreshold = 7;

inline Condition ARM64Condition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return eq;
    case kCondNE: return ne;
    case kCondLT: return lt;
    case kCondLE: return le;
    case kCondGT: return gt;
    case kCondGE: return ge;
    case kCondB:  return lo;
    case kCondBE: return ls;
    case kCondA:  return hi;
    case kCondAE: return hs;
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

inline Condition ARM64FPCondition(IfCondition cond, bool gt_bias) {
  // The ARM64 condition codes can express all the necessary branches, see the
  // "Meaning (floating-point)" column in the table C1-1 in the ARMv8 reference manual.
  // There is no dex instruction or HIR that would need the missing conditions
  // "equal or unordered" or "not equal".
  switch (cond) {
    case kCondEQ: return eq;
    case kCondNE: return ne /* unordered */;
    case kCondLT: return gt_bias ? cc : lt /* unordered */;
    case kCondLE: return gt_bias ? ls : le /* unordered */;
    case kCondGT: return gt_bias ? hi /* unordered */ : gt;
    case kCondGE: return gt_bias ? cs /* unordered */ : ge;
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
}

Condition ARM64PCondition(HVecPredToBoolean::PCondKind cond) {
  switch (cond) {
    case HVecPredToBoolean::PCondKind::kFirst: return mi;
    case HVecPredToBoolean::PCondKind::kNFirst: return pl;
    default:
      LOG(FATAL) << "Unsupported condition type: " << enum_cast<uint32_t>(cond);
      UNREACHABLE();
  }
}

Location ARM64ReturnLocation(DataType::Type return_type) {
  // Note that in practice, `LocationFrom(x0)` and `LocationFrom(w0)` create the
  // same Location object, and so do `LocationFrom(d0)` and `LocationFrom(s0)`,
  // but we use the exact registers for clarity.
  if (return_type == DataType::Type::kFloat32) {
    return LocationFrom(s0);
  } else if (return_type == DataType::Type::kFloat64) {
    return LocationFrom(d0);
  } else if (return_type == DataType::Type::kInt64) {
    return LocationFrom(x0);
  } else if (return_type == DataType::Type::kVoid) {
    return Location::NoLocation();
  } else {
    return LocationFrom(w0);
  }
}

Location InvokeRuntimeCallingConvention::GetReturnLocation(DataType::Type return_type) {
  return ARM64ReturnLocation(return_type);
}

static RegisterSet OneRegInReferenceOutSaveEverythingCallerSaves() {
  InvokeRuntimeCallingConvention calling_convention;
  RegisterSet caller_saves = RegisterSet::Empty();
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(0).GetCode()));
  DCHECK_EQ(calling_convention.GetRegisterAt(0).GetCode(),
            RegisterFrom(calling_convention.GetReturnLocation(DataType::Type::kReference),
                         DataType::Type::kReference).GetCode());
  return caller_saves;
}

// NOLINT on __ macro to suppress wrong warning/fix (misc-macro-parentheses) from clang-tidy.
#define __ down_cast<CodeGeneratorARM64*>(codegen)->GetVIXLAssembler()->  // NOLINT
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kArm64PointerSize, x).Int32Value()

void SlowPathCodeARM64::SaveLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) {
  size_t stack_offset = codegen->GetFirstRegisterSlotInSlowPath();
  const uint32_t core_spills = codegen->GetSlowPathSpills(locations, /* core_registers= */ true);
  for (uint32_t i : LowToHighBits(core_spills)) {
    // If the register holds an object, update the stack mask.
    if (locations->RegisterContainsObject(i)) {
      locations->SetStackBit(stack_offset / kVRegSize);
    }
    DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
    DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
    saved_core_stack_offsets_[i] = stack_offset;
    stack_offset += kXRegSizeInBytes;
  }

  const size_t fp_reg_size = codegen->GetSlowPathFPWidth();
  const uint32_t fp_spills = codegen->GetSlowPathSpills(locations, /* core_registers= */ false);
  for (uint32_t i : LowToHighBits(fp_spills)) {
    DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
    DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
    saved_fpu_stack_offsets_[i] = stack_offset;
    stack_offset += fp_reg_size;
  }

  InstructionCodeGeneratorARM64* visitor =
      down_cast<CodeGeneratorARM64*>(codegen)->GetInstructionCodeGeneratorArm64();
  visitor->SaveLiveRegistersHelper(locations, codegen->GetFirstRegisterSlotInSlowPath());
}

void SlowPathCodeARM64::RestoreLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) {
  InstructionCodeGeneratorARM64* visitor =
      down_cast<CodeGeneratorARM64*>(codegen)->GetInstructionCodeGeneratorArm64();
  visitor->RestoreLiveRegistersHelper(locations, codegen->GetFirstRegisterSlotInSlowPath());
}

class BoundsCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit BoundsCheckSlowPathARM64(HBoundsCheck* instruction) : SlowPathCodeARM64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(locations->InAt(0),
                               LocationFrom(calling_convention.GetRegisterAt(0)),
                               DataType::Type::kInt32,
                               locations->InAt(1),
                               LocationFrom(calling_convention.GetRegisterAt(1)),
                               DataType::Type::kInt32);
    QuickEntrypointEnum entrypoint = instruction_->AsBoundsCheck()->IsStringCharAt()
        ? kQuickThrowStringBounds
        : kQuickThrowArrayBounds;
    arm64_codegen->InvokeRuntime(entrypoint, instruction_, this);
    CheckEntrypointTypes<kQuickThrowStringBounds, void, int32_t, int32_t>();
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "BoundsCheckSlowPathARM64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM64);
};

class DivZeroCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit DivZeroCheckSlowPathARM64(HDivZeroCheck* instruction) : SlowPathCodeARM64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    arm64_codegen->InvokeRuntime(kQuickThrowDivZero, instruction_, this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "DivZeroCheckSlowPathARM64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathARM64);
};

class LoadMethodTypeSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit LoadMethodTypeSlowPathARM64(HLoadMethodType* mt) : SlowPathCodeARM64(mt) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    Location out = locations->Out();
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    const dex::ProtoIndex proto_index = instruction_->AsLoadMethodType()->GetProtoIndex();
    __ Mov(calling_convention.GetRegisterAt(0).W(), proto_index.index_);

    arm64_codegen->InvokeRuntime(kQuickResolveMethodType, instruction_, this);
    CheckEntrypointTypes<kQuickResolveMethodType, void*, uint32_t>();

    DataType::Type type = instruction_->GetType();
    arm64_codegen->MoveLocation(out, calling_convention.GetReturnLocation(type), type);
    RestoreLiveRegisters(codegen, locations);

    __ B(GetExitLabel());
  }

  const char* GetDescription() const override { return "LoadMethodTypeSlowPathARM64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadMethodTypeSlowPathARM64);
};


class LoadClassSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  LoadClassSlowPathARM64(HLoadClass* cls, HInstruction* at)
      : SlowPathCodeARM64(at), cls_(cls) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
    DCHECK_EQ(instruction_->IsLoadClass(), cls_ == instruction_);
  }

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    Location out = locations->Out();
    bool must_resolve_type = instruction_->IsLoadClass() && cls_->MustResolveTypeOnSlowPath();
    bool must_do_clinit = instruction_->IsClinitCheck() || cls_->MustGenerateClinitCheck();

    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    if (must_resolve_type) {
      DCHECK(IsSameDexFile(cls_->GetDexFile(), arm64_codegen->GetGraph()->GetDexFile()) ||
             arm64_codegen->GetCompilerOptions().WithinOatFile(&cls_->GetDexFile()) ||
             ContainsElement(Runtime::Current()->GetClassLinker()->GetBootClassPath(),
                             &cls_->GetDexFile()));
      dex::TypeIndex type_index = cls_->GetTypeIndex();
      __ Mov(calling_convention.GetRegisterAt(0).W(), type_index.index_);
      if (cls_->NeedsAccessCheck()) {
        CheckEntrypointTypes<kQuickResolveTypeAndVerifyAccess, void*, uint32_t>();
        arm64_codegen->InvokeRuntime(kQuickResolveTypeAndVerifyAccess, instruction_, this);
      } else {
        CheckEntrypointTypes<kQuickResolveType, void*, uint32_t>();
        arm64_codegen->InvokeRuntime(kQuickResolveType, instruction_, this);
      }
      // If we also must_do_clinit, the resolved type is now in the correct register.
    } else {
      DCHECK(must_do_clinit);
      Location source = instruction_->IsLoadClass() ? out : locations->InAt(0);
      arm64_codegen->MoveLocation(LocationFrom(calling_convention.GetRegisterAt(0)),
                                  source,
                                  cls_->GetType());
    }
    if (must_do_clinit) {
      arm64_codegen->InvokeRuntime(kQuickInitializeStaticStorage, instruction_, this);
      CheckEntrypointTypes<kQuickInitializeStaticStorage, void*, mirror::Class*>();
    }

    // Move the class to the desired location.
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      DataType::Type type = instruction_->GetType();
      arm64_codegen->MoveLocation(out, calling_convention.GetReturnLocation(type), type);
    }
    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const override { return "LoadClassSlowPathARM64"; }

 private:
  // The class this slow path will load.
  HLoadClass* const cls_;

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathARM64);
};

class LoadStringSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit LoadStringSlowPathARM64(HLoadString* instruction)
      : SlowPathCodeARM64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    const dex::StringIndex string_index = instruction_->AsLoadString()->GetStringIndex();
    __ Mov(calling_convention.GetRegisterAt(0).W(), string_index.index_);
    arm64_codegen->InvokeRuntime(kQuickResolveString, instruction_, this);
    CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
    DataType::Type type = instruction_->GetType();
    arm64_codegen->MoveLocation(locations->Out(), calling_convention.GetReturnLocation(type), type);

    RestoreLiveRegisters(codegen, locations);

    __ B(GetExitLabel());
  }

  const char* GetDescription() const override { return "LoadStringSlowPathARM64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathARM64);
};

class NullCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit NullCheckSlowPathARM64(HNullCheck* instr) : SlowPathCodeARM64(instr) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    arm64_codegen->InvokeRuntime(kQuickThrowNullPointer, instruction_, this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "NullCheckSlowPathARM64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM64);
};

class SuspendCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  SuspendCheckSlowPathARM64(HSuspendCheck* instruction, HBasicBlock* successor)
      : SlowPathCodeARM64(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);  // Only saves live vector regs for SIMD.
    arm64_codegen->InvokeRuntime(kQuickTestSuspend, instruction_, this);
    CheckEntrypointTypes<kQuickTestSuspend, void, void>();
    RestoreLiveRegisters(codegen, locations);  // Only restores live vector regs for SIMD.
    if (successor_ == nullptr) {
      __ B(GetReturnLabel());
    } else {
      __ B(arm64_codegen->GetLabelOf(successor_));
    }
  }

  vixl::aarch64::Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

  HBasicBlock* GetSuccessor() const {
    return successor_;
  }

  const char* GetDescription() const override { return "SuspendCheckSlowPathARM64"; }

 private:
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  vixl::aarch64::Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathARM64);
};

class TypeCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  TypeCheckSlowPathARM64(HInstruction* instruction, bool is_fatal)
      : SlowPathCodeARM64(instruction), is_fatal_(is_fatal) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();

    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());

    if (!is_fatal_ || instruction_->CanThrowIntoCatchBlock()) {
      SaveLiveRegisters(codegen, locations);
    }

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(locations->InAt(0),
                               LocationFrom(calling_convention.GetRegisterAt(0)),
                               DataType::Type::kReference,
                               locations->InAt(1),
                               LocationFrom(calling_convention.GetRegisterAt(1)),
                               DataType::Type::kReference);
    if (instruction_->IsInstanceOf()) {
      arm64_codegen->InvokeRuntime(kQuickInstanceofNonTrivial, instruction_, this);
      CheckEntrypointTypes<kQuickInstanceofNonTrivial, size_t, mirror::Object*, mirror::Class*>();
      DataType::Type ret_type = instruction_->GetType();
      Location ret_loc = calling_convention.GetReturnLocation(ret_type);
      arm64_codegen->MoveLocation(locations->Out(), ret_loc, ret_type);
    } else {
      DCHECK(instruction_->IsCheckCast());
      arm64_codegen->InvokeRuntime(kQuickCheckInstanceOf, instruction_, this);
      CheckEntrypointTypes<kQuickCheckInstanceOf, void, mirror::Object*, mirror::Class*>();
    }

    if (!is_fatal_) {
      RestoreLiveRegisters(codegen, locations);
      __ B(GetExitLabel());
    }
  }

  const char* GetDescription() const override { return "TypeCheckSlowPathARM64"; }
  bool IsFatal() const override { return is_fatal_; }

 private:
  const bool is_fatal_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathARM64);
};

class DeoptimizationSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit DeoptimizationSlowPathARM64(HDeoptimize* instruction)
      : SlowPathCodeARM64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    LocationSummary* locations = instruction_->GetLocations();
    SaveLiveRegisters(codegen, locations);
    InvokeRuntimeCallingConvention calling_convention;
    __ Mov(calling_convention.GetRegisterAt(0),
           static_cast<uint32_t>(instruction_->AsDeoptimize()->GetDeoptimizationKind()));
    arm64_codegen->InvokeRuntime(kQuickDeoptimize, instruction_, this);
    CheckEntrypointTypes<kQuickDeoptimize, void, DeoptimizationKind>();
  }

  const char* GetDescription() const override { return "DeoptimizationSlowPathARM64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathARM64);
};

class ArraySetSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit ArraySetSlowPathARM64(HInstruction* instruction) : SlowPathCodeARM64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    HParallelMove parallel_move(codegen->GetGraph()->GetAllocator());
    parallel_move.AddMove(
        locations->InAt(0),
        LocationFrom(calling_convention.GetRegisterAt(0)),
        DataType::Type::kReference,
        nullptr);
    parallel_move.AddMove(
        locations->InAt(1),
        LocationFrom(calling_convention.GetRegisterAt(1)),
        DataType::Type::kInt32,
        nullptr);
    parallel_move.AddMove(
        locations->InAt(2),
        LocationFrom(calling_convention.GetRegisterAt(2)),
        DataType::Type::kReference,
        nullptr);
    codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);

    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    arm64_codegen->InvokeRuntime(kQuickAputObject, instruction_, this);
    CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const override { return "ArraySetSlowPathARM64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(ArraySetSlowPathARM64);
};

void JumpTableARM64::EmitTable(CodeGeneratorARM64* codegen) {
  uint32_t num_entries = switch_instr_->GetNumEntries();
  DCHECK_GE(num_entries, kPackedSwitchCompareJumpThreshold);

  // We are about to use the assembler to place literals directly. Make sure we have enough
  // underlying code buffer and we have generated the jump table with right size.
  ExactAssemblyScope scope(codegen->GetVIXLAssembler(),
                           num_entries * sizeof(int32_t),
                           CodeBufferCheckScope::kExactSize);
  codegen->GetVIXLAssembler()->bind(&table_start_);
  for (uint32_t i = 0; i < num_entries; i++) {
    codegen->GetVIXLAssembler()->place(jump_targets_[i].get());
  }
}

void JumpTableARM64::FixTable(CodeGeneratorARM64* codegen) {
  uint32_t num_entries = switch_instr_->GetNumEntries();
  DCHECK_GE(num_entries, kPackedSwitchCompareJumpThreshold);

  const ArenaVector<HBasicBlock*>& successors = switch_instr_->GetBlock()->GetSuccessors();
  for (uint32_t i = 0; i < num_entries; i++) {
    vixl::aarch64::Label* target_label = codegen->GetLabelOf(successors[i]);
    DCHECK(target_label->IsBound());
    ptrdiff_t jump_offset = target_label->GetLocation() - table_start_.GetLocation();
    DCHECK_GT(jump_offset, std::numeric_limits<int32_t>::min());
    DCHECK_LE(jump_offset, std::numeric_limits<int32_t>::max());
    jump_targets_[i].get()->UpdateValue(jump_offset, codegen->GetVIXLAssembler());
  }
}

// Slow path generating a read barrier for a heap reference.
class ReadBarrierForHeapReferenceSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  ReadBarrierForHeapReferenceSlowPathARM64(HInstruction* instruction,
                                           Location out,
                                           Location ref,
                                           Location obj,
                                           uint32_t offset,
                                           Location index)
      : SlowPathCodeARM64(instruction),
        out_(out),
        ref_(ref),
        obj_(obj),
        offset_(offset),
        index_(index) {
    // If `obj` is equal to `out` or `ref`, it means the initial object
    // has been overwritten by (or after) the heap object reference load
    // to be instrumented, e.g.:
    //
    //   __ Ldr(out, HeapOperand(out, class_offset);
    //   codegen_->GenerateReadBarrierSlow(instruction, out_loc, out_loc, out_loc, offset);
    //
    // In that case, we have lost the information about the original
    // object, and the emitted read barrier cannot work properly.
    DCHECK(!obj.Equals(out)) << "obj=" << obj << " out=" << out;
    DCHECK(!obj.Equals(ref)) << "obj=" << obj << " ref=" << ref;
  }

  void EmitNativeCode(CodeGenerator* codegen) override {
    DCHECK(codegen->EmitReadBarrier());
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    LocationSummary* locations = instruction_->GetLocations();
    DataType::Type type = DataType::Type::kReference;
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(out_.reg()));
    DCHECK(instruction_->IsInstanceFieldGet() ||
           instruction_->IsStaticFieldGet() ||
           instruction_->IsArrayGet() ||
           instruction_->IsInstanceOf() ||
           instruction_->IsCheckCast() ||
           (instruction_->IsInvoke() && instruction_->GetLocations()->Intrinsified()))
        << "Unexpected instruction in read barrier for heap reference slow path: "
        << instruction_->DebugName();
    // The read barrier instrumentation of object ArrayGet
    // instructions does not support the HIntermediateAddress
    // instruction.
    DCHECK(!(instruction_->IsArrayGet() &&
             instruction_->AsArrayGet()->GetArray()->IsIntermediateAddress()));

    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, locations);

    // We may have to change the index's value, but as `index_` is a
    // constant member (like other "inputs" of this slow path),
    // introduce a copy of it, `index`.
    Location index = index_;
    if (index_.IsValid()) {
      // Handle `index_` for HArrayGet and UnsafeGetObject/UnsafeGetObjectVolatile intrinsics.
      if (instruction_->IsArrayGet()) {
        // Compute the actual memory offset and store it in `index`.
        Register index_reg = RegisterFrom(index_, DataType::Type::kInt32);
        DCHECK(locations->GetLiveRegisters()->ContainsCoreRegister(index_.reg()));
        if (codegen->IsCoreCalleeSaveRegister(index_.reg())) {
          // We are about to change the value of `index_reg` (see the
          // calls to vixl::MacroAssembler::Lsl and
          // vixl::MacroAssembler::Mov below), but it has
          // not been saved by the previous call to
          // art::SlowPathCode::SaveLiveRegisters, as it is a
          // callee-save register --
          // art::SlowPathCode::SaveLiveRegisters does not consider
          // callee-save registers, as it has been designed with the
          // assumption that callee-save registers are supposed to be
          // handled by the called function.  So, as a callee-save
          // register, `index_reg` _would_ eventually be saved onto
          // the stack, but it would be too late: we would have
          // changed its value earlier.  Therefore, we manually save
          // it here into another freely available register,
          // `free_reg`, chosen of course among the caller-save
          // registers (as a callee-save `free_reg` register would
          // exhibit the same problem).
          //
          // Note we could have requested a temporary register from
          // the register allocator instead; but we prefer not to, as
          // this is a slow path, and we know we can find a
          // caller-save register that is available.
          Register free_reg = FindAvailableCallerSaveRegister(codegen);
          __ Mov(free_reg.W(), index_reg);
          index_reg = free_reg;
          index = LocationFrom(index_reg);
        } else {
          // The initial register stored in `index_` has already been
          // saved in the call to art::SlowPathCode::SaveLiveRegisters
          // (as it is not a callee-save register), so we can freely
          // use it.
        }
        // Shifting the index value contained in `index_reg` by the scale
        // factor (2) cannot overflow in practice, as the runtime is
        // unable to allocate object arrays with a size larger than
        // 2^26 - 1 (that is, 2^28 - 4 bytes).
        __ Lsl(index_reg, index_reg, DataType::SizeShift(type));
        static_assert(
            sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
            "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
        __ Add(index_reg, index_reg, Operand(offset_));
      } else {
        // In the case of the following intrinsics `index_` is not shifted by a scale factor of 2
        // (as in the case of ArrayGet), as it is actually an offset to an object field within an
        // object.
        DCHECK(instruction_->IsInvoke()) << instruction_->DebugName();
        DCHECK(instruction_->GetLocations()->Intrinsified());
        HInvoke* invoke = instruction_->AsInvoke();
        DCHECK(IsUnsafeGetReference(invoke) ||
               IsVarHandleGet(invoke) ||
               IsUnsafeCASReference(invoke) ||
               IsVarHandleCASFamily(invoke)) << invoke->GetIntrinsic();
        DCHECK_EQ(offset_, 0u);
        DCHECK(index_.IsRegister());
      }
    }

    // We're moving two or three locations to locations that could
    // overlap, so we need a parallel move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    HParallelMove parallel_move(codegen->GetGraph()->GetAllocator());
    parallel_move.AddMove(ref_,
                          LocationFrom(calling_convention.GetRegisterAt(0)),
                          type,
                          nullptr);
    parallel_move.AddMove(obj_,
                          LocationFrom(calling_convention.GetRegisterAt(1)),
                          type,
                          nullptr);
    if (index.IsValid()) {
      parallel_move.AddMove(index,
                            LocationFrom(calling_convention.GetRegisterAt(2)),
                            DataType::Type::kInt32,
                            nullptr);
      codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
    } else {
      codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
      arm64_codegen->MoveConstant(LocationFrom(calling_convention.GetRegisterAt(2)), offset_);
    }
    arm64_codegen->InvokeRuntime(kQuickReadBarrierSlow, instruction_, this);
    CheckEntrypointTypes<
        kQuickReadBarrierSlow, mirror::Object*, mirror::Object*, mirror::Object*, uint32_t>();
    arm64_codegen->MoveLocation(out_, calling_convention.GetReturnLocation(type), type);

    RestoreLiveRegisters(codegen, locations);

    __ B(GetExitLabel());
  }

  const char* GetDescription() const override { return "ReadBarrierForHeapReferenceSlowPathARM64"; }

 private:
  Register FindAvailableCallerSaveRegister(CodeGenerator* codegen) {
    size_t ref = static_cast<int>(XRegisterFrom(ref_).GetCode());
    size_t obj = static_cast<int>(XRegisterFrom(obj_).GetCode());
    for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
      if (i != ref && i != obj && !codegen->IsCoreCalleeSaveRegister(i)) {
        return Register(VIXLRegCodeFromART(i), kXRegSize);
      }
    }
    // We shall never fail to find a free caller-save register, as
    // there are more than two core caller-save registers on ARM64
    // (meaning it is possible to find one which is different from
    // `ref` and `obj`).
    DCHECK_GT(codegen->GetNumberOfCoreCallerSaveRegisters(), 2u);
    LOG(FATAL) << "Could not find a free register";
    UNREACHABLE();
  }

  const Location out_;
  const Location ref_;
  const Location obj_;
  const uint32_t offset_;
  // An additional location containing an index to an array.
  // Only used for HArrayGet and the UnsafeGetObject &
  // UnsafeGetObjectVolatile intrinsics.
  const Location index_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForHeapReferenceSlowPathARM64);
};

// Slow path generating a read barrier for a GC root.
class ReadBarrierForRootSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  ReadBarrierForRootSlowPathARM64(HInstruction* instruction, Location out, Location root)
      : SlowPathCodeARM64(instruction), out_(out), root_(root) {
  }

  void EmitNativeCode(CodeGenerator* codegen) override {
    DCHECK(codegen->EmitReadBarrier());
    LocationSummary* locations = instruction_->GetLocations();
    DataType::Type type = DataType::Type::kReference;
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(out_.reg()));
    DCHECK(instruction_->IsLoadClass() ||
           instruction_->IsLoadString() ||
           (instruction_->IsInvoke() && instruction_->GetLocations()->Intrinsified()))
        << "Unexpected instruction in read barrier for GC root slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    // The argument of the ReadBarrierForRootSlow is not a managed
    // reference (`mirror::Object*`), but a `GcRoot<mirror::Object>*`;
    // thus we need a 64-bit move here, and we cannot use
    //
    //   arm64_codegen->MoveLocation(
    //       LocationFrom(calling_convention.GetRegisterAt(0)),
    //       root_,
    //       type);
    //
    // which would emit a 32-bit move, as `type` is a (32-bit wide)
    // reference type (`DataType::Type::kReference`).
    __ Mov(calling_convention.GetRegisterAt(0), XRegisterFrom(out_));
    arm64_codegen->InvokeRuntime(kQuickReadBarrierForRootSlow, instruction_, this);
    CheckEntrypointTypes<kQuickReadBarrierForRootSlow, mirror::Object*, GcRoot<mirror::Object>*>();
    arm64_codegen->MoveLocation(out_, calling_convention.GetReturnLocation(type), type);

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const override { return "ReadBarrierForRootSlowPathARM64"; }

 private:
  const Location out_;
  const Location root_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForRootSlowPathARM64);
};

class TracingMethodEntryExitHooksSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit TracingMethodEntryExitHooksSlowPathARM64(bool is_method_entry)
      : SlowPathCodeARM64(/* instruction= */ nullptr), is_method_entry_(is_method_entry) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    QuickEntrypointEnum entry_point =
        (is_method_entry_) ? kQuickRecordEntryTraceEvent : kQuickRecordExitTraceEvent;
    vixl::aarch64::Label call;
    __ Bind(GetEntryLabel());
    uint32_t entrypoint_offset = GetThreadOffset<kArm64PointerSize>(entry_point).Int32Value();
    __ Ldr(lr, MemOperand(tr, entrypoint_offset));
    __ Blr(lr);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const override {
    return "TracingMethodEntryExitHooksSlowPath";
  }

 private:
  const bool is_method_entry_;

  DISALLOW_COPY_AND_ASSIGN(TracingMethodEntryExitHooksSlowPathARM64);
};

class MethodEntryExitHooksSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit MethodEntryExitHooksSlowPathARM64(HInstruction* instruction)
      : SlowPathCodeARM64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    QuickEntrypointEnum entry_point =
        (instruction_->IsMethodEntryHook()) ? kQuickMethodEntryHook : kQuickMethodExitHook;
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);
    if (instruction_->IsMethodExitHook()) {
      __ Mov(vixl::aarch64::x4, arm64_codegen->GetFrameSize());
    }
    arm64_codegen->InvokeRuntime(entry_point, instruction_, this);
    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const override {
    return "MethodEntryExitHooksSlowPath";
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(MethodEntryExitHooksSlowPathARM64);
};

class CompileOptimizedSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  CompileOptimizedSlowPathARM64(HSuspendCheck* check, Register profiling_info)
      : SlowPathCodeARM64(check),
        profiling_info_(profiling_info) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    uint32_t entrypoint_offset =
        GetThreadOffset<kArm64PointerSize>(kQuickCompileOptimized).Int32Value();
    __ Bind(GetEntryLabel());
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    UseScratchRegisterScope temps(arm64_codegen->GetVIXLAssembler());
    Register counter = temps.AcquireW();
    __ Mov(counter, ProfilingInfo::GetOptimizeThreshold());
    __ Strh(counter,
            MemOperand(profiling_info_, ProfilingInfo::BaselineHotnessCountOffset().Int32Value()));
    if (instruction_ != nullptr) {
      // Only saves live vector regs for SIMD.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    __ Ldr(lr, MemOperand(tr, entrypoint_offset));
    // Note: we don't record the call here (and therefore don't generate a stack
    // map), as the entrypoint should never be suspended.
    __ Blr(lr);
    if (instruction_ != nullptr) {
      // Only restores live vector regs for SIMD.
      RestoreLiveRegisters(codegen, instruction_->GetLocations());
    }
    __ B(GetExitLabel());
  }

  const char* GetDescription() const override {
    return "CompileOptimizedSlowPath";
  }

 private:
  // The register where the profiling info is stored when entering the slow
  // path.
  Register profiling_info_;

  DISALLOW_COPY_AND_ASSIGN(CompileOptimizedSlowPathARM64);
};

#undef __

Location InvokeDexCallingConventionVisitorARM64::GetNextLocation(DataType::Type type) {
  Location next_location;
  if (type == DataType::Type::kVoid) {
    LOG(FATAL) << "Unreachable type " << type;
  }

  if (DataType::IsFloatingPointType(type) &&
      (float_index_ < calling_convention.GetNumberOfFpuRegisters())) {
    next_location = LocationFrom(calling_convention.GetFpuRegisterAt(float_index_++));
  } else if (!DataType::IsFloatingPointType(type) &&
             (gp_index_ < calling_convention.GetNumberOfRegisters())) {
    next_location = LocationFrom(calling_convention.GetRegisterAt(gp_index_++));
  } else {
    size_t stack_offset = calling_convention.GetStackOffsetOf(stack_index_);
    next_location = DataType::Is64BitType(type) ? Location::DoubleStackSlot(stack_offset)
                                                : Location::StackSlot(stack_offset);
  }

  // Space on the stack is reserved for all arguments.
  stack_index_ += DataType::Is64BitType(type) ? 2 : 1;
  return next_location;
}

Location InvokeDexCallingConventionVisitorARM64::GetMethodLocation() const {
  return LocationFrom(kArtMethodRegister);
}

Location CriticalNativeCallingConventionVisitorARM64::GetNextLocation(DataType::Type type) {
  DCHECK_NE(type, DataType::Type::kReference);

  Location location = Location::NoLocation();
  if (DataType::IsFloatingPointType(type)) {
    if (fpr_index_ < kParameterFPRegistersLength) {
      location = LocationFrom(kParameterFPRegisters[fpr_index_]);
      ++fpr_index_;
    }
  } else {
    // Native ABI uses the same registers as managed, except that the method register x0
    // is a normal argument.
    if (gpr_index_ < 1u + kParameterCoreRegistersLength) {
      location = LocationFrom(gpr_index_ == 0u ? x0 : kParameterCoreRegisters[gpr_index_ - 1u]);
      ++gpr_index_;
    }
  }
  if (location.IsInvalid()) {
    if (DataType::Is64BitType(type)) {
      location = Location::DoubleStackSlot(stack_offset_);
    } else {
      location = Location::StackSlot(stack_offset_);
    }
    stack_offset_ += kFramePointerSize;

    if (for_register_allocation_) {
      location = Location::Any();
    }
  }
  return location;
}

Location CriticalNativeCallingConventionVisitorARM64::GetReturnLocation(DataType::Type type) const {
  // We perform conversion to the managed ABI return register after the call if needed.
  InvokeDexCallingConventionVisitorARM64 dex_calling_convention;
  return dex_calling_convention.GetReturnLocation(type);
}

Location CriticalNativeCallingConventionVisitorARM64::GetMethodLocation() const {
  // Pass the method in the hidden argument x15.
  return Location::RegisterLocation(x15.GetCode());
}

namespace detail {

// Mark which intrinsics we don't have handcrafted code for.
template <Intrinsics T>
struct IsUnimplemented {
  bool is_unimplemented = false;
};

#define TRUE_OVERRIDE(Name)                     \
  template <>                                   \
  struct IsUnimplemented<Intrinsics::k##Name> { \
    bool is_unimplemented = true;               \
  };
UNIMPLEMENTED_INTRINSIC_LIST_ARM64(TRUE_OVERRIDE)
#undef TRUE_OVERRIDE

static constexpr bool kIsIntrinsicUnimplemented[] = {
    false,  // kNone
#define IS_UNIMPLEMENTED(Intrinsic, ...) \
    IsUnimplemented<Intrinsics::k##Intrinsic>().is_unimplemented,
    ART_INTRINSICS_LIST(IS_UNIMPLEMENTED)
#undef IS_UNIMPLEMENTED
};

}  // namespace detail

CodeGeneratorARM64::CodeGeneratorARM64(HGraph* graph,
                                       const CompilerOptions& compiler_options,
                                       OptimizingCompilerStats* stats)
    : CodeGenerator(graph,
                    kNumberOfAllocatableRegisters,
                    kNumberOfAllocatableFPRegisters,
                    kNumberOfAllocatableRegisterPairs,
                    callee_saved_core_registers.GetList(),
                    callee_saved_fp_registers.GetList(),
                    compiler_options,
                    stats,
                    ArrayRef<const bool>(detail::kIsIntrinsicUnimplemented)),
      block_labels_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      jump_tables_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      location_builder_neon_(graph, this),
      instruction_visitor_neon_(graph, this),
      location_builder_sve_(graph, this),
      instruction_visitor_sve_(graph, this),
      move_resolver_(graph->GetAllocator(), this),
      assembler_(graph->GetAllocator(),
                 compiler_options.GetInstructionSetFeatures()->AsArm64InstructionSetFeatures()),
      boot_image_method_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      app_image_method_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      method_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_type_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      app_image_type_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      public_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      package_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_string_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      string_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      method_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_jni_entrypoint_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_other_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      call_entrypoint_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      baker_read_barrier_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      jit_patches_(&assembler_, graph->GetAllocator()),
      jit_baker_read_barrier_slow_paths_(std::less<uint32_t>(),
                                         graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)) {
  // Save the link register (containing the return address) to mimic Quick.
  AddAllocatedRegister(LocationFrom(lr));

  bool use_sve = ShouldUseSVE();
  if (use_sve) {
    location_builder_ = &location_builder_sve_;
    instruction_visitor_ = &instruction_visitor_sve_;
  } else {
    location_builder_ = &location_builder_neon_;
    instruction_visitor_ = &instruction_visitor_neon_;
  }
}

bool CodeGeneratorARM64::ShouldUseSVE() const {
  return GetInstructionSetFeatures().HasSVE();
}

size_t CodeGeneratorARM64::GetSIMDRegisterWidth() const {
  return SupportsPredicatedSIMD()
      ? GetInstructionSetFeatures().GetSVEVectorLength() / kBitsPerByte
      : vixl::aarch64::kQRegSizeInBytes;
}

#define __ GetVIXLAssembler()->

void CodeGeneratorARM64::FixJumpTables() {
  for (auto&& jump_table : jump_tables_) {
    jump_table->FixTable(this);
  }
}

void CodeGeneratorARM64::Finalize() {
  FixJumpTables();

  // Emit JIT baker read barrier slow paths.
  DCHECK(GetCompilerOptions().IsJitCompiler() || jit_baker_read_barrier_slow_paths_.empty());
  for (auto& entry : jit_baker_read_barrier_slow_paths_) {
    uint32_t encoded_data = entry.first;
    vixl::aarch64::Label* slow_path_entry = &entry.second.label;
    __ Bind(slow_path_entry);
    CompileBakerReadBarrierThunk(*GetAssembler(), encoded_data, /* debug_name= */ nullptr);
  }

  // Ensure we emit the literal pool.
  __ FinalizeCode();

  CodeGenerator::Finalize();

  // Verify Baker read barrier linker patches.
  if (kIsDebugBuild) {
    ArrayRef<const uint8_t> code(GetCode());
    for (const BakerReadBarrierPatchInfo& info : baker_read_barrier_patches_) {
      DCHECK(info.label.IsBound());
      uint32_t literal_offset = info.label.GetLocation();
      DCHECK_ALIGNED(literal_offset, 4u);

      auto GetInsn = [&code](uint32_t offset) {
        DCHECK_ALIGNED(offset, 4u);
        return
            (static_cast<uint32_t>(code[offset + 0]) << 0) +
            (static_cast<uint32_t>(code[offset + 1]) << 8) +
            (static_cast<uint32_t>(code[offset + 2]) << 16)+
            (static_cast<uint32_t>(code[offset + 3]) << 24);
      };

      const uint32_t encoded_data = info.custom_data;
      BakerReadBarrierKind kind = BakerReadBarrierKindField::Decode(encoded_data);
      // Check that the next instruction matches the expected LDR.
      switch (kind) {
        case BakerReadBarrierKind::kField:
        case BakerReadBarrierKind::kAcquire: {
          DCHECK_GE(code.size() - literal_offset, 8u);
          uint32_t next_insn = GetInsn(literal_offset + 4u);
          CheckValidReg(next_insn & 0x1fu);  // Check destination register.
          const uint32_t base_reg = BakerReadBarrierFirstRegField::Decode(encoded_data);
          if (kind == BakerReadBarrierKind::kField) {
            // LDR (immediate) with correct base_reg.
            CHECK_EQ(next_insn & 0xffc003e0u, 0xb9400000u | (base_reg << 5));
          } else {
            DCHECK(kind == BakerReadBarrierKind::kAcquire);
            // LDAR with correct base_reg.
            CHECK_EQ(next_insn & 0xffffffe0u, 0x88dffc00u | (base_reg << 5));
          }
          break;
        }
        case BakerReadBarrierKind::kArray: {
          DCHECK_GE(code.size() - literal_offset, 8u);
          uint32_t next_insn = GetInsn(literal_offset + 4u);
          // LDR (register) with the correct base_reg, size=10 (32-bit), option=011 (extend = LSL),
          // and S=1 (shift amount = 2 for 32-bit version), i.e. LDR Wt, [Xn, Xm, LSL #2].
          CheckValidReg(next_insn & 0x1fu);  // Check destination register.
          const uint32_t base_reg = BakerReadBarrierFirstRegField::Decode(encoded_data);
          CHECK_EQ(next_insn & 0xffe0ffe0u, 0xb8607800u | (base_reg << 5));
          CheckValidReg((next_insn >> 16) & 0x1f);  // Check index register
          break;
        }
        case BakerReadBarrierKind::kGcRoot: {
          DCHECK_GE(literal_offset, 4u);
          uint32_t prev_insn = GetInsn(literal_offset - 4u);
          const uint32_t root_reg = BakerReadBarrierFirstRegField::Decode(encoded_data);
          // Usually LDR (immediate) with correct root_reg but
          // we may have a "MOV marked, old_value" for intrinsic CAS.
          if ((prev_insn & 0xffe0ffff) != (0x2a0003e0 | root_reg)) {    // MOV?
            CHECK_EQ(prev_insn & 0xffc0001fu, 0xb9400000u | root_reg);  // LDR?
          }
          break;
        }
        default:
          LOG(FATAL) << "Unexpected kind: " << static_cast<uint32_t>(kind);
          UNREACHABLE();
      }
    }
  }
}

void ParallelMoveResolverARM64::PrepareForEmitNativeCode() {
  // Note: There are 6 kinds of moves:
  // 1. constant -> GPR/FPR (non-cycle)
  // 2. constant -> stack (non-cycle)
  // 3. GPR/FPR -> GPR/FPR
  // 4. GPR/FPR -> stack
  // 5. stack -> GPR/FPR
  // 6. stack -> stack (non-cycle)
  // Case 1, 2 and 6 should never be included in a dependency cycle on ARM64. For case 3, 4, and 5
  // VIXL uses at most 1 GPR. VIXL has 2 GPR and 1 FPR temps, and there should be no intersecting
  // cycles on ARM64, so we always have 1 GPR and 1 FPR available VIXL temps to resolve the
  // dependency.
  vixl_temps_.Open(GetVIXLAssembler());
}

void ParallelMoveResolverARM64::FinishEmitNativeCode() {
  vixl_temps_.Close();
}

Location ParallelMoveResolverARM64::AllocateScratchLocationFor(Location::Kind kind) {
  DCHECK(kind == Location::kRegister || kind == Location::kFpuRegister
         || kind == Location::kStackSlot || kind == Location::kDoubleStackSlot
         || kind == Location::kSIMDStackSlot);
  kind = (kind == Location::kFpuRegister || kind == Location::kSIMDStackSlot)
      ? Location::kFpuRegister
      : Location::kRegister;
  Location scratch = GetScratchLocation(kind);
  if (!scratch.Equals(Location::NoLocation())) {
    return scratch;
  }
  // Allocate from VIXL temp registers.
  if (kind == Location::kRegister) {
    scratch = LocationFrom(vixl_temps_.AcquireX());
  } else {
    DCHECK_EQ(kind, Location::kFpuRegister);
    scratch = codegen_->GetGraph()->HasSIMD()
        ? codegen_->GetInstructionCodeGeneratorArm64()->AllocateSIMDScratchLocation(&vixl_temps_)
        : LocationFrom(vixl_temps_.AcquireD());
  }
  AddScratchLocation(scratch);
  return scratch;
}

void ParallelMoveResolverARM64::FreeScratchLocation(Location loc) {
  if (loc.IsRegister()) {
    vixl_temps_.Release(XRegisterFrom(loc));
  } else {
    DCHECK(loc.IsFpuRegister());
    if (codegen_->GetGraph()->HasSIMD()) {
      codegen_->GetInstructionCodeGeneratorArm64()->FreeSIMDScratchLocation(loc, &vixl_temps_);
    } else {
      vixl_temps_.Release(DRegisterFrom(loc));
    }
  }
  RemoveScratchLocation(loc);
}

void ParallelMoveResolverARM64::EmitMove(size_t index) {
  MoveOperands* move = moves_[index];
  codegen_->MoveLocation(move->GetDestination(), move->GetSource(), DataType::Type::kVoid);
}

void LocationsBuilderARM64::VisitMethodExitHook(HMethodExitHook* method_hook) {
  LocationSummary* locations = new (GetGraph()->GetAllocator())
      LocationSummary(method_hook, LocationSummary::kCallOnSlowPath);
  DataType::Type return_type = method_hook->InputAt(0)->GetType();
  locations->SetInAt(0, ARM64ReturnLocation(return_type));
}

void InstructionCodeGeneratorARM64::GenerateMethodEntryExitHook(HInstruction* instruction) {
  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);
  Register addr = temps.AcquireX();
  Register curr_entry = temps.AcquireX();
  Register value = curr_entry.W();

  SlowPathCodeARM64* slow_path =
      new (codegen_->GetScopedAllocator()) MethodEntryExitHooksSlowPathARM64(instruction);
  codegen_->AddSlowPath(slow_path);

  if (instruction->IsMethodExitHook()) {
    // Check if we are required to check if the caller needs a deoptimization. Strictly speaking it
    // would be sufficient to check if CheckCallerForDeopt bit is set. Though it is faster to check
    // if it is just non-zero. kCHA bit isn't used in debuggable runtimes as cha optimization is
    // disabled in debuggable runtime. The other bit is used when this method itself requires a
    // deoptimization due to redefinition. So it is safe to just check for non-zero value here.
    __ Ldr(value, MemOperand(sp, codegen_->GetStackOffsetOfShouldDeoptimizeFlag()));
    __ Cbnz(value, slow_path->GetEntryLabel());
  }

  uint64_t address = reinterpret_cast64<uint64_t>(Runtime::Current()->GetInstrumentation());
  MemberOffset  offset = instruction->IsMethodExitHook() ?
      instrumentation::Instrumentation::HaveMethodExitListenersOffset() :
      instrumentation::Instrumentation::HaveMethodEntryListenersOffset();
  __ Mov(addr, address + offset.Int32Value());
  __ Ldrb(value, MemOperand(addr, 0));
  __ Cmp(value, Operand(instrumentation::Instrumentation::kFastTraceListeners));
  // Check if there are any method entry / exit listeners. If no, continue.
  __ B(lt, slow_path->GetExitLabel());
  // Check if there are any slow (jvmti / trace with thread cpu time) method entry / exit listeners.
  // If yes, just take the slow path.
  __ B(gt, slow_path->GetEntryLabel());

  Register init_entry = addr;
  // Check if there is place in the buffer to store a new entry, if no, take slow path.
  uint32_t trace_buffer_curr_entry_offset =
      Thread::TraceBufferCurrPtrOffset<kArm64PointerSize>().Int32Value();
  __ Ldr(curr_entry, MemOperand(tr, trace_buffer_curr_entry_offset));
  __ Sub(curr_entry, curr_entry, kNumEntriesForWallClock * sizeof(void*));
  __ Ldr(init_entry, MemOperand(tr, Thread::TraceBufferPtrOffset<kArm64PointerSize>().SizeValue()));
  __ Cmp(curr_entry, init_entry);
  __ B(lt, slow_path->GetEntryLabel());

  // Update the index in the `Thread`.
  __ Str(curr_entry, MemOperand(tr, trace_buffer_curr_entry_offset));

  Register tmp = init_entry;
  // Record method pointer and trace action.
  __ Ldr(tmp, MemOperand(sp, 0));
  // Use last two bits to encode trace method action. For MethodEntry it is 0
  // so no need to set the bits since they are 0 already.
  if (instruction->IsMethodExitHook()) {
    DCHECK_GE(ArtMethod::Alignment(kRuntimePointerSize), static_cast<size_t>(4));
    static_assert(enum_cast<int32_t>(TraceAction::kTraceMethodEnter) == 0);
    static_assert(enum_cast<int32_t>(TraceAction::kTraceMethodExit) == 1);
    __ Orr(tmp, tmp, Operand(enum_cast<int32_t>(TraceAction::kTraceMethodExit)));
  }
  __ Str(tmp, MemOperand(curr_entry, kMethodOffsetInBytes));
  // Record the timestamp.
  __ Mrs(tmp, (SystemRegister)SYS_CNTVCT_EL0);
  __ Str(tmp, MemOperand(curr_entry, kTimestampOffsetInBytes));
  __ Bind(slow_path->GetExitLabel());
}

void InstructionCodeGeneratorARM64::VisitMethodExitHook(HMethodExitHook* instruction) {
  DCHECK(codegen_->GetCompilerOptions().IsJitCompiler() && GetGraph()->IsDebuggable());
  DCHECK(codegen_->RequiresCurrentMethod());
  GenerateMethodEntryExitHook(instruction);
}

void LocationsBuilderARM64::VisitMethodEntryHook(HMethodEntryHook* method_hook) {
  new (GetGraph()->GetAllocator()) LocationSummary(method_hook, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorARM64::VisitMethodEntryHook(HMethodEntryHook* instruction) {
  DCHECK(codegen_->GetCompilerOptions().IsJitCompiler() && GetGraph()->IsDebuggable());
  DCHECK(codegen_->RequiresCurrentMethod());
  GenerateMethodEntryExitHook(instruction);
}

void CodeGeneratorARM64::MaybeRecordTraceEvent(bool is_method_entry) {
  if (!art_flags::always_enable_profile_code()) {
    return;
  }

  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);
  Register addr = temps.AcquireX();
  CHECK(addr.Is(vixl::aarch64::x16));

  SlowPathCodeARM64* slow_path =
      new (GetScopedAllocator()) TracingMethodEntryExitHooksSlowPathARM64(is_method_entry);
  AddSlowPath(slow_path);

  __ Ldr(addr, MemOperand(tr, Thread::TraceBufferPtrOffset<kArm64PointerSize>().SizeValue()));
  __ Cbnz(addr, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorARM64::MaybeIncrementHotness(HSuspendCheck* suspend_check, bool is_frame_entry) {
  MacroAssembler* masm = GetVIXLAssembler();
  if (GetCompilerOptions().CountHotnessInCompiledCode()) {
    UseScratchRegisterScope temps(masm);
    Register counter = temps.AcquireX();
    Register method = is_frame_entry ? kArtMethodRegister : temps.AcquireX();
    if (!is_frame_entry) {
      __ Ldr(method, MemOperand(sp, 0));
    }
    __ Ldrh(counter, MemOperand(method, ArtMethod::HotnessCountOffset().Int32Value()));
    vixl::aarch64::Label done;
    DCHECK_EQ(0u, interpreter::kNterpHotnessValue);
    __ Cbz(counter, &done);
    __ Add(counter, counter, -1);
    __ Strh(counter, MemOperand(method, ArtMethod::HotnessCountOffset().Int32Value()));
    __ Bind(&done);
  }

  if (GetGraph()->IsCompilingBaseline() &&
      GetGraph()->IsUsefulOptimizing() &&
      !Runtime::Current()->IsAotCompiler()) {
    ProfilingInfo* info = GetGraph()->GetProfilingInfo();
    DCHECK(info != nullptr);
    DCHECK(!HasEmptyFrame());
    uint64_t address = reinterpret_cast64<uint64_t>(info);
    UseScratchRegisterScope temps(masm);
    Register counter = temps.AcquireW();
    SlowPathCodeARM64* slow_path = new (GetScopedAllocator()) CompileOptimizedSlowPathARM64(
        suspend_check, /* profiling_info= */ lr);
    AddSlowPath(slow_path);
    __ Ldr(lr, jit_patches_.DeduplicateUint64Literal(address));
    __ Ldrh(counter, MemOperand(lr, ProfilingInfo::BaselineHotnessCountOffset().Int32Value()));
    __ Cbz(counter, slow_path->GetEntryLabel());
    __ Add(counter, counter, -1);
    __ Strh(counter, MemOperand(lr, ProfilingInfo::BaselineHotnessCountOffset().Int32Value()));
    __ Bind(slow_path->GetExitLabel());
  }
}

void CodeGeneratorARM64::GenerateFrameEntry() {
  MacroAssembler* masm = GetVIXLAssembler();

  // Check if we need to generate the clinit check. We will jump to the
  // resolution stub if the class is not initialized and the executing thread is
  // not the thread initializing it.
  // We do this before constructing the frame to get the correct stack trace if
  // an exception is thrown.
  if (GetCompilerOptions().ShouldCompileWithClinitCheck(GetGraph()->GetArtMethod())) {
    UseScratchRegisterScope temps(masm);
    vixl::aarch64::Label resolution;
    vixl::aarch64::Label memory_barrier;

    Register temp1 = temps.AcquireW();
    Register temp2 = temps.AcquireW();

    // Check if we're visibly initialized.

    // We don't emit a read barrier here to save on code size. We rely on the
    // resolution trampoline to do a suspend check before re-entering this code.
    __ Ldr(temp1, MemOperand(kArtMethodRegister, ArtMethod::DeclaringClassOffset().Int32Value()));
    __ Ldrb(temp2, HeapOperand(temp1, kClassStatusByteOffset));
    __ Cmp(temp2, kShiftedVisiblyInitializedValue);
    __ B(hs, &frame_entry_label_);

    // Check if we're initialized and jump to code that does a memory barrier if
    // so.
    __ Cmp(temp2, kShiftedInitializedValue);
    __ B(hs, &memory_barrier);

    // Check if we're initializing and the thread initializing is the one
    // executing the code.
    __ Cmp(temp2, kShiftedInitializingValue);
    __ B(lo, &resolution);

    __ Ldr(temp1, HeapOperand(temp1, mirror::Class::ClinitThreadIdOffset().Int32Value()));
    __ Ldr(temp2, MemOperand(tr, Thread::TidOffset<kArm64PointerSize>().Int32Value()));
    __ Cmp(temp1, temp2);
    __ B(eq, &frame_entry_label_);
    __ Bind(&resolution);

    // Jump to the resolution stub.
    ThreadOffset64 entrypoint_offset =
        GetThreadOffset<kArm64PointerSize>(kQuickQuickResolutionTrampoline);
    __ Ldr(temp1.X(), MemOperand(tr, entrypoint_offset.Int32Value()));
    __ Br(temp1.X());

    __ Bind(&memory_barrier);
    GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
  }
  __ Bind(&frame_entry_label_);

  bool do_overflow_check =
      FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kArm64) || !IsLeafMethod();
  if (do_overflow_check) {
    UseScratchRegisterScope temps(masm);
    Register temp = temps.AcquireX();
    DCHECK(GetCompilerOptions().GetImplicitStackOverflowChecks());
    __ Sub(temp, sp, static_cast<int32_t>(GetStackOverflowReservedBytes(InstructionSet::kArm64)));
    {
      // Ensure that between load and RecordPcInfo there are no pools emitted.
      ExactAssemblyScope eas(GetVIXLAssembler(),
                             kInstructionSize,
                             CodeBufferCheckScope::kExactSize);
      __ ldr(wzr, MemOperand(temp, 0));
      RecordPcInfoForFrameOrBlockEntry();
    }
  }

  if (!HasEmptyFrame()) {
    // Make sure the frame size isn't unreasonably large.
    DCHECK_LE(GetFrameSize(), GetMaximumFrameSize());

    // Stack layout:
    //      sp[frame_size - 8]        : lr.
    //      ...                       : other preserved core registers.
    //      ...                       : other preserved fp registers.
    //      ...                       : reserved frame space.
    //      sp[0]                     : current method.
    int32_t frame_size = dchecked_integral_cast<int32_t>(GetFrameSize());
    uint32_t core_spills_offset = frame_size - GetCoreSpillSize();
    CPURegList preserved_core_registers = GetFramePreservedCoreRegisters();
    DCHECK(!preserved_core_registers.IsEmpty());
    uint32_t fp_spills_offset = frame_size - FrameEntrySpillSize();
    CPURegList preserved_fp_registers = GetFramePreservedFPRegisters();

    // Save the current method if we need it, or if using STP reduces code
    // size. Note that we do not do this in HCurrentMethod, as the
    // instruction might have been removed in the SSA graph.
    CPURegister lowest_spill;
    if (core_spills_offset == kXRegSizeInBytes) {
      // If there is no gap between the method and the lowest core spill, use
      // aligned STP pre-index to store both. Max difference is 512. We do
      // that to reduce code size even if we do not have to save the method.
      DCHECK_LE(frame_size, 512);  // 32 core registers are only 256 bytes.
      lowest_spill = preserved_core_registers.PopLowestIndex();
      __ Stp(kArtMethodRegister, lowest_spill, MemOperand(sp, -frame_size, PreIndex));
    } else if (RequiresCurrentMethod()) {
      __ Str(kArtMethodRegister, MemOperand(sp, -frame_size, PreIndex));
    } else {
      __ Claim(frame_size);
    }
    GetAssembler()->cfi().AdjustCFAOffset(frame_size);
    if (lowest_spill.IsValid()) {
      GetAssembler()->cfi().RelOffset(DWARFReg(lowest_spill), core_spills_offset);
      core_spills_offset += kXRegSizeInBytes;
    }
    GetAssembler()->SpillRegisters(preserved_core_registers, core_spills_offset);
    GetAssembler()->SpillRegisters(preserved_fp_registers, fp_spills_offset);

    if (GetGraph()->HasShouldDeoptimizeFlag()) {
      // Initialize should_deoptimize flag to 0.
      Register wzr = Register(VIXLRegCodeFromART(WZR), kWRegSize);
      __ Str(wzr, MemOperand(sp, GetStackOffsetOfShouldDeoptimizeFlag()));
    }

    MaybeRecordTraceEvent(/* is_method_entry= */ true);
  }
  MaybeIncrementHotness(/* suspend_check= */ nullptr, /* is_frame_entry= */ true);
  MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void CodeGeneratorARM64::GenerateFrameExit() {
  if (!HasEmptyFrame()) {
    MaybeRecordTraceEvent(/* is_method_entry= */ false);
    PopFrameAndReturn(GetAssembler(),
                      dchecked_integral_cast<int32_t>(GetFrameSize()),
                      GetFramePreservedCoreRegisters(),
                      GetFramePreservedFPRegisters());
  } else {
    __ Ret();
  }
}

void CodeGeneratorARM64::PopFrameAndReturn(Arm64Assembler* assembler,
                                           int32_t frame_size,
                                           CPURegList preserved_core_registers,
                                           CPURegList preserved_fp_registers) {
  DCHECK(!preserved_core_registers.IsEmpty());
  uint32_t core_spill_size = preserved_core_registers.GetTotalSizeInBytes();
  uint32_t frame_entry_spill_size = preserved_fp_registers.GetTotalSizeInBytes() + core_spill_size;
  uint32_t core_spills_offset = frame_size - core_spill_size;
  uint32_t fp_spills_offset = frame_size - frame_entry_spill_size;
  vixl::aarch64::MacroAssembler* vixl_assembler = assembler->GetVIXLAssembler();

  CPURegister lowest_spill;
  if (core_spills_offset == kXRegSizeInBytes) {
    // If there is no gap between the method and the lowest core spill, use
    // aligned LDP pre-index to pop both. Max difference is 504. We do
    // that to reduce code size even though the loaded method is unused.
    DCHECK_LE(frame_size, 504);  // 32 core registers are only 256 bytes.
    lowest_spill = preserved_core_registers.PopLowestIndex();
    core_spills_offset += kXRegSizeInBytes;
  }

  assembler->cfi().RememberState();
  assembler->UnspillRegisters(preserved_fp_registers, fp_spills_offset);
  assembler->UnspillRegisters(preserved_core_registers, core_spills_offset);
  if (lowest_spill.IsValid()) {
    vixl_assembler->Ldp(xzr, lowest_spill, MemOperand(sp, frame_size, PostIndex));
    assembler->cfi().Restore(DWARFReg(lowest_spill));
  } else {
    vixl_assembler->Drop(frame_size);
  }
  assembler->cfi().AdjustCFAOffset(-frame_size);
  vixl_assembler->Ret();
  assembler->cfi().RestoreState();
  assembler->cfi().DefCFAOffset(frame_size);
}

CPURegList CodeGeneratorARM64::GetFramePreservedCoreRegisters() const {
  DCHECK(ArtVixlRegCodeCoherentForRegSet(core_spill_mask_, GetNumberOfCoreRegisters(), 0, 0));
  return CPURegList(CPURegister::kRegister, kXRegSize,
                    core_spill_mask_);
}

CPURegList CodeGeneratorARM64::GetFramePreservedFPRegisters() const {
  DCHECK(ArtVixlRegCodeCoherentForRegSet(0, 0, fpu_spill_mask_,
                                         GetNumberOfFloatingPointRegisters()));
  return CPURegList(CPURegister::kVRegister, kDRegSize,
                    fpu_spill_mask_);
}

void CodeGeneratorARM64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorARM64::MoveConstant(Location location, int32_t value) {
  DCHECK(location.IsRegister());
  __ Mov(RegisterFrom(location, DataType::Type::kInt32), value);
}

void CodeGeneratorARM64::AddLocationAsTemp(Location location, LocationSummary* locations) {
  if (location.IsRegister()) {
    locations->AddTemp(location);
  } else {
    UNIMPLEMENTED(FATAL) << "AddLocationAsTemp not implemented for location " << location;
  }
}

void CodeGeneratorARM64::MaybeMarkGCCard(Register object, Register value, bool emit_null_check) {
  vixl::aarch64::Label done;
  if (emit_null_check) {
    __ Cbz(value, &done);
  }
  MarkGCCard(object);
  if (emit_null_check) {
    __ Bind(&done);
  }
}

void CodeGeneratorARM64::MarkGCCard(Register object) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register card = temps.AcquireX();
  Register temp = temps.AcquireW();  // Index within the CardTable - 32bit.
  // Load the address of the card table into `card`.
  __ Ldr(card, MemOperand(tr, Thread::CardTableOffset<kArm64PointerSize>().Int32Value()));
  // Calculate the offset (in the card table) of the card corresponding to `object`.
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  // Write the `art::gc::accounting::CardTable::kCardDirty` value into the
  // `object`'s card.
  //
  // Register `card` contains the address of the card table. Note that the card
  // table's base is biased during its creation so that it always starts at an
  // address whose least-significant byte is equal to `kCardDirty` (see
  // art::gc::accounting::CardTable::Create). Therefore the STRB instruction
  // below writes the `kCardDirty` (byte) value into the `object`'s card
  // (located at `card + object >> kCardShift`).
  //
  // This dual use of the value in register `card` (1. to calculate the location
  // of the card to mark; and 2. to load the `kCardDirty` value) saves a load
  // (no need to explicitly load `kCardDirty` as an immediate value).
  __ Strb(card, MemOperand(card, temp.X()));
}

void CodeGeneratorARM64::CheckGCCardIsValid(Register object) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register card = temps.AcquireX();
  Register temp = temps.AcquireW();  // Index within the CardTable - 32bit.
  vixl::aarch64::Label done;
  // Load the address of the card table into `card`.
  __ Ldr(card, MemOperand(tr, Thread::CardTableOffset<kArm64PointerSize>().Int32Value()));
  // Calculate the offset (in the card table) of the card corresponding to `object`.
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  // assert (!clean || !self->is_gc_marking)
  __ Ldrb(temp, MemOperand(card, temp.X()));
  static_assert(gc::accounting::CardTable::kCardClean == 0);
  __ Cbnz(temp, &done);
  __ Cbz(mr, &done);
  __ Unreachable();
  __ Bind(&done);
}

void CodeGeneratorARM64::SetupBlockedRegisters() const {
  // Blocked core registers:
  //      lr        : Runtime reserved.
  //      tr        : Runtime reserved.
  //      mr        : Runtime reserved.
  //      ip1       : VIXL core temp.
  //      ip0       : VIXL core temp.
  //      x18       : Platform register.
  //
  // Blocked fp registers:
  //      d31       : VIXL fp temp.
  CPURegList reserved_core_registers = vixl_reserved_core_registers;
  reserved_core_registers.Combine(runtime_reserved_core_registers);
  while (!reserved_core_registers.IsEmpty()) {
    blocked_core_registers_[reserved_core_registers.PopLowestIndex().GetCode()] = true;
  }
  blocked_core_registers_[X18] = true;

  CPURegList reserved_fp_registers = vixl_reserved_fp_registers;
  while (!reserved_fp_registers.IsEmpty()) {
    blocked_fpu_registers_[reserved_fp_registers.PopLowestIndex().GetCode()] = true;
  }

  if (GetGraph()->IsDebuggable()) {
    // Stubs do not save callee-save floating point registers. If the graph
    // is debuggable, we need to deal with these registers differently. For
    // now, just block them.
    CPURegList reserved_fp_registers_debuggable = callee_saved_fp_registers;
    while (!reserved_fp_registers_debuggable.IsEmpty()) {
      blocked_fpu_registers_[reserved_fp_registers_debuggable.PopLowestIndex().GetCode()] = true;
    }
  }
}

size_t CodeGeneratorARM64::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  Register reg = Register(VIXLRegCodeFromART(reg_id), kXRegSize);
  __ Str(reg, MemOperand(sp, stack_index));
  return kArm64WordSize;
}

size_t CodeGeneratorARM64::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  Register reg = Register(VIXLRegCodeFromART(reg_id), kXRegSize);
  __ Ldr(reg, MemOperand(sp, stack_index));
  return kArm64WordSize;
}

size_t CodeGeneratorARM64::SaveFloatingPointRegister([[maybe_unused]] size_t stack_index,
                                                     [[maybe_unused]] uint32_t reg_id) {
  LOG(FATAL) << "FP registers shouldn't be saved/restored individually, "
             << "use SaveRestoreLiveRegistersHelper";
  UNREACHABLE();
}

size_t CodeGeneratorARM64::RestoreFloatingPointRegister([[maybe_unused]] size_t stack_index,
                                                        [[maybe_unused]] uint32_t reg_id) {
  LOG(FATAL) << "FP registers shouldn't be saved/restored individually, "
             << "use SaveRestoreLiveRegistersHelper";
  UNREACHABLE();
}

// BEGIN Motorola, a5705c, 10/16/2015, IKSWM-7832
size_t CodeGeneratorARM64::SaveBulkLiveCoreRegisters(LocationSummary* locations,
                                                     size_t stack_offset,
                                                     uint32_t* saved_stack_offsets) {
  auto update_location_and_stack_offset = [locations, saved_stack_offsets]
                                          (size_t x, size_t offset) {
    if (locations->RegisterContainsObject(x)) {
      locations->SetStackBit(offset / kVRegSize);
    }
    saved_stack_offsets[x] = offset;
  };

  RegisterSet* register_set = locations->GetLiveRegisters();
  stack_offset = (stack_offset + (kArm64WordSize - 1)) & ~(kArm64WordSize - 1);
  size_t last_reg = SIZE_MAX;
  for (size_t i = 0, e = GetNumberOfCoreRegisters(); i < e; ++i) {
    if (!IsCoreCalleeSaveRegister(i) && register_set->ContainsCoreRegister(i)) {
      DCHECK_LT(stack_offset, GetFrameSize() - FrameEntrySpillSize());
      DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
      if (last_reg == SIZE_MAX) {
        last_reg = i;
      } else {
        Register reg1 = Register(VIXLRegCodeFromART(last_reg), kXRegSize);
        Register reg2 = Register(VIXLRegCodeFromART(i), kXRegSize);
        update_location_and_stack_offset(last_reg, stack_offset);
        if (vixl::aarch64::AreSameSizeAndType(reg1, reg2)) {
          update_location_and_stack_offset(i, (stack_offset + kArm64WordSize));
          __ Stp(reg1, reg2, MemOperand(sp, stack_offset));
          stack_offset += kArm64WordSize + kArm64WordSize;
          last_reg = SIZE_MAX;
        } else {
          stack_offset += SaveCoreRegister(stack_offset, last_reg);
          last_reg = i;
        }
      }
    }
  }
  if (last_reg != SIZE_MAX) {
    update_location_and_stack_offset(last_reg, stack_offset);
    stack_offset += SaveCoreRegister(stack_offset, last_reg);
  }
  return stack_offset;
}

size_t CodeGeneratorARM64::SaveBulkLiveFpuRegisters(LocationSummary* locations,
                                                     size_t stack_offset,
                                                     uint32_t* saved_stack_offsets) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  stack_offset = (stack_offset + (kArm64WordSize - 1)) & ~(kArm64WordSize - 1);
  size_t last_reg = SIZE_MAX;
  for (size_t i = 0, e = GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    if (!IsFloatingPointCalleeSaveRegister(i) && register_set->ContainsFloatingPointRegister(i)) {
      DCHECK_LT(stack_offset, GetFrameSize() - FrameEntrySpillSize());
      DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
      if (last_reg == SIZE_MAX) {
        last_reg = i;
      } else {
        VRegister reg1 = VRegister(last_reg, kDRegSize);
        VRegister reg2 = VRegister(i, kDRegSize);
        saved_stack_offsets[last_reg] = stack_offset;
        if (vixl::aarch64::AreSameSizeAndType(reg1, reg2)) {
          saved_stack_offsets[i] = stack_offset + kArm64WordSize;
          __ Stp(reg1, reg2, MemOperand(sp, stack_offset));
          stack_offset += kArm64WordSize + kArm64WordSize;
          last_reg = SIZE_MAX;
        } else {
          stack_offset += SaveFloatingPointRegister(stack_offset, last_reg);
          last_reg = i;
        }
      }
    }
  }
  if (last_reg != SIZE_MAX) {
    stack_offset += SaveFloatingPointRegister(stack_offset, last_reg);
  }
  return stack_offset;
}

size_t CodeGeneratorARM64::RestoreBulkLiveCoreRegisters(LocationSummary* locations,
                                                        size_t stack_offset) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  stack_offset = (stack_offset + (kArm64WordSize - 1)) & ~(kArm64WordSize - 1);
  size_t last_reg = SIZE_MAX;
  for (size_t i = 0, e = GetNumberOfCoreRegisters(); i < e; ++i) {
    if (!IsCoreCalleeSaveRegister(i) && register_set->ContainsCoreRegister(i)) {
      DCHECK_LT(stack_offset, GetFrameSize() - FrameEntrySpillSize());
      if (last_reg == SIZE_MAX) {
        last_reg = i;
      } else {
        Register reg1 = Register(VIXLRegCodeFromART(last_reg), kXRegSize);
        Register reg2 = Register(VIXLRegCodeFromART(i), kXRegSize);
        if (vixl::aarch64::AreSameSizeAndType(reg1, reg2)) {
          __ Ldp(reg1, reg2, MemOperand(sp, stack_offset));
          stack_offset += kArm64WordSize + kArm64WordSize;
          last_reg = SIZE_MAX;
        } else {
          stack_offset += RestoreCoreRegister(stack_offset, last_reg);
          last_reg = i;
        }
      }
    }
  }
  if (last_reg != SIZE_MAX) {
    stack_offset += RestoreCoreRegister(stack_offset, last_reg);
  }
  return stack_offset;
}

size_t CodeGeneratorARM64::RestoreBulkLiveFpuRegisters(LocationSummary* locations,
                                                        size_t stack_offset) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  stack_offset = (stack_offset + (kArm64WordSize - 1)) & ~(kArm64WordSize - 1);
  size_t last_reg = SIZE_MAX;
  for (size_t i = 0, e = GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    if (!IsFloatingPointCalleeSaveRegister(i) && register_set->ContainsFloatingPointRegister(i)) {
      DCHECK_LT(stack_offset, GetFrameSize() - FrameEntrySpillSize());
      if (last_reg == SIZE_MAX) {
        last_reg = i;
      } else {
        VRegister reg1 = VRegister(last_reg, kDRegSize);
        VRegister reg2 = VRegister(i, kDRegSize);
        if (vixl::aarch64::AreSameSizeAndType(reg1, reg2)) {
          __ Ldp(reg1, reg2, MemOperand(sp, stack_offset));
          stack_offset += kArm64WordSize + kArm64WordSize;
          last_reg = SIZE_MAX;
        } else {
          stack_offset += RestoreFloatingPointRegister(stack_offset, last_reg);
          last_reg = i;
        }
      }
    }
  }
  if (last_reg != SIZE_MAX) {
    stack_offset += RestoreFloatingPointRegister(stack_offset, last_reg);
  }
  return stack_offset;
}
// END IKSWM-7832

void CodeGeneratorARM64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << XRegister(reg);
}

void CodeGeneratorARM64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << DRegister(reg);
}

const Arm64InstructionSetFeatures& CodeGeneratorARM64::GetInstructionSetFeatures() const {
  return *GetCompilerOptions().GetInstructionSetFeatures()->AsArm64InstructionSetFeatures();
}

void CodeGeneratorARM64::MoveConstant(CPURegister destination, HConstant* constant) {
  if (constant->IsIntConstant()) {
    __ Mov(Register(destination), constant->AsIntConstant()->GetValue());
  } else if (constant->IsLongConstant()) {
    __ Mov(Register(destination), constant->AsLongConstant()->GetValue());
  } else if (constant->IsNullConstant()) {
    __ Mov(Register(destination), 0);
  } else if (constant->IsFloatConstant()) {
    __ Fmov(VRegister(destination), constant->AsFloatConstant()->GetValue());
  } else {
    DCHECK(constant->IsDoubleConstant());
    __ Fmov(VRegister(destination), constant->AsDoubleConstant()->GetValue());
  }
}


static bool CoherentConstantAndType(Location constant, DataType::Type type) {
  DCHECK(constant.IsConstant());
  HConstant* cst = constant.GetConstant();
  return (cst->IsIntConstant() && type == DataType::Type::kInt32) ||
         // Null is mapped to a core W register, which we associate with kPrimInt.
         (cst->IsNullConstant() && type == DataType::Type::kInt32) ||
         (cst->IsLongConstant() && type == DataType::Type::kInt64) ||
         (cst->IsFloatConstant() && type == DataType::Type::kFloat32) ||
         (cst->IsDoubleConstant() && type == DataType::Type::kFloat64);
}

// Allocate a scratch register from the VIXL pool, querying first
// the floating-point register pool, and then the core register
// pool. This is essentially a reimplementation of
// vixl::aarch64::UseScratchRegisterScope::AcquireCPURegisterOfSize
// using a different allocation strategy.
static CPURegister AcquireFPOrCoreCPURegisterOfSize(vixl::aarch64::MacroAssembler* masm,
                                                    vixl::aarch64::UseScratchRegisterScope* temps,
                                                    int size_in_bits) {
  return masm->GetScratchVRegisterList()->IsEmpty()
      ? CPURegister(temps->AcquireRegisterOfSize(size_in_bits))
      : CPURegister(temps->AcquireVRegisterOfSize(size_in_bits));
}

void CodeGeneratorARM64::MoveLocation(Location destination,
                                      Location source,
                                      DataType::Type dst_type) {
  if (source.Equals(destination)) {
    return;
  }

  // A valid move can always be inferred from the destination and source
  // locations. When moving from and to a register, the argument type can be
  // used to generate 32bit instead of 64bit moves. In debug mode we also
  // checks the coherency of the locations and the type.
  bool unspecified_type = (dst_type == DataType::Type::kVoid);

  if (destination.IsRegister() || destination.IsFpuRegister()) {
    if (unspecified_type) {
      HConstant* src_cst = source.IsConstant() ? source.GetConstant() : nullptr;
      if (source.IsStackSlot() ||
          (src_cst != nullptr && (src_cst->IsIntConstant()
                                  || src_cst->IsFloatConstant()
                                  || src_cst->IsNullConstant()))) {
        // For stack slots and 32bit constants, a 64bit type is appropriate.
        dst_type = destination.IsRegister() ? DataType::Type::kInt32 : DataType::Type::kFloat32;
      } else {
        // If the source is a double stack slot or a 64bit constant, a 64bit
        // type is appropriate. Else the source is a register, and since the
        // type has not been specified, we chose a 64bit type to force a 64bit
        // move.
        dst_type = destination.IsRegister() ? DataType::Type::kInt64 : DataType::Type::kFloat64;
      }
    }
    DCHECK((destination.IsFpuRegister() && DataType::IsFloatingPointType(dst_type)) ||
           (destination.IsRegister() && !DataType::IsFloatingPointType(dst_type)));
    CPURegister dst = CPURegisterFrom(destination, dst_type);
    if (source.IsStackSlot() || source.IsDoubleStackSlot()) {
      DCHECK(dst.Is64Bits() == source.IsDoubleStackSlot());
      __ Ldr(dst, StackOperandFrom(source));
    } else if (source.IsSIMDStackSlot()) {
      GetInstructionCodeGeneratorArm64()->LoadSIMDRegFromStack(destination, source);
    } else if (source.IsConstant()) {
      DCHECK(CoherentConstantAndType(source, dst_type));
      MoveConstant(dst, source.GetConstant());
    } else if (source.IsRegister()) {
      if (destination.IsRegister()) {
        __ Mov(Register(dst), RegisterFrom(source, dst_type));
      } else {
        DCHECK(destination.IsFpuRegister());
        DataType::Type source_type = DataType::Is64BitType(dst_type)
            ? DataType::Type::kInt64
            : DataType::Type::kInt32;
        __ Fmov(FPRegisterFrom(destination, dst_type), RegisterFrom(source, source_type));
      }
    } else {
      DCHECK(source.IsFpuRegister());
      if (destination.IsRegister()) {
        DataType::Type source_type = DataType::Is64BitType(dst_type)
            ? DataType::Type::kFloat64
            : DataType::Type::kFloat32;
        __ Fmov(RegisterFrom(destination, dst_type), FPRegisterFrom(source, source_type));
      } else {
        DCHECK(destination.IsFpuRegister());
        if (GetGraph()->HasSIMD()) {
          GetInstructionCodeGeneratorArm64()->MoveSIMDRegToSIMDReg(destination, source);
        } else {
          __ Fmov(VRegister(dst), FPRegisterFrom(source, dst_type));
        }
      }
    }
  } else if (destination.IsSIMDStackSlot()) {
    GetInstructionCodeGeneratorArm64()->MoveToSIMDStackSlot(destination, source);
  } else {  // The destination is not a register. It must be a stack slot.
    DCHECK(destination.IsStackSlot() || destination.IsDoubleStackSlot());
    if (source.IsRegister() || source.IsFpuRegister()) {
      if (unspecified_type) {
        if (source.IsRegister()) {
          dst_type = destination.IsStackSlot() ? DataType::Type::kInt32 : DataType::Type::kInt64;
        } else {
          dst_type =
              destination.IsStackSlot() ? DataType::Type::kFloat32 : DataType::Type::kFloat64;
        }
      }
      DCHECK((destination.IsDoubleStackSlot() == DataType::Is64BitType(dst_type)) &&
             (source.IsFpuRegister() == DataType::IsFloatingPointType(dst_type)));
      __ Str(CPURegisterFrom(source, dst_type), StackOperandFrom(destination));
    } else if (source.IsConstant()) {
      DCHECK(unspecified_type || CoherentConstantAndType(source, dst_type))
          << source << " " << dst_type;
      UseScratchRegisterScope temps(GetVIXLAssembler());
      HConstant* src_cst = source.GetConstant();
      CPURegister temp;
      if (src_cst->IsZeroBitPattern()) {
        temp = (src_cst->IsLongConstant() || src_cst->IsDoubleConstant())
            ? Register(xzr)
            : Register(wzr);
      } else {
        if (src_cst->IsIntConstant()) {
          temp = temps.AcquireW();
        } else if (src_cst->IsLongConstant()) {
          temp = temps.AcquireX();
        } else if (src_cst->IsFloatConstant()) {
          temp = temps.AcquireS();
        } else {
          DCHECK(src_cst->IsDoubleConstant());
          temp = temps.AcquireD();
        }
        MoveConstant(temp, src_cst);
      }
      __ Str(temp, StackOperandFrom(destination));
    } else {
      DCHECK(source.IsStackSlot() || source.IsDoubleStackSlot());
      DCHECK(source.IsDoubleStackSlot() == destination.IsDoubleStackSlot());
      UseScratchRegisterScope temps(GetVIXLAssembler());
      // Use any scratch register (a core or a floating-point one)
      // from VIXL scratch register pools as a temporary.
      //
      // We used to only use the FP scratch register pool, but in some
      // rare cases the only register from this pool (D31) would
      // already be used (e.g. within a ParallelMove instruction, when
      // a move is blocked by a another move requiring a scratch FP
      // register, which would reserve D31). To prevent this issue, we
      // ask for a scratch register of any type (core or FP).
      //
      // Also, we start by asking for a FP scratch register first, as the
      // demand of scratch core registers is higher. This is why we
      // use AcquireFPOrCoreCPURegisterOfSize instead of
      // UseScratchRegisterScope::AcquireCPURegisterOfSize, which
      // allocates core scratch registers first.
      CPURegister temp = AcquireFPOrCoreCPURegisterOfSize(
          GetVIXLAssembler(),
          &temps,
          (destination.IsDoubleStackSlot() ? kXRegSize : kWRegSize));
      __ Ldr(temp, StackOperandFrom(source));
      __ Str(temp, StackOperandFrom(destination));
    }
  }
}

void CodeGeneratorARM64::Load(DataType::Type type,
                              CPURegister dst,
                              const MemOperand& src) {
  switch (type) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
      __ Ldrb(Register(dst), src);
      break;
    case DataType::Type::kInt8:
      __ Ldrsb(Register(dst), src);
      break;
    case DataType::Type::kUint16:
      __ Ldrh(Register(dst), src);
      break;
    case DataType::Type::kInt16:
      __ Ldrsh(Register(dst), src);
      break;
    case DataType::Type::kInt32:
    case DataType::Type::kReference:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      DCHECK_EQ(dst.Is64Bits(), DataType::Is64BitType(type));
      __ Ldr(dst, src);
      break;
    case DataType::Type::kUint32:
    case DataType::Type::kUint64:
    case DataType::Type::kVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::LoadAcquire(HInstruction* instruction,
                                     DataType::Type type,
                                     CPURegister dst,
                                     const MemOperand& src,
                                     bool needs_null_check) {
  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);
  Register temp_base = temps.AcquireX();

  DCHECK(!src.IsPreIndex());
  DCHECK(!src.IsPostIndex());

  // TODO(vixl): Let the MacroAssembler handle MemOperand.
  __ Add(temp_base, src.GetBaseRegister(), OperandFromMemOperand(src));
  {
    // Ensure that between load and MaybeRecordImplicitNullCheck there are no pools emitted.
    MemOperand base = MemOperand(temp_base);
    switch (type) {
      case DataType::Type::kBool:
      case DataType::Type::kUint8:
      case DataType::Type::kInt8:
        {
          ExactAssemblyScope eas(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
          __ ldarb(Register(dst), base);
          if (needs_null_check) {
            MaybeRecordImplicitNullCheck(instruction);
          }
        }
        if (type == DataType::Type::kInt8) {
          __ Sbfx(Register(dst), Register(dst), 0, DataType::Size(type) * kBitsPerByte);
        }
        break;
      case DataType::Type::kUint16:
      case DataType::Type::kInt16:
        {
          ExactAssemblyScope eas(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
          __ ldarh(Register(dst), base);
          if (needs_null_check) {
            MaybeRecordImplicitNullCheck(instruction);
          }
        }
        if (type == DataType::Type::kInt16) {
          __ Sbfx(Register(dst), Register(dst), 0, DataType::Size(type) * kBitsPerByte);
        }
        break;
      case DataType::Type::kInt32:
      case DataType::Type::kReference:
      case DataType::Type::kInt64:
        DCHECK_EQ(dst.Is64Bits(), DataType::Is64BitType(type));
        {
          ExactAssemblyScope eas(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
          __ ldar(Register(dst), base);
          if (needs_null_check) {
            MaybeRecordImplicitNullCheck(instruction);
          }
        }
        break;
      case DataType::Type::kFloat32:
      case DataType::Type::kFloat64: {
        DCHECK(dst.IsFPRegister());
        DCHECK_EQ(dst.Is64Bits(), DataType::Is64BitType(type));

        Register temp = dst.Is64Bits() ? temps.AcquireX() : temps.AcquireW();
        {
          ExactAssemblyScope eas(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
          __ ldar(temp, base);
          if (needs_null_check) {
            MaybeRecordImplicitNullCheck(instruction);
          }
        }
        __ Fmov(VRegister(dst), temp);
        break;
      }
      case DataType::Type::kUint32:
      case DataType::Type::kUint64:
      case DataType::Type::kVoid:
        LOG(FATAL) << "Unreachable type " << type;
    }
  }
}

void CodeGeneratorARM64::Store(DataType::Type type,
                               CPURegister src,
                               const MemOperand& dst) {
  switch (type) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      __ Strb(Register(src), dst);
      break;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      __ Strh(Register(src), dst);
      break;
    case DataType::Type::kInt32:
    case DataType::Type::kReference:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      DCHECK_EQ(src.Is64Bits(), DataType::Is64BitType(type));
      __ Str(src, dst);
      break;
    case DataType::Type::kUint32:
    case DataType::Type::kUint64:
    case DataType::Type::kVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::StoreRelease(HInstruction* instruction,
                                      DataType::Type type,
                                      CPURegister src,
                                      const MemOperand& dst,
                                      bool needs_null_check) {
  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp_base = temps.AcquireX();

  DCHECK(!dst.IsPreIndex());
  DCHECK(!dst.IsPostIndex());

  // TODO(vixl): Let the MacroAssembler handle this.
  Operand op = OperandFromMemOperand(dst);
  __ Add(temp_base, dst.GetBaseRegister(), op);
  MemOperand base = MemOperand(temp_base);
  // Ensure that between store and MaybeRecordImplicitNullCheck there are no pools emitted.
  switch (type) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      {
        ExactAssemblyScope eas(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
        __ stlrb(Register(src), base);
        if (needs_null_check) {
          MaybeRecordImplicitNullCheck(instruction);
        }
      }
      break;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      {
        ExactAssemblyScope eas(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
        __ stlrh(Register(src), base);
        if (needs_null_check) {
          MaybeRecordImplicitNullCheck(instruction);
        }
      }
      break;
    case DataType::Type::kInt32:
    case DataType::Type::kReference:
    case DataType::Type::kInt64:
      DCHECK_EQ(src.Is64Bits(), DataType::Is64BitType(type));
      {
        ExactAssemblyScope eas(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
        __ stlr(Register(src), base);
        if (needs_null_check) {
          MaybeRecordImplicitNullCheck(instruction);
        }
      }
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      DCHECK_EQ(src.Is64Bits(), DataType::Is64BitType(type));
      Register temp_src;
      if (src.IsZero()) {
        // The zero register is used to avoid synthesizing zero constants.
        temp_src = Register(src);
      } else {
        DCHECK(src.IsFPRegister());
        temp_src = src.Is64Bits() ? temps.AcquireX() : temps.AcquireW();
        __ Fmov(temp_src, VRegister(src));
      }
      {
        ExactAssemblyScope eas(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
        __ stlr(temp_src, base);
        if (needs_null_check) {
          MaybeRecordImplicitNullCheck(instruction);
        }
      }
      break;
    }
    case DataType::Type::kUint32:
    case DataType::Type::kUint64:
    case DataType::Type::kVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::InvokeRuntime(QuickEntrypointEnum entrypoint,
                                       HInstruction* instruction,
                                       SlowPathCode* slow_path) {
  ValidateInvokeRuntime(entrypoint, instruction, slow_path);

  ThreadOffset64 entrypoint_offset = GetThreadOffset<kArm64PointerSize>(entrypoint);
  // Reduce code size for AOT by using shared trampolines for slow path runtime calls across the
  // entire oat file. This adds an extra branch and we do not want to slow down the main path.
  // For JIT, thunk sharing is per-method, so the gains would be smaller or even negative.
  if (slow_path == nullptr || GetCompilerOptions().IsJitCompiler()) {
    __ Ldr(lr, MemOperand(tr, entrypoint_offset.Int32Value()));
    // Ensure the pc position is recorded immediately after the `blr` instruction.
    ExactAssemblyScope eas(GetVIXLAssembler(), kInstructionSize, CodeBufferCheckScope::kExactSize);
    __ blr(lr);
    if (EntrypointRequiresStackMap(entrypoint)) {
      RecordPcInfo(instruction, slow_path);
    }
  } else {
    // Ensure the pc position is recorded immediately after the `bl` instruction.
    ExactAssemblyScope eas(GetVIXLAssembler(), kInstructionSize, CodeBufferCheckScope::kExactSize);
    EmitEntrypointThunkCall(entrypoint_offset);
    if (EntrypointRequiresStackMap(entrypoint)) {
      RecordPcInfo(instruction, slow_path);
    }
  }
}

void CodeGeneratorARM64::InvokeRuntimeWithoutRecordingPcInfo(int32_t entry_point_offset,
                                                             HInstruction* instruction,
                                                             SlowPathCode* slow_path) {
  ValidateInvokeRuntimeWithoutRecordingPcInfo(instruction, slow_path);
  __ Ldr(lr, MemOperand(tr, entry_point_offset));
  __ Blr(lr);
}

void InstructionCodeGeneratorARM64::GenerateClassInitializationCheck(SlowPathCodeARM64* slow_path,
                                                                     Register class_reg) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp = temps.AcquireW();

  // CMP (immediate) is limited to imm12 or imm12<<12, so we would need to materialize
  // the constant 0xf0000000 for comparison with the full 32-bit field. To reduce the code
  // size, load only the high byte of the field and compare with 0xf0.
  // Note: The same code size could be achieved with LDR+MNV(asr #24)+CBNZ but benchmarks
  // show that this pattern is slower (tested on little cores).
  __ Ldrb(temp, HeapOperand(class_reg, kClassStatusByteOffset));
  __ Cmp(temp, kShiftedVisiblyInitializedValue);
  __ B(lo, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void InstructionCodeGeneratorARM64::GenerateBitstringTypeCheckCompare(
    HTypeCheckInstruction* check, vixl::aarch64::Register temp) {
  uint32_t path_to_root = check->GetBitstringPathToRoot();
  uint32_t mask = check->GetBitstringMask();
  DCHECK(IsPowerOfTwo(mask + 1));
  size_t mask_bits = WhichPowerOf2(mask + 1);

  if (mask_bits == 16u) {
    // Load only the bitstring part of the status word.
    __ Ldrh(temp, HeapOperand(temp, mirror::Class::StatusOffset()));
  } else {
    // /* uint32_t */ temp = temp->status_
    __ Ldr(temp, HeapOperand(temp, mirror::Class::StatusOffset()));
    // Extract the bitstring bits.
    __ Ubfx(temp, temp, 0, mask_bits);
  }
  // Compare the bitstring bits to `path_to_root`.
  __ Cmp(temp, path_to_root);
}

void CodeGeneratorARM64::GenerateMemoryBarrier(MemBarrierKind kind) {
  BarrierType type = BarrierAll;

  switch (kind) {
    case MemBarrierKind::kAnyAny:
    case MemBarrierKind::kAnyStore: {
      type = BarrierAll;
      break;
    }
    case MemBarrierKind::kLoadAny: {
      type = BarrierReads;
      break;
    }
    case MemBarrierKind::kStoreStore: {
      type = BarrierWrites;
      break;
    }
    default:
      LOG(FATAL) << "Unexpected memory barrier " << kind;
  }
  __ Dmb(InnerShareable, type);
}

bool CodeGeneratorARM64::CanUseImplicitSuspendCheck() const {
  // Use implicit suspend checks if requested in compiler options unless there are SIMD
  // instructions in the graph. The implicit suspend check saves all FP registers as
  // 64-bit (in line with the calling convention) but SIMD instructions can use 128-bit
  // registers, so they need to be saved in an explicit slow path.
  return GetCompilerOptions().GetImplicitSuspendChecks() && !GetGraph()->HasSIMD();
}

void InstructionCodeGeneratorARM64::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                         HBasicBlock* successor) {
  if (instruction->IsNoOp()) {
    if (successor != nullptr) {
      __ B(codegen_->GetLabelOf(successor));
    }
    return;
  }

  if (codegen_->CanUseImplicitSuspendCheck()) {
    __ Ldr(kImplicitSuspendCheckRegister, MemOperand(kImplicitSuspendCheckRegister));
    codegen_->RecordPcInfo(instruction);
    if (successor != nullptr) {
      __ B(codegen_->GetLabelOf(successor));
    }
    return;
  }

  SuspendCheckSlowPathARM64* slow_path =
      down_cast<SuspendCheckSlowPathARM64*>(instruction->GetSlowPath());
  if (slow_path == nullptr) {
    slow_path =
        new (codegen_->GetScopedAllocator()) SuspendCheckSlowPathARM64(instruction, successor);
    instruction->SetSlowPath(slow_path);
    codegen_->AddSlowPath(slow_path);
    if (successor != nullptr) {
      DCHECK(successor->IsLoopHeader());
    }
  } else {
    DCHECK_EQ(slow_path->GetSuccessor(), successor);
  }

  UseScratchRegisterScope temps(codegen_->GetVIXLAssembler());
  Register temp = temps.AcquireW();

  __ Ldr(temp, MemOperand(tr, Thread::ThreadFlagsOffset<kArm64PointerSize>().SizeValue()));
  __ Tst(temp, Thread::SuspendOrCheckpointRequestFlags());
  if (successor == nullptr) {
    __ B(ne, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ B(eq, codegen_->GetLabelOf(successor));
    __ B(slow_path->GetEntryLabel());
    // slow_path will return to GetLabelOf(successor).
  }
}

InstructionCodeGeneratorARM64::InstructionCodeGeneratorARM64(HGraph* graph,
                                                             CodeGeneratorARM64* codegen)
      : InstructionCodeGenerator(graph, codegen),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void LocationsBuilderARM64::HandleBinaryOp(HBinaryOperation* instr) {
  DCHECK_EQ(instr->InputCount(), 2U);
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instr);
  DataType::Type type = instr->GetResultType();
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, ARM64EncodableConstantOrRegister(instr->InputAt(1), instr));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected " << instr->DebugName() << " type " << type;
  }
}

void LocationsBuilderARM64::HandleFieldGet(HInstruction* instruction,
                                           const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  bool object_field_get_with_read_barrier =
      (instruction->GetType() == DataType::Type::kReference) && codegen_->EmitReadBarrier();
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction,
                                                       object_field_get_with_read_barrier
                                                           ? LocationSummary::kCallOnSlowPath
                                                           : LocationSummary::kNoCall);
  if (object_field_get_with_read_barrier && kUseBakerReadBarrier) {
    locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
    // We need a temporary register for the read barrier load in
    // CodeGeneratorARM64::GenerateFieldLoadWithBakerReadBarrier()
    // only if the field is volatile or the offset is too big.
    if (field_info.IsVolatile() ||
        field_info.GetFieldOffset().Uint32Value() >= kReferenceLoadMinFarOffset) {
      locations->AddTemp(FixedTempLocation());
    }
  }
  // Input for object receiver.
  locations->SetInAt(0, Location::RequiresRegister());
  if (DataType::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister());
  } else {
    // The output overlaps for an object field get for non-Baker read barriers: we do not want
    // the load to overwrite the object's location, as we need it to emit the read barrier.
    // Baker read barrier implementation with introspection does not have this restriction.
    bool overlap = object_field_get_with_read_barrier && !kUseBakerReadBarrier;
    locations->SetOut(Location::RequiresRegister(),
                      overlap ? Location::kOutputOverlap : Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::HandleFieldGet(HInstruction* instruction,
                                                   const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());
  LocationSummary* locations = instruction->GetLocations();
  uint32_t receiver_input = 0;
  Location base_loc = locations->InAt(receiver_input);
  Location out = locations->Out();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();
  DCHECK_EQ(DataType::Size(field_info.GetFieldType()), DataType::Size(instruction->GetType()));
  DataType::Type load_type = instruction->GetType();
  MemOperand field =
      HeapOperand(InputRegisterAt(instruction, receiver_input), field_info.GetFieldOffset());

  if (load_type == DataType::Type::kReference && codegen_->EmitBakerReadBarrier()) {
    // Object FieldGet with Baker's read barrier case.
    // /* HeapReference<Object> */ out = *(base + offset)
    Register base = RegisterFrom(base_loc, DataType::Type::kReference);
    Location maybe_temp =
        (locations->GetTempCount() != 0) ? locations->GetTemp(0) : Location::NoLocation();
    // Note that potential implicit null checks are handled in this
    // CodeGeneratorARM64::GenerateFieldLoadWithBakerReadBarrier call.
    codegen_->GenerateFieldLoadWithBakerReadBarrier(
        instruction,
        out,
        base,
        offset,
        maybe_temp,
        /* needs_null_check= */ true,
        field_info.IsVolatile());
  } else {
    // General case.
    if (field_info.IsVolatile()) {
      // Note that a potential implicit null check is handled in this
      // CodeGeneratorARM64::LoadAcquire call.
      // NB: LoadAcquire will record the pc info if needed.
      codegen_->LoadAcquire(instruction,
                            load_type,
                            OutputCPURegister(instruction),
                            field,
                            /* needs_null_check= */ true);
    } else {
      // Ensure that between load and MaybeRecordImplicitNullCheck there are no pools emitted.
      EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
      codegen_->Load(load_type, OutputCPURegister(instruction), field);
      codegen_->MaybeRecordImplicitNullCheck(instruction);
    }
    if (load_type == DataType::Type::kReference) {
      // If read barriers are enabled, emit read barriers other than
      // Baker's using a slow path (and also unpoison the loaded
      // reference, if heap poisoning is enabled).
      codegen_->MaybeGenerateReadBarrierSlow(instruction, out, out, base_loc, offset);
    }
  }
}

void LocationsBuilderARM64::HandleFieldSet(HInstruction* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  HInstruction* value = instruction->InputAt(1);
  if (IsZeroBitPattern(value)) {
    locations->SetInAt(1, Location::ConstantLocation(value));
  } else if (DataType::IsFloatingPointType(value->GetType())) {
    locations->SetInAt(1, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::HandleFieldSet(HInstruction* instruction,
                                                   const FieldInfo& field_info,
                                                   bool value_can_be_null,
                                                   WriteBarrierKind write_barrier_kind) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  Register obj = InputRegisterAt(instruction, 0);
  CPURegister value = InputCPURegisterOrZeroRegAt(instruction, 1);
  CPURegister source = value;
  Offset offset = field_info.GetFieldOffset();
  DataType::Type field_type = field_info.GetFieldType();
  {
    // We use a block to end the scratch scope before the write barrier, thus
    // freeing the temporary registers so they can be used in `MarkGCCard`.
    UseScratchRegisterScope temps(GetVIXLAssembler());

    if (kPoisonHeapReferences && field_type == DataType::Type::kReference) {
      DCHECK(value.IsW());
      Register temp = temps.AcquireW();
      __ Mov(temp, value.W());
      GetAssembler()->PoisonHeapReference(temp.W());
      source = temp;
    }

    if (field_info.IsVolatile()) {
      codegen_->StoreRelease(
          instruction, field_type, source, HeapOperand(obj, offset), /* needs_null_check= */ true);
    } else {
      // Ensure that between store and MaybeRecordImplicitNullCheck there are no pools emitted.
      EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
      codegen_->Store(field_type, source, HeapOperand(obj, offset));
      codegen_->MaybeRecordImplicitNullCheck(instruction);
    }
  }

  const bool needs_write_barrier =
      codegen_->StoreNeedsWriteBarrier(field_type, instruction->InputAt(1), write_barrier_kind);

  if (needs_write_barrier) {
    DCHECK_IMPLIES(Register(value).IsZero(),
                   write_barrier_kind == WriteBarrierKind::kEmitBeingReliedOn);
    codegen_->MaybeMarkGCCard(
        obj,
        Register(value),
        value_can_be_null && write_barrier_kind == WriteBarrierKind::kEmitNotBeingReliedOn);
  } else if (codegen_->ShouldCheckGCCard(field_type, instruction->InputAt(1), write_barrier_kind)) {
    codegen_->CheckGCCardIsValid(obj);
  }
}

void InstructionCodeGeneratorARM64::HandleBinaryOp(HBinaryOperation* instr) {
  DataType::Type type = instr->GetType();

  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      Register dst = OutputRegister(instr);
      Register lhs = InputRegisterAt(instr, 0);
      Operand rhs = InputOperandAt(instr, 1);
      if (instr->IsAdd()) {
        __ Add(dst, lhs, rhs);
      } else if (instr->IsAnd()) {
        __ And(dst, lhs, rhs);
      } else if (instr->IsOr()) {
        __ Orr(dst, lhs, rhs);
      } else if (instr->IsSub()) {
        __ Sub(dst, lhs, rhs);
      } else if (instr->IsRol()) {
        if (rhs.IsImmediate()) {
          uint32_t shift = (-rhs.GetImmediate()) & (lhs.GetSizeInBits() - 1);
          __ Ror(dst, lhs, shift);
        } else {
          UseScratchRegisterScope temps(GetVIXLAssembler());

          // Ensure shift distance is in the same size register as the result. If
          // we are rotating a long and the shift comes in a w register originally,
          // we don't need to sxtw for use as an x since the shift distances are
          // all & reg_bits - 1.
          Register right = RegisterFrom(instr->GetLocations()->InAt(1), type);
          Register negated = (type == DataType::Type::kInt32) ? temps.AcquireW() : temps.AcquireX();
          __ Neg(negated, right);
          __ Ror(dst, lhs, negated);
        }
      } else if (instr->IsRor()) {
        if (rhs.IsImmediate()) {
          uint32_t shift = rhs.GetImmediate() & (lhs.GetSizeInBits() - 1);
          __ Ror(dst, lhs, shift);
        } else {
          // Ensure shift distance is in the same size register as the result. If
          // we are rotating a long and the shift comes in a w register originally,
          // we don't need to sxtw for use as an x since the shift distances are
          // all & reg_bits - 1.
          __ Ror(dst, lhs, RegisterFrom(instr->GetLocations()->InAt(1), type));
        }
      } else if (instr->IsMin() || instr->IsMax()) {
          __ Cmp(lhs, rhs);
          __ Csel(dst, lhs, rhs, instr->IsMin() ? lt : gt);
      } else {
        DCHECK(instr->IsXor());
        __ Eor(dst, lhs, rhs);
      }
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      VRegister dst = OutputFPRegister(instr);
      VRegister lhs = InputFPRegisterAt(instr, 0);
      VRegister rhs = InputFPRegisterAt(instr, 1);
      if (instr->IsAdd()) {
        __ Fadd(dst, lhs, rhs);
      } else if (instr->IsSub()) {
        __ Fsub(dst, lhs, rhs);
      } else if (instr->IsMin()) {
        __ Fmin(dst, lhs, rhs);
      } else if (instr->IsMax()) {
        __ Fmax(dst, lhs, rhs);
      } else {
        LOG(FATAL) << "Unexpected floating-point binary operation";
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected binary operation type " << type;
  }
}

void LocationsBuilderARM64::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr());

  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instr);
  DataType::Type type = instr->GetResultType();
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instr->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift type " << type;
  }
}

void InstructionCodeGeneratorARM64::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr());

  DataType::Type type = instr->GetType();
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      Register dst = OutputRegister(instr);
      Register lhs = InputRegisterAt(instr, 0);
      Operand rhs = InputOperandAt(instr, 1);
      if (rhs.IsImmediate()) {
        uint32_t shift_value = rhs.GetImmediate() &
            (type == DataType::Type::kInt32 ? kMaxIntShiftDistance : kMaxLongShiftDistance);
        if (instr->IsShl()) {
          __ Lsl(dst, lhs, shift_value);
        } else if (instr->IsShr()) {
          __ Asr(dst, lhs, shift_value);
        } else {
          __ Lsr(dst, lhs, shift_value);
        }
      } else {
        Register rhs_reg = dst.IsX() ? rhs.GetRegister().X() : rhs.GetRegister().W();

        if (instr->IsShl()) {
          __ Lsl(dst, lhs, rhs_reg);
        } else if (instr->IsShr()) {
          __ Asr(dst, lhs, rhs_reg);
        } else {
          __ Lsr(dst, lhs, rhs_reg);
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift operation type " << type;
  }
}

void LocationsBuilderARM64::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitBitwiseNegatedRight(HBitwiseNegatedRight* instr) {
  DCHECK(DataType::IsIntegralType(instr->GetType())) << instr->GetType();
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instr);
  locations->SetInAt(0, Location::RequiresRegister());
  // There is no immediate variant of negated bitwise instructions in AArch64.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitBitwiseNegatedRight(HBitwiseNegatedRight* instr) {
  Register dst = OutputRegister(instr);
  Register lhs = InputRegisterAt(instr, 0);
  Register rhs = InputRegisterAt(instr, 1);

  switch (instr->GetOpKind()) {
    case HInstruction::kAnd:
      __ Bic(dst, lhs, rhs);
      break;
    case HInstruction::kOr:
      __ Orn(dst, lhs, rhs);
      break;
    case HInstruction::kXor:
      __ Eon(dst, lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unreachable";
  }
}

void LocationsBuilderARM64::VisitDataProcWithShifterOp(
    HDataProcWithShifterOp* instruction) {
  DCHECK(instruction->GetType() == DataType::Type::kInt32 ||
         instruction->GetType() == DataType::Type::kInt64);
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  if (instruction->GetInstrKind() == HInstruction::kNeg) {
    locations->SetInAt(0, Location::ConstantLocation(instruction->InputAt(0)));
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
  }
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitDataProcWithShifterOp(
    HDataProcWithShifterOp* instruction) {
  DataType::Type type = instruction->GetType();
  HInstruction::InstructionKind kind = instruction->GetInstrKind();
  DCHECK(type == DataType::Type::kInt32 || type == DataType::Type::kInt64);
  Register out = OutputRegister(instruction);
  Register left;
  if (kind != HInstruction::kNeg) {
    left = InputRegisterAt(instruction, 0);
  }
  // If this `HDataProcWithShifterOp` was created by merging a type conversion as the
  // shifter operand operation, the IR generating `right_reg` (input to the type
  // conversion) can have a different type from the current instruction's type,
  // so we manually indicate the type.
  Register right_reg = RegisterFrom(instruction->GetLocations()->InAt(1), type);
  Operand right_operand(0);

  HDataProcWithShifterOp::OpKind op_kind = instruction->GetOpKind();
  if (HDataProcWithShifterOp::IsExtensionOp(op_kind)) {
    right_operand = Operand(right_reg, helpers::ExtendFromOpKind(op_kind));
  } else {
    right_operand = Operand(right_reg,
                            helpers::ShiftFromOpKind(op_kind),
                            instruction->GetShiftAmount());
  }

  // Logical binary operations do not support extension operations in the
  // operand. Note that VIXL would still manage if it was passed by generating
  // the extension as a separate instruction.
  // `HNeg` also does not support extension. See comments in `ShifterOperandSupportsExtension()`.
  DCHECK_IMPLIES(right_operand.IsExtendedRegister(),
                 kind != HInstruction::kAnd && kind != HInstruction::kOr &&
                     kind != HInstruction::kXor && kind != HInstruction::kNeg);
  switch (kind) {
    case HInstruction::kAdd:
      __ Add(out, left, right_operand);
      break;
    case HInstruction::kAnd:
      __ And(out, left, right_operand);
      break;
    case HInstruction::kNeg:
      DCHECK(instruction->InputAt(0)->AsConstant()->IsArithmeticZero());
      __ Neg(out, right_operand);
      break;
    case HInstruction::kOr:
      __ Orr(out, left, right_operand);
      break;
    case HInstruction::kSub:
      __ Sub(out, left, right_operand);
      break;
    case HInstruction::kXor:
      __ Eor(out, left, right_operand);
      break;
    default:
      LOG(FATAL) << "Unexpected operation kind: " << kind;
      UNREACHABLE();
  }
}

void LocationsBuilderARM64::VisitIntermediateAddress(HIntermediateAddress* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, ARM64EncodableConstantOrRegister(instruction->GetOffset(), instruction));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitIntermediateAddress(HIntermediateAddress* instruction) {
  __ Add(OutputRegister(instruction),
         InputRegisterAt(instruction, 0),
         Operand(InputOperandAt(instruction, 1)));
}

void LocationsBuilderARM64::VisitIntermediateAddressIndex(HIntermediateAddressIndex* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);

  HIntConstant* shift = instruction->GetShift()->AsIntConstant();

  locations->SetInAt(0, Location::RequiresRegister());
  // For byte case we don't need to shift the index variable so we can encode the data offset into
  // ADD instruction. For other cases we prefer the data_offset to be in register; that will hoist
  // data offset constant generation out of the loop and reduce the critical path length in the
  // loop.
  locations->SetInAt(1, shift->GetValue() == 0
                        ? Location::ConstantLocation(instruction->GetOffset())
                        : Location::RequiresRegister());
  locations->SetInAt(2, Location::ConstantLocation(shift));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitIntermediateAddressIndex(
    HIntermediateAddressIndex* instruction) {
  Register index_reg = InputRegisterAt(instruction, 0);
  uint32_t shift = Int64FromLocation(instruction->GetLocations()->InAt(2));
  uint32_t offset = instruction->GetOffset()->AsIntConstant()->GetValue();

  if (shift == 0) {
    __ Add(OutputRegister(instruction), index_reg, offset);
  } else {
    Register offset_reg = InputRegisterAt(instruction, 1);
    __ Add(OutputRegister(instruction), offset_reg, Operand(index_reg, LSL, shift));
  }
}

void LocationsBuilderARM64::VisitMultiplyAccumulate(HMultiplyAccumulate* instr) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instr, LocationSummary::kNoCall);
  HInstruction* accumulator = instr->InputAt(HMultiplyAccumulate::kInputAccumulatorIndex);
  if (instr->GetOpKind() == HInstruction::kSub &&
      accumulator->IsConstant() &&
      accumulator->AsConstant()->IsArithmeticZero()) {
    // Don't allocate register for Mneg instruction.
  } else {
    locations->SetInAt(HMultiplyAccumulate::kInputAccumulatorIndex,
                       Location::RequiresRegister());
  }
  locations->SetInAt(HMultiplyAccumulate::kInputMulLeftIndex, Location::RequiresRegister());
  locations->SetInAt(HMultiplyAccumulate::kInputMulRightIndex, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitMultiplyAccumulate(HMultiplyAccumulate* instr) {
  Register res = OutputRegister(instr);
  Register mul_left = InputRegisterAt(instr, HMultiplyAccumulate::kInputMulLeftIndex);
  Register mul_right = InputRegisterAt(instr, HMultiplyAccumulate::kInputMulRightIndex);

  // Avoid emitting code that could trigger Cortex A53's erratum 835769.
  // This fixup should be carried out for all multiply-accumulate instructions:
  // madd, msub, smaddl, smsubl, umaddl and umsubl.
  if (instr->GetType() == DataType::Type::kInt64 &&
      codegen_->GetInstructionSetFeatures().NeedFixCortexA53_835769()) {
    MacroAssembler* masm = down_cast<CodeGeneratorARM64*>(codegen_)->GetVIXLAssembler();
    ptrdiff_t off = masm->GetCursorOffset();
    if (off >= static_cast<ptrdiff_t>(kInstructionSize) &&
        masm->GetInstructionAt(off - static_cast<ptrdiff_t>(kInstructionSize))->IsLoadOrStore()) {
      // Make sure we emit only exactly one nop.
      ExactAssemblyScope scope(masm, kInstructionSize, CodeBufferCheckScope::kExactSize);
      __ nop();
    }
  }

  if (instr->GetOpKind() == HInstruction::kAdd) {
    Register accumulator = InputRegisterAt(instr, HMultiplyAccumulate::kInputAccumulatorIndex);
    __ Madd(res, mul_left, mul_right, accumulator);
  } else {
    DCHECK(instr->GetOpKind() == HInstruction::kSub);
    HInstruction* accum_instr = instr->InputAt(HMultiplyAccumulate::kInputAccumulatorIndex);
    if (accum_instr->IsConstant() && accum_instr->AsConstant()->IsArithmeticZero()) {
      __ Mneg(res, mul_left, mul_right);
    } else {
      Register accumulator = InputRegisterAt(instr, HMultiplyAccumulate::kInputAccumulatorIndex);
      __ Msub(res, mul_left, mul_right, accumulator);
    }
  }
}

void LocationsBuilderARM64::VisitArrayGet(HArrayGet* instruction) {
  bool object_array_get_with_read_barrier =
      (instruction->GetType() == DataType::Type::kReference) && codegen_->EmitReadBarrier();
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction,
                                                       object_array_get_with_read_barrier
                                                           ? LocationSummary::kCallOnSlowPath
                                                           : LocationSummary::kNoCall);
  if (object_array_get_with_read_barrier && kUseBakerReadBarrier) {
    locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
    if (instruction->GetIndex()->IsConstant()) {
      // Array loads with constant index are treated as field loads.
      // We need a temporary register for the read barrier load in
      // CodeGeneratorARM64::GenerateFieldLoadWithBakerReadBarrier()
      // only if the offset is too big.
      uint32_t offset = CodeGenerator::GetArrayDataOffset(instruction);
      uint32_t index = instruction->GetIndex()->AsIntConstant()->GetValue();
      offset += index << DataType::SizeShift(DataType::Type::kReference);
      if (offset >= kReferenceLoadMinFarOffset) {
        locations->AddTemp(FixedTempLocation());
      }
    } else if (!instruction->GetArray()->IsIntermediateAddress()) {
      // We need a non-scratch temporary for the array data pointer in
      // CodeGeneratorARM64::GenerateArrayLoadWithBakerReadBarrier() for the case with no
      // intermediate address.
      locations->AddTemp(Location::RequiresRegister());
    }
  }
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (DataType::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    // The output overlaps for an object array get for non-Baker read barriers: we do not want
    // the load to overwrite the object's location, as we need it to emit the read barrier.
    // Baker read barrier implementation with introspection does not have this restriction.
    bool overlap = object_array_get_with_read_barrier && !kUseBakerReadBarrier;
    locations->SetOut(Location::RequiresRegister(),
                      overlap ? Location::kOutputOverlap : Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::VisitArrayGet(HArrayGet* instruction) {
  DataType::Type type = instruction->GetType();
  Register obj = InputRegisterAt(instruction, 0);
  LocationSummary* locations = instruction->GetLocations();
  Location index = locations->InAt(1);
  Location out = locations->Out();
  uint32_t offset = CodeGenerator::GetArrayDataOffset(instruction);
  const bool maybe_compressed_char_at = mirror::kUseStringCompression &&
                                        instruction->IsStringCharAt();
  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);

  // The non-Baker read barrier instrumentation of object ArrayGet instructions
  // does not support the HIntermediateAddress instruction.
  DCHECK(!((type == DataType::Type::kReference) &&
           instruction->GetArray()->IsIntermediateAddress() &&
           codegen_->EmitNonBakerReadBarrier()));

  if (type == DataType::Type::kReference && codegen_->EmitBakerReadBarrier()) {
    // Object ArrayGet with Baker's read barrier case.
    // Note that a potential implicit null check is handled in the
    // CodeGeneratorARM64::GenerateArrayLoadWithBakerReadBarrier call.
    DCHECK(!instruction->CanDoImplicitNullCheckOn(instruction->InputAt(0)));
    if (index.IsConstant()) {
      DCHECK(!instruction->GetArray()->IsIntermediateAddress());
      // Array load with a constant index can be treated as a field load.
      offset += Int64FromLocation(index) << DataType::SizeShift(type);
      Location maybe_temp =
          (locations->GetTempCount() != 0) ? locations->GetTemp(0) : Location::NoLocation();
      codegen_->GenerateFieldLoadWithBakerReadBarrier(instruction,
                                                      out,
                                                      obj.W(),
                                                      offset,
                                                      maybe_temp,
                                                      /* needs_null_check= */ false,
                                                      /* use_load_acquire= */ false);
    } else {
      codegen_->GenerateArrayLoadWithBakerReadBarrier(
          instruction, out, obj.W(), offset, index, /* needs_null_check= */ false);
    }
  } else {
    // General case.
    MemOperand source = HeapOperand(obj);
    Register length;
    if (maybe_compressed_char_at) {
      uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
      length = temps.AcquireW();
      {
        // Ensure that between load and MaybeRecordImplicitNullCheck there are no pools emitted.
        EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);

        if (instruction->GetArray()->IsIntermediateAddress()) {
          DCHECK_LT(count_offset, offset);
          int64_t adjusted_offset =
              static_cast<int64_t>(count_offset) - static_cast<int64_t>(offset);
          // Note that `adjusted_offset` is negative, so this will be a LDUR.
          __ Ldr(length, MemOperand(obj.X(), adjusted_offset));
        } else {
          __ Ldr(length, HeapOperand(obj, count_offset));
        }
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      }
    }
    if (index.IsConstant()) {
      if (maybe_compressed_char_at) {
        vixl::aarch64::Label uncompressed_load, done;
        static_assert(static_cast<uint32_t>(mirror::StringCompressionFlag::kCompressed) == 0u,
                      "Expecting 0=compressed, 1=uncompressed");
        __ Tbnz(length.W(), 0, &uncompressed_load);
        __ Ldrb(Register(OutputCPURegister(instruction)),
                HeapOperand(obj, offset + Int64FromLocation(index)));
        __ B(&done);
        __ Bind(&uncompressed_load);
        __ Ldrh(Register(OutputCPURegister(instruction)),
                HeapOperand(obj, offset + (Int64FromLocation(index) << 1)));
        __ Bind(&done);
      } else {
        offset += Int64FromLocation(index) << DataType::SizeShift(type);
        source = HeapOperand(obj, offset);
      }
    } else {
      Register temp = temps.AcquireSameSizeAs(obj);
      if (instruction->GetArray()->IsIntermediateAddress()) {
        // We do not need to compute the intermediate address from the array: the
        // input instruction has done it already. See the comment in
        // `TryExtractArrayAccessAddress()`.
        if (kIsDebugBuild) {
          HIntermediateAddress* interm_addr = instruction->GetArray()->AsIntermediateAddress();
          DCHECK_EQ(interm_addr->GetOffset()->AsIntConstant()->GetValueAsUint64(), offset);
        }
        temp = obj;
      } else {
        __ Add(temp, obj, offset);
      }
      if (maybe_compressed_char_at) {
        vixl::aarch64::Label uncompressed_load, done;
        static_assert(static_cast<uint32_t>(mirror::StringCompressionFlag::kCompressed) == 0u,
                      "Expecting 0=compressed, 1=uncompressed");
        __ Tbnz(length.W(), 0, &uncompressed_load);
        __ Ldrb(Register(OutputCPURegister(instruction)),
                HeapOperand(temp, XRegisterFrom(index), LSL, 0));
        __ B(&done);
        __ Bind(&uncompressed_load);
        __ Ldrh(Register(OutputCPURegister(instruction)),
                HeapOperand(temp, XRegisterFrom(index), LSL, 1));
        __ Bind(&done);
      } else {
        source = HeapOperand(temp, XRegisterFrom(index), LSL, DataType::SizeShift(type));
      }
    }
    if (!maybe_compressed_char_at) {
      // Ensure that between load and MaybeRecordImplicitNullCheck there are no pools emitted.
      EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
      codegen_->Load(type, OutputCPURegister(instruction), source);
      codegen_->MaybeRecordImplicitNullCheck(instruction);
    }

    if (type == DataType::Type::kReference) {
      static_assert(
          sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
          "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
      Location obj_loc = locations->InAt(0);
      if (index.IsConstant()) {
        codegen_->MaybeGenerateReadBarrierSlow(instruction, out, out, obj_loc, offset);
      } else {
        codegen_->MaybeGenerateReadBarrierSlow(instruction, out, out, obj_loc, offset, index);
      }
    }
  }
}

void LocationsBuilderARM64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitArrayLength(HArrayLength* instruction) {
  uint32_t offset = CodeGenerator::GetArrayLengthOffset(instruction);
  vixl::aarch64::Register out = OutputRegister(instruction);
  {
    // Ensure that between load and MaybeRecordImplicitNullCheck there are no pools emitted.
    EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
    __ Ldr(out, HeapOperand(InputRegisterAt(instruction, 0), offset));
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }
  // Mask out compression flag from String's array length.
  if (mirror::kUseStringCompression && instruction->IsStringLength()) {
    __ Lsr(out.W(), out.W(), 1u);
  }
}

void LocationsBuilderARM64::VisitArraySet(HArraySet* instruction) {
  DataType::Type value_type = instruction->GetComponentType();

  bool needs_type_check = instruction->NeedsTypeCheck();
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction,
      needs_type_check ? LocationSummary::kCallOnSlowPath : LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->GetIndex()));
  HInstruction* value = instruction->GetValue();
  if (IsZeroBitPattern(value)) {
    locations->SetInAt(2, Location::ConstantLocation(value));
  } else if (DataType::IsFloatingPointType(value_type)) {
    locations->SetInAt(2, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(2, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitArraySet(HArraySet* instruction) {
  DataType::Type value_type = instruction->GetComponentType();
  LocationSummary* locations = instruction->GetLocations();
  bool needs_type_check = instruction->NeedsTypeCheck();
  const WriteBarrierKind write_barrier_kind = instruction->GetWriteBarrierKind();
  bool needs_write_barrier =
      codegen_->StoreNeedsWriteBarrier(value_type, instruction->GetValue(), write_barrier_kind);

  Register array = InputRegisterAt(instruction, 0);
  CPURegister value = InputCPURegisterOrZeroRegAt(instruction, 2);
  CPURegister source = value;
  Location index = locations->InAt(1);
  size_t offset = mirror::Array::DataOffset(DataType::Size(value_type)).Uint32Value();
  MemOperand destination = HeapOperand(array);
  MacroAssembler* masm = GetVIXLAssembler();

  if (!needs_write_barrier) {
    if (codegen_->ShouldCheckGCCard(value_type, instruction->GetValue(), write_barrier_kind)) {
      codegen_->CheckGCCardIsValid(array);
    }

    DCHECK(!needs_type_check);
    UseScratchRegisterScope temps(masm);
    if (index.IsConstant()) {
      offset += Int64FromLocation(index) << DataType::SizeShift(value_type);
      destination = HeapOperand(array, offset);
    } else {
      Register temp_dest = temps.AcquireSameSizeAs(array);
      if (instruction->GetArray()->IsIntermediateAddress()) {
        // We do not need to compute the intermediate address from the array: the
        // input instruction has done it already. See the comment in
        // `TryExtractArrayAccessAddress()`.
        if (kIsDebugBuild) {
          HIntermediateAddress* interm_addr = instruction->GetArray()->AsIntermediateAddress();
          DCHECK(interm_addr->GetOffset()->AsIntConstant()->GetValueAsUint64() == offset);
        }
        temp_dest = array;
      } else {
        __ Add(temp_dest, array, offset);
      }
      destination = HeapOperand(temp_dest,
                                XRegisterFrom(index),
                                LSL,
                                DataType::SizeShift(value_type));
    }

    if (kPoisonHeapReferences && value_type == DataType::Type::kReference) {
      DCHECK(value.IsW());
      Register temp_src = temps.AcquireW();
      __ Mov(temp_src, value.W());
      GetAssembler()->PoisonHeapReference(temp_src.W());
      source = temp_src;
    }

    {
      // Ensure that between store and MaybeRecordImplicitNullCheck there are no pools emitted.
      EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
      codegen_->Store(value_type, source, destination);
      codegen_->MaybeRecordImplicitNullCheck(instruction);
    }
  } else {
    DCHECK(!instruction->GetArray()->IsIntermediateAddress());
    bool can_value_be_null = true;
    // The WriteBarrierKind::kEmitNotBeingReliedOn case is able to skip the write barrier when its
    // value is null (without an extra CompareAndBranchIfZero since we already checked if the
    // value is null for the type check).
    bool skip_marking_gc_card = false;
    SlowPathCodeARM64* slow_path = nullptr;
    vixl::aarch64::Label skip_writing_card;
    if (!Register(value).IsZero()) {
      can_value_be_null = instruction->GetValueCanBeNull();
      skip_marking_gc_card =
          can_value_be_null && write_barrier_kind == WriteBarrierKind::kEmitNotBeingReliedOn;
      vixl::aarch64::Label do_store;
      if (can_value_be_null) {
        if (skip_marking_gc_card) {
          __ Cbz(Register(value), &skip_writing_card);
        } else {
          __ Cbz(Register(value), &do_store);
        }
      }

      if (needs_type_check) {
        slow_path = new (codegen_->GetScopedAllocator()) ArraySetSlowPathARM64(instruction);
        codegen_->AddSlowPath(slow_path);

        const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
        const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
        const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();

        UseScratchRegisterScope temps(masm);
        Register temp = temps.AcquireSameSizeAs(array);
        Register temp2 = temps.AcquireSameSizeAs(array);

        // Note that when Baker read barriers are enabled, the type
        // checks are performed without read barriers.  This is fine,
        // even in the case where a class object is in the from-space
        // after the flip, as a comparison involving such a type would
        // not produce a false positive; it may of course produce a
        // false negative, in which case we would take the ArraySet
        // slow path.

        // /* HeapReference<Class> */ temp = array->klass_
        {
          // Ensure that between load and MaybeRecordImplicitNullCheck there are no pools emitted.
          EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
          __ Ldr(temp, HeapOperand(array, class_offset));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
        }
        GetAssembler()->MaybeUnpoisonHeapReference(temp);

        // /* HeapReference<Class> */ temp = temp->component_type_
        __ Ldr(temp, HeapOperand(temp, component_offset));
        // /* HeapReference<Class> */ temp2 = value->klass_
        __ Ldr(temp2, HeapOperand(Register(value), class_offset));
        // If heap poisoning is enabled, no need to unpoison `temp`
        // nor `temp2`, as we are comparing two poisoned references.
        __ Cmp(temp, temp2);

        if (instruction->StaticTypeOfArrayIsObjectArray()) {
          vixl::aarch64::Label do_put;
          __ B(eq, &do_put);
          // If heap poisoning is enabled, the `temp` reference has
          // not been unpoisoned yet; unpoison it now.
          GetAssembler()->MaybeUnpoisonHeapReference(temp);

          // /* HeapReference<Class> */ temp = temp->super_class_
          __ Ldr(temp, HeapOperand(temp, super_offset));
          // If heap poisoning is enabled, no need to unpoison
          // `temp`, as we are comparing against null below.
          __ Cbnz(temp, slow_path->GetEntryLabel());
          __ Bind(&do_put);
        } else {
          __ B(ne, slow_path->GetEntryLabel());
        }
      }

      if (can_value_be_null && !skip_marking_gc_card) {
        DCHECK(do_store.IsLinked());
        __ Bind(&do_store);
      }
    }

    DCHECK_NE(write_barrier_kind, WriteBarrierKind::kDontEmit);
    DCHECK_IMPLIES(Register(value).IsZero(),
                   write_barrier_kind == WriteBarrierKind::kEmitBeingReliedOn);
    codegen_->MarkGCCard(array);

    if (skip_marking_gc_card) {
      // Note that we don't check that the GC card is valid as it can be correctly clean.
      DCHECK(skip_writing_card.IsLinked());
      __ Bind(&skip_writing_card);
    }

    UseScratchRegisterScope temps(masm);
    if (kPoisonHeapReferences) {
      DCHECK(value.IsW());
      Register temp_source = temps.AcquireW();
      __ Mov(temp_source, value.W());
      GetAssembler()->PoisonHeapReference(temp_source);
      source = temp_source;
    }

    if (index.IsConstant()) {
      offset += Int64FromLocation(index) << DataType::SizeShift(value_type);
      destination = HeapOperand(array, offset);
    } else {
      Register temp_base = temps.AcquireSameSizeAs(array);
      __ Add(temp_base, array, offset);
      destination = HeapOperand(temp_base,
                                XRegisterFrom(index),
                                LSL,
                                DataType::SizeShift(value_type));
    }

    {
      // Ensure that between store and MaybeRecordImplicitNullCheck there are no pools emitted.
      EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
      __ Str(source, destination);

      if (can_value_be_null || !needs_type_check) {
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      }
    }

    if (slow_path != nullptr) {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  RegisterSet caller_saves = RegisterSet::Empty();
  InvokeRuntimeCallingConvention calling_convention;
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(0).GetCode()));
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(1).GetCode()));
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction, caller_saves);

  // If both index and length are constant, we can check the bounds statically and
  // generate code accordingly. We want to make sure we generate constant locations
  // in that case, regardless of whether they are encodable in the comparison or not.
  HInstruction* index = instruction->InputAt(0);
  HInstruction* length = instruction->InputAt(1);
  bool both_const = index->IsConstant() && length->IsConstant();
  locations->SetInAt(0, both_const
      ? Location::ConstantLocation(index)
      : ARM64EncodableConstantOrRegister(index, instruction));
  locations->SetInAt(1, both_const
      ? Location::ConstantLocation(length)
      : ARM64EncodableConstantOrRegister(length, instruction));
}

void InstructionCodeGeneratorARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location index_loc = locations->InAt(0);
  Location length_loc = locations->InAt(1);

  int cmp_first_input = 0;
  int cmp_second_input = 1;
  Condition cond = hs;

  if (index_loc.IsConstant()) {
    int64_t index = Int64FromLocation(index_loc);
    if (length_loc.IsConstant()) {
      int64_t length = Int64FromLocation(length_loc);
      if (index < 0 || index >= length) {
        BoundsCheckSlowPathARM64* slow_path =
            new (codegen_->GetScopedAllocator()) BoundsCheckSlowPathARM64(instruction);
        codegen_->AddSlowPath(slow_path);
        __ B(slow_path->GetEntryLabel());
      } else {
        // BCE will remove the bounds check if we are guaranteed to pass.
        // However, some optimization after BCE may have generated this, and we should not
        // generate a bounds check if it is a valid range.
      }
      return;
    }
    // Only the index is constant: change the order of the operands and commute the condition
    // so we can use an immediate constant for the index (only the second input to a cmp
    // instruction can be an immediate).
    cmp_first_input = 1;
    cmp_second_input = 0;
    cond = ls;
  }
  BoundsCheckSlowPathARM64* slow_path =
      new (codegen_->GetScopedAllocator()) BoundsCheckSlowPathARM64(instruction);
  __ Cmp(InputRegisterAt(instruction, cmp_first_input),
         InputOperandAt(instruction, cmp_second_input));
  codegen_->AddSlowPath(slow_path);
  __ B(slow_path->GetEntryLabel(), cond);
}

void LocationsBuilderARM64::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
  // Rely on the type initialization to save everything we need.
  locations->SetCustomSlowPathCallerSaves(OneRegInReferenceOutSaveEverythingCallerSaves());
}

void InstructionCodeGeneratorARM64::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class is not null.
  SlowPathCodeARM64* slow_path =
      new (codegen_->GetScopedAllocator()) LoadClassSlowPathARM64(check->GetLoadClass(), check);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path, InputRegisterAt(check, 0));
}

static bool IsFloatingPointZeroConstant(HInstruction* inst) {
  return (inst->IsFloatConstant() && (inst->AsFloatConstant()->IsArithmeticZero()))
      || (inst->IsDoubleConstant() && (inst->AsDoubleConstant()->IsArithmeticZero()));
}

void InstructionCodeGeneratorARM64::GenerateFcmp(HInstruction* instruction) {
  VRegister lhs_reg = InputFPRegisterAt(instruction, 0);
  Location rhs_loc = instruction->GetLocations()->InAt(1);
  if (rhs_loc.IsConstant()) {
    // 0.0 is the only immediate that can be encoded directly in
    // an FCMP instruction.
    //
    // Both the JLS (section 15.20.1) and the JVMS (section 6.5)
    // specify that in a floating-point comparison, positive zero
    // and negative zero are considered equal, so we can use the
    // literal 0.0 for both cases here.
    //
    // Note however that some methods (Float.equal, Float.compare,
    // Float.compareTo, Double.equal, Double.compare,
    // Double.compareTo, Math.max, Math.min, StrictMath.max,
    // StrictMath.min) consider 0.0 to be (strictly) greater than
    // -0.0. So if we ever translate calls to these methods into a
    // HCompare instruction, we must handle the -0.0 case with
    // care here.
    DCHECK(IsFloatingPointZeroConstant(rhs_loc.GetConstant()));
    __ Fcmp(lhs_reg, 0.0);
  } else {
    __ Fcmp(lhs_reg, InputFPRegisterAt(instruction, 1));
  }
}

void LocationsBuilderARM64::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(compare, LocationSummary::kNoCall);
  DataType::Type compare_type = compare->GetComparisonType();
  HInstruction* rhs = compare->InputAt(1);
  switch (compare_type) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kUint32:
    case DataType::Type::kInt64:
    case DataType::Type::kUint64: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, ARM64EncodableConstantOrRegister(rhs, compare));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1,
                         IsFloatingPointZeroConstant(rhs)
                             ? Location::ConstantLocation(rhs)
                             : Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << compare_type;
  }
}

void InstructionCodeGeneratorARM64::VisitCompare(HCompare* compare) {
  DataType::Type compare_type = compare->GetComparisonType();

  //  0 if: left == right
  //  1 if: left  > right
  // -1 if: left  < right
  Condition less_cond = lt;
  switch (compare_type) {
    case DataType::Type::kUint32:
    case DataType::Type::kUint64:
      less_cond = lo;
      FALLTHROUGH_INTENDED;
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      Register result = OutputRegister(compare);
      Register left = InputRegisterAt(compare, 0);
      Operand right = InputOperandAt(compare, 1);
      __ Cmp(left, right);
      __ Cset(result, ne);                 // result == +1 if NE or 0 otherwise
      __ Cneg(result, result, less_cond);  // result == -1 if LT or unchanged otherwise
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      Register result = OutputRegister(compare);
      GenerateFcmp(compare);
      __ Cset(result, ne);
      __ Cneg(result, result, ARM64FPCondition(kCondLT, compare->IsGtBias()));
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented compare type " << compare_type;
  }
}

void LocationsBuilderARM64::HandleCondition(HCondition* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);

  HInstruction* rhs = instruction->InputAt(1);
  if (DataType::IsFloatingPointType(instruction->InputAt(0)->GetType())) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetInAt(1,
                       IsFloatingPointZeroConstant(rhs)
                           ? Location::ConstantLocation(rhs)
                           : Location::RequiresFpuRegister());
  } else {
    // Integer cases.
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, ARM64EncodableConstantOrRegister(rhs, instruction));
  }

  if (!instruction->IsEmittedAtUseSite()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::HandleCondition(HCondition* instruction) {
  if (instruction->IsEmittedAtUseSite()) {
    return;
  }

  LocationSummary* locations = instruction->GetLocations();
  Register res = RegisterFrom(locations->Out(), instruction->GetType());
  IfCondition if_cond = instruction->GetCondition();

  if (DataType::IsFloatingPointType(instruction->InputAt(0)->GetType())) {
    GenerateFcmp(instruction);
    __ Cset(res, ARM64FPCondition(if_cond, instruction->IsGtBias()));
  } else {
    // Integer cases.
    Register lhs = InputRegisterAt(instruction, 0);
    Operand rhs = InputOperandAt(instruction, 1);
    __ Cmp(lhs, rhs);
    __ Cset(res, ARM64Condition(if_cond));
  }
}

#define FOR_EACH_CONDITION_INSTRUCTION(M)                                                \
  M(Equal)                                                                               \
  M(NotEqual)                                                                            \
  M(LessThan)                                                                            \
  M(LessThanOrEqual)                                                                     \
  M(GreaterThan)                                                                         \
  M(GreaterThanOrEqual)                                                                  \
  M(Below)                                                                               \
  M(BelowOrEqual)                                                                        \
  M(Above)                                                                               \
  M(AboveOrEqual)
#define DEFINE_CONDITION_VISITORS(Name)                                                  \
void LocationsBuilderARM64::Visit##Name(H##Name* comp) { HandleCondition(comp); }         \
void InstructionCodeGeneratorARM64::Visit##Name(H##Name* comp) { HandleCondition(comp); }
FOR_EACH_CONDITION_INSTRUCTION(DEFINE_CONDITION_VISITORS)
#undef DEFINE_CONDITION_VISITORS
#undef FOR_EACH_CONDITION_INSTRUCTION

void InstructionCodeGeneratorARM64::GenerateIntDivForPower2Denom(HDiv* instruction) {
  int64_t imm = Int64FromLocation(instruction->GetLocations()->InAt(1));
  uint64_t abs_imm = static_cast<uint64_t>(AbsOrMin(imm));
  DCHECK(IsPowerOfTwo(abs_imm)) << abs_imm;

  Register out = OutputRegister(instruction);
  Register dividend = InputRegisterAt(instruction, 0);

  Register final_dividend;
  if (HasNonNegativeOrMinIntInputAt(instruction, 0)) {
    // No need to adjust the result for non-negative dividends or the INT32_MIN/INT64_MIN dividends.
    // NOTE: The generated code for HDiv correctly works for the INT32_MIN/INT64_MIN dividends:
    //   imm == 2
    //     add out, dividend(0x80000000), dividend(0x80000000), lsr #31 => out = 0x80000001
    //     asr out, out(0x80000001), #1 => out = 0xc0000000
    //     This is the same as 'asr out, 0x80000000, #1'
    //
    //   imm > 2
    //     add temp, dividend(0x80000000), imm - 1 => temp = 0b10..01..1, where the number
    //         of the rightmost 1s is ctz_imm.
    //     cmp dividend(0x80000000), 0 => N = 1, V = 0 (lt is true)
    //     csel out, temp(0b10..01..1), dividend(0x80000000), lt => out = 0b10..01..1
    //     asr out, out(0b10..01..1), #ctz_imm => out = 0b1..10..0, where the number of the
    //         leftmost 1s is ctz_imm + 1.
    //     This is the same as 'asr out, dividend(0x80000000), #ctz_imm'.
    //
    //   imm == INT32_MIN
    //     add tmp, dividend(0x80000000), #0x7fffffff => tmp = -1
    //     cmp dividend(0x80000000), 0 => N = 1, V = 0 (lt is true)
    //     csel out, temp(-1), dividend(0x80000000), lt => out = -1
    //     neg out, out(-1), asr #31 => out = 1
    //     This is the same as 'neg out, dividend(0x80000000), asr #31'.
    final_dividend = dividend;
  } else {
    if (abs_imm == 2) {
      int bits = DataType::Size(instruction->GetResultType()) * kBitsPerByte;
      __ Add(out, dividend, Operand(dividend, LSR, bits - 1));
    } else {
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register temp = temps.AcquireSameSizeAs(out);
      __ Add(temp, dividend, abs_imm - 1);
      __ Cmp(dividend, 0);
      __ Csel(out, temp, dividend, lt);
    }
    final_dividend = out;
  }

  int ctz_imm = CTZ(abs_imm);
  if (imm > 0) {
    __ Asr(out, final_dividend, ctz_imm);
  } else {
    __ Neg(out, Operand(final_dividend, ASR, ctz_imm));
  }
}

// Return true if the magic number was modified by subtracting 2^32(Int32 div) or 2^64(Int64 div).
// So dividend needs to be added.
static inline bool NeedToAddDividend(int64_t magic_number, int64_t divisor) {
  return divisor > 0 && magic_number < 0;
}

// Return true if the magic number was modified by adding 2^32(Int32 div) or 2^64(Int64 div).
// So dividend needs to be subtracted.
static inline bool NeedToSubDividend(int64_t magic_number, int64_t divisor) {
  return divisor < 0 && magic_number > 0;
}

// Generate code which increments the value in register 'in' by 1 if the value is negative.
// It is done with 'add out, in, in, lsr #31 or #63'.
// If the value is a result of an operation setting the N flag, CINC MI can be used
// instead of ADD. 'use_cond_inc' controls this.
void InstructionCodeGeneratorARM64::GenerateIncrementNegativeByOne(
    Register out,
    Register in,
    bool use_cond_inc) {
  if (use_cond_inc) {
    __ Cinc(out, in, mi);
  } else {
    __ Add(out, in, Operand(in, LSR, in.GetSizeInBits() - 1));
  }
}

// Helper to generate code producing the result of HRem with a constant divisor.
void InstructionCodeGeneratorARM64::GenerateResultRemWithAnyConstant(
    Register out,
    Register dividend,
    Register quotient,
    int64_t divisor,
    UseScratchRegisterScope* temps_scope) {
  Register temp_imm = temps_scope->AcquireSameSizeAs(out);
  __ Mov(temp_imm, divisor);
  __ Msub(out, quotient, temp_imm, dividend);
}

// Helper to generate code for HDiv/HRem instructions when a dividend is non-negative and
// a divisor is a positive constant, not power of 2.
void InstructionCodeGeneratorARM64::GenerateInt64UnsignedDivRemWithAnyPositiveConstant(
    HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  DCHECK(instruction->GetResultType() == DataType::Type::kInt64);

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = OutputRegister(instruction);
  Register dividend = InputRegisterAt(instruction, 0);
  int64_t imm = Int64FromConstant(second.GetConstant());
  DCHECK_GT(imm, 0);

  int64_t magic;
  int shift;
  CalculateMagicAndShiftForDivRem(imm, /* is_long= */ true, &magic, &shift);

  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp = temps.AcquireSameSizeAs(out);

  auto generate_unsigned_div_code = [this, magic, shift](Register out,
                                                         Register dividend,
                                                         Register temp) {
    // temp = get_high(dividend * magic)
    __ Mov(temp, magic);
    if (magic > 0 && shift == 0) {
      __ Smulh(out, dividend, temp);
    } else {
      __ Smulh(temp, dividend, temp);
      if (magic < 0) {
        // The negative magic means that the multiplier m is greater than INT64_MAX.
        // In such a case shift is never 0. See the proof in
        // InstructionCodeGeneratorARMVIXL::GenerateDivRemWithAnyConstant.
        __ Add(temp, temp, dividend);
      }
      DCHECK_NE(shift, 0);
      __ Lsr(out, temp, shift);
    }
  };

  if (instruction->IsDiv()) {
    generate_unsigned_div_code(out, dividend, temp);
  } else {
    generate_unsigned_div_code(temp, dividend, temp);
    GenerateResultRemWithAnyConstant(out, dividend, temp, imm, &temps);
  }
}

// Helper to generate code for HDiv/HRem instructions for any dividend and a constant divisor
// (not power of 2).
void InstructionCodeGeneratorARM64::GenerateInt64DivRemWithAnyConstant(
    HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  DCHECK(instruction->GetResultType() == DataType::Type::kInt64);

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = OutputRegister(instruction);
  Register dividend = InputRegisterAt(instruction, 0);
  int64_t imm = Int64FromConstant(second.GetConstant());

  int64_t magic;
  int shift;
  CalculateMagicAndShiftForDivRem(imm, /* is_long= */ true, &magic, &shift);

  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp = temps.AcquireSameSizeAs(out);

  // temp = get_high(dividend * magic)
  __ Mov(temp, magic);
  __ Smulh(temp, dividend, temp);

  // The multiplication result might need some corrections to be finalized.
  // The last correction is to increment by 1, if the result is negative.
  // Currently it is done with 'add result, temp_result, temp_result, lsr #31 or #63'.
  // Such ADD usually has latency 2, e.g. on Cortex-A55.
  // However if one of the corrections is ADD or SUB, the sign can be detected
  // with ADDS/SUBS. They set the N flag if the result is negative.
  // This allows to use CINC MI which has latency 1.
  bool use_cond_inc = false;

  // Some combinations of magic_number and the divisor require to correct the result.
  // Check whether the correction is needed.
  if (NeedToAddDividend(magic, imm)) {
    __ Adds(temp, temp, dividend);
    use_cond_inc = true;
  } else if (NeedToSubDividend(magic, imm)) {
    __ Subs(temp, temp, dividend);
    use_cond_inc = true;
  }

  if (shift != 0) {
    __ Asr(temp, temp, shift);
  }

  if (instruction->IsRem()) {
    GenerateIncrementNegativeByOne(temp, temp, use_cond_inc);
    GenerateResultRemWithAnyConstant(out, dividend, temp, imm, &temps);
  } else {
    GenerateIncrementNegativeByOne(out, temp, use_cond_inc);
  }
}

void InstructionCodeGeneratorARM64::GenerateInt32DivRemWithAnyConstant(
    HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  DCHECK(instruction->GetResultType() == DataType::Type::kInt32);

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = OutputRegister(instruction);
  Register dividend = InputRegisterAt(instruction, 0);
  int64_t imm = Int64FromConstant(second.GetConstant());

  int64_t magic;
  int shift;
  CalculateMagicAndShiftForDivRem(imm, /* is_long= */ false, &magic, &shift);
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp = temps.AcquireSameSizeAs(out);

  // temp = get_high(dividend * magic)
  __ Mov(temp, magic);
  __ Smull(temp.X(), dividend, temp);

  // The multiplication result might need some corrections to be finalized.
  // The last correction is to increment by 1, if the result is negative.
  // Currently it is done with 'add result, temp_result, temp_result, lsr #31 or #63'.
  // Such ADD usually has latency 2, e.g. on Cortex-A55.
  // However if one of the corrections is ADD or SUB, the sign can be detected
  // with ADDS/SUBS. They set the N flag if the result is negative.
  // This allows to use CINC MI which has latency 1.
  bool use_cond_inc = false;

  // ADD/SUB correction is performed in the high 32 bits
  // as high 32 bits are ignored because type are kInt32.
  if (NeedToAddDividend(magic, imm)) {
    __ Adds(temp.X(), temp.X(), Operand(dividend.X(), LSL, 32));
    use_cond_inc = true;
  } else if (NeedToSubDividend(magic, imm)) {
    __ Subs(temp.X(), temp.X(), Operand(dividend.X(), LSL, 32));
    use_cond_inc = true;
  }

  // Extract the result from the high 32 bits and apply the final right shift.
  DCHECK_LT(shift, 32);
  if (imm > 0 && HasNonNegativeInputAt(instruction, 0)) {
    // No need to adjust the result for a non-negative dividend and a positive divisor.
    if (instruction->IsDiv()) {
      __ Lsr(out.X(), temp.X(), 32 + shift);
    } else {
      __ Lsr(temp.X(), temp.X(), 32 + shift);
      GenerateResultRemWithAnyConstant(out, dividend, temp, imm, &temps);
    }
  } else {
    __ Asr(temp.X(), temp.X(), 32 + shift);

    if (instruction->IsRem()) {
      GenerateIncrementNegativeByOne(temp, temp, use_cond_inc);
      GenerateResultRemWithAnyConstant(out, dividend, temp, imm, &temps);
    } else {
      GenerateIncrementNegativeByOne(out, temp, use_cond_inc);
    }
  }
}

void InstructionCodeGeneratorARM64::GenerateDivRemWithAnyConstant(HBinaryOperation* instruction,
                                                                  int64_t divisor) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  if (instruction->GetResultType() == DataType::Type::kInt64) {
    if (divisor > 0 && HasNonNegativeInputAt(instruction, 0)) {
      GenerateInt64UnsignedDivRemWithAnyPositiveConstant(instruction);
    } else {
      GenerateInt64DivRemWithAnyConstant(instruction);
    }
  } else {
    GenerateInt32DivRemWithAnyConstant(instruction);
  }
}

void InstructionCodeGeneratorARM64::GenerateIntDivForConstDenom(HDiv *instruction) {
  int64_t imm = Int64FromLocation(instruction->GetLocations()->InAt(1));

  if (imm == 0) {
    // Do not generate anything. DivZeroCheck would prevent any code to be executed.
    return;
  }

  if (IsPowerOfTwo(AbsOrMin(imm))) {
    GenerateIntDivForPower2Denom(instruction);
  } else {
    // Cases imm == -1 or imm == 1 are handled by InstructionSimplifier.
    DCHECK(imm < -2 || imm > 2) << imm;
    GenerateDivRemWithAnyConstant(instruction, imm);
  }
}

void InstructionCodeGeneratorARM64::GenerateIntDiv(HDiv *instruction) {
  DCHECK(DataType::IsIntOrLongType(instruction->GetResultType()))
       << instruction->GetResultType();

  if (instruction->GetLocations()->InAt(1).IsConstant()) {
    GenerateIntDivForConstDenom(instruction);
  } else {
    Register out = OutputRegister(instruction);
    Register dividend = InputRegisterAt(instruction, 0);
    Register divisor = InputRegisterAt(instruction, 1);
    __ Sdiv(out, dividend, divisor);
  }
}

void LocationsBuilderARM64::VisitDiv(HDiv* div) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(div, LocationSummary::kNoCall);
  switch (div->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(div->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorARM64::VisitDiv(HDiv* div) {
  DataType::Type type = div->GetResultType();
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      GenerateIntDiv(div);
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      __ Fdiv(OutputFPRegister(div), InputFPRegisterAt(div, 0), InputFPRegisterAt(div, 1));
      break;

    default:
      LOG(FATAL) << "Unexpected div type " << type;
  }
}

void LocationsBuilderARM64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
}

void InstructionCodeGeneratorARM64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeARM64* slow_path =
      new (codegen_->GetScopedAllocator()) DivZeroCheckSlowPathARM64(instruction);
  codegen_->AddSlowPath(slow_path);
  Location value = instruction->GetLocations()->InAt(0);

  DataType::Type type = instruction->GetType();

  if (!DataType::IsIntegralType(type)) {
    LOG(FATAL) << "Unexpected type " << type << " for DivZeroCheck.";
    UNREACHABLE();
  }

  if (value.IsConstant()) {
    int64_t divisor = Int64FromLocation(value);
    if (divisor == 0) {
      __ B(slow_path->GetEntryLabel());
    } else {
      // A division by a non-null constant is valid. We don't need to perform
      // any check, so simply fall through.
    }
  } else {
    __ Cbz(InputRegisterAt(instruction, 0), slow_path->GetEntryLabel());
  }
}

void LocationsBuilderARM64::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitDoubleConstant(
    [[maybe_unused]] HDoubleConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitExit([[maybe_unused]] HExit* exit) {}

void LocationsBuilderARM64::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitFloatConstant([[maybe_unused]] HFloatConstant* constant) {
  // Will be generated at use site.
}

void InstructionCodeGeneratorARM64::HandleGoto(HInstruction* got, HBasicBlock* successor) {
  if (successor->IsExitBlock()) {
    DCHECK(got->GetPrevious()->AlwaysThrows());
    return;  // no code needed
  }

  HBasicBlock* block = got->GetBlock();
  HInstruction* previous = got->GetPrevious();
  HLoopInformation* info = block->GetLoopInformation();

  if (info != nullptr && info->IsBackEdge(*block) && info->HasSuspendCheck()) {
    codegen_->MaybeIncrementHotness(info->GetSuspendCheck(), /* is_frame_entry= */ false);
    GenerateSuspendCheck(info->GetSuspendCheck(), successor);
    return;  // `GenerateSuspendCheck()` emitted the jump.
  }
  if (block->IsEntryBlock() && (previous != nullptr) && previous->IsSuspendCheck()) {
    GenerateSuspendCheck(previous->AsSuspendCheck(), nullptr);
    codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
  }
  if (!codegen_->GoesToNextBlock(block, successor)) {
    __ B(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderARM64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitGoto(HGoto* got) {
  HandleGoto(got, got->GetSuccessor());
}

void LocationsBuilderARM64::VisitTryBoundary(HTryBoundary* try_boundary) {
  try_boundary->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitTryBoundary(HTryBoundary* try_boundary) {
  HBasicBlock* successor = try_boundary->GetNormalFlowSuccessor();
  if (!successor->IsExitBlock()) {
    HandleGoto(try_boundary, successor);
  }
}

void InstructionCodeGeneratorARM64::GenerateTestAndBranch(HInstruction* instruction,
                                                          size_t condition_input_index,
                                                          vixl::aarch64::Label* true_target,
                                                          vixl::aarch64::Label* false_target) {
  HInstruction* cond = instruction->InputAt(condition_input_index);

  if (true_target == nullptr && false_target == nullptr) {
    // Nothing to do. The code always falls through.
    return;
  } else if (cond->IsIntConstant()) {
    // Constant condition, statically compared against "true" (integer value 1).
    if (cond->AsIntConstant()->IsTrue()) {
      if (true_target != nullptr) {
        __ B(true_target);
      }
    } else {
      DCHECK(cond->AsIntConstant()->IsFalse()) << cond->AsIntConstant()->GetValue();
      if (false_target != nullptr) {
        __ B(false_target);
      }
    }
    return;
  }

  // The following code generates these patterns:
  //  (1) true_target == nullptr && false_target != nullptr
  //        - opposite condition true => branch to false_target
  //  (2) true_target != nullptr && false_target == nullptr
  //        - condition true => branch to true_target
  //  (3) true_target != nullptr && false_target != nullptr
  //        - condition true => branch to true_target
  //        - branch to false_target
  if (IsBooleanValueOrMaterializedCondition(cond)) {
    // The condition instruction has been materialized, compare the output to 0.
    Location cond_val = instruction->GetLocations()->InAt(condition_input_index);
    DCHECK(cond_val.IsRegister());
    if (true_target == nullptr) {
      __ Cbz(InputRegisterAt(instruction, condition_input_index), false_target);
    } else {
      __ Cbnz(InputRegisterAt(instruction, condition_input_index), true_target);
    }
  } else {
    // The condition instruction has not been materialized, use its inputs as
    // the comparison and its condition as the branch condition.
    HCondition* condition = cond->AsCondition();

    DataType::Type type = condition->InputAt(0)->GetType();
    if (DataType::IsFloatingPointType(type)) {
      GenerateFcmp(condition);
      if (true_target == nullptr) {
        IfCondition opposite_condition = condition->GetOppositeCondition();
        __ B(ARM64FPCondition(opposite_condition, condition->IsGtBias()), false_target);
      } else {
        __ B(ARM64FPCondition(condition->GetCondition(), condition->IsGtBias()), true_target);
      }
    } else {
      // Integer cases.
      Register lhs = InputRegisterAt(condition, 0);
      Operand rhs = InputOperandAt(condition, 1);

      Condition arm64_cond;
      vixl::aarch64::Label* non_fallthrough_target;
      if (true_target == nullptr) {
        arm64_cond = ARM64Condition(condition->GetOppositeCondition());
        non_fallthrough_target = false_target;
      } else {
        arm64_cond = ARM64Condition(condition->GetCondition());
        non_fallthrough_target = true_target;
      }

      if ((arm64_cond == eq || arm64_cond == ne || arm64_cond == lt || arm64_cond == ge) &&
          rhs.IsImmediate() && (rhs.GetImmediate() == 0)) {
        switch (arm64_cond) {
          case eq:
            __ Cbz(lhs, non_fallthrough_target);
            break;
          case ne:
            __ Cbnz(lhs, non_fallthrough_target);
            break;
          case lt:
            // Test the sign bit and branch accordingly.
            __ Tbnz(lhs, (lhs.IsX() ? kXRegSize : kWRegSize) - 1, non_fallthrough_target);
            break;
          case ge:
            // Test the sign bit and branch accordingly.
            __ Tbz(lhs, (lhs.IsX() ? kXRegSize : kWRegSize) - 1, non_fallthrough_target);
            break;
          default:
            // Without the `static_cast` the compiler throws an error for
            // `-Werror=sign-promo`.
            LOG(FATAL) << "Unexpected condition: " << static_cast<int>(arm64_cond);
        }
      } else {
        __ Cmp(lhs, rhs);
        __ B(arm64_cond, non_fallthrough_target);
      }
    }
  }

  // If neither branch falls through (case 3), the conditional branch to `true_target`
  // was already emitted (case 2) and we need to emit a jump to `false_target`.
  if (true_target != nullptr && false_target != nullptr) {
    __ B(false_target);
  }
}

void LocationsBuilderARM64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(if_instr);
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitIf(HIf* if_instr) {
  HBasicBlock* true_successor = if_instr->IfTrueSuccessor();
  HBasicBlock* false_successor = if_instr->IfFalseSuccessor();
  vixl::aarch64::Label* true_target = codegen_->GetLabelOf(true_successor);
  if (codegen_->GoesToNextBlock(if_instr->GetBlock(), true_successor)) {
    true_target = nullptr;
  }
  vixl::aarch64::Label* false_target = codegen_->GetLabelOf(false_successor);
  if (codegen_->GoesToNextBlock(if_instr->GetBlock(), false_successor)) {
    false_target = nullptr;
  }
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    if (GetGraph()->IsCompilingBaseline() &&
        codegen_->GetCompilerOptions().ProfileBranches() &&
        !Runtime::Current()->IsAotCompiler()) {
      DCHECK(if_instr->InputAt(0)->IsCondition());
      ProfilingInfo* info = GetGraph()->GetProfilingInfo();
      DCHECK(info != nullptr);
      BranchCache* cache = info->GetBranchCache(if_instr->GetDexPc());
      // Currently, not all If branches are profiled.
      if (cache != nullptr) {
        uint64_t address =
            reinterpret_cast64<uint64_t>(cache) + BranchCache::FalseOffset().Int32Value();
        static_assert(
            BranchCache::TrueOffset().Int32Value() - BranchCache::FalseOffset().Int32Value() == 2,
            "Unexpected offsets for BranchCache");
        vixl::aarch64::Label done;
        UseScratchRegisterScope temps(GetVIXLAssembler());
        Register temp = temps.AcquireX();
        Register counter = temps.AcquireW();
        Register condition = InputRegisterAt(if_instr, 0).X();
        __ Mov(temp, address);
        __ Ldrh(counter, MemOperand(temp, condition, LSL, 1));
        __ Add(counter, counter, 1);
        __ Tbnz(counter, 16, &done);
        __ Strh(counter, MemOperand(temp, condition, LSL, 1));
        __ Bind(&done);
      }
    }
  }
  GenerateTestAndBranch(if_instr, /* condition_input_index= */ 0, true_target, false_target);
}

void LocationsBuilderARM64::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetAllocator())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  InvokeRuntimeCallingConvention calling_convention;
  RegisterSet caller_saves = RegisterSet::Empty();
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(0).GetCode()));
  locations->SetCustomSlowPathCallerSaves(caller_saves);
  if (IsBooleanValueOrMaterializedCondition(deoptimize->InputAt(0))) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCodeARM64* slow_path =
      deopt_slow_paths_.NewSlowPath<DeoptimizationSlowPathARM64>(deoptimize);
  GenerateTestAndBranch(deoptimize,
                        /* condition_input_index= */ 0,
                        slow_path->GetEntryLabel(),
                        /* false_target= */ nullptr);
}

void LocationsBuilderARM64::VisitShouldDeoptimizeFlag(HShouldDeoptimizeFlag* flag) {
  LocationSummary* locations = new (GetGraph()->GetAllocator())
      LocationSummary(flag, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitShouldDeoptimizeFlag(HShouldDeoptimizeFlag* flag) {
  __ Ldr(OutputRegister(flag),
         MemOperand(sp, codegen_->GetStackOffsetOfShouldDeoptimizeFlag()));
}

static inline bool IsConditionOnFloatingPointValues(HInstruction* condition) {
  return condition->IsCondition() &&
         DataType::IsFloatingPointType(condition->InputAt(0)->GetType());
}

static inline Condition GetConditionForSelect(HCondition* condition) {
  IfCondition cond = condition->GetCondition();
  return IsConditionOnFloatingPointValues(condition) ? ARM64FPCondition(cond, condition->IsGtBias())
                                                     : ARM64Condition(cond);
}

void LocationsBuilderARM64::VisitSelect(HSelect* select) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(select);
  if (DataType::IsFloatingPointType(select->GetType())) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetInAt(1, Location::RequiresFpuRegister());
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    HConstant* cst_true_value = select->GetTrueValue()->AsConstantOrNull();
    HConstant* cst_false_value = select->GetFalseValue()->AsConstantOrNull();
    bool is_true_value_constant = cst_true_value != nullptr;
    bool is_false_value_constant = cst_false_value != nullptr;
    // Ask VIXL whether we should synthesize constants in registers.
    // We give an arbitrary register to VIXL when dealing with non-constant inputs.
    Operand true_op = is_true_value_constant ?
        Operand(Int64FromConstant(cst_true_value)) : Operand(x1);
    Operand false_op = is_false_value_constant ?
        Operand(Int64FromConstant(cst_false_value)) : Operand(x2);
    bool true_value_in_register = false;
    bool false_value_in_register = false;
    MacroAssembler::GetCselSynthesisInformation(
        x0, true_op, false_op, &true_value_in_register, &false_value_in_register);
    true_value_in_register |= !is_true_value_constant;
    false_value_in_register |= !is_false_value_constant;

    locations->SetInAt(1, true_value_in_register ? Location::RequiresRegister()
                                                 : Location::ConstantLocation(cst_true_value));
    locations->SetInAt(0, false_value_in_register ? Location::RequiresRegister()
                                                  : Location::ConstantLocation(cst_false_value));
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }

  if (IsBooleanValueOrMaterializedCondition(select->GetCondition())) {
    locations->SetInAt(2, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitSelect(HSelect* select) {
  HInstruction* cond = select->GetCondition();
  Condition csel_cond;

  if (IsBooleanValueOrMaterializedCondition(cond)) {
    if (cond->IsCondition() && cond->GetNext() == select) {
      // Use the condition flags set by the previous instruction.
      csel_cond = GetConditionForSelect(cond->AsCondition());
    } else {
      __ Cmp(InputRegisterAt(select, 2), 0);
      csel_cond = ne;
    }
  } else if (IsConditionOnFloatingPointValues(cond)) {
    GenerateFcmp(cond);
    csel_cond = GetConditionForSelect(cond->AsCondition());
  } else {
    __ Cmp(InputRegisterAt(cond, 0), InputOperandAt(cond, 1));
    csel_cond = GetConditionForSelect(cond->AsCondition());
  }

  if (DataType::IsFloatingPointType(select->GetType())) {
    __ Fcsel(OutputFPRegister(select),
             InputFPRegisterAt(select, 1),
             InputFPRegisterAt(select, 0),
             csel_cond);
  } else {
    __ Csel(OutputRegister(select),
            InputOperandAt(select, 1),
            InputOperandAt(select, 0),
            csel_cond);
  }
}

void LocationsBuilderARM64::VisitNop(HNop* nop) {
  new (GetGraph()->GetAllocator()) LocationSummary(nop);
}

void InstructionCodeGeneratorARM64::VisitNop(HNop*) {
  // The environment recording already happened in CodeGenerator::Compile.
}

void CodeGeneratorARM64::IncreaseFrame(size_t adjustment) {
  __ Claim(adjustment);
  GetAssembler()->cfi().AdjustCFAOffset(adjustment);
}

void CodeGeneratorARM64::DecreaseFrame(size_t adjustment) {
  __ Drop(adjustment);
  GetAssembler()->cfi().AdjustCFAOffset(-adjustment);
}

void CodeGeneratorARM64::GenerateNop() {
  __ Nop();
}

void LocationsBuilderARM64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorARM64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction);
}

void InstructionCodeGeneratorARM64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction,
                 instruction->GetFieldInfo(),
                 instruction->GetValueCanBeNull(),
                 instruction->GetWriteBarrierKind());
}

// Temp is used for read barrier.
static size_t NumberOfInstanceOfTemps(bool emit_read_barrier, TypeCheckKind type_check_kind) {
  if (emit_read_barrier &&
      (kUseBakerReadBarrier ||
          type_check_kind == TypeCheckKind::kAbstractClassCheck ||
          type_check_kind == TypeCheckKind::kClassHierarchyCheck ||
          type_check_kind == TypeCheckKind::kArrayObjectCheck)) {
    return 1;
  }
  return 0;
}

// Interface case has 3 temps, one for holding the number of interfaces, one for the current
// interface pointer, one for loading the current interface.
// The other checks have one temp for loading the object's class.
static size_t NumberOfCheckCastTemps(bool emit_read_barrier, TypeCheckKind type_check_kind) {
  if (type_check_kind == TypeCheckKind::kInterfaceCheck) {
    return 3;
  }
  return 1 + NumberOfInstanceOfTemps(emit_read_barrier, type_check_kind);
}

void LocationsBuilderARM64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind = LocationSummary::kNoCall;
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  bool baker_read_barrier_slow_path = false;
  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kAbstractClassCheck:
    case TypeCheckKind::kClassHierarchyCheck:
    case TypeCheckKind::kArrayObjectCheck:
    case TypeCheckKind::kInterfaceCheck: {
      bool needs_read_barrier = codegen_->InstanceOfNeedsReadBarrier(instruction);
      call_kind = needs_read_barrier ? LocationSummary::kCallOnSlowPath : LocationSummary::kNoCall;
      baker_read_barrier_slow_path = (kUseBakerReadBarrier && needs_read_barrier) &&
                                     (type_check_kind != TypeCheckKind::kInterfaceCheck);
      break;
    }
    case TypeCheckKind::kArrayCheck:
    case TypeCheckKind::kUnresolvedCheck:
      call_kind = LocationSummary::kCallOnSlowPath;
      break;
    case TypeCheckKind::kBitstringCheck:
      break;
  }

  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, call_kind);
  if (baker_read_barrier_slow_path) {
    locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
  }
  locations->SetInAt(0, Location::RequiresRegister());
  if (type_check_kind == TypeCheckKind::kBitstringCheck) {
    locations->SetInAt(1, Location::ConstantLocation(instruction->InputAt(1)));
    locations->SetInAt(2, Location::ConstantLocation(instruction->InputAt(2)));
    locations->SetInAt(3, Location::ConstantLocation(instruction->InputAt(3)));
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
  // The "out" register is used as a temporary, so it overlaps with the inputs.
  // Note that TypeCheckSlowPathARM64 uses this register too.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
  // Add temps if necessary for read barriers.
  locations->AddRegisterTemps(
      NumberOfInstanceOfTemps(codegen_->EmitReadBarrier(), type_check_kind));
}

void InstructionCodeGeneratorARM64::VisitInstanceOf(HInstanceOf* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  Register obj = InputRegisterAt(instruction, 0);
  Register cls = (type_check_kind == TypeCheckKind::kBitstringCheck)
      ? Register()
      : InputRegisterAt(instruction, 1);
  Location out_loc = locations->Out();
  Register out = OutputRegister(instruction);
  const size_t num_temps = NumberOfInstanceOfTemps(codegen_->EmitReadBarrier(), type_check_kind);
  DCHECK_LE(num_temps, 1u);
  Location maybe_temp_loc = (num_temps >= 1) ? locations->GetTemp(0) : Location::NoLocation();
  const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  const uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();
  const uint32_t iftable_offset = mirror::Class::IfTableOffset().Uint32Value();
  const uint32_t array_length_offset = mirror::Array::LengthOffset().Uint32Value();
  const uint32_t object_array_data_offset =
      mirror::Array::DataOffset(kHeapReferenceSize).Uint32Value();

  vixl::aarch64::Label done, zero;
  SlowPathCodeARM64* slow_path = nullptr;

  // Return 0 if `obj` is null.
  // Avoid null check if we know `obj` is not null.
  if (instruction->MustDoNullCheck()) {
    __ Cbz(obj, &zero);
  }

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck: {
      ReadBarrierOption read_barrier_option =
          codegen_->ReadBarrierOptionForInstanceOf(instruction);
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp_loc,
                                        read_barrier_option);
      __ Cmp(out, cls);
      __ Cset(out, eq);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      ReadBarrierOption read_barrier_option =
          codegen_->ReadBarrierOptionForInstanceOf(instruction);
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp_loc,
                                        read_barrier_option);
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      vixl::aarch64::Label loop, success;
      __ Bind(&loop);
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction,
                                       out_loc,
                                       super_offset,
                                       maybe_temp_loc,
                                       read_barrier_option);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ Cbz(out, &done);
      __ Cmp(out, cls);
      __ B(ne, &loop);
      __ Mov(out, 1);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      ReadBarrierOption read_barrier_option =
          codegen_->ReadBarrierOptionForInstanceOf(instruction);
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp_loc,
                                        read_barrier_option);
      // Walk over the class hierarchy to find a match.
      vixl::aarch64::Label loop, success;
      __ Bind(&loop);
      __ Cmp(out, cls);
      __ B(eq, &success);
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction,
                                       out_loc,
                                       super_offset,
                                       maybe_temp_loc,
                                       read_barrier_option);
      __ Cbnz(out, &loop);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ B(&done);
      __ Bind(&success);
      __ Mov(out, 1);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      ReadBarrierOption read_barrier_option =
          codegen_->ReadBarrierOptionForInstanceOf(instruction);
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp_loc,
                                        read_barrier_option);
      // Do an exact check.
      vixl::aarch64::Label exact_check;
      __ Cmp(out, cls);
      __ B(eq, &exact_check);
      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ out = out->component_type_
      GenerateReferenceLoadOneRegister(instruction,
                                       out_loc,
                                       component_offset,
                                       maybe_temp_loc,
                                       read_barrier_option);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ Cbz(out, &done);
      __ Ldrh(out, HeapOperand(out, primitive_offset));
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      __ Cbnz(out, &zero);
      __ Bind(&exact_check);
      __ Mov(out, 1);
      __ B(&done);
      break;
    }

    case TypeCheckKind::kArrayCheck: {
      // No read barrier since the slow path will retry upon failure.
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp_loc,
                                        kWithoutReadBarrier);
      __ Cmp(out, cls);
      DCHECK(locations->OnlyCallsOnSlowPath());
      slow_path = new (codegen_->GetScopedAllocator()) TypeCheckSlowPathARM64(
          instruction, /* is_fatal= */ false);
      codegen_->AddSlowPath(slow_path);
      __ B(ne, slow_path->GetEntryLabel());
      __ Mov(out, 1);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }

    case TypeCheckKind::kInterfaceCheck: {
      if (codegen_->InstanceOfNeedsReadBarrier(instruction)) {
        DCHECK(locations->OnlyCallsOnSlowPath());
        slow_path = new (codegen_->GetScopedAllocator()) TypeCheckSlowPathARM64(
            instruction, /* is_fatal= */ false);
        codegen_->AddSlowPath(slow_path);
        if (codegen_->EmitNonBakerReadBarrier()) {
          __ B(slow_path->GetEntryLabel());
          break;
        }
        // For Baker read barrier, take the slow path while marking.
        __ Cbnz(mr, slow_path->GetEntryLabel());
      }

      // Fast-path without read barriers.
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register temp = temps.AcquireW();
      Register temp2 = temps.AcquireW();
      // /* HeapReference<Class> */ temp = obj->klass_
      __ Ldr(temp, HeapOperand(obj, class_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(temp);
      // /* HeapReference<Class> */ temp = temp->iftable_
      __ Ldr(temp, HeapOperand(temp, iftable_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(temp);
      // Load the size of the `IfTable`. The `Class::iftable_` is never null.
      __ Ldr(out, HeapOperand(temp, array_length_offset));
      // Loop through the `IfTable` and check if any class matches.
      vixl::aarch64::Label loop;
      __ Bind(&loop);
      __ Cbz(out, &done);  // If taken, the result in `out` is already 0 (false).
      __ Ldr(temp2, HeapOperand(temp, object_array_data_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(temp2);
      // Go to next interface.
      __ Add(temp, temp, 2 * kHeapReferenceSize);
      __ Sub(out, out, 2);
      // Compare the classes and continue the loop if they do not match.
      __ Cmp(cls, temp2);
      __ B(ne, &loop);
      __ Mov(out, 1);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }

    case TypeCheckKind::kUnresolvedCheck: {
      // Note that we indeed only call on slow path, but we always go
      // into the slow path for the unresolved check case.
      //
      // We cannot directly call the InstanceofNonTrivial runtime
      // entry point without resorting to a type checking slow path
      // here (i.e. by calling InvokeRuntime directly), as it would
      // require to assign fixed registers for the inputs of this
      // HInstanceOf instruction (following the runtime calling
      // convention), which might be cluttered by the potential first
      // read barrier emission at the beginning of this method.
      //
      // TODO: Introduce a new runtime entry point taking the object
      // to test (instead of its class) as argument, and let it deal
      // with the read barrier issues. This will let us refactor this
      // case of the `switch` code as it was previously (with a direct
      // call to the runtime not using a type checking slow path).
      // This should also be beneficial for the other cases above.
      DCHECK(locations->OnlyCallsOnSlowPath());
      slow_path = new (codegen_->GetScopedAllocator()) TypeCheckSlowPathARM64(
          instruction, /* is_fatal= */ false);
      codegen_->AddSlowPath(slow_path);
      __ B(slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kBitstringCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp_loc,
                                        kWithoutReadBarrier);

      GenerateBitstringTypeCheckCompare(instruction, out);
      __ Cset(out, eq);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }
  }

  if (zero.IsLinked()) {
    __ Bind(&zero);
    __ Mov(out, 0);
  }

  if (done.IsLinked()) {
    __ Bind(&done);
  }

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

void LocationsBuilderARM64::VisitCheckCast(HCheckCast* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary::CallKind call_kind = codegen_->GetCheckCastCallKind(instruction);
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  if (type_check_kind == TypeCheckKind::kBitstringCheck) {
    locations->SetInAt(1, Location::ConstantLocation(instruction->InputAt(1)));
    locations->SetInAt(2, Location::ConstantLocation(instruction->InputAt(2)));
    locations->SetInAt(3, Location::ConstantLocation(instruction->InputAt(3)));
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
  locations->AddRegisterTemps(NumberOfCheckCastTemps(codegen_->EmitReadBarrier(), type_check_kind));
}

void InstructionCodeGeneratorARM64::VisitCheckCast(HCheckCast* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  Register obj = InputRegisterAt(instruction, 0);
  Register cls = (type_check_kind == TypeCheckKind::kBitstringCheck)
      ? Register()
      : InputRegisterAt(instruction, 1);
  const size_t num_temps = NumberOfCheckCastTemps(codegen_->EmitReadBarrier(), type_check_kind);
  DCHECK_GE(num_temps, 1u);
  DCHECK_LE(num_temps, 3u);
  Location temp_loc = locations->GetTemp(0);
  Location maybe_temp2_loc = (num_temps >= 2) ? locations->GetTemp(1) : Location::NoLocation();
  Location maybe_temp3_loc = (num_temps >= 3) ? locations->GetTemp(2) : Location::NoLocation();
  Register temp = WRegisterFrom(temp_loc);
  const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  const uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();
  const uint32_t iftable_offset = mirror::Class::IfTableOffset().Uint32Value();
  const uint32_t array_length_offset = mirror::Array::LengthOffset().Uint32Value();
  const uint32_t object_array_data_offset =
      mirror::Array::DataOffset(kHeapReferenceSize).Uint32Value();

  bool is_type_check_slow_path_fatal = codegen_->IsTypeCheckSlowPathFatal(instruction);
  SlowPathCodeARM64* type_check_slow_path =
      new (codegen_->GetScopedAllocator()) TypeCheckSlowPathARM64(
          instruction, is_type_check_slow_path_fatal);
  codegen_->AddSlowPath(type_check_slow_path);

  vixl::aarch64::Label done;
  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ Cbz(obj, &done);
  }

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kArrayCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp2_loc,
                                        kWithoutReadBarrier);

      __ Cmp(temp, cls);
      // Jump to slow path for throwing the exception or doing a
      // more involved array check.
      __ B(ne, type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp2_loc,
                                        kWithoutReadBarrier);

      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      vixl::aarch64::Label loop;
      __ Bind(&loop);
      // /* HeapReference<Class> */ temp = temp->super_class_
      GenerateReferenceLoadOneRegister(instruction,
                                       temp_loc,
                                       super_offset,
                                       maybe_temp2_loc,
                                       kWithoutReadBarrier);

      // If the class reference currently in `temp` is null, jump to the slow path to throw the
      // exception.
      __ Cbz(temp, type_check_slow_path->GetEntryLabel());
      // Otherwise, compare classes.
      __ Cmp(temp, cls);
      __ B(ne, &loop);
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp2_loc,
                                        kWithoutReadBarrier);

      // Walk over the class hierarchy to find a match.
      vixl::aarch64::Label loop;
      __ Bind(&loop);
      __ Cmp(temp, cls);
      __ B(eq, &done);

      // /* HeapReference<Class> */ temp = temp->super_class_
      GenerateReferenceLoadOneRegister(instruction,
                                       temp_loc,
                                       super_offset,
                                       maybe_temp2_loc,
                                       kWithoutReadBarrier);

      // If the class reference currently in `temp` is not null, jump
      // back at the beginning of the loop.
      __ Cbnz(temp, &loop);
      // Otherwise, jump to the slow path to throw the exception.
      __ B(type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp2_loc,
                                        kWithoutReadBarrier);

      // Do an exact check.
      __ Cmp(temp, cls);
      __ B(eq, &done);

      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ temp = temp->component_type_
      GenerateReferenceLoadOneRegister(instruction,
                                       temp_loc,
                                       component_offset,
                                       maybe_temp2_loc,
                                       kWithoutReadBarrier);

      // If the component type is null, jump to the slow path to throw the exception.
      __ Cbz(temp, type_check_slow_path->GetEntryLabel());
      // Otherwise, the object is indeed an array. Further check that this component type is not a
      // primitive type.
      __ Ldrh(temp, HeapOperand(temp, primitive_offset));
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      __ Cbnz(temp, type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kUnresolvedCheck:
      // We always go into the type check slow path for the unresolved check cases.
      //
      // We cannot directly call the CheckCast runtime entry point
      // without resorting to a type checking slow path here (i.e. by
      // calling InvokeRuntime directly), as it would require to
      // assign fixed registers for the inputs of this HInstanceOf
      // instruction (following the runtime calling convention), which
      // might be cluttered by the potential first read barrier
      // emission at the beginning of this method.
      __ B(type_check_slow_path->GetEntryLabel());
      break;
    case TypeCheckKind::kInterfaceCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp2_loc,
                                        kWithoutReadBarrier);

      // /* HeapReference<Class> */ temp = temp->iftable_
      GenerateReferenceLoadOneRegister(instruction,
                                       temp_loc,
                                       iftable_offset,
                                       maybe_temp2_loc,
                                       kWithoutReadBarrier);
      // Load the size of the `IfTable`. The `Class::iftable_` is never null.
      __ Ldr(WRegisterFrom(maybe_temp2_loc), HeapOperand(temp.W(), array_length_offset));
      // Loop through the iftable and check if any class matches.
      vixl::aarch64::Label start_loop;
      __ Bind(&start_loop);
      __ Cbz(WRegisterFrom(maybe_temp2_loc), type_check_slow_path->GetEntryLabel());
      __ Ldr(WRegisterFrom(maybe_temp3_loc), HeapOperand(temp.W(), object_array_data_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(WRegisterFrom(maybe_temp3_loc));
      // Go to next interface.
      __ Add(temp, temp, 2 * kHeapReferenceSize);
      __ Sub(WRegisterFrom(maybe_temp2_loc), WRegisterFrom(maybe_temp2_loc), 2);
      // Compare the classes and continue the loop if they do not match.
      __ Cmp(cls, WRegisterFrom(maybe_temp3_loc));
      __ B(ne, &start_loop);
      break;
    }

    case TypeCheckKind::kBitstringCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        maybe_temp2_loc,
                                        kWithoutReadBarrier);

      GenerateBitstringTypeCheckCompare(instruction, temp);
      __ B(ne, type_check_slow_path->GetEntryLabel());
      break;
    }
  }
  __ Bind(&done);

  __ Bind(type_check_slow_path->GetExitLabel());
}

void LocationsBuilderARM64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitIntConstant([[maybe_unused]] HIntConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitNullConstant([[maybe_unused]] HNullConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  codegen_->GenerateInvokeUnresolvedRuntimeCall(invoke);
  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void LocationsBuilderARM64::HandleInvoke(HInvoke* invoke) {
  InvokeDexCallingConventionVisitorARM64 calling_convention_visitor;
  CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
}

void LocationsBuilderARM64::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  if (invoke->GetHiddenArgumentLoadKind() == MethodLoadKind::kRecursive) {
    // We cannot request ip1 as it's blocked by the register allocator.
    invoke->GetLocations()->SetInAt(invoke->GetNumberOfArguments() - 1, Location::Any());
  }
}

void CodeGeneratorARM64::MaybeGenerateInlineCacheCheck(HInstruction* instruction,
                                                       Register klass) {
  DCHECK_EQ(klass.GetCode(), 0u);
  if (ProfilingInfoBuilder::IsInlineCacheUseful(instruction->AsInvoke(), this)) {
    ProfilingInfo* info = GetGraph()->GetProfilingInfo();
    DCHECK(info != nullptr);
    InlineCache* cache = ProfilingInfoBuilder::GetInlineCache(
        info, GetCompilerOptions(), instruction->AsInvoke());
    if (cache != nullptr) {
      uint64_t address = reinterpret_cast64<uint64_t>(cache);
      vixl::aarch64::Label done;
      __ Mov(x8, address);
      __ Ldr(w9, MemOperand(x8, InlineCache::ClassesOffset().Int32Value()));
      // Fast path for a monomorphic cache.
      __ Cmp(klass.W(), w9);
      __ B(eq, &done);
      InvokeRuntime(kQuickUpdateInlineCache, instruction);
      __ Bind(&done);
    } else {
      // This is unexpected, but we don't guarantee stable compilation across
      // JIT runs so just warn about it.
      ScopedObjectAccess soa(Thread::Current());
      LOG(WARNING) << "Missing inline cache for " << GetGraph()->GetArtMethod()->PrettyMethod();
    }
  }
}

void InstructionCodeGeneratorARM64::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  LocationSummary* locations = invoke->GetLocations();
  Register temp = XRegisterFrom(locations->GetTemp(0));
  Location receiver = locations->InAt(0);
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize);

  // Ensure that between load and MaybeRecordImplicitNullCheck there are no pools emitted.
  if (receiver.IsStackSlot()) {
    __ Ldr(temp.W(), StackOperandFrom(receiver));
    {
      EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
      // /* HeapReference<Class> */ temp = temp->klass_
      __ Ldr(temp.W(), HeapOperand(temp.W(), class_offset));
      codegen_->MaybeRecordImplicitNullCheck(invoke);
    }
  } else {
    EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
    // /* HeapReference<Class> */ temp = receiver->klass_
    __ Ldr(temp.W(), HeapOperandFrom(receiver, class_offset));
    codegen_->MaybeRecordImplicitNullCheck(invoke);
  }

  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  GetAssembler()->MaybeUnpoisonHeapReference(temp.W());

  // If we're compiling baseline, update the inline cache.
  codegen_->MaybeGenerateInlineCacheCheck(invoke, temp);

  // The register ip1 is required to be used for the hidden argument in
  // art_quick_imt_conflict_trampoline, so prevent VIXL from using it.
  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope scratch_scope(masm);
  scratch_scope.Exclude(ip1);
  if (invoke->GetHiddenArgumentLoadKind() == MethodLoadKind::kRecursive) {
    Location interface_method = locations->InAt(invoke->GetNumberOfArguments() - 1);
    if (interface_method.IsStackSlot()) {
      __ Ldr(ip1, StackOperandFrom(interface_method));
    } else {
      __ Mov(ip1, XRegisterFrom(interface_method));
    }
  // If the load kind is through a runtime call, we will pass the method we
  // fetch the IMT, which will either be a no-op if we don't hit the conflict
  // stub, or will make us always go through the trampoline when there is a
  // conflict.
  } else if (invoke->GetHiddenArgumentLoadKind() != MethodLoadKind::kRuntimeCall) {
    codegen_->LoadMethod(
        invoke->GetHiddenArgumentLoadKind(), Location::RegisterLocation(ip1.GetCode()), invoke);
  }

  __ Ldr(temp,
      MemOperand(temp, mirror::Class::ImtPtrOffset(kArm64PointerSize).Uint32Value()));
  uint32_t method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
      invoke->GetImtIndex(), kArm64PointerSize));
  // temp = temp->GetImtEntryAt(method_offset);
  __ Ldr(temp, MemOperand(temp, method_offset));
  if (invoke->GetHiddenArgumentLoadKind() == MethodLoadKind::kRuntimeCall) {
    // We pass the method from the IMT in case of a conflict. This will ensure
    // we go into the runtime to resolve the actual method.
    __ Mov(ip1, temp);
  }
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, MemOperand(temp, entry_point.Int32Value()));

  {
    // Ensure the pc position is recorded immediately after the `blr` instruction.
    ExactAssemblyScope eas(GetVIXLAssembler(), kInstructionSize, CodeBufferCheckScope::kExactSize);

    // lr();
    __ blr(lr);
    DCHECK(!codegen_->IsLeafMethod());
    codegen_->RecordPcInfo(invoke);
  }

  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void LocationsBuilderARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  IntrinsicLocationsBuilderARM64 intrinsic(GetGraph()->GetAllocator(), codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void LocationsBuilderARM64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  IntrinsicLocationsBuilderARM64 intrinsic(GetGraph()->GetAllocator(), codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  if (invoke->GetCodePtrLocation() == CodePtrLocation::kCallCriticalNative) {
    CriticalNativeCallingConventionVisitorARM64 calling_convention_visitor(
        /*for_register_allocation=*/ true);
    CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
  } else {
    HandleInvoke(invoke);
  }
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorARM64* codegen) {
  if (invoke->GetLocations()->Intrinsified()) {
    IntrinsicCodeGeneratorARM64 intrinsic(codegen);
    intrinsic.Dispatch(invoke);
    return true;
  }
  return false;
}

HInvokeStaticOrDirect::DispatchInfo CodeGeneratorARM64::GetSupportedInvokeStaticOrDirectDispatch(
    const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
    [[maybe_unused]] ArtMethod* method) {
  // On ARM64 we support all dispatch types.
  return desired_dispatch_info;
}

void CodeGeneratorARM64::LoadMethod(MethodLoadKind load_kind, Location temp, HInvoke* invoke) {
  switch (load_kind) {
    case MethodLoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension());
      // Add ADRP with its PC-relative method patch.
      vixl::aarch64::Label* adrp_label =
          NewBootImageMethodPatch(invoke->GetResolvedMethodReference());
      EmitAdrpPlaceholder(adrp_label, XRegisterFrom(temp));
      // Add ADD with its PC-relative method patch.
      vixl::aarch64::Label* add_label =
          NewBootImageMethodPatch(invoke->GetResolvedMethodReference(), adrp_label);
      EmitAddPlaceholder(add_label, XRegisterFrom(temp), XRegisterFrom(temp));
      break;
    }
    case MethodLoadKind::kBootImageRelRo: {
      // Note: Boot image is in the low 4GiB and the entry is 32-bit, so emit a 32-bit load.
      uint32_t boot_image_offset = GetBootImageOffset(invoke);
      LoadBootImageRelRoEntry(WRegisterFrom(temp), boot_image_offset);
      break;
    }
    case MethodLoadKind::kAppImageRelRo: {
      DCHECK(GetCompilerOptions().IsAppImage());
      // Add ADRP with its PC-relative method patch.
      vixl::aarch64::Label* adrp_label =
          NewAppImageMethodPatch(invoke->GetResolvedMethodReference());
      EmitAdrpPlaceholder(adrp_label, XRegisterFrom(temp));
      // Add LDR with its PC-relative method patch.
      // Note: App image is in the low 4GiB and the entry is 32-bit, so emit a 32-bit load.
      vixl::aarch64::Label* ldr_label =
          NewAppImageMethodPatch(invoke->GetResolvedMethodReference(), adrp_label);
      EmitLdrOffsetPlaceholder(ldr_label, WRegisterFrom(temp), XRegisterFrom(temp));
      break;
    }
    case MethodLoadKind::kBssEntry: {
      // Add ADRP with its PC-relative .bss entry patch.
      vixl::aarch64::Label* adrp_label = NewMethodBssEntryPatch(invoke->GetMethodReference());
      EmitAdrpPlaceholder(adrp_label, XRegisterFrom(temp));
      // Add LDR with its PC-relative .bss entry patch.
      vixl::aarch64::Label* ldr_label =
          NewMethodBssEntryPatch(invoke->GetMethodReference(), adrp_label);
      // All aligned loads are implicitly atomic consume operations on ARM64.
      EmitLdrOffsetPlaceholder(ldr_label, XRegisterFrom(temp), XRegisterFrom(temp));
      break;
    }
    case MethodLoadKind::kJitDirectAddress: {
      // Load method address from literal pool.
      __ Ldr(XRegisterFrom(temp),
             jit_patches_.DeduplicateUint64Literal(
                 reinterpret_cast<uint64_t>(invoke->GetResolvedMethod())));
      break;
    }
    case MethodLoadKind::kRuntimeCall: {
      // Test situation, don't do anything.
      break;
    }
    default: {
      LOG(FATAL) << "Load kind should have already been handled " << load_kind;
      UNREACHABLE();
    }
  }
}

void CodeGeneratorARM64::GenerateStaticOrDirectCall(
    HInvokeStaticOrDirect* invoke, Location temp, SlowPathCode* slow_path) {
  // Make sure that ArtMethod* is passed in kArtMethodRegister as per the calling convention.
  Location callee_method = temp;  // For all kinds except kRecursive, callee will be in temp.
  switch (invoke->GetMethodLoadKind()) {
    case MethodLoadKind::kStringInit: {
      uint32_t offset =
          GetThreadOffset<kArm64PointerSize>(invoke->GetStringInitEntryPoint()).Int32Value();
      // temp = thread->string_init_entrypoint
      __ Ldr(XRegisterFrom(temp), MemOperand(tr, offset));
      break;
    }
    case MethodLoadKind::kRecursive:
      callee_method = invoke->GetLocations()->InAt(invoke->GetCurrentMethodIndex());
      break;
    case MethodLoadKind::kRuntimeCall:
      GenerateInvokeStaticOrDirectRuntimeCall(invoke, temp, slow_path);
      return;  // No code pointer retrieval; the runtime performs the call directly.
    case MethodLoadKind::kBootImageLinkTimePcRelative:
      DCHECK(GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension());
      if (invoke->GetCodePtrLocation() == CodePtrLocation::kCallCriticalNative) {
        // Do not materialize the method pointer, load directly the entrypoint.
        // Add ADRP with its PC-relative JNI entrypoint patch.
        vixl::aarch64::Label* adrp_label =
            NewBootImageJniEntrypointPatch(invoke->GetResolvedMethodReference());
        EmitAdrpPlaceholder(adrp_label, lr);
        // Add the LDR with its PC-relative method patch.
        vixl::aarch64::Label* add_label =
            NewBootImageJniEntrypointPatch(invoke->GetResolvedMethodReference(), adrp_label);
        EmitLdrOffsetPlaceholder(add_label, lr, lr);
        break;
      }
      FALLTHROUGH_INTENDED;
    default:
      LoadMethod(invoke->GetMethodLoadKind(), temp, invoke);
      break;
  }

  auto call_lr = [&]() {
    // Use a scope to help guarantee that `RecordPcInfo()` records the correct pc.
    ExactAssemblyScope eas(GetVIXLAssembler(),
                           kInstructionSize,
                           CodeBufferCheckScope::kExactSize);
    // lr()
    __ blr(lr);
    RecordPcInfo(invoke, slow_path);
  };
  switch (invoke->GetCodePtrLocation()) {
    case CodePtrLocation::kCallSelf:
      {
        DCHECK(!GetGraph()->HasShouldDeoptimizeFlag());
        // Use a scope to help guarantee that `RecordPcInfo()` records the correct pc.
        ExactAssemblyScope eas(GetVIXLAssembler(),
                               kInstructionSize,
                               CodeBufferCheckScope::kExactSize);
        __ bl(&frame_entry_label_);
        RecordPcInfo(invoke, slow_path);
      }
      break;
    case CodePtrLocation::kCallCriticalNative: {
      size_t out_frame_size =
          PrepareCriticalNativeCall<CriticalNativeCallingConventionVisitorARM64,
                                    kAapcs64StackAlignment,
                                    GetCriticalNativeDirectCallFrameSize>(invoke);
      if (invoke->GetMethodLoadKind() == MethodLoadKind::kBootImageLinkTimePcRelative) {
        call_lr();
      } else {
        // LR = callee_method->ptr_sized_fields_.data_;  // EntryPointFromJni
        MemberOffset offset = ArtMethod::EntryPointFromJniOffset(kArm64PointerSize);
        __ Ldr(lr, MemOperand(XRegisterFrom(callee_method), offset.Int32Value()));
        // lr()
        call_lr();
      }
      // Zero-/sign-extend the result when needed due to native and managed ABI mismatch.
      switch (invoke->GetType()) {
        case DataType::Type::kBool:
          __ Ubfx(w0, w0, 0, 8);
          break;
        case DataType::Type::kInt8:
          __ Sbfx(w0, w0, 0, 8);
          break;
        case DataType::Type::kUint16:
          __ Ubfx(w0, w0, 0, 16);
          break;
        case DataType::Type::kInt16:
          __ Sbfx(w0, w0, 0, 16);
          break;
        case DataType::Type::kInt32:
        case DataType::Type::kInt64:
        case DataType::Type::kFloat32:
        case DataType::Type::kFloat64:
        case DataType::Type::kVoid:
          break;
        default:
          DCHECK(false) << invoke->GetType();
          break;
      }
      if (out_frame_size != 0u) {
        DecreaseFrame(out_frame_size);
      }
      break;
    }
    case CodePtrLocation::kCallArtMethod: {
      // LR = callee_method->ptr_sized_fields_.entry_point_from_quick_compiled_code_;
      MemberOffset offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize);
      __ Ldr(lr, MemOperand(XRegisterFrom(callee_method), offset.Int32Value()));
      // lr()
      call_lr();
      break;
    }
  }

  DCHECK(!IsLeafMethod());
}

void CodeGeneratorARM64::GenerateVirtualCall(
    HInvokeVirtual* invoke, Location temp_in, SlowPathCode* slow_path) {
  // Use the calling convention instead of the location of the receiver, as
  // intrinsics may have put the receiver in a different register. In the intrinsics
  // slow path, the arguments have been moved to the right place, so here we are
  // guaranteed that the receiver is the first register of the calling convention.
  InvokeDexCallingConvention calling_convention;
  Register receiver = calling_convention.GetRegisterAt(0);
  Register temp = XRegisterFrom(temp_in);
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kArm64PointerSize).SizeValue();
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64PointerSize);

  DCHECK(receiver.IsRegister());

  {
    // Ensure that between load and MaybeRecordImplicitNullCheck there are no pools emitted.
    EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
    // /* HeapReference<Class> */ temp = receiver->klass_
    __ Ldr(temp.W(), HeapOperandFrom(LocationFrom(receiver), class_offset));
    MaybeRecordImplicitNullCheck(invoke);
  }
  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  GetAssembler()->MaybeUnpoisonHeapReference(temp.W());

  // If we're compiling baseline, update the inline cache.
  MaybeGenerateInlineCacheCheck(invoke, temp);

  // temp = temp->GetMethodAt(method_offset);
  __ Ldr(temp, MemOperand(temp, method_offset));
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, MemOperand(temp, entry_point.SizeValue()));
  {
    // Use a scope to help guarantee that `RecordPcInfo()` records the correct pc.
    ExactAssemblyScope eas(GetVIXLAssembler(), kInstructionSize, CodeBufferCheckScope::kExactSize);
    // lr();
    __ blr(lr);
    RecordPcInfo(invoke, slow_path);
  }
}

void CodeGeneratorARM64::MoveFromReturnRegister(Location trg, DataType::Type type) {
  if (!trg.IsValid()) {
    DCHECK(type == DataType::Type::kVoid);
    return;
  }

  DCHECK_NE(type, DataType::Type::kVoid);

  if (DataType::IsIntegralType(type) || type == DataType::Type::kReference) {
    Register trg_reg = RegisterFrom(trg, type);
    Register res_reg = RegisterFrom(ARM64ReturnLocation(type), type);
    __ Mov(trg_reg, res_reg, kDiscardForSameWReg);
  } else {
    VRegister trg_reg = FPRegisterFrom(trg, type);
    VRegister res_reg = FPRegisterFrom(ARM64ReturnLocation(type), type);
    __ Fmov(trg_reg, res_reg);
  }
}

void LocationsBuilderARM64::VisitInvokePolymorphic(HInvokePolymorphic* invoke) {
  IntrinsicLocationsBuilderARM64 intrinsic(GetGraph()->GetAllocator(), codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM64::VisitInvokePolymorphic(HInvokePolymorphic* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
    return;
  }
  codegen_->GenerateInvokePolymorphicCall(invoke);
  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void LocationsBuilderARM64::VisitInvokeCustom(HInvokeCustom* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM64::VisitInvokeCustom(HInvokeCustom* invoke) {
  codegen_->GenerateInvokeCustomCall(invoke);
  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewBootImageIntrinsicPatch(
    uint32_t intrinsic_data,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(
      /* dex_file= */ nullptr, intrinsic_data, adrp_label, &boot_image_other_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewBootImageRelRoPatch(
    uint32_t boot_image_offset,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(
      /* dex_file= */ nullptr, boot_image_offset, adrp_label, &boot_image_other_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewBootImageMethodPatch(
    MethodReference target_method,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(
      target_method.dex_file, target_method.index, adrp_label, &boot_image_method_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewAppImageMethodPatch(
    MethodReference target_method,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(
      target_method.dex_file, target_method.index, adrp_label, &app_image_method_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewMethodBssEntryPatch(
    MethodReference target_method,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(
      target_method.dex_file, target_method.index, adrp_label, &method_bss_entry_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewBootImageTypePatch(
    const DexFile& dex_file,
    dex::TypeIndex type_index,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(&dex_file, type_index.index_, adrp_label, &boot_image_type_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewAppImageTypePatch(
    const DexFile& dex_file,
    dex::TypeIndex type_index,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(&dex_file, type_index.index_, adrp_label, &app_image_type_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewBssEntryTypePatch(
    HLoadClass* load_class,
    vixl::aarch64::Label* adrp_label) {
  const DexFile& dex_file = load_class->GetDexFile();
  dex::TypeIndex type_index = load_class->GetTypeIndex();
  ArenaDeque<PcRelativePatchInfo>* patches = nullptr;
  switch (load_class->GetLoadKind()) {
    case HLoadClass::LoadKind::kBssEntry:
      patches = &type_bss_entry_patches_;
      break;
    case HLoadClass::LoadKind::kBssEntryPublic:
      patches = &public_type_bss_entry_patches_;
      break;
    case HLoadClass::LoadKind::kBssEntryPackage:
      patches = &package_type_bss_entry_patches_;
      break;
    default:
      LOG(FATAL) << "Unexpected load kind: " << load_class->GetLoadKind();
      UNREACHABLE();
  }
  return NewPcRelativePatch(&dex_file, type_index.index_, adrp_label, patches);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewBootImageStringPatch(
    const DexFile& dex_file,
    dex::StringIndex string_index,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(
      &dex_file, string_index.index_, adrp_label, &boot_image_string_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewStringBssEntryPatch(
    const DexFile& dex_file,
    dex::StringIndex string_index,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(&dex_file, string_index.index_, adrp_label, &string_bss_entry_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewMethodTypeBssEntryPatch(
    HLoadMethodType* load_method_type,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(&load_method_type->GetDexFile(),
                            load_method_type->GetProtoIndex().index_,
                            adrp_label,
                            &method_type_bss_entry_patches_);
}

vixl::aarch64::Label* CodeGeneratorARM64::NewBootImageJniEntrypointPatch(
    MethodReference target_method,
    vixl::aarch64::Label* adrp_label) {
  return NewPcRelativePatch(
      target_method.dex_file, target_method.index, adrp_label, &boot_image_jni_entrypoint_patches_);
}

void CodeGeneratorARM64::EmitEntrypointThunkCall(ThreadOffset64 entrypoint_offset) {
  DCHECK(!__ AllowMacroInstructions());  // In ExactAssemblyScope.
  DCHECK(!GetCompilerOptions().IsJitCompiler());
  call_entrypoint_patches_.emplace_back(/*dex_file*/ nullptr, entrypoint_offset.Uint32Value());
  vixl::aarch64::Label* bl_label = &call_entrypoint_patches_.back().label;
  __ bind(bl_label);
  __ bl(static_cast<int64_t>(0));  // Placeholder, patched at link-time.
}

void CodeGeneratorARM64::EmitBakerReadBarrierCbnz(uint32_t custom_data) {
  DCHECK(!__ AllowMacroInstructions());  // In ExactAssemblyScope.
  if (GetCompilerOptions().IsJitCompiler()) {
    auto it = jit_baker_read_barrier_slow_paths_.FindOrAdd(custom_data);
    vixl::aarch64::Label* slow_path_entry = &it->second.label;
    __ cbnz(mr, slow_path_entry);
  } else {
    baker_read_barrier_patches_.emplace_back(custom_data);
    vixl::aarch64::Label* cbnz_label = &baker_read_barrier_patches_.back().label;
    __ bind(cbnz_label);
    __ cbnz(mr, static_cast<int64_t>(0));  // Placeholder, patched at link-time.
  }
}

vixl::aarch64::Label* CodeGeneratorARM64::NewPcRelativePatch(
    const DexFile* dex_file,
    uint32_t offset_or_index,
    vixl::aarch64::Label* adrp_label,
    ArenaDeque<PcRelativePatchInfo>* patches) {
  // Add a patch entry and return the label.
  patches->emplace_back(dex_file, offset_or_index);
  PcRelativePatchInfo* info = &patches->back();
  vixl::aarch64::Label* label = &info->label;
  // If adrp_label is null, this is the ADRP patch and needs to point to its own label.
  info->pc_insn_label = (adrp_label != nullptr) ? adrp_label : label;
  return label;
}

void CodeGeneratorARM64::EmitJitRootPatches(uint8_t* code, const uint8_t* roots_data) {
  jit_patches_.EmitJitRootPatches(code, roots_data, *GetCodeGenerationData());
}

void CodeGeneratorARM64::EmitAdrpPlaceholder(vixl::aarch64::Label* fixup_label,
                                             vixl::aarch64::Register reg) {
  DCHECK(reg.IsX());
  SingleEmissionCheckScope guard(GetVIXLAssembler());
  __ Bind(fixup_label);
  __ adrp(reg, /* offset placeholder */ static_cast<int64_t>(0));
}

void CodeGeneratorARM64::EmitAddPlaceholder(vixl::aarch64::Label* fixup_label,
                                            vixl::aarch64::Register out,
                                            vixl::aarch64::Register base) {
  DCHECK(out.IsX());
  DCHECK(base.IsX());
  SingleEmissionCheckScope guard(GetVIXLAssembler());
  __ Bind(fixup_label);
  __ add(out, base, Operand(/* offset placeholder */ 0));
}

void CodeGeneratorARM64::EmitLdrOffsetPlaceholder(vixl::aarch64::Label* fixup_label,
                                                  vixl::aarch64::Register out,
                                                  vixl::aarch64::Register base) {
  DCHECK(base.IsX());
  SingleEmissionCheckScope guard(GetVIXLAssembler());
  __ Bind(fixup_label);
  __ ldr(out, MemOperand(base, /* offset placeholder */ 0));
}

void CodeGeneratorARM64::LoadBootImageRelRoEntry(vixl::aarch64::Register reg,
                                                 uint32_t boot_image_offset) {
  DCHECK(reg.IsW());
  // Add ADRP with its PC-relative boot image .data.img.rel.ro patch.
  vixl::aarch64::Label* adrp_label = NewBootImageRelRoPatch(boot_image_offset);
  EmitAdrpPlaceholder(adrp_label, reg.X());
  // Add LDR with its PC-relative boot image .data.img.rel.ro patch.
  vixl::aarch64::Label* ldr_label = NewBootImageRelRoPatch(boot_image_offset, adrp_label);
  EmitLdrOffsetPlaceholder(ldr_label, reg.W(), reg.X());
}

void CodeGeneratorARM64::LoadBootImageAddress(vixl::aarch64::Register reg,
                                              uint32_t boot_image_reference) {
  if (GetCompilerOptions().IsBootImage()) {
    // Add ADRP with its PC-relative type patch.
    vixl::aarch64::Label* adrp_label = NewBootImageIntrinsicPatch(boot_image_reference);
    EmitAdrpPlaceholder(adrp_label, reg.X());
    // Add ADD with its PC-relative type patch.
    vixl::aarch64::Label* add_label = NewBootImageIntrinsicPatch(boot_image_reference, adrp_label);
    EmitAddPlaceholder(add_label, reg.X(), reg.X());
  } else if (GetCompilerOptions().GetCompilePic()) {
    LoadBootImageRelRoEntry(reg, boot_image_reference);
  } else {
    DCHECK(GetCompilerOptions().IsJitCompiler());
    gc::Heap* heap = Runtime::Current()->GetHeap();
    DCHECK(!heap->GetBootImageSpaces().empty());
    const uint8_t* address = heap->GetBootImageSpaces()[0]->Begin() + boot_image_reference;
    __ Ldr(reg.W(), DeduplicateBootImageAddressLiteral(reinterpret_cast<uintptr_t>(address)));
  }
}

void CodeGeneratorARM64::LoadTypeForBootImageIntrinsic(vixl::aarch64::Register reg,
                                                       TypeReference target_type) {
  // Load the type the same way as for HLoadClass::LoadKind::kBootImageLinkTimePcRelative.
  DCHECK(GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension());
  // Add ADRP with its PC-relative type patch.
  vixl::aarch64::Label* adrp_label =
      NewBootImageTypePatch(*target_type.dex_file, target_type.TypeIndex());
  EmitAdrpPlaceholder(adrp_label, reg.X());
  // Add ADD with its PC-relative type patch.
  vixl::aarch64::Label* add_label =
      NewBootImageTypePatch(*target_type.dex_file, target_type.TypeIndex(), adrp_label);
  EmitAddPlaceholder(add_label, reg.X(), reg.X());
}

void CodeGeneratorARM64::LoadIntrinsicDeclaringClass(vixl::aarch64::Register reg, HInvoke* invoke) {
  DCHECK_NE(invoke->GetIntrinsic(), Intrinsics::kNone);
  if (GetCompilerOptions().IsBootImage()) {
    MethodReference target_method = invoke->GetResolvedMethodReference();
    dex::TypeIndex type_idx = target_method.dex_file->GetMethodId(target_method.index).class_idx_;
    LoadTypeForBootImageIntrinsic(reg, TypeReference(target_method.dex_file, type_idx));
  } else {
    uint32_t boot_image_offset = GetBootImageOffsetOfIntrinsicDeclaringClass(invoke);
    LoadBootImageAddress(reg, boot_image_offset);
  }
}

void CodeGeneratorARM64::LoadClassRootForIntrinsic(vixl::aarch64::Register reg,
                                                   ClassRoot class_root) {
  if (GetCompilerOptions().IsBootImage()) {
    ScopedObjectAccess soa(Thread::Current());
    ObjPtr<mirror::Class> klass = GetClassRoot(class_root);
    TypeReference target_type(&klass->GetDexFile(), klass->GetDexTypeIndex());
    LoadTypeForBootImageIntrinsic(reg, target_type);
  } else {
    uint32_t boot_image_offset = GetBootImageOffset(class_root);
    LoadBootImageAddress(reg, boot_image_offset);
  }
}

template <linker::LinkerPatch (*Factory)(size_t, const DexFile*, uint32_t, uint32_t)>
inline void CodeGeneratorARM64::EmitPcRelativeLinkerPatches(
    const ArenaDeque<PcRelativePatchInfo>& infos,
    ArenaVector<linker::LinkerPatch>* linker_patches) {
  for (const PcRelativePatchInfo& info : infos) {
    linker_patches->push_back(Factory(info.label.GetLocation(),
                                      info.target_dex_file,
                                      info.pc_insn_label->GetLocation(),
                                      info.offset_or_index));
  }
}

template <linker::LinkerPatch (*Factory)(size_t, uint32_t, uint32_t)>
linker::LinkerPatch NoDexFileAdapter(size_t literal_offset,
                                     const DexFile* target_dex_file,
                                     uint32_t pc_insn_offset,
                                     uint32_t boot_image_offset) {
  DCHECK(target_dex_file == nullptr);  // Unused for these patches, should be null.
  return Factory(literal_offset, pc_insn_offset, boot_image_offset);
}

void CodeGeneratorARM64::EmitLinkerPatches(ArenaVector<linker::LinkerPatch>* linker_patches) {
  DCHECK(linker_patches->empty());
  size_t size =
      boot_image_method_patches_.size() +
      app_image_method_patches_.size() +
      method_bss_entry_patches_.size() +
      boot_image_type_patches_.size() +
      app_image_type_patches_.size() +
      type_bss_entry_patches_.size() +
      public_type_bss_entry_patches_.size() +
      package_type_bss_entry_patches_.size() +
      boot_image_string_patches_.size() +
      string_bss_entry_patches_.size() +
      method_type_bss_entry_patches_.size() +
      boot_image_jni_entrypoint_patches_.size() +
      boot_image_other_patches_.size() +
      call_entrypoint_patches_.size() +
      baker_read_barrier_patches_.size();
  linker_patches->reserve(size);
  if (GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension()) {
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeMethodPatch>(
        boot_image_method_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeTypePatch>(
        boot_image_type_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeStringPatch>(
        boot_image_string_patches_, linker_patches);
  } else {
    DCHECK(boot_image_method_patches_.empty());
    DCHECK(boot_image_type_patches_.empty());
    DCHECK(boot_image_string_patches_.empty());
  }
  DCHECK_IMPLIES(!GetCompilerOptions().IsAppImage(), app_image_method_patches_.empty());
  DCHECK_IMPLIES(!GetCompilerOptions().IsAppImage(), app_image_type_patches_.empty());
  if (GetCompilerOptions().IsBootImage()) {
    EmitPcRelativeLinkerPatches<NoDexFileAdapter<linker::LinkerPatch::IntrinsicReferencePatch>>(
        boot_image_other_patches_, linker_patches);
  } else {
    EmitPcRelativeLinkerPatches<NoDexFileAdapter<linker::LinkerPatch::BootImageRelRoPatch>>(
        boot_image_other_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodAppImageRelRoPatch>(
        app_image_method_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::TypeAppImageRelRoPatch>(
        app_image_type_patches_, linker_patches);
  }
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodBssEntryPatch>(
      method_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::TypeBssEntryPatch>(
      type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::PublicTypeBssEntryPatch>(
      public_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::PackageTypeBssEntryPatch>(
      package_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::StringBssEntryPatch>(
      string_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodTypeBssEntryPatch>(
      method_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeJniEntrypointPatch>(
      boot_image_jni_entrypoint_patches_, linker_patches);
  for (const PatchInfo<vixl::aarch64::Label>& info : call_entrypoint_patches_) {
    DCHECK(info.target_dex_file == nullptr);
    linker_patches->push_back(linker::LinkerPatch::CallEntrypointPatch(
        info.label.GetLocation(), info.offset_or_index));
  }
  for (const BakerReadBarrierPatchInfo& info : baker_read_barrier_patches_) {
    linker_patches->push_back(linker::LinkerPatch::BakerReadBarrierBranchPatch(
        info.label.GetLocation(), info.custom_data));
  }
  DCHECK_EQ(size, linker_patches->size());
}

bool CodeGeneratorARM64::NeedsThunkCode(const linker::LinkerPatch& patch) const {
  return patch.GetType() == linker::LinkerPatch::Type::kCallEntrypoint ||
         patch.GetType() == linker::LinkerPatch::Type::kBakerReadBarrierBranch ||
         patch.GetType() == linker::LinkerPatch::Type::kCallRelative;
}

void CodeGeneratorARM64::EmitThunkCode(const linker::LinkerPatch& patch,
                                       /*out*/ ArenaVector<uint8_t>* code,
                                       /*out*/ std::string* debug_name) {
  Arm64Assembler assembler(GetGraph()->GetAllocator());
  switch (patch.GetType()) {
    case linker::LinkerPatch::Type::kCallRelative: {
      // The thunk just uses the entry point in the ArtMethod. This works even for calls
      // to the generic JNI and interpreter trampolines.
      Offset offset(ArtMethod::EntryPointFromQuickCompiledCodeOffset(
          kArm64PointerSize).Int32Value());
      assembler.JumpTo(ManagedRegister(arm64::X0), offset, ManagedRegister(arm64::IP0));
      if (debug_name != nullptr && GetCompilerOptions().GenerateAnyDebugInfo()) {
        *debug_name = "MethodCallThunk";
      }
      break;
    }
    case linker::LinkerPatch::Type::kCallEntrypoint: {
      Offset offset(patch.EntrypointOffset());
      assembler.JumpTo(ManagedRegister(arm64::TR), offset, ManagedRegister(arm64::IP0));
      if (debug_name != nullptr && GetCompilerOptions().GenerateAnyDebugInfo()) {
        *debug_name = "EntrypointCallThunk_" + std::to_string(offset.Uint32Value());
      }
      break;
    }
    case linker::LinkerPatch::Type::kBakerReadBarrierBranch: {
      DCHECK_EQ(patch.GetBakerCustomValue2(), 0u);
      CompileBakerReadBarrierThunk(assembler, patch.GetBakerCustomValue1(), debug_name);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected patch type " << patch.GetType();
      UNREACHABLE();
  }

  // Ensure we emit the literal pool if any.
  assembler.FinalizeCode();
  code->resize(assembler.CodeSize());
  MemoryRegion code_region(code->data(), code->size());
  assembler.CopyInstructions(code_region);
}

void InstructionCodeGeneratorARM64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
    return;
  }

  LocationSummary* locations = invoke->GetLocations();
  codegen_->GenerateStaticOrDirectCall(
      invoke, locations->HasTemps() ? locations->GetTemp(0) : Location::NoLocation());

  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void InstructionCodeGeneratorARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
    return;
  }

  codegen_->GenerateVirtualCall(invoke, invoke->GetLocations()->GetTemp(0));
  DCHECK(!codegen_->IsLeafMethod());

  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

HLoadClass::LoadKind CodeGeneratorARM64::GetSupportedLoadClassKind(
    HLoadClass::LoadKind desired_class_load_kind) {
  switch (desired_class_load_kind) {
    case HLoadClass::LoadKind::kInvalid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
    case HLoadClass::LoadKind::kReferrersClass:
      break;
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative:
    case HLoadClass::LoadKind::kBootImageRelRo:
    case HLoadClass::LoadKind::kAppImageRelRo:
    case HLoadClass::LoadKind::kBssEntry:
    case HLoadClass::LoadKind::kBssEntryPublic:
    case HLoadClass::LoadKind::kBssEntryPackage:
      DCHECK(!GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadClass::LoadKind::kJitBootImageAddress:
    case HLoadClass::LoadKind::kJitTableAddress:
      DCHECK(GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadClass::LoadKind::kRuntimeCall:
      break;
  }
  return desired_class_load_kind;
}

void LocationsBuilderARM64::VisitLoadClass(HLoadClass* cls) {
  HLoadClass::LoadKind load_kind = cls->GetLoadKind();
  if (load_kind == HLoadClass::LoadKind::kRuntimeCall) {
    InvokeRuntimeCallingConvention calling_convention;
    CodeGenerator::CreateLoadClassRuntimeCallLocationSummary(
        cls,
        LocationFrom(calling_convention.GetRegisterAt(0)),
        LocationFrom(vixl::aarch64::x0));
    DCHECK(calling_convention.GetRegisterAt(0).Is(vixl::aarch64::x0));
    return;
  }
  DCHECK_EQ(cls->NeedsAccessCheck(),
            load_kind == HLoadClass::LoadKind::kBssEntryPublic ||
                load_kind == HLoadClass::LoadKind::kBssEntryPackage);

  const bool requires_read_barrier = !cls->IsInImage() && codegen_->EmitReadBarrier();
  LocationSummary::CallKind call_kind = (cls->NeedsEnvironment() || requires_read_barrier)
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(cls, call_kind);
  if (kUseBakerReadBarrier && requires_read_barrier && !cls->NeedsEnvironment()) {
    locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
  }

  if (load_kind == HLoadClass::LoadKind::kReferrersClass) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
  locations->SetOut(Location::RequiresRegister());
  if (load_kind == HLoadClass::LoadKind::kBssEntry ||
      load_kind == HLoadClass::LoadKind::kBssEntryPublic ||
      load_kind == HLoadClass::LoadKind::kBssEntryPackage) {
    if (codegen_->EmitNonBakerReadBarrier()) {
      // For non-Baker read barrier we have a temp-clobbering call.
    } else {
      // Rely on the type resolution or initialization and marking to save everything we need.
      locations->SetCustomSlowPathCallerSaves(OneRegInReferenceOutSaveEverythingCallerSaves());
    }
  }
}

// NO_THREAD_SAFETY_ANALYSIS as we manipulate handles whose internal object we know does not
// move.
void InstructionCodeGeneratorARM64::VisitLoadClass(HLoadClass* cls) NO_THREAD_SAFETY_ANALYSIS {
  HLoadClass::LoadKind load_kind = cls->GetLoadKind();
  if (load_kind == HLoadClass::LoadKind::kRuntimeCall) {
    codegen_->GenerateLoadClassRuntimeCall(cls);
    codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
    return;
  }
  DCHECK_EQ(cls->NeedsAccessCheck(),
            load_kind == HLoadClass::LoadKind::kBssEntryPublic ||
                load_kind == HLoadClass::LoadKind::kBssEntryPackage);

  Location out_loc = cls->GetLocations()->Out();
  Register out = OutputRegister(cls);

  const ReadBarrierOption read_barrier_option =
      cls->IsInImage() ? kWithoutReadBarrier : codegen_->GetCompilerReadBarrierOption();
  bool generate_null_check = false;
  switch (load_kind) {
    case HLoadClass::LoadKind::kReferrersClass: {
      DCHECK(!cls->CanCallRuntime());
      DCHECK(!cls->MustGenerateClinitCheck());
      // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
      Register current_method = InputRegisterAt(cls, 0);
      codegen_->GenerateGcRootFieldLoad(cls,
                                        out_loc,
                                        current_method,
                                        ArtMethod::DeclaringClassOffset().Int32Value(),
                                        /* fixup_label= */ nullptr,
                                        read_barrier_option);
      break;
    }
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(codegen_->GetCompilerOptions().IsBootImage() ||
             codegen_->GetCompilerOptions().IsBootImageExtension());
      DCHECK_EQ(read_barrier_option, kWithoutReadBarrier);
      // Add ADRP with its PC-relative type patch.
      const DexFile& dex_file = cls->GetDexFile();
      dex::TypeIndex type_index = cls->GetTypeIndex();
      vixl::aarch64::Label* adrp_label = codegen_->NewBootImageTypePatch(dex_file, type_index);
      codegen_->EmitAdrpPlaceholder(adrp_label, out.X());
      // Add ADD with its PC-relative type patch.
      vixl::aarch64::Label* add_label =
          codegen_->NewBootImageTypePatch(dex_file, type_index, adrp_label);
      codegen_->EmitAddPlaceholder(add_label, out.X(), out.X());
      break;
    }
    case HLoadClass::LoadKind::kBootImageRelRo: {
      DCHECK(!codegen_->GetCompilerOptions().IsBootImage());
      uint32_t boot_image_offset = CodeGenerator::GetBootImageOffset(cls);
      codegen_->LoadBootImageRelRoEntry(out.W(), boot_image_offset);
      break;
    }
    case HLoadClass::LoadKind::kAppImageRelRo: {
      DCHECK(codegen_->GetCompilerOptions().IsAppImage());
      DCHECK_EQ(read_barrier_option, kWithoutReadBarrier);
      // Add ADRP with its PC-relative type patch.
      const DexFile& dex_file = cls->GetDexFile();
      dex::TypeIndex type_index = cls->GetTypeIndex();
      vixl::aarch64::Label* adrp_label = codegen_->NewAppImageTypePatch(dex_file, type_index);
      codegen_->EmitAdrpPlaceholder(adrp_label, out.X());
      // Add LDR with its PC-relative type patch.
      vixl::aarch64::Label* ldr_label =
          codegen_->NewAppImageTypePatch(dex_file, type_index, adrp_label);
      codegen_->EmitLdrOffsetPlaceholder(ldr_label, out.W(), out.X());
      break;
    }
    case HLoadClass::LoadKind::kBssEntry:
    case HLoadClass::LoadKind::kBssEntryPublic:
    case HLoadClass::LoadKind::kBssEntryPackage: {
      // Add ADRP with its PC-relative Class .bss entry patch.
      vixl::aarch64::Register temp = XRegisterFrom(out_loc);
      vixl::aarch64::Label* adrp_label = codegen_->NewBssEntryTypePatch(cls);
      codegen_->EmitAdrpPlaceholder(adrp_label, temp);
      // Add LDR with its PC-relative Class .bss entry patch.
      vixl::aarch64::Label* ldr_label = codegen_->NewBssEntryTypePatch(cls, adrp_label);
      // /* GcRoot<mirror::Class> */ out = *(base_address + offset)  /* PC-relative */
      // All aligned loads are implicitly atomic consume operations on ARM64.
      codegen_->GenerateGcRootFieldLoad(cls,
                                        out_loc,
                                        temp,
                                        /* offset placeholder */ 0u,
                                        ldr_label,
                                        read_barrier_option);
      generate_null_check = true;
      break;
    }
    case HLoadClass::LoadKind::kJitBootImageAddress: {
      DCHECK_EQ(read_barrier_option, kWithoutReadBarrier);
      uint32_t address = reinterpret_cast32<uint32_t>(cls->GetClass().Get());
      DCHECK_NE(address, 0u);
      __ Ldr(out.W(), codegen_->DeduplicateBootImageAddressLiteral(address));
      break;
    }
    case HLoadClass::LoadKind::kJitTableAddress: {
      __ Ldr(out, codegen_->DeduplicateJitClassLiteral(cls->GetDexFile(),
                                                       cls->GetTypeIndex(),
                                                       cls->GetClass()));
      codegen_->GenerateGcRootFieldLoad(cls,
                                        out_loc,
                                        out.X(),
                                        /* offset= */ 0,
                                        /* fixup_label= */ nullptr,
                                        read_barrier_option);
      break;
    }
    case HLoadClass::LoadKind::kRuntimeCall:
    case HLoadClass::LoadKind::kInvalid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }

  bool do_clinit = cls->MustGenerateClinitCheck();
  if (generate_null_check || do_clinit) {
    DCHECK(cls->CanCallRuntime());
    SlowPathCodeARM64* slow_path =
        new (codegen_->GetScopedAllocator()) LoadClassSlowPathARM64(cls, cls);
    codegen_->AddSlowPath(slow_path);
    if (generate_null_check) {
      __ Cbz(out, slow_path->GetEntryLabel());
    }
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
    codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
  }
}

void LocationsBuilderARM64::VisitLoadMethodHandle(HLoadMethodHandle* load) {
  InvokeRuntimeCallingConvention calling_convention;
  Location location = LocationFrom(calling_convention.GetRegisterAt(0));
  CodeGenerator::CreateLoadMethodHandleRuntimeCallLocationSummary(load, location, location);
}

void InstructionCodeGeneratorARM64::VisitLoadMethodHandle(HLoadMethodHandle* load) {
  codegen_->GenerateLoadMethodHandleRuntimeCall(load);
}

void LocationsBuilderARM64::VisitLoadMethodType(HLoadMethodType* load) {
  if (load->GetLoadKind() == HLoadMethodType::LoadKind::kRuntimeCall) {
    InvokeRuntimeCallingConvention calling_convention;
    Location location = LocationFrom(calling_convention.GetRegisterAt(0));
    CodeGenerator::CreateLoadMethodTypeRuntimeCallLocationSummary(load, location, location);
  } else {
    LocationSummary* locations =
        new (GetGraph()->GetAllocator()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
    locations->SetOut(Location::RequiresRegister());
    if (load->GetLoadKind() == HLoadMethodType::LoadKind::kBssEntry) {
      if (codegen_->EmitNonBakerReadBarrier()) {
        // For non-Baker read barrier we have a temp-clobbering call.
      } else {
        // Rely on the pResolveMethodType to save everything.
        locations->SetCustomSlowPathCallerSaves(OneRegInReferenceOutSaveEverythingCallerSaves());
      }
    }
  }
}

void InstructionCodeGeneratorARM64::VisitLoadMethodType(HLoadMethodType* load) {
  Location out_loc = load->GetLocations()->Out();
  Register out = OutputRegister(load);

  switch (load->GetLoadKind()) {
    case HLoadMethodType::LoadKind::kBssEntry: {
      // Add ADRP with its PC-relative Class .bss entry patch.
      vixl::aarch64::Register temp = XRegisterFrom(out_loc);
      vixl::aarch64::Label* adrp_label = codegen_->NewMethodTypeBssEntryPatch(load);
      codegen_->EmitAdrpPlaceholder(adrp_label, temp);
      // Add LDR with its PC-relative MethodType .bss entry patch.
      vixl::aarch64::Label* ldr_label = codegen_->NewMethodTypeBssEntryPatch(load, adrp_label);
      // /* GcRoot<mirror::MethodType> */ out = *(base_address + offset)  /* PC-relative */
      // All aligned loads are implicitly atomic consume operations on ARM64.
      codegen_->GenerateGcRootFieldLoad(load,
                                        out_loc,
                                        temp,
                                        /* offset placeholder */ 0u,
                                        ldr_label,
                                        codegen_->GetCompilerReadBarrierOption());
      SlowPathCodeARM64* slow_path =
          new (codegen_->GetScopedAllocator()) LoadMethodTypeSlowPathARM64(load);
      codegen_->AddSlowPath(slow_path);
      __ Cbz(out, slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      codegen_->MaybeGenerateMarkingRegisterCheck(/* code = */ __LINE__);
      return;
    }
    case HLoadMethodType::LoadKind::kJitTableAddress: {
      __ Ldr(out, codegen_->DeduplicateJitMethodTypeLiteral(load->GetDexFile(),
                                                            load->GetProtoIndex(),
                                                            load->GetMethodType()));
      codegen_->GenerateGcRootFieldLoad(load,
                                        out_loc,
                                        out.X(),
                                        /* offset= */ 0,
                                        /* fixup_label= */ nullptr,
                                        codegen_->GetCompilerReadBarrierOption());
      return;
    }
    default:
      DCHECK_EQ(load->GetLoadKind(), HLoadMethodType::LoadKind::kRuntimeCall);
      codegen_->GenerateLoadMethodTypeRuntimeCall(load);
      break;
  }
}

static MemOperand GetExceptionTlsAddress() {
  return MemOperand(tr, Thread::ExceptionOffset<kArm64PointerSize>().Int32Value());
}

void LocationsBuilderARM64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadException(HLoadException* instruction) {
  __ Ldr(OutputRegister(instruction), GetExceptionTlsAddress());
}

void LocationsBuilderARM64::VisitClearException(HClearException* clear) {
  new (GetGraph()->GetAllocator()) LocationSummary(clear, LocationSummary::kNoCall);
}

void InstructionCodeGeneratorARM64::VisitClearException([[maybe_unused]] HClearException* clear) {
  __ Str(wzr, GetExceptionTlsAddress());
}

HLoadString::LoadKind CodeGeneratorARM64::GetSupportedLoadStringKind(
    HLoadString::LoadKind desired_string_load_kind) {
  switch (desired_string_load_kind) {
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
    case HLoadString::LoadKind::kBootImageRelRo:
    case HLoadString::LoadKind::kBssEntry:
      DCHECK(!GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadString::LoadKind::kJitBootImageAddress:
    case HLoadString::LoadKind::kJitTableAddress:
      DCHECK(GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadString::LoadKind::kRuntimeCall:
      break;
  }
  return desired_string_load_kind;
}

void LocationsBuilderARM64::VisitLoadString(HLoadString* load) {
  LocationSummary::CallKind call_kind = codegen_->GetLoadStringCallKind(load);
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(load, call_kind);
  if (load->GetLoadKind() == HLoadString::LoadKind::kRuntimeCall) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetOut(calling_convention.GetReturnLocation(load->GetType()));
  } else {
    locations->SetOut(Location::RequiresRegister());
    if (load->GetLoadKind() == HLoadString::LoadKind::kBssEntry) {
      if (codegen_->EmitNonBakerReadBarrier()) {
        // For non-Baker read barrier we have a temp-clobbering call.
      } else {
        // Rely on the pResolveString and marking to save everything we need.
        locations->SetCustomSlowPathCallerSaves(OneRegInReferenceOutSaveEverythingCallerSaves());
      }
    }
  }
}

// NO_THREAD_SAFETY_ANALYSIS as we manipulate handles whose internal object we know does not
// move.
void InstructionCodeGeneratorARM64::VisitLoadString(HLoadString* load) NO_THREAD_SAFETY_ANALYSIS {
  Register out = OutputRegister(load);
  Location out_loc = load->GetLocations()->Out();

  switch (load->GetLoadKind()) {
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(codegen_->GetCompilerOptions().IsBootImage() ||
             codegen_->GetCompilerOptions().IsBootImageExtension());
      // Add ADRP with its PC-relative String patch.
      const DexFile& dex_file = load->GetDexFile();
      const dex::StringIndex string_index = load->GetStringIndex();
      vixl::aarch64::Label* adrp_label = codegen_->NewBootImageStringPatch(dex_file, string_index);
      codegen_->EmitAdrpPlaceholder(adrp_label, out.X());
      // Add ADD with its PC-relative String patch.
      vixl::aarch64::Label* add_label =
          codegen_->NewBootImageStringPatch(dex_file, string_index, adrp_label);
      codegen_->EmitAddPlaceholder(add_label, out.X(), out.X());
      return;
    }
    case HLoadString::LoadKind::kBootImageRelRo: {
      DCHECK(!codegen_->GetCompilerOptions().IsBootImage());
      uint32_t boot_image_offset = CodeGenerator::GetBootImageOffset(load);
      codegen_->LoadBootImageRelRoEntry(out.W(), boot_image_offset);
      return;
    }
    case HLoadString::LoadKind::kBssEntry: {
      // Add ADRP with its PC-relative String .bss entry patch.
      const DexFile& dex_file = load->GetDexFile();
      const dex::StringIndex string_index = load->GetStringIndex();
      Register temp = XRegisterFrom(out_loc);
      vixl::aarch64::Label* adrp_label = codegen_->NewStringBssEntryPatch(dex_file, string_index);
      codegen_->EmitAdrpPlaceholder(adrp_label, temp);
      // Add LDR with its PC-relative String .bss entry patch.
      vixl::aarch64::Label* ldr_label =
          codegen_->NewStringBssEntryPatch(dex_file, string_index, adrp_label);
      // /* GcRoot<mirror::String> */ out = *(base_address + offset)  /* PC-relative */
      // All aligned loads are implicitly atomic consume operations on ARM64.
      codegen_->GenerateGcRootFieldLoad(load,
                                        out_loc,
                                        temp,
                                        /* offset placeholder */ 0u,
                                        ldr_label,
                                        codegen_->GetCompilerReadBarrierOption());
      SlowPathCodeARM64* slow_path =
          new (codegen_->GetScopedAllocator()) LoadStringSlowPathARM64(load);
      codegen_->AddSlowPath(slow_path);
      __ Cbz(out.X(), slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
      return;
    }
    case HLoadString::LoadKind::kJitBootImageAddress: {
      uint32_t address = reinterpret_cast32<uint32_t>(load->GetString().Get());
      DCHECK_NE(address, 0u);
      __ Ldr(out.W(), codegen_->DeduplicateBootImageAddressLiteral(address));
      return;
    }
    case HLoadString::LoadKind::kJitTableAddress: {
      __ Ldr(out, codegen_->DeduplicateJitStringLiteral(load->GetDexFile(),
                                                        load->GetStringIndex(),
                                                        load->GetString()));
      codegen_->GenerateGcRootFieldLoad(load,
                                        out_loc,
                                        out.X(),
                                        /* offset= */ 0,
                                        /* fixup_label= */ nullptr,
                                        codegen_->GetCompilerReadBarrierOption());
      return;
    }
    default:
      break;
  }

  InvokeRuntimeCallingConvention calling_convention;
  DCHECK_EQ(calling_convention.GetRegisterAt(0).GetCode(), out.GetCode());
  __ Mov(calling_convention.GetRegisterAt(0).W(), load->GetStringIndex().index_);
  codegen_->InvokeRuntime(kQuickResolveString, load);
  CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void LocationsBuilderARM64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitLongConstant([[maybe_unused]] HLongConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM64::VisitMonitorOperation(HMonitorOperation* instruction) {
  codegen_->InvokeRuntime(instruction->IsEnter() ? kQuickLockObject : kQuickUnlockObject,
                          instruction);
  if (instruction->IsEnter()) {
    CheckEntrypointTypes<kQuickLockObject, void, mirror::Object*>();
  } else {
    CheckEntrypointTypes<kQuickUnlockObject, void, mirror::Object*>();
  }
  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void LocationsBuilderARM64::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorARM64::VisitMul(HMul* mul) {
  switch (mul->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      __ Mul(OutputRegister(mul), InputRegisterAt(mul, 0), InputRegisterAt(mul, 1));
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      __ Fmul(OutputFPRegister(mul), InputFPRegisterAt(mul, 0), InputFPRegisterAt(mul, 1));
      break;

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, ARM64EncodableConstantOrRegister(neg->InputAt(0), neg));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorARM64::VisitNeg(HNeg* neg) {
  switch (neg->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      __ Neg(OutputRegister(neg), InputOperandAt(neg, 0));
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      __ Fneg(OutputFPRegister(neg), InputFPRegisterAt(neg, 0));
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetOut(LocationFrom(x0));
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
}

void InstructionCodeGeneratorARM64::VisitNewArray(HNewArray* instruction) {
  // Note: if heap poisoning is enabled, the entry point takes care of poisoning the reference.
  QuickEntrypointEnum entrypoint = CodeGenerator::GetArrayAllocationEntrypoint(instruction);
  codegen_->InvokeRuntime(entrypoint, instruction);
  CheckEntrypointTypes<kQuickAllocArrayResolved, void*, mirror::Class*, int32_t>();
  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void LocationsBuilderARM64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetOut(calling_convention.GetReturnLocation(DataType::Type::kReference));
}

void InstructionCodeGeneratorARM64::VisitNewInstance(HNewInstance* instruction) {
  codegen_->InvokeRuntime(instruction->GetEntrypoint(), instruction);
  CheckEntrypointTypes<kQuickAllocObjectWithChecks, void*, mirror::Class*>();
  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void LocationsBuilderARM64::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitNot(HNot* instruction) {
  switch (instruction->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      __ Mvn(OutputRegister(instruction), InputOperandAt(instruction, 0));
      break;

    default:
      LOG(FATAL) << "Unexpected type for not operation " << instruction->GetResultType();
  }
}

void LocationsBuilderARM64::VisitBooleanNot(HBooleanNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitBooleanNot(HBooleanNot* instruction) {
  __ Eor(OutputRegister(instruction), InputRegisterAt(instruction, 0), vixl::aarch64::Operand(1));
}

void LocationsBuilderARM64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
}

void CodeGeneratorARM64::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (CanMoveNullCheckToUser(instruction)) {
    return;
  }
  {
    // Ensure that between load and RecordPcInfo there are no pools emitted.
    EmissionCheckScope guard(GetVIXLAssembler(), kMaxMacroInstructionSizeInBytes);
    Location obj = instruction->GetLocations()->InAt(0);
    __ Ldr(wzr, HeapOperandFrom(obj, Offset(0)));
    RecordPcInfo(instruction);
  }
}

void CodeGeneratorARM64::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCodeARM64* slow_path = new (GetScopedAllocator()) NullCheckSlowPathARM64(instruction);
  AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  __ Cbz(RegisterFrom(obj, instruction->InputAt(0)->GetType()), slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorARM64::VisitNullCheck(HNullCheck* instruction) {
  codegen_->GenerateNullCheck(instruction);
}

void LocationsBuilderARM64::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitParallelMove([[maybe_unused]] HParallelMove* instruction) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64::VisitParallelMove(HParallelMove* instruction) {
  if (instruction->GetNext()->IsSuspendCheck() &&
      instruction->GetBlock()->GetLoopInformation() != nullptr) {
    HSuspendCheck* suspend_check = instruction->GetNext()->AsSuspendCheck();
    // The back edge will generate the suspend check.
    codegen_->ClearSpillSlotsFromLoopPhisInStackMap(suspend_check, instruction);
  }

  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderARM64::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorARM64::VisitParameterValue(
    [[maybe_unused]] HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderARM64::VisitCurrentMethod(HCurrentMethod* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetOut(LocationFrom(kArtMethodRegister));
}

void InstructionCodeGeneratorARM64::VisitCurrentMethod(
    [[maybe_unused]] HCurrentMethod* instruction) {
  // Nothing to do, the method is already at its location.
}

void LocationsBuilderARM64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  for (size_t i = 0, e = locations->GetInputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorARM64::VisitPhi([[maybe_unused]] HPhi* instruction) {
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderARM64::VisitRem(HRem* rem) {
  DataType::Type type = rem->GetResultType();
  LocationSummary::CallKind call_kind =
      DataType::IsFloatingPointType(type) ? LocationSummary::kCallOnMainOnly
                                           : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(rem, call_kind);

  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(rem->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, LocationFrom(calling_convention.GetFpuRegisterAt(0)));
      locations->SetInAt(1, LocationFrom(calling_convention.GetFpuRegisterAt(1)));
      locations->SetOut(calling_convention.GetReturnLocation(type));

      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void InstructionCodeGeneratorARM64::GenerateIntRemForPower2Denom(HRem *instruction) {
  int64_t imm = Int64FromLocation(instruction->GetLocations()->InAt(1));
  uint64_t abs_imm = static_cast<uint64_t>(AbsOrMin(imm));
  DCHECK(IsPowerOfTwo(abs_imm)) << abs_imm;

  Register out = OutputRegister(instruction);
  Register dividend = InputRegisterAt(instruction, 0);

  if (HasNonNegativeOrMinIntInputAt(instruction, 0)) {
    // No need to adjust the result for non-negative dividends or the INT32_MIN/INT64_MIN dividends.
    // NOTE: The generated code for HRem correctly works for the INT32_MIN/INT64_MIN dividends.
    // INT*_MIN % imm must be 0 for any imm of power 2. 'and' works only with bits
    // 0..30 (Int32 case)/0..62 (Int64 case) of a dividend. For INT32_MIN/INT64_MIN they are zeros.
    // So 'and' always produces zero.
    __ And(out, dividend, abs_imm - 1);
  } else {
    if (abs_imm == 2) {
      __ Cmp(dividend, 0);
      __ And(out, dividend, 1);
      __ Csneg(out, out, out, ge);
    } else {
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register temp = temps.AcquireSameSizeAs(out);

      __ Negs(temp, dividend);
      __ And(out, dividend, abs_imm - 1);
      __ And(temp, temp, abs_imm - 1);
      __ Csneg(out, out, temp, mi);
    }
  }
}

void InstructionCodeGeneratorARM64::GenerateIntRemForConstDenom(HRem *instruction) {
  int64_t imm = Int64FromLocation(instruction->GetLocations()->InAt(1));

  if (imm == 0) {
    // Do not generate anything.
    // DivZeroCheck would prevent any code to be executed.
    return;
  }

  if (IsPowerOfTwo(AbsOrMin(imm))) {
    // Cases imm == -1 or imm == 1 are handled in constant folding by
    // InstructionWithAbsorbingInputSimplifier.
    // If the cases have survided till code generation they are handled in
    // GenerateIntRemForPower2Denom becauses -1 and 1 are the power of 2 (2^0).
    // The correct code is generated for them, just more instructions.
    GenerateIntRemForPower2Denom(instruction);
  } else {
    DCHECK(imm < -2 || imm > 2) << imm;
    GenerateDivRemWithAnyConstant(instruction, imm);
  }
}

void InstructionCodeGeneratorARM64::GenerateIntRem(HRem* instruction) {
  DCHECK(DataType::IsIntOrLongType(instruction->GetResultType()))
         << instruction->GetResultType();

  if (instruction->GetLocations()->InAt(1).IsConstant()) {
    GenerateIntRemForConstDenom(instruction);
  } else {
    Register out = OutputRegister(instruction);
    Register dividend = InputRegisterAt(instruction, 0);
    Register divisor = InputRegisterAt(instruction, 1);
    UseScratchRegisterScope temps(GetVIXLAssembler());
    Register temp = temps.AcquireSameSizeAs(out);
    __ Sdiv(temp, dividend, divisor);
    __ Msub(out, temp, divisor, dividend);
  }
}

void InstructionCodeGeneratorARM64::VisitRem(HRem* rem) {
  DataType::Type type = rem->GetResultType();

  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      GenerateIntRem(rem);
      break;
    }

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      QuickEntrypointEnum entrypoint =
          (type == DataType::Type::kFloat32) ? kQuickFmodf : kQuickFmod;
      codegen_->InvokeRuntime(entrypoint, rem);
      if (type == DataType::Type::kFloat32) {
        CheckEntrypointTypes<kQuickFmodf, float, float, float>();
      } else {
        CheckEntrypointTypes<kQuickFmod, double, double, double>();
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
      UNREACHABLE();
  }
}

void LocationsBuilderARM64::VisitMin(HMin* min) {
  HandleBinaryOp(min);
}

void InstructionCodeGeneratorARM64::VisitMin(HMin* min) {
  HandleBinaryOp(min);
}

void LocationsBuilderARM64::VisitMax(HMax* max) {
  HandleBinaryOp(max);
}

void InstructionCodeGeneratorARM64::VisitMax(HMax* max) {
  HandleBinaryOp(max);
}

void LocationsBuilderARM64::VisitAbs(HAbs* abs) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(abs);
  switch (abs->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    default:
      LOG(FATAL) << "Unexpected type for abs operation " << abs->GetResultType();
  }
}

void InstructionCodeGeneratorARM64::VisitAbs(HAbs* abs) {
  switch (abs->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      Register in_reg = InputRegisterAt(abs, 0);
      Register out_reg = OutputRegister(abs);
      __ Cmp(in_reg, Operand(0));
      __ Cneg(out_reg, in_reg, lt);
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      VRegister in_reg = InputFPRegisterAt(abs, 0);
      VRegister out_reg = OutputFPRegister(abs);
      __ Fabs(out_reg, in_reg);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for abs operation " << abs->GetResultType();
  }
}

void LocationsBuilderARM64::VisitConstructorFence(HConstructorFence* constructor_fence) {
  constructor_fence->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitConstructorFence(
    [[maybe_unused]] HConstructorFence* constructor_fence) {
  codegen_->GenerateMemoryBarrier(MemBarrierKind::kStoreStore);
}

void LocationsBuilderARM64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  codegen_->GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderARM64::VisitReturn(HReturn* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  DataType::Type return_type = instruction->InputAt(0)->GetType();
  locations->SetInAt(0, ARM64ReturnLocation(return_type));
}

void InstructionCodeGeneratorARM64::VisitReturn(HReturn* ret) {
  if (GetGraph()->IsCompilingOsr()) {
    // To simplify callers of an OSR method, we put the return value in both
    // floating point and core register.
    switch (ret->InputAt(0)->GetType()) {
      case DataType::Type::kFloat32:
        __ Fmov(w0, s0);
        break;
      case DataType::Type::kFloat64:
        __ Fmov(x0, d0);
        break;
      default:
        break;
    }
  }
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM64::VisitReturnVoid(HReturnVoid* instruction) {
  instruction->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitReturnVoid([[maybe_unused]] HReturnVoid* instruction) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM64::VisitRol(HRol* rol) {
  HandleBinaryOp(rol);
}

void InstructionCodeGeneratorARM64::VisitRol(HRol* rol) {
  HandleBinaryOp(rol);
}

void LocationsBuilderARM64::VisitRor(HRor* ror) {
  HandleBinaryOp(ror);
}

void InstructionCodeGeneratorARM64::VisitRor(HRor* ror) {
  HandleBinaryOp(ror);
}

void LocationsBuilderARM64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorARM64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderARM64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorARM64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderARM64::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorARM64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction);
}

void InstructionCodeGeneratorARM64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction,
                 instruction->GetFieldInfo(),
                 instruction->GetValueCanBeNull(),
                 instruction->GetWriteBarrierKind());
}

void LocationsBuilderARM64::VisitStringBuilderAppend(HStringBuilderAppend* instruction) {
  codegen_->CreateStringBuilderAppendLocations(instruction, LocationFrom(x0));
}

void InstructionCodeGeneratorARM64::VisitStringBuilderAppend(HStringBuilderAppend* instruction) {
  __ Mov(w0, instruction->GetFormat()->GetValue());
  codegen_->InvokeRuntime(kQuickStringBuilderAppend, instruction);
}

void LocationsBuilderARM64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorARM64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          calling_convention);
}

void LocationsBuilderARM64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorARM64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          calling_convention);
}

void LocationsBuilderARM64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorARM64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          calling_convention);
}

void LocationsBuilderARM64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorARM64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          calling_convention);
}

void LocationsBuilderARM64::VisitSuspendCheck(HSuspendCheck* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnSlowPath);
  // In suspend check slow path, usually there are no caller-save registers at all.
  // If SIMD instructions are present, however, we force spilling all live SIMD
  // registers in full width (since the runtime only saves/restores lower part).
  // Note that only a suspend check can see live SIMD registers. In the
  // loop optimization, we make sure this does not happen for any other slow
  // path.
  locations->SetCustomSlowPathCallerSaves(
      GetGraph()->HasSIMD() ? RegisterSet::AllFpu() : RegisterSet::Empty());
}

void InstructionCodeGeneratorARM64::VisitSuspendCheck(HSuspendCheck* instruction) {
  HBasicBlock* block = instruction->GetBlock();
  if (block->GetLoopInformation() != nullptr) {
    DCHECK(block->GetLoopInformation()->GetSuspendCheck() == instruction);
    // The back edge will generate the suspend check.
    return;
  }
  if (block->IsEntryBlock() && instruction->GetNext()->IsGoto()) {
    // The goto will generate the suspend check.
    return;
  }
  GenerateSuspendCheck(instruction, nullptr);
  codegen_->MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void LocationsBuilderARM64::VisitThrow(HThrow* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM64::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(kQuickDeliverException, instruction);
  CheckEntrypointTypes<kQuickDeliverException, void, mirror::Object*>();
}

void LocationsBuilderARM64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(conversion, LocationSummary::kNoCall);
  DataType::Type input_type = conversion->GetInputType();
  DataType::Type result_type = conversion->GetResultType();
  DCHECK(!DataType::IsTypeConversionImplicit(input_type, result_type))
      << input_type << " -> " << result_type;
  if ((input_type == DataType::Type::kReference) || (input_type == DataType::Type::kVoid) ||
      (result_type == DataType::Type::kReference) || (result_type == DataType::Type::kVoid)) {
    LOG(FATAL) << "Unexpected type conversion from " << input_type << " to " << result_type;
  }

  if (DataType::IsFloatingPointType(input_type)) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
  }

  if (DataType::IsFloatingPointType(result_type)) {
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::VisitTypeConversion(HTypeConversion* conversion) {
  DataType::Type result_type = conversion->GetResultType();
  DataType::Type input_type = conversion->GetInputType();

  DCHECK(!DataType::IsTypeConversionImplicit(input_type, result_type))
      << input_type << " -> " << result_type;

  if (DataType::IsIntegralType(result_type) && DataType::IsIntegralType(input_type)) {
    int result_size = DataType::Size(result_type);
    int input_size = DataType::Size(input_type);
    int min_size = std::min(result_size, input_size);
    Register output = OutputRegister(conversion);
    Register source = InputRegisterAt(conversion, 0);
    if (result_type == DataType::Type::kInt32 && input_type == DataType::Type::kInt64) {
      // 'int' values are used directly as W registers, discarding the top
      // bits, so we don't need to sign-extend and can just perform a move.
      // We do not pass the `kDiscardForSameWReg` argument to force clearing the
      // top 32 bits of the target register. We theoretically could leave those
      // bits unchanged, but we would have to make sure that no code uses a
      // 32bit input value as a 64bit value assuming that the top 32 bits are
      // zero.
      __ Mov(output.W(), source.W());
    } else if (DataType::IsUnsignedType(result_type) ||
               (DataType::IsUnsignedType(input_type) && input_size < result_size)) {
      __ Ubfx(output, output.IsX() ? source.X() : source.W(), 0, result_size * kBitsPerByte);
    } else {
      __ Sbfx(output, output.IsX() ? source.X() : source.W(), 0, min_size * kBitsPerByte);
    }
  } else if (DataType::IsFloatingPointType(result_type) && DataType::IsIntegralType(input_type)) {
    __ Scvtf(OutputFPRegister(conversion), InputRegisterAt(conversion, 0));
  } else if (DataType::IsIntegralType(result_type) && DataType::IsFloatingPointType(input_type)) {
    CHECK(result_type == DataType::Type::kInt32 || result_type == DataType::Type::kInt64);
    __ Fcvtzs(OutputRegister(conversion), InputFPRegisterAt(conversion, 0));
  } else if (DataType::IsFloatingPointType(result_type) &&
             DataType::IsFloatingPointType(input_type)) {
    __ Fcvt(OutputFPRegister(conversion), InputFPRegisterAt(conversion, 0));
  } else {
    LOG(FATAL) << "Unexpected or unimplemented type conversion from " << input_type
                << " to " << result_type;
  }
}

void LocationsBuilderARM64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorARM64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderARM64::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitBoundType([[maybe_unused]] HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64::VisitBoundType([[maybe_unused]] HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

// Simple implementation of packed switch - generate cascaded compare/jumps.
void LocationsBuilderARM64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  Register value_reg = InputRegisterAt(switch_instr, 0);
  HBasicBlock* default_block = switch_instr->GetDefaultBlock();

  if (num_entries <= kPackedSwitchCompareJumpThreshold) {
    // Create a series of compare/jumps.
    UseScratchRegisterScope temps(codegen_->GetVIXLAssembler());
    Register temp = temps.AcquireW();
    __ Subs(temp, value_reg, Operand(lower_bound));

    const ArenaVector<HBasicBlock*>& successors = switch_instr->GetBlock()->GetSuccessors();
    // Jump to successors[0] if value == lower_bound.
    __ B(eq, codegen_->GetLabelOf(successors[0]));
    int32_t last_index = 0;
    for (; num_entries - last_index > 2; last_index += 2) {
      __ Subs(temp, temp, Operand(2));
      // Jump to successors[last_index + 1] if value < case_value[last_index + 2].
      __ B(lo, codegen_->GetLabelOf(successors[last_index + 1]));
      // Jump to successors[last_index + 2] if value == case_value[last_index + 2].
      __ B(eq, codegen_->GetLabelOf(successors[last_index + 2]));
    }
    if (num_entries - last_index == 2) {
      // The last missing case_value.
      __ Cmp(temp, Operand(1));
      __ B(eq, codegen_->GetLabelOf(successors[last_index + 1]));
    }

    // And the default for any other value.
    if (!codegen_->GoesToNextBlock(switch_instr->GetBlock(), default_block)) {
      __ B(codegen_->GetLabelOf(default_block));
    }
  } else {
    JumpTableARM64* jump_table = codegen_->CreateJumpTable(switch_instr);

    UseScratchRegisterScope temps(codegen_->GetVIXLAssembler());

    // Below instructions should use at most one blocked register. Since there are two blocked
    // registers, we are free to block one.
    Register temp_w = temps.AcquireW();
    Register index;
    // Remove the bias.
    if (lower_bound != 0) {
      index = temp_w;
      __ Sub(index, value_reg, Operand(lower_bound));
    } else {
      index = value_reg;
    }

    // Jump to default block if index is out of the range.
    __ Cmp(index, Operand(num_entries));
    __ B(hs, codegen_->GetLabelOf(default_block));

    // In current VIXL implementation, it won't require any blocked registers to encode the
    // immediate value for Adr. So we are free to use both VIXL blocked registers to reduce the
    // register pressure.
    Register table_base = temps.AcquireX();

    const size_t jump_size = switch_instr->GetNumEntries() * sizeof(int32_t);
    ExactAssemblyScope scope(codegen_->GetVIXLAssembler(),
                             kInstructionSize * 4 + jump_size,
                             CodeBufferCheckScope::kExactSize);

    // Load jump offset from the table.
    // Note: the table start address is always in range as the table is emitted immediately
    // after these 4 instructions.
    __ adr(table_base, jump_table->GetTableStartLabel());
    Register jump_offset = temp_w;
    __ ldr(jump_offset, MemOperand(table_base, index, UXTW, 2));

    // Jump to target block by branching to table_base(pc related) + offset.
    Register target_address = table_base;
    __ add(target_address, table_base, Operand(jump_offset, SXTW));
    __ br(target_address);

    jump_table->EmitTable(codegen_);
  }
}

void InstructionCodeGeneratorARM64::GenerateReferenceLoadOneRegister(
    HInstruction* instruction,
    Location out,
    uint32_t offset,
    Location maybe_temp,
    ReadBarrierOption read_barrier_option) {
  DataType::Type type = DataType::Type::kReference;
  Register out_reg = RegisterFrom(out, type);
  if (read_barrier_option == kWithReadBarrier) {
    DCHECK(codegen_->EmitReadBarrier());
    if (kUseBakerReadBarrier) {
      // Load with fast path based Baker's read barrier.
      // /* HeapReference<Object> */ out = *(out + offset)
      codegen_->GenerateFieldLoadWithBakerReadBarrier(instruction,
                                                      out,
                                                      out_reg,
                                                      offset,
                                                      maybe_temp,
                                                      /* needs_null_check= */ false,
                                                      /* use_load_acquire= */ false);
    } else {
      // Load with slow path based read barrier.
      // Save the value of `out` into `maybe_temp` before overwriting it
      // in the following move operation, as we will need it for the
      // read barrier below.
      Register temp_reg = RegisterFrom(maybe_temp, type);
      __ Mov(temp_reg, out_reg);
      // /* HeapReference<Object> */ out = *(out + offset)
      __ Ldr(out_reg, HeapOperand(out_reg, offset));
      codegen_->GenerateReadBarrierSlow(instruction, out, out, maybe_temp, offset);
    }
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(out + offset)
    __ Ldr(out_reg, HeapOperand(out_reg, offset));
    GetAssembler()->MaybeUnpoisonHeapReference(out_reg);
  }
}

void InstructionCodeGeneratorARM64::GenerateReferenceLoadTwoRegisters(
    HInstruction* instruction,
    Location out,
    Location obj,
    uint32_t offset,
    Location maybe_temp,
    ReadBarrierOption read_barrier_option) {
  DataType::Type type = DataType::Type::kReference;
  Register out_reg = RegisterFrom(out, type);
  Register obj_reg = RegisterFrom(obj, type);
  if (read_barrier_option == kWithReadBarrier) {
    DCHECK(codegen_->EmitReadBarrier());
    if (kUseBakerReadBarrier) {
      // Load with fast path based Baker's read barrier.
      // /* HeapReference<Object> */ out = *(obj + offset)
      codegen_->GenerateFieldLoadWithBakerReadBarrier(instruction,
                                                      out,
                                                      obj_reg,
                                                      offset,
                                                      maybe_temp,
                                                      /* needs_null_check= */ false,
                                                      /* use_load_acquire= */ false);
    } else {
      // Load with slow path based read barrier.
      // /* HeapReference<Object> */ out = *(obj + offset)
      __ Ldr(out_reg, HeapOperand(obj_reg, offset));
      codegen_->GenerateReadBarrierSlow(instruction, out, out, obj, offset);
    }
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(obj + offset)
    __ Ldr(out_reg, HeapOperand(obj_reg, offset));
    GetAssembler()->MaybeUnpoisonHeapReference(out_reg);
  }
}

void CodeGeneratorARM64::GenerateGcRootFieldLoad(
    HInstruction* instruction,
    Location root,
    Register obj,
    uint32_t offset,
    vixl::aarch64::Label* fixup_label,
    ReadBarrierOption read_barrier_option) {
  DCHECK(fixup_label == nullptr || offset == 0u);
  Register root_reg = RegisterFrom(root, DataType::Type::kReference);
  if (read_barrier_option == kWithReadBarrier) {
    DCHECK(EmitReadBarrier());
    if (kUseBakerReadBarrier) {
      // Fast path implementation of art::ReadBarrier::BarrierForRoot when
      // Baker's read barrier are used.

      // Query `art::Thread::Current()->GetIsGcMarking()` (stored in
      // the Marking Register) to decide whether we need to enter
      // the slow path to mark the GC root.
      //
      // We use shared thunks for the slow path; shared within the method
      // for JIT, across methods for AOT. That thunk checks the reference
      // and jumps to the entrypoint if needed.
      //
      //     lr = &return_address;
      //     GcRoot<mirror::Object> root = *(obj+offset);  // Original reference load.
      //     if (mr) {  // Thread::Current()->GetIsGcMarking()
      //       goto gc_root_thunk<root_reg>(lr)
      //     }
      //   return_address:

      UseScratchRegisterScope temps(GetVIXLAssembler());
      DCHECK(temps.IsAvailable(ip0));
      DCHECK(temps.IsAvailable(ip1));
      temps.Exclude(ip0, ip1);
      uint32_t custom_data = EncodeBakerReadBarrierGcRootData(root_reg.GetCode());

      ExactAssemblyScope guard(GetVIXLAssembler(), 3 * vixl::aarch64::kInstructionSize);
      vixl::aarch64::Label return_address;
      __ adr(lr, &return_address);
      if (fixup_label != nullptr) {
        __ bind(fixup_label);
      }
      static_assert(BAKER_MARK_INTROSPECTION_GC_ROOT_LDR_OFFSET == -8,
                    "GC root LDR must be 2 instructions (8B) before the return address label.");
      __ ldr(root_reg, MemOperand(obj.X(), offset));
      EmitBakerReadBarrierCbnz(custom_data);
      __ bind(&return_address);
    } else {
      // GC root loaded through a slow path for read barriers other
      // than Baker's.
      // /* GcRoot<mirror::Object>* */ root = obj + offset
      if (fixup_label == nullptr) {
        __ Add(root_reg.X(), obj.X(), offset);
      } else {
        EmitAddPlaceholder(fixup_label, root_reg.X(), obj.X());
      }
      // /* mirror::Object* */ root = root->Read()
      GenerateReadBarrierForRootSlow(instruction, root, root);
    }
  } else {
    // Plain GC root load with no read barrier.
    // /* GcRoot<mirror::Object> */ root = *(obj + offset)
    if (fixup_label == nullptr) {
      __ Ldr(root_reg, MemOperand(obj, offset));
    } else {
      EmitLdrOffsetPlaceholder(fixup_label, root_reg, obj.X());
    }
    // Note that GC roots are not affected by heap poisoning, thus we
    // do not have to unpoison `root_reg` here.
  }
  MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__);
}

void CodeGeneratorARM64::GenerateIntrinsicMoveWithBakerReadBarrier(
    vixl::aarch64::Register marked_old_value,
    vixl::aarch64::Register old_value) {
  DCHECK(EmitBakerReadBarrier());

  // Similar to the Baker RB path in GenerateGcRootFieldLoad(), with a MOV instead of LDR.
  uint32_t custom_data = EncodeBakerReadBarrierGcRootData(marked_old_value.GetCode());

  ExactAssemblyScope guard(GetVIXLAssembler(), 3 * vixl::aarch64::kInstructionSize);
  vixl::aarch64::Label return_address;
  __ adr(lr, &return_address);
  static_assert(BAKER_MARK_INTROSPECTION_GC_ROOT_LDR_OFFSET == -8,
                "GC root LDR must be 2 instructions (8B) before the return address label.");
  __ mov(marked_old_value, old_value);
  EmitBakerReadBarrierCbnz(custom_data);
  __ bind(&return_address);
}

void CodeGeneratorARM64::GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                                               Location ref,
                                                               vixl::aarch64::Register obj,
                                                               const vixl::aarch64::MemOperand& src,
                                                               bool needs_null_check,
                                                               bool use_load_acquire) {
  DCHECK(EmitBakerReadBarrier());

  // Query `art::Thread::Current()->GetIsGcMarking()` (stored in the
  // Marking Register) to decide whether we need to enter the slow
  // path to mark the reference. Then, in the slow path, check the
  // gray bit in the lock word of the reference's holder (`obj`) to
  // decide whether to mark `ref` or not.
  //
  // We use shared thunks for the slow path; shared within the method
  // for JIT, across methods for AOT. That thunk checks the holder
  // and jumps to the entrypoint if needed. If the holder is not gray,
  // it creates a fake dependency and returns to the LDR instruction.
  //
  //     lr = &gray_return_address;
  //     if (mr) {  // Thread::Current()->GetIsGcMarking()
  //       goto field_thunk<holder_reg, base_reg, use_load_acquire>(lr)
  //     }
  //   not_gray_return_address:
  //     // Original reference load. If the offset is too large to fit
  //     // into LDR, we use an adjusted base register here.
  //     HeapReference<mirror::Object> reference = *(obj+offset);
  //   gray_return_address:

  DCHECK(src.GetAddrMode() == vixl::aarch64::Offset);
  DCHECK_ALIGNED(src.GetOffset(), sizeof(mirror::HeapReference<mirror::Object>));

  UseScratchRegisterScope temps(GetVIXLAssembler());
  DCHECK(temps.IsAvailable(ip0));
  DCHECK(temps.IsAvailable(ip1));
  temps.Exclude(ip0, ip1);
  uint32_t custom_data = use_load_acquire
      ? EncodeBakerReadBarrierAcquireData(src.GetBaseRegister().GetCode(), obj.GetCode())
      : EncodeBakerReadBarrierFieldData(src.GetBaseRegister().GetCode(), obj.GetCode());

  {
    ExactAssemblyScope guard(GetVIXLAssembler(),
                             (kPoisonHeapReferences ? 4u : 3u) * vixl::aarch64::kInstructionSize);
    vixl::aarch64::Label return_address;
    __ adr(lr, &return_address);
    EmitBakerReadBarrierCbnz(custom_data);
    static_assert(BAKER_MARK_INTROSPECTION_FIELD_LDR_OFFSET == (kPoisonHeapReferences ? -8 : -4),
                  "Field LDR must be 1 instruction (4B) before the return address label; "
                  " 2 instructions (8B) for heap poisoning.");
    Register ref_reg = RegisterFrom(ref, DataType::Type::kReference);
    if (use_load_acquire) {
      DCHECK_EQ(src.GetOffset(), 0);
      __ ldar(ref_reg, src);
    } else {
      __ ldr(ref_reg, src);
    }
    if (needs_null_check) {
      MaybeRecordImplicitNullCheck(instruction);
    }
    // Unpoison the reference explicitly if needed. MaybeUnpoisonHeapReference() uses
    // macro instructions disallowed in ExactAssemblyScope.
    if (kPoisonHeapReferences) {
      __ neg(ref_reg, Operand(ref_reg));
    }
    __ bind(&return_address);
  }
  MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__, /* temp_loc= */ LocationFrom(ip1));
}

void CodeGeneratorARM64::GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                                               Location ref,
                                                               Register obj,
                                                               uint32_t offset,
                                                               Location maybe_temp,
                                                               bool needs_null_check,
                                                               bool use_load_acquire) {
  DCHECK_ALIGNED(offset, sizeof(mirror::HeapReference<mirror::Object>));
  Register base = obj;
  if (use_load_acquire) {
    DCHECK(maybe_temp.IsRegister());
    base = WRegisterFrom(maybe_temp);
    __ Add(base, obj, offset);
    offset = 0u;
  } else if (offset >= kReferenceLoadMinFarOffset) {
    DCHECK(maybe_temp.IsRegister());
    base = WRegisterFrom(maybe_temp);
    static_assert(IsPowerOfTwo(kReferenceLoadMinFarOffset), "Expecting a power of 2.");
    __ Add(base, obj, Operand(offset & ~(kReferenceLoadMinFarOffset - 1u)));
    offset &= (kReferenceLoadMinFarOffset - 1u);
  }
  MemOperand src(base.X(), offset);
  GenerateFieldLoadWithBakerReadBarrier(
      instruction, ref, obj, src, needs_null_check, use_load_acquire);
}

void CodeGeneratorARM64::GenerateArrayLoadWithBakerReadBarrier(HArrayGet* instruction,
                                                               Location ref,
                                                               Register obj,
                                                               uint32_t data_offset,
                                                               Location index,
                                                               bool needs_null_check) {
  DCHECK(EmitBakerReadBarrier());

  static_assert(
      sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
      "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
  size_t scale_factor = DataType::SizeShift(DataType::Type::kReference);

  // Query `art::Thread::Current()->GetIsGcMarking()` (stored in the
  // Marking Register) to decide whether we need to enter the slow
  // path to mark the reference. Then, in the slow path, check the
  // gray bit in the lock word of the reference's holder (`obj`) to
  // decide whether to mark `ref` or not.
  //
  // We use shared thunks for the slow path; shared within the method
  // for JIT, across methods for AOT. That thunk checks the holder
  // and jumps to the entrypoint if needed. If the holder is not gray,
  // it creates a fake dependency and returns to the LDR instruction.
  //
  //     lr = &gray_return_address;
  //     if (mr) {  // Thread::Current()->GetIsGcMarking()
  //       goto array_thunk<base_reg>(lr)
  //     }
  //   not_gray_return_address:
  //     // Original reference load. If the offset is too large to fit
  //     // into LDR, we use an adjusted base register here.
  //     HeapReference<mirror::Object> reference = data[index];
  //   gray_return_address:

  DCHECK(index.IsValid());
  Register index_reg = RegisterFrom(index, DataType::Type::kInt32);
  Register ref_reg = RegisterFrom(ref, DataType::Type::kReference);

  UseScratchRegisterScope temps(GetVIXLAssembler());
  DCHECK(temps.IsAvailable(ip0));
  DCHECK(temps.IsAvailable(ip1));
  temps.Exclude(ip0, ip1);

  Register temp;
  if (instruction->GetArray()->IsIntermediateAddress()) {
    // We do not need to compute the intermediate address from the array: the
    // input instruction has done it already. See the comment in
    // `TryExtractArrayAccessAddress()`.
    if (kIsDebugBuild) {
      HIntermediateAddress* interm_addr = instruction->GetArray()->AsIntermediateAddress();
      DCHECK_EQ(interm_addr->GetOffset()->AsIntConstant()->GetValueAsUint64(), data_offset);
    }
    temp = obj;
  } else {
    temp = WRegisterFrom(instruction->GetLocations()->GetTemp(0));
    __ Add(temp.X(), obj.X(), Operand(data_offset));
  }

  uint32_t custom_data = EncodeBakerReadBarrierArrayData(temp.GetCode());

  {
    ExactAssemblyScope guard(GetVIXLAssembler(),
                             (kPoisonHeapReferences ? 4u : 3u) * vixl::aarch64::kInstructionSize);
    vixl::aarch64::Label return_address;
    __ adr(lr, &return_address);
    EmitBakerReadBarrierCbnz(custom_data);
    static_assert(BAKER_MARK_INTROSPECTION_ARRAY_LDR_OFFSET == (kPoisonHeapReferences ? -8 : -4),
                  "Array LDR must be 1 instruction (4B) before the return address label; "
                  " 2 instructions (8B) for heap poisoning.");
    __ ldr(ref_reg, MemOperand(temp.X(), index_reg.X(), LSL, scale_factor));
    DCHECK(!needs_null_check);  // The thunk cannot handle the null check.
    // Unpoison the reference explicitly if needed. MaybeUnpoisonHeapReference() uses
    // macro instructions disallowed in ExactAssemblyScope.
    if (kPoisonHeapReferences) {
      __ neg(ref_reg, Operand(ref_reg));
    }
    __ bind(&return_address);
  }
  MaybeGenerateMarkingRegisterCheck(/* code= */ __LINE__, /* temp_loc= */ LocationFrom(ip1));
}

void CodeGeneratorARM64::MaybeGenerateMarkingRegisterCheck(int code, Location temp_loc) {
  // The following condition is a compile-time one, so it does not have a run-time cost.
  if (kIsDebugBuild && EmitBakerReadBarrier()) {
    // The following condition is a run-time one; it is executed after the
    // previous compile-time test, to avoid penalizing non-debug builds.
    if (GetCompilerOptions().EmitRunTimeChecksInDebugMode()) {
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register temp = temp_loc.IsValid() ? WRegisterFrom(temp_loc) : temps.AcquireW();
      GetAssembler()->GenerateMarkingRegisterCheck(temp, code);
    }
  }
}

SlowPathCodeARM64* CodeGeneratorARM64::AddReadBarrierSlowPath(HInstruction* instruction,
                                                              Location out,
                                                              Location ref,
                                                              Location obj,
                                                              uint32_t offset,
                                                              Location index) {
  SlowPathCodeARM64* slow_path = new (GetScopedAllocator())
      ReadBarrierForHeapReferenceSlowPathARM64(instruction, out, ref, obj, offset, index);
  AddSlowPath(slow_path);
  return slow_path;
}

void CodeGeneratorARM64::GenerateReadBarrierSlow(HInstruction* instruction,
                                                 Location out,
                                                 Location ref,
                                                 Location obj,
                                                 uint32_t offset,
                                                 Location index) {
  DCHECK(EmitReadBarrier());

  // Insert a slow path based read barrier *after* the reference load.
  //
  // If heap poisoning is enabled, the unpoisoning of the loaded
  // reference will be carried out by the runtime within the slow
  // path.
  //
  // Note that `ref` currently does not get unpoisoned (when heap
  // poisoning is enabled), which is alright as the `ref` argument is
  // not used by the artReadBarrierSlow entry point.
  //
  // TODO: Unpoison `ref` when it is used by artReadBarrierSlow.
  SlowPathCodeARM64* slow_path = AddReadBarrierSlowPath(instruction, out, ref, obj, offset, index);

  __ B(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorARM64::MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                                      Location out,
                                                      Location ref,
                                                      Location obj,
                                                      uint32_t offset,
                                                      Location index) {
  if (EmitReadBarrier()) {
    // Baker's read barriers shall be handled by the fast path
    // (CodeGeneratorARM64::GenerateReferenceLoadWithBakerReadBarrier).
    DCHECK(!kUseBakerReadBarrier);
    // If heap poisoning is enabled, unpoisoning will be taken care of
    // by the runtime within the slow path.
    GenerateReadBarrierSlow(instruction, out, ref, obj, offset, index);
  } else if (kPoisonHeapReferences) {
    GetAssembler()->UnpoisonHeapReference(WRegisterFrom(out));
  }
}

void CodeGeneratorARM64::GenerateReadBarrierForRootSlow(HInstruction* instruction,
                                                        Location out,
                                                        Location root) {
  DCHECK(EmitReadBarrier());

  // Insert a slow path based read barrier *after* the GC root load.
  //
  // Note that GC roots are not affected by heap poisoning, so we do
  // not need to do anything special for this here.
  SlowPathCodeARM64* slow_path =
      new (GetScopedAllocator()) ReadBarrierForRootSlowPathARM64(instruction, out, root);
  AddSlowPath(slow_path);

  __ B(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM64::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  if (instruction->GetTableKind() == HClassTableGet::TableKind::kVTable) {
    uint32_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
        instruction->GetIndex(), kArm64PointerSize).SizeValue();
    __ Ldr(XRegisterFrom(locations->Out()),
           MemOperand(XRegisterFrom(locations->InAt(0)), method_offset));
  } else {
    uint32_t method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
        instruction->GetIndex(), kArm64PointerSize));
    __ Ldr(XRegisterFrom(locations->Out()), MemOperand(XRegisterFrom(locations->InAt(0)),
        mirror::Class::ImtPtrOffset(kArm64PointerSize).Uint32Value()));
    __ Ldr(XRegisterFrom(locations->Out()),
           MemOperand(XRegisterFrom(locations->Out()), method_offset));
  }
}

MemOperand InstructionCodeGeneratorARM64::VecNEONAddress(
    HVecMemoryOperation* instruction,
    UseScratchRegisterScope* temps_scope,
    size_t size,
    bool is_string_char_at,
    /*out*/ Register* scratch) {
  LocationSummary* locations = instruction->GetLocations();
  Register base = InputRegisterAt(instruction, 0);

  if (instruction->InputAt(1)->IsIntermediateAddressIndex()) {
    DCHECK(!is_string_char_at);
    return MemOperand(base.X(), InputRegisterAt(instruction, 1).X());
  }

  Location index = locations->InAt(1);
  uint32_t offset = is_string_char_at
      ? mirror::String::ValueOffset().Uint32Value()
      : mirror::Array::DataOffset(size).Uint32Value();
  size_t shift = ComponentSizeShiftWidth(size);

  // HIntermediateAddress optimization is only applied for scalar ArrayGet and ArraySet.
  DCHECK(!instruction->InputAt(0)->IsIntermediateAddress());

  if (index.IsConstant()) {
    offset += Int64FromLocation(index) << shift;
    return HeapOperand(base, offset);
  } else {
    *scratch = temps_scope->AcquireSameSizeAs(base);
    __ Add(*scratch, base, Operand(WRegisterFrom(index), LSL, shift));
    return HeapOperand(*scratch, offset);
  }
}

SVEMemOperand InstructionCodeGeneratorARM64::VecSVEAddress(
    HVecMemoryOperation* instruction,
    UseScratchRegisterScope* temps_scope,
    size_t size,
    bool is_string_char_at,
    /*out*/ Register* scratch) {
  LocationSummary* locations = instruction->GetLocations();
  Register base = InputRegisterAt(instruction, 0);
  Location index = locations->InAt(1);

  DCHECK(!instruction->InputAt(1)->IsIntermediateAddressIndex());
  DCHECK(!index.IsConstant());

  uint32_t offset = is_string_char_at
      ? mirror::String::ValueOffset().Uint32Value()
      : mirror::Array::DataOffset(size).Uint32Value();
  size_t shift = ComponentSizeShiftWidth(size);

  if (instruction->InputAt(0)->IsIntermediateAddress()) {
    return SVEMemOperand(base.X(), XRegisterFrom(index), LSL, shift);
  }

  *scratch = temps_scope->AcquireSameSizeAs(base);
  __ Add(*scratch, base, offset);
  return SVEMemOperand(scratch->X(), XRegisterFrom(index), LSL, shift);
}

#undef __
#undef QUICK_ENTRY_POINT

#define __ assembler.GetVIXLAssembler()->

static void EmitGrayCheckAndFastPath(arm64::Arm64Assembler& assembler,
                                     vixl::aarch64::Register base_reg,
                                     vixl::aarch64::MemOperand& lock_word,
                                     vixl::aarch64::Label* slow_path,
                                     vixl::aarch64::Label* throw_npe = nullptr) {
  vixl::aarch64::Label throw_npe_cont;
  // Load the lock word containing the rb_state.
  __ Ldr(ip0.W(), lock_word);
  // Given the numeric representation, it's enough to check the low bit of the rb_state.
  static_assert(ReadBarrier::NonGrayState() == 0, "Expecting non-gray to have value 0");
  static_assert(ReadBarrier::GrayState() == 1, "Expecting gray to have value 1");
  __ Tbnz(ip0.W(), LockWord::kReadBarrierStateShift, slow_path);
  static_assert(
      BAKER_MARK_INTROSPECTION_ARRAY_LDR_OFFSET == BAKER_MARK_INTROSPECTION_FIELD_LDR_OFFSET,
      "Field and array LDR offsets must be the same to reuse the same code.");
  // To throw NPE, we return to the fast path; the artificial dependence below does not matter.
  if (throw_npe != nullptr) {
    __ Bind(&throw_npe_cont);
  }
  // Adjust the return address back to the LDR (1 instruction; 2 for heap poisoning).
  static_assert(BAKER_MARK_INTROSPECTION_FIELD_LDR_OFFSET == (kPoisonHeapReferences ? -8 : -4),
                "Field LDR must be 1 instruction (4B) before the return address label; "
                " 2 instructions (8B) for heap poisoning.");
  __ Add(lr, lr, BAKER_MARK_INTROSPECTION_FIELD_LDR_OFFSET);
  // Introduce a dependency on the lock_word including rb_state,
  // to prevent load-load reordering, and without using
  // a memory barrier (which would be more expensive).
  __ Add(base_reg, base_reg, Operand(ip0, LSR, 32));
  __ Br(lr);          // And return back to the function.
  if (throw_npe != nullptr) {
    // Clear IP0 before returning to the fast path.
    __ Bind(throw_npe);
    __ Mov(ip0.X(), xzr);
    __ B(&throw_npe_cont);
  }
  // Note: The fake dependency is unnecessary for the slow path.
}

// Load the read barrier introspection entrypoint in register `entrypoint`.
static void LoadReadBarrierMarkIntrospectionEntrypoint(arm64::Arm64Assembler& assembler,
                                                       vixl::aarch64::Register entrypoint) {
  // entrypoint = Thread::Current()->pReadBarrierMarkReg16, i.e. pReadBarrierMarkIntrospection.
  DCHECK_EQ(ip0.GetCode(), 16u);
  const int32_t entry_point_offset =
      Thread::ReadBarrierMarkEntryPointsOffset<kArm64PointerSize>(ip0.GetCode());
  __ Ldr(entrypoint, MemOperand(tr, entry_point_offset));
}

void CodeGeneratorARM64::CompileBakerReadBarrierThunk(Arm64Assembler& assembler,
                                                      uint32_t encoded_data,
                                                      /*out*/ std::string* debug_name) {
  BakerReadBarrierKind kind = BakerReadBarrierKindField::Decode(encoded_data);
  switch (kind) {
    case BakerReadBarrierKind::kField:
    case BakerReadBarrierKind::kAcquire: {
      Register base_reg =
          vixl::aarch64::XRegister(BakerReadBarrierFirstRegField::Decode(encoded_data));
      CheckValidReg(base_reg.GetCode());
      Register holder_reg =
          vixl::aarch64::XRegister(BakerReadBarrierSecondRegField::Decode(encoded_data));
      CheckValidReg(holder_reg.GetCode());
      UseScratchRegisterScope temps(assembler.GetVIXLAssembler());
      temps.Exclude(ip0, ip1);
      // In the case of a field load (with relaxed semantic), if `base_reg` differs from
      // `holder_reg`, the offset was too large and we must have emitted (during the construction
      // of the HIR graph, see `art::HInstructionBuilder::BuildInstanceFieldAccess`) and preserved
      // (see `art::PrepareForRegisterAllocation::VisitNullCheck`) an explicit null check before
      // the load. Otherwise, for implicit null checks, we need to null-check the holder as we do
      // not necessarily do that check before going to the thunk.
      //
      // In the case of a field load with load-acquire semantics (where `base_reg` always differs
      // from `holder_reg`), we also need an explicit null check when implicit null checks are
      // allowed, as we do not emit one before going to the thunk.
      vixl::aarch64::Label throw_npe_label;
      vixl::aarch64::Label* throw_npe = nullptr;
      if (GetCompilerOptions().GetImplicitNullChecks() &&
          (holder_reg.Is(base_reg) || (kind == BakerReadBarrierKind::kAcquire))) {
        throw_npe = &throw_npe_label;
        __ Cbz(holder_reg.W(), throw_npe);
      }
      // Check if the holder is gray and, if not, add fake dependency to the base register
      // and return to the LDR instruction to load the reference. Otherwise, use introspection
      // to load the reference and call the entrypoint that performs further checks on the
      // reference and marks it if needed.
      vixl::aarch64::Label slow_path;
      MemOperand lock_word(holder_reg, mirror::Object::MonitorOffset().Int32Value());
      EmitGrayCheckAndFastPath(assembler, base_reg, lock_word, &slow_path, throw_npe);
      __ Bind(&slow_path);
      if (kind == BakerReadBarrierKind::kField) {
        MemOperand ldr_address(lr, BAKER_MARK_INTROSPECTION_FIELD_LDR_OFFSET);
        __ Ldr(ip0.W(), ldr_address);         // Load the LDR (immediate) unsigned offset.
        LoadReadBarrierMarkIntrospectionEntrypoint(assembler, ip1);
        __ Ubfx(ip0.W(), ip0.W(), 10, 12);    // Extract the offset.
        __ Ldr(ip0.W(), MemOperand(base_reg, ip0, LSL, 2));   // Load the reference.
      } else {
        DCHECK(kind == BakerReadBarrierKind::kAcquire);
        DCHECK(!base_reg.Is(holder_reg));
        LoadReadBarrierMarkIntrospectionEntrypoint(assembler, ip1);
        __ Ldar(ip0.W(), MemOperand(base_reg));
      }
      // Do not unpoison. With heap poisoning enabled, the entrypoint expects a poisoned reference.
      __ Br(ip1);                           // Jump to the entrypoint.
      break;
    }
    case BakerReadBarrierKind::kArray: {
      Register base_reg =
          vixl::aarch64::XRegister(BakerReadBarrierFirstRegField::Decode(encoded_data));
      CheckValidReg(base_reg.GetCode());
      DCHECK_EQ(kBakerReadBarrierInvalidEncodedReg,
                BakerReadBarrierSecondRegField::Decode(encoded_data));
      UseScratchRegisterScope temps(assembler.GetVIXLAssembler());
      temps.Exclude(ip0, ip1);
      vixl::aarch64::Label slow_path;
      int32_t data_offset =
          mirror::Array::DataOffset(Primitive::ComponentSize(Primitive::kPrimNot)).Int32Value();
      MemOperand lock_word(base_reg, mirror::Object::MonitorOffset().Int32Value() - data_offset);
      DCHECK_LT(lock_word.GetOffset(), 0);
      EmitGrayCheckAndFastPath(assembler, base_reg, lock_word, &slow_path);
      __ Bind(&slow_path);
      MemOperand ldr_address(lr, BAKER_MARK_INTROSPECTION_ARRAY_LDR_OFFSET);
      __ Ldr(ip0.W(), ldr_address);         // Load the LDR (register) unsigned offset.
      LoadReadBarrierMarkIntrospectionEntrypoint(assembler, ip1);
      __ Ubfx(ip0, ip0, 16, 6);             // Extract the index register, plus 32 (bit 21 is set).
      __ Bfi(ip1, ip0, 3, 6);               // Insert ip0 to the entrypoint address to create
                                            // a switch case target based on the index register.
      __ Mov(ip0, base_reg);                // Move the base register to ip0.
      __ Br(ip1);                           // Jump to the entrypoint's array switch case.
      break;
    }
    case BakerReadBarrierKind::kGcRoot: {
      // Check if the reference needs to be marked and if so (i.e. not null, not marked yet
      // and it does not have a forwarding address), call the correct introspection entrypoint;
      // otherwise return the reference (or the extracted forwarding address).
      // There is no gray bit check for GC roots.
      Register root_reg =
          vixl::aarch64::WRegister(BakerReadBarrierFirstRegField::Decode(encoded_data));
      CheckValidReg(root_reg.GetCode());
      DCHECK_EQ(kBakerReadBarrierInvalidEncodedReg,
                BakerReadBarrierSecondRegField::Decode(encoded_data));
      UseScratchRegisterScope temps(assembler.GetVIXLAssembler());
      temps.Exclude(ip0, ip1);
      vixl::aarch64::Label return_label, not_marked, forwarding_address;
      __ Cbz(root_reg, &return_label);
      MemOperand lock_word(root_reg.X(), mirror::Object::MonitorOffset().Int32Value());
      __ Ldr(ip0.W(), lock_word);
      __ Tbz(ip0.W(), LockWord::kMarkBitStateShift, &not_marked);
      __ Bind(&return_label);
      __ Br(lr);
      __ Bind(&not_marked);
      __ Tst(ip0.W(), Operand(ip0.W(), LSL, 1));
      __ B(&forwarding_address, mi);
      LoadReadBarrierMarkIntrospectionEntrypoint(assembler, ip1);
      // Adjust the art_quick_read_barrier_mark_introspection address in IP1 to
      // art_quick_read_barrier_mark_introspection_gc_roots.
      __ Add(ip1, ip1, Operand(BAKER_MARK_INTROSPECTION_GC_ROOT_ENTRYPOINT_OFFSET));
      __ Mov(ip0.W(), root_reg);
      __ Br(ip1);
      __ Bind(&forwarding_address);
      __ Lsl(root_reg, ip0.W(), LockWord::kForwardingAddressShift);
      __ Br(lr);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected kind: " << static_cast<uint32_t>(kind);
      UNREACHABLE();
  }

  // For JIT, the slow path is considered part of the compiled method,
  // so JIT should pass null as `debug_name`.
  DCHECK_IMPLIES(GetCompilerOptions().IsJitCompiler(), debug_name == nullptr);
  if (debug_name != nullptr && GetCompilerOptions().GenerateAnyDebugInfo()) {
    std::ostringstream oss;
    oss << "BakerReadBarrierThunk";
    switch (kind) {
      case BakerReadBarrierKind::kField:
        oss << "Field_r" << BakerReadBarrierFirstRegField::Decode(encoded_data)
            << "_r" << BakerReadBarrierSecondRegField::Decode(encoded_data);
        break;
      case BakerReadBarrierKind::kAcquire:
        oss << "Acquire_r" << BakerReadBarrierFirstRegField::Decode(encoded_data)
            << "_r" << BakerReadBarrierSecondRegField::Decode(encoded_data);
        break;
      case BakerReadBarrierKind::kArray:
        oss << "Array_r" << BakerReadBarrierFirstRegField::Decode(encoded_data);
        DCHECK_EQ(kBakerReadBarrierInvalidEncodedReg,
                  BakerReadBarrierSecondRegField::Decode(encoded_data));
        break;
      case BakerReadBarrierKind::kGcRoot:
        oss << "GcRoot_r" << BakerReadBarrierFirstRegField::Decode(encoded_data);
        DCHECK_EQ(kBakerReadBarrierInvalidEncodedReg,
                  BakerReadBarrierSecondRegField::Decode(encoded_data));
        break;
    }
    *debug_name = oss.str();
  }
}

#undef __

}  // namespace arm64
}  // namespace art

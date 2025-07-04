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

#ifndef ART_COMPILER_OPTIMIZING_NODES_H_
#define ART_COMPILER_OPTIMIZING_NODES_H_

#include <algorithm>
#include <array>
#include <type_traits>

#include "art_method.h"
#include "base/arena_allocator.h"
#include "base/arena_bit_vector.h"
#include "base/arena_containers.h"
#include "base/arena_object.h"
#include "base/array_ref.h"
#include "base/intrusive_forward_list.h"
#include "base/iteration_range.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/quasi_atomic.h"
#include "base/stl_util.h"
#include "base/transform_array_ref.h"
#include "block_namer.h"
#include "class_root.h"
#include "compilation_kind.h"
#include "data_type.h"
#include "deoptimization_kind.h"
#include "dex/dex_file.h"
#include "dex/dex_file_types.h"
#include "dex/invoke_type.h"
#include "dex/method_reference.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "handle.h"
#include "handle_cache.h"
#include "intrinsics_enum.h"
#include "locations.h"
#include "mirror/class.h"
#include "mirror/method_type.h"
#include "offsets.h"
#include "reference_type_info.h"

namespace art HIDDEN {

class ArenaStack;
class CodeGenerator;
class GraphChecker;
class HBasicBlock;
class HCondition;
class HConstructorFence;
class HCurrentMethod;
class HDoubleConstant;
class HEnvironment;
class HFloatConstant;
class HGraphBuilder;
class HGraphVisitor;
class HInstruction;
class HIntConstant;
class HInvoke;
class HLongConstant;
class HNullConstant;
class HParameterValue;
class HPhi;
class HSuspendCheck;
class HTryBoundary;
class HVecCondition;
class FieldInfo;
class LiveInterval;
class LocationSummary;
class ProfilingInfo;
class SlowPathCode;
class SsaBuilder;

namespace mirror {
class DexCache;
}  // namespace mirror

static const int kDefaultNumberOfBlocks = 8;
static const int kDefaultNumberOfSuccessors = 2;
static const int kDefaultNumberOfPredecessors = 2;
static const int kDefaultNumberOfExceptionalPredecessors = 0;
static const int kDefaultNumberOfDominatedBlocks = 1;
static const int kDefaultNumberOfBackEdges = 1;

// The maximum (meaningful) distance (31) that can be used in an integer shift/rotate operation.
static constexpr int32_t kMaxIntShiftDistance = 0x1f;
// The maximum (meaningful) distance (63) that can be used in a long shift/rotate operation.
static constexpr int32_t kMaxLongShiftDistance = 0x3f;

static constexpr uint32_t kUnknownFieldIndex = static_cast<uint32_t>(-1);
static constexpr uint16_t kUnknownClassDefIndex = static_cast<uint16_t>(-1);

static constexpr InvokeType kInvalidInvokeType = static_cast<InvokeType>(-1);

static constexpr uint32_t kNoDexPc = -1;

inline bool IsSameDexFile(const DexFile& lhs, const DexFile& rhs) {
  // For the purposes of the compiler, the dex files must actually be the same object
  // if we want to safely treat them as the same. This is especially important for JIT
  // as custom class loaders can open the same underlying file (or memory) multiple
  // times and provide different class resolution but no two class loaders should ever
  // use the same DexFile object - doing so is an unsupported hack that can lead to
  // all sorts of weird failures.
  return &lhs == &rhs;
}

enum IfCondition {
  // All types.
  kCondEQ,  // ==
  kCondNE,  // !=
  // Signed integers and floating-point numbers.
  kCondLT,  // <
  kCondLE,  // <=
  kCondGT,  // >
  kCondGE,  // >=
  // Unsigned integers.
  kCondB,   // <
  kCondBE,  // <=
  kCondA,   // >
  kCondAE,  // >=
  // First and last aliases.
  kCondFirst = kCondEQ,
  kCondLast = kCondAE,
};

enum GraphAnalysisResult {
  kAnalysisSkipped,
  kAnalysisInvalidBytecode,
  kAnalysisFailThrowCatchLoop,
  kAnalysisFailAmbiguousArrayOp,
  kAnalysisFailIrreducibleLoopAndStringInit,
  kAnalysisFailPhiEquivalentInOsr,
  kAnalysisSuccess,
};

std::ostream& operator<<(std::ostream& os, GraphAnalysisResult ga);

template <typename T>
static inline typename std::make_unsigned<T>::type MakeUnsigned(T x) {
  return static_cast<typename std::make_unsigned<T>::type>(x);
}

class HInstructionList : public ValueObject {
 public:
  HInstructionList() : first_instruction_(nullptr), last_instruction_(nullptr) {}

  void AddInstruction(HInstruction* instruction);
  void RemoveInstruction(HInstruction* instruction);

  // Insert `instruction` before/after an existing instruction `cursor`.
  void InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor);
  void InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor);

  // Return true if this list contains `instruction`.
  bool Contains(HInstruction* instruction) const;

  // Return true if `instruction1` is found before `instruction2` in
  // this instruction list and false otherwise.  Abort if none
  // of these instructions is found.
  bool FoundBefore(const HInstruction* instruction1,
                   const HInstruction* instruction2) const;

  bool IsEmpty() const { return first_instruction_ == nullptr; }
  void Clear() { first_instruction_ = last_instruction_ = nullptr; }

  // Update the block of all instructions to be `block`.
  void SetBlockOfInstructions(HBasicBlock* block) const;

  void AddAfter(HInstruction* cursor, const HInstructionList& instruction_list);
  void AddBefore(HInstruction* cursor, const HInstructionList& instruction_list);
  void Add(const HInstructionList& instruction_list);

  // Return the number of instructions in the list. This is an expensive operation.
  size_t CountSize() const;

 private:
  HInstruction* first_instruction_;
  HInstruction* last_instruction_;

  friend class HBasicBlock;
  friend class HGraph;
  friend class HInstruction;
  friend class HInstructionIterator;
  friend class HInstructionIteratorHandleChanges;
  friend class HBackwardInstructionIterator;

  DISALLOW_COPY_AND_ASSIGN(HInstructionList);
};

// Control-flow graph of a method. Contains a list of basic blocks.
class HGraph : public ArenaObject<kArenaAllocGraph> {
 public:
  HGraph(ArenaAllocator* allocator,
         ArenaStack* arena_stack,
         VariableSizedHandleScope* handles,
         const DexFile& dex_file,
         uint32_t method_idx,
         InstructionSet instruction_set,
         InvokeType invoke_type = kInvalidInvokeType,
         bool dead_reference_safe = false,
         bool debuggable = false,
         CompilationKind compilation_kind = CompilationKind::kOptimized,
         int start_instruction_id = 0)
      : allocator_(allocator),
        arena_stack_(arena_stack),
        handle_cache_(handles),
        blocks_(allocator->Adapter(kArenaAllocBlockList)),
        reverse_post_order_(allocator->Adapter(kArenaAllocReversePostOrder)),
        linear_order_(allocator->Adapter(kArenaAllocLinearOrder)),
        entry_block_(nullptr),
        exit_block_(nullptr),
        number_of_vregs_(0),
        number_of_in_vregs_(0),
        temporaries_vreg_slots_(0),
        has_bounds_checks_(false),
        has_try_catch_(false),
        has_monitor_operations_(false),
        has_traditional_simd_(false),
        has_predicated_simd_(false),
        has_loops_(false),
        has_irreducible_loops_(false),
        has_direct_critical_native_call_(false),
        has_always_throwing_invokes_(false),
        dead_reference_safe_(dead_reference_safe),
        debuggable_(debuggable),
        current_instruction_id_(start_instruction_id),
        dex_file_(dex_file),
        method_idx_(method_idx),
        invoke_type_(invoke_type),
        in_ssa_form_(false),
        number_of_cha_guards_(0),
        instruction_set_(instruction_set),
        cached_null_constant_(nullptr),
        cached_int_constants_(std::less<int32_t>(), allocator->Adapter(kArenaAllocConstantsMap)),
        cached_float_constants_(std::less<int32_t>(), allocator->Adapter(kArenaAllocConstantsMap)),
        cached_long_constants_(std::less<int64_t>(), allocator->Adapter(kArenaAllocConstantsMap)),
        cached_double_constants_(std::less<int64_t>(), allocator->Adapter(kArenaAllocConstantsMap)),
        cached_current_method_(nullptr),
        art_method_(nullptr),
        compilation_kind_(compilation_kind),
        useful_optimizing_(false),
        cha_single_implementation_list_(allocator->Adapter(kArenaAllocCHA)) {
    blocks_.reserve(kDefaultNumberOfBlocks);
  }

  std::ostream& Dump(std::ostream& os,
                     CodeGenerator* codegen,
                     std::optional<std::reference_wrapper<const BlockNamer>> namer = std::nullopt);

  ArenaAllocator* GetAllocator() const { return allocator_; }
  ArenaStack* GetArenaStack() const { return arena_stack_; }

  HandleCache* GetHandleCache() { return &handle_cache_; }

  const ArenaVector<HBasicBlock*>& GetBlocks() const { return blocks_; }

  // An iterator to only blocks that are still actually in the graph (when
  // blocks are removed they are replaced with 'nullptr' in GetBlocks to
  // simplify block-id assignment and avoid memmoves in the block-list).
  IterationRange<FilterNull<ArenaVector<HBasicBlock*>::const_iterator>> GetActiveBlocks() const {
    return FilterOutNull(MakeIterationRange(GetBlocks()));
  }

  bool IsInSsaForm() const { return in_ssa_form_; }
  void SetInSsaForm() { in_ssa_form_ = true; }

  HBasicBlock* GetEntryBlock() const { return entry_block_; }
  HBasicBlock* GetExitBlock() const { return exit_block_; }
  bool HasExitBlock() const { return exit_block_ != nullptr; }

  void SetEntryBlock(HBasicBlock* block) { entry_block_ = block; }
  void SetExitBlock(HBasicBlock* block) { exit_block_ = block; }

  void AddBlock(HBasicBlock* block);

  void ComputeDominanceInformation();
  void ClearDominanceInformation();
  void ClearLoopInformation();
  void FindBackEdges(/*out*/ BitVectorView<size_t> visited);
  GraphAnalysisResult BuildDominatorTree();
  GraphAnalysisResult RecomputeDominatorTree();
  void SimplifyCFG();
  void SimplifyCatchBlocks();

  // Analyze all natural loops in this graph. Returns a code specifying that it
  // was successful or the reason for failure. The method will fail if a loop
  // is a throw-catch loop, i.e. the header is a catch block.
  GraphAnalysisResult AnalyzeLoops() const;

  // Iterate over blocks to compute try block membership. Needs reverse post
  // order and loop information.
  void ComputeTryBlockInformation();

  // Inline this graph in `outer_graph`, replacing the given `invoke` instruction.
  // Returns the instruction to replace the invoke expression or null if the
  // invoke is for a void method. Note that the caller is responsible for replacing
  // and removing the invoke instruction.
  HInstruction* InlineInto(HGraph* outer_graph, HInvoke* invoke);

  // Update the loop and try membership of `block`, which was spawned from `reference`.
  // In case `reference` is a back edge, `replace_if_back_edge` notifies whether `block`
  // should be the new back edge.
  // `has_more_specific_try_catch_info` will be set to true when inlining a try catch.
  void UpdateLoopAndTryInformationOfNewBlock(HBasicBlock* block,
                                             HBasicBlock* reference,
                                             bool replace_if_back_edge,
                                             bool has_more_specific_try_catch_info = false);

  // Need to add a couple of blocks to test if the loop body is entered and
  // put deoptimization instructions, etc.
  void TransformLoopHeaderForBCE(HBasicBlock* header);

  // Adds a new loop directly after the loop with the given header and exit.
  // Returns the new preheader.
  HBasicBlock* TransformLoopForVectorization(HBasicBlock* header,
                                             HBasicBlock* body,
                                             HBasicBlock* exit);

  // Removes `block` from the graph. Assumes `block` has been disconnected from
  // other blocks and has no instructions or phis.
  void DeleteDeadEmptyBlock(HBasicBlock* block);

  // Splits the edge between `block` and `successor` while preserving the
  // indices in the predecessor/successor lists. If there are multiple edges
  // between the blocks, the lowest indices are used.
  // Returns the new block which is empty and has the same dex pc as `successor`.
  HBasicBlock* SplitEdge(HBasicBlock* block, HBasicBlock* successor);

  void SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor);

  // Splits the edge between `block` and `successor` and then updates the graph's RPO to keep
  // consistency without recomputing the whole graph.
  HBasicBlock* SplitEdgeAndUpdateRPO(HBasicBlock* block, HBasicBlock* successor);

  void OrderLoopHeaderPredecessors(HBasicBlock* header);

  // Transform a loop into a format with a single preheader.
  //
  // Each phi in the header should be split: original one in the header should only hold
  // inputs reachable from the back edges and a single input from the preheader. The newly created
  // phi in the preheader should collate the inputs from the original multiple incoming blocks.
  //
  // Loops in the graph typically have a single preheader, so this method is used to "repair" loops
  // that no longer have this property.
  void TransformLoopToSinglePreheaderFormat(HBasicBlock* header);

  void SimplifyLoop(HBasicBlock* header);

  ALWAYS_INLINE int32_t AllocateInstructionId();

  int32_t GetCurrentInstructionId() const {
    return current_instruction_id_;
  }

  void SetCurrentInstructionId(int32_t id) {
    CHECK_GE(id, current_instruction_id_);
    current_instruction_id_ = id;
  }

  void UpdateTemporariesVRegSlots(size_t slots) {
    temporaries_vreg_slots_ = std::max(slots, temporaries_vreg_slots_);
  }

  size_t GetTemporariesVRegSlots() const {
    DCHECK(!in_ssa_form_);
    return temporaries_vreg_slots_;
  }

  void SetNumberOfVRegs(uint16_t number_of_vregs) {
    number_of_vregs_ = number_of_vregs;
  }

  uint16_t GetNumberOfVRegs() const {
    return number_of_vregs_;
  }

  void SetNumberOfInVRegs(uint16_t value) {
    number_of_in_vregs_ = value;
  }

  uint16_t GetNumberOfInVRegs() const {
    return number_of_in_vregs_;
  }

  uint16_t GetNumberOfLocalVRegs() const {
    DCHECK(!in_ssa_form_);
    return number_of_vregs_ - number_of_in_vregs_;
  }

  const ArenaVector<HBasicBlock*>& GetReversePostOrder() const {
    return reverse_post_order_;
  }

  ArrayRef<HBasicBlock* const> GetReversePostOrderSkipEntryBlock() const {
    DCHECK(GetReversePostOrder()[0] == entry_block_);
    return ArrayRef<HBasicBlock* const>(GetReversePostOrder()).SubArray(1);
  }

  IterationRange<ArenaVector<HBasicBlock*>::const_reverse_iterator> GetPostOrder() const {
    return ReverseRange(GetReversePostOrder());
  }

  const ArenaVector<HBasicBlock*>& GetLinearOrder() const {
    return linear_order_;
  }

  IterationRange<ArenaVector<HBasicBlock*>::const_reverse_iterator> GetLinearPostOrder() const {
    return ReverseRange(GetLinearOrder());
  }

  bool HasBoundsChecks() const {
    return has_bounds_checks_;
  }

  void SetHasBoundsChecks(bool value) {
    has_bounds_checks_ = value;
  }

  // Is the code known to be robust against eliminating dead references
  // and the effects of early finalization?
  bool IsDeadReferenceSafe() const { return dead_reference_safe_; }

  void MarkDeadReferenceUnsafe() { dead_reference_safe_ = false; }

  bool IsDebuggable() const { return debuggable_; }

  // Returns a constant of the given type and value. If it does not exist
  // already, it is created and inserted into the graph. This method is only for
  // integral types.
  HConstant* GetConstant(DataType::Type type, int64_t value);

  // TODO: This is problematic for the consistency of reference type propagation
  // because it can be created anytime after the pass and thus it will be left
  // with an invalid type.
  HNullConstant* GetNullConstant();

  HIntConstant* GetIntConstant(int32_t value);
  HLongConstant* GetLongConstant(int64_t value);
  HFloatConstant* GetFloatConstant(float value);
  HDoubleConstant* GetDoubleConstant(double value);

  HCurrentMethod* GetCurrentMethod();

  const DexFile& GetDexFile() const {
    return dex_file_;
  }

  uint32_t GetMethodIdx() const {
    return method_idx_;
  }

  // Get the method name (without the signature), e.g. "<init>"
  const char* GetMethodName() const;

  // Get the pretty method name (class + name + optionally signature).
  std::string PrettyMethod(bool with_signature = true) const;

  InvokeType GetInvokeType() const {
    return invoke_type_;
  }

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  bool IsCompilingOsr() const { return compilation_kind_ == CompilationKind::kOsr; }

  bool IsCompilingBaseline() const { return compilation_kind_ == CompilationKind::kBaseline; }

  CompilationKind GetCompilationKind() const { return compilation_kind_; }

  ArenaSet<ArtMethod*>& GetCHASingleImplementationList() {
    return cha_single_implementation_list_;
  }

  // In case of OSR we intend to use SuspendChecks as an entry point to the
  // function; for debuggable graphs we might deoptimize to interpreter from
  // SuspendChecks. In these cases we should always generate code for them.
  bool SuspendChecksAreAllowedToNoOp() const {
    return !IsDebuggable() && !IsCompilingOsr();
  }

  void AddCHASingleImplementationDependency(ArtMethod* method) {
    cha_single_implementation_list_.insert(method);
  }

  bool HasShouldDeoptimizeFlag() const {
    return number_of_cha_guards_ != 0 || debuggable_;
  }

  bool HasTryCatch() const { return has_try_catch_; }
  void SetHasTryCatch(bool value) { has_try_catch_ = value; }

  bool HasMonitorOperations() const { return has_monitor_operations_; }
  void SetHasMonitorOperations(bool value) { has_monitor_operations_ = value; }

  bool HasTraditionalSIMD() { return has_traditional_simd_; }
  void SetHasTraditionalSIMD(bool value) { has_traditional_simd_ = value; }

  bool HasPredicatedSIMD() { return has_predicated_simd_; }
  void SetHasPredicatedSIMD(bool value) { has_predicated_simd_ = value; }

  bool HasSIMD() const { return has_traditional_simd_ || has_predicated_simd_; }

  bool HasLoops() const { return has_loops_; }
  void SetHasLoops(bool value) { has_loops_ = value; }

  bool HasIrreducibleLoops() const { return has_irreducible_loops_; }
  void SetHasIrreducibleLoops(bool value) { has_irreducible_loops_ = value; }

  bool HasDirectCriticalNativeCall() const { return has_direct_critical_native_call_; }
  void SetHasDirectCriticalNativeCall(bool value) { has_direct_critical_native_call_ = value; }

  bool HasAlwaysThrowingInvokes() const { return has_always_throwing_invokes_; }
  void SetHasAlwaysThrowingInvokes(bool value) { has_always_throwing_invokes_ = value; }

  ArtMethod* GetArtMethod() const { return art_method_; }
  void SetArtMethod(ArtMethod* method) { art_method_ = method; }

  void SetProfilingInfo(ProfilingInfo* info) { profiling_info_ = info; }
  ProfilingInfo* GetProfilingInfo() const { return profiling_info_; }

  ReferenceTypeInfo GetInexactObjectRti() {
    return ReferenceTypeInfo::Create(handle_cache_.GetObjectClassHandle(), /* is_exact= */ false);
  }

  uint32_t GetNumberOfCHAGuards() const { return number_of_cha_guards_; }
  void SetNumberOfCHAGuards(uint32_t num) { number_of_cha_guards_ = num; }
  void IncrementNumberOfCHAGuards() { number_of_cha_guards_++; }

  void SetUsefulOptimizing() { useful_optimizing_ = true; }
  bool IsUsefulOptimizing() const { return useful_optimizing_; }

 private:
  void RemoveDeadBlocksInstructionsAsUsersAndDisconnect(BitVectorView<const size_t> visited) const;
  void RemoveDeadBlocks(BitVectorView<const size_t> visited);

  template <class InstructionType, typename ValueType>
  InstructionType* CreateConstant(ValueType value,
                                  ArenaSafeMap<ValueType, InstructionType*>* cache);

  void InsertConstant(HConstant* instruction);

  // Cache a float constant into the graph. This method should only be
  // called by the SsaBuilder when creating "equivalent" instructions.
  void CacheFloatConstant(HFloatConstant* constant);

  // See CacheFloatConstant comment.
  void CacheDoubleConstant(HDoubleConstant* constant);

  ArenaAllocator* const allocator_;
  ArenaStack* const arena_stack_;

  HandleCache handle_cache_;

  // List of blocks in insertion order.
  ArenaVector<HBasicBlock*> blocks_;

  // List of blocks to perform a reverse post order tree traversal.
  ArenaVector<HBasicBlock*> reverse_post_order_;

  // List of blocks to perform a linear order tree traversal. Unlike the reverse
  // post order, this order is not incrementally kept up-to-date.
  ArenaVector<HBasicBlock*> linear_order_;

  HBasicBlock* entry_block_;
  HBasicBlock* exit_block_;

  // The number of virtual registers in this method. Contains the parameters.
  uint16_t number_of_vregs_;

  // The number of virtual registers used by parameters of this method.
  uint16_t number_of_in_vregs_;

  // Number of vreg size slots that the temporaries use (used in baseline compiler).
  size_t temporaries_vreg_slots_;

  // Flag whether there are bounds checks in the graph. We can skip
  // BCE if it's false.
  bool has_bounds_checks_;

  // Flag whether there are try/catch blocks in the graph. We will skip
  // try/catch-related passes if it's false.
  bool has_try_catch_;

  // Flag whether there are any HMonitorOperation in the graph. If yes this will mandate
  // DexRegisterMap to be present to allow deadlock analysis for non-debuggable code.
  bool has_monitor_operations_;

  // Flags whether SIMD (traditional or predicated) instructions appear in the graph.
  // If either is true, the code generators may have to be more careful spilling the wider
  // contents of SIMD registers.
  bool has_traditional_simd_;
  bool has_predicated_simd_;

  // Flag whether there are any loops in the graph. We can skip loop
  // optimization if it's false.
  bool has_loops_;

  // Flag whether there are any irreducible loops in the graph.
  bool has_irreducible_loops_;

  // Flag whether there are any direct calls to native code registered
  // for @CriticalNative methods.
  bool has_direct_critical_native_call_;

  // Flag whether the graph contains invokes that always throw.
  bool has_always_throwing_invokes_;

  // Is the code known to be robust against eliminating dead references
  // and the effects of early finalization? If false, dead reference variables
  // are kept if they might be visible to the garbage collector.
  // Currently this means that the class was declared to be dead-reference-safe,
  // the method accesses no reachability-sensitive fields or data, and the same
  // is true for any methods that were inlined into the current one.
  bool dead_reference_safe_;

  // Indicates whether the graph should be compiled in a way that
  // ensures full debuggability. If false, we can apply more
  // aggressive optimizations that may limit the level of debugging.
  const bool debuggable_;

  // The current id to assign to a newly added instruction. See HInstruction.id_.
  int32_t current_instruction_id_;

  // The dex file from which the method is from.
  const DexFile& dex_file_;

  // The method index in the dex file.
  const uint32_t method_idx_;

  // If inlined, this encodes how the callee is being invoked.
  const InvokeType invoke_type_;

  // Whether the graph has been transformed to SSA form. Only used
  // in debug mode to ensure we are not using properties only valid
  // for non-SSA form (like the number of temporaries).
  bool in_ssa_form_;

  // Number of CHA guards in the graph. Used to short-circuit the
  // CHA guard optimization pass when there is no CHA guard left.
  uint32_t number_of_cha_guards_;

  const InstructionSet instruction_set_;

  // Cached constants.
  HNullConstant* cached_null_constant_;
  ArenaSafeMap<int32_t, HIntConstant*> cached_int_constants_;
  ArenaSafeMap<int32_t, HFloatConstant*> cached_float_constants_;
  ArenaSafeMap<int64_t, HLongConstant*> cached_long_constants_;
  ArenaSafeMap<int64_t, HDoubleConstant*> cached_double_constants_;

  HCurrentMethod* cached_current_method_;

  // The ArtMethod this graph is for. Note that for AOT, it may be null,
  // for example for methods whose declaring class could not be resolved
  // (such as when the superclass could not be found).
  ArtMethod* art_method_;

  // The `ProfilingInfo` associated with the method being compiled.
  ProfilingInfo* profiling_info_;

  // How we are compiling the graph: either optimized, osr, or baseline.
  // For osr, we will make all loops seen as irreducible and emit special
  // stack maps to mark compiled code entries which the interpreter can
  // directly jump to.
  const CompilationKind compilation_kind_;

  // Whether after compiling baseline it is still useful re-optimizing this
  // method.
  bool useful_optimizing_;

  // List of methods that are assumed to have single implementation.
  ArenaSet<ArtMethod*> cha_single_implementation_list_;

  friend class SsaBuilder;           // For caching constants.
  friend class SsaLivenessAnalysis;  // For the linear order.
  friend class HInliner;             // For the reverse post order.
  ART_FRIEND_TEST(GraphTest, IfSuccessorSimpleJoinBlock1);
  DISALLOW_COPY_AND_ASSIGN(HGraph);
};

class HLoopInformation : public ArenaObject<kArenaAllocLoopInfo> {
 public:
  HLoopInformation(HBasicBlock* header, HGraph* graph)
      : header_(header),
        suspend_check_(nullptr),
        irreducible_(false),
        contains_irreducible_loop_(false),
        back_edges_(graph->GetAllocator()->Adapter(kArenaAllocLoopInfoBackEdges)),
        // Make bit vector growable, as the number of blocks may change.
        blocks_(graph->GetAllocator(),
                graph->GetBlocks().size(),
                true,
                kArenaAllocLoopInfoBackEdges) {
    back_edges_.reserve(kDefaultNumberOfBackEdges);
  }

  bool IsIrreducible() const { return irreducible_; }
  bool ContainsIrreducibleLoop() const { return contains_irreducible_loop_; }

  void Dump(std::ostream& os);

  HBasicBlock* GetHeader() const {
    return header_;
  }

  void SetHeader(HBasicBlock* block) {
    header_ = block;
  }

  HSuspendCheck* GetSuspendCheck() const { return suspend_check_; }
  void SetSuspendCheck(HSuspendCheck* check) { suspend_check_ = check; }
  bool HasSuspendCheck() const { return suspend_check_ != nullptr; }

  void AddBackEdge(HBasicBlock* back_edge) {
    back_edges_.push_back(back_edge);
  }

  void RemoveBackEdge(HBasicBlock* back_edge) {
    RemoveElement(back_edges_, back_edge);
  }

  bool IsBackEdge(const HBasicBlock& block) const {
    return ContainsElement(back_edges_, &block);
  }

  size_t NumberOfBackEdges() const {
    return back_edges_.size();
  }

  HBasicBlock* GetPreHeader() const;

  const ArenaVector<HBasicBlock*>& GetBackEdges() const {
    return back_edges_;
  }

  // Returns the lifetime position of the back edge that has the
  // greatest lifetime position.
  size_t GetLifetimeEnd() const;

  void ReplaceBackEdge(HBasicBlock* existing, HBasicBlock* new_back_edge) {
    ReplaceElement(back_edges_, existing, new_back_edge);
  }

  // Finds blocks that are part of this loop.
  void Populate();

  // Updates blocks population of the loop and all of its outer' ones recursively after the
  // population of the inner loop is updated.
  void PopulateInnerLoopUpwards(HLoopInformation* inner_loop);

  // Returns whether this loop information contains `block`.
  // Note that this loop information *must* be populated before entering this function.
  bool Contains(const HBasicBlock& block) const;

  // Returns whether this loop information is an inner loop of `other`.
  // Note that `other` *must* be populated before entering this function.
  bool IsIn(const HLoopInformation& other) const;

  // Returns true if instruction is not defined within this loop.
  bool IsDefinedOutOfTheLoop(HInstruction* instruction) const;

  const ArenaBitVector& GetBlocks() const { return blocks_; }

  void Add(HBasicBlock* block);
  void Remove(HBasicBlock* block);

  void ClearAllBlocks() {
    blocks_.ClearAllBits();
  }

  bool HasBackEdgeNotDominatedByHeader() const;

  bool IsPopulated() const {
    return blocks_.GetHighestBitSet() != -1;
  }

  bool DominatesAllBackEdges(HBasicBlock* block);

  bool HasExitEdge() const;

  // Resets back edge and blocks-in-loop data.
  void ResetBasicBlockData() {
    back_edges_.clear();
    ClearAllBlocks();
  }

 private:
  // Internal recursive implementation of `Populate`.
  void PopulateRecursive(HBasicBlock* block);
  void PopulateIrreducibleRecursive(HBasicBlock* block, ArenaBitVector* finalized);

  HBasicBlock* header_;
  HSuspendCheck* suspend_check_;
  bool irreducible_;
  bool contains_irreducible_loop_;
  ArenaVector<HBasicBlock*> back_edges_;
  ArenaBitVector blocks_;

  DISALLOW_COPY_AND_ASSIGN(HLoopInformation);
};

// Stores try/catch information for basic blocks.
// Note that HGraph is constructed so that catch blocks cannot simultaneously
// be try blocks.
class TryCatchInformation : public ArenaObject<kArenaAllocTryCatchInfo> {
 public:
  // Try block information constructor.
  explicit TryCatchInformation(const HTryBoundary& try_entry)
      : try_entry_(&try_entry),
        catch_dex_file_(nullptr),
        catch_type_index_(dex::TypeIndex::Invalid()) {
    DCHECK(try_entry_ != nullptr);
  }

  // Catch block information constructor.
  TryCatchInformation(dex::TypeIndex catch_type_index, const DexFile& dex_file)
      : try_entry_(nullptr),
        catch_dex_file_(&dex_file),
        catch_type_index_(catch_type_index) {}

  bool IsTryBlock() const { return try_entry_ != nullptr; }

  const HTryBoundary& GetTryEntry() const {
    DCHECK(IsTryBlock());
    return *try_entry_;
  }

  bool IsCatchBlock() const { return catch_dex_file_ != nullptr; }

  bool IsValidTypeIndex() const {
    DCHECK(IsCatchBlock());
    return catch_type_index_.IsValid();
  }

  dex::TypeIndex GetCatchTypeIndex() const {
    DCHECK(IsCatchBlock());
    return catch_type_index_;
  }

  const DexFile& GetCatchDexFile() const {
    DCHECK(IsCatchBlock());
    return *catch_dex_file_;
  }

  void SetInvalidTypeIndex() {
    catch_type_index_ = dex::TypeIndex::Invalid();
  }

 private:
  // One of possibly several TryBoundary instructions entering the block's try.
  // Only set for try blocks.
  const HTryBoundary* try_entry_;

  // Exception type information. Only set for catch blocks.
  const DexFile* catch_dex_file_;
  dex::TypeIndex catch_type_index_;
};

static constexpr size_t kNoLifetime = -1;
static constexpr uint32_t kInvalidBlockId = static_cast<uint32_t>(-1);

// A block in a method. Contains the list of instructions represented
// as a double linked list. Each block knows its predecessors and
// successors.

class HBasicBlock : public ArenaObject<kArenaAllocBasicBlock> {
 public:
  explicit HBasicBlock(HGraph* graph, uint32_t dex_pc = kNoDexPc)
      : graph_(graph),
        predecessors_(graph->GetAllocator()->Adapter(kArenaAllocPredecessors)),
        successors_(graph->GetAllocator()->Adapter(kArenaAllocSuccessors)),
        loop_information_(nullptr),
        dominator_(nullptr),
        dominated_blocks_(graph->GetAllocator()->Adapter(kArenaAllocDominated)),
        block_id_(kInvalidBlockId),
        dex_pc_(dex_pc),
        lifetime_start_(kNoLifetime),
        lifetime_end_(kNoLifetime),
        try_catch_information_(nullptr) {
    predecessors_.reserve(kDefaultNumberOfPredecessors);
    successors_.reserve(kDefaultNumberOfSuccessors);
    dominated_blocks_.reserve(kDefaultNumberOfDominatedBlocks);
  }

  const ArenaVector<HBasicBlock*>& GetPredecessors() const {
    return predecessors_;
  }

  size_t GetNumberOfPredecessors() const {
    return GetPredecessors().size();
  }

  const ArenaVector<HBasicBlock*>& GetSuccessors() const {
    return successors_;
  }

  ArrayRef<HBasicBlock* const> GetNormalSuccessors() const;
  ArrayRef<HBasicBlock* const> GetExceptionalSuccessors() const;

  bool HasSuccessor(const HBasicBlock* block, size_t start_from = 0u) {
    return ContainsElement(successors_, block, start_from);
  }

  const ArenaVector<HBasicBlock*>& GetDominatedBlocks() const {
    return dominated_blocks_;
  }

  bool IsEntryBlock() const {
    return graph_->GetEntryBlock() == this;
  }

  bool IsExitBlock() const {
    return graph_->GetExitBlock() == this;
  }

  bool IsSingleGoto() const;
  bool IsSingleReturn() const;
  bool IsSingleReturnOrReturnVoidAllowingPhis() const;
  bool IsSingleTryBoundary() const;

  // Returns true if this block emits nothing but a jump.
  bool IsSingleJump() const {
    HLoopInformation* loop_info = GetLoopInformation();
    return (IsSingleGoto() || IsSingleTryBoundary())
           // Back edges generate a suspend check.
           && (loop_info == nullptr || !loop_info->IsBackEdge(*this));
  }

  void AddBackEdge(HBasicBlock* back_edge) {
    if (loop_information_ == nullptr) {
      loop_information_ = new (graph_->GetAllocator()) HLoopInformation(this, graph_);
    }
    DCHECK_EQ(loop_information_->GetHeader(), this);
    loop_information_->AddBackEdge(back_edge);
  }

  // Registers a back edge; if the block was not a loop header before the call associates a newly
  // created loop info with it.
  //
  // Used in SuperblockCloner to preserve LoopInformation object instead of reseting loop
  // info for all blocks during back edges recalculation.
  void AddBackEdgeWhileUpdating(HBasicBlock* back_edge) {
    if (loop_information_ == nullptr || loop_information_->GetHeader() != this) {
      loop_information_ = new (graph_->GetAllocator()) HLoopInformation(this, graph_);
    }
    loop_information_->AddBackEdge(back_edge);
  }

  HGraph* GetGraph() const { return graph_; }
  void SetGraph(HGraph* graph) { graph_ = graph; }

  uint32_t GetBlockId() const { return block_id_; }
  void SetBlockId(int id) { block_id_ = id; }
  uint32_t GetDexPc() const { return dex_pc_; }

  HBasicBlock* GetDominator() const { return dominator_; }
  void SetDominator(HBasicBlock* dominator) { dominator_ = dominator; }
  void AddDominatedBlock(HBasicBlock* block) { dominated_blocks_.push_back(block); }

  void RemoveDominatedBlock(HBasicBlock* block) {
    RemoveElement(dominated_blocks_, block);
  }

  void ReplaceDominatedBlock(HBasicBlock* existing, HBasicBlock* new_block) {
    ReplaceElement(dominated_blocks_, existing, new_block);
  }

  void ClearDominanceInformation();

  int NumberOfBackEdges() const {
    return IsLoopHeader() ? loop_information_->NumberOfBackEdges() : 0;
  }

  HInstruction* GetFirstInstruction() const { return instructions_.first_instruction_; }
  HInstruction* GetLastInstruction() const { return instructions_.last_instruction_; }
  const HInstructionList& GetInstructions() const { return instructions_; }
  HInstruction* GetFirstPhi() const { return phis_.first_instruction_; }
  HInstruction* GetLastPhi() const { return phis_.last_instruction_; }
  const HInstructionList& GetPhis() const { return phis_; }

  HInstruction* GetFirstInstructionDisregardMoves() const;

  void AddSuccessor(HBasicBlock* block) {
    successors_.push_back(block);
    block->predecessors_.push_back(this);
  }

  void ReplaceSuccessor(HBasicBlock* existing, HBasicBlock* new_block) {
    size_t successor_index = GetSuccessorIndexOf(existing);
    existing->RemovePredecessor(this);
    new_block->predecessors_.push_back(this);
    successors_[successor_index] = new_block;
  }

  void ReplacePredecessor(HBasicBlock* existing, HBasicBlock* new_block) {
    size_t predecessor_index = GetPredecessorIndexOf(existing);
    existing->RemoveSuccessor(this);
    new_block->successors_.push_back(this);
    predecessors_[predecessor_index] = new_block;
  }

  // Insert `this` between `predecessor` and `successor. This method
  // preserves the indices, and will update the first edge found between
  // `predecessor` and `successor`.
  void InsertBetween(HBasicBlock* predecessor, HBasicBlock* successor) {
    size_t predecessor_index = successor->GetPredecessorIndexOf(predecessor);
    size_t successor_index = predecessor->GetSuccessorIndexOf(successor);
    successor->predecessors_[predecessor_index] = this;
    predecessor->successors_[successor_index] = this;
    successors_.push_back(successor);
    predecessors_.push_back(predecessor);
  }

  void RemovePredecessor(HBasicBlock* block) {
    predecessors_.erase(predecessors_.begin() + GetPredecessorIndexOf(block));
  }

  void RemoveSuccessor(HBasicBlock* block) {
    successors_.erase(successors_.begin() + GetSuccessorIndexOf(block));
  }

  void ClearAllPredecessors() {
    predecessors_.clear();
  }

  void AddPredecessor(HBasicBlock* block) {
    predecessors_.push_back(block);
    block->successors_.push_back(this);
  }

  void SwapPredecessors() {
    DCHECK_EQ(predecessors_.size(), 2u);
    std::swap(predecessors_[0], predecessors_[1]);
  }

  void SwapSuccessors() {
    DCHECK_EQ(successors_.size(), 2u);
    std::swap(successors_[0], successors_[1]);
  }

  size_t GetPredecessorIndexOf(HBasicBlock* predecessor) const {
    return IndexOfElement(predecessors_, predecessor);
  }

  size_t GetSuccessorIndexOf(HBasicBlock* successor) const {
    return IndexOfElement(successors_, successor);
  }

  HBasicBlock* GetSinglePredecessor() const {
    DCHECK_EQ(GetPredecessors().size(), 1u);
    return GetPredecessors()[0];
  }

  HBasicBlock* GetSingleSuccessor() const {
    DCHECK_EQ(GetSuccessors().size(), 1u);
    return GetSuccessors()[0];
  }

  // Returns whether the first occurrence of `predecessor` in the list of
  // predecessors is at index `idx`.
  bool IsFirstIndexOfPredecessor(HBasicBlock* predecessor, size_t idx) const {
    DCHECK_EQ(GetPredecessors()[idx], predecessor);
    return GetPredecessorIndexOf(predecessor) == idx;
  }

  // Create a new block between this block and its predecessors. The new block
  // is added to the graph, all predecessor edges are relinked to it and an edge
  // is created to `this`. Returns the new empty block. Reverse post order or
  // loop and try/catch information are not updated.
  HBasicBlock* CreateImmediateDominator();

  // Split the block into two blocks just before `cursor`. Returns the newly
  // created, latter block. Note that this method will add the block to the
  // graph, create a Goto at the end of the former block and will create an edge
  // between the blocks. It will not, however, update the reverse post order or
  // loop and try/catch information.
  HBasicBlock* SplitBefore(HInstruction* cursor, bool require_graph_not_in_ssa_form = true);

  // Split the block into two blocks just before `cursor`. Returns the newly
  // created block. Note that this method just updates raw block information,
  // like predecessors, successors, dominators, and instruction list. It does not
  // update the graph, reverse post order, loop information, nor make sure the
  // blocks are consistent (for example ending with a control flow instruction).
  HBasicBlock* SplitBeforeForInlining(HInstruction* cursor);

  // Similar to `SplitBeforeForInlining` but does it after `cursor`.
  HBasicBlock* SplitAfterForInlining(HInstruction* cursor);

  // Merge `other` at the end of `this`. Successors and dominated blocks of
  // `other` are changed to be successors and dominated blocks of `this`. Note
  // that this method does not update the graph, reverse post order, loop
  // information, nor make sure the blocks are consistent (for example ending
  // with a control flow instruction).
  void MergeWithInlined(HBasicBlock* other);

  // Replace `this` with `other`. Predecessors, successors, and dominated blocks
  // of `this` are moved to `other`.
  // Note that this method does not update the graph, reverse post order, loop
  // information, nor make sure the blocks are consistent (for example ending
  // with a control flow instruction).
  void ReplaceWith(HBasicBlock* other);

  // Merges the instructions of `other` at the end of `this`.
  void MergeInstructionsWith(HBasicBlock* other);

  // Merge `other` at the end of `this`. This method updates loops, reverse post
  // order, links to predecessors, successors, dominators and deletes the block
  // from the graph. The two blocks must be successive, i.e. `this` the only
  // predecessor of `other` and vice versa.
  void MergeWith(HBasicBlock* other);

  // Disconnects `this` from all its predecessors, successors and dominator,
  // removes it from all loops it is included in and eventually from the graph.
  // The block must not dominate any other block. Predecessors and successors
  // are safely updated.
  void DisconnectAndDelete();

  // Disconnects `this` from all its successors and updates their phis, if the successors have them.
  // If `visited` is provided, it will use the information to know if a successor is reachable and
  // skip updating those phis.
  void DisconnectFromSuccessors(BitVectorView<const size_t> visited = {});

  // Removes the catch phi uses of the instructions in `this`, and then remove the instruction
  // itself. If `building_dominator_tree` is true, it will not remove the instruction as user, since
  // we do it in a previous step. This is a special case for building up the dominator tree: we want
  // to eliminate uses before inputs but we don't have domination information, so we remove all
  // connections from input/uses first before removing any instruction.
  // This method assumes the instructions have been removed from all users with the exception of
  // catch phis because of missing exceptional edges in the graph.
  void RemoveCatchPhiUsesAndInstruction(bool building_dominator_tree);

  void AddInstruction(HInstruction* instruction);
  // Insert `instruction` before/after an existing instruction `cursor`.
  void InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor);
  void InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor);
  // Replace phi `initial` with `replacement` within this block.
  void ReplaceAndRemovePhiWith(HPhi* initial, HPhi* replacement);
  // Replace instruction `initial` with `replacement` within this block.
  void ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                       HInstruction* replacement);
  void AddPhi(HPhi* phi);
  void InsertPhiAfter(HPhi* instruction, HPhi* cursor);
  // RemoveInstruction and RemovePhi delete a given instruction from the respective
  // instruction list. With 'ensure_safety' set to true, it verifies that the
  // instruction is not in use and removes it from the use lists of its inputs.
  void RemoveInstruction(HInstruction* instruction, bool ensure_safety = true);
  void RemovePhi(HPhi* phi, bool ensure_safety = true);
  void RemoveInstructionOrPhi(HInstruction* instruction, bool ensure_safety = true);

  bool IsLoopHeader() const {
    return IsInLoop() && (loop_information_->GetHeader() == this);
  }

  bool IsLoopPreHeaderFirstPredecessor() const {
    DCHECK(IsLoopHeader());
    return GetPredecessors()[0] == GetLoopInformation()->GetPreHeader();
  }

  bool IsFirstPredecessorBackEdge() const {
    DCHECK(IsLoopHeader());
    return GetLoopInformation()->IsBackEdge(*GetPredecessors()[0]);
  }

  HLoopInformation* GetLoopInformation() const {
    return loop_information_;
  }

  // Set the loop_information_ on this block. Overrides the current
  // loop_information if it is an outer loop of the passed loop information.
  // Note that this method is called while creating the loop information.
  void SetInLoop(HLoopInformation* info) {
    if (IsLoopHeader()) {
      // Nothing to do. This just means `info` is an outer loop.
    } else if (!IsInLoop()) {
      loop_information_ = info;
    } else if (loop_information_->Contains(*info->GetHeader())) {
      // Block is currently part of an outer loop. Make it part of this inner loop.
      // Note that a non loop header having a loop information means this loop information
      // has already been populated
      loop_information_ = info;
    } else {
      // Block is part of an inner loop. Do not update the loop information.
      // Note that we cannot do the check `info->Contains(loop_information_)->GetHeader()`
      // at this point, because this method is being called while populating `info`.
    }
  }

  // Raw update of the loop information.
  void SetLoopInformation(HLoopInformation* info) {
    loop_information_ = info;
  }

  bool IsInLoop() const { return loop_information_ != nullptr; }

  TryCatchInformation* GetTryCatchInformation() const { return try_catch_information_; }

  void SetTryCatchInformation(TryCatchInformation* try_catch_information) {
    try_catch_information_ = try_catch_information;
  }

  bool IsTryBlock() const {
    return try_catch_information_ != nullptr && try_catch_information_->IsTryBlock();
  }

  bool IsCatchBlock() const {
    return try_catch_information_ != nullptr && try_catch_information_->IsCatchBlock();
  }

  // Returns the try entry that this block's successors should have. They will
  // be in the same try, unless the block ends in a try boundary. In that case,
  // the appropriate try entry will be returned.
  const HTryBoundary* ComputeTryEntryOfSuccessors() const;

  bool HasThrowingInstructions() const;

  // Returns whether this block dominates the blocked passed as parameter.
  bool Dominates(const HBasicBlock* block) const;

  size_t GetLifetimeStart() const { return lifetime_start_; }
  size_t GetLifetimeEnd() const { return lifetime_end_; }

  void SetLifetimeStart(size_t start) { lifetime_start_ = start; }
  void SetLifetimeEnd(size_t end) { lifetime_end_ = end; }

  bool EndsWithControlFlowInstruction() const;
  bool EndsWithReturn() const;
  bool EndsWithIf() const;
  bool EndsWithTryBoundary() const;
  bool HasSinglePhi() const;

 private:
  HGraph* graph_;
  ArenaVector<HBasicBlock*> predecessors_;
  ArenaVector<HBasicBlock*> successors_;
  HInstructionList instructions_;
  HInstructionList phis_;
  HLoopInformation* loop_information_;
  HBasicBlock* dominator_;
  ArenaVector<HBasicBlock*> dominated_blocks_;
  uint32_t block_id_;
  // The dex program counter of the first instruction of this block.
  const uint32_t dex_pc_;
  size_t lifetime_start_;
  size_t lifetime_end_;
  TryCatchInformation* try_catch_information_;

  friend class HGraph;
  friend class HInstruction;
  // Allow manual control of the ordering of predecessors/successors
  friend class OptimizingUnitTestHelper;

  DISALLOW_COPY_AND_ASSIGN(HBasicBlock);
};

// Iterates over the LoopInformation of all loops which contain 'block'
// from the innermost to the outermost.
class HLoopInformationOutwardIterator : public ValueObject {
 public:
  explicit HLoopInformationOutwardIterator(const HBasicBlock& block)
      : current_(block.GetLoopInformation()) {}

  bool Done() const { return current_ == nullptr; }

  void Advance() {
    DCHECK(!Done());
    current_ = current_->GetPreHeader()->GetLoopInformation();
  }

  HLoopInformation* Current() const {
    DCHECK(!Done());
    return current_;
  }

 private:
  HLoopInformation* current_;

  DISALLOW_COPY_AND_ASSIGN(HLoopInformationOutwardIterator);
};

#define FOR_EACH_CONCRETE_INSTRUCTION_SCALAR_COMMON(M)                  \
  M(Above, Condition)                                                   \
  M(AboveOrEqual, Condition)                                            \
  M(Abs, UnaryOperation)                                                \
  M(Add, BinaryOperation)                                               \
  M(And, BinaryOperation)                                               \
  M(ArrayGet, Instruction)                                              \
  M(ArrayLength, Instruction)                                           \
  M(ArraySet, Instruction)                                              \
  M(Below, Condition)                                                   \
  M(BelowOrEqual, Condition)                                            \
  M(BitwiseNegatedRight, BinaryOperation)                               \
  M(BooleanNot, UnaryOperation)                                         \
  M(BoundsCheck, Instruction)                                           \
  M(BoundType, Instruction)                                             \
  M(CheckCast, Instruction)                                             \
  M(ClassTableGet, Instruction)                                         \
  M(ClearException, Instruction)                                        \
  M(ClinitCheck, Instruction)                                           \
  M(Compare, BinaryOperation)                                           \
  M(ConstructorFence, Instruction)                                      \
  M(CurrentMethod, Instruction)                                         \
  M(ShouldDeoptimizeFlag, Instruction)                                  \
  M(Deoptimize, Instruction)                                            \
  M(Div, BinaryOperation)                                               \
  M(DivZeroCheck, Instruction)                                          \
  M(DoubleConstant, Constant)                                           \
  M(Equal, Condition)                                                   \
  M(Exit, Instruction)                                                  \
  M(FloatConstant, Constant)                                            \
  M(Goto, Instruction)                                                  \
  M(GreaterThan, Condition)                                             \
  M(GreaterThanOrEqual, Condition)                                      \
  M(If, Instruction)                                                    \
  M(InstanceFieldGet, FieldAccess)                                      \
  M(InstanceFieldSet, FieldAccess)                                      \
  M(InstanceOf, Instruction)                                            \
  M(IntConstant, Constant)                                              \
  M(IntermediateAddress, Instruction)                                   \
  M(InvokeUnresolved, Invoke)                                           \
  M(InvokeInterface, Invoke)                                            \
  M(InvokeStaticOrDirect, Invoke)                                       \
  M(InvokeVirtual, Invoke)                                              \
  M(InvokePolymorphic, Invoke)                                          \
  M(InvokeCustom, Invoke)                                               \
  M(LessThan, Condition)                                                \
  M(LessThanOrEqual, Condition)                                         \
  M(LoadClass, Instruction)                                             \
  M(LoadException, Instruction)                                         \
  M(LoadMethodHandle, Instruction)                                      \
  M(LoadMethodType, Instruction)                                        \
  M(LoadString, Instruction)                                            \
  M(LongConstant, Constant)                                             \
  M(Max, Instruction)                                                   \
  M(MemoryBarrier, Instruction)                                         \
  M(MethodEntryHook, Instruction)                                       \
  M(MethodExitHook, Instruction)                                        \
  M(Min, BinaryOperation)                                               \
  M(MonitorOperation, Instruction)                                      \
  M(Mul, BinaryOperation)                                               \
  M(Neg, UnaryOperation)                                                \
  M(NewArray, Instruction)                                              \
  M(NewInstance, Instruction)                                           \
  M(Nop, Instruction)                                                   \
  M(Not, UnaryOperation)                                                \
  M(NotEqual, Condition)                                                \
  M(NullConstant, Instruction)                                          \
  M(NullCheck, Instruction)                                             \
  M(Or, BinaryOperation)                                                \
  M(PackedSwitch, Instruction)                                          \
  M(ParallelMove, Instruction)                                          \
  M(ParameterValue, Instruction)                                        \
  M(Phi, Instruction)                                                   \
  M(Rem, BinaryOperation)                                               \
  M(Return, Instruction)                                                \
  M(ReturnVoid, Instruction)                                            \
  M(Rol, BinaryOperation)                                               \
  M(Ror, BinaryOperation)                                               \
  M(Shl, BinaryOperation)                                               \
  M(Shr, BinaryOperation)                                               \
  M(StaticFieldGet, FieldAccess)                                        \
  M(StaticFieldSet, FieldAccess)                                        \
  M(StringBuilderAppend, Instruction)                                   \
  M(UnresolvedInstanceFieldGet, Instruction)                            \
  M(UnresolvedInstanceFieldSet, Instruction)                            \
  M(UnresolvedStaticFieldGet, Instruction)                              \
  M(UnresolvedStaticFieldSet, Instruction)                              \
  M(Select, Instruction)                                                \
  M(Sub, BinaryOperation)                                               \
  M(SuspendCheck, Instruction)                                          \
  M(Throw, Instruction)                                                 \
  M(TryBoundary, Instruction)                                           \
  M(TypeConversion, Instruction)                                        \
  M(UShr, BinaryOperation)                                              \
  M(Xor, BinaryOperation)

#define FOR_EACH_CONCRETE_INSTRUCTION_VECTOR_COMMON(M)                  \
  M(VecReplicateScalar, VecUnaryOperation)                              \
  M(VecExtractScalar, VecUnaryOperation)                                \
  M(VecReduce, VecUnaryOperation)                                       \
  M(VecCnv, VecUnaryOperation)                                          \
  M(VecNeg, VecUnaryOperation)                                          \
  M(VecAbs, VecUnaryOperation)                                          \
  M(VecNot, VecUnaryOperation)                                          \
  M(VecAdd, VecBinaryOperation)                                         \
  M(VecHalvingAdd, VecBinaryOperation)                                  \
  M(VecSub, VecBinaryOperation)                                         \
  M(VecMul, VecBinaryOperation)                                         \
  M(VecDiv, VecBinaryOperation)                                         \
  M(VecMin, VecBinaryOperation)                                         \
  M(VecMax, VecBinaryOperation)                                         \
  M(VecAnd, VecBinaryOperation)                                         \
  M(VecAndNot, VecBinaryOperation)                                      \
  M(VecOr, VecBinaryOperation)                                          \
  M(VecXor, VecBinaryOperation)                                         \
  M(VecSaturationAdd, VecBinaryOperation)                               \
  M(VecSaturationSub, VecBinaryOperation)                               \
  M(VecShl, VecBinaryOperation)                                         \
  M(VecShr, VecBinaryOperation)                                         \
  M(VecUShr, VecBinaryOperation)                                        \
  M(VecSetScalars, VecOperation)                                        \
  M(VecMultiplyAccumulate, VecOperation)                                \
  M(VecSADAccumulate, VecOperation)                                     \
  M(VecDotProd, VecOperation)                                           \
  M(VecLoad, VecMemoryOperation)                                        \
  M(VecStore, VecMemoryOperation)                                       \
  M(VecPredSetAll, VecPredSetOperation)                                 \
  M(VecPredWhile, VecPredSetOperation)                                  \
  M(VecPredToBoolean, VecOperation)                                     \
  M(VecEqual, VecCondition)                                             \
  M(VecNotEqual, VecCondition)                                          \
  M(VecLessThan, VecCondition)                                          \
  M(VecLessThanOrEqual, VecCondition)                                   \
  M(VecGreaterThan, VecCondition)                                       \
  M(VecGreaterThanOrEqual, VecCondition)                                \
  M(VecBelow, VecCondition)                                             \
  M(VecBelowOrEqual, VecCondition)                                      \
  M(VecAbove, VecCondition)                                             \
  M(VecAboveOrEqual, VecCondition)                                      \
  M(VecPredNot, VecPredSetOperation)

#define FOR_EACH_CONCRETE_INSTRUCTION_COMMON(M)                         \
  FOR_EACH_CONCRETE_INSTRUCTION_SCALAR_COMMON(M)                        \
  FOR_EACH_CONCRETE_INSTRUCTION_VECTOR_COMMON(M)

/*
 * Instructions, shared across several (not all) architectures.
 */
#if !defined(ART_ENABLE_CODEGEN_arm) && !defined(ART_ENABLE_CODEGEN_arm64)
#define FOR_EACH_CONCRETE_INSTRUCTION_SHARED(M)
#else
#define FOR_EACH_CONCRETE_INSTRUCTION_SHARED(M)                         \
  M(DataProcWithShifterOp, Instruction)                                 \
  M(MultiplyAccumulate, Instruction)                                    \
  M(IntermediateAddressIndex, Instruction)
#endif

#define FOR_EACH_CONCRETE_INSTRUCTION_ARM(M)

#define FOR_EACH_CONCRETE_INSTRUCTION_ARM64(M)

#if defined(ART_ENABLE_CODEGEN_riscv64)
#define FOR_EACH_CONCRETE_INSTRUCTION_RISCV64(M) M(Riscv64ShiftAdd, Instruction)
#else
#define FOR_EACH_CONCRETE_INSTRUCTION_RISCV64(M)
#endif

#ifndef ART_ENABLE_CODEGEN_x86
#define FOR_EACH_CONCRETE_INSTRUCTION_X86(M)
#else
#define FOR_EACH_CONCRETE_INSTRUCTION_X86(M)                            \
  M(X86ComputeBaseMethodAddress, Instruction)                           \
  M(X86LoadFromConstantTable, Instruction)                              \
  M(X86FPNeg, Instruction)                                              \
  M(X86PackedSwitch, Instruction)
#endif

#if defined(ART_ENABLE_CODEGEN_x86) || defined(ART_ENABLE_CODEGEN_x86_64)
#define FOR_EACH_CONCRETE_INSTRUCTION_X86_COMMON(M)                     \
  M(X86AndNot, Instruction)                                             \
  M(X86MaskOrResetLeastSetBit, Instruction)
#else
#define FOR_EACH_CONCRETE_INSTRUCTION_X86_COMMON(M)
#endif

#define FOR_EACH_CONCRETE_INSTRUCTION_X86_64(M)

#define FOR_EACH_CONCRETE_INSTRUCTION(M)                                \
  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(M)                               \
  FOR_EACH_CONCRETE_INSTRUCTION_SHARED(M)                               \
  FOR_EACH_CONCRETE_INSTRUCTION_ARM(M)                                  \
  FOR_EACH_CONCRETE_INSTRUCTION_ARM64(M)                                \
  FOR_EACH_CONCRETE_INSTRUCTION_RISCV64(M)                              \
  FOR_EACH_CONCRETE_INSTRUCTION_X86(M)                                  \
  FOR_EACH_CONCRETE_INSTRUCTION_X86_64(M)                               \
  FOR_EACH_CONCRETE_INSTRUCTION_X86_COMMON(M)

#define FOR_EACH_ABSTRACT_INSTRUCTION(M)                                \
  M(Condition, BinaryOperation)                                         \
  M(Constant, Instruction)                                              \
  M(UnaryOperation, Instruction)                                        \
  M(BinaryOperation, Instruction)                                       \
  M(FieldAccess, Instruction)                                           \
  M(Invoke, Instruction)                                                \
  M(VecOperation, Instruction)                                          \
  M(VecUnaryOperation, VecOperation)                                    \
  M(VecBinaryOperation, VecOperation)                                   \
  M(VecMemoryOperation, VecOperation)                                   \
  M(VecPredSetOperation, VecOperation)                                  \
  M(VecCondition, VecPredSetOperation)

#define FOR_EACH_INSTRUCTION(M)                                         \
  FOR_EACH_CONCRETE_INSTRUCTION(M)                                      \
  FOR_EACH_ABSTRACT_INSTRUCTION(M)

#define FORWARD_DECLARATION(type, super) class H##type;
FOR_EACH_INSTRUCTION(FORWARD_DECLARATION)
#undef FORWARD_DECLARATION

#define DECLARE_INSTRUCTION(type)                                         \
  private:                                                                \
  H##type& operator=(const H##type&) = delete;                            \
  public:                                                                 \
  const char* DebugName() const override { return #type; }                \
  HInstruction* Clone(ArenaAllocator* arena) const override {             \
    DCHECK(IsClonable());                                                 \
    return new (arena) H##type(*this);                                    \
  }                                                                       \
  void Accept(HGraphVisitor* visitor) override

#define DECLARE_ABSTRACT_INSTRUCTION(type)                              \
  private:                                                              \
  H##type& operator=(const H##type&) = delete;                          \
  public:

#define DEFAULT_COPY_CONSTRUCTOR(type) H##type(const H##type& other) = default;

template <typename T>
class HUseListNode : public ArenaObject<kArenaAllocUseListNode>,
                     public IntrusiveForwardListNode<HUseListNode<T>> {
 public:
  // Get the instruction which has this use as one of the inputs.
  T GetUser() const { return user_; }
  // Get the position of the input record that this use corresponds to.
  size_t GetIndex() const { return index_; }
  // Set the position of the input record that this use corresponds to.
  void SetIndex(size_t index) { index_ = index; }

 private:
  HUseListNode(T user, size_t index)
      : user_(user), index_(index) {}

  T const user_;
  size_t index_;

  friend class HInstruction;

  DISALLOW_COPY_AND_ASSIGN(HUseListNode);
};

template <typename T>
using HUseList = IntrusiveForwardList<HUseListNode<T>>;

// This class is used by HEnvironment and HInstruction classes to record the
// instructions they use and pointers to the corresponding HUseListNodes kept
// by the used instructions.
template <typename T>
class HUserRecord : public ValueObject {
 public:
  HUserRecord() : instruction_(nullptr), before_use_node_() {}
  explicit HUserRecord(HInstruction* instruction) : instruction_(instruction), before_use_node_() {}

  HUserRecord(const HUserRecord<T>& old_record, typename HUseList<T>::iterator before_use_node)
      : HUserRecord(old_record.instruction_, before_use_node) {}
  HUserRecord(HInstruction* instruction, typename HUseList<T>::iterator before_use_node)
      : instruction_(instruction), before_use_node_(before_use_node) {
    DCHECK(instruction_ != nullptr);
  }

  HInstruction* GetInstruction() const { return instruction_; }
  typename HUseList<T>::iterator GetBeforeUseNode() const { return before_use_node_; }
  typename HUseList<T>::iterator GetUseNode() const { return ++GetBeforeUseNode(); }

 private:
  // Instruction used by the user.
  HInstruction* instruction_;

  // Iterator before the corresponding entry in the use list kept by 'instruction_'.
  typename HUseList<T>::iterator before_use_node_;
};

// Helper class that extracts the input instruction from HUserRecord<HInstruction*>.
// This is used for HInstruction::GetInputs() to return a container wrapper providing
// HInstruction* values even though the underlying container has HUserRecord<>s.
struct HInputExtractor {
  HInstruction* operator()(HUserRecord<HInstruction*>& record) const {
    return record.GetInstruction();
  }
  const HInstruction* operator()(const HUserRecord<HInstruction*>& record) const {
    return record.GetInstruction();
  }
};

using HInputsRef = TransformArrayRef<HUserRecord<HInstruction*>, HInputExtractor>;
using HConstInputsRef = TransformArrayRef<const HUserRecord<HInstruction*>, HInputExtractor>;

/**
 * Side-effects representation.
 *
 * For write/read dependences on fields/arrays, the dependence analysis uses
 * type disambiguation (e.g. a float field write cannot modify the value of an
 * integer field read) and the access type (e.g.  a reference array write cannot
 * modify the value of a reference field read [although it may modify the
 * reference fetch prior to reading the field, which is represented by its own
 * write/read dependence]). The analysis makes conservative points-to
 * assumptions on reference types (e.g. two same typed arrays are assumed to be
 * the same, and any reference read depends on any reference read without
 * further regard of its type).
 *
 * kDependsOnGCBit is defined in the following way: instructions with kDependsOnGCBit must not be
 * alive across the point where garbage collection might happen.
 *
 * Note: Instructions with kCanTriggerGCBit do not depend on each other.
 *
 * kCanTriggerGCBit must be used for instructions for which GC might happen on the path across
 * those instructions from the compiler perspective (between this instruction and the next one
 * in the IR).
 *
 * Note: Instructions which can cause GC only on a fatal slow path do not need
 *       kCanTriggerGCBit as the execution never returns to the instruction next to the exceptional
 *       one. However the execution may return to compiled code if there is a catch block in the
 *       current method; for this purpose the TryBoundary exit instruction has kCanTriggerGCBit
 *       set.
 *
 * The internal representation uses 38-bit and is described in the table below.
 * The first line indicates the side effect, and for field/array accesses the
 * second line indicates the type of the access (in the order of the
 * DataType::Type enum).
 * The two numbered lines below indicate the bit position in the bitfield (read
 * vertically).
 *
 *   |Depends on GC|ARRAY-R  |FIELD-R  |Can trigger GC|ARRAY-W  |FIELD-W  |
 *   +-------------+---------+---------+--------------+---------+---------+
 *   |             |DFJISCBZL|DFJISCBZL|              |DFJISCBZL|DFJISCBZL|
 *   |      3      |333333322|222222221|       1      |111111110|000000000|
 *   |      7      |654321098|765432109|       8      |765432109|876543210|
 *
 * Note that, to ease the implementation, 'changes' bits are least significant
 * bits, while 'dependency' bits are most significant bits.
 */
class SideEffects : public ValueObject {
 public:
  SideEffects() : flags_(0) {}

  static SideEffects None() {
    return SideEffects(0);
  }

  static SideEffects All() {
    return SideEffects(kAllChangeBits | kAllDependOnBits);
  }

  static SideEffects AllChanges() {
    return SideEffects(kAllChangeBits);
  }

  static SideEffects AllDependencies() {
    return SideEffects(kAllDependOnBits);
  }

  static SideEffects AllExceptGCDependency() {
    return AllWritesAndReads().Union(SideEffects::CanTriggerGC());
  }

  static SideEffects AllWritesAndReads() {
    return SideEffects(kAllWrites | kAllReads);
  }

  static SideEffects AllWrites() {
    return SideEffects(kAllWrites);
  }

  static SideEffects AllReads() {
    return SideEffects(kAllReads);
  }

  static SideEffects FieldWriteOfType(DataType::Type type, bool is_volatile) {
    return is_volatile
        ? AllWritesAndReads()
        : SideEffects(TypeFlag(type, kFieldWriteOffset));
  }

  static SideEffects ArrayWriteOfType(DataType::Type type) {
    return SideEffects(TypeFlag(type, kArrayWriteOffset));
  }

  static SideEffects FieldReadOfType(DataType::Type type, bool is_volatile) {
    return is_volatile
        ? AllWritesAndReads()
        : SideEffects(TypeFlag(type, kFieldReadOffset));
  }

  static SideEffects ArrayReadOfType(DataType::Type type) {
    return SideEffects(TypeFlag(type, kArrayReadOffset));
  }

  // Returns whether GC might happen across this instruction from the compiler perspective so
  // the next instruction in the IR would see that.
  //
  // See the SideEffect class comments.
  static SideEffects CanTriggerGC() {
    return SideEffects(1ULL << kCanTriggerGCBit);
  }

  // Returns whether the instruction must not be alive across a GC point.
  //
  // See the SideEffect class comments.
  static SideEffects DependsOnGC() {
    return SideEffects(1ULL << kDependsOnGCBit);
  }

  // Combines the side-effects of this and the other.
  SideEffects Union(SideEffects other) const {
    return SideEffects(flags_ | other.flags_);
  }

  SideEffects Exclusion(SideEffects other) const {
    return SideEffects(flags_ & ~other.flags_);
  }

  void Add(SideEffects other) {
    flags_ |= other.flags_;
  }

  bool Includes(SideEffects other) const {
    return (other.flags_ & flags_) == other.flags_;
  }

  bool HasSideEffects() const {
    return (flags_ & kAllChangeBits) != 0u;
  }

  bool HasDependencies() const {
    return (flags_ & kAllDependOnBits) != 0u;
  }

  // Returns true if there are no side effects or dependencies.
  bool DoesNothing() const {
    return flags_ == 0u;
  }

  // Returns true if something is written.
  bool DoesAnyWrite() const {
    return (flags_ & kAllWrites) != 0u;
  }

  // Returns true if something is read.
  bool DoesAnyRead() const {
    return (flags_ & kAllReads) != 0u;
  }

  // Returns true if potentially everything is written and read
  // (every type and every kind of access).
  bool DoesAllReadWrite() const {
    return (flags_ & (kAllWrites | kAllReads)) == (kAllWrites | kAllReads);
  }

  bool DoesAll() const {
    return flags_ == (kAllChangeBits | kAllDependOnBits);
  }

  // Returns true if `this` may read something written by `other`.
  bool MayDependOn(SideEffects other) const {
    const uint64_t depends_on_flags = (flags_ & kAllDependOnBits) >> kChangeBits;
    return (other.flags_ & depends_on_flags) != 0u;
  }

  // Returns string representation of flags (for debugging only).
  // Format: |x|DFJISCBZL|DFJISCBZL|y|DFJISCBZL|DFJISCBZL|
  std::string ToString() const {
    std::string flags = "|";
    for (int s = kLastBit; s >= 0; s--) {
      bool current_bit_is_set = ((flags_ >> s) & 1) != 0;
      if ((s == kDependsOnGCBit) || (s == kCanTriggerGCBit)) {
        // This is a bit for the GC side effect.
        if (current_bit_is_set) {
          flags += "GC";
        }
        flags += "|";
      } else {
        // This is a bit for the array/field analysis.
        // The underscore character stands for the 'can trigger GC' bit.
        static const char *kDebug = "LZBCSIJFDLZBCSIJFD_LZBCSIJFDLZBCSIJFD";
        if (current_bit_is_set) {
          flags += kDebug[s];
        }
        if ((s == kFieldWriteOffset) || (s == kArrayWriteOffset) ||
            (s == kFieldReadOffset) || (s == kArrayReadOffset)) {
          flags += "|";
        }
      }
    }
    return flags;
  }

  bool Equals(const SideEffects& other) const { return flags_ == other.flags_; }

 private:
  static constexpr int kFieldArrayAnalysisBits = 9;

  static constexpr int kFieldWriteOffset = 0;
  static constexpr int kArrayWriteOffset = kFieldWriteOffset + kFieldArrayAnalysisBits;
  static constexpr int kLastBitForWrites = kArrayWriteOffset + kFieldArrayAnalysisBits - 1;
  static constexpr int kCanTriggerGCBit = kLastBitForWrites + 1;

  static constexpr int kChangeBits = kCanTriggerGCBit + 1;

  static constexpr int kFieldReadOffset = kCanTriggerGCBit + 1;
  static constexpr int kArrayReadOffset = kFieldReadOffset + kFieldArrayAnalysisBits;
  static constexpr int kLastBitForReads = kArrayReadOffset + kFieldArrayAnalysisBits - 1;
  static constexpr int kDependsOnGCBit = kLastBitForReads + 1;

  static constexpr int kLastBit = kDependsOnGCBit;
  static constexpr int kDependOnBits = kLastBit + 1 - kChangeBits;

  // Aliases.

  static_assert(kChangeBits == kDependOnBits,
                "the 'change' bits should match the 'depend on' bits.");

  static constexpr uint64_t kAllChangeBits = ((1ULL << kChangeBits) - 1);
  static constexpr uint64_t kAllDependOnBits = ((1ULL << kDependOnBits) - 1) << kChangeBits;
  static constexpr uint64_t kAllWrites =
      ((1ULL << (kLastBitForWrites + 1 - kFieldWriteOffset)) - 1) << kFieldWriteOffset;
  static constexpr uint64_t kAllReads =
      ((1ULL << (kLastBitForReads + 1 - kFieldReadOffset)) - 1) << kFieldReadOffset;

  // Translates type to bit flag. The type must correspond to a Java type.
  static uint64_t TypeFlag(DataType::Type type, int offset) {
    int shift;
    switch (type) {
      case DataType::Type::kReference: shift = 0; break;
      case DataType::Type::kBool:      shift = 1; break;
      case DataType::Type::kInt8:      shift = 2; break;
      case DataType::Type::kUint16:    shift = 3; break;
      case DataType::Type::kInt16:     shift = 4; break;
      case DataType::Type::kInt32:     shift = 5; break;
      case DataType::Type::kInt64:     shift = 6; break;
      case DataType::Type::kFloat32:   shift = 7; break;
      case DataType::Type::kFloat64:   shift = 8; break;
      default:
        LOG(FATAL) << "Unexpected data type " << type;
        UNREACHABLE();
    }
    DCHECK_LE(kFieldWriteOffset, shift);
    DCHECK_LT(shift, kArrayWriteOffset);
    return UINT64_C(1) << (shift + offset);
  }

  // Private constructor on direct flags value.
  explicit SideEffects(uint64_t flags) : flags_(flags) {}

  uint64_t flags_;
};

// A HEnvironment object contains the values of virtual registers at a given location.
class HEnvironment : public ArenaObject<kArenaAllocEnvironment> {
 public:
  static HEnvironment* Create(ArenaAllocator* allocator,
                              size_t number_of_vregs,
                              ArtMethod* method,
                              uint32_t dex_pc,
                              HInstruction* holder) {
    // The storage for vreg records is allocated right after the `HEnvironment` itself.
    static_assert(IsAligned<alignof(HUserRecord<HEnvironment*>)>(sizeof(HEnvironment)));
    static_assert(IsAligned<alignof(HUserRecord<HEnvironment*>)>(ArenaAllocator::kAlignment));
    size_t alloc_size = sizeof(HEnvironment) + number_of_vregs * sizeof(HUserRecord<HEnvironment*>);
    void* storage = allocator->Alloc(alloc_size, kArenaAllocEnvironment);
    return new (storage) HEnvironment(number_of_vregs, method, dex_pc, holder);
  }

  static HEnvironment* Create(ArenaAllocator* allocator,
                              const HEnvironment& to_copy,
                              HInstruction* holder) {
    return Create(allocator, to_copy.Size(), to_copy.GetMethod(), to_copy.GetDexPc(), holder);
  }

  void AllocateLocations(ArenaAllocator* allocator) {
    DCHECK(locations_ == nullptr);
    if (Size() != 0u) {
      locations_ = allocator->AllocArray<Location>(Size(), kArenaAllocEnvironmentLocations);
    }
  }

  void SetAndCopyParentChain(ArenaAllocator* allocator, HEnvironment* parent) {
    if (parent_ != nullptr) {
      parent_->SetAndCopyParentChain(allocator, parent);
    } else {
      parent_ = Create(allocator, *parent, holder_);
      parent_->CopyFrom(allocator, parent);
      if (parent->GetParent() != nullptr) {
        parent_->SetAndCopyParentChain(allocator, parent->GetParent());
      }
    }
  }

  void CopyFrom(ArenaAllocator* allocator, ArrayRef<HInstruction* const> locals);
  void CopyFrom(ArenaAllocator* allocator, const HEnvironment* environment);

  // Copy from `env`. If it's a loop phi for `loop_header`, copy the first
  // input to the loop phi instead. This is for inserting instructions that
  // require an environment (like HDeoptimization) in the loop pre-header.
  void CopyFromWithLoopPhiAdjustment(ArenaAllocator* allocator,
                                     HEnvironment* env,
                                     HBasicBlock* loop_header);

  void SetRawEnvAt(size_t index, HInstruction* instruction) {
    GetVRegs()[index] = HUserRecord<HEnvironment*>(instruction);
  }

  HInstruction* GetInstructionAt(size_t index) const {
    return GetVRegs()[index].GetInstruction();
  }

  void RemoveAsUserOfInput(size_t index) const;

  // Replaces the input at the position 'index' with the replacement; the replacement and old
  // input instructions' env_uses_ lists are adjusted. The function works similar to
  // HInstruction::ReplaceInput.
  void ReplaceInput(HInstruction* replacement, size_t index);

  size_t Size() const { return number_of_vregs_; }

  HEnvironment* GetParent() const { return parent_; }

  void SetLocationAt(size_t index, Location location) {
    DCHECK_LT(index, number_of_vregs_);
    DCHECK(locations_ != nullptr);
    locations_[index] = location;
  }

  Location GetLocationAt(size_t index) const {
    DCHECK_LT(index, number_of_vregs_);
    DCHECK(locations_ != nullptr);
    return locations_[index];
  }

  uint32_t GetDexPc() const {
    return dex_pc_;
  }

  ArtMethod* GetMethod() const {
    return method_;
  }

  HInstruction* GetHolder() const {
    return holder_;
  }


  bool IsFromInlinedInvoke() const {
    return GetParent() != nullptr;
  }

  class EnvInputSelector {
   public:
    explicit EnvInputSelector(const HEnvironment* e) : env_(e) {}
    HInstruction* operator()(size_t s) const {
      return env_->GetInstructionAt(s);
    }
   private:
    const HEnvironment* env_;
  };

  using HConstEnvInputRef = TransformIterator<CountIter, EnvInputSelector>;
  IterationRange<HConstEnvInputRef> GetEnvInputs() const {
    IterationRange<CountIter> range(Range(Size()));
    return MakeIterationRange(MakeTransformIterator(range.begin(), EnvInputSelector(this)),
                              MakeTransformIterator(range.end(), EnvInputSelector(this)));
  }

 private:
  ALWAYS_INLINE HEnvironment(size_t number_of_vregs,
                             ArtMethod* method,
                             uint32_t dex_pc,
                             HInstruction* holder)
      : number_of_vregs_(dchecked_integral_cast<uint32_t>(number_of_vregs)),
        dex_pc_(dex_pc),
        holder_(holder),
        parent_(nullptr),
        method_(method),
        locations_(nullptr) {
  }

  ArrayRef<HUserRecord<HEnvironment*>> GetVRegs() {
    auto* vregs = reinterpret_cast<HUserRecord<HEnvironment*>*>(this + 1);
    return ArrayRef<HUserRecord<HEnvironment*>>(vregs, number_of_vregs_);
  }

  ArrayRef<const HUserRecord<HEnvironment*>> GetVRegs() const {
    auto* vregs = reinterpret_cast<const HUserRecord<HEnvironment*>*>(this + 1);
    return ArrayRef<const HUserRecord<HEnvironment*>>(vregs, number_of_vregs_);
  }

  const uint32_t number_of_vregs_;
  const uint32_t dex_pc_;

  // The instruction that holds this environment.
  HInstruction* const holder_;

  // The parent environment for inlined code.
  HEnvironment* parent_;

  // The environment's method, if resolved.
  ArtMethod* method_;

  // Locations assigned by the register allocator.
  Location* locations_;

  friend class HInstruction;

  DISALLOW_COPY_AND_ASSIGN(HEnvironment);
};

std::ostream& operator<<(std::ostream& os, const HInstruction& rhs);

// Iterates over the Environments
class HEnvironmentIterator : public ValueObject {
 public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = HEnvironment*;
  using difference_type = ptrdiff_t;
  using pointer = void;
  using reference = void;

  explicit HEnvironmentIterator(HEnvironment* cur) : cur_(cur) {}

  HEnvironment* operator*() const {
    return cur_;
  }

  HEnvironmentIterator& operator++() {
    DCHECK(cur_ != nullptr);
    cur_ = cur_->GetParent();
    return *this;
  }

  HEnvironmentIterator operator++(int) {
    HEnvironmentIterator prev(*this);
    ++(*this);
    return prev;
  }

  bool operator==(const HEnvironmentIterator& other) const {
    return other.cur_ == cur_;
  }

  bool operator!=(const HEnvironmentIterator& other) const {
    return !(*this == other);
  }

 private:
  HEnvironment* cur_;
};

class HInstruction : public ArenaObject<kArenaAllocInstruction> {
 public:
#define DECLARE_KIND(type, super) k##type,
  enum InstructionKind {  // private marker to avoid generate-operator-out.py from processing.
    FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_KIND)
    kLastInstructionKind
  };
#undef DECLARE_KIND

  HInstruction(InstructionKind kind, SideEffects side_effects, uint32_t dex_pc)
      : HInstruction(kind, DataType::Type::kVoid, side_effects, dex_pc) {}

  HInstruction(InstructionKind kind, DataType::Type type, SideEffects side_effects, uint32_t dex_pc)
      : previous_(nullptr),
        next_(nullptr),
        block_(nullptr),
        dex_pc_(dex_pc),
        id_(-1),
        ssa_index_(-1),
        packed_fields_(0u),
        environment_(nullptr),
        locations_(nullptr),
        live_interval_(nullptr),
        lifetime_position_(kNoLifetime),
        side_effects_(side_effects),
        reference_type_handle_(ReferenceTypeInfo::CreateInvalid().GetTypeHandle()) {
    SetPackedField<InstructionKindField>(kind);
    SetPackedField<TypeField>(type);
    SetPackedFlag<kFlagReferenceTypeIsExact>(ReferenceTypeInfo::CreateInvalid().IsExact());
  }

  virtual ~HInstruction() {}

  std::ostream& Dump(std::ostream& os, bool dump_args = false);

  // Helper for dumping without argument information using operator<<
  struct NoArgsDump {
    const HInstruction* ins;
  };
  NoArgsDump DumpWithoutArgs() const {
    return NoArgsDump{this};
  }
  // Helper for dumping with argument information using operator<<
  struct ArgsDump {
    const HInstruction* ins;
  };
  ArgsDump DumpWithArgs() const {
    return ArgsDump{this};
  }

  HInstruction* GetNext() const { return next_; }
  HInstruction* GetPrevious() const { return previous_; }

  HInstruction* GetNextDisregardingMoves() const;
  HInstruction* GetPreviousDisregardingMoves() const;

  HBasicBlock* GetBlock() const { return block_; }
  void SetBlock(HBasicBlock* block) { block_ = block; }
  bool IsInBlock() const { return block_ != nullptr; }
  bool IsInLoop() const { return block_->IsInLoop(); }
  bool IsLoopHeaderPhi() const { return IsPhi() && block_->IsLoopHeader(); }
  bool IsIrreducibleLoopHeaderPhi() const {
    return IsLoopHeaderPhi() && GetBlock()->GetLoopInformation()->IsIrreducible();
  }

  virtual ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() = 0;

  ArrayRef<const HUserRecord<HInstruction*>> GetInputRecords() const {
    // One virtual method is enough, just const_cast<> and then re-add the const.
    return ArrayRef<const HUserRecord<HInstruction*>>(
        const_cast<HInstruction*>(this)->GetInputRecords());
  }

  HInputsRef GetInputs() {
    return MakeTransformArrayRef(GetInputRecords(), HInputExtractor());
  }

  HConstInputsRef GetInputs() const {
    return MakeTransformArrayRef(GetInputRecords(), HInputExtractor());
  }

  size_t InputCount() const { return GetInputRecords().size(); }
  HInstruction* InputAt(size_t i) const { return InputRecordAt(i).GetInstruction(); }

  bool HasInput(HInstruction* input) const {
    for (const HInstruction* i : GetInputs()) {
      if (i == input) {
        return true;
      }
    }
    return false;
  }

  void SetRawInputAt(size_t index, HInstruction* input) {
    SetRawInputRecordAt(index, HUserRecord<HInstruction*>(input));
  }

  virtual void Accept(HGraphVisitor* visitor) = 0;
  virtual const char* DebugName() const = 0;

  DataType::Type GetType() const {
    return TypeField::Decode(GetPackedFields());
  }

  virtual bool NeedsEnvironment() const { return false; }
  virtual bool NeedsBss() const {
    return false;
  }

  uint32_t GetDexPc() const { return dex_pc_; }

  virtual bool IsControlFlow() const { return false; }

  // Can the instruction throw?
  // TODO: We should rename to CanVisiblyThrow, as some instructions (like HNewInstance),
  // could throw OOME, but it is still OK to remove them if they are unused.
  virtual bool CanThrow() const { return false; }

  // Does the instruction always throw an exception unconditionally?
  virtual bool AlwaysThrows() const { return false; }
  // Will this instruction only cause async exceptions if it causes any at all?
  virtual bool OnlyThrowsAsyncExceptions() const {
    return false;
  }

  bool CanThrowIntoCatchBlock() const { return CanThrow() && block_->IsTryBlock(); }

  bool HasSideEffects() const { return side_effects_.HasSideEffects(); }
  bool DoesAnyWrite() const { return side_effects_.DoesAnyWrite(); }

  // Does not apply for all instructions, but having this at top level greatly
  // simplifies the null check elimination.
  // TODO: Consider merging can_be_null into ReferenceTypeInfo.
  virtual bool CanBeNull() const {
    DCHECK_EQ(GetType(), DataType::Type::kReference) << "CanBeNull only applies to reference types";
    return true;
  }

  virtual bool CanDoImplicitNullCheckOn([[maybe_unused]] HInstruction* obj) const { return false; }

  // If this instruction will do an implicit null check, return the `HNullCheck` associated
  // with it. Otherwise return null.
  HNullCheck* GetImplicitNullCheck() const {
    // Go over previous non-move instructions that are emitted at use site.
    HInstruction* prev_not_move = GetPreviousDisregardingMoves();
    while (prev_not_move != nullptr && prev_not_move->IsEmittedAtUseSite()) {
      if (prev_not_move->IsNullCheck()) {
        return prev_not_move->AsNullCheck();
      }
      prev_not_move = prev_not_move->GetPreviousDisregardingMoves();
    }
    return nullptr;
  }

  virtual bool IsActualObject() const {
    return GetType() == DataType::Type::kReference;
  }

  // Sets the ReferenceTypeInfo. The RTI must be valid.
  void SetReferenceTypeInfo(ReferenceTypeInfo rti);
  // Same as above, but we only set it if it's valid. Otherwise, we don't change the current RTI.
  void SetReferenceTypeInfoIfValid(ReferenceTypeInfo rti);

  ReferenceTypeInfo GetReferenceTypeInfo() const {
    DCHECK_EQ(GetType(), DataType::Type::kReference);
    return ReferenceTypeInfo::CreateUnchecked(reference_type_handle_,
                                              GetPackedFlag<kFlagReferenceTypeIsExact>());
  }

  void AddUseAt(ArenaAllocator* allocator, HInstruction* user, size_t index) {
    DCHECK(user != nullptr);
    HUseListNode<HInstruction*>* new_node =
        new (allocator) HUseListNode<HInstruction*>(user, index);
    // Note: `old_begin` remains valid across `push_front()`.
    auto old_begin = uses_.begin();
    uses_.push_front(*new_node);
    // To speed up this code, we inline the
    //     FixUpUserRecordsAfterUseInsertion(
    //         old_begin != uses_.end() ? ++old_begin : old_begin);
    // to reduce branching as we know that we're going to fix up either one or two entries.
    auto new_begin = uses_.begin();
    user->SetRawInputRecordAt(index, HUserRecord<HInstruction*>(this, uses_.before_begin()));
    if (old_begin != uses_.end()) {
      HInstruction* old_begin_user = old_begin->GetUser();
      size_t old_begin_index = old_begin->GetIndex();
      old_begin_user->SetRawInputRecordAt(
          old_begin_index, HUserRecord<HInstruction*>(this, new_begin));
    }
  }

  void AddEnvUseAt(ArenaAllocator* allocator, HEnvironment* user, size_t index) {
    DCHECK(user != nullptr);
    HUseListNode<HEnvironment*>* new_node =
        new (allocator) HUseListNode<HEnvironment*>(user, index);
    // Note: `old_env_begin` remains valid across `push_front()`.
    auto old_env_begin = env_uses_.begin();
    env_uses_.push_front(*new_node);
    // To speed up this code, we inline the
    //     FixUpUserRecordsAfterEnvUseInsertion(
    //         old_env_begin != env_uses_.end() ? ++old_env_begin : old_env_begin);
    // to reduce branching as we know that we're going to fix up either one or two entries.
    auto new_env_begin = env_uses_.begin();
    user->GetVRegs()[index] = HUserRecord<HEnvironment*>(this, env_uses_.before_begin());
    if (old_env_begin != env_uses_.end()) {
      HEnvironment* old_env_begin_user = old_env_begin->GetUser();
      size_t old_env_begin_index = old_env_begin->GetIndex();
      old_env_begin_user->GetVRegs()[old_env_begin_index] =
          HUserRecord<HEnvironment*>(this, new_env_begin);
    }
  }

  void RemoveAsUserOfInput(size_t input) {
    HUserRecord<HInstruction*> input_use = InputRecordAt(input);
    HUseList<HInstruction*>::iterator before_use_node = input_use.GetBeforeUseNode();
    input_use.GetInstruction()->uses_.erase_after(before_use_node);
    input_use.GetInstruction()->FixUpUserRecordsAfterUseRemoval(before_use_node);
  }

  void RemoveAsUserOfAllInputs() {
    for (const HUserRecord<HInstruction*>& input_use : GetInputRecords()) {
      HUseList<HInstruction*>::iterator before_use_node = input_use.GetBeforeUseNode();
      input_use.GetInstruction()->uses_.erase_after(before_use_node);
      input_use.GetInstruction()->FixUpUserRecordsAfterUseRemoval(before_use_node);
    }
  }

  const HUseList<HInstruction*>& GetUses() const { return uses_; }
  const HUseList<HEnvironment*>& GetEnvUses() const { return env_uses_; }

  bool HasUses() const { return !uses_.empty() || !env_uses_.empty(); }
  bool HasEnvironmentUses() const { return !env_uses_.empty(); }
  bool HasNonEnvironmentUses() const { return !uses_.empty(); }
  bool HasOnlyOneNonEnvironmentUse() const {
    return !HasEnvironmentUses() && GetUses().HasExactlyOneElement();
  }

  bool IsRemovable() const {
    return
        !DoesAnyWrite() &&
        // TODO(solanes): Merge calls from IsSuspendCheck to IsControlFlow into one that doesn't
        // do virtual dispatching.
        !IsSuspendCheck() &&
        !IsNop() &&
        !IsParameterValue() &&
        // If we added an explicit barrier then we should keep it.
        !IsMemoryBarrier() &&
        !IsConstructorFence() &&
        !IsControlFlow() &&
        !CanThrow();
  }

  bool IsDeadAndRemovable() const {
    return !HasUses() && IsRemovable();
  }

  bool IsPhiDeadAndRemovable() const {
    DCHECK(IsPhi());
    DCHECK(IsRemovable()) << " phis are always removable";
    return !HasUses();
  }

  // Does this instruction dominate `other_instruction`?
  // Aborts if this instruction and `other_instruction` are different phis.
  bool Dominates(HInstruction* other_instruction) const;

  // Same but with `strictly dominates` i.e. returns false if this instruction and
  // `other_instruction` are the same.
  bool StrictlyDominates(HInstruction* other_instruction) const;

  int GetId() const { return id_; }
  void SetId(int id) { id_ = id; }

  int GetSsaIndex() const { return ssa_index_; }
  void SetSsaIndex(int ssa_index) { ssa_index_ = ssa_index; }
  bool HasSsaIndex() const { return ssa_index_ != -1; }

  bool HasEnvironment() const { return environment_ != nullptr; }
  HEnvironment* GetEnvironment() const { return environment_; }
  IterationRange<HEnvironmentIterator> GetAllEnvironments() const {
    return MakeIterationRange(HEnvironmentIterator(GetEnvironment()),
                              HEnvironmentIterator(nullptr));
  }
  // Set the `environment_` field. Raw because this method does not
  // update the uses lists.
  void SetRawEnvironment(HEnvironment* environment) {
    DCHECK(environment_ == nullptr);
    DCHECK_EQ(environment->GetHolder(), this);
    environment_ = environment;
  }

  void InsertRawEnvironment(HEnvironment* environment) {
    DCHECK(environment_ != nullptr);
    DCHECK_EQ(environment->GetHolder(), this);
    DCHECK(environment->GetParent() == nullptr);
    environment->parent_ = environment_;
    environment_ = environment;
  }

  void RemoveEnvironment();

  // Set the environment of this instruction, copying it from `environment`. While
  // copying, the uses lists are being updated.
  void CopyEnvironmentFrom(HEnvironment* environment) {
    DCHECK(environment_ == nullptr);
    ArenaAllocator* allocator = GetBlock()->GetGraph()->GetAllocator();
    environment_ = HEnvironment::Create(allocator, *environment, this);
    environment_->CopyFrom(allocator, environment);
    if (environment->GetParent() != nullptr) {
      environment_->SetAndCopyParentChain(allocator, environment->GetParent());
    }
  }

  void CopyEnvironmentFromWithLoopPhiAdjustment(HEnvironment* environment,
                                                HBasicBlock* loop_header) {
    DCHECK(environment_ == nullptr);
    ArenaAllocator* allocator = loop_header->GetGraph()->GetAllocator();
    environment_ = HEnvironment::Create(allocator, *environment, this);
    environment_->CopyFromWithLoopPhiAdjustment(allocator, environment, loop_header);
    if (environment->GetParent() != nullptr) {
      environment_->SetAndCopyParentChain(allocator, environment->GetParent());
    }
  }

  // Returns the number of entries in the environment. Typically, that is the
  // number of dex registers in a method. It could be more in case of inlining.
  size_t EnvironmentSize() const;

  LocationSummary* GetLocations() const { return locations_; }
  void SetLocations(LocationSummary* locations) { locations_ = locations; }

  void ReplaceWith(HInstruction* instruction);
  void ReplaceUsesDominatedBy(HInstruction* dominator,
                              HInstruction* replacement,
                              bool strictly_dominated = true);
  void ReplaceEnvUsesDominatedBy(HInstruction* dominator, HInstruction* replacement);
  void ReplaceInput(HInstruction* replacement, size_t index);

  // This is almost the same as doing `ReplaceWith()`. But in this helper, the
  // uses of this instruction by `other` are *not* updated.
  void ReplaceWithExceptInReplacementAtIndex(HInstruction* other, size_t use_index) {
    ReplaceWith(other);
    other->ReplaceInput(this, use_index);
  }

  // Move `this` instruction before `cursor`
  void MoveBefore(HInstruction* cursor, bool do_checks = true);

  // Move `this` before its first user and out of any loops. If there is no
  // out-of-loop user that dominates all other users, move the instruction
  // to the end of the out-of-loop common dominator of the user's blocks.
  //
  // This can be used only on non-throwing instructions with no side effects that
  // have at least one use but no environment uses.
  void MoveBeforeFirstUserAndOutOfLoops();

#define INSTRUCTION_TYPE_CHECK(type, super)                                    \
  bool Is##type() const;

  FOR_EACH_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

#define INSTRUCTION_TYPE_CAST(type, super)                                     \
  const H##type* As##type() const;                                             \
  H##type* As##type();                                                         \
  const H##type* As##type##OrNull() const;                                     \
  H##type* As##type##OrNull();

  FOR_EACH_INSTRUCTION(INSTRUCTION_TYPE_CAST)
#undef INSTRUCTION_TYPE_CAST

  // Return a clone of the instruction if it is clonable (shallow copy by default, custom copy
  // if a custom copy-constructor is provided for a particular type). If IsClonable() is false for
  // the instruction then the behaviour of this function is undefined.
  //
  // Note: It is semantically valid to create a clone of the instruction only until
  // prepare_for_register_allocator phase as lifetime, intervals and codegen info are not
  // copied.
  //
  // Note: HEnvironment and some other fields are not copied and are set to default values, see
  // 'explicit HInstruction(const HInstruction& other)' for details.
  virtual HInstruction* Clone([[maybe_unused]] ArenaAllocator* arena) const {
    LOG(FATAL) << "Cloning is not implemented for the instruction " <<
                  DebugName() << " " << GetId();
    UNREACHABLE();
  }

  // Return whether instruction can be cloned (copied).
  virtual bool IsClonable() const { return false; }

  // Returns whether the instruction can be moved within the graph.
  // TODO: this method is used by LICM and GVN with possibly different
  //       meanings? split and rename?
  virtual bool CanBeMoved() const { return false; }

  // Returns whether any data encoded in the two instructions is equal.
  // This method does not look at the inputs. Both instructions must be
  // of the same type, otherwise the method has undefined behavior.
  virtual bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const {
    return false;
  }

  // Returns whether two instructions are equal, that is:
  // 1) They have the same type and contain the same data (InstructionDataEquals).
  // 2) Their inputs are identical.
  bool Equals(const HInstruction* other) const;

  InstructionKind GetKind() const { return GetPackedField<InstructionKindField>(); }

  virtual size_t ComputeHashCode() const {
    size_t result = GetKind();
    for (const HInstruction* input : GetInputs()) {
      result = (result * 31) + input->GetId();
    }
    return result;
  }

  SideEffects GetSideEffects() const { return side_effects_; }
  void SetSideEffects(SideEffects other) { side_effects_ = other; }
  void AddSideEffects(SideEffects other) { side_effects_.Add(other); }

  size_t GetLifetimePosition() const { return lifetime_position_; }
  void SetLifetimePosition(size_t position) { lifetime_position_ = position; }
  LiveInterval* GetLiveInterval() const { return live_interval_; }
  void SetLiveInterval(LiveInterval* interval) { live_interval_ = interval; }
  bool HasLiveInterval() const { return live_interval_ != nullptr; }

  bool IsSuspendCheckEntry() const { return IsSuspendCheck() && GetBlock()->IsEntryBlock(); }

  // Returns whether the code generation of the instruction will require to have access
  // to the current method. Such instructions are:
  // (1): Instructions that require an environment, as calling the runtime requires
  //      to walk the stack and have the current method stored at a specific stack address.
  // (2): HCurrentMethod, potentially used by HInvokeStaticOrDirect, HLoadString, or HLoadClass
  //      to access the dex cache.
  bool NeedsCurrentMethod() const {
    return NeedsEnvironment() || IsCurrentMethod();
  }

  // Does this instruction have any use in an environment before
  // control flow hits 'other'?
  bool HasAnyEnvironmentUseBefore(HInstruction* other);

  // Remove all references to environment uses of this instruction.
  // The caller must ensure that this is safe to do.
  void RemoveEnvironmentUsers();

  bool IsEmittedAtUseSite() const { return GetPackedFlag<kFlagEmittedAtUseSite>(); }
  void MarkEmittedAtUseSite() { SetPackedFlag<kFlagEmittedAtUseSite>(true); }

 protected:
  // If set, the machine code for this instruction is assumed to be generated by
  // its users. Used by liveness analysis to compute use positions accordingly.
  static constexpr size_t kFlagEmittedAtUseSite = 0u;
  static constexpr size_t kFlagReferenceTypeIsExact = kFlagEmittedAtUseSite + 1;
  static constexpr size_t kFieldInstructionKind = kFlagReferenceTypeIsExact + 1;
  static constexpr size_t kFieldInstructionKindSize =
      MinimumBitsToStore(static_cast<size_t>(InstructionKind::kLastInstructionKind - 1));
  static constexpr size_t kFieldType =
      kFieldInstructionKind + kFieldInstructionKindSize;
  static constexpr size_t kFieldTypeSize =
      MinimumBitsToStore(static_cast<size_t>(DataType::Type::kLast));
  static constexpr size_t kNumberOfGenericPackedBits = kFieldType + kFieldTypeSize;
  static constexpr size_t kMaxNumberOfPackedBits = sizeof(uint32_t) * kBitsPerByte;

  static_assert(kNumberOfGenericPackedBits <= kMaxNumberOfPackedBits,
                "Too many generic packed fields");

  using TypeField = BitField<DataType::Type, kFieldType, kFieldTypeSize>;

  const HUserRecord<HInstruction*> InputRecordAt(size_t i) const {
    return GetInputRecords()[i];
  }

  void SetRawInputRecordAt(size_t index, const HUserRecord<HInstruction*>& input) {
    ArrayRef<HUserRecord<HInstruction*>> input_records = GetInputRecords();
    input_records[index] = input;
  }

  uint32_t GetPackedFields() const {
    return packed_fields_;
  }

  template <size_t flag>
  bool GetPackedFlag() const {
    return (packed_fields_ & (1u << flag)) != 0u;
  }

  template <size_t flag>
  void SetPackedFlag(bool value = true) {
    packed_fields_ = (packed_fields_ & ~(1u << flag)) | ((value ? 1u : 0u) << flag);
  }

  template <typename BitFieldType>
  typename BitFieldType::value_type GetPackedField() const {
    return BitFieldType::Decode(packed_fields_);
  }

  template <typename BitFieldType>
  void SetPackedField(typename BitFieldType::value_type value) {
    DCHECK(IsUint<BitFieldType::size>(static_cast<uintptr_t>(value)));
    packed_fields_ = BitFieldType::Update(value, packed_fields_);
  }

  // Copy construction for the instruction (used for Clone function).
  //
  // Fields (e.g. lifetime, intervals and codegen info) associated with phases starting from
  // prepare_for_register_allocator are not copied (set to default values).
  //
  // Copy constructors must be provided for every HInstruction type; default copy constructor is
  // fine for most of them. However for some of the instructions a custom copy constructor must be
  // specified (when instruction has non-trivially copyable fields and must have a special behaviour
  // for copying them).
  explicit HInstruction(const HInstruction& other)
      : previous_(nullptr),
        next_(nullptr),
        block_(nullptr),
        dex_pc_(other.dex_pc_),
        id_(-1),
        ssa_index_(-1),
        packed_fields_(other.packed_fields_),
        environment_(nullptr),
        locations_(nullptr),
        live_interval_(nullptr),
        lifetime_position_(kNoLifetime),
        side_effects_(other.side_effects_),
        reference_type_handle_(other.reference_type_handle_) {
  }

 private:
  using InstructionKindField =
      BitField<InstructionKind, kFieldInstructionKind, kFieldInstructionKindSize>;

  void FixUpUserRecordsAfterUseInsertion(HUseList<HInstruction*>::iterator fixup_end) {
    auto before_use_node = uses_.before_begin();
    for (auto use_node = uses_.begin(); use_node != fixup_end; ++use_node) {
      HInstruction* user = use_node->GetUser();
      size_t input_index = use_node->GetIndex();
      user->SetRawInputRecordAt(input_index, HUserRecord<HInstruction*>(this, before_use_node));
      before_use_node = use_node;
    }
  }

  void FixUpUserRecordsAfterUseRemoval(HUseList<HInstruction*>::iterator before_use_node) {
    auto next = ++HUseList<HInstruction*>::iterator(before_use_node);
    if (next != uses_.end()) {
      HInstruction* next_user = next->GetUser();
      size_t next_index = next->GetIndex();
      DCHECK(next_user->InputRecordAt(next_index).GetInstruction() == this);
      next_user->SetRawInputRecordAt(next_index, HUserRecord<HInstruction*>(this, before_use_node));
    }
  }

  void FixUpUserRecordsAfterEnvUseInsertion(HUseList<HEnvironment*>::iterator env_fixup_end) {
    auto before_env_use_node = env_uses_.before_begin();
    for (auto env_use_node = env_uses_.begin(); env_use_node != env_fixup_end; ++env_use_node) {
      HEnvironment* user = env_use_node->GetUser();
      size_t input_index = env_use_node->GetIndex();
      user->GetVRegs()[input_index] = HUserRecord<HEnvironment*>(this, before_env_use_node);
      before_env_use_node = env_use_node;
    }
  }

  void FixUpUserRecordsAfterEnvUseRemoval(HUseList<HEnvironment*>::iterator before_env_use_node) {
    auto next = ++HUseList<HEnvironment*>::iterator(before_env_use_node);
    if (next != env_uses_.end()) {
      HEnvironment* next_user = next->GetUser();
      size_t next_index = next->GetIndex();
      DCHECK(next_user->GetVRegs()[next_index].GetInstruction() == this);
      next_user->GetVRegs()[next_index] = HUserRecord<HEnvironment*>(this, before_env_use_node);
    }
  }

  HInstruction* previous_;
  HInstruction* next_;
  HBasicBlock* block_;
  const uint32_t dex_pc_;

  // An instruction gets an id when it is added to the graph.
  // It reflects creation order. A negative id means the instruction
  // has not been added to the graph.
  int id_;

  // When doing liveness analysis, instructions that have uses get an SSA index.
  int ssa_index_;

  // Packed fields.
  uint32_t packed_fields_;

  // List of instructions that have this instruction as input.
  HUseList<HInstruction*> uses_;

  // List of environments that contain this instruction.
  HUseList<HEnvironment*> env_uses_;

  // The environment associated with this instruction. Not null if the instruction
  // might jump out of the method.
  HEnvironment* environment_;

  // Set by the code generator.
  LocationSummary* locations_;

  // Set by the liveness analysis.
  LiveInterval* live_interval_;

  // Set by the liveness analysis, this is the position in a linear
  // order of blocks where this instruction's live interval start.
  size_t lifetime_position_;

  SideEffects side_effects_;

  // The reference handle part of the reference type info.
  // The IsExact() flag is stored in packed fields.
  // TODO: for primitive types this should be marked as invalid.
  ReferenceTypeInfo::TypeHandle reference_type_handle_;

  friend class GraphChecker;
  friend class HBasicBlock;
  friend class HEnvironment;
  friend class HGraph;
  friend class HInstructionList;
};

std::ostream& operator<<(std::ostream& os, HInstruction::InstructionKind rhs);
std::ostream& operator<<(std::ostream& os, const HInstruction::NoArgsDump rhs);
std::ostream& operator<<(std::ostream& os, const HInstruction::ArgsDump rhs);
std::ostream& operator<<(std::ostream& os, const HUseList<HInstruction*>& lst);
std::ostream& operator<<(std::ostream& os, const HUseList<HEnvironment*>& lst);

// Forward declarations for friends
template <typename InnerIter> struct HSTLInstructionIterator;

// Iterates over the instructions, while preserving the next instruction
// in case the current instruction gets removed from the list by the user
// of this iterator.
class HInstructionIterator : public ValueObject {
 public:
  explicit HInstructionIterator(const HInstructionList& instructions)
      : instruction_(instructions.first_instruction_) {
    next_ = Done() ? nullptr : instruction_->GetNext();
  }

  bool Done() const { return instruction_ == nullptr; }
  HInstruction* Current() const { return instruction_; }
  void Advance() {
    instruction_ = next_;
    next_ = Done() ? nullptr : instruction_->GetNext();
  }

 private:
  HInstructionIterator() : instruction_(nullptr), next_(nullptr) {}

  HInstruction* instruction_;
  HInstruction* next_;

  friend struct HSTLInstructionIterator<HInstructionIterator>;
};

// Iterates over the instructions without saving the next instruction,
// therefore handling changes in the graph potentially made by the user
// of this iterator.
class HInstructionIteratorHandleChanges : public ValueObject {
 public:
  explicit HInstructionIteratorHandleChanges(const HInstructionList& instructions)
      : instruction_(instructions.first_instruction_) {
  }

  bool Done() const { return instruction_ == nullptr; }
  HInstruction* Current() const { return instruction_; }
  void Advance() {
    instruction_ = instruction_->GetNext();
  }

 private:
  HInstructionIteratorHandleChanges() : instruction_(nullptr) {}

  HInstruction* instruction_;

  friend struct HSTLInstructionIterator<HInstructionIteratorHandleChanges>;
};


class HBackwardInstructionIterator : public ValueObject {
 public:
  explicit HBackwardInstructionIterator(const HInstructionList& instructions)
      : instruction_(instructions.last_instruction_) {
    next_ = Done() ? nullptr : instruction_->GetPrevious();
  }

  explicit HBackwardInstructionIterator(HInstruction* instruction) : instruction_(instruction) {
    next_ = Done() ? nullptr : instruction_->GetPrevious();
  }

  bool Done() const { return instruction_ == nullptr; }
  HInstruction* Current() const { return instruction_; }
  void Advance() {
    instruction_ = next_;
    next_ = Done() ? nullptr : instruction_->GetPrevious();
  }

 private:
  HBackwardInstructionIterator() : instruction_(nullptr), next_(nullptr) {}

  HInstruction* instruction_;
  HInstruction* next_;

  friend struct HSTLInstructionIterator<HBackwardInstructionIterator>;
};

template <typename InnerIter>
struct HSTLInstructionIterator : public ValueObject {
 public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = HInstruction*;
  using difference_type = ptrdiff_t;
  using pointer = void;
  using reference = void;

  static_assert(std::is_same_v<InnerIter, HBackwardInstructionIterator> ||
                    std::is_same_v<InnerIter, HInstructionIterator> ||
                    std::is_same_v<InnerIter, HInstructionIteratorHandleChanges>,
                "Unknown wrapped iterator!");

  explicit HSTLInstructionIterator(InnerIter inner) : inner_(inner) {}
  HInstruction* operator*() const {
    DCHECK(inner_.Current() != nullptr);
    return inner_.Current();
  }

  HSTLInstructionIterator<InnerIter>& operator++() {
    DCHECK(*this != HSTLInstructionIterator<InnerIter>::EndIter());
    inner_.Advance();
    return *this;
  }

  HSTLInstructionIterator<InnerIter> operator++(int) {
    HSTLInstructionIterator<InnerIter> prev(*this);
    ++(*this);
    return prev;
  }

  bool operator==(const HSTLInstructionIterator<InnerIter>& other) const {
    return inner_.Current() == other.inner_.Current();
  }

  bool operator!=(const HSTLInstructionIterator<InnerIter>& other) const {
    return !(*this == other);
  }

  static HSTLInstructionIterator<InnerIter> EndIter() {
    return HSTLInstructionIterator<InnerIter>(InnerIter());
  }

 private:
  InnerIter inner_;
};

template <typename InnerIter>
IterationRange<HSTLInstructionIterator<InnerIter>> MakeSTLInstructionIteratorRange(InnerIter iter) {
  return MakeIterationRange(HSTLInstructionIterator<InnerIter>(iter),
                            HSTLInstructionIterator<InnerIter>::EndIter());
}

class HVariableInputSizeInstruction : public HInstruction {
 public:
  using HInstruction::GetInputRecords;  // Keep the const version visible.
  ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() override {
    return ArrayRef<HUserRecord<HInstruction*>>(inputs_);
  }

  void AddInput(HInstruction* input);
  void InsertInputAt(size_t index, HInstruction* input);
  void RemoveInputAt(size_t index);

  // Removes all the inputs.
  // Also removes this instructions from each input's use list
  // (for non-environment uses only).
  void RemoveAllInputs();

 protected:
  HVariableInputSizeInstruction(InstructionKind inst_kind,
                                SideEffects side_effects,
                                uint32_t dex_pc,
                                ArenaAllocator* allocator,
                                size_t number_of_inputs,
                                ArenaAllocKind kind)
      : HInstruction(inst_kind, side_effects, dex_pc),
        inputs_(number_of_inputs, allocator->Adapter(kind)) {}
  HVariableInputSizeInstruction(InstructionKind inst_kind,
                                DataType::Type type,
                                SideEffects side_effects,
                                uint32_t dex_pc,
                                ArenaAllocator* allocator,
                                size_t number_of_inputs,
                                ArenaAllocKind kind)
      : HInstruction(inst_kind, type, side_effects, dex_pc),
        inputs_(number_of_inputs, allocator->Adapter(kind)) {}

  DEFAULT_COPY_CONSTRUCTOR(VariableInputSizeInstruction);

  ArenaVector<HUserRecord<HInstruction*>> inputs_;
};

template<size_t N, typename Base = HInstruction>
class HExpression : public Base {
 public:
  template <typename... Args>
  explicit HExpression(Args&&... args)
      : Base(std::forward<Args>(args)...), inputs_() {}

  virtual ~HExpression() {}

  using HInstruction::GetInputRecords;  // Keep the const version visible.
  ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() final {
    return ArrayRef<HUserRecord<HInstruction*>>(inputs_);
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Expression);

 private:
  std::array<HUserRecord<HInstruction*>, N> inputs_;

  friend class SsaBuilder;
};

// HExpression specialization for N=0.
template<typename Base>
class HExpression<0, Base> : public Base {
 public:
  template <typename... Args>
  explicit HExpression(Args&&... args)
      : Base(std::forward<Args>(args)...) {}

  virtual ~HExpression() {}

  using HInstruction::GetInputRecords;  // Keep the const version visible.
  ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() final {
    return ArrayRef<HUserRecord<HInstruction*>>();
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Expression);

 private:
  friend class SsaBuilder;
};

class HMethodEntryHook : public HExpression<0> {
 public:
  explicit HMethodEntryHook(uint32_t dex_pc)
      : HExpression(kMethodEntryHook, SideEffects::All(), dex_pc) {}

  bool NeedsEnvironment() const override {
    return true;
  }

  bool CanThrow() const override { return true; }

  DECLARE_INSTRUCTION(MethodEntryHook);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(MethodEntryHook);
};

class HMethodExitHook : public HExpression<1> {
 public:
  HMethodExitHook(HInstruction* value, uint32_t dex_pc)
      : HExpression(kMethodExitHook, SideEffects::All(), dex_pc) {
    SetRawInputAt(0, value);
  }

  bool NeedsEnvironment() const override {
    return true;
  }

  bool CanThrow() const override { return true; }

  DECLARE_INSTRUCTION(MethodExitHook);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(MethodExitHook);
};

// Represents dex's RETURN_VOID opcode. A HReturnVoid is a control flow
// instruction that branches to the exit block.
class HReturnVoid final : public HExpression<0> {
 public:
  explicit HReturnVoid(uint32_t dex_pc = kNoDexPc)
      : HExpression(kReturnVoid, SideEffects::None(), dex_pc) {
  }

  bool IsControlFlow() const override { return true; }

  DECLARE_INSTRUCTION(ReturnVoid);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ReturnVoid);
};

// Represents dex's RETURN opcodes. A HReturn is a control flow
// instruction that branches to the exit block.
class HReturn final : public HExpression<1> {
 public:
  explicit HReturn(HInstruction* value, uint32_t dex_pc = kNoDexPc)
      : HExpression(kReturn, SideEffects::None(), dex_pc) {
    SetRawInputAt(0, value);
  }

  bool IsControlFlow() const override { return true; }

  DECLARE_INSTRUCTION(Return);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Return);
};

class HPhi final : public HVariableInputSizeInstruction {
 public:
  HPhi(ArenaAllocator* allocator,
       uint32_t reg_number,
       size_t number_of_inputs,
       DataType::Type type,
       uint32_t dex_pc = kNoDexPc)
      : HVariableInputSizeInstruction(
            kPhi,
            ToPhiType(type),
            SideEffects::None(),
            dex_pc,
            allocator,
            number_of_inputs,
            kArenaAllocPhiInputs),
        reg_number_(reg_number) {
    DCHECK_NE(GetType(), DataType::Type::kVoid);
    // Phis are constructed live and marked dead if conflicting or unused.
    // Individual steps of SsaBuilder should assume that if a phi has been
    // marked dead, it can be ignored and will be removed by SsaPhiElimination.
    SetPackedFlag<kFlagIsLive>(true);
    SetPackedFlag<kFlagCanBeNull>(true);
  }

  bool IsClonable() const override { return true; }

  // Returns a type equivalent to the given `type`, but that a `HPhi` can hold.
  static DataType::Type ToPhiType(DataType::Type type) {
    return DataType::Kind(type);
  }

  bool IsCatchPhi() const { return GetBlock()->IsCatchBlock(); }

  void SetType(DataType::Type new_type) {
    // Make sure that only valid type changes occur. The following are allowed:
    //  (1) int  -> float/ref (primitive type propagation),
    //  (2) long -> double (primitive type propagation).
    DCHECK(GetType() == new_type ||
           (GetType() == DataType::Type::kInt32 && new_type == DataType::Type::kFloat32) ||
           (GetType() == DataType::Type::kInt32 && new_type == DataType::Type::kReference) ||
           (GetType() == DataType::Type::kInt64 && new_type == DataType::Type::kFloat64));
    SetPackedField<TypeField>(new_type);
  }

  bool CanBeNull() const override { return GetPackedFlag<kFlagCanBeNull>(); }
  void SetCanBeNull(bool can_be_null) { SetPackedFlag<kFlagCanBeNull>(can_be_null); }

  uint32_t GetRegNumber() const { return reg_number_; }

  void SetDead() { SetPackedFlag<kFlagIsLive>(false); }
  void SetLive() { SetPackedFlag<kFlagIsLive>(true); }
  bool IsDead() const { return !IsLive(); }
  bool IsLive() const { return GetPackedFlag<kFlagIsLive>(); }

  bool IsVRegEquivalentOf(const HInstruction* other) const {
    return other != nullptr
        && other->IsPhi()
        && other->GetBlock() == GetBlock()
        && other->AsPhi()->GetRegNumber() == GetRegNumber();
  }

  bool HasEquivalentPhi() const {
    if (GetPrevious() != nullptr && GetPrevious()->AsPhi()->GetRegNumber() == GetRegNumber()) {
      return true;
    }
    if (GetNext() != nullptr && GetNext()->AsPhi()->GetRegNumber() == GetRegNumber()) {
      return true;
    }
    return false;
  }

  // Returns the next equivalent phi (starting from the current one) or null if there is none.
  // An equivalent phi is a phi having the same dex register and type.
  // It assumes that phis with the same dex register are adjacent.
  HPhi* GetNextEquivalentPhiWithSameType() {
    HInstruction* next = GetNext();
    while (next != nullptr && next->AsPhi()->GetRegNumber() == reg_number_) {
      if (next->GetType() == GetType()) {
        return next->AsPhi();
      }
      next = next->GetNext();
    }
    return nullptr;
  }

  DECLARE_INSTRUCTION(Phi);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Phi);

 private:
  static constexpr size_t kFlagIsLive = HInstruction::kNumberOfGenericPackedBits;
  static constexpr size_t kFlagCanBeNull = kFlagIsLive + 1;
  static constexpr size_t kNumberOfPhiPackedBits = kFlagCanBeNull + 1;
  static_assert(kNumberOfPhiPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");

  const uint32_t reg_number_;
};

// The exit instruction is the only instruction of the exit block.
// Instructions aborting the method (HThrow and HReturn) must branch to the
// exit block.
class HExit final : public HExpression<0> {
 public:
  explicit HExit(uint32_t dex_pc = kNoDexPc)
      : HExpression(kExit, SideEffects::None(), dex_pc) {
  }

  bool IsControlFlow() const override { return true; }

  DECLARE_INSTRUCTION(Exit);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Exit);
};

// Jumps from one block to another.
class HGoto final : public HExpression<0> {
 public:
  explicit HGoto(uint32_t dex_pc = kNoDexPc)
      : HExpression(kGoto, SideEffects::None(), dex_pc) {
  }

  bool IsClonable() const override { return true; }
  bool IsControlFlow() const override { return true; }

  HBasicBlock* GetSuccessor() const {
    return GetBlock()->GetSingleSuccessor();
  }

  DECLARE_INSTRUCTION(Goto);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Goto);
};

class HConstant : public HExpression<0> {
 public:
  explicit HConstant(InstructionKind kind, DataType::Type type)
      : HExpression(kind, type, SideEffects::None(), kNoDexPc) {
  }

  bool CanBeMoved() const override { return true; }

  // Is this constant -1 in the arithmetic sense?
  virtual bool IsMinusOne() const { return false; }
  // Is this constant 0 in the arithmetic sense?
  virtual bool IsArithmeticZero() const { return false; }
  // Is this constant a 0-bit pattern?
  virtual bool IsZeroBitPattern() const { return false; }
  // Is this constant 1 in the arithmetic sense?
  virtual bool IsOne() const { return false; }

  virtual uint64_t GetValueAsUint64() const = 0;

  DECLARE_ABSTRACT_INSTRUCTION(Constant);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Constant);
};

class HNullConstant final : public HConstant {
 public:
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  uint64_t GetValueAsUint64() const override { return 0; }

  size_t ComputeHashCode() const override { return 0; }

  // The null constant representation is a 0-bit pattern.
  bool IsZeroBitPattern() const override { return true; }

  DECLARE_INSTRUCTION(NullConstant);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(NullConstant);

 private:
  explicit HNullConstant()
      : HConstant(kNullConstant, DataType::Type::kReference) {
  }

  friend class HGraph;
};

// Constants of the type int. Those can be from Dex instructions, or
// synthesized (for example with the if-eqz instruction).
class HIntConstant final : public HConstant {
 public:
  int32_t GetValue() const { return value_; }

  uint64_t GetValueAsUint64() const override {
    return static_cast<uint64_t>(static_cast<uint32_t>(value_));
  }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsIntConstant()) << other->DebugName();
    return other->AsIntConstant()->value_ == value_;
  }

  size_t ComputeHashCode() const override { return GetValue(); }

  bool IsMinusOne() const override { return GetValue() == -1; }
  bool IsArithmeticZero() const override { return GetValue() == 0; }
  bool IsZeroBitPattern() const override { return GetValue() == 0; }
  bool IsOne() const override { return GetValue() == 1; }

  // Integer constants are used to encode Boolean values as well,
  // where 1 means true and 0 means false.
  bool IsTrue() const { return GetValue() == 1; }
  bool IsFalse() const { return GetValue() == 0; }

  explicit HIntConstant(int32_t value)
      : HConstant(kIntConstant, DataType::Type::kInt32), value_(value) {
  }
  explicit HIntConstant(bool value)
      : HConstant(kIntConstant, DataType::Type::kInt32),
        value_(value ? 1 : 0) {
  }

  DECLARE_INSTRUCTION(IntConstant);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(IntConstant);

 private:
  const int32_t value_;

  friend class HGraph;
  ART_FRIEND_TEST(GraphTest, InsertInstructionBefore);
  ART_FRIEND_TYPED_TEST(ParallelMoveTest, ConstantLast);
};

class HLongConstant final : public HConstant {
 public:
  int64_t GetValue() const { return value_; }

  uint64_t GetValueAsUint64() const override { return value_; }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsLongConstant()) << other->DebugName();
    return other->AsLongConstant()->value_ == value_;
  }

  size_t ComputeHashCode() const override { return static_cast<size_t>(GetValue()); }

  bool IsMinusOne() const override { return GetValue() == -1; }
  bool IsArithmeticZero() const override { return GetValue() == 0; }
  bool IsZeroBitPattern() const override { return GetValue() == 0; }
  bool IsOne() const override { return GetValue() == 1; }

  DECLARE_INSTRUCTION(LongConstant);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(LongConstant);

 private:
  explicit HLongConstant(int64_t value)
      : HConstant(kLongConstant, DataType::Type::kInt64),
        value_(value) {
  }

  const int64_t value_;

  friend class HGraph;
};

class HFloatConstant final : public HConstant {
 public:
  float GetValue() const { return value_; }

  uint64_t GetValueAsUint64() const override {
    return static_cast<uint64_t>(bit_cast<uint32_t, float>(value_));
  }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsFloatConstant()) << other->DebugName();
    return other->AsFloatConstant()->GetValueAsUint64() == GetValueAsUint64();
  }

  size_t ComputeHashCode() const override { return static_cast<size_t>(GetValue()); }

  bool IsMinusOne() const override {
    return bit_cast<uint32_t, float>(value_) == bit_cast<uint32_t, float>((-1.0f));
  }
  bool IsArithmeticZero() const override {
    return std::fpclassify(value_) == FP_ZERO;
  }
  bool IsArithmeticPositiveZero() const {
    return IsArithmeticZero() && !std::signbit(value_);
  }
  bool IsArithmeticNegativeZero() const {
    return IsArithmeticZero() && std::signbit(value_);
  }
  bool IsZeroBitPattern() const override {
    return bit_cast<uint32_t, float>(value_) == bit_cast<uint32_t, float>(0.0f);
  }
  bool IsOne() const override {
    return bit_cast<uint32_t, float>(value_) == bit_cast<uint32_t, float>(1.0f);
  }
  bool IsNaN() const {
    return std::isnan(value_);
  }

  DECLARE_INSTRUCTION(FloatConstant);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(FloatConstant);

 private:
  explicit HFloatConstant(float value)
      : HConstant(kFloatConstant, DataType::Type::kFloat32),
        value_(value) {
  }
  explicit HFloatConstant(int32_t value)
      : HConstant(kFloatConstant, DataType::Type::kFloat32),
        value_(bit_cast<float, int32_t>(value)) {
  }

  const float value_;

  // Only the SsaBuilder and HGraph can create floating-point constants.
  friend class SsaBuilder;
  friend class HGraph;
};

class HDoubleConstant final : public HConstant {
 public:
  double GetValue() const { return value_; }

  uint64_t GetValueAsUint64() const override { return bit_cast<uint64_t, double>(value_); }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsDoubleConstant()) << other->DebugName();
    return other->AsDoubleConstant()->GetValueAsUint64() == GetValueAsUint64();
  }

  size_t ComputeHashCode() const override { return static_cast<size_t>(GetValue()); }

  bool IsMinusOne() const override {
    return bit_cast<uint64_t, double>(value_) == bit_cast<uint64_t, double>((-1.0));
  }
  bool IsArithmeticZero() const override {
    return std::fpclassify(value_) == FP_ZERO;
  }
  bool IsArithmeticPositiveZero() const {
    return IsArithmeticZero() && !std::signbit(value_);
  }
  bool IsArithmeticNegativeZero() const {
    return IsArithmeticZero() && std::signbit(value_);
  }
  bool IsZeroBitPattern() const override {
    return bit_cast<uint64_t, double>(value_) == bit_cast<uint64_t, double>((0.0));
  }
  bool IsOne() const override {
    return bit_cast<uint64_t, double>(value_) == bit_cast<uint64_t, double>(1.0);
  }
  bool IsNaN() const {
    return std::isnan(value_);
  }

  DECLARE_INSTRUCTION(DoubleConstant);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(DoubleConstant);

 private:
  explicit HDoubleConstant(double value)
      : HConstant(kDoubleConstant, DataType::Type::kFloat64),
        value_(value) {
  }
  explicit HDoubleConstant(int64_t value)
      : HConstant(kDoubleConstant, DataType::Type::kFloat64),
        value_(bit_cast<double, int64_t>(value)) {
  }

  const double value_;

  // Only the SsaBuilder and HGraph can create floating-point constants.
  friend class SsaBuilder;
  friend class HGraph;
};

// Conditional branch. A block ending with an HIf instruction must have
// two successors.
class HIf final : public HExpression<1> {
 public:
  explicit HIf(HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HExpression(kIf, SideEffects::None(), dex_pc),
        true_count_(std::numeric_limits<uint16_t>::max()),
        false_count_(std::numeric_limits<uint16_t>::max()) {
    SetRawInputAt(0, input);
  }

  bool IsClonable() const override { return true; }
  bool IsControlFlow() const override { return true; }

  HBasicBlock* IfTrueSuccessor() const {
    return GetBlock()->GetSuccessors()[0];
  }

  HBasicBlock* IfFalseSuccessor() const {
    return GetBlock()->GetSuccessors()[1];
  }

  void SetTrueCount(uint16_t count) { true_count_ = count; }
  uint16_t GetTrueCount() const { return true_count_; }

  void SetFalseCount(uint16_t count) { false_count_ = count; }
  uint16_t GetFalseCount() const { return false_count_; }

  DECLARE_INSTRUCTION(If);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(If);

 private:
  uint16_t true_count_;
  uint16_t false_count_;
};


// Abstract instruction which marks the beginning and/or end of a try block and
// links it to the respective exception handlers. Behaves the same as a Goto in
// non-exceptional control flow.
// Normal-flow successor is stored at index zero, exception handlers under
// higher indices in no particular order.
class HTryBoundary final : public HExpression<0> {
 public:
  enum class BoundaryKind {
    kEntry,
    kExit,
    kLast = kExit
  };

  // SideEffects::CanTriggerGC prevents instructions with SideEffects::DependOnGC to be alive
  // across the catch block entering edges as GC might happen during throwing an exception.
  // TryBoundary with BoundaryKind::kExit is conservatively used for that as there is no
  // HInstruction which a catch block must start from.
  explicit HTryBoundary(BoundaryKind kind, uint32_t dex_pc = kNoDexPc)
      : HExpression(kTryBoundary,
                    (kind == BoundaryKind::kExit) ? SideEffects::CanTriggerGC()
                                                  : SideEffects::None(),
                    dex_pc) {
    SetPackedField<BoundaryKindField>(kind);
  }

  bool IsControlFlow() const override { return true; }

  // Returns the block's non-exceptional successor (index zero).
  HBasicBlock* GetNormalFlowSuccessor() const { return GetBlock()->GetSuccessors()[0]; }

  ArrayRef<HBasicBlock* const> GetExceptionHandlers() const {
    return ArrayRef<HBasicBlock* const>(GetBlock()->GetSuccessors()).SubArray(1u);
  }

  // Returns whether `handler` is among its exception handlers (non-zero index
  // successors).
  bool HasExceptionHandler(const HBasicBlock& handler) const {
    DCHECK(handler.IsCatchBlock());
    return GetBlock()->HasSuccessor(&handler, 1u /* Skip first successor. */);
  }

  // If not present already, adds `handler` to its block's list of exception
  // handlers.
  void AddExceptionHandler(HBasicBlock* handler) {
    if (!HasExceptionHandler(*handler)) {
      GetBlock()->AddSuccessor(handler);
    }
  }

  BoundaryKind GetBoundaryKind() const { return GetPackedField<BoundaryKindField>(); }
  bool IsEntry() const { return GetBoundaryKind() == BoundaryKind::kEntry; }

  bool HasSameExceptionHandlersAs(const HTryBoundary& other) const;

  DECLARE_INSTRUCTION(TryBoundary);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(TryBoundary);

 private:
  static constexpr size_t kFieldBoundaryKind = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldBoundaryKindSize =
      MinimumBitsToStore(static_cast<size_t>(BoundaryKind::kLast));
  static constexpr size_t kNumberOfTryBoundaryPackedBits =
      kFieldBoundaryKind + kFieldBoundaryKindSize;
  static_assert(kNumberOfTryBoundaryPackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using BoundaryKindField = BitField<BoundaryKind, kFieldBoundaryKind, kFieldBoundaryKindSize>;
};

// Deoptimize to interpreter, upon checking a condition.
class HDeoptimize final : public HVariableInputSizeInstruction {
 public:
  // Use this constructor when the `HDeoptimize` acts as a barrier, where no code can move
  // across.
  HDeoptimize(ArenaAllocator* allocator,
              HInstruction* cond,
              DeoptimizationKind kind,
              uint32_t dex_pc)
      : HVariableInputSizeInstruction(
            kDeoptimize,
            SideEffects::All(),
            dex_pc,
            allocator,
            /* number_of_inputs= */ 1,
            kArenaAllocMisc) {
    SetPackedFlag<kFieldCanBeMoved>(false);
    SetPackedField<DeoptimizeKindField>(kind);
    SetRawInputAt(0, cond);
  }

  bool IsClonable() const override { return true; }

  // Use this constructor when the `HDeoptimize` guards an instruction, and any user
  // that relies on the deoptimization to pass should have its input be the `HDeoptimize`
  // instead of `guard`.
  // We set CanTriggerGC to prevent any intermediate address to be live
  // at the point of the `HDeoptimize`.
  HDeoptimize(ArenaAllocator* allocator,
              HInstruction* cond,
              HInstruction* guard,
              DeoptimizationKind kind,
              uint32_t dex_pc)
      : HVariableInputSizeInstruction(
            kDeoptimize,
            guard->GetType(),
            SideEffects::CanTriggerGC(),
            dex_pc,
            allocator,
            /* number_of_inputs= */ 2,
            kArenaAllocMisc) {
    SetPackedFlag<kFieldCanBeMoved>(true);
    SetPackedField<DeoptimizeKindField>(kind);
    SetRawInputAt(0, cond);
    SetRawInputAt(1, guard);
  }

  bool CanBeMoved() const override { return GetPackedFlag<kFieldCanBeMoved>(); }

  bool InstructionDataEquals(const HInstruction* other) const override {
    return (other->CanBeMoved() == CanBeMoved()) &&
           (other->AsDeoptimize()->GetDeoptimizationKind() == GetDeoptimizationKind());
  }

  bool NeedsEnvironment() const override { return true; }

  bool CanThrow() const override { return true; }

  DeoptimizationKind GetDeoptimizationKind() const { return GetPackedField<DeoptimizeKindField>(); }

  bool GuardsAnInput() const {
    return InputCount() == 2;
  }

  HInstruction* GuardedInput() const {
    DCHECK(GuardsAnInput());
    return InputAt(1);
  }

  void RemoveGuard() {
    RemoveInputAt(1);
  }

  DECLARE_INSTRUCTION(Deoptimize);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Deoptimize);

 private:
  static constexpr size_t kFieldCanBeMoved = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldDeoptimizeKind = kNumberOfGenericPackedBits + 1;
  static constexpr size_t kFieldDeoptimizeKindSize =
      MinimumBitsToStore(static_cast<size_t>(DeoptimizationKind::kLast));
  static constexpr size_t kNumberOfDeoptimizePackedBits =
      kFieldDeoptimizeKind + kFieldDeoptimizeKindSize;
  static_assert(kNumberOfDeoptimizePackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using DeoptimizeKindField =
      BitField<DeoptimizationKind, kFieldDeoptimizeKind, kFieldDeoptimizeKindSize>;
};

// Represents a should_deoptimize flag. Currently used for CHA-based devirtualization.
// The compiled code checks this flag value in a guard before devirtualized call and
// if it's true, starts to do deoptimization.
// It has a 4-byte slot on stack.
// TODO: allocate a register for this flag.
class HShouldDeoptimizeFlag final : public HVariableInputSizeInstruction {
 public:
  // CHA guards are only optimized in a separate pass and it has no side effects
  // with regard to other passes.
  HShouldDeoptimizeFlag(ArenaAllocator* allocator, uint32_t dex_pc)
      : HVariableInputSizeInstruction(kShouldDeoptimizeFlag,
                                      DataType::Type::kInt32,
                                      SideEffects::None(),
                                      dex_pc,
                                      allocator,
                                      0,
                                      kArenaAllocCHA) {
  }

  // We do all CHA guard elimination/motion in a single pass, after which there is no
  // further guard elimination/motion since a guard might have been used for justification
  // of the elimination of another guard. Therefore, we pretend this guard cannot be moved
  // to avoid other optimizations trying to move it.
  bool CanBeMoved() const override { return false; }

  DECLARE_INSTRUCTION(ShouldDeoptimizeFlag);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ShouldDeoptimizeFlag);
};

// Represents the ArtMethod that was passed as a first argument to
// the method. It is used by instructions that depend on it, like
// instructions that work with the dex cache.
class HCurrentMethod final : public HExpression<0> {
 public:
  explicit HCurrentMethod(DataType::Type type, uint32_t dex_pc = kNoDexPc)
      : HExpression(kCurrentMethod, type, SideEffects::None(), dex_pc) {
  }

  DECLARE_INSTRUCTION(CurrentMethod);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(CurrentMethod);
};

// Fetches an ArtMethod from the virtual table or the interface method table
// of a class.
class HClassTableGet final : public HExpression<1> {
 public:
  enum class TableKind {
    kVTable,
    kIMTable,
    kLast = kIMTable
  };
  HClassTableGet(HInstruction* cls,
                 DataType::Type type,
                 TableKind kind,
                 size_t index,
                 uint32_t dex_pc)
      : HExpression(kClassTableGet, type, SideEffects::None(), dex_pc),
        index_(index) {
    SetPackedField<TableKindField>(kind);
    SetRawInputAt(0, cls);
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals(const HInstruction* other) const override {
    return other->AsClassTableGet()->GetIndex() == index_ &&
        other->AsClassTableGet()->GetPackedFields() == GetPackedFields();
  }

  TableKind GetTableKind() const { return GetPackedField<TableKindField>(); }
  size_t GetIndex() const { return index_; }

  DECLARE_INSTRUCTION(ClassTableGet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ClassTableGet);

 private:
  static constexpr size_t kFieldTableKind = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldTableKindSize =
      MinimumBitsToStore(static_cast<size_t>(TableKind::kLast));
  static constexpr size_t kNumberOfClassTableGetPackedBits = kFieldTableKind + kFieldTableKindSize;
  static_assert(kNumberOfClassTableGetPackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using TableKindField = BitField<TableKind, kFieldTableKind, kFieldTableKindSize>;

  // The index of the ArtMethod in the table.
  const size_t index_;
};

// PackedSwitch (jump table). A block ending with a PackedSwitch instruction will
// have one successor for each entry in the switch table, and the final successor
// will be the block containing the next Dex opcode.
class HPackedSwitch final : public HExpression<1> {
 public:
  HPackedSwitch(int32_t start_value,
                uint32_t num_entries,
                HInstruction* input,
                uint32_t dex_pc = kNoDexPc)
    : HExpression(kPackedSwitch, SideEffects::None(), dex_pc),
      start_value_(start_value),
      num_entries_(num_entries) {
    SetRawInputAt(0, input);
  }

  bool IsClonable() const override { return true; }

  bool IsControlFlow() const override { return true; }

  int32_t GetStartValue() const { return start_value_; }

  uint32_t GetNumEntries() const { return num_entries_; }

  HBasicBlock* GetDefaultBlock() const {
    // Last entry is the default block.
    return GetBlock()->GetSuccessors()[num_entries_];
  }
  DECLARE_INSTRUCTION(PackedSwitch);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(PackedSwitch);

 private:
  const int32_t start_value_;
  const uint32_t num_entries_;
};

class HUnaryOperation : public HExpression<1> {
 public:
  HUnaryOperation(InstructionKind kind,
                  DataType::Type result_type,
                  HInstruction* input,
                  uint32_t dex_pc = kNoDexPc)
      : HExpression(kind, result_type, SideEffects::None(), dex_pc) {
    SetRawInputAt(0, input);
  }

  // All of the UnaryOperation instructions are clonable.
  bool IsClonable() const override { return true; }

  HInstruction* GetInput() const { return InputAt(0); }
  DataType::Type GetResultType() const { return GetType(); }

  bool CanBeMoved() const final { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  // Try to statically evaluate `this` and return a HConstant
  // containing the result of this evaluation.  If `this` cannot
  // be evaluated as a constant, return null.
  HConstant* TryStaticEvaluation() const;

  // Same but for `input` instead of GetInput().
  HConstant* TryStaticEvaluation(HInstruction* input) const;

  // Apply this operation to `x`.
  virtual HConstant* Evaluate([[maybe_unused]] HIntConstant* x) const {
    LOG(FATAL) << DebugName() << " is not defined for int values";
    UNREACHABLE();
  }
  virtual HConstant* Evaluate([[maybe_unused]] HLongConstant* x) const {
    LOG(FATAL) << DebugName() << " is not defined for long values";
    UNREACHABLE();
  }
  virtual HConstant* Evaluate([[maybe_unused]] HFloatConstant* x) const {
    LOG(FATAL) << DebugName() << " is not defined for float values";
    UNREACHABLE();
  }
  virtual HConstant* Evaluate([[maybe_unused]] HDoubleConstant* x) const {
    LOG(FATAL) << DebugName() << " is not defined for double values";
    UNREACHABLE();
  }

  DECLARE_ABSTRACT_INSTRUCTION(UnaryOperation);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(UnaryOperation);
};

class HBinaryOperation : public HExpression<2> {
 public:
  HBinaryOperation(InstructionKind kind,
                   DataType::Type result_type,
                   HInstruction* left,
                   HInstruction* right,
                   SideEffects side_effects = SideEffects::None(),
                   uint32_t dex_pc = kNoDexPc)
      : HExpression(kind, result_type, side_effects, dex_pc) {
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
  }

  // All of the BinaryOperation instructions are clonable.
  bool IsClonable() const override { return true; }

  HInstruction* GetLeft() const { return InputAt(0); }
  HInstruction* GetRight() const { return InputAt(1); }
  DataType::Type GetResultType() const { return GetType(); }

  virtual bool IsCommutative() const { return false; }

  // Put constant on the right.
  // Returns whether order is changed.
  bool OrderInputsWithConstantOnTheRight() {
    HInstruction* left = InputAt(0);
    HInstruction* right = InputAt(1);
    if (left->IsConstant() && !right->IsConstant()) {
      ReplaceInput(right, 0);
      ReplaceInput(left, 1);
      return true;
    }
    return false;
  }

  // Order inputs by instruction id, but favor constant on the right side.
  // This helps GVN for commutative ops.
  void OrderInputs() {
    DCHECK(IsCommutative());
    HInstruction* left = InputAt(0);
    HInstruction* right = InputAt(1);
    if (left == right || (!left->IsConstant() && right->IsConstant())) {
      return;
    }
    if (OrderInputsWithConstantOnTheRight()) {
      return;
    }
    // Order according to instruction id.
    if (left->GetId() > right->GetId()) {
      ReplaceInput(right, 0);
      ReplaceInput(left, 1);
    }
  }

  bool CanBeMoved() const final { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  // Try to statically evaluate `this` and return a HConstant
  // containing the result of this evaluation.  If `this` cannot
  // be evaluated as a constant, return null.
  HConstant* TryStaticEvaluation() const;

  // Same but for `left` and `right` instead of GetLeft() and GetRight().
  HConstant* TryStaticEvaluation(HInstruction* left, HInstruction* right) const;

  // Apply this operation to `x` and `y`.
  virtual HConstant* Evaluate([[maybe_unused]] HNullConstant* x,
                              [[maybe_unused]] HNullConstant* y) const {
    LOG(FATAL) << DebugName() << " is not defined for the (null, null) case.";
    UNREACHABLE();
  }
  virtual HConstant* Evaluate([[maybe_unused]] HIntConstant* x,
                              [[maybe_unused]] HIntConstant* y) const {
    LOG(FATAL) << DebugName() << " is not defined for the (int, int) case.";
    UNREACHABLE();
  }
  virtual HConstant* Evaluate([[maybe_unused]] HLongConstant* x,
                              [[maybe_unused]] HLongConstant* y) const {
    LOG(FATAL) << DebugName() << " is not defined for the (long, long) case.";
    UNREACHABLE();
  }
  virtual HConstant* Evaluate([[maybe_unused]] HLongConstant* x,
                              [[maybe_unused]] HIntConstant* y) const {
    LOG(FATAL) << DebugName() << " is not defined for the (long, int) case.";
    UNREACHABLE();
  }
  virtual HConstant* Evaluate([[maybe_unused]] HFloatConstant* x,
                              [[maybe_unused]] HFloatConstant* y) const {
    LOG(FATAL) << DebugName() << " is not defined for float values";
    UNREACHABLE();
  }
  virtual HConstant* Evaluate([[maybe_unused]] HDoubleConstant* x,
                              [[maybe_unused]] HDoubleConstant* y) const {
    LOG(FATAL) << DebugName() << " is not defined for double values";
    UNREACHABLE();
  }

  // Returns an input that can legally be used as the right input and is
  // constant, or null.
  HConstant* GetConstantRight() const;

  // If `GetConstantRight()` returns one of the input, this returns the other
  // one. Otherwise it returns null.
  HInstruction* GetLeastConstantLeft() const;

  DECLARE_ABSTRACT_INSTRUCTION(BinaryOperation);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(BinaryOperation);
};

// The comparison bias applies for floating point operations and indicates how NaN
// comparisons are treated:
enum class ComparisonBias {  // private marker to avoid generate-operator-out.py from processing.
  kNoBias,  // bias is not applicable (i.e. for long operation)
  kGtBias,  // return 1 for NaN comparisons
  kLtBias,  // return -1 for NaN comparisons
  kLast = kLtBias
};

std::ostream& operator<<(std::ostream& os, ComparisonBias rhs);

class HCondition : public HBinaryOperation {
 public:
  HCondition(InstructionKind kind,
             HInstruction* first,
             HInstruction* second,
             uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kind,
                         DataType::Type::kBool,
                         first,
                         second,
                         SideEffects::None(),
                         dex_pc) {
    SetPackedField<ComparisonBiasField>(ComparisonBias::kNoBias);
  }

  static HCondition* Create(HGraph* graph,
                            IfCondition cond,
                            HInstruction* lhs,
                            HInstruction* rhs,
                            uint32_t dex_pc = kNoDexPc);

  // For code generation purposes, returns whether this instruction is just before
  // `instruction`, and disregard moves in between.
  bool IsBeforeWhenDisregardMoves(HInstruction* instruction) const;

  DECLARE_ABSTRACT_INSTRUCTION(Condition);

  virtual IfCondition GetCondition() const = 0;

  virtual IfCondition GetOppositeCondition() const = 0;

  bool IsGtBias() const { return GetBias() == ComparisonBias::kGtBias; }
  bool IsLtBias() const { return GetBias() == ComparisonBias::kLtBias; }

  ComparisonBias GetBias() const { return GetPackedField<ComparisonBiasField>(); }
  void SetBias(ComparisonBias bias) { SetPackedField<ComparisonBiasField>(bias); }

  bool InstructionDataEquals(const HInstruction* other) const override {
    return GetPackedFields() == other->AsCondition()->GetPackedFields();
  }

  bool IsFPConditionTrueIfNaN() const {
    DCHECK(DataType::IsFloatingPointType(InputAt(0)->GetType())) << InputAt(0)->GetType();
    IfCondition if_cond = GetCondition();
    if (if_cond == kCondNE) {
      return true;
    } else if (if_cond == kCondEQ) {
      return false;
    }
    return ((if_cond == kCondGT) || (if_cond == kCondGE)) && IsGtBias();
  }

  bool IsFPConditionFalseIfNaN() const {
    DCHECK(DataType::IsFloatingPointType(InputAt(0)->GetType())) << InputAt(0)->GetType();
    IfCondition if_cond = GetCondition();
    if (if_cond == kCondEQ) {
      return true;
    } else if (if_cond == kCondNE) {
      return false;
    }
    return ((if_cond == kCondLT) || (if_cond == kCondLE)) && IsGtBias();
  }

 protected:
  // Needed if we merge a HCompare into a HCondition.
  static constexpr size_t kFieldComparisonBias = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldComparisonBiasSize =
      MinimumBitsToStore(static_cast<size_t>(ComparisonBias::kLast));
  static constexpr size_t kNumberOfConditionPackedBits =
      kFieldComparisonBias + kFieldComparisonBiasSize;
  static_assert(kNumberOfConditionPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
  using ComparisonBiasField =
      BitField<ComparisonBias, kFieldComparisonBias, kFieldComparisonBiasSize>;

  template <typename T>
  int32_t Compare(T x, T y) const { return x > y ? 1 : (x < y ? -1 : 0); }

  template <typename T>
  int32_t CompareFP(T x, T y) const {
    DCHECK(DataType::IsFloatingPointType(InputAt(0)->GetType())) << InputAt(0)->GetType();
    DCHECK_NE(GetBias(), ComparisonBias::kNoBias);
    // Handle the bias.
    return std::isunordered(x, y) ? (IsGtBias() ? 1 : -1) : Compare(x, y);
  }

  // Return an integer constant containing the result of a condition evaluated at compile time.
  HIntConstant* MakeConstantCondition(bool value) const {
    return GetBlock()->GetGraph()->GetIntConstant(value);
  }

  DEFAULT_COPY_CONSTRUCTOR(Condition);
};

// Instruction to check if two inputs are equal to each other.
class HEqual final : public HCondition {
 public:
  HEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kEqual, first, second, dex_pc) {
  }

  bool IsCommutative() const override { return true; }

  HConstant* Evaluate([[maybe_unused]] HNullConstant* x,
                      [[maybe_unused]] HNullConstant* y) const override {
    return MakeConstantCondition(true);
  }
  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  // In the following Evaluate methods, a HCompare instruction has
  // been merged into this HEqual instruction; evaluate it as
  // `Compare(x, y) == 0`.
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(Compare(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }

  DECLARE_INSTRUCTION(Equal);

  IfCondition GetCondition() const override {
    return kCondEQ;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondNE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Equal);

 private:
  template <typename T> static bool Compute(T x, T y) { return x == y; }
};

class HNotEqual final : public HCondition {
 public:
  HNotEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kNotEqual, first, second, dex_pc) {
  }

  bool IsCommutative() const override { return true; }

  HConstant* Evaluate([[maybe_unused]] HNullConstant* x,
                      [[maybe_unused]] HNullConstant* y) const override {
    return MakeConstantCondition(false);
  }
  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  // In the following Evaluate methods, a HCompare instruction has
  // been merged into this HNotEqual instruction; evaluate it as
  // `Compare(x, y) != 0`.
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(Compare(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }

  DECLARE_INSTRUCTION(NotEqual);

  IfCondition GetCondition() const override {
    return kCondNE;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondEQ;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(NotEqual);

 private:
  template <typename T> static bool Compute(T x, T y) { return x != y; }
};

class HLessThan final : public HCondition {
 public:
  HLessThan(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kLessThan, first, second, dex_pc) {
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  // In the following Evaluate methods, a HCompare instruction has
  // been merged into this HLessThan instruction; evaluate it as
  // `Compare(x, y) < 0`.
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(Compare(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }

  DECLARE_INSTRUCTION(LessThan);

  IfCondition GetCondition() const override {
    return kCondLT;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondGE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(LessThan);

 private:
  template <typename T> static bool Compute(T x, T y) { return x < y; }
};

class HLessThanOrEqual final : public HCondition {
 public:
  HLessThanOrEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kLessThanOrEqual, first, second, dex_pc) {
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  // In the following Evaluate methods, a HCompare instruction has
  // been merged into this HLessThanOrEqual instruction; evaluate it as
  // `Compare(x, y) <= 0`.
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(Compare(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }

  DECLARE_INSTRUCTION(LessThanOrEqual);

  IfCondition GetCondition() const override {
    return kCondLE;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondGT;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(LessThanOrEqual);

 private:
  template <typename T> static bool Compute(T x, T y) { return x <= y; }
};

class HGreaterThan final : public HCondition {
 public:
  HGreaterThan(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kGreaterThan, first, second, dex_pc) {
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  // In the following Evaluate methods, a HCompare instruction has
  // been merged into this HGreaterThan instruction; evaluate it as
  // `Compare(x, y) > 0`.
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(Compare(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }

  DECLARE_INSTRUCTION(GreaterThan);

  IfCondition GetCondition() const override {
    return kCondGT;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondLE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(GreaterThan);

 private:
  template <typename T> static bool Compute(T x, T y) { return x > y; }
};

class HGreaterThanOrEqual final : public HCondition {
 public:
  HGreaterThanOrEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kGreaterThanOrEqual, first, second, dex_pc) {
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  // In the following Evaluate methods, a HCompare instruction has
  // been merged into this HGreaterThanOrEqual instruction; evaluate it as
  // `Compare(x, y) >= 0`.
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(Compare(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return MakeConstantCondition(Compute(CompareFP(x->GetValue(), y->GetValue()), 0));
  }

  DECLARE_INSTRUCTION(GreaterThanOrEqual);

  IfCondition GetCondition() const override {
    return kCondGE;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondLT;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(GreaterThanOrEqual);

 private:
  template <typename T> static bool Compute(T x, T y) { return x >= y; }
};

class HBelow final : public HCondition {
 public:
  HBelow(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kBelow, first, second, dex_pc) {
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Below);

  IfCondition GetCondition() const override {
    return kCondB;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondAE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Below);

 private:
  template <typename T> static bool Compute(T x, T y) {
    return MakeUnsigned(x) < MakeUnsigned(y);
  }
};

class HBelowOrEqual final : public HCondition {
 public:
  HBelowOrEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kBelowOrEqual, first, second, dex_pc) {
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(BelowOrEqual);

  IfCondition GetCondition() const override {
    return kCondBE;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondA;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(BelowOrEqual);

 private:
  template <typename T> static bool Compute(T x, T y) {
    return MakeUnsigned(x) <= MakeUnsigned(y);
  }
};

class HAbove final : public HCondition {
 public:
  HAbove(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kAbove, first, second, dex_pc) {
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Above);

  IfCondition GetCondition() const override {
    return kCondA;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondBE;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Above);

 private:
  template <typename T> static bool Compute(T x, T y) {
    return MakeUnsigned(x) > MakeUnsigned(y);
  }
};

class HAboveOrEqual final : public HCondition {
 public:
  HAboveOrEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(kAboveOrEqual, first, second, dex_pc) {
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return MakeConstantCondition(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(AboveOrEqual);

  IfCondition GetCondition() const override {
    return kCondAE;
  }

  IfCondition GetOppositeCondition() const override {
    return kCondB;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(AboveOrEqual);

 private:
  template <typename T> static bool Compute(T x, T y) {
    return MakeUnsigned(x) >= MakeUnsigned(y);
  }
};

// Instruction to check how two inputs compare to each other.
// Result is 0 if input0 == input1, 1 if input0 > input1, or -1 if input0 < input1.
class HCompare final : public HBinaryOperation {
 public:
  // Note that `comparison_type` is the type of comparison performed
  // between the comparison's inputs, not the type of the instantiated
  // HCompare instruction (which is always DataType::Type::kInt).
  HCompare(DataType::Type comparison_type,
           HInstruction* first,
           HInstruction* second,
           ComparisonBias bias,
           uint32_t dex_pc)
      : HBinaryOperation(kCompare,
                         DataType::Type::kInt32,
                         first,
                         second,
                         SideEffectsForArchRuntimeCalls(comparison_type),
                         dex_pc) {
    SetPackedField<ComparisonBiasField>(bias);
    SetPackedField<ComparisonTypeField>(comparison_type);
  }

  template <typename T>
  int32_t Compute(T x, T y) const { return x > y ? 1 : (x < y ? -1 : 0); }

  template <typename T>
  int32_t ComputeFP(T x, T y) const {
    DCHECK(DataType::IsFloatingPointType(InputAt(0)->GetType())) << InputAt(0)->GetType();
    DCHECK_NE(GetBias(), ComparisonBias::kNoBias);
    // Handle the bias.
    return std::isunordered(x, y) ? (IsGtBias() ? 1 : -1) : Compute(x, y);
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    // Note that there is no "cmp-int" Dex instruction so we shouldn't
    // reach this code path when processing a freshly built HIR
    // graph. However HCompare integer instructions can be synthesized
    // by the instruction simplifier to implement IntegerCompare and
    // IntegerSignum intrinsics, so we have to handle this case.
    const int32_t value = DataType::IsUnsignedType(GetComparisonType()) ?
        Compute(x->GetValueAsUint64(), y->GetValueAsUint64()) :
        Compute(x->GetValue(), y->GetValue());
    return MakeConstantComparison(value);
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    const int32_t value = DataType::IsUnsignedType(GetComparisonType()) ?
        Compute(x->GetValueAsUint64(), y->GetValueAsUint64()) :
        Compute(x->GetValue(), y->GetValue());
    return MakeConstantComparison(value);
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return MakeConstantComparison(ComputeFP(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return MakeConstantComparison(ComputeFP(x->GetValue(), y->GetValue()));
  }

  bool InstructionDataEquals(const HInstruction* other) const override {
    return GetPackedFields() == other->AsCompare()->GetPackedFields();
  }

  ComparisonBias GetBias() const { return GetPackedField<ComparisonBiasField>(); }

  DataType::Type GetComparisonType() const { return GetPackedField<ComparisonTypeField>(); }

  void SetComparisonType(DataType::Type newType) { SetPackedField<ComparisonTypeField>(newType); }

  // Does this compare instruction have a "gt bias" (vs an "lt bias")?
  // Only meaningful for floating-point comparisons.
  bool IsGtBias() const {
    DCHECK(DataType::IsFloatingPointType(InputAt(0)->GetType())) << InputAt(0)->GetType();
    return GetBias() == ComparisonBias::kGtBias;
  }

  static SideEffects SideEffectsForArchRuntimeCalls([[maybe_unused]] DataType::Type type) {
    // Comparisons do not require a runtime call in any back end.
    return SideEffects::None();
  }

  DECLARE_INSTRUCTION(Compare);

 protected:
  static constexpr size_t kFieldComparisonBias = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldComparisonBiasSize =
      MinimumBitsToStore(static_cast<size_t>(ComparisonBias::kLast));
  static constexpr size_t kFieldComparisonType = kFieldComparisonBias + kFieldComparisonBiasSize;
  static constexpr size_t kFieldComparisonTypeSize =
      MinimumBitsToStore(static_cast<size_t>(DataType::Type::kLast));
  static constexpr size_t kNumberOfComparePackedBits =
      kFieldComparisonType + kFieldComparisonTypeSize;
  static_assert(kNumberOfComparePackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
  using ComparisonBiasField =
      BitField<ComparisonBias, kFieldComparisonBias, kFieldComparisonBiasSize>;
  using ComparisonTypeField =
      BitField<DataType::Type, kFieldComparisonType, kFieldComparisonTypeSize>;

  // Return an integer constant containing the result of a comparison evaluated at compile time.
  HIntConstant* MakeConstantComparison(int32_t value) const {
    DCHECK(value == -1 || value == 0 || value == 1) << value;
    return GetBlock()->GetGraph()->GetIntConstant(value);
  }

  DEFAULT_COPY_CONSTRUCTOR(Compare);
};

class HNewInstance final : public HExpression<1> {
 public:
  HNewInstance(HInstruction* cls,
               uint32_t dex_pc,
               dex::TypeIndex type_index,
               const DexFile& dex_file,
               bool finalizable,
               QuickEntrypointEnum entrypoint)
      : HExpression(kNewInstance,
                    DataType::Type::kReference,
                    SideEffects::CanTriggerGC(),
                    dex_pc),
        type_index_(type_index),
        dex_file_(dex_file),
        entrypoint_(entrypoint) {
    SetPackedFlag<kFlagFinalizable>(finalizable);
    SetPackedFlag<kFlagPartialMaterialization>(false);
    SetRawInputAt(0, cls);
  }

  bool IsClonable() const override { return true; }

  void SetPartialMaterialization() {
    SetPackedFlag<kFlagPartialMaterialization>(true);
  }

  dex::TypeIndex GetTypeIndex() const { return type_index_; }
  const DexFile& GetDexFile() const { return dex_file_; }

  // Calls runtime so needs an environment.
  bool NeedsEnvironment() const override { return true; }

  // Can throw errors when out-of-memory or if it's not instantiable/accessible.
  bool CanThrow() const override { return true; }
  bool OnlyThrowsAsyncExceptions() const override {
    return !IsFinalizable() && !NeedsChecks();
  }

  bool NeedsChecks() const {
    return entrypoint_ == kQuickAllocObjectWithChecks;
  }

  bool IsFinalizable() const { return GetPackedFlag<kFlagFinalizable>(); }

  bool CanBeNull() const override { return false; }

  bool IsPartialMaterialization() const {
    return GetPackedFlag<kFlagPartialMaterialization>();
  }

  QuickEntrypointEnum GetEntrypoint() const { return entrypoint_; }

  void SetEntrypoint(QuickEntrypointEnum entrypoint) {
    entrypoint_ = entrypoint;
  }

  HLoadClass* GetLoadClass() const {
    HInstruction* input = InputAt(0);
    if (input->IsClinitCheck()) {
      input = input->InputAt(0);
    }
    DCHECK(input->IsLoadClass());
    return input->AsLoadClass();
  }

  bool IsStringAlloc() const;

  DECLARE_INSTRUCTION(NewInstance);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(NewInstance);

 private:
  static constexpr size_t kFlagFinalizable = kNumberOfGenericPackedBits;
  static constexpr size_t kFlagPartialMaterialization = kFlagFinalizable + 1;
  static constexpr size_t kNumberOfNewInstancePackedBits = kFlagPartialMaterialization + 1;
  static_assert(kNumberOfNewInstancePackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");

  const dex::TypeIndex type_index_;
  const DexFile& dex_file_;
  QuickEntrypointEnum entrypoint_;
};

enum IntrinsicNeedsEnvironment {
  kNoEnvironment,        // Intrinsic does not require an environment.
  kNeedsEnvironment      // Intrinsic requires an environment.
};

enum IntrinsicSideEffects {
  kNoSideEffects,     // Intrinsic does not have any heap memory side effects.
  kReadSideEffects,   // Intrinsic may read heap memory.
  kWriteSideEffects,  // Intrinsic may write heap memory.
  kAllSideEffects     // Intrinsic may read or write heap memory, or trigger GC.
};

enum IntrinsicExceptions {
  kNoThrow,  // Intrinsic does not throw any exceptions.
  kCanThrow  // Intrinsic may throw exceptions.
};

// Determines how to load an ArtMethod*.
enum class MethodLoadKind {
  // Use a String init ArtMethod* loaded from Thread entrypoints.
  kStringInit,

  // Use the method's own ArtMethod* loaded by the register allocator.
  kRecursive,

  // Use PC-relative boot image ArtMethod* address that will be known at link time.
  // Used for boot image methods referenced by boot image code.
  kBootImageLinkTimePcRelative,

  // Load from a boot image entry in the .data.img.rel.ro using a PC-relative load.
  // Used for app->boot calls with relocatable image.
  kBootImageRelRo,

  // Load from an app image entry in the .data.img.rel.ro using a PC-relative load.
  // Used for app image methods referenced by apps in AOT-compiled code.
  kAppImageRelRo,

  // Load from an entry in the .bss section using a PC-relative load.
  // Used for methods outside boot image referenced by AOT-compiled app and boot image code.
  kBssEntry,

  // Use ArtMethod* at a known address, embed the direct address in the code.
  // Used for for JIT-compiled calls.
  kJitDirectAddress,

  // Make a runtime call to resolve and call the method. This is the last-resort-kind
  // used when other kinds are unimplemented on a particular architecture.
  kRuntimeCall,
};

// Determines the location of the code pointer of an invoke.
enum class CodePtrLocation {
  // Recursive call, use local PC-relative call instruction.
  kCallSelf,

  // Use native pointer from the Artmethod*.
  // Used for @CriticalNative to avoid going through the compiled stub. This call goes through
  // a special resolution stub if the class is not initialized or no native code is registered.
  kCallCriticalNative,

  // Use code pointer from the ArtMethod*.
  // Used when we don't know the target code. This is also the last-resort-kind used when
  // other kinds are unimplemented or impractical (i.e. slow) on a particular architecture.
  kCallArtMethod,
};

static inline bool IsPcRelativeMethodLoadKind(MethodLoadKind load_kind) {
  return load_kind == MethodLoadKind::kBootImageLinkTimePcRelative ||
         load_kind == MethodLoadKind::kBootImageRelRo ||
         load_kind == MethodLoadKind::kAppImageRelRo ||
         load_kind == MethodLoadKind::kBssEntry;
}

class HInvoke : public HVariableInputSizeInstruction {
 public:
  bool NeedsEnvironment() const override;

  void SetArgumentAt(size_t index, HInstruction* argument) {
    SetRawInputAt(index, argument);
  }

  // Return the number of arguments.  This number can be lower than
  // the number of inputs returned by InputCount(), as some invoke
  // instructions (e.g. HInvokeStaticOrDirect) can have non-argument
  // inputs at the end of their list of inputs.
  uint32_t GetNumberOfArguments() const { return number_of_arguments_; }

  // Return the number of outgoing vregs.
  uint32_t GetNumberOfOutVRegs() const { return number_of_out_vregs_; }

  InvokeType GetInvokeType() const {
    return GetPackedField<InvokeTypeField>();
  }

  Intrinsics GetIntrinsic() const {
    return intrinsic_;
  }

  void SetIntrinsic(Intrinsics intrinsic,
                    IntrinsicNeedsEnvironment needs_env,
                    IntrinsicSideEffects side_effects,
                    IntrinsicExceptions exceptions);

  bool IsFromInlinedInvoke() const {
    return GetEnvironment()->IsFromInlinedInvoke();
  }

  void SetCanThrow(bool can_throw) { SetPackedFlag<kFlagCanThrow>(can_throw); }

  bool CanThrow() const override { return GetPackedFlag<kFlagCanThrow>(); }

  void SetAlwaysThrows(bool always_throws) { SetPackedFlag<kFlagAlwaysThrows>(always_throws); }

  bool AlwaysThrows() const override final { return GetPackedFlag<kFlagAlwaysThrows>(); }

  bool CanBeMoved() const override { return IsIntrinsic() && !DoesAnyWrite(); }

  bool CanBeNull() const override;

  bool InstructionDataEquals(const HInstruction* other) const override {
    return intrinsic_ != Intrinsics::kNone && intrinsic_ == other->AsInvoke()->intrinsic_;
  }

  uint32_t* GetIntrinsicOptimizations() {
    return &intrinsic_optimizations_;
  }

  const uint32_t* GetIntrinsicOptimizations() const {
    return &intrinsic_optimizations_;
  }

  bool IsIntrinsic() const { return intrinsic_ != Intrinsics::kNone; }

  ArtMethod* GetResolvedMethod() const { return resolved_method_; }
  void SetResolvedMethod(ArtMethod* method, bool enable_intrinsic_opt);

  MethodReference GetMethodReference() const { return method_reference_; }

  const MethodReference GetResolvedMethodReference() const {
    return resolved_method_reference_;
  }

  DECLARE_ABSTRACT_INSTRUCTION(Invoke);

 protected:
  static constexpr size_t kFieldInvokeType = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldInvokeTypeSize =
      MinimumBitsToStore(static_cast<size_t>(kMaxInvokeType));
  static constexpr size_t kFlagCanThrow = kFieldInvokeType + kFieldInvokeTypeSize;
  static constexpr size_t kFlagAlwaysThrows = kFlagCanThrow + 1;
  static constexpr size_t kNumberOfInvokePackedBits = kFlagAlwaysThrows + 1;
  static_assert(kNumberOfInvokePackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
  using InvokeTypeField = BitField<InvokeType, kFieldInvokeType, kFieldInvokeTypeSize>;

  HInvoke(InstructionKind kind,
          ArenaAllocator* allocator,
          uint32_t number_of_arguments,
          uint32_t number_of_out_vregs,
          uint32_t number_of_other_inputs,
          DataType::Type return_type,
          uint32_t dex_pc,
          MethodReference method_reference,
          ArtMethod* resolved_method,
          MethodReference resolved_method_reference,
          InvokeType invoke_type,
          bool enable_intrinsic_opt)
    : HVariableInputSizeInstruction(
          kind,
          return_type,
          SideEffects::AllExceptGCDependency(),  // Assume write/read on all fields/arrays.
          dex_pc,
          allocator,
          number_of_arguments + number_of_other_inputs,
          kArenaAllocInvokeInputs),
      method_reference_(method_reference),
      resolved_method_reference_(resolved_method_reference),
      number_of_arguments_(dchecked_integral_cast<uint16_t>(number_of_arguments)),
      number_of_out_vregs_(dchecked_integral_cast<uint16_t>(number_of_out_vregs)),
      intrinsic_(Intrinsics::kNone),
      intrinsic_optimizations_(0) {
    SetPackedField<InvokeTypeField>(invoke_type);
    SetPackedFlag<kFlagCanThrow>(true);
    SetResolvedMethod(resolved_method, enable_intrinsic_opt);
  }

  DEFAULT_COPY_CONSTRUCTOR(Invoke);

  ArtMethod* resolved_method_;
  const MethodReference method_reference_;
  // Cached values of the resolved method, to avoid needing the mutator lock.
  const MethodReference resolved_method_reference_;

  uint16_t number_of_arguments_;
  uint16_t number_of_out_vregs_;

  Intrinsics intrinsic_;

  // A magic word holding optimizations for intrinsics. See intrinsics.h.
  uint32_t intrinsic_optimizations_;
};

class HInvokeUnresolved final : public HInvoke {
 public:
  HInvokeUnresolved(ArenaAllocator* allocator,
                    uint32_t number_of_arguments,
                    uint32_t number_of_out_vregs,
                    DataType::Type return_type,
                    uint32_t dex_pc,
                    MethodReference method_reference,
                    InvokeType invoke_type)
      : HInvoke(kInvokeUnresolved,
                allocator,
                number_of_arguments,
                number_of_out_vregs,
                /* number_of_other_inputs= */ 0u,
                return_type,
                dex_pc,
                method_reference,
                nullptr,
                MethodReference(nullptr, 0u),
                invoke_type,
                /* enable_intrinsic_opt= */ false) {
  }

  bool IsClonable() const override { return true; }

  DECLARE_INSTRUCTION(InvokeUnresolved);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(InvokeUnresolved);
};

class HInvokePolymorphic final : public HInvoke {
 public:
  HInvokePolymorphic(ArenaAllocator* allocator,
                     uint32_t number_of_arguments,
                     uint32_t number_of_out_vregs,
                     uint32_t number_of_other_inputs,
                     DataType::Type return_type,
                     uint32_t dex_pc,
                     MethodReference method_reference,
                     // resolved_method is the ArtMethod object corresponding to the polymorphic
                     // method (e.g. VarHandle.get), resolved using the class linker. It is needed
                     // to pass intrinsic information to the HInvokePolymorphic node.
                     ArtMethod* resolved_method,
                     MethodReference resolved_method_reference,
                     dex::ProtoIndex proto_idx)
      : HInvoke(kInvokePolymorphic,
                allocator,
                number_of_arguments,
                number_of_out_vregs,
                number_of_other_inputs,
                return_type,
                dex_pc,
                method_reference,
                resolved_method,
                resolved_method_reference,
                kPolymorphic,
                /* enable_intrinsic_opt= */ true),
        proto_idx_(proto_idx) {}

  bool IsClonable() const override { return true; }

  dex::ProtoIndex GetProtoIndex() { return proto_idx_; }

  bool IsMethodHandleInvokeExact() const {
    return GetIntrinsic() == Intrinsics::kMethodHandleInvokeExact;
  }

  bool CanTargetInstanceMethod() const {
    DCHECK(IsMethodHandleInvokeExact());
    return GetNumberOfArguments() >= 2 &&
        InputAt(1)->GetType() == DataType::Type::kReference;
  }

  DECLARE_INSTRUCTION(InvokePolymorphic);

 protected:
  dex::ProtoIndex proto_idx_;
  DEFAULT_COPY_CONSTRUCTOR(InvokePolymorphic);
};

class HInvokeCustom final : public HInvoke {
 public:
  HInvokeCustom(ArenaAllocator* allocator,
                uint32_t number_of_arguments,
                uint32_t number_of_out_vregs,
                uint32_t call_site_index,
                DataType::Type return_type,
                uint32_t dex_pc,
                MethodReference method_reference,
                bool enable_intrinsic_opt)
      : HInvoke(kInvokeCustom,
                allocator,
                number_of_arguments,
                number_of_out_vregs,
                /* number_of_other_inputs= */ 0u,
                return_type,
                dex_pc,
                method_reference,
                /* resolved_method= */ nullptr,
                MethodReference(nullptr, 0u),
                kStatic,
                enable_intrinsic_opt),
      call_site_index_(call_site_index) {
  }

  uint32_t GetCallSiteIndex() const { return call_site_index_; }

  bool IsClonable() const override { return true; }

  DECLARE_INSTRUCTION(InvokeCustom);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(InvokeCustom);

 private:
  uint32_t call_site_index_;
};

class HInvokeStaticOrDirect final : public HInvoke {
 public:
  // Requirements of this method call regarding the class
  // initialization (clinit) check of its declaring class.
  enum class ClinitCheckRequirement {  // private marker to avoid generate-operator-out.py from processing.
    kNone,      // Class already initialized.
    kExplicit,  // Static call having explicit clinit check as last input.
    kImplicit,  // Static call implicitly requiring a clinit check.
    kLast = kImplicit
  };

  struct DispatchInfo {
    MethodLoadKind method_load_kind;
    CodePtrLocation code_ptr_location;
    // The method load data holds
    //   - thread entrypoint offset for kStringInit method if this is a string init invoke.
    //     Note that there are multiple string init methods, each having its own offset.
    //   - the method address for kDirectAddress
    uint64_t method_load_data;
  };

  HInvokeStaticOrDirect(ArenaAllocator* allocator,
                        uint32_t number_of_arguments,
                        uint32_t number_of_out_vregs,
                        DataType::Type return_type,
                        uint32_t dex_pc,
                        MethodReference method_reference,
                        ArtMethod* resolved_method,
                        DispatchInfo dispatch_info,
                        InvokeType invoke_type,
                        MethodReference resolved_method_reference,
                        ClinitCheckRequirement clinit_check_requirement,
                        bool enable_intrinsic_opt)
      : HInvoke(kInvokeStaticOrDirect,
                allocator,
                number_of_arguments,
                number_of_out_vregs,
                // There is potentially one extra argument for the HCurrentMethod input,
                // and one other if the clinit check is explicit. These can be removed later.
                (NeedsCurrentMethodInput(dispatch_info) ? 1u : 0u) +
                    (clinit_check_requirement == ClinitCheckRequirement::kExplicit ? 1u : 0u),
                return_type,
                dex_pc,
                method_reference,
                resolved_method,
                resolved_method_reference,
                invoke_type,
                enable_intrinsic_opt),
        dispatch_info_(dispatch_info) {
    SetPackedField<ClinitCheckRequirementField>(clinit_check_requirement);
  }

  bool IsClonable() const override { return true; }
  bool NeedsBss() const override {
    return GetMethodLoadKind() == MethodLoadKind::kBssEntry;
  }

  void SetDispatchInfo(DispatchInfo dispatch_info) {
    bool had_current_method_input = HasCurrentMethodInput();
    bool needs_current_method_input = NeedsCurrentMethodInput(dispatch_info);

    // Using the current method is the default and once we find a better
    // method load kind, we should not go back to using the current method.
    DCHECK(had_current_method_input || !needs_current_method_input);

    if (had_current_method_input && !needs_current_method_input) {
      DCHECK_EQ(InputAt(GetCurrentMethodIndex()), GetBlock()->GetGraph()->GetCurrentMethod());
      RemoveInputAt(GetCurrentMethodIndex());
    }
    dispatch_info_ = dispatch_info;
  }

  DispatchInfo GetDispatchInfo() const {
    return dispatch_info_;
  }

  using HInstruction::GetInputRecords;  // Keep the const version visible.
  ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() override {
    ArrayRef<HUserRecord<HInstruction*>> input_records = HInvoke::GetInputRecords();
    if (kIsDebugBuild && IsStaticWithExplicitClinitCheck()) {
      DCHECK(!input_records.empty());
      DCHECK_GT(input_records.size(), GetNumberOfArguments());
      HInstruction* last_input = input_records.back().GetInstruction();
      // Note: `last_input` may be null during arguments setup.
      if (last_input != nullptr) {
        // `last_input` is the last input of a static invoke marked as having
        // an explicit clinit check. It must either be:
        // - an art::HClinitCheck instruction, set by art::HGraphBuilder; or
        // - an art::HLoadClass instruction, set by art::PrepareForRegisterAllocation.
        DCHECK(last_input->IsClinitCheck() || last_input->IsLoadClass()) << last_input->DebugName();
      }
    }
    return input_records;
  }

  bool CanDoImplicitNullCheckOn([[maybe_unused]] HInstruction* obj) const override {
    // We do not access the method via object reference, so we cannot do an implicit null check.
    // TODO: for intrinsics we can generate implicit null checks.
    return false;
  }

  bool CanBeNull() const override;

  MethodLoadKind GetMethodLoadKind() const { return dispatch_info_.method_load_kind; }
  CodePtrLocation GetCodePtrLocation() const {
    // We do CHA analysis after sharpening. When a method has CHA inlining, it
    // cannot call itself, as if the CHA optmization is invalid we want to make
    // sure the method is never executed again. So, while sharpening can return
    // kCallSelf, we bypass it here if there is a CHA optimization.
    if (dispatch_info_.code_ptr_location == CodePtrLocation::kCallSelf &&
        GetBlock()->GetGraph()->HasShouldDeoptimizeFlag()) {
      return CodePtrLocation::kCallArtMethod;
    } else {
      return dispatch_info_.code_ptr_location;
    }
  }
  bool IsRecursive() const { return GetMethodLoadKind() == MethodLoadKind::kRecursive; }
  bool IsStringInit() const { return GetMethodLoadKind() == MethodLoadKind::kStringInit; }
  bool HasMethodAddress() const { return GetMethodLoadKind() == MethodLoadKind::kJitDirectAddress; }
  bool HasPcRelativeMethodLoadKind() const {
    return IsPcRelativeMethodLoadKind(GetMethodLoadKind());
  }

  QuickEntrypointEnum GetStringInitEntryPoint() const {
    DCHECK(IsStringInit());
    return static_cast<QuickEntrypointEnum>(dispatch_info_.method_load_data);
  }

  uint64_t GetMethodAddress() const {
    DCHECK(HasMethodAddress());
    return dispatch_info_.method_load_data;
  }

  const DexFile& GetDexFileForPcRelativeDexCache() const;

  ClinitCheckRequirement GetClinitCheckRequirement() const {
    return GetPackedField<ClinitCheckRequirementField>();
  }

  // Is this instruction a call to a static method?
  bool IsStatic() const {
    return GetInvokeType() == kStatic;
  }

  // Does this method load kind need the current method as an input?
  static bool NeedsCurrentMethodInput(DispatchInfo dispatch_info) {
    return dispatch_info.method_load_kind == MethodLoadKind::kRecursive ||
           dispatch_info.method_load_kind == MethodLoadKind::kRuntimeCall ||
           dispatch_info.code_ptr_location == CodePtrLocation::kCallCriticalNative;
  }

  // Get the index of the current method input.
  size_t GetCurrentMethodIndex() const {
    DCHECK(HasCurrentMethodInput());
    return GetCurrentMethodIndexUnchecked();
  }
  size_t GetCurrentMethodIndexUnchecked() const {
    return GetNumberOfArguments();
  }

  // Check if the method has a current method input.
  bool HasCurrentMethodInput() const {
    if (NeedsCurrentMethodInput(GetDispatchInfo())) {
      DCHECK(InputAt(GetCurrentMethodIndexUnchecked()) == nullptr ||  // During argument setup.
             InputAt(GetCurrentMethodIndexUnchecked())->IsCurrentMethod());
      return true;
    } else {
      DCHECK(InputCount() == GetCurrentMethodIndexUnchecked() ||
             InputAt(GetCurrentMethodIndexUnchecked()) == nullptr ||  // During argument setup.
             !InputAt(GetCurrentMethodIndexUnchecked())->IsCurrentMethod());
      return false;
    }
  }

  // Get the index of the special input.
  size_t GetSpecialInputIndex() const {
    DCHECK(HasSpecialInput());
    return GetSpecialInputIndexUnchecked();
  }
  size_t GetSpecialInputIndexUnchecked() const {
    return GetNumberOfArguments() + (HasCurrentMethodInput() ? 1u : 0u);
  }

  // Check if the method has a special input.
  bool HasSpecialInput() const {
    size_t other_inputs =
        GetSpecialInputIndexUnchecked() + (IsStaticWithExplicitClinitCheck() ? 1u : 0u);
    size_t input_count = InputCount();
    DCHECK_LE(input_count - other_inputs, 1u) << other_inputs << " " << input_count;
    return other_inputs != input_count;
  }

  void AddSpecialInput(HInstruction* input) {
    // We allow only one special input.
    DCHECK(!HasSpecialInput());
    InsertInputAt(GetSpecialInputIndexUnchecked(), input);
  }

  // Remove the HClinitCheck or the replacement HLoadClass (set as last input by
  // PrepareForRegisterAllocation::VisitClinitCheck() in lieu of the initial HClinitCheck)
  // instruction; only relevant for static calls with explicit clinit check.
  void RemoveExplicitClinitCheck(ClinitCheckRequirement new_requirement) {
    DCHECK(IsStaticWithExplicitClinitCheck());
    size_t last_input_index = inputs_.size() - 1u;
    HInstruction* last_input = inputs_.back().GetInstruction();
    DCHECK(last_input != nullptr);
    DCHECK(last_input->IsLoadClass() || last_input->IsClinitCheck()) << last_input->DebugName();
    RemoveAsUserOfInput(last_input_index);
    inputs_.pop_back();
    SetPackedField<ClinitCheckRequirementField>(new_requirement);
    DCHECK(!IsStaticWithExplicitClinitCheck());
  }

  // Is this a call to a static method whose declaring class has an
  // explicit initialization check in the graph?
  bool IsStaticWithExplicitClinitCheck() const {
    return IsStatic() && (GetClinitCheckRequirement() == ClinitCheckRequirement::kExplicit);
  }

  // Is this a call to a static method whose declaring class has an
  // implicit intialization check requirement?
  bool IsStaticWithImplicitClinitCheck() const {
    return IsStatic() && (GetClinitCheckRequirement() == ClinitCheckRequirement::kImplicit);
  }

  DECLARE_INSTRUCTION(InvokeStaticOrDirect);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(InvokeStaticOrDirect);

 private:
  static constexpr size_t kFieldClinitCheckRequirement = kNumberOfInvokePackedBits;
  static constexpr size_t kFieldClinitCheckRequirementSize =
      MinimumBitsToStore(static_cast<size_t>(ClinitCheckRequirement::kLast));
  static constexpr size_t kNumberOfInvokeStaticOrDirectPackedBits =
      kFieldClinitCheckRequirement + kFieldClinitCheckRequirementSize;
  static_assert(kNumberOfInvokeStaticOrDirectPackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using ClinitCheckRequirementField = BitField<ClinitCheckRequirement,
                                               kFieldClinitCheckRequirement,
                                               kFieldClinitCheckRequirementSize>;

  DispatchInfo dispatch_info_;
};
std::ostream& operator<<(std::ostream& os, MethodLoadKind rhs);
std::ostream& operator<<(std::ostream& os, CodePtrLocation rhs);
std::ostream& operator<<(std::ostream& os, HInvokeStaticOrDirect::ClinitCheckRequirement rhs);

class HInvokeVirtual final : public HInvoke {
 public:
  HInvokeVirtual(ArenaAllocator* allocator,
                 uint32_t number_of_arguments,
                 uint32_t number_of_out_vregs,
                 DataType::Type return_type,
                 uint32_t dex_pc,
                 MethodReference method_reference,
                 ArtMethod* resolved_method,
                 MethodReference resolved_method_reference,
                 uint32_t vtable_index,
                 bool enable_intrinsic_opt)
      : HInvoke(kInvokeVirtual,
                allocator,
                number_of_arguments,
                number_of_out_vregs,
                0u,
                return_type,
                dex_pc,
                method_reference,
                resolved_method,
                resolved_method_reference,
                kVirtual,
                enable_intrinsic_opt),
        vtable_index_(vtable_index) {
  }

  bool IsClonable() const override { return true; }

  bool CanDoImplicitNullCheckOn(HInstruction* obj) const override;

  uint32_t GetVTableIndex() const { return vtable_index_; }

  DECLARE_INSTRUCTION(InvokeVirtual);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(InvokeVirtual);

 private:
  // Cached value of the resolved method, to avoid needing the mutator lock.
  const uint32_t vtable_index_;
};

class HInvokeInterface final : public HInvoke {
 public:
  HInvokeInterface(ArenaAllocator* allocator,
                   uint32_t number_of_arguments,
                   uint32_t number_of_out_vregs,
                   DataType::Type return_type,
                   uint32_t dex_pc,
                   MethodReference method_reference,
                   ArtMethod* resolved_method,
                   MethodReference resolved_method_reference,
                   uint32_t imt_index,
                   MethodLoadKind load_kind,
                   bool enable_intrinsic_opt)
      : HInvoke(kInvokeInterface,
                allocator,
                number_of_arguments + (NeedsCurrentMethod(load_kind) ? 1 : 0),
                number_of_out_vregs,
                0u,
                return_type,
                dex_pc,
                method_reference,
                resolved_method,
                resolved_method_reference,
                kInterface,
                enable_intrinsic_opt),
        imt_index_(imt_index),
        hidden_argument_load_kind_(load_kind) {
  }

  static bool NeedsCurrentMethod(MethodLoadKind load_kind) {
    return load_kind == MethodLoadKind::kRecursive;
  }

  bool IsClonable() const override { return true; }
  bool NeedsBss() const override {
    return GetHiddenArgumentLoadKind() == MethodLoadKind::kBssEntry;
  }

  bool CanDoImplicitNullCheckOn(HInstruction* obj) const override {
    // TODO: Add implicit null checks in intrinsics.
    return (obj == InputAt(0)) && !IsIntrinsic();
  }

  size_t GetSpecialInputIndex() const {
    return GetNumberOfArguments();
  }

  void AddSpecialInput(HInstruction* input) {
    InsertInputAt(GetSpecialInputIndex(), input);
  }

  uint32_t GetImtIndex() const { return imt_index_; }
  MethodLoadKind GetHiddenArgumentLoadKind() const { return hidden_argument_load_kind_; }

  DECLARE_INSTRUCTION(InvokeInterface);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(InvokeInterface);

 private:
  // Cached value of the resolved method, to avoid needing the mutator lock.
  const uint32_t imt_index_;

  // How the hidden argument (the interface method) is being loaded.
  const MethodLoadKind hidden_argument_load_kind_;
};

class HNeg final : public HUnaryOperation {
 public:
  HNeg(DataType::Type result_type, HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HUnaryOperation(kNeg, result_type, input, dex_pc) {
    DCHECK_EQ(result_type, DataType::Kind(input->GetType()));
  }

  template <typename T> static T Compute(T x) { return -x; }

  HConstant* Evaluate(HIntConstant* x) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue()));
  }
  HConstant* Evaluate(HFloatConstant* x) const override {
    return GetBlock()->GetGraph()->GetFloatConstant(Compute(x->GetValue()));
  }
  HConstant* Evaluate(HDoubleConstant* x) const override {
    return GetBlock()->GetGraph()->GetDoubleConstant(Compute(x->GetValue()));
  }

  DECLARE_INSTRUCTION(Neg);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Neg);
};

class HNewArray final : public HExpression<2> {
 public:
  HNewArray(HInstruction* cls, HInstruction* length, uint32_t dex_pc, size_t component_size_shift)
      : HExpression(kNewArray, DataType::Type::kReference, SideEffects::CanTriggerGC(), dex_pc) {
    SetRawInputAt(0, cls);
    SetRawInputAt(1, length);
    SetPackedField<ComponentSizeShiftField>(component_size_shift);
  }

  bool IsClonable() const override { return true; }

  // Calls runtime so needs an environment.
  bool NeedsEnvironment() const override { return true; }

  // May throw NegativeArraySizeException, OutOfMemoryError, etc.
  bool CanThrow() const override { return true; }

  bool CanBeNull() const override { return false; }

  HLoadClass* GetLoadClass() const {
    DCHECK(InputAt(0)->IsLoadClass());
    return InputAt(0)->AsLoadClass();
  }

  HInstruction* GetLength() const {
    return InputAt(1);
  }

  size_t GetComponentSizeShift() {
    return GetPackedField<ComponentSizeShiftField>();
  }

  DECLARE_INSTRUCTION(NewArray);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(NewArray);

 private:
  static constexpr size_t kFieldComponentSizeShift = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldComponentSizeShiftSize = MinimumBitsToStore(3u);
  static constexpr size_t kNumberOfNewArrayPackedBits =
      kFieldComponentSizeShift + kFieldComponentSizeShiftSize;
  static_assert(kNumberOfNewArrayPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
  using ComponentSizeShiftField =
      BitField<size_t, kFieldComponentSizeShift, kFieldComponentSizeShiftSize>;
};

class HAdd final : public HBinaryOperation {
 public:
  HAdd(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kAdd, result_type, left, right, SideEffects::None(), dex_pc) {
  }

  bool IsCommutative() const override { return true; }

  template <typename T> static T Compute(T x, T y) { return x + y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return GetBlock()->GetGraph()->GetFloatConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return GetBlock()->GetGraph()->GetDoubleConstant(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Add);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Add);
};

class HSub final : public HBinaryOperation {
 public:
  HSub(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kSub, result_type, left, right, SideEffects::None(), dex_pc) {
  }

  template <typename T> static T Compute(T x, T y) { return x - y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return GetBlock()->GetGraph()->GetFloatConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return GetBlock()->GetGraph()->GetDoubleConstant(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Sub);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Sub);
};

class HMul final : public HBinaryOperation {
 public:
  HMul(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kMul, result_type, left, right, SideEffects::None(), dex_pc) {
  }

  bool IsCommutative() const override { return true; }

  template <typename T> static T Compute(T x, T y) { return x * y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return GetBlock()->GetGraph()->GetFloatConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return GetBlock()->GetGraph()->GetDoubleConstant(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Mul);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Mul);
};

class HDiv final : public HBinaryOperation {
 public:
  HDiv(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc)
      : HBinaryOperation(kDiv, result_type, left, right, SideEffects::None(), dex_pc) {
  }

  template <typename T>
  T ComputeIntegral(T x, T y) const {
    DCHECK(!DataType::IsFloatingPointType(GetType())) << GetType();
    // Our graph structure ensures we never have 0 for `y` during
    // constant folding.
    DCHECK_NE(y, 0);
    // Special case -1 to avoid getting a SIGFPE on x86(_64).
    return (y == -1) ? -x : x / y;
  }

  template <typename T>
  T ComputeFP(T x, T y) const {
    DCHECK(DataType::IsFloatingPointType(GetType())) << GetType();
    return x / y;
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(ComputeIntegral(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(ComputeIntegral(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return GetBlock()->GetGraph()->GetFloatConstant(ComputeFP(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return GetBlock()->GetGraph()->GetDoubleConstant(ComputeFP(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Div);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Div);
};

class HRem final : public HBinaryOperation {
 public:
  HRem(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc)
      : HBinaryOperation(kRem, result_type, left, right, SideEffects::None(), dex_pc) {
  }

  template <typename T>
  T ComputeIntegral(T x, T y) const {
    DCHECK(!DataType::IsFloatingPointType(GetType())) << GetType();
    // Our graph structure ensures we never have 0 for `y` during
    // constant folding.
    DCHECK_NE(y, 0);
    // Special case -1 to avoid getting a SIGFPE on x86(_64).
    return (y == -1) ? 0 : x % y;
  }

  template <typename T>
  T ComputeFP(T x, T y) const {
    DCHECK(DataType::IsFloatingPointType(GetType())) << GetType();
    return std::fmod(x, y);
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(ComputeIntegral(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(ComputeIntegral(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HFloatConstant* x, HFloatConstant* y) const override {
    return GetBlock()->GetGraph()->GetFloatConstant(ComputeFP(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HDoubleConstant* x, HDoubleConstant* y) const override {
    return GetBlock()->GetGraph()->GetDoubleConstant(ComputeFP(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Rem);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Rem);
};

class HMin final : public HBinaryOperation {
 public:
  HMin(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc)
      : HBinaryOperation(kMin, result_type, left, right, SideEffects::None(), dex_pc) {}

  bool IsCommutative() const override { return true; }

  // Evaluation for integral values.
  template <typename T> static T ComputeIntegral(T x, T y) {
    return (x <= y) ? x : y;
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(ComputeIntegral(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(ComputeIntegral(x->GetValue(), y->GetValue()));
  }
  // TODO: Evaluation for floating-point values.
  HConstant* Evaluate([[maybe_unused]] HFloatConstant* x,
                      [[maybe_unused]] HFloatConstant* y) const override {
    return nullptr;
  }
  HConstant* Evaluate([[maybe_unused]] HDoubleConstant* x,
                      [[maybe_unused]] HDoubleConstant* y) const override {
    return nullptr;
  }

  DECLARE_INSTRUCTION(Min);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Min);
};

class HMax final : public HBinaryOperation {
 public:
  HMax(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc)
      : HBinaryOperation(kMax, result_type, left, right, SideEffects::None(), dex_pc) {}

  bool IsCommutative() const override { return true; }

  // Evaluation for integral values.
  template <typename T> static T ComputeIntegral(T x, T y) {
    return (x >= y) ? x : y;
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(ComputeIntegral(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(ComputeIntegral(x->GetValue(), y->GetValue()));
  }
  // TODO: Evaluation for floating-point values.
  HConstant* Evaluate([[maybe_unused]] HFloatConstant* x,
                      [[maybe_unused]] HFloatConstant* y) const override {
    return nullptr;
  }
  HConstant* Evaluate([[maybe_unused]] HDoubleConstant* x,
                      [[maybe_unused]] HDoubleConstant* y) const override {
    return nullptr;
  }

  DECLARE_INSTRUCTION(Max);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Max);
};

class HAbs final : public HUnaryOperation {
 public:
  HAbs(DataType::Type result_type, HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HUnaryOperation(kAbs, result_type, input, dex_pc) {}

  // Evaluation for integral values.
  template <typename T> static T ComputeIntegral(T x) {
    return x < 0 ? -x : x;
  }

  // Evaluation for floating-point values.
  // Note, as a "quality of implementation", rather than pure "spec compliance",
  // we require that Math.abs() clears the sign bit (but changes nothing else)
  // for all floating-point numbers, including NaN (signaling NaN may become quiet though).
  // http://b/30758343
  template <typename T, typename S> static T ComputeFP(T x) {
    S bits = bit_cast<S, T>(x);
    return bit_cast<T, S>(bits & std::numeric_limits<S>::max());
  }

  HConstant* Evaluate(HIntConstant* x) const override {
    return GetBlock()->GetGraph()->GetIntConstant(ComputeIntegral(x->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x) const override {
    return GetBlock()->GetGraph()->GetLongConstant(ComputeIntegral(x->GetValue()));
  }
  HConstant* Evaluate(HFloatConstant* x) const override {
    return GetBlock()->GetGraph()->GetFloatConstant(ComputeFP<float, int32_t>(x->GetValue()));
  }
  HConstant* Evaluate(HDoubleConstant* x) const override {
    return GetBlock()->GetGraph()->GetDoubleConstant(ComputeFP<double, int64_t>(x->GetValue()));
  }

  DECLARE_INSTRUCTION(Abs);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Abs);
};

class HDivZeroCheck final : public HExpression<1> {
 public:
  // `HDivZeroCheck` can trigger GC, as it may call the `ArithmeticException`
  // constructor. However it can only do it on a fatal slow path so execution never returns to the
  // instruction following the current one; thus 'SideEffects::None()' is used.
  HDivZeroCheck(HInstruction* value, uint32_t dex_pc)
      : HExpression(kDivZeroCheck, value->GetType(), SideEffects::None(), dex_pc) {
    SetRawInputAt(0, value);
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }

  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  bool NeedsEnvironment() const override { return true; }
  bool CanThrow() const override { return true; }

  DECLARE_INSTRUCTION(DivZeroCheck);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(DivZeroCheck);
};

class HShl final : public HBinaryOperation {
 public:
  HShl(DataType::Type result_type,
       HInstruction* value,
       HInstruction* distance,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kShl, result_type, value, distance, SideEffects::None(), dex_pc) {
    DCHECK_EQ(result_type, DataType::Kind(value->GetType()));
    DCHECK_EQ(DataType::Type::kInt32, DataType::Kind(distance->GetType()));
  }

  template <typename T>
  static T Compute(T value, int32_t distance, int32_t max_shift_distance) {
    return value << (distance & max_shift_distance);
  }

  HConstant* Evaluate(HIntConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxIntShiftDistance));
  }
  HConstant* Evaluate(HLongConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxLongShiftDistance));
  }

  DECLARE_INSTRUCTION(Shl);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Shl);
};

class HShr final : public HBinaryOperation {
 public:
  HShr(DataType::Type result_type,
       HInstruction* value,
       HInstruction* distance,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kShr, result_type, value, distance, SideEffects::None(), dex_pc) {
    DCHECK_EQ(result_type, DataType::Kind(value->GetType()));
    DCHECK_EQ(DataType::Type::kInt32, DataType::Kind(distance->GetType()));
  }

  template <typename T>
  static T Compute(T value, int32_t distance, int32_t max_shift_distance) {
    return value >> (distance & max_shift_distance);
  }

  HConstant* Evaluate(HIntConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxIntShiftDistance));
  }
  HConstant* Evaluate(HLongConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxLongShiftDistance));
  }

  DECLARE_INSTRUCTION(Shr);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Shr);
};

class HUShr final : public HBinaryOperation {
 public:
  HUShr(DataType::Type result_type,
        HInstruction* value,
        HInstruction* distance,
        uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kUShr, result_type, value, distance, SideEffects::None(), dex_pc) {
    DCHECK_EQ(result_type, DataType::Kind(value->GetType()));
    DCHECK_EQ(DataType::Type::kInt32, DataType::Kind(distance->GetType()));
  }

  template <typename T>
  static T Compute(T value, int32_t distance, int32_t max_shift_distance) {
    using V = std::make_unsigned_t<T>;
    V ux = static_cast<V>(value);
    return static_cast<T>(ux >> (distance & max_shift_distance));
  }

  HConstant* Evaluate(HIntConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxIntShiftDistance));
  }
  HConstant* Evaluate(HLongConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxLongShiftDistance));
  }

  DECLARE_INSTRUCTION(UShr);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(UShr);
};

class HAnd final : public HBinaryOperation {
 public:
  HAnd(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kAnd, result_type, left, right, SideEffects::None(), dex_pc) {
  }

  bool IsCommutative() const override { return true; }

  template <typename T> static T Compute(T x, T y) { return x & y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(And);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(And);
};

class HOr final : public HBinaryOperation {
 public:
  HOr(DataType::Type result_type,
      HInstruction* left,
      HInstruction* right,
      uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kOr, result_type, left, right, SideEffects::None(), dex_pc) {
  }

  bool IsCommutative() const override { return true; }

  template <typename T> static T Compute(T x, T y) { return x | y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Or);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Or);
};

class HXor final : public HBinaryOperation {
 public:
  HXor(DataType::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(kXor, result_type, left, right, SideEffects::None(), dex_pc) {
  }

  bool IsCommutative() const override { return true; }

  template <typename T> static T Compute(T x, T y) { return x ^ y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue(), y->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue(), y->GetValue()));
  }

  DECLARE_INSTRUCTION(Xor);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Xor);
};

class HRor final : public HBinaryOperation {
 public:
  HRor(DataType::Type result_type, HInstruction* value, HInstruction* distance)
      : HBinaryOperation(kRor, result_type, value, distance) {
  }

  template <typename T>
  static T Compute(T value, int32_t distance, int32_t max_shift_value) {
    using V = std::make_unsigned_t<T>;
    V ux = static_cast<V>(value);
    if ((distance & max_shift_value) == 0) {
      return static_cast<T>(ux);
    } else {
      const V reg_bits = sizeof(T) * 8;
      return static_cast<T>(ux >> (distance & max_shift_value)) |
                           (value << (reg_bits - (distance & max_shift_value)));
    }
  }

  HConstant* Evaluate(HIntConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxIntShiftDistance));
  }
  HConstant* Evaluate(HLongConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxLongShiftDistance));
  }

  DECLARE_INSTRUCTION(Ror);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Ror);
};

class HRol final : public HBinaryOperation {
 public:
  HRol(DataType::Type result_type, HInstruction* value, HInstruction* distance)
      : HBinaryOperation(kRol, result_type, value, distance) {}

  template <typename T>
  static T Compute(T value, int32_t distance, int32_t max_shift_value) {
    return HRor::Compute(value, -distance, max_shift_value);
  }

  HConstant* Evaluate(HIntConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxIntShiftDistance));
  }
  HConstant* Evaluate(HLongConstant* value, HIntConstant* distance) const override {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(value->GetValue(), distance->GetValue(), kMaxLongShiftDistance));
  }

  DECLARE_INSTRUCTION(Rol);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Rol);
};

// The value of a parameter in this method. Its location depends on
// the calling convention.
class HParameterValue final : public HExpression<0> {
 public:
  HParameterValue(const DexFile& dex_file,
                  dex::TypeIndex type_index,
                  uint8_t index,
                  DataType::Type parameter_type,
                  bool is_this = false)
      : HExpression(kParameterValue, parameter_type, SideEffects::None(), kNoDexPc),
        dex_file_(dex_file),
        type_index_(type_index),
        index_(index) {
    SetPackedFlag<kFlagIsThis>(is_this);
    SetPackedFlag<kFlagCanBeNull>(!is_this);
  }

  const DexFile& GetDexFile() const { return dex_file_; }
  dex::TypeIndex GetTypeIndex() const { return type_index_; }
  uint8_t GetIndex() const { return index_; }
  bool IsThis() const { return GetPackedFlag<kFlagIsThis>(); }

  bool CanBeNull() const override { return GetPackedFlag<kFlagCanBeNull>(); }
  void SetCanBeNull(bool can_be_null) { SetPackedFlag<kFlagCanBeNull>(can_be_null); }

  DECLARE_INSTRUCTION(ParameterValue);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ParameterValue);

 private:
  // Whether or not the parameter value corresponds to 'this' argument.
  static constexpr size_t kFlagIsThis = kNumberOfGenericPackedBits;
  static constexpr size_t kFlagCanBeNull = kFlagIsThis + 1;
  static constexpr size_t kNumberOfParameterValuePackedBits = kFlagCanBeNull + 1;
  static_assert(kNumberOfParameterValuePackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");

  const DexFile& dex_file_;
  const dex::TypeIndex type_index_;
  // The index of this parameter in the parameters list. Must be less
  // than HGraph::number_of_in_vregs_.
  const uint8_t index_;
};

class HNot final : public HUnaryOperation {
 public:
  HNot(DataType::Type result_type, HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HUnaryOperation(kNot, result_type, input, dex_pc) {
  }

  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  template <typename T> static T Compute(T x) { return ~x; }

  HConstant* Evaluate(HIntConstant* x) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue()));
  }
  HConstant* Evaluate(HLongConstant* x) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue()));
  }

  DECLARE_INSTRUCTION(Not);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Not);
};

class HBooleanNot final : public HUnaryOperation {
 public:
  explicit HBooleanNot(HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HUnaryOperation(kBooleanNot, DataType::Type::kBool, input, dex_pc) {
  }

  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  template <typename T> static bool Compute(T x) {
    DCHECK(IsUint<1>(x)) << x;
    return !x;
  }

  HConstant* Evaluate(HIntConstant* x) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue()));
  }

  DECLARE_INSTRUCTION(BooleanNot);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(BooleanNot);
};

class HTypeConversion final : public HExpression<1> {
 public:
  // Instantiate a type conversion of `input` to `result_type`.
  HTypeConversion(DataType::Type result_type, HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HExpression(kTypeConversion, result_type, SideEffects::None(), dex_pc) {
    SetRawInputAt(0, input);
    // Invariant: We should never generate a conversion to a Boolean value.
    DCHECK_NE(DataType::Type::kBool, result_type);
  }

  HInstruction* GetInput() const { return InputAt(0); }
  DataType::Type GetInputType() const { return GetInput()->GetType(); }
  DataType::Type GetResultType() const { return GetType(); }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }
  // Return whether the conversion is implicit. This includes conversion to the same type.
  bool IsImplicitConversion() const {
    return DataType::IsTypeConversionImplicit(GetInputType(), GetResultType());
  }

  // Try to statically evaluate the conversion and return a HConstant
  // containing the result.  If the input cannot be converted, return nullptr.
  HConstant* TryStaticEvaluation() const;

  // Same but for `input` instead of GetInput().
  HConstant* TryStaticEvaluation(HInstruction* input) const;

  DECLARE_INSTRUCTION(TypeConversion);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(TypeConversion);
};

static constexpr uint32_t kNoRegNumber = -1;

class HNullCheck final : public HExpression<1> {
 public:
  // `HNullCheck` can trigger GC, as it may call the `NullPointerException`
  // constructor. However it can only do it on a fatal slow path so execution never returns to the
  // instruction following the current one; thus 'SideEffects::None()' is used.
  HNullCheck(HInstruction* value, uint32_t dex_pc)
      : HExpression(kNullCheck, value->GetType(), SideEffects::None(), dex_pc) {
    SetRawInputAt(0, value);
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  bool NeedsEnvironment() const override { return true; }

  bool CanThrow() const override { return true; }

  bool CanBeNull() const override { return false; }

  DECLARE_INSTRUCTION(NullCheck);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(NullCheck);
};

// Embeds an ArtField and all the information required by the compiler. We cache
// that information to avoid requiring the mutator lock every time we need it.
class FieldInfo : public ValueObject {
 public:
  FieldInfo(ArtField* field,
            MemberOffset field_offset,
            DataType::Type field_type,
            bool is_volatile,
            uint32_t index,
            uint16_t declaring_class_def_index,
            const DexFile& dex_file)
      : field_(field),
        field_offset_(field_offset),
        field_type_(field_type),
        is_volatile_(is_volatile),
        index_(index),
        declaring_class_def_index_(declaring_class_def_index),
        dex_file_(dex_file) {}

  ArtField* GetField() const { return field_; }
  MemberOffset GetFieldOffset() const { return field_offset_; }
  DataType::Type GetFieldType() const { return field_type_; }
  uint32_t GetFieldIndex() const { return index_; }
  uint16_t GetDeclaringClassDefIndex() const { return declaring_class_def_index_;}
  const DexFile& GetDexFile() const { return dex_file_; }
  bool IsVolatile() const { return is_volatile_; }

  bool Equals(const FieldInfo& other) const {
    return field_ == other.field_ &&
           field_offset_ == other.field_offset_ &&
           field_type_ == other.field_type_ &&
           is_volatile_ == other.is_volatile_ &&
           index_ == other.index_ &&
           declaring_class_def_index_ == other.declaring_class_def_index_ &&
           &dex_file_ == &other.dex_file_;
  }

  std::ostream& Dump(std::ostream& os) const {
    os << field_ << ", off: " << field_offset_ << ", type: " << field_type_
       << ", volatile: " << std::boolalpha << is_volatile_ << ", index_: " << std::dec << index_
       << ", declaring_class: " << declaring_class_def_index_ << ", dex: " << dex_file_;
    return os;
  }

 private:
  ArtField* const field_;
  const MemberOffset field_offset_;
  const DataType::Type field_type_;
  const bool is_volatile_;
  const uint32_t index_;
  const uint16_t declaring_class_def_index_;
  const DexFile& dex_file_;
};

inline bool operator==(const FieldInfo& a, const FieldInfo& b) {
  return a.Equals(b);
}

inline std::ostream& operator<<(std::ostream& os, const FieldInfo& a) {
  return a.Dump(os);
}

class HFieldAccess : public HInstruction {
 public:
  HFieldAccess(InstructionKind kind,
               SideEffects side_effects,
               ArtField* field,
               DataType::Type field_type,
               MemberOffset field_offset,
               bool is_volatile,
               uint32_t field_idx,
               uint16_t declaring_class_def_index,
               const DexFile& dex_file,
               uint32_t dex_pc)
      : HInstruction(kind, field_type, side_effects, dex_pc),
        field_info_(field,
                    field_offset,
                    field_type,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file) {}

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  DataType::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }

  DECLARE_ABSTRACT_INSTRUCTION(FieldAccess);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(FieldAccess);

 private:
  const FieldInfo field_info_;
};

class HInstanceFieldGet final : public HExpression<1, HFieldAccess> {
 public:
  HInstanceFieldGet(HInstruction* object,
                    ArtField* field,
                    DataType::Type field_type,
                    MemberOffset field_offset,
                    bool is_volatile,
                    uint32_t field_idx,
                    uint16_t declaring_class_def_index,
                    const DexFile& dex_file,
                    uint32_t dex_pc)
      : HExpression(kInstanceFieldGet,
                    SideEffects::FieldReadOfType(field_type, is_volatile),
                    field,
                    field_type,
                    field_offset,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file,
                    dex_pc) {
    SetRawInputAt(0, object);
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return !IsVolatile(); }

  bool InstructionDataEquals(const HInstruction* other) const override {
    const HInstanceFieldGet* other_get = other->AsInstanceFieldGet();
    return GetFieldOffset().SizeValue() == other_get->GetFieldOffset().SizeValue();
  }

  bool CanDoImplicitNullCheckOn(HInstruction* obj) const override {
    return (obj == InputAt(0)) && art::CanDoImplicitNullCheckOn(GetFieldOffset().Uint32Value());
  }

  size_t ComputeHashCode() const override {
    return (HInstruction::ComputeHashCode() << 7) | GetFieldOffset().SizeValue();
  }

  void SetType(DataType::Type new_type) {
    DCHECK(DataType::IsIntegralType(GetType()));
    DCHECK(DataType::IsIntegralType(new_type));
    DCHECK_EQ(DataType::Size(GetType()), DataType::Size(new_type));
    SetPackedField<TypeField>(new_type);
  }

  DECLARE_INSTRUCTION(InstanceFieldGet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(InstanceFieldGet);
};

enum class WriteBarrierKind {
  // Emit the write barrier. This write barrier is not being relied on so e.g. codegen can decide to
  // skip it if the value stored is null. This is the default behavior.
  kEmitNotBeingReliedOn,
  // Emit the write barrier. This write barrier is being relied on and must be emitted.
  kEmitBeingReliedOn,
  // Skip emitting the write barrier. This could be set because:
  //  A) The write barrier is not needed (i.e. it is not a reference, or the value is the null
  //  constant)
  //  B) This write barrier was coalesced into another one so there's no need to emit it.
  kDontEmit,
  kLast = kDontEmit
};
std::ostream& operator<<(std::ostream& os, WriteBarrierKind rhs);

class HInstanceFieldSet final : public HExpression<2, HFieldAccess> {
 public:
  HInstanceFieldSet(HInstruction* object,
                    HInstruction* value,
                    ArtField* field,
                    DataType::Type field_type,
                    MemberOffset field_offset,
                    bool is_volatile,
                    uint32_t field_idx,
                    uint16_t declaring_class_def_index,
                    const DexFile& dex_file,
                    uint32_t dex_pc)
      : HExpression(kInstanceFieldSet,
                    SideEffects::FieldWriteOfType(field_type, is_volatile),
                    field,
                    field_type,
                    field_offset,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file,
                    dex_pc) {
    SetPackedFlag<kFlagValueCanBeNull>(true);
    SetPackedField<WriteBarrierKindField>(
        field_type == DataType::Type::kReference
            ? WriteBarrierKind::kEmitNotBeingReliedOn
            : WriteBarrierKind::kDontEmit);
    SetRawInputAt(0, object);
    SetRawInputAt(1, value);
  }

  bool IsClonable() const override { return true; }

  bool CanDoImplicitNullCheckOn(HInstruction* obj) const override {
    return (obj == InputAt(0)) && art::CanDoImplicitNullCheckOn(GetFieldOffset().Uint32Value());
  }

  HInstruction* GetValue() const { return InputAt(1); }
  bool GetValueCanBeNull() const { return GetPackedFlag<kFlagValueCanBeNull>(); }
  void ClearValueCanBeNull() { SetPackedFlag<kFlagValueCanBeNull>(false); }
  WriteBarrierKind GetWriteBarrierKind() { return GetPackedField<WriteBarrierKindField>(); }
  void SetWriteBarrierKind(WriteBarrierKind kind) {
    DCHECK(kind != WriteBarrierKind::kEmitNotBeingReliedOn)
        << "We shouldn't go back to the original value.";
    DCHECK_IMPLIES(kind == WriteBarrierKind::kDontEmit,
                   GetWriteBarrierKind() != WriteBarrierKind::kEmitBeingReliedOn)
        << "If a write barrier was relied on by other write barriers, we cannot skip emitting it.";
    SetPackedField<WriteBarrierKindField>(kind);
  }

  DECLARE_INSTRUCTION(InstanceFieldSet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(InstanceFieldSet);

 private:
  static constexpr size_t kFlagValueCanBeNull = kNumberOfGenericPackedBits;
  static constexpr size_t kWriteBarrierKind = kFlagValueCanBeNull + 1;
  static constexpr size_t kWriteBarrierKindSize =
      MinimumBitsToStore(static_cast<size_t>(WriteBarrierKind::kLast));
  static constexpr size_t kNumberOfInstanceFieldSetPackedBits =
      kWriteBarrierKind + kWriteBarrierKindSize;
  static_assert(kNumberOfInstanceFieldSetPackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");

  using WriteBarrierKindField =
      BitField<WriteBarrierKind, kWriteBarrierKind, kWriteBarrierKindSize>;
};

class HArrayGet final : public HExpression<2> {
 public:
  HArrayGet(HInstruction* array,
            HInstruction* index,
            DataType::Type type,
            uint32_t dex_pc)
      : HArrayGet(array,
                  index,
                  type,
                  SideEffects::ArrayReadOfType(type),
                  dex_pc,
                  /* is_string_char_at= */ false) {
  }

  HArrayGet(HInstruction* array,
            HInstruction* index,
            DataType::Type type,
            SideEffects side_effects,
            uint32_t dex_pc,
            bool is_string_char_at)
      : HExpression(kArrayGet, type, side_effects, dex_pc) {
    SetPackedFlag<kFlagIsStringCharAt>(is_string_char_at);
    SetRawInputAt(0, array);
    SetRawInputAt(1, index);
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }
  bool CanDoImplicitNullCheckOn([[maybe_unused]] HInstruction* obj) const override {
    // TODO: We can be smarter here.
    // Currently, unless the array is the result of NewArray, the array access is always
    // preceded by some form of null NullCheck necessary for the bounds check, usually
    // implicit null check on the ArrayLength input to BoundsCheck or Deoptimize for
    // dynamic BCE. There are cases when these could be removed to produce better code.
    // If we ever add optimizations to do so we should allow an implicit check here
    // (as long as the address falls in the first page).
    //
    // As an example of such fancy optimization, we could eliminate BoundsCheck for
    //     a = cond ? new int[1] : null;
    //     a[0];  // The Phi does not need bounds check for either input.
    return false;
  }

  bool IsEquivalentOf(HArrayGet* other) const {
    bool result = (GetDexPc() == other->GetDexPc());
    if (kIsDebugBuild && result) {
      DCHECK_EQ(GetBlock(), other->GetBlock());
      DCHECK_EQ(GetArray(), other->GetArray());
      DCHECK_EQ(GetIndex(), other->GetIndex());
      if (DataType::IsIntOrLongType(GetType())) {
        DCHECK(DataType::IsFloatingPointType(other->GetType())) << other->GetType();
      } else {
        DCHECK(DataType::IsFloatingPointType(GetType())) << GetType();
        DCHECK(DataType::IsIntOrLongType(other->GetType())) << other->GetType();
      }
    }
    return result;
  }

  bool IsStringCharAt() const { return GetPackedFlag<kFlagIsStringCharAt>(); }

  HInstruction* GetArray() const { return InputAt(0); }
  HInstruction* GetIndex() const { return InputAt(1); }

  void SetType(DataType::Type new_type) {
    DCHECK(DataType::IsIntegralType(GetType()));
    DCHECK(DataType::IsIntegralType(new_type));
    DCHECK_EQ(DataType::Size(GetType()), DataType::Size(new_type));
    SetPackedField<TypeField>(new_type);
  }

  DECLARE_INSTRUCTION(ArrayGet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ArrayGet);

 private:
  // We treat a String as an array, creating the HArrayGet from String.charAt()
  // intrinsic in the instruction simplifier. We can always determine whether
  // a particular HArrayGet is actually a String.charAt() by looking at the type
  // of the input but that requires holding the mutator lock, so we prefer to use
  // a flag, so that code generators don't need to do the locking.
  static constexpr size_t kFlagIsStringCharAt = kNumberOfGenericPackedBits;
  static constexpr size_t kNumberOfArrayGetPackedBits = kFlagIsStringCharAt + 1;
  static_assert(kNumberOfArrayGetPackedBits <= HInstruction::kMaxNumberOfPackedBits,
                "Too many packed fields.");
};

class HArraySet final : public HExpression<3> {
 public:
  HArraySet(HInstruction* array,
            HInstruction* index,
            HInstruction* value,
            DataType::Type expected_component_type,
            uint32_t dex_pc)
      : HArraySet(array,
                  index,
                  value,
                  expected_component_type,
                  // Make a best guess for side effects now, may be refined during SSA building.
                  ComputeSideEffects(GetComponentType(value->GetType(), expected_component_type)),
                  dex_pc) {
  }

  HArraySet(HInstruction* array,
            HInstruction* index,
            HInstruction* value,
            DataType::Type expected_component_type,
            SideEffects side_effects,
            uint32_t dex_pc)
      : HExpression(kArraySet, side_effects, dex_pc) {
    SetPackedField<ExpectedComponentTypeField>(expected_component_type);
    SetPackedFlag<kFlagNeedsTypeCheck>(value->GetType() == DataType::Type::kReference);
    SetPackedFlag<kFlagValueCanBeNull>(true);
    SetPackedFlag<kFlagStaticTypeOfArrayIsObjectArray>(false);
    SetPackedField<WriteBarrierKindField>(
        value->GetType() == DataType::Type::kReference
            ? WriteBarrierKind::kEmitNotBeingReliedOn
            : WriteBarrierKind::kDontEmit);
    SetRawInputAt(0, array);
    SetRawInputAt(1, index);
    SetRawInputAt(2, value);
  }

  bool IsClonable() const override { return true; }

  bool NeedsEnvironment() const override {
    // We call a runtime method to throw ArrayStoreException.
    return NeedsTypeCheck();
  }

  // Can throw ArrayStoreException.
  bool CanThrow() const override { return NeedsTypeCheck(); }

  bool CanDoImplicitNullCheckOn([[maybe_unused]] HInstruction* obj) const override {
    // TODO: Same as for ArrayGet.
    return false;
  }

  void ClearTypeCheck() {
    SetPackedFlag<kFlagNeedsTypeCheck>(false);
    // Clear the `CanTriggerGC` flag too as we can only trigger a GC when doing a type check.
    SetSideEffects(GetSideEffects().Exclusion(SideEffects::CanTriggerGC()));
    // Clear the environment too as we can only throw if we need a type check.
    RemoveEnvironment();
  }

  void ClearValueCanBeNull() {
    SetPackedFlag<kFlagValueCanBeNull>(false);
  }

  void SetStaticTypeOfArrayIsObjectArray() {
    SetPackedFlag<kFlagStaticTypeOfArrayIsObjectArray>(true);
  }

  bool GetValueCanBeNull() const { return GetPackedFlag<kFlagValueCanBeNull>(); }
  bool NeedsTypeCheck() const { return GetPackedFlag<kFlagNeedsTypeCheck>(); }
  bool StaticTypeOfArrayIsObjectArray() const {
    return GetPackedFlag<kFlagStaticTypeOfArrayIsObjectArray>();
  }

  HInstruction* GetArray() const { return InputAt(0); }
  HInstruction* GetIndex() const { return InputAt(1); }
  HInstruction* GetValue() const { return InputAt(2); }

  DataType::Type GetComponentType() const {
    return GetComponentType(GetValue()->GetType(), GetRawExpectedComponentType());
  }

  static DataType::Type GetComponentType(DataType::Type value_type,
                                         DataType::Type expected_component_type) {
    // The Dex format does not type floating point index operations. Since the
    // `expected_component_type` comes from SSA building and can therefore not
    // be correct, we also check what is the value type. If it is a floating
    // point type, we must use that type.
    return ((value_type == DataType::Type::kFloat32) || (value_type == DataType::Type::kFloat64))
        ? value_type
        : expected_component_type;
  }

  DataType::Type GetRawExpectedComponentType() const {
    return GetPackedField<ExpectedComponentTypeField>();
  }

  static SideEffects ComputeSideEffects(DataType::Type type) {
    return SideEffects::ArrayWriteOfType(type).Union(SideEffectsForArchRuntimeCalls(type));
  }

  static SideEffects SideEffectsForArchRuntimeCalls(DataType::Type value_type) {
    return (value_type == DataType::Type::kReference) ? SideEffects::CanTriggerGC()
                                                      : SideEffects::None();
  }

  WriteBarrierKind GetWriteBarrierKind() { return GetPackedField<WriteBarrierKindField>(); }

  void SetWriteBarrierKind(WriteBarrierKind kind) {
    DCHECK(kind != WriteBarrierKind::kEmitNotBeingReliedOn)
        << "We shouldn't go back to the original value.";
    DCHECK_IMPLIES(kind == WriteBarrierKind::kDontEmit,
                   GetWriteBarrierKind() != WriteBarrierKind::kEmitBeingReliedOn)
        << "If a write barrier was relied on by other write barriers, we cannot skip emitting it.";
    SetPackedField<WriteBarrierKindField>(kind);
  }

  DECLARE_INSTRUCTION(ArraySet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ArraySet);

 private:
  static constexpr size_t kFieldExpectedComponentType = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldExpectedComponentTypeSize =
      MinimumBitsToStore(static_cast<size_t>(DataType::Type::kLast));
  static constexpr size_t kFlagNeedsTypeCheck =
      kFieldExpectedComponentType + kFieldExpectedComponentTypeSize;
  static constexpr size_t kFlagValueCanBeNull = kFlagNeedsTypeCheck + 1;
  // Cached information for the reference_type_info_ so that codegen
  // does not need to inspect the static type.
  static constexpr size_t kFlagStaticTypeOfArrayIsObjectArray = kFlagValueCanBeNull + 1;
  static constexpr size_t kWriteBarrierKind = kFlagStaticTypeOfArrayIsObjectArray + 1;
  static constexpr size_t kWriteBarrierKindSize =
      MinimumBitsToStore(static_cast<size_t>(WriteBarrierKind::kLast));
  static constexpr size_t kNumberOfArraySetPackedBits = kWriteBarrierKind + kWriteBarrierKindSize;
  static_assert(kNumberOfArraySetPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
  using ExpectedComponentTypeField =
      BitField<DataType::Type, kFieldExpectedComponentType, kFieldExpectedComponentTypeSize>;

  using WriteBarrierKindField =
      BitField<WriteBarrierKind, kWriteBarrierKind, kWriteBarrierKindSize>;
};

class HArrayLength final : public HExpression<1> {
 public:
  HArrayLength(HInstruction* array, uint32_t dex_pc, bool is_string_length = false)
      : HExpression(kArrayLength, DataType::Type::kInt32, SideEffects::None(), dex_pc) {
    SetPackedFlag<kFlagIsStringLength>(is_string_length);
    // Note that arrays do not change length, so the instruction does not
    // depend on any write.
    SetRawInputAt(0, array);
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }
  bool CanDoImplicitNullCheckOn(HInstruction* obj) const override {
    return obj == InputAt(0);
  }

  bool IsStringLength() const { return GetPackedFlag<kFlagIsStringLength>(); }

  DECLARE_INSTRUCTION(ArrayLength);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ArrayLength);

 private:
  // We treat a String as an array, creating the HArrayLength from String.length()
  // or String.isEmpty() intrinsic in the instruction simplifier. We can always
  // determine whether a particular HArrayLength is actually a String.length() by
  // looking at the type of the input but that requires holding the mutator lock, so
  // we prefer to use a flag, so that code generators don't need to do the locking.
  static constexpr size_t kFlagIsStringLength = kNumberOfGenericPackedBits;
  static constexpr size_t kNumberOfArrayLengthPackedBits = kFlagIsStringLength + 1;
  static_assert(kNumberOfArrayLengthPackedBits <= HInstruction::kMaxNumberOfPackedBits,
                "Too many packed fields.");
};

class HBoundsCheck final : public HExpression<2> {
 public:
  // `HBoundsCheck` can trigger GC, as it may call the `IndexOutOfBoundsException`
  // constructor. However it can only do it on a fatal slow path so execution never returns to the
  // instruction following the current one; thus 'SideEffects::None()' is used.
  HBoundsCheck(HInstruction* index,
               HInstruction* length,
               uint32_t dex_pc,
               bool is_string_char_at = false)
      : HExpression(kBoundsCheck, index->GetType(), SideEffects::None(), dex_pc) {
    DCHECK_EQ(DataType::Type::kInt32, DataType::Kind(index->GetType()));
    SetPackedFlag<kFlagIsStringCharAt>(is_string_char_at);
    SetRawInputAt(0, index);
    SetRawInputAt(1, length);
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  bool NeedsEnvironment() const override { return true; }

  bool CanThrow() const override { return true; }

  bool IsStringCharAt() const { return GetPackedFlag<kFlagIsStringCharAt>(); }

  HInstruction* GetIndex() const { return InputAt(0); }

  DECLARE_INSTRUCTION(BoundsCheck);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(BoundsCheck);

 private:
  static constexpr size_t kFlagIsStringCharAt = kNumberOfGenericPackedBits;
  static constexpr size_t kNumberOfBoundsCheckPackedBits = kFlagIsStringCharAt + 1;
  static_assert(kNumberOfBoundsCheckPackedBits <= HInstruction::kMaxNumberOfPackedBits,
                "Too many packed fields.");
};

class HSuspendCheck final : public HExpression<0> {
 public:
  explicit HSuspendCheck(uint32_t dex_pc = kNoDexPc, bool is_no_op = false)
      : HExpression(kSuspendCheck, SideEffects::CanTriggerGC(), dex_pc),
        slow_path_(nullptr) {
    SetPackedFlag<kFlagIsNoOp>(is_no_op);
  }

  bool IsClonable() const override { return true; }

  bool NeedsEnvironment() const override {
    return true;
  }

  void SetIsNoOp(bool is_no_op) { SetPackedFlag<kFlagIsNoOp>(is_no_op); }
  bool IsNoOp() const { return GetPackedFlag<kFlagIsNoOp>(); }


  void SetSlowPath(SlowPathCode* slow_path) { slow_path_ = slow_path; }
  SlowPathCode* GetSlowPath() const { return slow_path_; }

  DECLARE_INSTRUCTION(SuspendCheck);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(SuspendCheck);

  // True if the HSuspendCheck should not emit any code during codegen. It is
  // not possible to simply remove this instruction to disable codegen, as
  // other optimizations (e.g: CHAGuardVisitor::HoistGuard) depend on
  // HSuspendCheck being present in every loop.
  static constexpr size_t kFlagIsNoOp = kNumberOfGenericPackedBits;
  static constexpr size_t kNumberOfSuspendCheckPackedBits = kFlagIsNoOp + 1;
  static_assert(kNumberOfSuspendCheckPackedBits <= HInstruction::kMaxNumberOfPackedBits,
                "Too many packed fields.");

 private:
  // Only used for code generation, in order to share the same slow path between back edges
  // of a same loop.
  SlowPathCode* slow_path_;
};

// Pseudo-instruction which doesn't generate any code.
// If `emit_environment` is true, it can be used to generate an environment. It is used, for
// example, to provide the native debugger with mapping information. It ensures that we can generate
// line number and local variables at this point.
class HNop : public HExpression<0> {
 public:
  explicit HNop(uint32_t dex_pc, bool needs_environment)
      : HExpression<0>(kNop, SideEffects::None(), dex_pc), needs_environment_(needs_environment) {
  }

  bool NeedsEnvironment() const override {
    return needs_environment_;
  }

  DECLARE_INSTRUCTION(Nop);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Nop);

 private:
  bool needs_environment_;
};

/**
 * Instruction to load a Class object.
 */
class HLoadClass final : public HInstruction {
 public:
  // Determines how to load the Class.
  enum class LoadKind {
    // We cannot load this class. See HSharpening::SharpenLoadClass.
    kInvalid = -1,

    // Use the Class* from the method's own ArtMethod*.
    kReferrersClass,

    // Use PC-relative boot image Class* address that will be known at link time.
    // Used for boot image classes referenced by boot image code.
    kBootImageLinkTimePcRelative,

    // Load from a boot image entry in the .data.img.rel.ro using a PC-relative load.
    // Used for boot image classes referenced by apps in AOT-compiled code.
    kBootImageRelRo,

    // Load from an app image entry in the .data.img.rel.ro using a PC-relative load.
    // Used for app image classes referenced by apps in AOT-compiled code.
    kAppImageRelRo,

    // Load from an entry in the .bss section using a PC-relative load.
    // Used for classes outside boot image referenced by AOT-compiled app and boot image code.
    kBssEntry,

    // Load from an entry for public class in the .bss section using a PC-relative load.
    // Used for classes that were unresolved during AOT-compilation outside the literal
    // package of the compiling class. Such classes are accessible only if they are public
    // and the .bss entry shall therefore be filled only if the resolved class is public.
    kBssEntryPublic,

    // Load from an entry for package class in the .bss section using a PC-relative load.
    // Used for classes that were unresolved during AOT-compilation but within the literal
    // package of the compiling class. Such classes are accessible if they are public or
    // in the same package which, given the literal package match, requires only matching
    // defining class loader and the .bss entry shall therefore be filled only if at least
    // one of those conditions holds. Note that all code in an oat file belongs to classes
    // with the same defining class loader.
    kBssEntryPackage,

    // Use a known boot image Class* address, embedded in the code by the codegen.
    // Used for boot image classes referenced by apps in JIT-compiled code.
    kJitBootImageAddress,

    // Load from the root table associated with the JIT compiled method.
    kJitTableAddress,

    // Load using a simple runtime call. This is the fall-back load kind when
    // the codegen is unable to use another appropriate kind.
    kRuntimeCall,

    kLast = kRuntimeCall
  };

  HLoadClass(HCurrentMethod* current_method,
             dex::TypeIndex type_index,
             const DexFile& dex_file,
             Handle<mirror::Class> klass,
             bool is_referrers_class,
             uint32_t dex_pc,
             bool needs_access_check)
      : HInstruction(kLoadClass,
                     DataType::Type::kReference,
                     SideEffectsForArchRuntimeCalls(),
                     dex_pc),
        special_input_(HUserRecord<HInstruction*>(current_method)),
        type_index_(type_index),
        dex_file_(dex_file),
        klass_(klass) {
    // Referrers class should not need access check. We never inline unverified
    // methods so we can't possibly end up in this situation.
    DCHECK_IMPLIES(is_referrers_class, !needs_access_check);

    SetPackedField<LoadKindField>(
        is_referrers_class ? LoadKind::kReferrersClass : LoadKind::kRuntimeCall);
    SetPackedFlag<kFlagNeedsAccessCheck>(needs_access_check);
    SetPackedFlag<kFlagIsInImage>(false);
    SetPackedFlag<kFlagGenerateClInitCheck>(false);
    SetPackedFlag<kFlagValidLoadedClassRTI>(false);
  }

  bool IsClonable() const override { return true; }

  void SetLoadKind(LoadKind load_kind);

  LoadKind GetLoadKind() const {
    return GetPackedField<LoadKindField>();
  }

  bool HasPcRelativeLoadKind() const {
    return GetLoadKind() == LoadKind::kBootImageLinkTimePcRelative ||
           GetLoadKind() == LoadKind::kBootImageRelRo ||
           GetLoadKind() == LoadKind::kAppImageRelRo ||
           GetLoadKind() == LoadKind::kBssEntry ||
           GetLoadKind() == LoadKind::kBssEntryPublic ||
           GetLoadKind() == LoadKind::kBssEntryPackage;
  }

  bool CanBeMoved() const override { return true; }

  bool InstructionDataEquals(const HInstruction* other) const override;

  size_t ComputeHashCode() const override { return type_index_.index_; }

  bool CanBeNull() const override { return false; }

  bool NeedsEnvironment() const override {
    return CanCallRuntime();
  }
  bool NeedsBss() const override {
    LoadKind load_kind = GetLoadKind();
    return load_kind == LoadKind::kBssEntry ||
           load_kind == LoadKind::kBssEntryPublic ||
           load_kind == LoadKind::kBssEntryPackage;
  }

  void SetMustGenerateClinitCheck(bool generate_clinit_check) {
    SetPackedFlag<kFlagGenerateClInitCheck>(generate_clinit_check);
  }

  bool CanCallRuntime() const {
    return NeedsAccessCheck() ||
           MustGenerateClinitCheck() ||
           NeedsBss() ||
           GetLoadKind() == LoadKind::kRuntimeCall;
  }

  bool CanThrow() const override {
    return NeedsAccessCheck() ||
           MustGenerateClinitCheck() ||
           // If the class is in the boot or app image, the lookup in the runtime call cannot throw.
           ((GetLoadKind() == LoadKind::kRuntimeCall || NeedsBss()) && !IsInImage());
  }

  ReferenceTypeInfo GetLoadedClassRTI() {
    if (GetPackedFlag<kFlagValidLoadedClassRTI>()) {
      // Note: The is_exact flag from the return value should not be used.
      return ReferenceTypeInfo::CreateUnchecked(klass_, /* is_exact= */ true);
    } else {
      return ReferenceTypeInfo::CreateInvalid();
    }
  }

  // Loaded class RTI is marked as valid by RTP if the klass_ is admissible.
  void SetValidLoadedClassRTI() {
    DCHECK(klass_ != nullptr);
    SetPackedFlag<kFlagValidLoadedClassRTI>(true);
  }

  dex::TypeIndex GetTypeIndex() const { return type_index_; }
  const DexFile& GetDexFile() const { return dex_file_; }

  static SideEffects SideEffectsForArchRuntimeCalls() {
    return SideEffects::CanTriggerGC();
  }

  bool IsReferrersClass() const { return GetLoadKind() == LoadKind::kReferrersClass; }
  bool NeedsAccessCheck() const { return GetPackedFlag<kFlagNeedsAccessCheck>(); }
  bool IsInImage() const { return GetPackedFlag<kFlagIsInImage>(); }
  bool MustGenerateClinitCheck() const { return GetPackedFlag<kFlagGenerateClInitCheck>(); }

  bool MustResolveTypeOnSlowPath() const {
    // Check that this instruction has a slow path.
    LoadKind load_kind = GetLoadKind();
    DCHECK(load_kind != LoadKind::kRuntimeCall);  // kRuntimeCall calls on main path.
    bool must_resolve_type_on_slow_path =
       load_kind == LoadKind::kBssEntry ||
       load_kind == LoadKind::kBssEntryPublic ||
       load_kind == LoadKind::kBssEntryPackage;
    DCHECK(must_resolve_type_on_slow_path || MustGenerateClinitCheck());
    return must_resolve_type_on_slow_path;
  }

  void MarkInImage() {
    SetPackedFlag<kFlagIsInImage>(true);
  }

  void AddSpecialInput(HInstruction* special_input);

  using HInstruction::GetInputRecords;  // Keep the const version visible.
  ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() final {
    return ArrayRef<HUserRecord<HInstruction*>>(
        &special_input_, (special_input_.GetInstruction() != nullptr) ? 1u : 0u);
  }

  Handle<mirror::Class> GetClass() const {
    return klass_;
  }

  DECLARE_INSTRUCTION(LoadClass);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(LoadClass);

 private:
  static constexpr size_t kFlagNeedsAccessCheck    = kNumberOfGenericPackedBits;
  // Whether the type is in an image (boot image or app image).
  static constexpr size_t kFlagIsInImage           = kFlagNeedsAccessCheck + 1;
  // Whether this instruction must generate the initialization check.
  // Used for code generation.
  static constexpr size_t kFlagGenerateClInitCheck = kFlagIsInImage + 1;
  static constexpr size_t kFieldLoadKind           = kFlagGenerateClInitCheck + 1;
  static constexpr size_t kFieldLoadKindSize =
      MinimumBitsToStore(static_cast<size_t>(LoadKind::kLast));
  static constexpr size_t kFlagValidLoadedClassRTI = kFieldLoadKind + kFieldLoadKindSize;
  static constexpr size_t kNumberOfLoadClassPackedBits = kFlagValidLoadedClassRTI + 1;
  static_assert(kNumberOfLoadClassPackedBits < kMaxNumberOfPackedBits, "Too many packed fields.");
  using LoadKindField = BitField<LoadKind, kFieldLoadKind, kFieldLoadKindSize>;

  static bool HasTypeReference(LoadKind load_kind) {
    return load_kind == LoadKind::kReferrersClass ||
        load_kind == LoadKind::kBootImageLinkTimePcRelative ||
        load_kind == LoadKind::kAppImageRelRo ||
        load_kind == LoadKind::kBssEntry ||
        load_kind == LoadKind::kBssEntryPublic ||
        load_kind == LoadKind::kBssEntryPackage ||
        load_kind == LoadKind::kRuntimeCall;
  }

  void SetLoadKindInternal(LoadKind load_kind);

  // The special input is the HCurrentMethod for kRuntimeCall or kReferrersClass.
  // For other load kinds it's empty or possibly some architecture-specific instruction
  // for PC-relative loads, i.e. kBssEntry* or kBootImageLinkTimePcRelative.
  HUserRecord<HInstruction*> special_input_;

  // A type index and dex file where the class can be accessed. The dex file can be:
  // - The compiling method's dex file if the class is defined there too.
  // - The compiling method's dex file if the class is referenced there.
  // - The dex file where the class is defined. When the load kind can only be
  //   kBssEntry* or kRuntimeCall, we cannot emit code for this `HLoadClass`.
  const dex::TypeIndex type_index_;
  const DexFile& dex_file_;

  Handle<mirror::Class> klass_;
};
std::ostream& operator<<(std::ostream& os, HLoadClass::LoadKind rhs);

// Note: defined outside class to see operator<<(., HLoadClass::LoadKind).
inline void HLoadClass::SetLoadKind(LoadKind load_kind) {
  // The load kind should be determined before inserting the instruction to the graph.
  DCHECK(GetBlock() == nullptr);
  DCHECK(GetEnvironment() == nullptr);
  SetPackedField<LoadKindField>(load_kind);
  if (load_kind != LoadKind::kRuntimeCall && load_kind != LoadKind::kReferrersClass) {
    special_input_ = HUserRecord<HInstruction*>(nullptr);
  }
  if (!NeedsEnvironment()) {
    SetSideEffects(SideEffects::None());
  }
}

// Note: defined outside class to see operator<<(., HLoadClass::LoadKind).
inline void HLoadClass::AddSpecialInput(HInstruction* special_input) {
  // The special input is used for PC-relative loads on some architectures,
  // including literal pool loads, which are PC-relative too.
  DCHECK(GetLoadKind() == LoadKind::kBootImageLinkTimePcRelative ||
         GetLoadKind() == LoadKind::kBootImageRelRo ||
         GetLoadKind() == LoadKind::kAppImageRelRo ||
         GetLoadKind() == LoadKind::kBssEntry ||
         GetLoadKind() == LoadKind::kBssEntryPublic ||
         GetLoadKind() == LoadKind::kBssEntryPackage ||
         GetLoadKind() == LoadKind::kJitBootImageAddress) << GetLoadKind();
  DCHECK(special_input_.GetInstruction() == nullptr);
  special_input_ = HUserRecord<HInstruction*>(special_input);
  special_input->AddUseAt(GetBlock()->GetGraph()->GetAllocator(), this, 0);
}

class HLoadString final : public HInstruction {
 public:
  // Determines how to load the String.
  enum class LoadKind {
    // Use PC-relative boot image String* address that will be known at link time.
    // Used for boot image strings referenced by boot image code.
    kBootImageLinkTimePcRelative,

    // Load from a boot image entry in the .data.img.rel.ro using a PC-relative load.
    // Used for boot image strings referenced by apps in AOT-compiled code.
    kBootImageRelRo,

    // Load from an entry in the .bss section using a PC-relative load.
    // Used for strings outside boot image referenced by AOT-compiled app and boot image code.
    kBssEntry,

    // Use a known boot image String* address, embedded in the code by the codegen.
    // Used for boot image strings referenced by apps in JIT-compiled code.
    kJitBootImageAddress,

    // Load from the root table associated with the JIT compiled method.
    kJitTableAddress,

    // Load using a simple runtime call. This is the fall-back load kind when
    // the codegen is unable to use another appropriate kind.
    kRuntimeCall,

    kLast = kRuntimeCall,
  };

  HLoadString(HCurrentMethod* current_method,
              dex::StringIndex string_index,
              const DexFile& dex_file,
              uint32_t dex_pc)
      : HInstruction(kLoadString,
                     DataType::Type::kReference,
                     SideEffectsForArchRuntimeCalls(),
                     dex_pc),
        special_input_(HUserRecord<HInstruction*>(current_method)),
        string_index_(string_index),
        dex_file_(dex_file) {
    SetPackedField<LoadKindField>(LoadKind::kRuntimeCall);
  }

  bool IsClonable() const override { return true; }
  bool NeedsBss() const override {
    return GetLoadKind() == LoadKind::kBssEntry;
  }

  void SetLoadKind(LoadKind load_kind);

  LoadKind GetLoadKind() const {
    return GetPackedField<LoadKindField>();
  }

  bool HasPcRelativeLoadKind() const {
    return GetLoadKind() == LoadKind::kBootImageLinkTimePcRelative ||
           GetLoadKind() == LoadKind::kBootImageRelRo ||
           GetLoadKind() == LoadKind::kBssEntry;
  }

  const DexFile& GetDexFile() const {
    return dex_file_;
  }

  dex::StringIndex GetStringIndex() const {
    return string_index_;
  }

  Handle<mirror::String> GetString() const {
    return string_;
  }

  void SetString(Handle<mirror::String> str) {
    string_ = str;
  }

  bool CanBeMoved() const override { return true; }

  bool InstructionDataEquals(const HInstruction* other) const override;

  size_t ComputeHashCode() const override { return string_index_.index_; }

  // Will call the runtime if we need to load the string through
  // the dex cache and the string is not guaranteed to be there yet.
  bool NeedsEnvironment() const override {
    LoadKind load_kind = GetLoadKind();
    if (load_kind == LoadKind::kBootImageLinkTimePcRelative ||
        load_kind == LoadKind::kBootImageRelRo ||
        load_kind == LoadKind::kJitBootImageAddress ||
        load_kind == LoadKind::kJitTableAddress) {
      return false;
    }
    return true;
  }

  bool CanBeNull() const override { return false; }
  bool CanThrow() const override { return NeedsEnvironment(); }

  static SideEffects SideEffectsForArchRuntimeCalls() {
    return SideEffects::CanTriggerGC();
  }

  void AddSpecialInput(HInstruction* special_input);

  using HInstruction::GetInputRecords;  // Keep the const version visible.
  ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() final {
    return ArrayRef<HUserRecord<HInstruction*>>(
        &special_input_, (special_input_.GetInstruction() != nullptr) ? 1u : 0u);
  }

  DECLARE_INSTRUCTION(LoadString);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(LoadString);

 private:
  static constexpr size_t kFieldLoadKind = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldLoadKindSize =
      MinimumBitsToStore(static_cast<size_t>(LoadKind::kLast));
  static constexpr size_t kNumberOfLoadStringPackedBits = kFieldLoadKind + kFieldLoadKindSize;
  static_assert(kNumberOfLoadStringPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
  using LoadKindField = BitField<LoadKind, kFieldLoadKind, kFieldLoadKindSize>;

  void SetLoadKindInternal(LoadKind load_kind);

  // The special input is the HCurrentMethod for kRuntimeCall.
  // For other load kinds it's empty or possibly some architecture-specific instruction
  // for PC-relative loads, i.e. kBssEntry or kBootImageLinkTimePcRelative.
  HUserRecord<HInstruction*> special_input_;

  dex::StringIndex string_index_;
  const DexFile& dex_file_;

  Handle<mirror::String> string_;
};
std::ostream& operator<<(std::ostream& os, HLoadString::LoadKind rhs);

// Note: defined outside class to see operator<<(., HLoadString::LoadKind).
inline void HLoadString::SetLoadKind(LoadKind load_kind) {
  // The load kind should be determined before inserting the instruction to the graph.
  DCHECK(GetBlock() == nullptr);
  DCHECK(GetEnvironment() == nullptr);
  DCHECK_EQ(GetLoadKind(), LoadKind::kRuntimeCall);
  SetPackedField<LoadKindField>(load_kind);
  if (load_kind != LoadKind::kRuntimeCall) {
    special_input_ = HUserRecord<HInstruction*>(nullptr);
  }
  if (!NeedsEnvironment()) {
    SetSideEffects(SideEffects::None());
  }
}

// Note: defined outside class to see operator<<(., HLoadString::LoadKind).
inline void HLoadString::AddSpecialInput(HInstruction* special_input) {
  // The special input is used for PC-relative loads on some architectures,
  // including literal pool loads, which are PC-relative too.
  DCHECK(GetLoadKind() == LoadKind::kBootImageLinkTimePcRelative ||
         GetLoadKind() == LoadKind::kBootImageRelRo ||
         GetLoadKind() == LoadKind::kBssEntry ||
         GetLoadKind() == LoadKind::kJitBootImageAddress) << GetLoadKind();
  // HLoadString::GetInputRecords() returns an empty array at this point,
  // so use the GetInputRecords() from the base class to set the input record.
  DCHECK(special_input_.GetInstruction() == nullptr);
  special_input_ = HUserRecord<HInstruction*>(special_input);
  special_input->AddUseAt(GetBlock()->GetGraph()->GetAllocator(), this, 0);
}

class HLoadMethodHandle final : public HInstruction {
 public:
  HLoadMethodHandle(HCurrentMethod* current_method,
                    uint16_t method_handle_idx,
                    const DexFile& dex_file,
                    uint32_t dex_pc)
      : HInstruction(kLoadMethodHandle,
                     DataType::Type::kReference,
                     SideEffectsForArchRuntimeCalls(),
                     dex_pc),
        special_input_(HUserRecord<HInstruction*>(current_method)),
        method_handle_idx_(method_handle_idx),
        dex_file_(dex_file) {
  }

  using HInstruction::GetInputRecords;  // Keep the const version visible.
  ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() final {
    return ArrayRef<HUserRecord<HInstruction*>>(
        &special_input_, (special_input_.GetInstruction() != nullptr) ? 1u : 0u);
  }

  bool IsClonable() const override { return true; }

  uint16_t GetMethodHandleIndex() const { return method_handle_idx_; }

  const DexFile& GetDexFile() const { return dex_file_; }

  static SideEffects SideEffectsForArchRuntimeCalls() {
    return SideEffects::CanTriggerGC();
  }

  bool CanThrow() const override { return true; }

  bool NeedsEnvironment() const override { return true; }

  DECLARE_INSTRUCTION(LoadMethodHandle);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(LoadMethodHandle);

 private:
  // The special input is the HCurrentMethod for kRuntimeCall.
  HUserRecord<HInstruction*> special_input_;

  const uint16_t method_handle_idx_;
  const DexFile& dex_file_;
};

class HLoadMethodType final : public HInstruction {
 public:
  // Determines how to load the MethodType.
  enum class LoadKind {
    // Load from an entry in the .bss section using a PC-relative load.
    kBssEntry,
    // Load from the root table associated with the JIT compiled method.
    kJitTableAddress,
    // Load using a single runtime call.
    kRuntimeCall,

    kLast = kRuntimeCall,
  };

  HLoadMethodType(HCurrentMethod* current_method,
                  dex::ProtoIndex proto_index,
                  const DexFile& dex_file,
                  uint32_t dex_pc)
      : HInstruction(kLoadMethodType,
                     DataType::Type::kReference,
                     SideEffectsForArchRuntimeCalls(),
                     dex_pc),
        special_input_(HUserRecord<HInstruction*>(current_method)),
        proto_index_(proto_index),
        dex_file_(dex_file) {
    SetPackedField<LoadKindField>(LoadKind::kRuntimeCall);
  }

  using HInstruction::GetInputRecords;  // Keep the const version visible.
  ArrayRef<HUserRecord<HInstruction*>> GetInputRecords() final {
    return ArrayRef<HUserRecord<HInstruction*>>(
        &special_input_, (special_input_.GetInstruction() != nullptr) ? 1u : 0u);
  }

  bool IsClonable() const override { return true; }

  void SetLoadKind(LoadKind load_kind);

  LoadKind GetLoadKind() const {
    return GetPackedField<LoadKindField>();
  }

  dex::ProtoIndex GetProtoIndex() const { return proto_index_; }

  Handle<mirror::MethodType> GetMethodType() const { return method_type_; }

  void SetMethodType(Handle<mirror::MethodType> method_type) { method_type_ = method_type; }

  const DexFile& GetDexFile() const { return dex_file_; }

  static SideEffects SideEffectsForArchRuntimeCalls() {
    return SideEffects::CanTriggerGC();
  }

  bool CanThrow() const override { return true; }

  bool NeedsEnvironment() const override { return true; }

  DECLARE_INSTRUCTION(LoadMethodType);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(LoadMethodType);

 private:
  static constexpr size_t kFieldLoadKind = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldLoadKindSize =
      MinimumBitsToStore(static_cast<size_t>(LoadKind::kLast));
  static constexpr size_t kNumberOfLoadMethodTypePackedBits = kFieldLoadKind + kFieldLoadKindSize;
  static_assert(kNumberOfLoadMethodTypePackedBits <= kMaxNumberOfPackedBits,
      "Too many packed fields.");
  using LoadKindField = BitField<LoadKind, kFieldLoadKind, kFieldLoadKindSize>;

  // The special input is the HCurrentMethod for kRuntimeCall.
  HUserRecord<HInstruction*> special_input_;

  const dex::ProtoIndex proto_index_;
  const DexFile& dex_file_;

  Handle<mirror::MethodType> method_type_;
};

std::ostream& operator<<(std::ostream& os, HLoadMethodType::LoadKind rhs);

// Note: defined outside class to see operator<<(., HLoadMethodType::LoadKind).
inline void HLoadMethodType::SetLoadKind(LoadKind load_kind) {
  // The load kind should be determined before inserting the instruction to the graph.
  DCHECK(GetBlock() == nullptr);
  DCHECK(GetEnvironment() == nullptr);
  DCHECK_EQ(GetLoadKind(), LoadKind::kRuntimeCall);
  DCHECK_IMPLIES(GetLoadKind() == LoadKind::kJitTableAddress, GetMethodType() != nullptr);
  SetPackedField<LoadKindField>(load_kind);
}

/**
 * Performs an initialization check on its Class object input.
 */
class HClinitCheck final : public HExpression<1> {
 public:
  HClinitCheck(HLoadClass* constant, uint32_t dex_pc)
      : HExpression(
            kClinitCheck,
            DataType::Type::kReference,
            SideEffects::AllExceptGCDependency(),  // Assume write/read on all fields/arrays.
            dex_pc) {
    SetRawInputAt(0, constant);
  }
  // TODO: Make ClinitCheck clonable.
  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  bool NeedsEnvironment() const override {
    // May call runtime to initialize the class.
    return true;
  }

  bool CanThrow() const override { return true; }

  HLoadClass* GetLoadClass() const {
    DCHECK(InputAt(0)->IsLoadClass());
    return InputAt(0)->AsLoadClass();
  }

  DECLARE_INSTRUCTION(ClinitCheck);


 protected:
  DEFAULT_COPY_CONSTRUCTOR(ClinitCheck);
};

class HStaticFieldGet final : public HExpression<1, HFieldAccess> {
 public:
  HStaticFieldGet(HInstruction* cls,
                  ArtField* field,
                  DataType::Type field_type,
                  MemberOffset field_offset,
                  bool is_volatile,
                  uint32_t field_idx,
                  uint16_t declaring_class_def_index,
                  const DexFile& dex_file,
                  uint32_t dex_pc)
      : HExpression(kStaticFieldGet,
                    SideEffects::FieldReadOfType(field_type, is_volatile),
                    field,
                    field_type,
                    field_offset,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file,
                    dex_pc) {
    SetRawInputAt(0, cls);
  }


  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return !IsVolatile(); }

  bool InstructionDataEquals(const HInstruction* other) const override {
    const HStaticFieldGet* other_get = other->AsStaticFieldGet();
    return GetFieldOffset().SizeValue() == other_get->GetFieldOffset().SizeValue();
  }

  size_t ComputeHashCode() const override {
    return (HInstruction::ComputeHashCode() << 7) | GetFieldOffset().SizeValue();
  }

  void SetType(DataType::Type new_type) {
    DCHECK(DataType::IsIntegralType(GetType()));
    DCHECK(DataType::IsIntegralType(new_type));
    DCHECK_EQ(DataType::Size(GetType()), DataType::Size(new_type));
    SetPackedField<TypeField>(new_type);
  }

  DECLARE_INSTRUCTION(StaticFieldGet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(StaticFieldGet);
};

class HStaticFieldSet final : public HExpression<2, HFieldAccess> {
 public:
  HStaticFieldSet(HInstruction* cls,
                  HInstruction* value,
                  ArtField* field,
                  DataType::Type field_type,
                  MemberOffset field_offset,
                  bool is_volatile,
                  uint32_t field_idx,
                  uint16_t declaring_class_def_index,
                  const DexFile& dex_file,
                  uint32_t dex_pc)
      : HExpression(kStaticFieldSet,
                    SideEffects::FieldWriteOfType(field_type, is_volatile),
                    field,
                    field_type,
                    field_offset,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file,
                    dex_pc) {
    SetPackedFlag<kFlagValueCanBeNull>(true);
    SetPackedField<WriteBarrierKindField>(
        field_type == DataType::Type::kReference
            ? WriteBarrierKind::kEmitNotBeingReliedOn
            : WriteBarrierKind::kDontEmit);
    SetRawInputAt(0, cls);
    SetRawInputAt(1, value);
  }

  bool IsClonable() const override { return true; }

  HInstruction* GetValue() const { return InputAt(1); }
  bool GetValueCanBeNull() const { return GetPackedFlag<kFlagValueCanBeNull>(); }
  void ClearValueCanBeNull() { SetPackedFlag<kFlagValueCanBeNull>(false); }

  WriteBarrierKind GetWriteBarrierKind() { return GetPackedField<WriteBarrierKindField>(); }
  void SetWriteBarrierKind(WriteBarrierKind kind) {
    DCHECK(kind != WriteBarrierKind::kEmitNotBeingReliedOn)
        << "We shouldn't go back to the original value.";
    DCHECK_IMPLIES(kind == WriteBarrierKind::kDontEmit,
                   GetWriteBarrierKind() != WriteBarrierKind::kEmitBeingReliedOn)
        << "If a write barrier was relied on by other write barriers, we cannot skip emitting it.";
    SetPackedField<WriteBarrierKindField>(kind);
  }

  DECLARE_INSTRUCTION(StaticFieldSet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(StaticFieldSet);

 private:
  static constexpr size_t kFlagValueCanBeNull = kNumberOfGenericPackedBits;
  static constexpr size_t kWriteBarrierKind = kFlagValueCanBeNull + 1;
  static constexpr size_t kWriteBarrierKindSize =
      MinimumBitsToStore(static_cast<size_t>(WriteBarrierKind::kLast));
  static constexpr size_t kNumberOfStaticFieldSetPackedBits =
      kWriteBarrierKind + kWriteBarrierKindSize;
  static_assert(kNumberOfStaticFieldSetPackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");

  using WriteBarrierKindField =
      BitField<WriteBarrierKind, kWriteBarrierKind, kWriteBarrierKindSize>;
};

class HStringBuilderAppend final : public HVariableInputSizeInstruction {
 public:
  HStringBuilderAppend(HIntConstant* format,
                       uint32_t number_of_arguments,
                       uint32_t number_of_out_vregs,
                       bool has_fp_args,
                       ArenaAllocator* allocator,
                       uint32_t dex_pc)
      : HVariableInputSizeInstruction(
            kStringBuilderAppend,
            DataType::Type::kReference,
            SideEffects::CanTriggerGC().Union(
                // The runtime call may read memory from inputs. It never writes outside
                // of the newly allocated result object or newly allocated helper objects,
                // except for float/double arguments where we reuse thread-local helper objects.
                has_fp_args ? SideEffects::AllWritesAndReads() : SideEffects::AllReads()),
            dex_pc,
            allocator,
            number_of_arguments + /* format */ 1u,
            kArenaAllocInvokeInputs),
        number_of_out_vregs_(number_of_out_vregs) {
    DCHECK_GE(number_of_arguments, 1u);  // There must be something to append.
    SetRawInputAt(FormatIndex(), format);
  }

  void SetArgumentAt(size_t index, HInstruction* argument) {
    DCHECK_LE(index, GetNumberOfArguments());
    SetRawInputAt(index, argument);
  }

  // Return the number of arguments, excluding the format.
  size_t GetNumberOfArguments() const {
    DCHECK_GE(InputCount(), 1u);
    return InputCount() - 1u;
  }

  // Return the number of outgoing vregs.
  uint32_t GetNumberOfOutVRegs() const { return number_of_out_vregs_; }

  size_t FormatIndex() const {
    return GetNumberOfArguments();
  }

  HIntConstant* GetFormat() {
    return InputAt(FormatIndex())->AsIntConstant();
  }

  bool NeedsEnvironment() const override { return true; }

  bool CanThrow() const override { return true; }

  bool CanBeNull() const override { return false; }

  DECLARE_INSTRUCTION(StringBuilderAppend);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(StringBuilderAppend);

 private:
  uint32_t number_of_out_vregs_;
};

class HUnresolvedInstanceFieldGet final : public HExpression<1> {
 public:
  HUnresolvedInstanceFieldGet(HInstruction* obj,
                              DataType::Type field_type,
                              uint32_t field_index,
                              uint32_t dex_pc)
      : HExpression(kUnresolvedInstanceFieldGet,
                    field_type,
                    SideEffects::AllExceptGCDependency(),
                    dex_pc),
        field_index_(field_index) {
    SetRawInputAt(0, obj);
  }

  bool IsClonable() const override { return true; }
  bool NeedsEnvironment() const override { return true; }
  bool CanThrow() const override { return true; }

  DataType::Type GetFieldType() const { return GetType(); }
  uint32_t GetFieldIndex() const { return field_index_; }

  DECLARE_INSTRUCTION(UnresolvedInstanceFieldGet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(UnresolvedInstanceFieldGet);

 private:
  const uint32_t field_index_;
};

class HUnresolvedInstanceFieldSet final : public HExpression<2> {
 public:
  HUnresolvedInstanceFieldSet(HInstruction* obj,
                              HInstruction* value,
                              DataType::Type field_type,
                              uint32_t field_index,
                              uint32_t dex_pc)
      : HExpression(kUnresolvedInstanceFieldSet, SideEffects::AllExceptGCDependency(), dex_pc),
        field_index_(field_index) {
    SetPackedField<FieldTypeField>(field_type);
    DCHECK_EQ(DataType::Kind(field_type), DataType::Kind(value->GetType()));
    SetRawInputAt(0, obj);
    SetRawInputAt(1, value);
  }

  bool IsClonable() const override { return true; }
  bool NeedsEnvironment() const override { return true; }
  bool CanThrow() const override { return true; }

  DataType::Type GetFieldType() const { return GetPackedField<FieldTypeField>(); }
  uint32_t GetFieldIndex() const { return field_index_; }

  DECLARE_INSTRUCTION(UnresolvedInstanceFieldSet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(UnresolvedInstanceFieldSet);

 private:
  static constexpr size_t kFieldFieldType = HInstruction::kNumberOfGenericPackedBits;
  static constexpr size_t kFieldFieldTypeSize =
      MinimumBitsToStore(static_cast<size_t>(DataType::Type::kLast));
  static constexpr size_t kNumberOfUnresolvedStaticFieldSetPackedBits =
      kFieldFieldType + kFieldFieldTypeSize;
  static_assert(kNumberOfUnresolvedStaticFieldSetPackedBits <= HInstruction::kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using FieldTypeField = BitField<DataType::Type, kFieldFieldType, kFieldFieldTypeSize>;

  const uint32_t field_index_;
};

class HUnresolvedStaticFieldGet final : public HExpression<0> {
 public:
  HUnresolvedStaticFieldGet(DataType::Type field_type,
                            uint32_t field_index,
                            uint32_t dex_pc)
      : HExpression(kUnresolvedStaticFieldGet,
                    field_type,
                    SideEffects::AllExceptGCDependency(),
                    dex_pc),
        field_index_(field_index) {
  }

  bool IsClonable() const override { return true; }
  bool NeedsEnvironment() const override { return true; }
  bool CanThrow() const override { return true; }

  DataType::Type GetFieldType() const { return GetType(); }
  uint32_t GetFieldIndex() const { return field_index_; }

  DECLARE_INSTRUCTION(UnresolvedStaticFieldGet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(UnresolvedStaticFieldGet);

 private:
  const uint32_t field_index_;
};

class HUnresolvedStaticFieldSet final : public HExpression<1> {
 public:
  HUnresolvedStaticFieldSet(HInstruction* value,
                            DataType::Type field_type,
                            uint32_t field_index,
                            uint32_t dex_pc)
      : HExpression(kUnresolvedStaticFieldSet, SideEffects::AllExceptGCDependency(), dex_pc),
        field_index_(field_index) {
    SetPackedField<FieldTypeField>(field_type);
    DCHECK_EQ(DataType::Kind(field_type), DataType::Kind(value->GetType()));
    SetRawInputAt(0, value);
  }

  bool IsClonable() const override { return true; }
  bool NeedsEnvironment() const override { return true; }
  bool CanThrow() const override { return true; }

  DataType::Type GetFieldType() const { return GetPackedField<FieldTypeField>(); }
  uint32_t GetFieldIndex() const { return field_index_; }

  DECLARE_INSTRUCTION(UnresolvedStaticFieldSet);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(UnresolvedStaticFieldSet);

 private:
  static constexpr size_t kFieldFieldType = HInstruction::kNumberOfGenericPackedBits;
  static constexpr size_t kFieldFieldTypeSize =
      MinimumBitsToStore(static_cast<size_t>(DataType::Type::kLast));
  static constexpr size_t kNumberOfUnresolvedStaticFieldSetPackedBits =
      kFieldFieldType + kFieldFieldTypeSize;
  static_assert(kNumberOfUnresolvedStaticFieldSetPackedBits <= HInstruction::kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using FieldTypeField = BitField<DataType::Type, kFieldFieldType, kFieldFieldTypeSize>;

  const uint32_t field_index_;
};

// Implement the move-exception DEX instruction.
class HLoadException final : public HExpression<0> {
 public:
  explicit HLoadException(uint32_t dex_pc = kNoDexPc)
      : HExpression(kLoadException, DataType::Type::kReference, SideEffects::None(), dex_pc) {
  }

  bool CanBeNull() const override { return false; }

  DECLARE_INSTRUCTION(LoadException);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(LoadException);
};

// Implicit part of move-exception which clears thread-local exception storage.
// Must not be removed because the runtime expects the TLS to get cleared.
class HClearException final : public HExpression<0> {
 public:
  explicit HClearException(uint32_t dex_pc = kNoDexPc)
      : HExpression(kClearException, SideEffects::AllWrites(), dex_pc) {
  }

  DECLARE_INSTRUCTION(ClearException);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ClearException);
};

class HThrow final : public HExpression<1> {
 public:
  HThrow(HInstruction* exception, uint32_t dex_pc)
      : HExpression(kThrow, SideEffects::CanTriggerGC(), dex_pc) {
    SetRawInputAt(0, exception);
  }

  bool IsControlFlow() const override { return true; }

  bool NeedsEnvironment() const override { return true; }

  bool CanThrow() const override { return true; }

  bool AlwaysThrows() const override { return true; }

  DECLARE_INSTRUCTION(Throw);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Throw);
};

/**
 * Implementation strategies for the code generator of a HInstanceOf
 * or `HCheckCast`.
 */
enum class TypeCheckKind {  // private marker to avoid generate-operator-out.py from processing.
  kUnresolvedCheck,       // Check against an unresolved type.
  kExactCheck,            // Can do a single class compare.
  kClassHierarchyCheck,   // Can just walk the super class chain.
  kAbstractClassCheck,    // Can just walk the super class chain, starting one up.
  kInterfaceCheck,        // No optimization yet when checking against an interface.
  kArrayObjectCheck,      // Can just check if the array is not primitive.
  kArrayCheck,            // No optimization yet when checking against a generic array.
  kBitstringCheck,        // Compare the type check bitstring.
  kLast = kArrayCheck
};

std::ostream& operator<<(std::ostream& os, TypeCheckKind rhs);

// Note: HTypeCheckInstruction is just a helper class, not an abstract instruction with an
// `IsTypeCheckInstruction()`. (New virtual methods in the HInstruction class have a high cost.)
class HTypeCheckInstruction : public HVariableInputSizeInstruction {
 public:
  HTypeCheckInstruction(InstructionKind kind,
                        DataType::Type type,
                        HInstruction* object,
                        HInstruction* target_class_or_null,
                        TypeCheckKind check_kind,
                        Handle<mirror::Class> klass,
                        uint32_t dex_pc,
                        ArenaAllocator* allocator,
                        HIntConstant* bitstring_path_to_root,
                        HIntConstant* bitstring_mask,
                        SideEffects side_effects)
      : HVariableInputSizeInstruction(
          kind,
          type,
          side_effects,
          dex_pc,
          allocator,
          /* number_of_inputs= */ check_kind == TypeCheckKind::kBitstringCheck ? 4u : 2u,
          kArenaAllocTypeCheckInputs),
        klass_(klass) {
    SetPackedField<TypeCheckKindField>(check_kind);
    SetPackedFlag<kFlagMustDoNullCheck>(true);
    SetPackedFlag<kFlagValidTargetClassRTI>(false);
    SetRawInputAt(0, object);
    SetRawInputAt(1, target_class_or_null);
    DCHECK_EQ(check_kind == TypeCheckKind::kBitstringCheck, bitstring_path_to_root != nullptr);
    DCHECK_EQ(check_kind == TypeCheckKind::kBitstringCheck, bitstring_mask != nullptr);
    if (check_kind == TypeCheckKind::kBitstringCheck) {
      DCHECK(target_class_or_null->IsNullConstant());
      SetRawInputAt(2, bitstring_path_to_root);
      SetRawInputAt(3, bitstring_mask);
    } else {
      DCHECK(target_class_or_null->IsLoadClass());
    }
  }

  HLoadClass* GetTargetClass() const {
    DCHECK_NE(GetTypeCheckKind(), TypeCheckKind::kBitstringCheck);
    HInstruction* load_class = InputAt(1);
    DCHECK(load_class->IsLoadClass());
    return load_class->AsLoadClass();
  }

  uint32_t GetBitstringPathToRoot() const {
    DCHECK_EQ(GetTypeCheckKind(), TypeCheckKind::kBitstringCheck);
    HInstruction* path_to_root = InputAt(2);
    DCHECK(path_to_root->IsIntConstant());
    return static_cast<uint32_t>(path_to_root->AsIntConstant()->GetValue());
  }

  uint32_t GetBitstringMask() const {
    DCHECK_EQ(GetTypeCheckKind(), TypeCheckKind::kBitstringCheck);
    HInstruction* mask = InputAt(3);
    DCHECK(mask->IsIntConstant());
    return static_cast<uint32_t>(mask->AsIntConstant()->GetValue());
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }

  bool InstructionDataEquals(const HInstruction* other) const override {
    DCHECK(other->IsInstanceOf() || other->IsCheckCast()) << other->DebugName();
    return GetPackedFields() == down_cast<const HTypeCheckInstruction*>(other)->GetPackedFields();
  }

  bool MustDoNullCheck() const { return GetPackedFlag<kFlagMustDoNullCheck>(); }
  void ClearMustDoNullCheck() { SetPackedFlag<kFlagMustDoNullCheck>(false); }
  TypeCheckKind GetTypeCheckKind() const { return GetPackedField<TypeCheckKindField>(); }
  bool IsExactCheck() const { return GetTypeCheckKind() == TypeCheckKind::kExactCheck; }

  ReferenceTypeInfo GetTargetClassRTI() {
    if (GetPackedFlag<kFlagValidTargetClassRTI>()) {
      // Note: The is_exact flag from the return value should not be used.
      return ReferenceTypeInfo::CreateUnchecked(klass_, /* is_exact= */ true);
    } else {
      return ReferenceTypeInfo::CreateInvalid();
    }
  }

  // Target class RTI is marked as valid by RTP if the klass_ is admissible.
  void SetValidTargetClassRTI() {
    DCHECK(klass_ != nullptr);
    SetPackedFlag<kFlagValidTargetClassRTI>(true);
  }

  Handle<mirror::Class> GetClass() const {
    return klass_;
  }

 protected:
  DEFAULT_COPY_CONSTRUCTOR(TypeCheckInstruction);

 private:
  static constexpr size_t kFieldTypeCheckKind = kNumberOfGenericPackedBits;
  static constexpr size_t kFieldTypeCheckKindSize =
      MinimumBitsToStore(static_cast<size_t>(TypeCheckKind::kLast));
  static constexpr size_t kFlagMustDoNullCheck = kFieldTypeCheckKind + kFieldTypeCheckKindSize;
  static constexpr size_t kFlagValidTargetClassRTI = kFlagMustDoNullCheck + 1;
  static constexpr size_t kNumberOfInstanceOfPackedBits = kFlagValidTargetClassRTI + 1;
  static_assert(kNumberOfInstanceOfPackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");
  using TypeCheckKindField = BitField<TypeCheckKind, kFieldTypeCheckKind, kFieldTypeCheckKindSize>;

  Handle<mirror::Class> klass_;
};

class HInstanceOf final : public HTypeCheckInstruction {
 public:
  HInstanceOf(HInstruction* object,
              HInstruction* target_class_or_null,
              TypeCheckKind check_kind,
              Handle<mirror::Class> klass,
              uint32_t dex_pc,
              ArenaAllocator* allocator,
              HIntConstant* bitstring_path_to_root,
              HIntConstant* bitstring_mask)
      : HTypeCheckInstruction(kInstanceOf,
                              DataType::Type::kBool,
                              object,
                              target_class_or_null,
                              check_kind,
                              klass,
                              dex_pc,
                              allocator,
                              bitstring_path_to_root,
                              bitstring_mask,
                              SideEffectsForArchRuntimeCalls(check_kind)) {}

  bool IsClonable() const override { return true; }

  bool NeedsEnvironment() const override {
    return CanCallRuntime(GetTypeCheckKind());
  }

  static bool CanCallRuntime(TypeCheckKind check_kind) {
    // TODO: Re-evaluate now that mips codegen has been removed.
    return check_kind != TypeCheckKind::kExactCheck;
  }

  static SideEffects SideEffectsForArchRuntimeCalls(TypeCheckKind check_kind) {
    return CanCallRuntime(check_kind) ? SideEffects::CanTriggerGC() : SideEffects::None();
  }

  DECLARE_INSTRUCTION(InstanceOf);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(InstanceOf);
};

class HBoundType final : public HExpression<1> {
 public:
  explicit HBoundType(HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HExpression(kBoundType, DataType::Type::kReference, SideEffects::None(), dex_pc),
        upper_bound_(ReferenceTypeInfo::CreateInvalid()) {
    SetPackedFlag<kFlagUpperCanBeNull>(true);
    SetPackedFlag<kFlagCanBeNull>(true);
    DCHECK_EQ(input->GetType(), DataType::Type::kReference);
    SetRawInputAt(0, input);
  }

  bool InstructionDataEquals(const HInstruction* other) const override;
  bool IsClonable() const override { return true; }

  // {Get,Set}Upper* should only be used in reference type propagation.
  const ReferenceTypeInfo& GetUpperBound() const { return upper_bound_; }
  bool GetUpperCanBeNull() const { return GetPackedFlag<kFlagUpperCanBeNull>(); }
  void SetUpperBound(const ReferenceTypeInfo& upper_bound, bool can_be_null);

  void SetCanBeNull(bool can_be_null) {
    DCHECK(GetUpperCanBeNull() || !can_be_null);
    SetPackedFlag<kFlagCanBeNull>(can_be_null);
  }

  bool CanBeNull() const override { return GetPackedFlag<kFlagCanBeNull>(); }

  DECLARE_INSTRUCTION(BoundType);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(BoundType);

 private:
  // Represents the top constraint that can_be_null_ cannot exceed (i.e. if this
  // is false then CanBeNull() cannot be true).
  static constexpr size_t kFlagUpperCanBeNull = kNumberOfGenericPackedBits;
  static constexpr size_t kFlagCanBeNull = kFlagUpperCanBeNull + 1;
  static constexpr size_t kNumberOfBoundTypePackedBits = kFlagCanBeNull + 1;
  static_assert(kNumberOfBoundTypePackedBits <= kMaxNumberOfPackedBits, "Too many packed fields.");

  // Encodes the most upper class that this instruction can have. In other words
  // it is always the case that GetUpperBound().IsSupertypeOf(GetReferenceType()).
  // It is used to bound the type in cases like:
  //   if (x instanceof ClassX) {
  //     // uper_bound_ will be ClassX
  //   }
  ReferenceTypeInfo upper_bound_;
};

class HCheckCast final : public HTypeCheckInstruction {
 public:
  HCheckCast(HInstruction* object,
             HInstruction* target_class_or_null,
             TypeCheckKind check_kind,
             Handle<mirror::Class> klass,
             uint32_t dex_pc,
             ArenaAllocator* allocator,
             HIntConstant* bitstring_path_to_root,
             HIntConstant* bitstring_mask)
      : HTypeCheckInstruction(kCheckCast,
                              DataType::Type::kVoid,
                              object,
                              target_class_or_null,
                              check_kind,
                              klass,
                              dex_pc,
                              allocator,
                              bitstring_path_to_root,
                              bitstring_mask,
                              SideEffects::CanTriggerGC()) {}

  bool IsClonable() const override { return true; }
  bool NeedsEnvironment() const override {
    // Instruction may throw a CheckCastError.
    return true;
  }

  bool CanThrow() const override { return true; }

  DECLARE_INSTRUCTION(CheckCast);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(CheckCast);
};

/**
 * @brief Memory barrier types (see "The JSR-133 Cookbook for Compiler Writers").
 * @details We define the combined barrier types that are actually required
 * by the Java Memory Model, rather than using exactly the terminology from
 * the JSR-133 cookbook.  These should, in many cases, be replaced by acquire/release
 * primitives.  Note that the JSR-133 cookbook generally does not deal with
 * store atomicity issues, and the recipes there are not always entirely sufficient.
 * The current recipe is as follows:
 * -# Use AnyStore ~= (LoadStore | StoreStore) ~= release barrier before volatile store.
 * -# Use AnyAny barrier after volatile store.  (StoreLoad is as expensive.)
 * -# Use LoadAny barrier ~= (LoadLoad | LoadStore) ~= acquire barrier after each volatile load.
 * -# Use StoreStore barrier after all stores but before return from any constructor whose
 *    class has final fields.
 * -# Use NTStoreStore to order non-temporal stores with respect to all later
 *    store-to-memory instructions.  Only generated together with non-temporal stores.
 */
enum MemBarrierKind {
  kAnyStore,
  kLoadAny,
  kStoreStore,
  kAnyAny,
  kNTStoreStore,
  kLastBarrierKind = kNTStoreStore
};
std::ostream& operator<<(std::ostream& os, MemBarrierKind kind);

class HMemoryBarrier final : public HExpression<0> {
 public:
  explicit HMemoryBarrier(MemBarrierKind barrier_kind, uint32_t dex_pc = kNoDexPc)
      : HExpression(kMemoryBarrier,
                    SideEffects::AllWritesAndReads(),  // Assume write/read on all fields/arrays.
                    dex_pc) {
    SetPackedField<BarrierKindField>(barrier_kind);
  }

  bool IsClonable() const override { return true; }

  MemBarrierKind GetBarrierKind() { return GetPackedField<BarrierKindField>(); }

  DECLARE_INSTRUCTION(MemoryBarrier);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(MemoryBarrier);

 private:
  static constexpr size_t kFieldBarrierKind = HInstruction::kNumberOfGenericPackedBits;
  static constexpr size_t kFieldBarrierKindSize =
      MinimumBitsToStore(static_cast<size_t>(kLastBarrierKind));
  static constexpr size_t kNumberOfMemoryBarrierPackedBits =
      kFieldBarrierKind + kFieldBarrierKindSize;
  static_assert(kNumberOfMemoryBarrierPackedBits <= kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using BarrierKindField = BitField<MemBarrierKind, kFieldBarrierKind, kFieldBarrierKindSize>;
};

// A constructor fence orders all prior stores to fields that could be accessed via a final field of
// the specified object(s), with respect to any subsequent store that might "publish"
// (i.e. make visible) the specified object to another thread.
//
// JLS 17.5.1 "Semantics of final fields" states that a freeze action happens
// for all final fields (that were set) at the end of the invoked constructor.
//
// The constructor fence models the freeze actions for the final fields of an object
// being constructed (semantically at the end of the constructor). Constructor fences
// have a per-object affinity; two separate objects being constructed get two separate
// constructor fences.
//
// (Note: that if calling a super-constructor or forwarding to another constructor,
// the freezes would happen at the end of *that* constructor being invoked).
//
// The memory model guarantees that when the object being constructed is "published" after
// constructor completion (i.e. escapes the current thread via a store), then any final field
// writes must be observable on other threads (once they observe that publication).
//
// Further, anything written before the freeze, and read by dereferencing through the final field,
// must also be visible (so final object field could itself have an object with non-final fields;
// yet the freeze must also extend to them).
//
// Constructor example:
//
//     class HasFinal {
//        final int field;                              Optimizing IR for <init>()V:
//        HasFinal() {
//          field = 123;                                HInstanceFieldSet(this, HasFinal.field, 123)
//          // freeze(this.field);                      HConstructorFence(this)
//        }                                             HReturn
//     }
//
// HConstructorFence can serve double duty as a fence for new-instance/new-array allocations of
// already-initialized classes; in that case the allocation must act as a "default-initializer"
// of the object which effectively writes the class pointer "final field".
//
// For example, we can model default-initialiation as roughly the equivalent of the following:
//
//     class Object {
//       private final Class header;
//     }
//
//  Java code:                                           Optimizing IR:
//
//     T new_instance<T>() {
//       Object obj = allocate_memory(T.class.size);     obj = HInvoke(art_quick_alloc_object, T)
//       obj.header = T.class;                           // header write is done by above call.
//       // freeze(obj.header)                           HConstructorFence(obj)
//       return (T)obj;
//     }
//
// See also:
// * DexCompilationUnit::RequiresConstructorBarrier
// * QuasiAtomic::ThreadFenceForConstructor
//
class HConstructorFence final : public HVariableInputSizeInstruction {
                                  // A fence has variable inputs because the inputs can be removed
                                  // after prepare_for_register_allocation phase.
                                  // (TODO: In the future a fence could freeze multiple objects
                                  //        after merging two fences together.)
 public:
  // `fence_object` is the reference that needs to be protected for correct publication.
  //
  // It makes sense in the following situations:
  // * <init> constructors, it's the "this" parameter (i.e. HParameterValue, s.t. IsThis() == true).
  // * new-instance-like instructions, it's the return value (i.e. HNewInstance).
  //
  // After construction the `fence_object` becomes the 0th input.
  // This is not an input in a real sense, but just a convenient place to stash the information
  // about the associated object.
  HConstructorFence(HInstruction* fence_object,
                    uint32_t dex_pc,
                    ArenaAllocator* allocator)
    // We strongly suspect there is not a more accurate way to describe the fine-grained reordering
    // constraints described in the class header. We claim that these SideEffects constraints
    // enforce a superset of the real constraints.
    //
    // The ordering described above is conservatively modeled with SideEffects as follows:
    //
    // * To prevent reordering of the publication stores:
    // ----> "Reads of objects" is the initial SideEffect.
    // * For every primitive final field store in the constructor:
    // ----> Union that field's type as a read (e.g. "Read of T") into the SideEffect.
    // * If there are any stores to reference final fields in the constructor:
    // ----> Use a more conservative "AllReads" SideEffect because any stores to any references
    //       that are reachable from `fence_object` also need to be prevented for reordering
    //       (and we do not want to do alias analysis to figure out what those stores are).
    //
    // In the implementation, this initially starts out as an "all reads" side effect; this is an
    // even more conservative approach than the one described above, and prevents all of the
    // above reordering without analyzing any of the instructions in the constructor.
    //
    // If in a later phase we discover that there are no writes to reference final fields,
    // we can refine the side effect to a smaller set of type reads (see above constraints).
      : HVariableInputSizeInstruction(kConstructorFence,
                                      SideEffects::AllReads(),
                                      dex_pc,
                                      allocator,
                                      /* number_of_inputs= */ 1,
                                      kArenaAllocConstructorFenceInputs) {
    DCHECK(fence_object != nullptr);
    SetRawInputAt(0, fence_object);
  }

  // The object associated with this constructor fence.
  //
  // (Note: This will be null after the prepare_for_register_allocation phase,
  // as all constructor fence inputs are removed there).
  HInstruction* GetFenceObject() const {
    return InputAt(0);
  }

  // Find all the HConstructorFence uses (`fence_use`) for `this` and:
  // - Delete `fence_use` from `this`'s use list.
  // - Delete `this` from `fence_use`'s inputs list.
  // - If the `fence_use` is dead, remove it from the graph.
  //
  // A fence is considered dead once it no longer has any uses
  // and all of the inputs are dead.
  //
  // This must *not* be called during/after prepare_for_register_allocation,
  // because that removes all the inputs to the fences but the fence is actually
  // still considered live.
  //
  // Returns how many HConstructorFence instructions were removed from graph.
  static size_t RemoveConstructorFences(HInstruction* instruction);

  // Combine all inputs of `this` and `other` instruction and remove
  // `other` from the graph.
  //
  // Inputs are unique after the merge.
  //
  // Requirement: `this` must not be the same as `other.
  void Merge(HConstructorFence* other);

  // Check if this constructor fence is protecting
  // an HNewInstance or HNewArray that is also the immediate
  // predecessor of `this`.
  //
  // If `ignore_inputs` is true, then the immediate predecessor doesn't need
  // to be one of the inputs of `this`.
  //
  // Returns the associated HNewArray or HNewInstance,
  // or null otherwise.
  HInstruction* GetAssociatedAllocation(bool ignore_inputs = false);

  DECLARE_INSTRUCTION(ConstructorFence);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ConstructorFence);
};

class HMonitorOperation final : public HExpression<1> {
 public:
  enum class OperationKind {
    kEnter,
    kExit,
    kLast = kExit
  };

  HMonitorOperation(HInstruction* object, OperationKind kind, uint32_t dex_pc)
    : HExpression(kMonitorOperation,
                  SideEffects::AllExceptGCDependency(),  // Assume write/read on all fields/arrays.
                  dex_pc) {
    SetPackedField<OperationKindField>(kind);
    SetRawInputAt(0, object);
  }

  // Instruction may go into runtime, so we need an environment.
  bool NeedsEnvironment() const override { return true; }

  bool CanThrow() const override {
    // Verifier guarantees that monitor-exit cannot throw.
    // This is important because it allows the HGraphBuilder to remove
    // a dead throw-catch loop generated for `synchronized` blocks/methods.
    return IsEnter();
  }

  OperationKind GetOperationKind() const { return GetPackedField<OperationKindField>(); }
  bool IsEnter() const { return GetOperationKind() == OperationKind::kEnter; }

  DECLARE_INSTRUCTION(MonitorOperation);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(MonitorOperation);

 private:
  static constexpr size_t kFieldOperationKind = HInstruction::kNumberOfGenericPackedBits;
  static constexpr size_t kFieldOperationKindSize =
      MinimumBitsToStore(static_cast<size_t>(OperationKind::kLast));
  static constexpr size_t kNumberOfMonitorOperationPackedBits =
      kFieldOperationKind + kFieldOperationKindSize;
  static_assert(kNumberOfMonitorOperationPackedBits <= HInstruction::kMaxNumberOfPackedBits,
                "Too many packed fields.");
  using OperationKindField = BitField<OperationKind, kFieldOperationKind, kFieldOperationKindSize>;
};

class HSelect final : public HExpression<3> {
 public:
  HSelect(HInstruction* condition,
          HInstruction* true_value,
          HInstruction* false_value,
          uint32_t dex_pc)
      : HExpression(kSelect, HPhi::ToPhiType(true_value->GetType()), SideEffects::None(), dex_pc) {
    DCHECK_EQ(HPhi::ToPhiType(true_value->GetType()), HPhi::ToPhiType(false_value->GetType()));

    // First input must be `true_value` or `false_value` to allow codegens to
    // use the SameAsFirstInput allocation policy. We make it `false_value`, so
    // that architectures which implement HSelect as a conditional move also
    // will not need to invert the condition.
    SetRawInputAt(0, false_value);
    SetRawInputAt(1, true_value);
    SetRawInputAt(2, condition);
  }

  bool IsClonable() const override { return true; }
  HInstruction* GetFalseValue() const { return InputAt(0); }
  HInstruction* GetTrueValue() const { return InputAt(1); }
  HInstruction* GetCondition() const { return InputAt(2); }

  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }

  bool CanBeNull() const override {
    return GetTrueValue()->CanBeNull() || GetFalseValue()->CanBeNull();
  }

  void UpdateType() {
    DCHECK_EQ(HPhi::ToPhiType(GetTrueValue()->GetType()),
              HPhi::ToPhiType(GetFalseValue()->GetType()));
    SetPackedField<TypeField>(HPhi::ToPhiType(GetTrueValue()->GetType()));
  }

  DECLARE_INSTRUCTION(Select);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(Select);
};

class MoveOperands : public ArenaObject<kArenaAllocMoveOperands> {
 public:
  MoveOperands(Location source,
               Location destination,
               DataType::Type type,
               HInstruction* instruction)
      : source_(source), destination_(destination), type_(type), instruction_(instruction) {}

  Location GetSource() const { return source_; }
  Location GetDestination() const { return destination_; }

  void SetSource(Location value) { source_ = value; }
  void SetDestination(Location value) { destination_ = value; }

  // The parallel move resolver marks moves as "in-progress" by clearing the
  // destination (but not the source).
  Location MarkPending() {
    DCHECK(!IsPending());
    Location dest = destination_;
    destination_ = Location::NoLocation();
    return dest;
  }

  void ClearPending(Location dest) {
    DCHECK(IsPending());
    destination_ = dest;
  }

  bool IsPending() const {
    DCHECK(source_.IsValid() || destination_.IsInvalid());
    return destination_.IsInvalid() && source_.IsValid();
  }

  // True if this blocks a move from the given location.
  bool Blocks(Location loc) const {
    return !IsEliminated() && source_.OverlapsWith(loc);
  }

  // A move is redundant if it's been eliminated, if its source and
  // destination are the same, or if its destination is unneeded.
  bool IsRedundant() const {
    return IsEliminated() || destination_.IsInvalid() || source_.Equals(destination_);
  }

  // We clear both operands to indicate move that's been eliminated.
  void Eliminate() {
    source_ = destination_ = Location::NoLocation();
  }

  bool IsEliminated() const {
    DCHECK_IMPLIES(source_.IsInvalid(), destination_.IsInvalid());
    return source_.IsInvalid();
  }

  DataType::Type GetType() const { return type_; }

  bool Is64BitMove() const {
    return DataType::Is64BitType(type_);
  }

  HInstruction* GetInstruction() const { return instruction_; }

 private:
  Location source_;
  Location destination_;
  // The type this move is for.
  DataType::Type type_;
  // The instruction this move is assocatied with. Null when this move is
  // for moving an input in the expected locations of user (including a phi user).
  // This is only used in debug mode, to ensure we do not connect interval siblings
  // in the same parallel move.
  HInstruction* instruction_;
};

std::ostream& operator<<(std::ostream& os, const MoveOperands& rhs);

static constexpr size_t kDefaultNumberOfMoves = 4;

class HParallelMove final : public HExpression<0> {
 public:
  explicit HParallelMove(ArenaAllocator* allocator, uint32_t dex_pc = kNoDexPc)
      : HExpression(kParallelMove, SideEffects::None(), dex_pc),
        moves_(allocator->Adapter(kArenaAllocMoveOperands)) {
    moves_.reserve(kDefaultNumberOfMoves);
  }

  void AddMove(Location source,
               Location destination,
               DataType::Type type,
               HInstruction* instruction) {
    DCHECK(source.IsValid());
    DCHECK(destination.IsValid());
    if (kIsDebugBuild) {
      if (instruction != nullptr) {
        for (const MoveOperands& move : moves_) {
          if (move.GetInstruction() == instruction) {
            // Special case the situation where the move is for the spill slot
            // of the instruction.
            if ((GetPrevious() == instruction)
                || ((GetPrevious() == nullptr)
                    && instruction->IsPhi()
                    && instruction->GetBlock() == GetBlock())) {
              DCHECK_NE(destination.GetKind(), move.GetDestination().GetKind())
                  << "Doing parallel moves for the same instruction.";
            } else {
              DCHECK(false) << "Doing parallel moves for the same instruction.";
            }
          }
        }
      }
      for (const MoveOperands& move : moves_) {
        DCHECK(!destination.OverlapsWith(move.GetDestination()))
            << "Overlapped destination for two moves in a parallel move: "
            << move.GetSource() << " ==> " << move.GetDestination() << " and "
            << source << " ==> " << destination << " for " << SafePrint(instruction);
      }
    }
    moves_.emplace_back(source, destination, type, instruction);
  }

  MoveOperands* MoveOperandsAt(size_t index) {
    return &moves_[index];
  }

  size_t NumMoves() const { return moves_.size(); }

  DECLARE_INSTRUCTION(ParallelMove);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(ParallelMove);

 private:
  ArenaVector<MoveOperands> moves_;
};

class HBitwiseNegatedRight final : public HBinaryOperation {
 public:
  HBitwiseNegatedRight(DataType::Type result_type,
                       InstructionKind op,
                       HInstruction* left,
                       HInstruction* right,
                       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(
            kBitwiseNegatedRight, result_type, left, right, SideEffects::None(), dex_pc),
        op_kind_(op) {
    DCHECK(op == HInstruction::kAnd || op == HInstruction::kOr || op == HInstruction::kXor) << op;
  }

  template <typename T, typename U>
  auto Compute(T x, U y) const -> decltype(x & ~y) {
    static_assert(std::is_same<decltype(x & ~y), decltype(x | ~y)>::value &&
                      std::is_same<decltype(x & ~y), decltype(x ^ ~y)>::value,
                  "Inconsistent negated bitwise types");
    switch (op_kind_) {
      case HInstruction::kAnd:
        return x & ~y;
      case HInstruction::kOr:
        return x | ~y;
      case HInstruction::kXor:
        return x ^ ~y;
      default:
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
    }
  }

  bool InstructionDataEquals(const HInstruction* other) const override {
    return op_kind_ == other->AsBitwiseNegatedRight()->op_kind_;
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const override {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue(), y->GetValue()));
  }

  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const override {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue(), y->GetValue()));
  }

  InstructionKind GetOpKind() const { return op_kind_; }

  DECLARE_INSTRUCTION(BitwiseNegatedRight);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(BitwiseNegatedRight);

 private:
  // Specifies the bitwise operation, which will be then negated.
  const InstructionKind op_kind_;
};

// This instruction computes an intermediate address pointing in the 'middle' of an object. The
// result pointer cannot be handled by GC, so extra care is taken to make sure that this value is
// never used across anything that can trigger GC.
// The result of this instruction is not a pointer in the sense of `DataType::Type::kreference`.
// So we represent it by the type `DataType::Type::kInt`.
class HIntermediateAddress final : public HExpression<2> {
 public:
  HIntermediateAddress(HInstruction* base_address, HInstruction* offset, uint32_t dex_pc)
      : HExpression(kIntermediateAddress,
                    DataType::Type::kInt32,
                    SideEffects::DependsOnGC(),
                    dex_pc) {
        DCHECK_EQ(DataType::Size(DataType::Type::kInt32),
                  DataType::Size(DataType::Type::kReference))
            << "kPrimInt and kPrimNot have different sizes.";
    SetRawInputAt(0, base_address);
    SetRawInputAt(1, offset);
  }

  bool IsClonable() const override { return true; }
  bool CanBeMoved() const override { return true; }
  bool InstructionDataEquals([[maybe_unused]] const HInstruction* other) const override {
    return true;
  }
  bool IsActualObject() const override { return false; }

  HInstruction* GetBaseAddress() const { return InputAt(0); }
  HInstruction* GetOffset() const { return InputAt(1); }

  DECLARE_INSTRUCTION(IntermediateAddress);

 protected:
  DEFAULT_COPY_CONSTRUCTOR(IntermediateAddress);
};


}  // namespace art

#include "nodes_vector.h"

#if defined(ART_ENABLE_CODEGEN_arm) || defined(ART_ENABLE_CODEGEN_arm64)
#include "nodes_shared.h"
#endif
#if defined(ART_ENABLE_CODEGEN_x86) || defined(ART_ENABLE_CODEGEN_x86_64)
#include "nodes_x86.h"
#endif
#if defined(ART_ENABLE_CODEGEN_riscv64)
#include "nodes_riscv64.h"
#endif

namespace art HIDDEN {

class OptimizingCompilerStats;

class HGraphVisitor : public ValueObject {
 public:
  explicit HGraphVisitor(HGraph* graph, OptimizingCompilerStats* stats = nullptr)
      : stats_(stats),
        graph_(graph) {}
  virtual ~HGraphVisitor() {}

  virtual void VisitInstruction([[maybe_unused]] HInstruction* instruction) {}
  virtual void VisitBasicBlock(HBasicBlock* block);

  // Visit the graph following basic block insertion order.
  void VisitInsertionOrder();

  // Visit the graph following dominator tree reverse post-order.
  void VisitReversePostOrder();

  HGraph* GetGraph() const { return graph_; }

  // Visit functions for instruction classes.
#define DECLARE_VISIT_INSTRUCTION(name, super)                                        \
  virtual void Visit##name(H##name* instr) { VisitInstruction(instr); }

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 protected:
  void VisitPhis(HBasicBlock* block);
  void VisitNonPhiInstructions(HBasicBlock* block);
  void VisitNonPhiInstructionsHandleChanges(HBasicBlock* block);

  OptimizingCompilerStats* stats_;

 private:
  HGraph* const graph_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisitor);
};

class HGraphDelegateVisitor : public HGraphVisitor {
 public:
  explicit HGraphDelegateVisitor(HGraph* graph, OptimizingCompilerStats* stats = nullptr)
      : HGraphVisitor(graph, stats) {}
  virtual ~HGraphDelegateVisitor() {}

  // Visit functions that delegate to to super class.
#define DECLARE_VISIT_INSTRUCTION(name, super)                                        \
  void Visit##name(H##name* instr) override { Visit##super(instr); }

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  DISALLOW_COPY_AND_ASSIGN(HGraphDelegateVisitor);
};

// Create a clone of the instruction, insert it into the graph; replace the old one with a new
// and remove the old instruction.
HInstruction* ReplaceInstrOrPhiByClone(HInstruction* instr);

// Create a clone for each clonable instructions/phis and replace the original with the clone.
//
// Used for testing individual instruction cloner.
class CloneAndReplaceInstructionVisitor final : public HGraphDelegateVisitor {
 public:
  explicit CloneAndReplaceInstructionVisitor(HGraph* graph)
      : HGraphDelegateVisitor(graph), instr_replaced_by_clones_count_(0) {}

  void VisitInstruction(HInstruction* instruction) override {
    if (instruction->IsClonable()) {
      ReplaceInstrOrPhiByClone(instruction);
      instr_replaced_by_clones_count_++;
    }
  }

  size_t GetInstrReplacedByClonesCount() const { return instr_replaced_by_clones_count_; }

 private:
  size_t instr_replaced_by_clones_count_;

  DISALLOW_COPY_AND_ASSIGN(CloneAndReplaceInstructionVisitor);
};

// Iterator over the blocks that are part of the loop; includes blocks which are part
// of an inner loop. The order in which the blocks are iterated is on their
// block id.
class HBlocksInLoopIterator : public ValueObject {
 public:
  explicit HBlocksInLoopIterator(const HLoopInformation& info)
      : blocks_in_loop_(info.GetBlocks()),
        blocks_(info.GetHeader()->GetGraph()->GetBlocks()),
        index_(0) {
    if (!blocks_in_loop_.IsBitSet(index_)) {
      Advance();
    }
  }

  bool Done() const { return index_ == blocks_.size(); }
  HBasicBlock* Current() const { return blocks_[index_]; }
  void Advance() {
    ++index_;
    for (size_t e = blocks_.size(); index_ < e; ++index_) {
      if (blocks_in_loop_.IsBitSet(index_)) {
        break;
      }
    }
  }

 private:
  const BitVector& blocks_in_loop_;
  const ArenaVector<HBasicBlock*>& blocks_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HBlocksInLoopIterator);
};

// Iterator over the blocks that are part of the loop; includes blocks which are part
// of an inner loop. The order in which the blocks are iterated is reverse
// post order.
class HBlocksInLoopReversePostOrderIterator : public ValueObject {
 public:
  explicit HBlocksInLoopReversePostOrderIterator(const HLoopInformation& info)
      : blocks_in_loop_(info.GetBlocks()),
        blocks_(info.GetHeader()->GetGraph()->GetReversePostOrder()),
        index_(0) {
    if (!blocks_in_loop_.IsBitSet(blocks_[index_]->GetBlockId())) {
      Advance();
    }
  }

  bool Done() const { return index_ == blocks_.size(); }
  HBasicBlock* Current() const { return blocks_[index_]; }
  void Advance() {
    ++index_;
    for (size_t e = blocks_.size(); index_ < e; ++index_) {
      if (blocks_in_loop_.IsBitSet(blocks_[index_]->GetBlockId())) {
        break;
      }
    }
  }

 private:
  const BitVector& blocks_in_loop_;
  const ArenaVector<HBasicBlock*>& blocks_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HBlocksInLoopReversePostOrderIterator);
};

// Iterator over the blocks that are part of the loop; includes blocks which are part
// of an inner loop. The order in which the blocks are iterated is post order.
class HBlocksInLoopPostOrderIterator : public ValueObject {
 public:
  explicit HBlocksInLoopPostOrderIterator(const HLoopInformation& info)
      : blocks_in_loop_(info.GetBlocks()),
        blocks_(info.GetHeader()->GetGraph()->GetReversePostOrder()),
        index_(blocks_.size() - 1) {
    if (!blocks_in_loop_.IsBitSet(blocks_[index_]->GetBlockId())) {
      Advance();
    }
  }

  bool Done() const { return index_ < 0; }
  HBasicBlock* Current() const { return blocks_[index_]; }
  void Advance() {
    --index_;
    for (; index_ >= 0; --index_) {
      if (blocks_in_loop_.IsBitSet(blocks_[index_]->GetBlockId())) {
        break;
      }
    }
  }

 private:
  const BitVector& blocks_in_loop_;
  const ArenaVector<HBasicBlock*>& blocks_;

  int32_t index_;

  DISALLOW_COPY_AND_ASSIGN(HBlocksInLoopPostOrderIterator);
};

// Returns int64_t value of a properly typed constant.
inline int64_t Int64FromConstant(HConstant* constant) {
  if (constant->IsIntConstant()) {
    return constant->AsIntConstant()->GetValue();
  } else if (constant->IsLongConstant()) {
    return constant->AsLongConstant()->GetValue();
  } else {
    DCHECK(constant->IsNullConstant()) << constant->DebugName();
    return 0;
  }
}

// Returns true iff instruction is an integral constant (and sets value on success).
inline bool IsInt64AndGet(HInstruction* instruction, /*out*/ int64_t* value) {
  if (instruction->IsIntConstant()) {
    *value = instruction->AsIntConstant()->GetValue();
    return true;
  } else if (instruction->IsLongConstant()) {
    *value = instruction->AsLongConstant()->GetValue();
    return true;
  } else if (instruction->IsNullConstant()) {
    *value = 0;
    return true;
  }
  return false;
}

// Returns true iff instruction is the given integral constant.
inline bool IsInt64Value(HInstruction* instruction, int64_t value) {
  int64_t val = 0;
  return IsInt64AndGet(instruction, &val) && val == value;
}

// Returns true iff instruction is a zero bit pattern.
inline bool IsZeroBitPattern(HInstruction* instruction) {
  return instruction->IsConstant() && instruction->AsConstant()->IsZeroBitPattern();
}

// Implement HInstruction::Is##type() for concrete instructions.
#define INSTRUCTION_TYPE_CHECK(type, super)                                    \
  inline bool HInstruction::Is##type() const { return GetKind() == k##type; }
  FOR_EACH_CONCRETE_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

// Implement HInstruction::Is##type() for abstract instructions.
#define INSTRUCTION_TYPE_CHECK_RESULT(type, super)                             \
  std::is_base_of<BaseType, H##type>::value,
#define INSTRUCTION_TYPE_CHECK(type, super)                                    \
  inline bool HInstruction::Is##type() const {                                 \
    DCHECK_LT(GetKind(), kLastInstructionKind);                                \
    using BaseType = H##type;                                                  \
    static constexpr bool results[] = {                                        \
        FOR_EACH_CONCRETE_INSTRUCTION(INSTRUCTION_TYPE_CHECK_RESULT)           \
    };                                                                         \
    return results[static_cast<size_t>(GetKind())];                            \
  }

  FOR_EACH_ABSTRACT_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK
#undef INSTRUCTION_TYPE_CHECK_RESULT

#define INSTRUCTION_TYPE_CAST(type, super)                                     \
  inline const H##type* HInstruction::As##type() const {                       \
    DCHECK(Is##type());                                                        \
    return down_cast<const H##type*>(this);                                    \
  }                                                                            \
  inline H##type* HInstruction::As##type() {                                   \
    DCHECK(Is##type());                                                        \
    return down_cast<H##type*>(this);                                          \
  }                                                                            \
  inline const H##type* HInstruction::As##type##OrNull() const {               \
    return Is##type() ? down_cast<const H##type*>(this) : nullptr;             \
  }                                                                            \
  inline H##type* HInstruction::As##type##OrNull() {                           \
    return Is##type() ? down_cast<H##type*>(this) : nullptr;                   \
  }

  FOR_EACH_INSTRUCTION(INSTRUCTION_TYPE_CAST)
#undef INSTRUCTION_TYPE_CAST


// Create space in `blocks` for adding `number_of_new_blocks` entries
// starting at location `at`. Blocks after `at` are moved accordingly.
inline void MakeRoomFor(ArenaVector<HBasicBlock*>* blocks,
                        size_t number_of_new_blocks,
                        size_t after) {
  DCHECK_LT(after, blocks->size());
  size_t old_size = blocks->size();
  size_t new_size = old_size + number_of_new_blocks;
  blocks->resize(new_size);
  std::copy_backward(blocks->begin() + after + 1u, blocks->begin() + old_size, blocks->end());
}

/*
 * Hunt "under the hood" of array lengths (leading to array references),
 * null checks (also leading to array references), and new arrays
 * (leading to the actual length). This makes it more likely related
 * instructions become actually comparable.
 */
inline HInstruction* HuntForDeclaration(HInstruction* instruction) {
  while (instruction->IsArrayLength() ||
         instruction->IsNullCheck() ||
         instruction->IsNewArray()) {
    instruction = instruction->IsNewArray()
        ? instruction->AsNewArray()->GetLength()
        : instruction->InputAt(0);
  }
  return instruction;
}

inline bool IsAddOrSub(const HInstruction* instruction) {
  return instruction->IsAdd() || instruction->IsSub();
}

void RemoveEnvironmentUses(HInstruction* instruction);
bool HasEnvironmentUsedByOthers(HInstruction* instruction);
void ResetEnvironmentInputRecords(HInstruction* instruction);

// Detects an instruction that is >= 0. As long as the value is carried by
// a single instruction, arithmetic wrap-around cannot occur.
bool IsGEZero(HInstruction* instruction);

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_H_

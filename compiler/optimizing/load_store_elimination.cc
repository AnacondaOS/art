/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "load_store_elimination.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <variant>

#include "base/arena_allocator.h"
#include "base/arena_bit_vector.h"
#include "base/array_ref.h"
#include "base/bit_utils_iterator.h"
#include "base/bit_vector-inl.h"
#include "base/bit_vector.h"
#include "base/globals.h"
#include "base/indenter.h"
#include "base/iteration_range.h"
#include "base/scoped_arena_allocator.h"
#include "base/scoped_arena_containers.h"
#include "base/transform_iterator.h"
#include "escape.h"
#include "handle.h"
#include "load_store_analysis.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "oat/stack_map.h"
#include "optimizing_compiler_stats.h"
#include "reference_type_propagation.h"
#include "side_effects_analysis.h"

/**
 * The general algorithm of load-store elimination (LSE).
 *
 * We use load-store analysis to collect a list of heap locations and perform
 * alias analysis of those heap locations. LSE then keeps track of a list of
 * heap values corresponding to the heap locations and stores that put those
 * values in these locations.
 *  - In phase 1, we visit basic blocks in reverse post order and for each basic
 *    block, visit instructions sequentially, recording heap values and looking
 *    for loads and stores to eliminate without relying on loop Phis.
 *  - In phase 2, we look for loads that can be replaced by creating loop Phis
 *    or using a loop-invariant value.
 *  - In phase 3, we determine which stores are dead and can be eliminated and
 *    based on that information we re-evaluate whether some kept stores are
 *    storing the same value as the value in the heap location; such stores are
 *    also marked for elimination.
 *  - In phase 4, we commit the changes, replacing loads marked for elimination
 *    in previous processing and removing stores not marked for keeping. We also
 *    remove allocations that are no longer needed.
 *  - In phase 5, we move allocations which only escape along some executions
 *    closer to their escape points and fixup non-escaping paths with their actual
 *    values, creating PHIs when needed.
 *
 * 1. Walk over blocks and their instructions.
 *
 * The initial set of heap values for a basic block is
 *  - For a loop header of an irreducible loop, all heap values are unknown.
 *  - For a loop header of a normal loop, all values unknown at the end of the
 *    preheader are initialized to unknown, other heap values are set to Phi
 *    placeholders as we cannot determine yet whether these values are known on
 *    all back-edges. We use Phi placeholders also for array heap locations with
 *    index defined inside the loop but this helps only when the value remains
 *    zero from the array allocation throughout the loop.
 *  - For catch blocks, we clear all assumptions since we arrived due to an
 *    instruction throwing.
 *  - For other basic blocks, we merge incoming values from the end of all
 *    predecessors. If any incoming value is unknown, the start value for this
 *    block is also unknown. Otherwise, if all the incoming values are the same
 *    (including the case of a single predecessor), the incoming value is used.
 *    Otherwise, we use a Phi placeholder to indicate different incoming values.
 *    We record whether such Phi placeholder depends on a loop Phi placeholder.
 *
 * For each instruction in the block
 *  - If the instruction is a load from a heap location with a known value not
 *    dependent on a loop Phi placeholder, the load can be eliminated, either by
 *    using an existing instruction or by creating new Phi(s) instead. In order
 *    to maintain the validity of all heap locations during the optimization
 *    phase, we only record substitutes at this phase and the real elimination
 *    is delayed till the end of LSE. Loads that require a loop Phi placeholder
 *    replacement are recorded for processing later.
 *  - If the instruction is a store, it updates the heap value for the heap
 *    location with the stored value and records the store itself so that we can
 *    mark it for keeping if the value becomes observable. Heap values are
 *    invalidated for heap locations that may alias with the store instruction's
 *    heap location and their recorded stores are marked for keeping as they are
 *    now potentially observable. The store instruction can be eliminated unless
 *    the value stored is later needed e.g. by a load from the same/aliased heap
 *    location or the heap location persists at method return/deoptimization.
 *  - A store that stores the same value as the heap value is eliminated.
 *  - For newly instantiated instances, their heap values are initialized to
 *    language defined default values.
 *  - Finalizable objects are considered as persisting at method
 *    return/deoptimization.
 *  - Some instructions such as invokes are treated as loading and invalidating
 *    all the heap values, depending on the instruction's side effects.
 *  - SIMD graphs (with VecLoad and VecStore instructions) are also handled. Any
 *    partial overlap access among ArrayGet/ArraySet/VecLoad/Store is seen as
 *    alias and no load/store is eliminated in such case.
 *
 * The time complexity of the initial phase has several components. The total
 * time for the initialization of heap values for all blocks is
 *    O(heap_locations * edges)
 * and the time complexity for simple instruction processing is
 *    O(instructions).
 * See the description of phase 3 for additional complexity due to matching of
 * existing Phis for replacing loads.
 *
 * 2. Process loads that depend on loop Phi placeholders.
 *
 * We go over these loads to determine whether they can be eliminated. We look
 * for the set of all Phi placeholders that feed the load and depend on a loop
 * Phi placeholder and, if we find no unknown value, we construct the necessary
 * Phi(s) or, if all other inputs are identical, i.e. the location does not
 * change in the loop, just use that input. If we do find an unknown input, this
 * must be from a loop back-edge and we replace the loop Phi placeholder with
 * unknown value and re-process loads and stores that previously depended on
 * loop Phi placeholders. This shall find at least one load of an unknown value
 * which is now known to be unreplaceable or a new unknown value on a back-edge
 * and we repeat this process until each load is either marked for replacement
 * or found to be unreplaceable. As we mark at least one additional loop Phi
 * placeholder as unreplacable in each iteration, this process shall terminate.
 *
 * The depth-first search for Phi placeholders in FindLoopPhisToMaterialize()
 * is limited by the number of Phi placeholders and their dependencies we need
 * to search with worst-case time complexity
 *    O(phi_placeholder_dependencies) .
 * The dependencies are usually just the Phi placeholders' potential inputs,
 * but if we use TryReplacingLoopPhiPlaceholderWithDefault() for default value
 * replacement search, there are additional dependencies to consider, see below.
 *
 * In the successful case (no unknown inputs found) we use the Floyd-Warshall
 * algorithm to determine transitive closures for each found Phi placeholder,
 * and then match or materialize Phis from the smallest transitive closure,
 * so that we can determine if such subset has a single other input. This has
 * time complexity
 *    O(phi_placeholders_found^3) .
 * Note that successful TryReplacingLoopPhiPlaceholderWithDefault() does not
 * contribute to this as such Phi placeholders are replaced immediately.
 * The total time of all such successful cases has time complexity
 *    O(phi_placeholders^3)
 * because the found sets are disjoint and `Sum(n_i^3) <= Sum(n_i)^3`. Similar
 * argument applies to the searches used to find all successful cases, so their
 * total contribution is also just an insignificant
 *    O(phi_placeholder_dependencies) .
 * The materialization of Phis has an insignificant total time complexity
 *    O(phi_placeholders * edges) .
 *
 * If we find an unknown input, we re-process heap values and loads with a time
 * complexity that's the same as the phase 1 in the worst case. Adding this to
 * the depth-first search time complexity yields
 *    O(phi_placeholder_dependencies + heap_locations * edges + instructions)
 * for a single iteration. We can ignore the middle term as it's proprotional
 * to the number of Phi placeholder inputs included in the first term. Using
 * the upper limit of number of such iterations, the total time complexity is
 *    O((phi_placeholder_dependencies + instructions) * phi_placeholders) .
 *
 * The upper bound of Phi placeholder inputs is
 *    heap_locations * edges
 * but if we use TryReplacingLoopPhiPlaceholderWithDefault(), the dependencies
 * include other heap locations in predecessor blocks with the upper bound of
 *    heap_locations^2 * edges .
 * Using the estimate
 *    edges <= blocks^2
 * and
 *    phi_placeholders <= heap_locations * blocks ,
 * the worst-case time complexity of the
 *    O(phi_placeholder_dependencies * phi_placeholders)
 * term from unknown input cases is actually
 *    O(heap_locations^3 * blocks^3) ,
 * exactly as the estimate for the Floyd-Warshall parts of successful cases.
 * Adding the other term from the unknown input cases (to account for the case
 * with significantly more instructions than blocks and heap locations), the
 * phase 2 time complexity is
 *    O(heap_locations^3 * blocks^3 + heap_locations * blocks * instructions) .
 *
 * See the description of phase 3 for additional complexity due to matching of
 * existing Phis for replacing loads.
 *
 * 3. Determine which stores to keep and which to eliminate.
 *
 * During instruction processing in phase 1 and re-processing in phase 2, we are
 * keeping a record of the stores and Phi placeholders that become observable
 * and now propagate the observable Phi placeholders to all actual stores that
 * feed them. Having determined observable stores, we look for stores that just
 * overwrite the old value with the same. Since ignoring non-observable stores
 * actually changes the old values in heap locations, we need to recalculate
 * Phi placeholder replacements but we proceed similarly to the previous phase.
 * We look for the set of all Phis that feed the old value replaced by the store
 * (but ignoring whether they depend on a loop Phi) and, if we find no unknown
 * value, we try to match existing Phis (we do not create new Phis anymore) or,
 * if all other inputs are identical, i.e. the location does not change in the
 * loop, just use that input. If this succeeds and the old value is identical to
 * the value we're storing, such store shall be eliminated.
 *
 * The work is similar to the phase 2, except that we're not re-processing loads
 * and stores anymore, so the time complexity of phase 3 is
 *    O(heap_locations^3 * blocks^3) .
 *
 * There is additional complexity in matching existing Phis shared between the
 * phases 1, 2 and 3. We are never trying to match two or more Phis at the same
 * time (this could be difficult and slow), so each matching attempt is just
 * looking at Phis in the block (both old Phis and newly created Phis) and their
 * inputs. As we create at most `heap_locations` Phis in each block, the upper
 * bound on the number of Phis we look at is
 *    heap_locations * (old_phis + heap_locations)
 * and the worst-case time complexity is
 *    O(heap_locations^2 * edges + heap_locations * old_phis * edges) .
 * The first term is lower than one term in phase 2, so the relevant part is
 *    O(heap_locations * old_phis * edges) .
 *
 * 4. Replace loads and remove unnecessary stores and singleton allocations.
 *
 * A special type of objects called singletons are instantiated in the method
 * and have a single name, i.e. no aliases. Singletons have exclusive heap
 * locations since they have no aliases. Singletons are helpful in narrowing
 * down the life span of a heap location such that they do not always need to
 * participate in merging heap values. Allocation of a singleton can be
 * eliminated if that singleton is not used and does not persist at method
 * return/deoptimization.
 *
 * The time complexity of this phase is
 *    O(instructions + instruction_uses) .
 *
 * FIXME: The time complexities described above assumes that the
 * HeapLocationCollector finds a heap location for an instruction in O(1)
 * time but it is currently O(heap_locations); this can be fixed by adding
 * a hash map to the HeapLocationCollector.
 */

namespace art HIDDEN {

#define LSE_VLOG \
  if (::art::LoadStoreElimination::kVerboseLoggingMode && VLOG_IS_ON(compiler)) LOG(INFO)

class HeapRefHolder;

// Use HGraphDelegateVisitor for which all VisitInvokeXXX() delegate to VisitInvoke().
class LSEVisitor final : private HGraphDelegateVisitor {
 public:
  LSEVisitor(HGraph* graph,
             const HeapLocationCollector& heap_location_collector,
             OptimizingCompilerStats* stats);

  void Run();

 private:
  class PhiPlaceholder {
   public:
    constexpr PhiPlaceholder() : block_id_(-1), heap_location_(-1) {}
    constexpr PhiPlaceholder(uint32_t block_id, size_t heap_location)
        : block_id_(block_id), heap_location_(dchecked_integral_cast<uint32_t>(heap_location)) {}

    constexpr PhiPlaceholder(const PhiPlaceholder& p) = default;
    constexpr PhiPlaceholder(PhiPlaceholder&& p) = default;
    constexpr PhiPlaceholder& operator=(const PhiPlaceholder& p) = default;
    constexpr PhiPlaceholder& operator=(PhiPlaceholder&& p) = default;

    constexpr uint32_t GetBlockId() const {
      return block_id_;
    }

    constexpr size_t GetHeapLocation() const {
      return heap_location_;
    }

    constexpr bool Equals(const PhiPlaceholder& p2) const {
      return block_id_ == p2.block_id_ && heap_location_ == p2.heap_location_;
    }

    void Dump(std::ostream& oss) const {
      oss << "PhiPlaceholder[blk: " << block_id_ << ", heap_location_: " << heap_location_ << "]";
    }

   private:
    uint32_t block_id_;
    uint32_t heap_location_;
  };

  friend constexpr bool operator==(const PhiPlaceholder& p1, const PhiPlaceholder& p2);
  friend std::ostream& operator<<(std::ostream& oss, const PhiPlaceholder& p2);

  class Value {
   public:
    enum class ValuelessType {
      kInvalid,
      kUnknown,
      kDefault,
    };
    struct NeedsNonLoopPhiMarker {
      PhiPlaceholder phi_;
    };
    struct NeedsPlainLoopPhiMarker {
      PhiPlaceholder phi_;
    };
    struct NeedsConvertedLoopPhiMarker {
      HInstruction* load_;  // Load from a narrower location than the loop phi it needs.
    };

    static constexpr Value Invalid() {
      return Value(ValuelessType::kInvalid);
    }

    // An unknown heap value. Loads with such a value in the heap location cannot be eliminated.
    // A heap location can be set to an unknown heap value when:
    // - it is coming from outside the method,
    // - it is killed due to aliasing, or side effects, or merging with an unknown value.
    static constexpr Value Unknown() {
      return Value(ValuelessType::kUnknown);
    }

    // Default heap value after an allocation.
    // A heap location can be set to that value right after an allocation.
    static constexpr Value Default() {
      return Value(ValuelessType::kDefault);
    }

    static constexpr Value ForInstruction(HInstruction* instruction) {
      return Value(instruction);
    }

    static constexpr Value ForNonLoopPhiPlaceholder(PhiPlaceholder phi_placeholder) {
      return Value(NeedsNonLoopPhiMarker{phi_placeholder});
    }

    static constexpr Value ForPlainLoopPhiPlaceholder(PhiPlaceholder phi_placeholder) {
      return Value(NeedsPlainLoopPhiMarker{phi_placeholder});
    }

    static constexpr Value ForConvertedLoopPhiPlaceholder(HInstruction* load) {
      return Value(NeedsConvertedLoopPhiMarker{load});
    }

    static constexpr Value ForPhiPlaceholder(PhiPlaceholder phi_placeholder, bool needs_loop_phi) {
      return needs_loop_phi ? ForPlainLoopPhiPlaceholder(phi_placeholder)
                            : ForNonLoopPhiPlaceholder(phi_placeholder);
    }

    constexpr bool IsValid() const {
      return !IsInvalid();
    }

    constexpr bool IsInvalid() const {
      return std::holds_alternative<ValuelessType>(value_) &&
             GetValuelessType() == ValuelessType::kInvalid;
    }

    bool IsUnknown() const {
      return std::holds_alternative<ValuelessType>(value_) &&
             GetValuelessType() == ValuelessType::kUnknown;
    }

    bool IsDefault() const {
      return std::holds_alternative<ValuelessType>(value_) &&
             GetValuelessType() == ValuelessType::kDefault;
    }

    bool IsInstruction() const {
      return std::holds_alternative<HInstruction*>(value_);
    }

    bool NeedsNonLoopPhi() const {
      return std::holds_alternative<NeedsNonLoopPhiMarker>(value_);
    }

    bool NeedsPlainLoopPhi() const {
      return std::holds_alternative<NeedsPlainLoopPhiMarker>(value_);
    }

    bool NeedsConvertedLoopPhi() const {
      return std::holds_alternative<NeedsConvertedLoopPhiMarker>(value_);
    }

    bool NeedsLoopPhi() const {
      return NeedsPlainLoopPhi() || NeedsConvertedLoopPhi();
    }

    bool NeedsPhi() const {
      return NeedsNonLoopPhi() || NeedsLoopPhi();
    }

    HInstruction* GetInstruction() const {
      DCHECK(IsInstruction()) << *this;
      return std::get<HInstruction*>(value_);
    }

    PhiPlaceholder GetPhiPlaceholder() const {
      if (NeedsNonLoopPhi()) {
        return std::get<NeedsNonLoopPhiMarker>(value_).phi_;
      } else {
        DCHECK(NeedsPlainLoopPhi());
        return std::get<NeedsPlainLoopPhiMarker>(value_).phi_;
      }
    }

    size_t GetHeapLocation() const {
      DCHECK(NeedsPhi()) << this;
      return GetPhiPlaceholder().GetHeapLocation();
    }

    HInstruction* GetLoopPhiConversionLoad() const {
      DCHECK(NeedsConvertedLoopPhi());
      return std::get<NeedsConvertedLoopPhiMarker>(value_).load_;
    }

    constexpr bool ExactEquals(Value other) const;

    constexpr bool Equals(Value other) const;

    constexpr bool Equals(HInstruction* instruction) const {
      return Equals(ForInstruction(instruction));
    }

    std::ostream& Dump(std::ostream& os) const;

    // Public for use with lists.
    constexpr Value() : value_(ValuelessType::kInvalid) {}

   private:
    using ValueHolder = std::variant<ValuelessType,
                                     HInstruction*,
                                     NeedsNonLoopPhiMarker,
                                     NeedsPlainLoopPhiMarker,
                                     NeedsConvertedLoopPhiMarker>;
    constexpr ValuelessType GetValuelessType() const {
      return std::get<ValuelessType>(value_);
    }

    constexpr explicit Value(ValueHolder v) : value_(v) {}

    friend std::ostream& operator<<(std::ostream& os, const Value& v);

    ValueHolder value_;

    static_assert(std::is_move_assignable<PhiPlaceholder>::value);
  };

  friend constexpr bool operator==(const Value::NeedsPlainLoopPhiMarker& p1,
                                   const Value::NeedsPlainLoopPhiMarker& p2);
  friend constexpr bool operator==(const Value::NeedsConvertedLoopPhiMarker& p1,
                                   const Value::NeedsConvertedLoopPhiMarker& p2);
  friend constexpr bool operator==(const Value::NeedsNonLoopPhiMarker& p1,
                                   const Value::NeedsNonLoopPhiMarker& p2);

  class TypeConversionSet {
   public:
    TypeConversionSet() : type_conversions_(0u) {}

    void Add(DataType::Type result_type) {
      static_assert(enum_cast<>(DataType::Type::kLast) < BitSizeOf<uint32_t>());
      type_conversions_ |= 1u << enum_cast<>(result_type);
    }

    void Add(TypeConversionSet other) {
      type_conversions_ |= other.type_conversions_;
    }

    bool AreAllTypeConversionsImplicit(HInstruction* input) const {
      if (type_conversions_ != 0u) {
        if (input->IsIntConstant()) {
          int32_t value = input->AsIntConstant()->GetValue();
          for (uint32_t raw_type : LowToHighBits(type_conversions_)) {
            DataType::Type type = enum_cast<DataType::Type>(raw_type);
            if (!DataType::IsTypeConversionImplicit(value, type)) {
              return false;
            }
          }
        } else {
          DataType::Type input_type = input->GetType();
          for (uint32_t raw_type : LowToHighBits(type_conversions_)) {
            DataType::Type type = enum_cast<DataType::Type>(raw_type);
            if (!DataType::IsTypeConversionImplicit(input_type, type)) {
              return false;
            }
          }
        }
      }
      return true;
    }

   private:
    uint32_t type_conversions_;
  };

  Value SkipTypeConversions(Value value,
                            /*inout*/ TypeConversionSet* type_conversions = nullptr) const {
    while (value.NeedsConvertedLoopPhi()) {
      HInstruction* conversion_load = value.GetLoopPhiConversionLoad();
      DCHECK(!conversion_load->IsVecLoad());
      if (type_conversions != nullptr) {
        type_conversions->Add(conversion_load->GetType());
      }
      ValueRecord* prev_record = loads_requiring_loop_phi_[conversion_load->GetId()];
      DCHECK(prev_record != nullptr);
      value = prev_record->value;
    }
    return value;
  }

  // Get Phi placeholder index for access to `phi_placeholder_replacements_`
  // and "visited" bit vectors during depth-first searches.
  size_t PhiPlaceholderIndex(PhiPlaceholder phi_placeholder) const {
    size_t res =
        phi_placeholder.GetBlockId() * heap_location_collector_.GetNumberOfHeapLocations() +
        phi_placeholder.GetHeapLocation();
    DCHECK_EQ(phi_placeholder, GetPhiPlaceholderAt(res))
        << res << "blks: " << GetGraph()->GetBlocks().size()
        << " hls: " << heap_location_collector_.GetNumberOfHeapLocations();
    return res;
  }

  size_t PhiPlaceholderIndex(Value phi_placeholder) const {
    return PhiPlaceholderIndex(SkipTypeConversions(phi_placeholder).GetPhiPlaceholder());
  }

  bool IsEscapingObject(ReferenceInfo* info) { return !info->IsSingletonAndRemovable(); }

  PhiPlaceholder GetPhiPlaceholderAt(size_t off) const {
    DCHECK_LT(off, num_phi_placeholders_);
    size_t id = off % heap_location_collector_.GetNumberOfHeapLocations();
    // Technically this should be (off - id) / NumberOfHeapLocations
    // but due to truncation it's all the same.
    size_t blk_id = off / heap_location_collector_.GetNumberOfHeapLocations();
    return GetPhiPlaceholder(blk_id, id);
  }

  PhiPlaceholder GetPhiPlaceholder(uint32_t block_id, size_t idx) const {
    DCHECK(GetGraph()->GetBlocks()[block_id] != nullptr) << block_id;
    return PhiPlaceholder(block_id, idx);
  }

  Value Replacement(Value value) const {
    DCHECK(value.NeedsNonLoopPhi() || value.NeedsPlainLoopPhi())
        << value << " phase: " << current_phase_;
    Value replacement = phi_placeholder_replacements_[PhiPlaceholderIndex(value)];
    DCHECK(replacement.IsUnknown() || replacement.IsInstruction());
    DCHECK(replacement.IsUnknown() ||
           FindSubstitute(replacement.GetInstruction()) == replacement.GetInstruction());
    return replacement;
  }

  Value ReplacementOrValue(Value value) const {
    if (value.NeedsConvertedLoopPhi() &&
        substitute_instructions_for_loads_[value.GetLoopPhiConversionLoad()->GetId()] != nullptr) {
      return Value::ForInstruction(
          substitute_instructions_for_loads_[value.GetLoopPhiConversionLoad()->GetId()]);
    } else if ((value.NeedsNonLoopPhi() || value.NeedsPlainLoopPhi()) &&
               phi_placeholder_replacements_[PhiPlaceholderIndex(value)].IsValid()) {
      return Replacement(value);
    } else {
      DCHECK_IMPLIES(value.IsInstruction(),
                     FindSubstitute(value.GetInstruction()) == value.GetInstruction());
      return value;
    }
  }

  // The record of a heap value and instruction(s) that feed that value.
  struct ValueRecord {
    Value value;
    Value stored_by;
  };

  // Calculate the value stored in location `idx` for a loop Phi placeholder-dependent `load`.
  Value StoredValueForLoopPhiPlaceholderDependentLoad(size_t idx, HInstruction* load) const {
    DCHECK(IsLoad(load));
    DCHECK_LT(static_cast<size_t>(load->GetId()), loads_requiring_loop_phi_.size());
    DCHECK(loads_requiring_loop_phi_[load->GetId()] != nullptr);
    Value loaded_value = loads_requiring_loop_phi_[load->GetId()]->value;
    DCHECK(loaded_value.NeedsLoopPhi());
    DataType::Type load_type = load->GetType();
    size_t load_size = DataType::Size(load_type);
    size_t store_size = DataType::Size(heap_location_collector_.GetHeapLocation(idx)->GetType());

    if (kIsDebugBuild && load->IsVecLoad()) {
      // For vector operations, the load type is always `Float64` and therefore the store size is
      // never higher and we do not record any conversions below. This is OK because we currently
      // do not vectorize any loops with widening operations.
      CHECK_EQ(load_size, DataType::Size(DataType::Type::kFloat64));
      CHECK_LE(store_size, load_size);
      CHECK(!loaded_value.NeedsConvertedLoopPhi());
    } else if (kIsDebugBuild) {
      // There are no implicit conversions between 64-bit types and smaller types.
      // We shall not record any conversions for 64-bit types.
      CHECK_EQ(load_size == DataType::Size(DataType::Type::kInt64),
               store_size == DataType::Size(DataType::Type::kInt64));
      CHECK_IMPLIES(load_size == DataType::Size(DataType::Type::kInt64),
                    !loaded_value.NeedsConvertedLoopPhi());
    }
    // The `loaded_value` can record a conversion only if the `load` was from
    // a wider field than the previous converting load.
    DCHECK_IMPLIES(loaded_value.NeedsConvertedLoopPhi(),
                   load_size > DataType::Size(loaded_value.GetLoopPhiConversionLoad()->GetType()));

    Value value = loaded_value;
    if (load_size < store_size) {
      // Add a type conversion to a narrow type unless it's an implicit conversion
      // from an already converted value.
      if (!loaded_value.NeedsConvertedLoopPhi() ||
          !DataType::IsTypeConversionImplicit(loaded_value.GetLoopPhiConversionLoad()->GetType(),
                                              load_type)) {
        value = Value::ForConvertedLoopPhiPlaceholder(load);
      } else {
        DCHECK(value.Equals(loaded_value));
      }
    } else {
      // Remove conversions to types at least as wide as the field we're storing to.
      // We record only conversions that define sign-/zero-extension bits to store.
      while (value.NeedsConvertedLoopPhi() &&
             DataType::Size(value.GetLoopPhiConversionLoad()->GetType()) >= store_size) {
        HInstruction* conversion_load = value.GetLoopPhiConversionLoad();
        ValueRecord* prev_record =
            loads_requiring_loop_phi_[value.GetLoopPhiConversionLoad()->GetId()];
        DCHECK(prev_record != nullptr);
        value = prev_record->value;
        DCHECK(value.NeedsLoopPhi());
      }
    }

    DCHECK_EQ(PhiPlaceholderIndex(loaded_value), PhiPlaceholderIndex(value));
    return value;
  }

  HTypeConversion* FindOrAddTypeConversionIfNecessary(HInstruction* instruction,
                                                      HInstruction* value,
                                                      DataType::Type expected_type) {
    // Should never add type conversion into boolean value.
    if (expected_type == DataType::Type::kBool ||
        DataType::IsTypeConversionImplicit(value->GetType(), expected_type) ||
        // TODO: This prevents type conversion of default values but we can still insert
        // type conversion of other constants and there is no constant folding pass after LSE.
        IsZeroBitPattern(value)) {
      return nullptr;
    }

    // All vector instructions report their type as `Float64`, so the conversion is implicit.
    // This is OK because we currently do not vectorize any loops with widening operations.
    DCHECK(!instruction->IsVecLoad());

    // Check if there is already a suitable TypeConversion we can reuse.
    for (const HUseListNode<HInstruction*>& use : value->GetUses()) {
      if (use.GetUser()->IsTypeConversion() &&
          use.GetUser()->GetType() == expected_type &&
          // TODO: We could move the TypeConversion to a common dominator
          // if it does not cross irreducible loop header.
          use.GetUser()->GetBlock()->Dominates(instruction->GetBlock()) &&
          // Don't share across irreducible loop headers.
          // TODO: can be more fine-grained than this by testing each dominator.
          (use.GetUser()->GetBlock() == instruction->GetBlock() ||
           !GetGraph()->HasIrreducibleLoops())) {
        if (use.GetUser()->GetBlock() == instruction->GetBlock() &&
            use.GetUser()->GetBlock()->GetInstructions().FoundBefore(instruction, use.GetUser())) {
          // Move the TypeConversion before the instruction.
          use.GetUser()->MoveBefore(instruction);
        }
        DCHECK(use.GetUser()->StrictlyDominates(instruction));
        return use.GetUser()->AsTypeConversion();
      }
    }

    // We must create a new TypeConversion instruction.
    HTypeConversion* type_conversion = new (GetGraph()->GetAllocator()) HTypeConversion(
          expected_type, value, instruction->GetDexPc());
    instruction->GetBlock()->InsertInstructionBefore(type_conversion, instruction);
    return type_conversion;
  }

  // Find an instruction's substitute if it's a removed load.
  // Return the same instruction if it should not be removed.
  HInstruction* FindSubstitute(HInstruction* instruction) const {
    size_t id = static_cast<size_t>(instruction->GetId());
    if (id >= substitute_instructions_for_loads_.size()) {
      // New Phi (may not be in the graph yet), or default value.
      DCHECK(!IsLoad(instruction));
      return instruction;
    }
    HInstruction* substitute = substitute_instructions_for_loads_[id];
    DCHECK(substitute == nullptr || IsLoad(instruction));
    return (substitute != nullptr) ? substitute : instruction;
  }

  void AddRemovedLoad(HInstruction* load, HInstruction* heap_value) {
    DCHECK(IsLoad(load));
    DCHECK_EQ(FindSubstitute(load), load);
    DCHECK_EQ(FindSubstitute(heap_value), heap_value) <<
        "Unexpected heap_value that has a substitute " << heap_value->DebugName();

    // The load expects to load the heap value as type load->GetType().
    // However the tracked heap value may not be of that type. An explicit
    // type conversion may be needed.
    // There are actually three types involved here:
    // (1) tracked heap value's type (type A)
    // (2) heap location (field or element)'s type (type B)
    // (3) load's type (type C)
    // We guarantee that type A stored as type B and then fetched out as
    // type C is the same as casting from type A to type C directly, since
    // type B and type C will have the same size which is guaranteed in
    // HInstanceFieldGet/HStaticFieldGet/HArrayGet/HVecLoad's SetType().
    // So we only need one type conversion from type A to type C.
    HTypeConversion* type_conversion = FindOrAddTypeConversionIfNecessary(
        load, heap_value, load->GetType());

    substitute_instructions_for_loads_[load->GetId()] =
        type_conversion != nullptr ? type_conversion : heap_value;
  }

  static bool IsLoad(HInstruction* instruction) {
    // Unresolved load is not treated as a load.
    return instruction->IsInstanceFieldGet() ||
           instruction->IsStaticFieldGet() ||
           instruction->IsVecLoad() ||
           instruction->IsArrayGet();
  }

  static bool IsStore(HInstruction* instruction) {
    // Unresolved store is not treated as a store.
    return instruction->IsInstanceFieldSet() ||
           instruction->IsArraySet() ||
           instruction->IsVecStore() ||
           instruction->IsStaticFieldSet();
  }

  // Check if it is allowed to use default values or Phis for the specified load.
  static bool IsDefaultOrPhiAllowedForLoad(HInstruction* instruction) {
    DCHECK(IsLoad(instruction));
    // Using defaults for VecLoads requires to create additional vector operations.
    // As there are some issues with scheduling vector operations it is better to avoid creating
    // them.
    return !instruction->IsVecOperation();
  }

  // Keep the store referenced by the instruction, or all stores that feed a Phi placeholder.
  // This is necessary if the stored heap value can be observed.
  void KeepStores(Value value) {
    if (value.IsUnknown()) {
      return;
    }
    if (value.NeedsPhi()) {
      phi_placeholders_to_search_for_kept_stores_.SetBit(PhiPlaceholderIndex(value));
    } else {
      HInstruction* instruction = value.GetInstruction();
      DCHECK(IsStore(instruction));
      kept_stores_.SetBit(instruction->GetId());
    }
  }

  // If a heap location X may alias with heap location at `loc_index`
  // and heap_values of that heap location X holds a store, keep that store.
  // It's needed for a dependent load that's not eliminated since any store
  // that may put value into the load's heap location needs to be kept.
  void KeepStoresIfAliasedToLocation(ScopedArenaVector<ValueRecord>& heap_values,
                                     size_t loc_index) {
    for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
      if (i == loc_index) {
        // We use this function when reading a location with unknown value and
        // therefore we cannot know what exact store wrote that unknown value.
        // But we can have a phi placeholder here marking multiple stores to keep.
        DCHECK(!heap_values[i].stored_by.IsInstruction());
        KeepStores(heap_values[i].stored_by);
        heap_values[i].stored_by = Value::Unknown();
      } else if (heap_location_collector_.MayAlias(i, loc_index)) {
        KeepStores(heap_values[i].stored_by);
        heap_values[i].stored_by = Value::Unknown();
      }
    }
  }

  HInstruction* GetDefaultValue(DataType::Type type) {
    switch (type) {
      case DataType::Type::kReference:
        return GetGraph()->GetNullConstant();
      case DataType::Type::kBool:
      case DataType::Type::kUint8:
      case DataType::Type::kInt8:
      case DataType::Type::kUint16:
      case DataType::Type::kInt16:
      case DataType::Type::kInt32:
        return GetGraph()->GetIntConstant(0);
      case DataType::Type::kInt64:
        return GetGraph()->GetLongConstant(0);
      case DataType::Type::kFloat32:
        return GetGraph()->GetFloatConstant(0);
      case DataType::Type::kFloat64:
        return GetGraph()->GetDoubleConstant(0);
      default:
        UNREACHABLE();
    }
  }

  bool CanValueBeKeptIfSameAsNew(Value value,
                                 HInstruction* new_value,
                                 HInstruction* new_value_set_instr) {
    // For field/array set location operations, if the value is the same as the new_value
    // it can be kept even if aliasing happens. All aliased operations will access the same memory
    // range.
    // For vector values, this is not true. For example:
    //  packed_data = [0xA, 0xB, 0xC, 0xD];            <-- Different values in each lane.
    //  VecStore array[i  ,i+1,i+2,i+3] = packed_data;
    //  VecStore array[i+1,i+2,i+3,i+4] = packed_data; <-- We are here (partial overlap).
    //  VecLoad  vx = array[i,i+1,i+2,i+3];            <-- Cannot be eliminated because the value
    //                                                     here is not packed_data anymore.
    //
    // TODO: to allow such 'same value' optimization on vector data,
    // LSA needs to report more fine-grain MAY alias information:
    // (1) May alias due to two vector data partial overlap.
    //     e.g. a[i..i+3] and a[i+1,..,i+4].
    // (2) May alias due to two vector data may complete overlap each other.
    //     e.g. a[i..i+3] and b[i..i+3].
    // (3) May alias but the exact relationship between two locations is unknown.
    //     e.g. a[i..i+3] and b[j..j+3], where values of a,b,i,j are all unknown.
    // This 'same value' optimization can apply only on case (2).
    if (new_value_set_instr->IsVecOperation()) {
      return false;
    }

    return value.Equals(new_value);
  }

  Value PrepareLoopValue(HBasicBlock* block, size_t idx);
  Value PrepareLoopStoredBy(HBasicBlock* block, size_t idx);
  void PrepareLoopRecords(HBasicBlock* block);
  Value MergePredecessorValues(HBasicBlock* block, size_t idx);
  void MergePredecessorRecords(HBasicBlock* block);

  void MaterializeNonLoopPhis(PhiPlaceholder phi_placeholder, DataType::Type type);

  void VisitGetLocation(HInstruction* instruction, size_t idx);
  void VisitSetLocation(HInstruction* instruction, size_t idx, HInstruction* value);
  void RecordFieldInfo(const FieldInfo* info, size_t heap_loc) {
    field_infos_[heap_loc] = info;
  }

  void VisitBasicBlock(HBasicBlock* block) override;

  enum class Phase {
    kLoadElimination,
    kStoreElimination,
  };

  bool MayAliasOnBackEdge(HBasicBlock* loop_header, size_t idx1, size_t idx2) const;

  bool TryReplacingLoopPhiPlaceholderWithDefault(
      PhiPlaceholder phi_placeholder,
      DataType::Type type,
      /*inout*/ ArenaBitVector* phi_placeholders_to_materialize);
  bool TryReplacingLoopPhiPlaceholderWithSingleInput(
      PhiPlaceholder phi_placeholder,
      /*inout*/ ArenaBitVector* phi_placeholders_to_materialize);
  std::optional<PhiPlaceholder> FindLoopPhisToMaterialize(
      PhiPlaceholder phi_placeholder,
      /*out*/ ArenaBitVector* phi_placeholders_to_materialize,
      DataType::Type type,
      bool can_use_default_or_phi);
  void MaterializeTypeConversionsIfNeeded(Value value);
  bool MaterializeLoopPhis(const ScopedArenaVector<size_t>& phi_placeholder_indexes,
                           DataType::Type type);
  bool MaterializeLoopPhis(ArrayRef<const size_t> phi_placeholder_indexes, DataType::Type type);
  bool MaterializeLoopPhis(const ArenaBitVector& phi_placeholders_to_materialize,
                           DataType::Type type);
  bool FullyMaterializePhi(PhiPlaceholder phi_placeholder, DataType::Type type);
  std::optional<PhiPlaceholder> TryToMaterializeLoopPhis(PhiPlaceholder phi_placeholder,
                                                         HInstruction* load);
  void ProcessLoopPhiWithUnknownInput(PhiPlaceholder loop_phi_with_unknown_input);
  void ProcessLoadsRequiringLoopPhis();

  void SearchPhiPlaceholdersForKeptStores();
  void UpdateValueRecordForStoreElimination(/*inout*/ValueRecord* value_record);
  void FindOldValueForPhiPlaceholder(PhiPlaceholder phi_placeholder, DataType::Type type);
  void FindStoresWritingOldValues();
  void FinishFullLSE();

  void HandleAcquireLoad(HInstruction* instruction) {
    DCHECK((instruction->IsInstanceFieldGet() && instruction->AsInstanceFieldGet()->IsVolatile()) ||
           (instruction->IsStaticFieldGet() && instruction->AsStaticFieldGet()->IsVolatile()) ||
           (instruction->IsMonitorOperation() && instruction->AsMonitorOperation()->IsEnter()))
        << "Unexpected instruction " << instruction->GetId() << ": " << instruction->DebugName();

    // Acquire operations e.g. MONITOR_ENTER change the thread's view of the memory, so we must
    // invalidate all current values.
    ScopedArenaVector<ValueRecord>& heap_values =
        heap_values_for_[instruction->GetBlock()->GetBlockId()];
    for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
      KeepStores(heap_values[i].stored_by);
      heap_values[i].stored_by = Value::Unknown();
      heap_values[i].value = Value::Unknown();
    }

    // Note that there's no need to record the load as subsequent acquire loads shouldn't be
    // eliminated either.
  }

  void HandleReleaseStore(HInstruction* instruction) {
    DCHECK((instruction->IsInstanceFieldSet() && instruction->AsInstanceFieldSet()->IsVolatile()) ||
           (instruction->IsStaticFieldSet() && instruction->AsStaticFieldSet()->IsVolatile()) ||
           (instruction->IsMonitorOperation() && !instruction->AsMonitorOperation()->IsEnter()))
        << "Unexpected instruction " << instruction->GetId() << ": " << instruction->DebugName();

    // Release operations e.g. MONITOR_EXIT do not affect this thread's view of the memory, but
    // they will push the modifications for other threads to see. Therefore, we must keep the
    // stores but there's no need to clobber the value.
    ScopedArenaVector<ValueRecord>& heap_values =
        heap_values_for_[instruction->GetBlock()->GetBlockId()];
    for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
      KeepStores(heap_values[i].stored_by);
      heap_values[i].stored_by = Value::Unknown();
    }

    // Note that there's no need to record the store as subsequent release store shouldn't be
    // eliminated either.
  }

  void VisitInstanceFieldGet(HInstanceFieldGet* instruction) override {
    HInstruction* object = instruction->InputAt(0);
    if (instruction->IsVolatile()) {
      ReferenceInfo* ref_info = heap_location_collector_.FindReferenceInfoOf(
          heap_location_collector_.HuntForOriginalReference(object));
      if (!ref_info->IsSingletonAndRemovable()) {
        HandleAcquireLoad(instruction);
        return;
      }
      // Treat it as a normal load if it is a removable singleton.
    }

    const FieldInfo& field_info = instruction->GetFieldInfo();
    size_t idx = heap_location_collector_.GetFieldHeapLocation(object, &field_info);
    RecordFieldInfo(&field_info, idx);
    VisitGetLocation(instruction, idx);
  }

  void VisitInstanceFieldSet(HInstanceFieldSet* instruction) override {
    HInstruction* object = instruction->InputAt(0);
    if (instruction->IsVolatile()) {
      ReferenceInfo* ref_info = heap_location_collector_.FindReferenceInfoOf(
          heap_location_collector_.HuntForOriginalReference(object));
      if (!ref_info->IsSingletonAndRemovable()) {
        HandleReleaseStore(instruction);
        return;
      }
      // Treat it as a normal store if it is a removable singleton.
    }

    const FieldInfo& field_info = instruction->GetFieldInfo();
    HInstruction* value = instruction->InputAt(1);
    size_t idx = heap_location_collector_.GetFieldHeapLocation(object, &field_info);
    RecordFieldInfo(&field_info, idx);
    VisitSetLocation(instruction, idx, value);
  }

  void VisitStaticFieldGet(HStaticFieldGet* instruction) override {
    if (instruction->IsVolatile()) {
      HandleAcquireLoad(instruction);
      return;
    }

    const FieldInfo& field_info = instruction->GetFieldInfo();
    HInstruction* cls = instruction->InputAt(0);
    size_t idx = heap_location_collector_.GetFieldHeapLocation(cls, &field_info);
    RecordFieldInfo(&field_info, idx);
    VisitGetLocation(instruction, idx);
  }

  void VisitStaticFieldSet(HStaticFieldSet* instruction) override {
    if (instruction->IsVolatile()) {
      HandleReleaseStore(instruction);
      return;
    }

    const FieldInfo& field_info = instruction->GetFieldInfo();
    HInstruction* cls = instruction->InputAt(0);
    HInstruction* value = instruction->InputAt(1);
    size_t idx = heap_location_collector_.GetFieldHeapLocation(cls, &field_info);
    RecordFieldInfo(&field_info, idx);
    VisitSetLocation(instruction, idx, value);
  }

  void VisitMonitorOperation(HMonitorOperation* monitor_op) override {
    HInstruction* object = monitor_op->InputAt(0);
    ReferenceInfo* ref_info = heap_location_collector_.FindReferenceInfoOf(
        heap_location_collector_.HuntForOriginalReference(object));
    if (ref_info->IsSingletonAndRemovable()) {
      // If the object is a removable singleton, we know that no other threads will have
      // access to it, and we can remove the MonitorOperation instruction.
      // MONITOR_ENTER throws when encountering a null object. If `object` is a removable
      // singleton, it is guaranteed to be non-null so we don't have to worry about the NullCheck.
      DCHECK(!object->CanBeNull());
      monitor_op->GetBlock()->RemoveInstruction(monitor_op);
      MaybeRecordStat(stats_, MethodCompilationStat::kRemovedMonitorOp);
      return;
    }

    // We detected a monitor operation that we couldn't remove. See also LSEVisitor::Run().
    monitor_op->GetBlock()->GetGraph()->SetHasMonitorOperations(true);
    if (monitor_op->IsEnter()) {
      HandleAcquireLoad(monitor_op);
    } else {
      HandleReleaseStore(monitor_op);
    }
  }

  void VisitArrayGet(HArrayGet* instruction) override {
    VisitGetLocation(instruction, heap_location_collector_.GetArrayHeapLocation(instruction));
  }

  void VisitArraySet(HArraySet* instruction) override {
    size_t idx = heap_location_collector_.GetArrayHeapLocation(instruction);
    VisitSetLocation(instruction, idx, instruction->GetValue());
  }

  void VisitVecLoad(HVecLoad* instruction) override {
    DCHECK(!instruction->IsPredicated());
    VisitGetLocation(instruction, heap_location_collector_.GetArrayHeapLocation(instruction));
  }

  void VisitVecStore(HVecStore* instruction) override {
    DCHECK(!instruction->IsPredicated());
    size_t idx = heap_location_collector_.GetArrayHeapLocation(instruction);
    VisitSetLocation(instruction, idx, instruction->GetValue());
  }

  void VisitDeoptimize(HDeoptimize* instruction) override {
    // If we are in a try, even singletons are observable.
    const bool inside_a_try = instruction->GetBlock()->IsTryBlock();
    HBasicBlock* block = instruction->GetBlock();
    ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block->GetBlockId()];
    for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
      Value* stored_by = &heap_values[i].stored_by;
      if (stored_by->IsUnknown()) {
        continue;
      }
      // Stores are generally observeable after deoptimization, except
      // for singletons that don't escape in the deoptimization environment.
      bool observable = true;
      ReferenceInfo* info = heap_location_collector_.GetHeapLocation(i)->GetReferenceInfo();
      if (!inside_a_try && info->IsSingleton()) {
        HInstruction* reference = info->GetReference();
        // Finalizable objects always escape.
        const bool finalizable_object =
            reference->IsNewInstance() && reference->AsNewInstance()->IsFinalizable();
        if (!finalizable_object && !IsEscapingObject(info)) {
          // Check whether the reference for a store is used by an environment local of
          // the HDeoptimize. If not, the singleton is not observed after deoptimization.
          const HUseList<HEnvironment*>& env_uses = reference->GetEnvUses();
          observable = std::any_of(
              env_uses.begin(),
              env_uses.end(),
              [instruction](const HUseListNode<HEnvironment*>& use) {
                return use.GetUser()->GetHolder() == instruction;
              });
        }
      }
      if (observable) {
        KeepStores(*stored_by);
        *stored_by = Value::Unknown();
      }
    }
  }

  // Keep necessary stores before exiting a method via return/throw.
  void HandleExit(HBasicBlock* block, bool must_keep_stores = false) {
    ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block->GetBlockId()];
    for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
      ReferenceInfo* ref_info = heap_location_collector_.GetHeapLocation(i)->GetReferenceInfo();
      if (must_keep_stores || IsEscapingObject(ref_info)) {
        KeepStores(heap_values[i].stored_by);
        heap_values[i].stored_by = Value::Unknown();
      }
    }
  }

  void VisitReturn(HReturn* instruction) override {
    HandleExit(instruction->GetBlock());
  }

  void VisitReturnVoid(HReturnVoid* return_void) override {
    HandleExit(return_void->GetBlock());
  }

  void HandleThrowingInstruction(HInstruction* instruction) {
    DCHECK(instruction->CanThrow());
    // If we are inside of a try, singletons can become visible since we may not exit the method.
    HandleExit(instruction->GetBlock(), instruction->GetBlock()->IsTryBlock());
  }

  void VisitMethodEntryHook(HMethodEntryHook* method_entry) override {
    HandleThrowingInstruction(method_entry);
  }

  void VisitMethodExitHook(HMethodExitHook* method_exit) override {
    HandleThrowingInstruction(method_exit);
  }

  void VisitDivZeroCheck(HDivZeroCheck* div_zero_check) override {
    HandleThrowingInstruction(div_zero_check);
  }

  void VisitNullCheck(HNullCheck* null_check) override {
    HandleThrowingInstruction(null_check);
  }

  void VisitBoundsCheck(HBoundsCheck* bounds_check) override {
    HandleThrowingInstruction(bounds_check);
  }

  void VisitLoadClass(HLoadClass* load_class) override {
    if (load_class->CanThrow()) {
      HandleThrowingInstruction(load_class);
    }
  }

  void VisitLoadString(HLoadString* load_string) override {
    if (load_string->CanThrow()) {
      HandleThrowingInstruction(load_string);
    }
  }

  void VisitLoadMethodHandle(HLoadMethodHandle* load_method_handle) override {
    HandleThrowingInstruction(load_method_handle);
  }

  void VisitLoadMethodType(HLoadMethodType* load_method_type) override {
    HandleThrowingInstruction(load_method_type);
  }

  void VisitStringBuilderAppend(HStringBuilderAppend* sb_append) override {
    HandleThrowingInstruction(sb_append);
  }

  void VisitThrow(HThrow* throw_instruction) override {
    HandleThrowingInstruction(throw_instruction);
  }

  void VisitCheckCast(HCheckCast* check_cast) override {
    HandleThrowingInstruction(check_cast);
  }

  void HandleInvoke(HInstruction* instruction) {
    // If `instruction` can throw we have to presume all stores are visible.
    const bool can_throw = instruction->CanThrow();
    // If we are in a try, even singletons are observable.
    const bool can_throw_inside_a_try = can_throw && instruction->GetBlock()->IsTryBlock();
    SideEffects side_effects = instruction->GetSideEffects();
    ScopedArenaVector<ValueRecord>& heap_values =
        heap_values_for_[instruction->GetBlock()->GetBlockId()];
    for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
      ReferenceInfo* ref_info = heap_location_collector_.GetHeapLocation(i)->GetReferenceInfo();
      // We don't need to do anything if the reference has not escaped at this point.
      // This is true if we never escape.
      if (!can_throw_inside_a_try && ref_info->IsSingleton()) {
        // Singleton references cannot be seen by the callee.
      } else {
        if (can_throw || side_effects.DoesAnyRead() || side_effects.DoesAnyWrite()) {
          // Previous stores may become visible (read) and/or impossible for LSE to track (write).
          KeepStores(heap_values[i].stored_by);
          heap_values[i].stored_by = Value::Unknown();
        }
        if (side_effects.DoesAnyWrite()) {
          // The value may be clobbered.
          heap_values[i].value = Value::Unknown();
        }
      }
    }
  }

  void VisitInvoke(HInvoke* invoke) override {
    HandleInvoke(invoke);
  }

  void VisitClinitCheck(HClinitCheck* clinit) override {
    // Class initialization check can result in class initializer calling arbitrary methods.
    HandleInvoke(clinit);
  }

  void VisitUnresolvedInstanceFieldGet(HUnresolvedInstanceFieldGet* instruction) override {
    // Conservatively treat it as an invocation.
    HandleInvoke(instruction);
  }

  void VisitUnresolvedInstanceFieldSet(HUnresolvedInstanceFieldSet* instruction) override {
    // Conservatively treat it as an invocation.
    HandleInvoke(instruction);
  }

  void VisitUnresolvedStaticFieldGet(HUnresolvedStaticFieldGet* instruction) override {
    // Conservatively treat it as an invocation.
    HandleInvoke(instruction);
  }

  void VisitUnresolvedStaticFieldSet(HUnresolvedStaticFieldSet* instruction) override {
    // Conservatively treat it as an invocation.
    HandleInvoke(instruction);
  }

  void VisitNewInstance(HNewInstance* new_instance) override {
    // If we are in a try, even singletons are observable.
    const bool inside_a_try = new_instance->GetBlock()->IsTryBlock();
    ReferenceInfo* ref_info = heap_location_collector_.FindReferenceInfoOf(new_instance);
    if (ref_info == nullptr) {
      // new_instance isn't used for field accesses. No need to process it.
      return;
    }
    if (ref_info->IsSingletonAndRemovable() && !new_instance->NeedsChecks()) {
      DCHECK(!new_instance->IsFinalizable());
      // new_instance can potentially be eliminated.
      singleton_new_instances_.push_back(new_instance);
    }
    HBasicBlock* block = new_instance->GetBlock();
    ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block->GetBlockId()];
    for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
      ReferenceInfo* info = heap_location_collector_.GetHeapLocation(i)->GetReferenceInfo();
      HInstruction* ref = info->GetReference();
      size_t offset = heap_location_collector_.GetHeapLocation(i)->GetOffset();
      if (ref == new_instance) {
        if (offset >= mirror::kObjectHeaderSize ||
            MemberOffset(offset) == mirror::Object::MonitorOffset()) {
          // Instance fields except the header fields are set to default heap values.
          // The shadow$_monitor_ field is set to the default value however.
          heap_values[i].value = Value::Default();
          heap_values[i].stored_by = Value::Unknown();
        } else if (MemberOffset(offset) == mirror::Object::ClassOffset()) {
          // The shadow$_klass_ field is special and has an actual value however.
          heap_values[i].value = Value::ForInstruction(new_instance->GetLoadClass());
          heap_values[i].stored_by = Value::Unknown();
        }
      } else if (inside_a_try || IsEscapingObject(info)) {
        // Since NewInstance can throw, we presume all previous stores could be visible.
        KeepStores(heap_values[i].stored_by);
        heap_values[i].stored_by = Value::Unknown();
      }
    }
  }

  void VisitNewArray(HNewArray* new_array) override {
    // If we are in a try, even singletons are observable.
    const bool inside_a_try = new_array->GetBlock()->IsTryBlock();
    ReferenceInfo* ref_info = heap_location_collector_.FindReferenceInfoOf(new_array);
    if (ref_info == nullptr) {
      // new_array isn't used for array accesses. No need to process it.
      return;
    }
    if (ref_info->IsSingletonAndRemovable()) {
      if (new_array->GetLength()->IsIntConstant() &&
          new_array->GetLength()->AsIntConstant()->GetValue() >= 0) {
        // new_array can potentially be eliminated.
        singleton_new_instances_.push_back(new_array);
      } else {
        // new_array may throw NegativeArraySizeException. Keep it.
      }
    }
    HBasicBlock* block = new_array->GetBlock();
    ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block->GetBlockId()];
    for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
      HeapLocation* location = heap_location_collector_.GetHeapLocation(i);
      ReferenceInfo* info = location->GetReferenceInfo();
      HInstruction* ref = info->GetReference();
      if (ref == new_array && location->GetIndex() != nullptr) {
        // Array elements are set to default heap values.
        heap_values[i].value = Value::Default();
        heap_values[i].stored_by = Value::Unknown();
      } else if (inside_a_try || IsEscapingObject(info)) {
        // Since NewArray can throw, we presume all previous stores could be visible.
        KeepStores(heap_values[i].stored_by);
        heap_values[i].stored_by = Value::Unknown();
      }
    }
  }

  void VisitInstruction(HInstruction* instruction) override {
    // Throwing instructions must be handled specially.
    DCHECK(!instruction->CanThrow());
  }

  const HeapLocationCollector& heap_location_collector_;

  // Use local allocator for allocating memory.
  ScopedArenaAllocator allocator_;

  // The number of unique phi_placeholders there possibly are
  size_t num_phi_placeholders_;

  // One array of heap value records for each block.
  ScopedArenaVector<ScopedArenaVector<ValueRecord>> heap_values_for_;

  // We record loads and stores for re-processing when we find a loop Phi placeholder
  // with unknown value from a predecessor, and also for removing stores that are
  // found to be dead, i.e. not marked in `kept_stores_` at the end.
  struct LoadStoreRecord {
    HInstruction* load_or_store;
    size_t heap_location_index;
  };
  ScopedArenaVector<LoadStoreRecord> loads_and_stores_;

  // We record the substitute instructions for loads that should be
  // eliminated but may be used by heap locations. They'll be removed
  // in the end. These are indexed by the load's id.
  ScopedArenaVector<HInstruction*> substitute_instructions_for_loads_;

  // Record stores to keep in a bit vector indexed by instruction ID.
  ArenaBitVector kept_stores_;
  // When we need to keep all stores that feed a Phi placeholder, we just record the
  // index of that placeholder for processing after graph traversal.
  ArenaBitVector phi_placeholders_to_search_for_kept_stores_;

  // Loads that would require a loop Phi to replace are recorded for processing
  // later as we do not have enough information from back-edges to determine if
  // a suitable Phi can be found or created when we visit these loads.
  // This is a flat "map" indexed by the load instruction id.
  ScopedArenaVector<ValueRecord*> loads_requiring_loop_phi_;

  // For stores, record the old value records that were replaced and the stored values.
  struct StoreRecord {
    StoreRecord(ValueRecord old_value_record_in, HInstruction* stored_value_in)
        : old_value_record(old_value_record_in), stored_value(stored_value_in) {}
    ValueRecord old_value_record;
    HInstruction* stored_value;
  };
  // This is a flat "map" indexed by the store instruction id.
  ScopedArenaVector<StoreRecord*> store_records_;

  // Replacements for Phi placeholders.
  // The invalid heap value is used to mark Phi placeholders that cannot be replaced.
  ScopedArenaVector<Value> phi_placeholder_replacements_;

  ScopedArenaVector<HInstruction*> singleton_new_instances_;

  // The field infos for each heap location (if relevant).
  ScopedArenaVector<const FieldInfo*> field_infos_;

  Phase current_phase_;

  friend std::ostream& operator<<(std::ostream& os, const Value& v);
  friend std::ostream& operator<<(std::ostream& oss, const LSEVisitor::Phase& phase);

  DISALLOW_COPY_AND_ASSIGN(LSEVisitor);
};

std::ostream& operator<<(std::ostream& oss, const LSEVisitor::Phase& phase) {
  switch (phase) {
    case LSEVisitor::Phase::kLoadElimination:
      return oss << "kLoadElimination";
    case LSEVisitor::Phase::kStoreElimination:
      return oss << "kStoreElimination";
  }
}

constexpr bool operator==(const LSEVisitor::PhiPlaceholder& p1,
                          const LSEVisitor::PhiPlaceholder& p2) {
  return p1.Equals(p2);
}

constexpr bool operator==(const LSEVisitor::Value::NeedsPlainLoopPhiMarker& p1,
                          const LSEVisitor::Value::NeedsPlainLoopPhiMarker& p2) {
  return p1.phi_ == p2.phi_;
}

constexpr bool operator==(const LSEVisitor::Value::NeedsConvertedLoopPhiMarker& p1,
                          const LSEVisitor::Value::NeedsConvertedLoopPhiMarker& p2) {
  return p1.load_ == p2.load_;
}

constexpr bool operator==(const LSEVisitor::Value::NeedsNonLoopPhiMarker& p1,
                          const LSEVisitor::Value::NeedsNonLoopPhiMarker& p2) {
  return p1.phi_ == p2.phi_;
}

std::ostream& operator<<(std::ostream& oss, const LSEVisitor::PhiPlaceholder& p) {
  p.Dump(oss);
  return oss;
}

constexpr bool LSEVisitor::Value::ExactEquals(LSEVisitor::Value other) const {
  return value_ == other.value_;
}

constexpr bool LSEVisitor::Value::Equals(LSEVisitor::Value other) const {
  // Only valid values can be compared.
  DCHECK(IsValid());
  DCHECK(other.IsValid());
  if (value_ == other.value_) {
    // Note: Two unknown values are considered different.
    return !IsUnknown();
  } else {
    // Default is considered equal to zero-bit-pattern instructions.
    return (IsDefault() && other.IsInstruction() && IsZeroBitPattern(other.GetInstruction())) ||
            (other.IsDefault() && IsInstruction() && IsZeroBitPattern(GetInstruction()));
  }
}

std::ostream& LSEVisitor::Value::Dump(std::ostream& os) const {
  if (std::holds_alternative<LSEVisitor::Value::ValuelessType>(value_)) {
    switch (GetValuelessType()) {
      case ValuelessType::kDefault:
        return os << "Default";
      case ValuelessType::kUnknown:
        return os << "Unknown";
      case ValuelessType::kInvalid:
        return os << "Invalid";
    }
  } else if (IsInstruction()) {
    return os << "Instruction[id: " << GetInstruction()->GetId()
              << ", block: " << GetInstruction()->GetBlock()->GetBlockId() << "]";
  } else if (NeedsPlainLoopPhi()) {
    return os << "NeedsPlainLoopPhi[block: " << GetPhiPlaceholder().GetBlockId()
              << ", heap_loc: " << GetPhiPlaceholder().GetHeapLocation() << "]";
  } else if (NeedsConvertedLoopPhi()) {
    return os << "NeedsConvertedLoopPhi[id: " << GetLoopPhiConversionLoad()->GetId()
              << ", block: " << GetLoopPhiConversionLoad()->GetBlock()->GetBlockId() << "]";
  } else {
    return os << "NeedsNonLoopPhi[block: " << GetPhiPlaceholder().GetBlockId()
              << ", heap_loc: " << GetPhiPlaceholder().GetHeapLocation() << "]";
  }
}

std::ostream& operator<<(std::ostream& os, const LSEVisitor::Value& v) {
  return v.Dump(os);
}

LSEVisitor::LSEVisitor(HGraph* graph,
                       const HeapLocationCollector& heap_location_collector,
                       OptimizingCompilerStats* stats)
    : HGraphDelegateVisitor(graph, stats),
      heap_location_collector_(heap_location_collector),
      allocator_(graph->GetArenaStack()),
      num_phi_placeholders_(GetGraph()->GetBlocks().size() *
                            heap_location_collector_.GetNumberOfHeapLocations()),
      heap_values_for_(graph->GetBlocks().size(),
                       ScopedArenaVector<ValueRecord>(allocator_.Adapter(kArenaAllocLSE)),
                       allocator_.Adapter(kArenaAllocLSE)),
      loads_and_stores_(allocator_.Adapter(kArenaAllocLSE)),
      // We may add new instructions (default values, Phis) but we're not adding loads
      // or stores, so we shall not need to resize following vector and BitVector.
      substitute_instructions_for_loads_(
          graph->GetCurrentInstructionId(), nullptr, allocator_.Adapter(kArenaAllocLSE)),
      kept_stores_(&allocator_,
                   /*start_bits=*/graph->GetCurrentInstructionId(),
                   /*expandable=*/false,
                   kArenaAllocLSE),
      phi_placeholders_to_search_for_kept_stores_(&allocator_,
                                                  num_phi_placeholders_,
                                                  /*expandable=*/false,
                                                  kArenaAllocLSE),
      loads_requiring_loop_phi_(allocator_.Adapter(kArenaAllocLSE)),
      store_records_(allocator_.Adapter(kArenaAllocLSE)),
      phi_placeholder_replacements_(
          num_phi_placeholders_, Value::Invalid(), allocator_.Adapter(kArenaAllocLSE)),
      singleton_new_instances_(allocator_.Adapter(kArenaAllocLSE)),
      field_infos_(heap_location_collector_.GetNumberOfHeapLocations(),
                   allocator_.Adapter(kArenaAllocLSE)),
      current_phase_(Phase::kLoadElimination) {}

LSEVisitor::Value LSEVisitor::PrepareLoopValue(HBasicBlock* block, size_t idx) {
  // If the pre-header value is known (which implies that the reference dominates this
  // block), use a Phi placeholder for the value in the loop header. If all predecessors
  // are later found to have a known value, we can replace loads from this location,
  // either with the pre-header value or with a new Phi. For array locations, the index
  // may be defined inside the loop but the only known value in that case should be the
  // default value or a Phi placeholder that can be replaced only with the default value.
  HLoopInformation* loop_info = block->GetLoopInformation();
  uint32_t pre_header_block_id = loop_info->GetPreHeader()->GetBlockId();
  Value pre_header_value = ReplacementOrValue(heap_values_for_[pre_header_block_id][idx].value);
  if (pre_header_value.IsUnknown()) {
    return pre_header_value;
  }
  if (kIsDebugBuild) {
    // Check that the reference indeed dominates this loop.
    HeapLocation* location = heap_location_collector_.GetHeapLocation(idx);
    HInstruction* ref = location->GetReferenceInfo()->GetReference();
    CHECK(ref->GetBlock() != block && ref->GetBlock()->Dominates(block))
        << GetGraph()->PrettyMethod();
    // Check that the index, if defined inside the loop, tracks a default value
    // or a Phi placeholder requiring a loop Phi.
    HInstruction* index = location->GetIndex();
    if (index != nullptr && loop_info->Contains(*index->GetBlock())) {
      CHECK(pre_header_value.NeedsLoopPhi() || pre_header_value.Equals(Value::Default()))
          << GetGraph()->PrettyMethod() << " blk: " << block->GetBlockId() << " "
          << pre_header_value;
    }
  }
  PhiPlaceholder phi_placeholder = GetPhiPlaceholder(block->GetBlockId(), idx);
  return ReplacementOrValue(Value::ForPlainLoopPhiPlaceholder(phi_placeholder));
}

LSEVisitor::Value LSEVisitor::PrepareLoopStoredBy(HBasicBlock* block, size_t idx) {
  // Use the Phi placeholder for `stored_by` to make sure all incoming stores are kept
  // if the value in the location escapes. This is not applicable to singletons that are
  // defined inside the loop as they shall be dead in the loop header.
  const ReferenceInfo* ref_info = heap_location_collector_.GetHeapLocation(idx)->GetReferenceInfo();
  const HInstruction* reference = ref_info->GetReference();
  // Finalizable objects always escape.
  const bool is_finalizable =
      reference->IsNewInstance() && reference->AsNewInstance()->IsFinalizable();
  if (ref_info->IsSingleton() &&
      block->GetLoopInformation()->Contains(*reference->GetBlock()) &&
      !is_finalizable) {
    return Value::Unknown();
  }
  PhiPlaceholder phi_placeholder = GetPhiPlaceholder(block->GetBlockId(), idx);
  return Value::ForPlainLoopPhiPlaceholder(phi_placeholder);
}

void LSEVisitor::PrepareLoopRecords(HBasicBlock* block) {
  DCHECK(block->IsLoopHeader());
  int block_id = block->GetBlockId();
  HBasicBlock* pre_header = block->GetLoopInformation()->GetPreHeader();
  ScopedArenaVector<ValueRecord>& pre_header_heap_values =
      heap_values_for_[pre_header->GetBlockId()];
  size_t num_heap_locations = heap_location_collector_.GetNumberOfHeapLocations();
  DCHECK_EQ(num_heap_locations, pre_header_heap_values.size());
  ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block_id];
  DCHECK(heap_values.empty());

  // Don't eliminate loads in irreducible loops.
  if (block->GetLoopInformation()->IsIrreducible()) {
    heap_values.resize(num_heap_locations,
                       {/*value=*/Value::Unknown(), /*stored_by=*/Value::Unknown()});
    // Also keep the stores before the loop header, including in blocks that were not visited yet.
    for (size_t idx = 0u; idx != num_heap_locations; ++idx) {
      KeepStores(Value::ForPlainLoopPhiPlaceholder(GetPhiPlaceholder(block->GetBlockId(), idx)));
    }
    return;
  }

  // Fill `heap_values` based on values from pre-header.
  heap_values.reserve(num_heap_locations);
  for (size_t idx = 0u; idx != num_heap_locations; ++idx) {
    heap_values.push_back({ PrepareLoopValue(block, idx), PrepareLoopStoredBy(block, idx) });
  }
}

LSEVisitor::Value LSEVisitor::MergePredecessorValues(HBasicBlock* block, size_t idx) {
  ArrayRef<HBasicBlock* const> predecessors(block->GetPredecessors());
  DCHECK(!predecessors.empty());
  Value merged_value =
      ReplacementOrValue(heap_values_for_[predecessors[0]->GetBlockId()][idx].value);
  for (size_t i = 1u, size = predecessors.size(); i != size; ++i) {
    Value pred_value =
        ReplacementOrValue(heap_values_for_[predecessors[i]->GetBlockId()][idx].value);
    if (pred_value.Equals(merged_value)) {
      // Value is the same. No need to update our merged value.
      continue;
    } else if (pred_value.IsUnknown() || merged_value.IsUnknown()) {
      // If one is unknown and the other is not, the merged value is unknown.
      merged_value = Value::Unknown();
      break;
    } else {
      // There are conflicting known values. We may still be able to replace loads with a Phi.
      PhiPlaceholder phi_placeholder = GetPhiPlaceholder(block->GetBlockId(), idx);
      // Propagate the need for a new loop Phi from all predecessors.
      bool needs_loop_phi = merged_value.NeedsLoopPhi() || pred_value.NeedsLoopPhi();
      merged_value = ReplacementOrValue(Value::ForPhiPlaceholder(phi_placeholder, needs_loop_phi));
    }
  }
  return merged_value;
}

void LSEVisitor::MergePredecessorRecords(HBasicBlock* block) {
  if (block->IsExitBlock()) {
    // Exit block doesn't really merge values since the control flow ends in
    // its predecessors. Each predecessor needs to make sure stores are kept
    // if necessary.
    return;
  }

  ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block->GetBlockId()];
  DCHECK(heap_values.empty());
  size_t num_heap_locations = heap_location_collector_.GetNumberOfHeapLocations();
  if (block->GetPredecessors().empty() || block->IsCatchBlock()) {
    DCHECK_IMPLIES(block->GetPredecessors().empty(), block->IsEntryBlock());
    heap_values.resize(num_heap_locations,
                       {/*value=*/Value::Unknown(), /*stored_by=*/Value::Unknown()});
    return;
  }

  heap_values.reserve(num_heap_locations);
  for (size_t idx = 0u; idx != num_heap_locations; ++idx) {
    Value merged_value = MergePredecessorValues(block, idx);
    if (kIsDebugBuild) {
      if (merged_value.NeedsPhi()) {
        uint32_t block_id = merged_value.GetPhiPlaceholder().GetBlockId();
        CHECK(GetGraph()->GetBlocks()[block_id]->Dominates(block));
      } else if (merged_value.IsInstruction()) {
        CHECK(merged_value.GetInstruction()->GetBlock()->Dominates(block));
      }
    }
    ArrayRef<HBasicBlock* const> predecessors(block->GetPredecessors());
    Value merged_stored_by = heap_values_for_[predecessors[0]->GetBlockId()][idx].stored_by;
    for (size_t predecessor_idx = 1u; predecessor_idx != predecessors.size(); ++predecessor_idx) {
      uint32_t predecessor_block_id = predecessors[predecessor_idx]->GetBlockId();
      Value stored_by = heap_values_for_[predecessor_block_id][idx].stored_by;
      if ((!stored_by.IsUnknown() || !merged_stored_by.IsUnknown()) &&
          !merged_stored_by.Equals(stored_by)) {
        // Use the Phi placeholder to track that we need to keep stores from all predecessors.
        PhiPlaceholder phi_placeholder = GetPhiPlaceholder(block->GetBlockId(), idx);
        merged_stored_by = Value::ForNonLoopPhiPlaceholder(phi_placeholder);
        break;
      }
    }
    heap_values.push_back({ merged_value, merged_stored_by });
  }
}

static HInstruction* FindOrConstructNonLoopPhi(
    HBasicBlock* block,
    const ScopedArenaVector<HInstruction*>& phi_inputs,
    DataType::Type type) {
  for (HInstructionIterator phi_it(block->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
    HInstruction* phi = phi_it.Current();
    DCHECK_EQ(phi->InputCount(), phi_inputs.size());
    auto cmp = [](HInstruction* lhs, const HUserRecord<HInstruction*>& rhs) {
      return lhs == rhs.GetInstruction();
    };
    if (std::equal(phi_inputs.begin(), phi_inputs.end(), phi->GetInputRecords().begin(), cmp)) {
      return phi;
    }
  }
  ArenaAllocator* allocator = block->GetGraph()->GetAllocator();
  HPhi* phi = new (allocator) HPhi(allocator, kNoRegNumber, phi_inputs.size(), type);
  for (size_t i = 0, size = phi_inputs.size(); i != size; ++i) {
    DCHECK_NE(phi_inputs[i]->GetType(), DataType::Type::kVoid) << phi_inputs[i]->DebugName();
    phi->SetRawInputAt(i, phi_inputs[i]);
  }
  block->AddPhi(phi);
  if (type == DataType::Type::kReference) {
    // Update reference type information. Pass invalid handles, these are not used for Phis.
    ReferenceTypePropagation rtp_fixup(block->GetGraph(),
                                       Handle<mirror::DexCache>(),
                                       /* is_first_run= */ false);
    rtp_fixup.Visit(phi);
  }
  return phi;
}

void LSEVisitor::MaterializeNonLoopPhis(PhiPlaceholder phi_placeholder, DataType::Type type) {
  DCHECK(phi_placeholder_replacements_[PhiPlaceholderIndex(phi_placeholder)].IsInvalid());
  const ArenaVector<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  size_t idx = phi_placeholder.GetHeapLocation();

  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());
  // Reuse the same vector for collecting phi inputs.
  ScopedArenaVector<HInstruction*> phi_inputs(allocator.Adapter(kArenaAllocLSE));

  ScopedArenaVector<PhiPlaceholder> work_queue(allocator.Adapter(kArenaAllocLSE));
  work_queue.push_back(phi_placeholder);
  while (!work_queue.empty()) {
    PhiPlaceholder current_phi_placeholder = work_queue.back();
    if (phi_placeholder_replacements_[PhiPlaceholderIndex(current_phi_placeholder)].IsValid()) {
      // This Phi placeholder was pushed to the `work_queue` followed by another Phi placeholder
      // that directly or indirectly depends on it, so it was already processed as part of the
      // other Phi placeholder's dependencies before this one got back to the top of the stack.
      work_queue.pop_back();
      continue;
    }
    uint32_t current_block_id = current_phi_placeholder.GetBlockId();
    HBasicBlock* current_block = blocks[current_block_id];
    DCHECK_GE(current_block->GetPredecessors().size(), 2u);

    // Non-loop Phis cannot depend on a loop Phi, so we should not see any loop header here.
    // And the only way for such merged value to reach a different heap location is through
    // a load at which point we materialize the Phi. Therefore all non-loop Phi placeholders
    // seen here are tied to one heap location.
    DCHECK(!current_block->IsLoopHeader())
        << current_phi_placeholder << " phase: " << current_phase_;
    DCHECK_EQ(current_phi_placeholder.GetHeapLocation(), idx);

    phi_inputs.clear();
    for (HBasicBlock* predecessor : current_block->GetPredecessors()) {
      Value pred_value = ReplacementOrValue(heap_values_for_[predecessor->GetBlockId()][idx].value);
      DCHECK(!pred_value.IsUnknown()) << pred_value << " block " << current_block->GetBlockId()
                                      << " pred: " << predecessor->GetBlockId();
      if (pred_value.NeedsNonLoopPhi()) {
        // We need to process the Phi placeholder first.
        work_queue.push_back(pred_value.GetPhiPlaceholder());
      } else if (pred_value.IsDefault()) {
        phi_inputs.push_back(GetDefaultValue(type));
      } else {
        DCHECK(pred_value.IsInstruction()) << pred_value << " block " << current_block->GetBlockId()
                                           << " pred: " << predecessor->GetBlockId();
        phi_inputs.push_back(pred_value.GetInstruction());
      }
    }
    if (phi_inputs.size() == current_block->GetPredecessors().size()) {
      // All inputs are available. Find or construct the Phi replacement.
      phi_placeholder_replacements_[PhiPlaceholderIndex(current_phi_placeholder)] =
          Value::ForInstruction(FindOrConstructNonLoopPhi(current_block, phi_inputs, type));
      // Remove the block from the queue.
      DCHECK_EQ(current_phi_placeholder, work_queue.back());
      work_queue.pop_back();
    }
  }
}

void LSEVisitor::VisitGetLocation(HInstruction* instruction, size_t idx) {
  DCHECK_NE(idx, HeapLocationCollector::kHeapLocationNotFound);
  DCHECK_EQ(DataType::Size(heap_location_collector_.GetHeapLocation(idx)->GetType()),
            DataType::Size(instruction->IsVecLoad() ? instruction->AsVecLoad()->GetPackedType()
                                                    : instruction->GetType()));
  uint32_t block_id = instruction->GetBlock()->GetBlockId();
  ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block_id];
  ValueRecord& record = heap_values[idx];
  DCHECK(record.value.IsUnknown() || record.value.Equals(ReplacementOrValue(record.value)));
  loads_and_stores_.push_back({ instruction, idx });
  if ((record.value.IsDefault() || record.value.NeedsNonLoopPhi()) &&
      !IsDefaultOrPhiAllowedForLoad(instruction)) {
    record.value = Value::Unknown();
  }
  if (record.value.IsDefault()) {
    KeepStores(record.stored_by);
    HInstruction* constant = GetDefaultValue(instruction->GetType());
    AddRemovedLoad(instruction, constant);
    record.value = Value::ForInstruction(constant);
  } else if (record.value.IsUnknown()) {
    // Load isn't eliminated. Put the load as the value into the HeapLocation.
    // This acts like GVN but with better aliasing analysis.
    Value old_value = record.value;
    record.value = Value::ForInstruction(instruction);
    KeepStoresIfAliasedToLocation(heap_values, idx);
    KeepStores(old_value);
  } else if (record.value.NeedsLoopPhi()) {
    // We do not know yet if the value is known for all back edges. Record for future processing.
    if (loads_requiring_loop_phi_.empty()) {
      loads_requiring_loop_phi_.resize(GetGraph()->GetCurrentInstructionId(), nullptr);
    }
    DCHECK_EQ(loads_requiring_loop_phi_[instruction->GetId()], nullptr);
    loads_requiring_loop_phi_[instruction->GetId()] =
        new (allocator_.Alloc<ValueRecord>(kArenaAllocLSE)) ValueRecord(record);
  } else {
    // This load can be eliminated but we may need to construct non-loop Phis.
    if (record.value.NeedsNonLoopPhi()) {
      MaterializeNonLoopPhis(record.value.GetPhiPlaceholder(), instruction->GetType());
      record.value = Replacement(record.value);
    }
    HInstruction* heap_value = FindSubstitute(record.value.GetInstruction());
    AddRemovedLoad(instruction, heap_value);
  }
}

void LSEVisitor::VisitSetLocation(HInstruction* instruction, size_t idx, HInstruction* value) {
  DCHECK_NE(idx, HeapLocationCollector::kHeapLocationNotFound);
  DCHECK(!IsStore(value)) << value->DebugName();
  // The `value` may already have a substitute.
  value = FindSubstitute(value);
  HBasicBlock* block = instruction->GetBlock();
  ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block->GetBlockId()];
  ValueRecord& record = heap_values[idx];
  DCHECK_IMPLIES(record.value.IsInstruction(),
                 FindSubstitute(record.value.GetInstruction()) == record.value.GetInstruction());

  // Calculate the new `Value` to store to the `record`.
  Value new_value = Value::ForInstruction(value);
  // Note that the `value` can be a newly created `Phi` with an id that falls outside
  // the allocated `loads_requiring_loop_phi_` range.
  DCHECK_IMPLIES(IsLoad(value) && !loads_requiring_loop_phi_.empty(),
                 static_cast<size_t>(value->GetId()) < loads_requiring_loop_phi_.size());
  if (static_cast<size_t>(value->GetId()) < loads_requiring_loop_phi_.size() &&
      loads_requiring_loop_phi_[value->GetId()] != nullptr) {
    // Propapate the Phi placeholder or appropriate converting load to the record.
    new_value = StoredValueForLoopPhiPlaceholderDependentLoad(idx, value);
    DCHECK(new_value.NeedsLoopPhi());
  }

  if (record.value.Equals(value)) {
    // Store into the heap location with the same value.
    // This store can be eliminated right away.
    block->RemoveInstruction(instruction);
    return;
  }

  if (store_records_.empty()) {
    store_records_.resize(GetGraph()->GetCurrentInstructionId(), nullptr);
  }
  DCHECK_EQ(store_records_[instruction->GetId()], nullptr);
  store_records_[instruction->GetId()] =
      new (allocator_.Alloc<StoreRecord>(kArenaAllocLSE)) StoreRecord(record, value);
  loads_and_stores_.push_back({ instruction, idx });

  // If the `record.stored_by` specified a store from this block, it shall be removed
  // at the end, except for throwing ArraySet; it cannot be marked for keeping in
  // `kept_stores_` anymore after we update the `record.stored_by` below.
  DCHECK(!record.stored_by.IsInstruction() ||
         record.stored_by.GetInstruction()->GetBlock() != block ||
         record.stored_by.GetInstruction()->CanThrow() ||
         !kept_stores_.IsBitSet(record.stored_by.GetInstruction()->GetId()));

  if (instruction->CanThrow()) {
    // Previous stores can become visible.
    HandleThrowingInstruction(instruction);
    // We cannot remove a possibly throwing store.
    // After marking it as kept, it does not matter if we track it in `stored_by` or not.
    kept_stores_.SetBit(instruction->GetId());
  }

  // Update the record.
  record.value = new_value;
  // Track the store in the value record. If the value is loaded or needed after
  // return/deoptimization later, this store isn't really redundant.
  record.stored_by = Value::ForInstruction(instruction);

  // This store may kill values in other heap locations due to aliasing.
  for (size_t i = 0u, size = heap_values.size(); i != size; ++i) {
    if (i == idx ||
        heap_values[i].value.IsUnknown() ||
        CanValueBeKeptIfSameAsNew(heap_values[i].value, value, instruction) ||
        !heap_location_collector_.MayAlias(i, idx)) {
      continue;
    }
    // Kill heap locations that may alias and keep previous stores to these locations.
    KeepStores(heap_values[i].stored_by);
    heap_values[i].stored_by = Value::Unknown();
    heap_values[i].value = Value::Unknown();
  }
}

void LSEVisitor::VisitBasicBlock(HBasicBlock* block) {
  // Populate the heap_values array for this block.
  // TODO: try to reuse the heap_values array from one predecessor if possible.
  if (block->IsLoopHeader()) {
    PrepareLoopRecords(block);
  } else {
    MergePredecessorRecords(block);
  }
  // Visit non-Phi instructions.
  VisitNonPhiInstructions(block);
}

bool LSEVisitor::MayAliasOnBackEdge(HBasicBlock* loop_header, size_t idx1, size_t idx2) const {
  DCHECK_NE(idx1, idx2);
  DCHECK(loop_header->IsLoopHeader());
  if (heap_location_collector_.MayAlias(idx1, idx2)) {
    return true;
  }
  // For array locations with index defined inside the loop, include
  // all other locations in the array, even those that LSA declares
  // non-aliasing, such as `a[i]` and `a[i + 1]`, as they may actually
  // refer to the same locations for different iterations. (LSA's
  // `ComputeMayAlias()` does not consider different loop iterations.)
  HeapLocation* loc1 = heap_location_collector_.GetHeapLocation(idx1);
  HeapLocation* loc2 = heap_location_collector_.GetHeapLocation(idx2);
  if (loc1->IsArray() &&
      loc2->IsArray() &&
      HeapLocationCollector::CanReferencesAlias(loc1->GetReferenceInfo(),
                                                loc2->GetReferenceInfo())) {
    HLoopInformation* loop_info = loop_header->GetLoopInformation();
    if (loop_info->Contains(*loc1->GetIndex()->GetBlock()) ||
        loop_info->Contains(*loc2->GetIndex()->GetBlock())) {
      // Consider the locations aliasing. Do not optimize the case where both indexes
      // are loop invariants defined inside the loop, rely on LICM to pull them out.
      return true;
    }
  }
  return false;
}

bool LSEVisitor::TryReplacingLoopPhiPlaceholderWithDefault(
    PhiPlaceholder phi_placeholder,
    DataType::Type type,
    /*inout*/ ArenaBitVector* phi_placeholders_to_materialize) {
  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());
  ArenaBitVector visited(&allocator,
                         /*start_bits=*/ num_phi_placeholders_,
                         /*expandable=*/ false,
                         kArenaAllocLSE);
  ScopedArenaVector<PhiPlaceholder> work_queue(allocator.Adapter(kArenaAllocLSE));

  auto maybe_add_to_work_queue = [&](Value predecessor_value) {
    // Visit the predecessor Phi placeholder if it's not visited yet.
    DCHECK(predecessor_value.NeedsNonLoopPhi() || predecessor_value.NeedsPlainLoopPhi());
    PhiPlaceholder predecessor_phi_placeholder = predecessor_value.GetPhiPlaceholder();
    if (!visited.IsBitSet(PhiPlaceholderIndex(predecessor_phi_placeholder))) {
      visited.SetBit(PhiPlaceholderIndex(predecessor_phi_placeholder));
      work_queue.push_back(predecessor_phi_placeholder);
    }
  };

  // Use depth first search to check if any non-Phi input is unknown.
  const ArenaVector<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  size_t num_heap_locations = heap_location_collector_.GetNumberOfHeapLocations();
  visited.SetBit(PhiPlaceholderIndex(phi_placeholder));
  work_queue.push_back(phi_placeholder);
  while (!work_queue.empty()) {
    PhiPlaceholder current_phi_placeholder = work_queue.back();
    work_queue.pop_back();
    HBasicBlock* block = blocks[current_phi_placeholder.GetBlockId()];
    DCHECK_GE(block->GetPredecessors().size(), 2u);
    size_t idx = current_phi_placeholder.GetHeapLocation();
    for (HBasicBlock* predecessor : block->GetPredecessors()) {
      Value value = ReplacementOrValue(heap_values_for_[predecessor->GetBlockId()][idx].value);
      // Skip over type conversions (these are unnecessary for the default value).
      value = SkipTypeConversions(value);
      if (value.NeedsPhi()) {
        maybe_add_to_work_queue(value);
      } else if (!value.Equals(Value::Default())) {
        return false;  // Report failure.
      }
    }
    if (block->IsLoopHeader()) {
      // For back-edges we need to check all locations that write to the same array,
      // even those that LSA declares non-aliasing, such as `a[i]` and `a[i + 1]`
      // as they may actually refer to the same locations for different iterations.
      for (size_t i = 0; i != num_heap_locations; ++i) {
        if (i == idx ||
            heap_location_collector_.GetHeapLocation(i)->GetReferenceInfo() !=
                heap_location_collector_.GetHeapLocation(idx)->GetReferenceInfo()) {
          continue;
        }
        for (HBasicBlock* predecessor : block->GetPredecessors()) {
          // Check if there were any writes to this location.
          // Note: We could simply process the values but due to the vector operation
          // carve-out (see `IsDefaultOrPhiAllowedForLoad()`), a vector load can cause
          // the value to change and not be equal to default. To work around this and
          // allow replacing the non-vector load of loop-invariant default values
          // anyway, skip over paths that do not have any writes.
          ValueRecord record = heap_values_for_[predecessor->GetBlockId()][i];
          while (record.stored_by.NeedsPlainLoopPhi() &&
                 blocks[record.stored_by.GetPhiPlaceholder().GetBlockId()]->IsLoopHeader()) {
            HLoopInformation* loop_info =
                blocks[record.stored_by.GetPhiPlaceholder().GetBlockId()]->GetLoopInformation();
            record = heap_values_for_[loop_info->GetPreHeader()->GetBlockId()][i];
          }
          DCHECK(!record.stored_by.NeedsConvertedLoopPhi());
          Value value = ReplacementOrValue(record.value);
          // Skip over type conversions (these are unnecessary for the default value).
          value = SkipTypeConversions(value);
          if (value.NeedsPhi()) {
            maybe_add_to_work_queue(value);
          } else if (!value.Equals(Value::Default())) {
            return false;  // Report failure.
          }
        }
      }
    }
  }

  // Record replacement and report success.
  HInstruction* replacement = GetDefaultValue(type);
  for (uint32_t phi_placeholder_index : visited.Indexes()) {
    DCHECK(phi_placeholder_replacements_[phi_placeholder_index].IsInvalid());
    PhiPlaceholder curr = GetPhiPlaceholderAt(phi_placeholder_index);
    HeapLocation* hl = heap_location_collector_.GetHeapLocation(curr.GetHeapLocation());
    // We use both vector and non vector operations to analyze the information. However, we replace
    // only non vector operations in this code path.
    if (!hl->IsVecOp()) {
      phi_placeholder_replacements_[phi_placeholder_index] = Value::ForInstruction(replacement);
      phi_placeholders_to_materialize->ClearBit(phi_placeholder_index);
    }
  }
  return true;
}

bool LSEVisitor::TryReplacingLoopPhiPlaceholderWithSingleInput(
    PhiPlaceholder phi_placeholder,
    /*inout*/ ArenaBitVector* phi_placeholders_to_materialize) {
  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());
  ArenaBitVector visited(&allocator,
                         /*start_bits=*/ num_phi_placeholders_,
                         /*expandable=*/ false,
                         kArenaAllocLSE);
  ScopedArenaVector<PhiPlaceholder> work_queue(allocator.Adapter(kArenaAllocLSE));

  TypeConversionSet type_conversions;

  // Use depth first search to check if any non-Phi input is unknown.
  HInstruction* replacement = nullptr;
  const ArenaVector<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  visited.SetBit(PhiPlaceholderIndex(phi_placeholder));
  work_queue.push_back(phi_placeholder);
  while (!work_queue.empty()) {
    PhiPlaceholder current_phi_placeholder = work_queue.back();
    work_queue.pop_back();
    HBasicBlock* current_block = blocks[current_phi_placeholder.GetBlockId()];
    DCHECK_GE(current_block->GetPredecessors().size(), 2u);
    size_t idx = current_phi_placeholder.GetHeapLocation();
    for (HBasicBlock* predecessor : current_block->GetPredecessors()) {
      Value value = ReplacementOrValue(heap_values_for_[predecessor->GetBlockId()][idx].value);
      // Skip type conversions but record them for checking later.
      value = SkipTypeConversions(value, &type_conversions);
      if (value.NeedsPhi()) {
        // Visit the predecessor Phi placeholder if it's not visited yet.
        if (!visited.IsBitSet(PhiPlaceholderIndex(value))) {
          visited.SetBit(PhiPlaceholderIndex(value));
          work_queue.push_back(value.GetPhiPlaceholder());
        }
      } else {
        if (!value.IsInstruction() ||
            (replacement != nullptr && replacement != value.GetInstruction())) {
          return false;  // Report failure.
        }
        replacement = value.GetInstruction();
      }
    }
    // While `TryReplacingLoopPhiPlaceholderWithDefault()` has special treatment
    // for back-edges, it is not needed here. When looking for a single input
    // instruction coming from before the loop, the array index must also be
    // defined before the loop and the aliasing analysis done by LSA is sufficient.
    // Any writes of a different value with an index that is not loop invariant
    // would invalidate the heap location in `VisitSetLocation()`.
  }

  // Check that there are no type conversions that would change the stored value.
  DCHECK(replacement != nullptr);
  if (!type_conversions.AreAllTypeConversionsImplicit(replacement)) {
    return false;
  }

  // Record replacement and report success.
  // Note: Replacements for the loads where we skipped type conversions above (and do not really
  // need the type conversion) shall be recorded later, either when we process the loads in
  // `ProcessLoadsRequiringLoopPhis()` or when needed to materialize another Phi.
  for (uint32_t phi_placeholder_index : visited.Indexes()) {
    DCHECK(phi_placeholder_replacements_[phi_placeholder_index].IsInvalid());
    PhiPlaceholder curr = GetPhiPlaceholderAt(phi_placeholder_index);
    HeapLocation* hl = heap_location_collector_.GetHeapLocation(curr.GetHeapLocation());
    // We use both vector and non vector operations to analyze the information. However, we replace
    // only vector operations in this code path.
    if (hl->IsVecOp()) {
      phi_placeholder_replacements_[phi_placeholder_index] = Value::ForInstruction(replacement);
      phi_placeholders_to_materialize->ClearBit(phi_placeholder_index);
    }
  }
  return true;
}

std::optional<LSEVisitor::PhiPlaceholder> LSEVisitor::FindLoopPhisToMaterialize(
    PhiPlaceholder phi_placeholder,
    /*inout*/ ArenaBitVector* phi_placeholders_to_materialize,
    DataType::Type type,
    bool can_use_default_or_phi) {
  DCHECK(phi_placeholder_replacements_[PhiPlaceholderIndex(phi_placeholder)].IsInvalid());

  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());
  ScopedArenaVector<PhiPlaceholder> work_queue(allocator.Adapter(kArenaAllocLSE));

  // Use depth first search to check if any non-Phi input is unknown.
  const ArenaVector<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  phi_placeholders_to_materialize->ClearAllBits();
  phi_placeholders_to_materialize->SetBit(PhiPlaceholderIndex(phi_placeholder));
  work_queue.push_back(phi_placeholder);
  while (!work_queue.empty()) {
    PhiPlaceholder current_phi_placeholder = work_queue.back();
    work_queue.pop_back();
    if (!phi_placeholders_to_materialize->IsBitSet(PhiPlaceholderIndex(current_phi_placeholder))) {
      // Replaced by `TryReplacingLoopPhiPlaceholderWith{Default,SingleInput}()`.
      DCHECK(phi_placeholder_replacements_[PhiPlaceholderIndex(current_phi_placeholder)].Equals(
                 Value::Default()));
      continue;
    }
    HBasicBlock* current_block = blocks[current_phi_placeholder.GetBlockId()];
    DCHECK_GE(current_block->GetPredecessors().size(), 2u);
    size_t idx = current_phi_placeholder.GetHeapLocation();
    if (current_block->IsLoopHeader()) {
      // If the index is defined inside the loop, it may reference different elements of the
      // array on each iteration. Since we do not track if all elements of an array are set
      // to the same value explicitly, the only known value in pre-header can be the default
      // value from NewArray or a Phi placeholder depending on a default value from some outer
      // loop pre-header. This Phi placeholder can be replaced only by the default value.
      HInstruction* index = heap_location_collector_.GetHeapLocation(idx)->GetIndex();
      if (index != nullptr && current_block->GetLoopInformation()->Contains(*index->GetBlock())) {
        if (can_use_default_or_phi &&
            TryReplacingLoopPhiPlaceholderWithDefault(current_phi_placeholder,
                                                      type,
                                                      phi_placeholders_to_materialize)) {
          continue;
        } else {
          return current_phi_placeholder;  // Report the loop Phi placeholder.
        }
      }
      // A similar situation arises with the index defined outside the loop if we cannot use
      // default values or Phis, i.e. for vector loads, as we can only replace the Phi
      // placeholder with a single instruction defined before the loop.
      if (!can_use_default_or_phi) {
        DCHECK(index != nullptr);  // Vector operations are array operations.
        if (TryReplacingLoopPhiPlaceholderWithSingleInput(current_phi_placeholder,
                                                          phi_placeholders_to_materialize)) {
          continue;
        } else {
          return current_phi_placeholder;  // Report the loop Phi placeholder.
        }
      }
    }
    for (HBasicBlock* predecessor : current_block->GetPredecessors()) {
      ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[predecessor->GetBlockId()];
      Value value = ReplacementOrValue(heap_values[idx].value);
      if (value.IsUnknown()) {
        // We cannot create a Phi for this loop Phi placeholder.
        return current_phi_placeholder;  // Report the loop Phi placeholder.
      }
      // For arrays, the location may have been clobbered by writes to other locations
      // in a loop that LSA does not consider aliasing, such as `a[i]` and `a[i + 1]`.
      if (current_block->IsLoopHeader() &&
          predecessor != current_block->GetLoopInformation()->GetPreHeader() &&
          heap_location_collector_.GetHeapLocation(idx)->GetIndex() != nullptr) {
        for (size_t i = 0, size = heap_values.size(); i != size; ++i) {
          if (i != idx &&
              !heap_values[i].stored_by.IsUnknown() &&
              MayAliasOnBackEdge(current_block, idx, i)) {
            // We cannot create a Phi for this loop Phi placeholder.
            return current_phi_placeholder;
          }
        }
      }
      // Skip type conversions. We're looking for the Phi placeholders now.
      value = SkipTypeConversions(value);
      if (value.NeedsPlainLoopPhi()) {
        // Visit the predecessor Phi placeholder if it's not visited yet.
        if (!phi_placeholders_to_materialize->IsBitSet(PhiPlaceholderIndex(value))) {
          phi_placeholders_to_materialize->SetBit(PhiPlaceholderIndex(value));
          work_queue.push_back(value.GetPhiPlaceholder());
          LSE_VLOG << "For materialization of " << phi_placeholder
                   << " we need to materialize " << value;
        }
      }
    }
  }

  // There are no unknown values feeding this Phi, so we can construct the Phis if needed.
  return std::nullopt;
}

void LSEVisitor::MaterializeTypeConversionsIfNeeded(Value value) {
  if (!value.NeedsConvertedLoopPhi()) {
    return;
  }
  // There are at most 2 conversions (Uint8+Int16 or Int8+Uint16). Conversion to Int32
  // is implicit and conversions to same or smaller size replace previous conversions.
  static constexpr size_t kMaxConversionLoads = 2u;
  HInstruction* conversion_loads[kMaxConversionLoads];
  size_t num_conversion_loads = 0u;
  do {
    DCHECK_LT(num_conversion_loads, kMaxConversionLoads);
    HInstruction* conversion_load = value.GetLoopPhiConversionLoad();
    DCHECK(!conversion_load->IsVecLoad());
    HInstruction* substitute = FindSubstitute(conversion_load);
    if (substitute != conversion_load) {
      value = Value::ForInstruction(substitute);
      break;
    }
    conversion_loads[num_conversion_loads] = conversion_load;
    ++num_conversion_loads;
    ValueRecord* prev_record = loads_requiring_loop_phi_[conversion_load->GetId()];
    DCHECK(prev_record != nullptr);
    value = prev_record->value;
  } while (value.NeedsConvertedLoopPhi());
  value = value.NeedsPlainLoopPhi() ? Replacement(value) : value;
  HInstruction* replacement = value.GetInstruction();
  ArrayRef<HInstruction*> conversion_loads_array(conversion_loads, num_conversion_loads);
  for (HInstruction* conversion_load : ReverseRange(conversion_loads_array)) {
    AddRemovedLoad(conversion_load, replacement);
    replacement = substitute_instructions_for_loads_[conversion_load->GetId()];
    DCHECK(replacement != nullptr);
    DCHECK(replacement->IsTypeConversion());
  }
}

bool LSEVisitor::MaterializeLoopPhis(const ScopedArenaVector<size_t>& phi_placeholder_indexes,
                                     DataType::Type type) {
  return MaterializeLoopPhis(ArrayRef<const size_t>(phi_placeholder_indexes), type);
}

bool LSEVisitor::MaterializeLoopPhis(ArrayRef<const size_t> phi_placeholder_indexes,
                                     DataType::Type type) {
  // Materialize all predecessors that do not need a loop Phi and determine if all inputs
  // other than loop Phis are the same.
  const ArenaVector<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  TypeConversionSet type_conversions;
  std::optional<Value> other_value = std::nullopt;
  for (size_t phi_placeholder_index : phi_placeholder_indexes) {
    PhiPlaceholder phi_placeholder = GetPhiPlaceholderAt(phi_placeholder_index);
    HBasicBlock* block = blocks[phi_placeholder.GetBlockId()];
    DCHECK_GE(block->GetPredecessors().size(), 2u);
    size_t idx = phi_placeholder.GetHeapLocation();
    for (HBasicBlock* predecessor : block->GetPredecessors()) {
      Value value = ReplacementOrValue(heap_values_for_[predecessor->GetBlockId()][idx].value);
      if (value.NeedsNonLoopPhi()) {
        DCHECK(current_phase_ == Phase::kLoadElimination) << current_phase_;
        MaterializeNonLoopPhis(value.GetPhiPlaceholder(), type);
        value = Replacement(value);
      } else if (value.NeedsConvertedLoopPhi()) {
        TypeConversionSet local_type_conversions;
        Value without_conversions = SkipTypeConversions(value, &local_type_conversions);
        DCHECK(!without_conversions.NeedsNonLoopPhi());  // Would have been already materialized.
        if (without_conversions.NeedsPlainLoopPhi()) {
          type_conversions.Add(local_type_conversions);
          value = without_conversions;
        } else {
          MaterializeTypeConversionsIfNeeded(value);
          value = ReplacementOrValue(value);
        }
      }
      if (!value.NeedsPlainLoopPhi()) {
        if (!other_value) {
          // The first other value we found.
          other_value = value;
        } else if (!other_value->IsInvalid()) {
          // Check if the current `value` differs from the previous `other_value`.
          if (!value.Equals(*other_value)) {
            other_value = Value::Invalid();
          }
        }
      }
    }
  }

  DCHECK(other_value.has_value());
  DCHECK(other_value->IsInvalid() || other_value->IsDefault() || other_value->IsInstruction());
  if (other_value->IsDefault() ||  // Default value does not need type conversions.
      (other_value->IsInstruction() &&
          type_conversions.AreAllTypeConversionsImplicit(other_value->GetInstruction()))) {
    HInstruction* replacement =
        (other_value->IsDefault()) ? GetDefaultValue(type) : other_value->GetInstruction();
    DCHECK(type_conversions.AreAllTypeConversionsImplicit(replacement));
    for (size_t phi_placeholder_index : phi_placeholder_indexes) {
      phi_placeholder_replacements_[phi_placeholder_index] = Value::ForInstruction(replacement);
    }
    return true;
  }

  // If we're materializing only a single Phi, try to match it with an existing Phi.
  // (Matching multiple Phis would need investigation. It may be prohibitively slow.)
  // This also covers the case when after replacing a previous set of Phi placeholders,
  // we continue with a Phi placeholder that does not really need a loop Phi anymore.
  if (phi_placeholder_indexes.size() == 1u) {
    PhiPlaceholder phi_placeholder = GetPhiPlaceholderAt(phi_placeholder_indexes[0]);
    size_t idx = phi_placeholder.GetHeapLocation();
    HBasicBlock* block = GetGraph()->GetBlocks()[phi_placeholder.GetBlockId()];
    ArrayRef<HBasicBlock* const> predecessors(block->GetPredecessors());
    for (HInstructionIterator phi_it(block->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
      HInstruction* phi = phi_it.Current();
      DCHECK_EQ(phi->InputCount(), predecessors.size());
      ArrayRef<HUserRecord<HInstruction*>> phi_inputs = phi->GetInputRecords();
      auto cmp = [=, this](const HUserRecord<HInstruction*>& lhs, HBasicBlock* rhs) {
        Value value = ReplacementOrValue(heap_values_for_[rhs->GetBlockId()][idx].value);
        HInstruction* lhs_instruction = lhs.GetInstruction();
        while (value.NeedsConvertedLoopPhi()) {
          HInstruction* conversion_load = value.GetLoopPhiConversionLoad();
          if (!lhs_instruction->IsTypeConversion() ||
              lhs_instruction->GetType() != conversion_load->GetType()) {
            return false;
          }
          lhs_instruction = lhs_instruction->InputAt(0);
          ValueRecord* prev_record = loads_requiring_loop_phi_[conversion_load->GetId()];
          DCHECK(prev_record != nullptr);
          value = prev_record->value;
        }
        if (value.NeedsPlainLoopPhi() && value.GetPhiPlaceholder().Equals(phi_placeholder)) {
          return lhs_instruction == phi;
        } else {
          value = ReplacementOrValue(value);
          DCHECK(value.IsDefault() || value.IsInstruction());
          return value.Equals(lhs_instruction);
        }
      };
      if (std::equal(phi_inputs.begin(), phi_inputs.end(), predecessors.begin(), cmp)) {
        phi_placeholder_replacements_[phi_placeholder_indexes[0]] = Value::ForInstruction(phi);
        return true;
      }
    }
  }

  if (current_phase_ == Phase::kStoreElimination) {
    // We're not creating Phis during the final store elimination phase.
    return false;
  }

  // There are different inputs to the Phi chain. Create the Phis.
  ArenaAllocator* allocator = GetGraph()->GetAllocator();
  for (size_t phi_placeholder_index : phi_placeholder_indexes) {
    PhiPlaceholder phi_placeholder = GetPhiPlaceholderAt(phi_placeholder_index);
    HBasicBlock* block = blocks[phi_placeholder.GetBlockId()];
    CHECK_GE(block->GetPredecessors().size(), 2u);
    phi_placeholder_replacements_[phi_placeholder_index] = Value::ForInstruction(
        new (allocator) HPhi(allocator, kNoRegNumber, block->GetPredecessors().size(), type));
  }
  // Fill the Phi inputs.
  for (size_t phi_placeholder_index : phi_placeholder_indexes) {
    PhiPlaceholder phi_placeholder = GetPhiPlaceholderAt(phi_placeholder_index);
    HBasicBlock* block = blocks[phi_placeholder.GetBlockId()];
    size_t idx = phi_placeholder.GetHeapLocation();
    HInstruction* phi = phi_placeholder_replacements_[phi_placeholder_index].GetInstruction();
    DCHECK(DataType::IsTypeConversionImplicit(type, phi->GetType()))
        << "type=" << type << " vs phi-type=" << phi->GetType();
    for (size_t i = 0, size = block->GetPredecessors().size(); i != size; ++i) {
      HBasicBlock* predecessor = block->GetPredecessors()[i];
      Value predecessor_value = heap_values_for_[predecessor->GetBlockId()][idx].value;
      MaterializeTypeConversionsIfNeeded(predecessor_value);
      Value value = ReplacementOrValue(predecessor_value);
      HInstruction* input = value.IsDefault() ? GetDefaultValue(type) : value.GetInstruction();
      DCHECK_NE(input->GetType(), DataType::Type::kVoid);
      phi->SetRawInputAt(i, input);
      DCHECK(DataType::IsTypeConversionImplicit(input->GetType(), phi->GetType()))
          << " input: " << input->GetType() << value << " phi: " << phi->GetType()
          << " request: " << type;
    }
  }
  // Add the Phis to their blocks.
  for (size_t phi_placeholder_index : phi_placeholder_indexes) {
    PhiPlaceholder phi_placeholder = GetPhiPlaceholderAt(phi_placeholder_index);
    HBasicBlock* block = blocks[phi_placeholder.GetBlockId()];
    block->AddPhi(phi_placeholder_replacements_[phi_placeholder_index].GetInstruction()->AsPhi());
  }
  if (type == DataType::Type::kReference) {
    ScopedArenaAllocator local_allocator(allocator_.GetArenaStack());
    ScopedArenaVector<HInstruction*> phis(local_allocator.Adapter(kArenaAllocLSE));
    for (size_t phi_placeholder_index : phi_placeholder_indexes) {
      phis.push_back(phi_placeholder_replacements_[phi_placeholder_index].GetInstruction());
    }
    // Update reference type information. Pass invalid handles, these are not used for Phis.
    ReferenceTypePropagation rtp_fixup(GetGraph(),
                                       Handle<mirror::DexCache>(),
                                       /* is_first_run= */ false);
    rtp_fixup.Visit(ArrayRef<HInstruction* const>(phis));
  }

  return true;
}

bool LSEVisitor::MaterializeLoopPhis(const ArenaBitVector& phi_placeholders_to_materialize,
                                     DataType::Type type) {
  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());

  // We want to recognize when a subset of these loop Phis that do not need other
  // loop Phis, i.e. a transitive closure, has only one other instruction as an input,
  // i.e. that instruction can be used instead of each Phi in the set. See for example
  // Main.testLoop{5,6,7,8}() in the test 530-checker-lse. To do that, we shall
  // materialize these loop Phis from the smallest transitive closure.

  // Construct a matrix of loop phi placeholder dependencies. To reduce the memory usage,
  // assign new indexes to the Phi placeholders, making the matrix dense.
  ScopedArenaVector<size_t> matrix_indexes(num_phi_placeholders_,
                                           static_cast<size_t>(-1),  // Invalid.
                                           allocator.Adapter(kArenaAllocLSE));
  ScopedArenaVector<size_t> phi_placeholder_indexes(allocator.Adapter(kArenaAllocLSE));
  size_t num_phi_placeholders = phi_placeholders_to_materialize.NumSetBits();
  phi_placeholder_indexes.reserve(num_phi_placeholders);
  for (uint32_t marker_index : phi_placeholders_to_materialize.Indexes()) {
    matrix_indexes[marker_index] = phi_placeholder_indexes.size();
    phi_placeholder_indexes.push_back(marker_index);
  }
  const ArenaVector<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  ScopedArenaVector<ArenaBitVector*> dependencies(allocator.Adapter(kArenaAllocLSE));
  dependencies.reserve(num_phi_placeholders);
  for (size_t matrix_index = 0; matrix_index != num_phi_placeholders; ++matrix_index) {
    static constexpr bool kExpandable = false;
    dependencies.push_back(
        ArenaBitVector::Create(&allocator, num_phi_placeholders, kExpandable, kArenaAllocLSE));
    ArenaBitVector* current_dependencies = dependencies.back();
    current_dependencies->SetBit(matrix_index);  // Count the Phi placeholder as its own dependency.
    PhiPlaceholder current_phi_placeholder =
        GetPhiPlaceholderAt(phi_placeholder_indexes[matrix_index]);
    HBasicBlock* current_block = blocks[current_phi_placeholder.GetBlockId()];
    DCHECK_GE(current_block->GetPredecessors().size(), 2u);
    size_t idx = current_phi_placeholder.GetHeapLocation();
    for (HBasicBlock* predecessor : current_block->GetPredecessors()) {
      Value pred_value = ReplacementOrValue(heap_values_for_[predecessor->GetBlockId()][idx].value);
      if (pred_value.NeedsLoopPhi()) {
        size_t pred_value_index = PhiPlaceholderIndex(pred_value);
        DCHECK(phi_placeholder_replacements_[pred_value_index].IsInvalid());
        DCHECK_NE(matrix_indexes[pred_value_index], static_cast<size_t>(-1));
        current_dependencies->SetBit(matrix_indexes[PhiPlaceholderIndex(pred_value)]);
      }
    }
  }

  // Use the Floyd-Warshall algorithm to determine all transitive dependencies.
  for (size_t k = 0; k != num_phi_placeholders; ++k) {
    for (size_t i = 0; i != num_phi_placeholders; ++i) {
      for (size_t j = 0; j != num_phi_placeholders; ++j) {
        if (dependencies[i]->IsBitSet(k) && dependencies[k]->IsBitSet(j)) {
          dependencies[i]->SetBit(j);
        }
      }
    }
  }

  // Count the number of transitive dependencies for each replaceable Phi placeholder.
  ScopedArenaVector<size_t> num_dependencies(allocator.Adapter(kArenaAllocLSE));
  num_dependencies.reserve(num_phi_placeholders);
  for (size_t matrix_index = 0; matrix_index != num_phi_placeholders; ++matrix_index) {
    num_dependencies.push_back(dependencies[matrix_index]->NumSetBits());
  }

  // Pick a Phi placeholder with the smallest number of transitive dependencies and
  // materialize it and its dependencies. Repeat until we have materialized all.
  ScopedArenaVector<size_t> current_subset(allocator.Adapter(kArenaAllocLSE));
  current_subset.reserve(num_phi_placeholders);
  size_t remaining_phi_placeholders = num_phi_placeholders;
  while (remaining_phi_placeholders != 0u) {
    auto it = std::min_element(num_dependencies.begin(), num_dependencies.end());
    DCHECK_LE(*it, remaining_phi_placeholders);
    size_t current_matrix_index = std::distance(num_dependencies.begin(), it);
    ArenaBitVector* current_dependencies = dependencies[current_matrix_index];
    size_t current_num_dependencies = num_dependencies[current_matrix_index];
    current_subset.clear();
    for (uint32_t matrix_index : current_dependencies->Indexes()) {
      current_subset.push_back(phi_placeholder_indexes[matrix_index]);
    }
    if (!MaterializeLoopPhis(current_subset, type)) {
      DCHECK_EQ(current_phase_, Phase::kStoreElimination);
      // This is the final store elimination phase and we shall not be able to eliminate any
      // stores that depend on the current subset, so mark these Phi placeholders unreplaceable.
      for (uint32_t matrix_index = 0; matrix_index != num_phi_placeholders; ++matrix_index) {
        if (dependencies[matrix_index]->IsBitSet(current_matrix_index)) {
          DCHECK(phi_placeholder_replacements_[phi_placeholder_indexes[matrix_index]].IsInvalid());
          phi_placeholder_replacements_[phi_placeholder_indexes[matrix_index]] = Value::Unknown();
        }
      }
      return false;
    }
    for (uint32_t matrix_index = 0; matrix_index != num_phi_placeholders; ++matrix_index) {
      if (current_dependencies->IsBitSet(matrix_index)) {
        // Mark all dependencies as done by incrementing their `num_dependencies[.]`,
        // so that they shall never be the minimum again.
        num_dependencies[matrix_index] = num_phi_placeholders;
      } else if (dependencies[matrix_index]->IsBitSet(current_matrix_index)) {
        // Remove dependencies from other Phi placeholders.
        dependencies[matrix_index]->Subtract(current_dependencies);
        num_dependencies[matrix_index] -= current_num_dependencies;
      }
    }
    remaining_phi_placeholders -= current_num_dependencies;
  }
  return true;
}

bool LSEVisitor::FullyMaterializePhi(PhiPlaceholder phi_placeholder, DataType::Type type) {
  ScopedArenaAllocator saa(GetGraph()->GetArenaStack());
  ArenaBitVector abv(&saa, num_phi_placeholders_, false, ArenaAllocKind::kArenaAllocLSE);
  auto res =
      FindLoopPhisToMaterialize(phi_placeholder, &abv, type, /* can_use_default_or_phi=*/true);
  CHECK(!res.has_value()) << *res;
  return MaterializeLoopPhis(abv, type);
}

std::optional<LSEVisitor::PhiPlaceholder> LSEVisitor::TryToMaterializeLoopPhis(
    PhiPlaceholder phi_placeholder, HInstruction* load) {
  DCHECK(phi_placeholder_replacements_[PhiPlaceholderIndex(phi_placeholder)].IsInvalid());

  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());

  // Find Phi placeholders to materialize.
  ArenaBitVector phi_placeholders_to_materialize(
      &allocator, num_phi_placeholders_, /*expandable=*/ false, kArenaAllocLSE);
  DataType::Type type = load->GetType();
  bool can_use_default_or_phi = IsDefaultOrPhiAllowedForLoad(load);
  std::optional<PhiPlaceholder> loop_phi_with_unknown_input = FindLoopPhisToMaterialize(
      phi_placeholder, &phi_placeholders_to_materialize, type, can_use_default_or_phi);
  if (loop_phi_with_unknown_input) {
    DCHECK_GE(GetGraph()
                  ->GetBlocks()[loop_phi_with_unknown_input->GetBlockId()]
                  ->GetPredecessors()
                  .size(),
              2u);
    return loop_phi_with_unknown_input;  // Return failure.
  }

  DCHECK_EQ(current_phase_, Phase::kLoadElimination);
  bool success = MaterializeLoopPhis(phi_placeholders_to_materialize, type);
  DCHECK(success);

  // Report success.
  return std::nullopt;
}

// Re-process loads and stores in successors from the `loop_phi_with_unknown_input`. This may
// find one or more loads from `loads_requiring_loop_phi_` which cannot be replaced by Phis and
// propagate the load(s) as the new value(s) to successors; this may uncover new elimination
// opportunities. If we find no such load, we shall at least propagate an unknown value to some
// heap location that is needed by another loop Phi placeholder.
void LSEVisitor::ProcessLoopPhiWithUnknownInput(PhiPlaceholder loop_phi_with_unknown_input) {
  DCHECK(!loads_requiring_loop_phi_.empty());
  size_t loop_phi_with_unknown_input_index = PhiPlaceholderIndex(loop_phi_with_unknown_input);
  DCHECK(phi_placeholder_replacements_[loop_phi_with_unknown_input_index].IsInvalid());
  phi_placeholder_replacements_[loop_phi_with_unknown_input_index] = Value::Unknown();

  uint32_t block_id = loop_phi_with_unknown_input.GetBlockId();
  const ArenaVector<HBasicBlock*> reverse_post_order = GetGraph()->GetReversePostOrder();
  size_t reverse_post_order_index = 0;
  size_t reverse_post_order_size = reverse_post_order.size();
  size_t loads_and_stores_index = 0u;
  size_t loads_and_stores_size = loads_and_stores_.size();

  // Skip blocks and instructions before the block containing the loop phi with unknown input.
  DCHECK_NE(reverse_post_order_index, reverse_post_order_size);
  while (reverse_post_order[reverse_post_order_index]->GetBlockId() != block_id) {
    HBasicBlock* block = reverse_post_order[reverse_post_order_index];
    while (loads_and_stores_index != loads_and_stores_size &&
           loads_and_stores_[loads_and_stores_index].load_or_store->GetBlock() == block) {
      ++loads_and_stores_index;
    }
    ++reverse_post_order_index;
    DCHECK_NE(reverse_post_order_index, reverse_post_order_size);
  }

  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());
  // Reuse one temporary vector for all remaining blocks.
  size_t num_heap_locations = heap_location_collector_.GetNumberOfHeapLocations();
  ScopedArenaVector<Value> local_heap_values(allocator.Adapter(kArenaAllocLSE));

  auto get_initial_value = [this](HBasicBlock* block, size_t idx) {
    Value value;
    if (block->IsLoopHeader()) {
      if (block->GetLoopInformation()->IsIrreducible()) {
        value = Value::Unknown();
      } else {
        value = PrepareLoopValue(block, idx);
      }
    } else {
      value = MergePredecessorValues(block, idx);
    }
    DCHECK(value.IsUnknown() || ReplacementOrValue(value).Equals(value));
    return value;
  };

  // Process remaining blocks and instructions.
  bool found_unreplaceable_load = false;
  bool replaced_heap_value_with_unknown = false;
  for (; reverse_post_order_index != reverse_post_order_size; ++reverse_post_order_index) {
    HBasicBlock* block = reverse_post_order[reverse_post_order_index];
    if (block->IsExitBlock()) {
      continue;
    }

    // We shall reconstruct only the heap values that we need for processing loads and stores.
    local_heap_values.clear();
    local_heap_values.resize(num_heap_locations, Value::Invalid());

    for (; loads_and_stores_index != loads_and_stores_size; ++loads_and_stores_index) {
      HInstruction* load_or_store = loads_and_stores_[loads_and_stores_index].load_or_store;
      size_t idx = loads_and_stores_[loads_and_stores_index].heap_location_index;
      if (load_or_store->GetBlock() != block) {
        break;  // End of instructions from the current block.
      }
      if (IsStore(load_or_store)) {
        StoreRecord* store_record = store_records_[load_or_store->GetId()];
        DCHECK(store_record != nullptr);
        HInstruction* stored_value = store_record->stored_value;
        DCHECK(stored_value != nullptr);
        // Note that the `stored_value` can be a newly created `Phi` with an id that falls
        // outside the allocated `loads_requiring_loop_phi_` range.
        DCHECK_IMPLIES(
            IsLoad(stored_value),
            static_cast<size_t>(stored_value->GetId()) < loads_requiring_loop_phi_.size());
        if (static_cast<size_t>(stored_value->GetId()) >= loads_requiring_loop_phi_.size() ||
            loads_requiring_loop_phi_[stored_value->GetId()] == nullptr) {
          continue;  // This store never needed a loop Phi.
        }
        ValueRecord* record = loads_requiring_loop_phi_[stored_value->GetId()];
        // Process the store by updating `local_heap_values[idx]`. The last update shall
        // be propagated to the `heap_values[idx].value` if it previously needed a loop Phi
        // at the end of the block.
        Value replacement = ReplacementOrValue(record->value);
        if (replacement.NeedsLoopPhi()) {
          // No replacement yet. Use the Phi placeholder or an appropriate converting load.
          DCHECK(record->value.NeedsLoopPhi());
          local_heap_values[idx] = StoredValueForLoopPhiPlaceholderDependentLoad(idx, stored_value);
          DCHECK(local_heap_values[idx].NeedsLoopPhi());
        } else {
          // If the load fetched a known value, use it, otherwise use the load.
          local_heap_values[idx] = Value::ForInstruction(
              replacement.IsUnknown() ? stored_value : replacement.GetInstruction());
        }
      } else {
        // Process the load unless it has previously been marked unreplaceable.
        DCHECK(IsLoad(load_or_store));
        ValueRecord* record = loads_requiring_loop_phi_[load_or_store->GetId()];
        if (record == nullptr) {
          continue;  // This load never needed a loop Phi.
        }
        if (record->value.NeedsLoopPhi()) {
          if (local_heap_values[idx].IsInvalid()) {
            local_heap_values[idx] = get_initial_value(block, idx);
          }
          if (local_heap_values[idx].IsUnknown()) {
            // This load cannot be replaced. Keep stores that feed the Phi placeholder
            // (no aliasing since then, otherwise the Phi placeholder would not have been
            // propagated as a value to this load) and store the load as the new heap value.
            found_unreplaceable_load = true;
            KeepStores(record->value);
            record->value = Value::Unknown();
            local_heap_values[idx] = Value::ForInstruction(load_or_store);
          } else if (local_heap_values[idx].NeedsLoopPhi()) {
            // The load may still be replaced with a Phi later.
            DCHECK(local_heap_values[idx].Equals(record->value));
          } else {
            // This load can be eliminated but we may need to construct non-loop Phis.
            if (local_heap_values[idx].NeedsNonLoopPhi()) {
              MaterializeNonLoopPhis(local_heap_values[idx].GetPhiPlaceholder(),
                                     load_or_store->GetType());
              local_heap_values[idx] = Replacement(local_heap_values[idx]);
            }
            record->value = local_heap_values[idx];
            DCHECK(local_heap_values[idx].IsDefault() || local_heap_values[idx].IsInstruction())
                << "The replacement heap value can be an HIR instruction or the default value.";
            HInstruction* heap_value = local_heap_values[idx].IsDefault() ?
                                           GetDefaultValue(load_or_store->GetType()) :
                                           local_heap_values[idx].GetInstruction();
            AddRemovedLoad(load_or_store, heap_value);
          }
        }
      }
    }

    // All heap values that previously needed a loop Phi at the end of the block
    // need to be updated for processing successors.
    ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[block->GetBlockId()];
    for (size_t idx = 0; idx != num_heap_locations; ++idx) {
      if (heap_values[idx].value.NeedsLoopPhi()) {
        if (local_heap_values[idx].IsValid()) {
          heap_values[idx].value = local_heap_values[idx];
        } else {
          heap_values[idx].value = get_initial_value(block, idx);
        }
        if (heap_values[idx].value.IsUnknown()) {
          replaced_heap_value_with_unknown = true;
        }
      }
    }
  }
  DCHECK(found_unreplaceable_load || replaced_heap_value_with_unknown);
}

void LSEVisitor::ProcessLoadsRequiringLoopPhis() {
  // Note: The vector operations carve-out (see `IsDefaultOrPhiAllowedForLoad()`) can possibly
  // make the result of the processing depend on the order in which we process these loads.
  // To make sure the result is deterministic, iterate over `loads_and_stores_` instead of the
  // `loads_requiring_loop_phi_` indexed by non-deterministic pointers.
  if (loads_requiring_loop_phi_.empty()) {
    return;  // No loads to process.
  }
  for (const LoadStoreRecord& load_store_record : loads_and_stores_) {
    ValueRecord* record = loads_requiring_loop_phi_[load_store_record.load_or_store->GetId()];
    if (record == nullptr) {
      continue;
    }
    HInstruction* load = load_store_record.load_or_store;
    while (record->value.NeedsLoopPhi()) {
      Value without_conversions = SkipTypeConversions(record->value);
      if (!without_conversions.NeedsPlainLoopPhi() ||
          phi_placeholder_replacements_[PhiPlaceholderIndex(without_conversions)].IsValid()) {
        break;
      }
      std::optional<PhiPlaceholder> loop_phi_with_unknown_input =
          TryToMaterializeLoopPhis(without_conversions.GetPhiPlaceholder(), load);
      DCHECK_EQ(
          loop_phi_with_unknown_input.has_value(),
          phi_placeholder_replacements_[PhiPlaceholderIndex(without_conversions)].IsInvalid());
      if (loop_phi_with_unknown_input) {
        DCHECK_GE(GetGraph()
                      ->GetBlocks()[loop_phi_with_unknown_input->GetBlockId()]
                      ->GetPredecessors()
                      .size(),
                  2u);
        ProcessLoopPhiWithUnknownInput(*loop_phi_with_unknown_input);
      }
    }
    // The load, or converting load's underlying phi placeholder, could have been marked
    // as unreplaceable (and stores marked for keeping) or marked for replacement with an
    // instruction in `ProcessLoopPhiWithUnknownInput()`.
    DCHECK(record->value.IsUnknown() ||
           record->value.IsInstruction() ||
           record->value.NeedsLoopPhi());
    if (record->value.NeedsLoopPhi()) {
      MaterializeTypeConversionsIfNeeded(record->value);
      record->value = ReplacementOrValue(record->value);
      HInstruction* heap_value = record->value.GetInstruction();
      // Type conversion substitutes can be created by `MaterializeTypeConversionsIfNeeded()`,
      // either in the call directly above, or while materializing Phis. For all loads that did
      // not have a substitute recorded, record it now; this can also be a type conversion.
      HInstruction* substitute = FindSubstitute(load);
      if (substitute == load) {
        AddRemovedLoad(load, heap_value);
      } else {
        DCHECK(substitute->IsTypeConversion());
      }
    }
  }
}

void LSEVisitor::SearchPhiPlaceholdersForKeptStores() {
  ScopedArenaVector<uint32_t> work_queue(allocator_.Adapter(kArenaAllocLSE));
  size_t start_size = phi_placeholders_to_search_for_kept_stores_.NumSetBits();
  work_queue.reserve(((start_size * 3u) + 1u) / 2u);  // Reserve 1.5x start size, rounded up.
  for (uint32_t index : phi_placeholders_to_search_for_kept_stores_.Indexes()) {
    work_queue.push_back(index);
  }
  const ArenaVector<HBasicBlock*>& blocks = GetGraph()->GetBlocks();
  while (!work_queue.empty()) {
    uint32_t cur_phi_idx = work_queue.back();
    PhiPlaceholder phi_placeholder = GetPhiPlaceholderAt(cur_phi_idx);
    work_queue.pop_back();
    size_t idx = phi_placeholder.GetHeapLocation();
    HBasicBlock* block = blocks[phi_placeholder.GetBlockId()];
    DCHECK(block != nullptr) << cur_phi_idx << " phi: " << phi_placeholder
                             << " (blocks: " << blocks.size() << ")";
    for (HBasicBlock* predecessor : block->GetPredecessors()) {
      ScopedArenaVector<ValueRecord>& heap_values = heap_values_for_[predecessor->GetBlockId()];
      // For loop back-edges we must also preserve all stores to locations that
      // may alias with the location `idx`.
      // TODO: Add tests cases around this.
      bool is_back_edge =
          block->IsLoopHeader() && predecessor != block->GetLoopInformation()->GetPreHeader();
      size_t start = is_back_edge ? 0u : idx;
      size_t end = is_back_edge ? heap_values.size() : idx + 1u;
      for (size_t i = start; i != end; ++i) {
        Value stored_by = heap_values[i].stored_by;
        if (!stored_by.IsUnknown() && (i == idx || MayAliasOnBackEdge(block, idx, i))) {
          if (stored_by.NeedsPhi()) {
            size_t phi_placeholder_index = PhiPlaceholderIndex(stored_by);
            if (!phi_placeholders_to_search_for_kept_stores_.IsBitSet(phi_placeholder_index)) {
              phi_placeholders_to_search_for_kept_stores_.SetBit(phi_placeholder_index);
              work_queue.push_back(phi_placeholder_index);
            }
          } else {
            DCHECK(IsStore(stored_by.GetInstruction()));
            ReferenceInfo* ri = heap_location_collector_.GetHeapLocation(i)->GetReferenceInfo();
            DCHECK(ri != nullptr) << "No heap value for " << stored_by.GetInstruction()->DebugName()
                                  << " id: " << stored_by.GetInstruction()->GetId() << " block: "
                                  << stored_by.GetInstruction()->GetBlock()->GetBlockId();
            kept_stores_.SetBit(stored_by.GetInstruction()->GetId());
          }
        }
      }
    }
  }
}

void LSEVisitor::UpdateValueRecordForStoreElimination(/*inout*/ValueRecord* value_record) {
  while (value_record->stored_by.IsInstruction() &&
         !kept_stores_.IsBitSet(value_record->stored_by.GetInstruction()->GetId())) {
    StoreRecord* store_record = store_records_[value_record->stored_by.GetInstruction()->GetId()];
    DCHECK(store_record != nullptr);
    *value_record = store_record->old_value_record;
  }
  if (value_record->stored_by.NeedsPhi() &&
      !phi_placeholders_to_search_for_kept_stores_.IsBitSet(
           PhiPlaceholderIndex(value_record->stored_by))) {
    // Some stores feeding this heap location may have been eliminated. Use the `stored_by`
    // Phi placeholder to recalculate the actual value.
    value_record->value = value_record->stored_by;
  }
  value_record->value = ReplacementOrValue(value_record->value);
  if (value_record->value.NeedsNonLoopPhi()) {
    // Treat all Phi placeholders as requiring loop Phis at this point.
    // We do not want MaterializeLoopPhis() to call MaterializeNonLoopPhis().
    value_record->value =
        Value::ForPlainLoopPhiPlaceholder(value_record->value.GetPhiPlaceholder());
  }
}

void LSEVisitor::FindOldValueForPhiPlaceholder(PhiPlaceholder phi_placeholder,
                                               DataType::Type type) {
  DCHECK(phi_placeholder_replacements_[PhiPlaceholderIndex(phi_placeholder)].IsInvalid());

  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());
  ArenaBitVector visited(&allocator,
                         /*start_bits=*/ num_phi_placeholders_,
                         /*expandable=*/ false,
                         kArenaAllocLSE);

  // Find Phi placeholders to try and match against existing Phis or other replacement values.
  ArenaBitVector phi_placeholders_to_materialize(
      &allocator, num_phi_placeholders_, /*expandable=*/ false, kArenaAllocLSE);
  std::optional<PhiPlaceholder> loop_phi_with_unknown_input = FindLoopPhisToMaterialize(
      phi_placeholder, &phi_placeholders_to_materialize, type, /*can_use_default_or_phi=*/true);
  if (loop_phi_with_unknown_input) {
    DCHECK_GE(GetGraph()
                  ->GetBlocks()[loop_phi_with_unknown_input->GetBlockId()]
                  ->GetPredecessors()
                  .size(),
              2u);
    // Mark the unreplacable placeholder as well as the input Phi placeholder as unreplaceable.
    phi_placeholder_replacements_[PhiPlaceholderIndex(phi_placeholder)] = Value::Unknown();
    phi_placeholder_replacements_[PhiPlaceholderIndex(*loop_phi_with_unknown_input)] =
        Value::Unknown();
    return;
  }

  DCHECK_EQ(current_phase_, Phase::kStoreElimination);
  bool success = MaterializeLoopPhis(phi_placeholders_to_materialize, type);
  DCHECK(phi_placeholder_replacements_[PhiPlaceholderIndex(phi_placeholder)].IsValid());
  DCHECK_EQ(phi_placeholder_replacements_[PhiPlaceholderIndex(phi_placeholder)].IsUnknown(),
            !success);
}

void LSEVisitor::FindStoresWritingOldValues() {
  // The Phi placeholder replacements have so far been used for eliminating loads,
  // tracking values that would be stored if all stores were kept. As we want to
  // compare actual old values after removing unmarked stores, prune the Phi
  // placeholder replacements that can be fed by values we may not actually store.
  // Replacements marked as unknown can be kept as they are fed by some unknown
  // value and would end up as unknown again if we recalculated them.
  for (size_t i = 0, size = phi_placeholder_replacements_.size(); i != size; ++i) {
    if (!phi_placeholder_replacements_[i].IsUnknown() &&
        !phi_placeholders_to_search_for_kept_stores_.IsBitSet(i)) {
      phi_placeholder_replacements_[i] = Value::Invalid();
    }
  }

  // Update heap values at end of blocks.
  for (HBasicBlock* block : GetGraph()->GetReversePostOrder()) {
    for (ValueRecord& value_record : heap_values_for_[block->GetBlockId()]) {
      UpdateValueRecordForStoreElimination(&value_record);
    }
  }

  // Use local allocator to reduce peak memory usage.
  ScopedArenaAllocator allocator(allocator_.GetArenaStack());
  // Mark the stores we want to eliminate in a separate bit vector.
  ArenaBitVector eliminated_stores(&allocator,
                                   /*start_bits=*/ GetGraph()->GetCurrentInstructionId(),
                                   /*expandable=*/ false,
                                   kArenaAllocLSE);

  for (uint32_t store_id : kept_stores_.Indexes()) {
    DCHECK(kept_stores_.IsBitSet(store_id));
    StoreRecord* store_record = store_records_[store_id];
    DCHECK(store_record != nullptr);
    UpdateValueRecordForStoreElimination(&store_record->old_value_record);
    if (store_record->old_value_record.value.NeedsPhi()) {
      DataType::Type type = store_record->stored_value->GetType();
      FindOldValueForPhiPlaceholder(store_record->old_value_record.value.GetPhiPlaceholder(), type);
      store_record->old_value_record.value =
          ReplacementOrValue(store_record->old_value_record.value);
    }
    DCHECK(!store_record->old_value_record.value.NeedsPhi());
    HInstruction* stored_value = FindSubstitute(store_record->stored_value);
    if (store_record->old_value_record.value.Equals(stored_value)) {
      eliminated_stores.SetBit(store_id);
    }
  }

  // Commit the stores to eliminate by removing them from `kept_stores_`.
  kept_stores_.Subtract(&eliminated_stores);
}

void LSEVisitor::Run() {
  // 0. Set HasMonitorOperations to false. If we encounter some MonitorOperations that we can't
  // remove, we will set it to true in VisitMonitorOperation.
  GetGraph()->SetHasMonitorOperations(false);

  // 1. Process blocks and instructions in reverse post order.
  for (HBasicBlock* block : GetGraph()->GetReversePostOrder()) {
    VisitBasicBlock(block);
  }

  // 2. Process loads that require loop Phis, trying to find/create replacements.
  current_phase_ = Phase::kLoadElimination;
  ProcessLoadsRequiringLoopPhis();

  // 3. Determine which stores to keep and which to eliminate.
  current_phase_ = Phase::kStoreElimination;
  // Finish marking stores for keeping.
  SearchPhiPlaceholdersForKeptStores();

  // Find stores that write the same value as is already present in the location.
  FindStoresWritingOldValues();

  // 4. Replace loads and remove unnecessary stores and singleton allocations.
  FinishFullLSE();
}


void LSEVisitor::FinishFullLSE() {
  // Remove recorded load instructions that should be eliminated.
  for (const LoadStoreRecord& record : loads_and_stores_) {
    size_t id = dchecked_integral_cast<size_t>(record.load_or_store->GetId());
    HInstruction* substitute = substitute_instructions_for_loads_[id];
    if (substitute == nullptr) {
      continue;
    }
    HInstruction* load = record.load_or_store;
    DCHECK(load != nullptr);
    DCHECK(IsLoad(load));
    DCHECK(load->GetBlock() != nullptr) << load->DebugName() << "@" << load->GetDexPc();
    // We proactively retrieve the substitute for a removed load, so
    // a load that has a substitute should not be observed as a heap
    // location value.
    DCHECK_EQ(FindSubstitute(substitute), substitute);

    load->ReplaceWith(substitute);
    load->GetBlock()->RemoveInstruction(load);
    if ((load->IsInstanceFieldGet() && load->AsInstanceFieldGet()->IsVolatile()) ||
        (load->IsStaticFieldGet() && load->AsStaticFieldGet()->IsVolatile())) {
      MaybeRecordStat(stats_, MethodCompilationStat::kRemovedVolatileLoad);
    }
  }

  // Remove all the stores we can.
  for (const LoadStoreRecord& record : loads_and_stores_) {
    if (IsStore(record.load_or_store) && !kept_stores_.IsBitSet(record.load_or_store->GetId())) {
      record.load_or_store->GetBlock()->RemoveInstruction(record.load_or_store);
      if ((record.load_or_store->IsInstanceFieldSet() &&
           record.load_or_store->AsInstanceFieldSet()->IsVolatile()) ||
          (record.load_or_store->IsStaticFieldSet() &&
           record.load_or_store->AsStaticFieldSet()->IsVolatile())) {
        MaybeRecordStat(stats_, MethodCompilationStat::kRemovedVolatileStore);
      }
    }
  }

  // Eliminate singleton-classified instructions:
  //   * - Constructor fences (they never escape this thread).
  //   * - Allocations (if they are unused).
  for (HInstruction* new_instance : singleton_new_instances_) {
    size_t removed = HConstructorFence::RemoveConstructorFences(new_instance);
    MaybeRecordStat(stats_,
                    MethodCompilationStat::kConstructorFenceRemovedLSE,
                    removed);

    if (!new_instance->HasNonEnvironmentUses()) {
      new_instance->RemoveEnvironmentUsers();
      new_instance->GetBlock()->RemoveInstruction(new_instance);
      MaybeRecordStat(stats_, MethodCompilationStat::kFullLSEAllocationRemoved);
    }
  }
}

// The LSEVisitor is a ValueObject (indirectly through base classes) and therefore
// cannot be directly allocated with an arena allocator, so we need to wrap it.
class LSEVisitorWrapper : public DeletableArenaObject<kArenaAllocLSE> {
 public:
  LSEVisitorWrapper(HGraph* graph,
                    const HeapLocationCollector& heap_location_collector,
                    OptimizingCompilerStats* stats)
      : lse_visitor_(graph, heap_location_collector, stats) {}

  void Run() {
    lse_visitor_.Run();
  }

 private:
  LSEVisitor lse_visitor_;
};

bool LoadStoreElimination::Run() {
  if (graph_->IsDebuggable()) {
    // Debugger may set heap values or trigger deoptimization of callers.
    // Skip this optimization.
    return false;
  }
  ScopedArenaAllocator allocator(graph_->GetArenaStack());
  LoadStoreAnalysis lsa(graph_, stats_, &allocator);
  lsa.Run();
  const HeapLocationCollector& heap_location_collector = lsa.GetHeapLocationCollector();
  if (heap_location_collector.GetNumberOfHeapLocations() == 0) {
    // No HeapLocation information from LSA, skip this optimization.
    return false;
  }

  // Currently load_store analysis can't handle predicated load/stores; specifically pairs of
  // memory operations with different predicates.
  // TODO: support predicated SIMD.
  if (graph_->HasPredicatedSIMD()) {
    return false;
  }

  std::unique_ptr<LSEVisitorWrapper> lse_visitor(
      new (&allocator) LSEVisitorWrapper(graph_, heap_location_collector, stats_));
  lse_visitor->Run();
  return true;
}

#undef LSE_VLOG

}  // namespace art

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

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "android-base/logging.h"
#include "android-base/parseint.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/array_ref.h"
#include "base/bit_utils_iterator.h"
#include "base/file_utils.h"
#include "base/indenter.h"
#include "base/os.h"
#include "base/safe_map.h"
#include "base/stats-inl.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "class_linker-inl.h"
#include "class_linker.h"
#include "class_root-inl.h"
#include "cmdline.h"
#include "debug/debug_info.h"
#include "debug/elf_debug_writer.h"
#include "debug/method_debug_info.h"
#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_instruction-inl.h"
#include "dex/string_reference.h"
#include "dex/type_lookup_table.h"
#include "disassembler.h"
#include "elf/elf_builder.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "imtable-inl.h"
#include "interpreter/unstarted_runtime.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "oat/image-inl.h"
#include "oat/index_bss_mapping.h"
#include "oat/oat.h"
#include "oat/oat_file-inl.h"
#include "oat/oat_file_assistant.h"
#include "oat/oat_file_assistant_context.h"
#include "oat/oat_file_manager.h"
#include "oat/stack_map.h"
#include "scoped_thread_state_change-inl.h"
#include "stack.h"
#include "stream/buffered_output_stream.h"
#include "stream/file_output_stream.h"
#include "subtype_check.h"
#include "thread_list.h"
#include "vdex_file.h"
#include "verifier/method_verifier.h"
#include "verifier/verifier_deps.h"
#include "well_known_classes.h"

namespace art {

using android::base::StringPrintf;

const char* image_methods_descriptions_[] = {
  "kResolutionMethod",
  "kImtConflictMethod",
  "kImtUnimplementedMethod",
  "kSaveAllCalleeSavesMethod",
  "kSaveRefsOnlyMethod",
  "kSaveRefsAndArgsMethod",
  "kSaveEverythingMethod",
  "kSaveEverythingMethodForClinit",
  "kSaveEverythingMethodForSuspendCheck",
};

const char* image_roots_descriptions_[] = {
  "kDexCaches",
  "kClassRoots",
  "kSpecialRoots",
};

// Map is so that we don't allocate multiple dex files for the same OatDexFile.
static std::map<const OatDexFile*, std::unique_ptr<const DexFile>> opened_dex_files;

const DexFile* OpenDexFile(const OatDexFile* oat_dex_file, std::string* error_msg) {
  DCHECK(oat_dex_file != nullptr);
  auto it = opened_dex_files.find(oat_dex_file);
  if (it != opened_dex_files.end()) {
    return it->second.get();
  }
  const DexFile* ret = oat_dex_file->OpenDexFile(error_msg).release();
  opened_dex_files.emplace(oat_dex_file, std::unique_ptr<const DexFile>(ret));
  return ret;
}

template <typename ElfTypes>
class OatSymbolizer final {
 public:
  OatSymbolizer(const OatFile* oat_file, const std::string& output_name, bool no_bits) :
      oat_file_(oat_file),
      builder_(nullptr),
      output_name_(output_name.empty() ? "symbolized.oat" : output_name),
      no_bits_(no_bits) {
  }

  bool Symbolize() {
    const InstructionSet isa = oat_file_->GetOatHeader().GetInstructionSet();
    std::unique_ptr<const InstructionSetFeatures> features = InstructionSetFeatures::FromBitmap(
        isa, oat_file_->GetOatHeader().GetInstructionSetFeaturesBitmap());

    std::unique_ptr<File> elf_file(OS::CreateEmptyFile(output_name_.c_str()));
    if (elf_file == nullptr) {
      return false;
    }
    std::unique_ptr<BufferedOutputStream> output_stream =
        std::make_unique<BufferedOutputStream>(
            std::make_unique<FileOutputStream>(elf_file.get()));
    builder_.reset(new ElfBuilder<ElfTypes>(isa, output_stream.get()));

    builder_->Start();
    builder_->ReserveSpaceForDynamicSection(elf_file->GetPath());

    auto* rodata = builder_->GetRoData();
    auto* text = builder_->GetText();

    const uint8_t* rodata_begin = oat_file_->Begin();
    const size_t rodata_size = oat_file_->GetOatHeader().GetExecutableOffset();
    if (!no_bits_) {
      rodata->Start();
      rodata->WriteFully(rodata_begin, rodata_size);
      rodata->End();
    }

    const uint8_t* text_begin = oat_file_->Begin() + rodata_size;
    const size_t text_size = oat_file_->End() - text_begin;
    if (!no_bits_) {
      text->Start();
      text->WriteFully(text_begin, text_size);
      text->End();
    }

    builder_->PrepareDynamicSection(elf_file->GetPath(),
                                    rodata_size,
                                    text_size,
                                    oat_file_->DataImgRelRoSize(),
                                    oat_file_->DataImgRelRoAppImageOffset(),
                                    oat_file_->BssSize(),
                                    oat_file_->BssMethodsOffset(),
                                    oat_file_->BssRootsOffset(),
                                    oat_file_->VdexSize());
    builder_->WriteDynamicSection();

    const OatHeader& oat_header = oat_file_->GetOatHeader();
    #define DO_TRAMPOLINE(fn_name)                                                            \
      if (oat_header.Get ## fn_name ## Offset() != 0) {                                       \
        debug::MethodDebugInfo info = {};                                                     \
        info.custom_name = #fn_name;                                                          \
        info.isa = oat_header.GetInstructionSet();                                            \
        info.is_code_address_text_relative = true;                                            \
        size_t code_offset = oat_header.Get ## fn_name ## Offset();                           \
        code_offset -= GetInstructionSetEntryPointAdjustment(oat_header.GetInstructionSet()); \
        info.code_address = code_offset - oat_header.GetExecutableOffset();                   \
        info.code_size = 0;  /* The symbol lasts until the next symbol. */                    \
        method_debug_infos_.push_back(std::move(info));                                       \
      }
    DO_TRAMPOLINE(JniDlsymLookupTrampoline);
    DO_TRAMPOLINE(JniDlsymLookupCriticalTrampoline);
    DO_TRAMPOLINE(QuickGenericJniTrampoline);
    DO_TRAMPOLINE(QuickImtConflictTrampoline);
    DO_TRAMPOLINE(QuickResolutionTrampoline);
    DO_TRAMPOLINE(QuickToInterpreterBridge);
    DO_TRAMPOLINE(NterpTrampoline);
    #undef DO_TRAMPOLINE

    Walk();

    // TODO: Try to symbolize link-time thunks?
    // This would require disassembling all methods to find branches outside the method code.

    // TODO: Add symbols for dex bytecode in the .dex section.

    debug::DebugInfo debug_info{};
    debug_info.compiled_methods = ArrayRef<const debug::MethodDebugInfo>(method_debug_infos_);

    debug::WriteDebugInfo(builder_.get(), debug_info);

    builder_->End();

    bool ret_value = builder_->Good();

    builder_.reset();
    output_stream.reset();

    if (elf_file->FlushCloseOrErase() != 0) {
      return false;
    }
    elf_file.reset();

    return ret_value;
  }

  void Walk() {
    std::vector<const OatDexFile*> oat_dex_files = oat_file_->GetOatDexFiles();
    for (size_t i = 0; i < oat_dex_files.size(); i++) {
      const OatDexFile* oat_dex_file = oat_dex_files[i];
      CHECK(oat_dex_file != nullptr);
      WalkOatDexFile(oat_dex_file);
    }
  }

  void WalkOatDexFile(const OatDexFile* oat_dex_file) {
    std::string error_msg;
    const DexFile* const dex_file = OpenDexFile(oat_dex_file, &error_msg);
    if (dex_file == nullptr) {
      return;
    }
    for (size_t class_def_index = 0;
        class_def_index < dex_file->NumClassDefs();
        class_def_index++) {
      const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
      OatClassType type = oat_class.GetType();
      switch (type) {
        case OatClassType::kAllCompiled:
        case OatClassType::kSomeCompiled:
          WalkOatClass(oat_class, *dex_file, class_def_index);
          break;

        case OatClassType::kNoneCompiled:
          // Ignore.
          break;
      }
    }
  }

  void WalkOatClass(const OatFile::OatClass& oat_class,
                    const DexFile& dex_file,
                    uint32_t class_def_index) {
    ClassAccessor accessor(dex_file, class_def_index);
    // Note: even if this is an interface or a native class, we still have to walk it, as there
    //       might be a static initializer.
    uint32_t class_method_idx = 0;
    for (const ClassAccessor::Method& method : accessor.GetMethods()) {
      WalkOatMethod(oat_class.GetOatMethod(class_method_idx++),
                    dex_file,
                    class_def_index,
                    method.GetIndex(),
                    method.GetCodeItem(),
                    method.GetAccessFlags());
    }
  }

  void WalkOatMethod(const OatFile::OatMethod& oat_method,
                     const DexFile& dex_file,
                     uint32_t class_def_index,
                     uint32_t dex_method_index,
                     const dex::CodeItem* code_item,
                     uint32_t method_access_flags) {
    if ((method_access_flags & kAccAbstract) != 0) {
      // Abstract method, no code.
      return;
    }
    const OatHeader& oat_header = oat_file_->GetOatHeader();
    const OatQuickMethodHeader* method_header = oat_method.GetOatQuickMethodHeader();
    if (method_header == nullptr || method_header->GetCodeSize() == 0) {
      // No code.
      return;
    }

    uint32_t entry_point = oat_method.GetCodeOffset() - oat_header.GetExecutableOffset();
    // Clear Thumb2 bit.
    const void* code_address = EntryPointToCodePointer(reinterpret_cast<void*>(entry_point));

    debug::MethodDebugInfo info = {};
    DCHECK(info.custom_name.empty());
    info.dex_file = &dex_file;
    info.class_def_index = class_def_index;
    info.dex_method_index = dex_method_index;
    info.access_flags = method_access_flags;
    info.code_item = code_item;
    info.isa = oat_header.GetInstructionSet();
    info.deduped = !seen_offsets_.insert(oat_method.GetCodeOffset()).second;
    info.is_native_debuggable = oat_header.IsNativeDebuggable();
    info.is_optimized = method_header->IsOptimized();
    info.is_code_address_text_relative = true;
    info.code_address = reinterpret_cast<uintptr_t>(code_address);
    info.code_size = method_header->GetCodeSize();
    info.frame_size_in_bytes = method_header->GetFrameSizeInBytes();
    info.code_info = info.is_optimized ? method_header->GetOptimizedCodeInfoPtr() : nullptr;
    info.cfi = ArrayRef<uint8_t>();
    method_debug_infos_.push_back(info);
  }

 private:
  const OatFile* oat_file_;
  std::unique_ptr<ElfBuilder<ElfTypes>> builder_;
  std::vector<debug::MethodDebugInfo> method_debug_infos_;
  std::unordered_set<uint32_t> seen_offsets_;
  const std::string output_name_;
  bool no_bits_;
};

class OatDumperOptions {
 public:
  OatDumperOptions(bool dump_vmap,
                   bool dump_code_info_stack_maps,
                   bool disassemble_code,
                   bool absolute_addresses,
                   const char* class_filter,
                   const char* method_filter,
                   bool list_classes,
                   bool list_methods,
                   bool dump_header_only,
                   bool dump_method_and_offset_as_json,
                   const char* export_dex_location,
                   const char* app_image,
                   const char* oat_filename,
                   const char* dex_filename,
                   uint32_t addr2instr)
      : dump_vmap_(dump_vmap),
        dump_code_info_stack_maps_(dump_code_info_stack_maps),
        disassemble_code_(disassemble_code),
        absolute_addresses_(absolute_addresses),
        class_filter_(class_filter),
        method_filter_(method_filter),
        list_classes_(list_classes),
        list_methods_(list_methods),
        dump_header_only_(dump_header_only),
        dump_method_and_offset_as_json(dump_method_and_offset_as_json),
        export_dex_location_(export_dex_location),
        app_image_(app_image),
        oat_filename_(oat_filename != nullptr ? std::make_optional(oat_filename) : std::nullopt),
        dex_filename_(dex_filename != nullptr ? std::make_optional(dex_filename) : std::nullopt),
        addr2instr_(addr2instr),
        class_loader_(nullptr) {}

  const bool dump_vmap_;
  const bool dump_code_info_stack_maps_;
  const bool disassemble_code_;
  const bool absolute_addresses_;
  const char* const class_filter_;
  const char* const method_filter_;
  const bool list_classes_;
  const bool list_methods_;
  const bool dump_header_only_;
  const bool dump_method_and_offset_as_json;
  const char* const export_dex_location_;
  const char* const app_image_;
  const std::optional<std::string> oat_filename_;
  const std::optional<std::string> dex_filename_;
  uint32_t addr2instr_;
  Handle<mirror::ClassLoader>* class_loader_;
};

class OatDumper {
 public:
  OatDumper(const OatFile& oat_file, const OatDumperOptions& options)
      : oat_file_(oat_file),
        oat_dex_files_(oat_file.GetOatDexFiles()),
        options_(options),
        resolved_addr2instr_(0),
        instruction_set_(oat_file_.GetOatHeader().GetInstructionSet()) {
    CHECK(options_.class_loader_ != nullptr);
    CHECK(options_.class_filter_ != nullptr);
    CHECK(options_.method_filter_ != nullptr);

    std::string error_msg;
    const uint8_t* elf_begin = oat_file.ComputeElfBegin(&error_msg);
    DCHECK_NE(elf_begin, nullptr) << error_msg;
    DCHECK_GE(oat_file.Begin(), elf_begin);
    oat_offset_ = reinterpret_cast<size_t>(oat_file.Begin()) - reinterpret_cast<size_t>(elf_begin);

    disassembler_ = Disassembler::Create(
        instruction_set_,
        new DisassemblerOptions(options_.absolute_addresses_,
                                elf_begin,
                                oat_file.End(),
                                /* can_read_literals_= */ true,
                                Is64BitInstructionSet(instruction_set_) ?
                                    &Thread::DumpThreadOffset<PointerSize::k64> :
                                    &Thread::DumpThreadOffset<PointerSize::k32>));

    AddAllOffsets();
  }

  ~OatDumper() {
    delete disassembler_;
  }

  InstructionSet GetInstructionSet() {
    return instruction_set_;
  }

  using DexFileUniqV = std::vector<std::unique_ptr<const DexFile>>;

  bool Dump(std::ostream& os) {
    if (options_.dump_method_and_offset_as_json) {
      return DumpMethodAndOffsetAsJson(os);
    }

    bool success = true;
    const OatHeader& oat_header = oat_file_.GetOatHeader();

    os << "MAGIC:\n";
    os << oat_header.GetMagic() << "\n\n";

    os << "LOCATION:\n";
    os << oat_file_.GetLocation() << "\n\n";

    os << "CHECKSUM:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetChecksum());

    os << "INSTRUCTION SET:\n";
    os << oat_header.GetInstructionSet() << "\n\n";

    {
      std::unique_ptr<const InstructionSetFeatures> features(
          InstructionSetFeatures::FromBitmap(oat_header.GetInstructionSet(),
                                             oat_header.GetInstructionSetFeaturesBitmap()));
      os << "INSTRUCTION SET FEATURES:\n";
      os << features->GetFeatureString() << "\n\n";
    }

    os << "DEX FILE COUNT:\n";
    os << oat_header.GetDexFileCount() << "\n\n";

#define DUMP_OAT_HEADER_OFFSET(label, offset)                             \
  os << label " OFFSET:\n";                                               \
  os << StringPrintf("0x%08zx", AdjustOffset(oat_header.offset()));       \
  if (oat_header.offset() != 0 && options_.absolute_addresses_) {         \
    os << StringPrintf(" (%p)", oat_file_.Begin() + oat_header.offset()); \
  }                                                                       \
  os << StringPrintf("\n\n");

    DUMP_OAT_HEADER_OFFSET("EXECUTABLE", GetExecutableOffset);
    DUMP_OAT_HEADER_OFFSET("JNI DLSYM LOOKUP TRAMPOLINE",
                           GetJniDlsymLookupTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("JNI DLSYM LOOKUP CRITICAL TRAMPOLINE",
                           GetJniDlsymLookupCriticalTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK GENERIC JNI TRAMPOLINE",
                           GetQuickGenericJniTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK IMT CONFLICT TRAMPOLINE",
                           GetQuickImtConflictTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK RESOLUTION TRAMPOLINE",
                           GetQuickResolutionTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK TO INTERPRETER BRIDGE",
                           GetQuickToInterpreterBridgeOffset);
    DUMP_OAT_HEADER_OFFSET("NTERP_TRAMPOLINE",
                           GetNterpTrampolineOffset);
#undef DUMP_OAT_HEADER_OFFSET

    // Print the key-value store.
    {
      os << "KEY VALUE STORE:\n";
      uint32_t offset = 0;
      const char* key;
      const char* value;
      while (oat_header.GetNextStoreKeyValuePair(&offset, &key, &value)) {
        os << key << " = " << value << "\n";
      }
      os << "\n";
    }

    if (options_.absolute_addresses_) {
      os << "BEGIN:\n";
      os << reinterpret_cast<const void*>(oat_file_.Begin()) << "\n\n";

      os << "END:\n";
      os << reinterpret_cast<const void*>(oat_file_.End()) << "\n\n";
    }

    os << "SIZE:\n";
    os << oat_file_.Size() << "\n\n";

    os << std::flush;

    // If set, adjust relative address to be searched
    if (options_.addr2instr_ != 0) {
      resolved_addr2instr_ = options_.addr2instr_ + oat_header.GetExecutableOffset();
      os << "SEARCH ADDRESS (executable offset + input):\n";
      os << StringPrintf("0x%08zx\n\n", AdjustOffset(resolved_addr2instr_));
    }

    // Dump .data.img.rel.ro entries.
    DumpDataImgRelRoEntries(os);

    // Dump .bss summary, individual entries are dumped per dex file.
    os << ".bss: ";
    if (oat_file_.GetBssMethods().empty() && oat_file_.GetBssGcRoots().empty()) {
      os << "empty.\n\n";
    } else {
      os << oat_file_.GetBssMethods().size() << " methods, ";
      os << oat_file_.GetBssGcRoots().size() << " GC roots.\n\n";
    }

    // Dumping the dex file overview is compact enough to do even if header only.
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      std::string error_msg;
      const DexFile* const dex_file = OpenDexFile(oat_dex_file, &error_msg);
      if (dex_file == nullptr) {
        os << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation() << "': "
           << error_msg;
        continue;
      }

      const DexLayoutSections* const layout_sections = oat_dex_file->GetDexLayoutSections();
      if (layout_sections != nullptr) {
        os << "Layout data\n";
        os << *layout_sections;
        os << "\n";
      }

      if (!options_.dump_header_only_) {
        DumpBssMappings(os,
                        dex_file,
                        oat_dex_file->GetMethodBssMapping(),
                        oat_dex_file->GetTypeBssMapping(),
                        oat_dex_file->GetPublicTypeBssMapping(),
                        oat_dex_file->GetPackageTypeBssMapping(),
                        oat_dex_file->GetStringBssMapping(),
                        oat_dex_file->GetMethodTypeBssMapping());
      }
    }

    if (!options_.dump_header_only_) {
      Runtime* const runtime = Runtime::Current();
      ClassLinker* const linker = runtime != nullptr ? runtime->GetClassLinker() : nullptr;

      if (linker != nullptr) {
        ArrayRef<const DexFile* const> bcp_dex_files(linker->GetBootClassPath());
        // The guarantee that we have is that we can safely take a look the BCP DexFiles in
        // [0..number_of_compiled_bcp_dexfiles) since the runtime may add more DexFiles after that.
        // As a note, in the case of not having mappings or in the case of multi image we
        // purposively leave `oat_file_.bcp_bss_info` empty.
        CHECK_LE(oat_file_.bcp_bss_info_.size(), bcp_dex_files.size());
        for (size_t i = 0; i < oat_file_.bcp_bss_info_.size(); i++) {
          const DexFile* const dex_file = bcp_dex_files[i];
          os << "Entries for BCP DexFile: " << dex_file->GetLocation() << "\n";
          DumpBssMappings(os,
                          dex_file,
                          oat_file_.bcp_bss_info_[i].method_bss_mapping,
                          oat_file_.bcp_bss_info_[i].type_bss_mapping,
                          oat_file_.bcp_bss_info_[i].public_type_bss_mapping,
                          oat_file_.bcp_bss_info_[i].package_type_bss_mapping,
                          oat_file_.bcp_bss_info_[i].string_bss_mapping,
                          oat_file_.bcp_bss_info_[i].method_type_bss_mapping);
        }
      } else {
        // We don't have a runtime, just dump the offsets
        for (size_t i = 0; i < oat_file_.bcp_bss_info_.size(); i++) {
          os << "Offsets for BCP DexFile at index " << i << "\n";
          DumpBssOffsets(os, "ArtMethod", oat_file_.bcp_bss_info_[i].method_bss_mapping);
          DumpBssOffsets(os, "Class", oat_file_.bcp_bss_info_[i].type_bss_mapping);
          DumpBssOffsets(os, "Public Class", oat_file_.bcp_bss_info_[i].public_type_bss_mapping);
          DumpBssOffsets(os, "Package Class", oat_file_.bcp_bss_info_[i].package_type_bss_mapping);
          DumpBssOffsets(os, "String", oat_file_.bcp_bss_info_[i].string_bss_mapping);
          DumpBssOffsets(os, "MethodType", oat_file_.bcp_bss_info_[i].method_type_bss_mapping);
        }
      }
    }

    if (!options_.dump_header_only_) {
      VariableIndentationOutputStream vios(&os);
      VdexFile::VdexFileHeader vdex_header = oat_file_.GetVdexFile()->GetVdexFileHeader();
      if (vdex_header.IsValid()) {
        std::string error_msg;
        std::vector<const DexFile*> dex_files;
        for (size_t i = 0; i < oat_dex_files_.size(); i++) {
          const DexFile* dex_file = OpenDexFile(oat_dex_files_[i], &error_msg);
          if (dex_file == nullptr) {
            os << "Error opening dex file: " << error_msg << std::endl;
            return false;
          }
          dex_files.push_back(dex_file);
        }
        verifier::VerifierDeps deps(dex_files, /*output_only=*/ false);
        if (!deps.ParseStoredData(dex_files, oat_file_.GetVdexFile()->GetVerifierDepsData())) {
          os << "Error parsing verifier dependencies." << std::endl;
          return false;
        }
        deps.Dump(&vios);
      } else {
        os << "UNRECOGNIZED vdex file, magic "
           << vdex_header.GetMagic()
           << ", version "
           << vdex_header.GetVdexVersion()
           << "\n";
      }
      for (size_t i = 0; i < oat_dex_files_.size(); i++) {
        const OatDexFile* oat_dex_file = oat_dex_files_[i];
        CHECK(oat_dex_file != nullptr);
        if (!DumpOatDexFile(os, *oat_dex_file)) {
          success = false;
        }
      }
    }

    if (options_.export_dex_location_) {
      std::string error_msg;
      std::string vdex_filename = GetVdexFilename(oat_file_.GetLocation());
      if (!OS::FileExists(vdex_filename.c_str())) {
        os << "File " << vdex_filename.c_str() << " does not exist\n";
        return false;
      }

      DexFileUniqV vdex_dex_files;
      std::unique_ptr<const VdexFile> vdex_file = OpenVdex(vdex_filename,
                                                           &vdex_dex_files,
                                                           &error_msg);
      if (vdex_file.get() == nullptr) {
        os << "Failed to open vdex file: " << error_msg << "\n";
        return false;
      }
      if (oat_dex_files_.size() != vdex_dex_files.size()) {
        os << "Dex files number in Vdex file does not match Dex files number in Oat file: "
           << vdex_dex_files.size() << " vs " << oat_dex_files_.size() << '\n';
        return false;
      }

      size_t i = 0;
      for (const auto& vdex_dex_file : vdex_dex_files) {
        const OatDexFile* oat_dex_file = oat_dex_files_[i];
        CHECK(oat_dex_file != nullptr);
        CHECK(vdex_dex_file != nullptr);

        if (!vdex_dex_file->IsDexContainerFirstEntry()) {
          // All the data was already exported together with the primary dex file.
          continue;
        }

        if (!ExportDexFile(os, *oat_dex_file, vdex_dex_file.get(), /*used_dexlayout=*/ false)) {
          success = false;
          break;
        }
        i++;
      }
    }

    {
      os << "OAT FILE STATS:\n";
      VariableIndentationOutputStream vios(&os);
      stats_.AddBytes(oat_file_.Size());
      stats_.DumpSizes(vios, "OatFile");
    }

    os << std::flush;
    return success;
  }

  bool DumpMethodAndOffsetAsJson(std::ostream& os) {
    for (const OatDexFile* oat_dex_file : oat_dex_files_) {
      CHECK(oat_dex_file != nullptr);
      // Create the dex file early. A lot of print-out things depend on it.
      std::string error_msg;
      const DexFile* const dex_file = art::OpenDexFile(oat_dex_file, &error_msg);
      if (dex_file == nullptr) {
        LOG(WARNING) << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation()
                     << "': " << error_msg;
        return false;
      }
      for (ClassAccessor accessor : dex_file->GetClasses()) {
        std::string_view descriptor = accessor.GetDescriptorView();
        if (DescriptorToDot(descriptor).find(options_.class_filter_) == std::string::npos) {
          continue;
        }

        const uint16_t class_def_index = accessor.GetClassDefIndex();
        const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
        uint32_t class_method_index = 0;

        // inspired by DumpOatMethod
        for (const ClassAccessor::Method& method : accessor.GetMethods()) {
          uint32_t code_offset = oat_class.GetOatMethod(class_method_index).GetCodeOffset();
          class_method_index++;

          uint32_t dex_method_idx = method.GetIndex();
          std::string method_name = dex_file->GetMethodName(dex_file->GetMethodId(dex_method_idx));
          if (method_name.find(options_.method_filter_) == std::string::npos) {
            continue;
          }

          std::string pretty_method = dex_file->PrettyMethod(dex_method_idx, true);

          os << StringPrintf("{\"method\":\"%s\",\"offset\":\"0x%08zx\"}\n",
                             pretty_method.c_str(),
                             AdjustOffset(code_offset));
        }
      }
    }
    return true;
  }

  size_t ComputeSize(const void* oat_data) {
    if (reinterpret_cast<const uint8_t*>(oat_data) < oat_file_.Begin() ||
        reinterpret_cast<const uint8_t*>(oat_data) > oat_file_.End()) {
      return 0;  // Address not in oat file
    }
    uintptr_t begin_offset = reinterpret_cast<uintptr_t>(oat_data) -
                             reinterpret_cast<uintptr_t>(oat_file_.Begin());
    auto it = offsets_.upper_bound(begin_offset);
    CHECK(it != offsets_.end());
    uintptr_t end_offset = *it;
    return end_offset - begin_offset;
  }

  InstructionSet GetOatInstructionSet() {
    return oat_file_.GetOatHeader().GetInstructionSet();
  }

  const void* GetQuickOatCode(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_) {
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      std::string error_msg;
      const DexFile* const dex_file = OpenDexFile(oat_dex_file, &error_msg);
      if (dex_file == nullptr) {
        LOG(WARNING) << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation()
            << "': " << error_msg;
      } else {
        const char* descriptor = m->GetDeclaringClassDescriptor();
        const dex::ClassDef* class_def =
            OatDexFile::FindClassDef(*dex_file, descriptor, ComputeModifiedUtf8Hash(descriptor));
        if (class_def != nullptr) {
          uint16_t class_def_index = dex_file->GetIndexForClassDef(*class_def);
          const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
          uint32_t oat_method_index;
          if (m->IsStatic() || m->IsDirect()) {
            // Simple case where the oat method index was stashed at load time.
            oat_method_index = m->GetMethodIndex();
          } else {
            // Compute the oat_method_index by search for its position in the class def.
            ClassAccessor accessor(*dex_file, *class_def);
            oat_method_index = accessor.NumDirectMethods();
            bool found_virtual = false;
            for (ClassAccessor::Method dex_method : accessor.GetVirtualMethods()) {
              // Check method index instead of identity in case of duplicate method definitions.
              if (dex_method.GetIndex() == m->GetDexMethodIndex()) {
                found_virtual = true;
                break;
              }
              ++oat_method_index;
            }
            CHECK(found_virtual) << "Didn't find oat method index for virtual method: "
                                 << dex_file->PrettyMethod(m->GetDexMethodIndex());
          }
          return oat_class.GetOatMethod(oat_method_index).GetQuickCode();
        }
      }
    }
    return nullptr;
  }

  // Returns nullptr and updates error_msg if the Vdex file cannot be opened, otherwise all Dex
  // files are stored in dex_files.
  std::unique_ptr<const VdexFile> OpenVdex(const std::string& vdex_filename,
                                           /* out */ DexFileUniqV* dex_files,
                                           /* out */ std::string* error_msg) {
    std::unique_ptr<const File> file(OS::OpenFileForReading(vdex_filename.c_str()));
    if (file == nullptr) {
      *error_msg = "Could not open file " + vdex_filename + " for reading.";
      return nullptr;
    }

    int64_t vdex_length = file->GetLength();
    if (vdex_length == -1) {
      *error_msg = "Could not read the length of file " + vdex_filename;
      return nullptr;
    }

    MemMap mmap = MemMap::MapFile(
        file->GetLength(),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE,
        file->Fd(),
        /* start offset= */ 0,
        /* low_4gb= */ false,
        vdex_filename.c_str(),
        error_msg);
    if (!mmap.IsValid()) {
      *error_msg = "Failed to mmap file " + vdex_filename + ": " + *error_msg;
      return nullptr;
    }

    std::unique_ptr<VdexFile> vdex_file(new VdexFile(std::move(mmap)));
    if (!vdex_file->IsValid()) {
      *error_msg = "Vdex file is not valid";
      return nullptr;
    }

    DexFileUniqV tmp_dex_files;
    if (!vdex_file->OpenAllDexFiles(&tmp_dex_files, error_msg)) {
      *error_msg = "Failed to open Dex files from Vdex: " + *error_msg;
      return nullptr;
    }

    *dex_files = std::move(tmp_dex_files);
    return vdex_file;
  }

  bool AddStatsObject(const void* address) {
    return seen_stats_objects_.insert(address).second;  // Inserted new entry.
  }

 private:
  void AddAllOffsets() {
    // We don't know the length of the code for each method, but we need to know where to stop
    // when disassembling. What we do know is that a region of code will be followed by some other
    // region, so if we keep a sorted sequence of the start of each region, we can infer the length
    // of a piece of code by using upper_bound to find the start of the next region.
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      std::string error_msg;
      const DexFile* const dex_file = OpenDexFile(oat_dex_file, &error_msg);
      if (dex_file == nullptr) {
        LOG(WARNING) << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation()
            << "': " << error_msg;
        continue;
      }
      offsets_.insert(reinterpret_cast<uintptr_t>(&dex_file->GetHeader()));
      for (ClassAccessor accessor : dex_file->GetClasses()) {
        const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(accessor.GetClassDefIndex());
        for (uint32_t class_method_index = 0;
            class_method_index < accessor.NumMethods();
            ++class_method_index) {
          AddOffsets(oat_class.GetOatMethod(class_method_index));
        }
      }
    }

    // If the last thing in the file is code for a method, there won't be an offset for the "next"
    // thing. Instead of having a special case in the upper_bound code, let's just add an entry
    // for the end of the file.
    offsets_.insert(oat_file_.Size());
  }

  static uint32_t AlignCodeOffset(uint32_t maybe_thumb_offset) {
    return maybe_thumb_offset & ~0x1;  // TODO: Make this Thumb2 specific.
  }

  void AddOffsets(const OatFile::OatMethod& oat_method) {
    uint32_t code_offset = oat_method.GetCodeOffset();
    if (oat_file_.GetOatHeader().GetInstructionSet() == InstructionSet::kThumb2) {
      code_offset &= ~0x1;
    }
    offsets_.insert(code_offset);
    offsets_.insert(oat_method.GetVmapTableOffset());
  }

  bool DumpOatDexFile(std::ostream& os, const OatDexFile& oat_dex_file) {
    bool success = true;
    bool stop_analysis = false;
    os << "OatDexFile:\n";
    os << StringPrintf("location: %s\n", oat_dex_file.GetDexFileLocation().c_str());
    os << StringPrintf("checksum: 0x%08x\n", oat_dex_file.GetDexFileLocationChecksum());

    if (oat_dex_file.GetOatFile()->ContainsDexCode()) {
      const uint8_t* const vdex_file_begin = oat_dex_file.GetOatFile()->DexBegin();

      // Print data range of the dex file embedded inside the corresponding vdex file.
      const uint8_t* const dex_file_pointer = oat_dex_file.GetDexFilePointer();
      uint32_t dex_offset = dchecked_integral_cast<uint32_t>(dex_file_pointer - vdex_file_begin);
      os << StringPrintf(
          "dex-file: 0x%08x..0x%08x\n",
          dex_offset,
          dchecked_integral_cast<uint32_t>(dex_offset + oat_dex_file.FileSize() - 1));
    } else {
      os << StringPrintf("dex-file not in VDEX file\n");
    }

    // Create the dex file early. A lot of print-out things depend on it.
    std::string error_msg;
    const DexFile* const dex_file = OpenDexFile(&oat_dex_file, &error_msg);
    if (dex_file == nullptr) {
      os << "NOT FOUND: " << error_msg << "\n\n";
      os << std::flush;
      return false;
    }

    // Print lookup table, if it exists.
    if (oat_dex_file.GetLookupTableData() != nullptr) {
      uint32_t table_offset = dchecked_integral_cast<uint32_t>(
          oat_dex_file.GetLookupTableData() - oat_dex_file.GetOatFile()->DexBegin());
      uint32_t table_size = TypeLookupTable::RawDataLength(dex_file->NumClassDefs());
      os << StringPrintf("type-table: 0x%08x..0x%08x\n",
                         table_offset,
                         table_offset + table_size - 1);
      const TypeLookupTable& lookup = oat_dex_file.GetTypeLookupTable();
      lookup.Dump(os);
    }

    VariableIndentationOutputStream vios(&os);
    ScopedIndentation indent1(&vios);
    for (ClassAccessor accessor : dex_file->GetClasses()) {
      // TODO: Support regex
      std::string_view descriptor = accessor.GetDescriptorView();
      if (DescriptorToDot(descriptor).find(options_.class_filter_) == std::string::npos) {
        continue;
      }

      const uint16_t class_def_index = accessor.GetClassDefIndex();
      uint32_t oat_class_offset = oat_dex_file.GetOatClassOffset(class_def_index);
      const OatFile::OatClass oat_class = oat_dex_file.GetOatClass(class_def_index);
      os << static_cast<ssize_t>(class_def_index) << ": " << descriptor << " (offset=0x"
         << StringPrintf("%08zx", AdjustOffset(oat_class_offset))
         << ") (type_idx=" << accessor.GetClassIdx().index_
         << ") (" << oat_class.GetStatus() << ")" << " (" << oat_class.GetType() << ")\n";
      // TODO: include bitmap here if type is kOatClassSomeCompiled?
      if (options_.list_classes_) {
        continue;
      }
      if (!DumpOatClass(&vios, oat_class, *dex_file, accessor, &stop_analysis)) {
        success = false;
      }
      if (stop_analysis) {
        os << std::flush;
        return success;
      }
    }
    os << "\n";
    os << std::flush;
    return success;
  }

  // Backwards compatible Dex file export. If dex_file is nullptr (valid Vdex file not present) the
  // Dex resource is extracted from the oat_dex_file and its checksum is repaired since it's not
  // unquickened. Otherwise the dex_file has been fully unquickened and is expected to verify the
  // original checksum.
  bool ExportDexFile(std::ostream& os,
                     const OatDexFile& oat_dex_file,
                     const DexFile* dex_file,
                     bool used_dexlayout) {
    std::string error_msg;
    std::string dex_file_location = oat_dex_file.GetDexFileLocation();

    // If dex_file (from unquicken or dexlayout) is not available, the output DexFile size is the
    // same as the one extracted from the Oat container (pre-oreo)
    size_t fsize = dex_file == nullptr ? oat_dex_file.FileSize() : dex_file->Size();

    // Some quick checks just in case
    if (fsize == 0 || fsize < sizeof(DexFile::Header)) {
      os << "Invalid dex file\n";
      return false;
    }

    if (dex_file == nullptr) {
      // Exported bytecode is quickened (dex-to-dex transformations present)
      dex_file = OpenDexFile(&oat_dex_file, &error_msg);
      if (dex_file == nullptr) {
        os << "Failed to open dex file '" << dex_file_location << "': " << error_msg;
        return false;
      }

      // Recompute checksum
      reinterpret_cast<DexFile::Header*>(const_cast<uint8_t*>(dex_file->Begin()))->checksum_ =
          dex_file->CalculateChecksum();
    } else {
      // If dexlayout was used to convert CompactDex back to StandardDex, checksum will be updated
      // due to `update_checksum_` option, otherwise we expect a reproducible checksum.
      if (!used_dexlayout) {
        // Vdex unquicken output should match original input bytecode
        uint32_t orig_checksum =
            reinterpret_cast<DexFile::Header*>(const_cast<uint8_t*>(dex_file->Begin()))->checksum_;
        if (orig_checksum != dex_file->CalculateChecksum()) {
          os << "Unexpected checksum from unquicken dex file '" << dex_file_location << "'\n";
          return false;
        }
      }
      // Extend the data range to export all the dex files in the container.
      CHECK(dex_file->IsDexContainerFirstEntry()) << dex_file_location;
      fsize = dex_file->GetHeader().ContainerSize();
    }

    // Verify output directory exists
    if (!OS::DirectoryExists(options_.export_dex_location_)) {
      // TODO: Extend OS::DirectoryExists if symlink support is required
      os << options_.export_dex_location_ << " output directory not found or symlink\n";
      return false;
    }

    // Beautify path names
    if (dex_file_location.size() > PATH_MAX || dex_file_location.size() <= 0) {
      return false;
    }

    std::string dex_orig_name;
    size_t dex_orig_pos = dex_file_location.rfind('/');
    if (dex_orig_pos == std::string::npos)
      dex_orig_name = dex_file_location;
    else
      dex_orig_name = dex_file_location.substr(dex_orig_pos + 1);

    // A more elegant approach to efficiently name user installed apps is welcome
    if (dex_orig_name.size() == 8 &&
        dex_orig_name.compare("base.apk") == 0 &&
        dex_orig_pos != std::string::npos) {
      dex_file_location.erase(dex_orig_pos, strlen("base.apk") + 1);
      size_t apk_orig_pos = dex_file_location.rfind('/');
      if (apk_orig_pos != std::string::npos) {
        dex_orig_name = dex_file_location.substr(++apk_orig_pos);
      }
    }

    std::string out_dex_path(options_.export_dex_location_);
    if (out_dex_path.back() != '/') {
      out_dex_path.append("/");
    }
    out_dex_path.append(dex_orig_name);
    out_dex_path.append("_export.dex");
    if (out_dex_path.length() > PATH_MAX) {
      return false;
    }

    std::unique_ptr<File> file(OS::CreateEmptyFile(out_dex_path.c_str()));
    if (file.get() == nullptr) {
      os << "Failed to open output dex file " << out_dex_path;
      return false;
    }

    bool success = file->WriteFully(dex_file->Begin(), fsize);
    if (!success) {
      os << "Failed to write dex file";
      file->Erase();
      return false;
    }

    if (file->FlushCloseOrErase() != 0) {
      os << "Flush and close failed";
      return false;
    }

    os << StringPrintf("Dex file exported at %s (%zd bytes)\n", out_dex_path.c_str(), fsize);
    os << std::flush;

    return true;
  }

  bool DumpOatClass(VariableIndentationOutputStream* vios,
                    const OatFile::OatClass& oat_class,
                    const DexFile& dex_file,
                    const ClassAccessor& class_accessor,
                    bool* stop_analysis) {
    bool success = true;
    bool addr_found = false;
    uint32_t class_method_index = 0;
    for (const ClassAccessor::Method& method : class_accessor.GetMethods()) {
      if (!DumpOatMethod(vios,
                         dex_file.GetClassDef(class_accessor.GetClassDefIndex()),
                         class_method_index,
                         oat_class,
                         dex_file,
                         method.GetIndex(),
                         method.GetCodeItem(),
                         method.GetAccessFlags(),
                         &addr_found)) {
        success = false;
      }
      if (addr_found) {
        *stop_analysis = true;
        return success;
      }
      class_method_index++;
    }
    vios->Stream() << std::flush;
    return success;
  }

  static constexpr uint32_t kPrologueBytes = 16;

  // When this was picked, the largest arm method was 55,256 bytes and arm64 was 50,412 bytes.
  static constexpr uint32_t kMaxCodeSize = 100 * 1000;

  bool DumpOatMethod(VariableIndentationOutputStream* vios,
                     const dex::ClassDef& class_def,
                     uint32_t class_method_index,
                     const OatFile::OatClass& oat_class,
                     const DexFile& dex_file,
                     uint32_t dex_method_idx,
                     const dex::CodeItem* code_item,
                     uint32_t method_access_flags,
                     bool* addr_found) {
    bool success = true;

    CodeItemDataAccessor code_item_accessor(dex_file, code_item);

    // TODO: Support regex
    std::string method_name = dex_file.GetMethodName(dex_file.GetMethodId(dex_method_idx));
    if (method_name.find(options_.method_filter_) == std::string::npos) {
      return success;
    }

    std::string pretty_method = dex_file.PrettyMethod(dex_method_idx, true);
    vios->Stream() << StringPrintf("%d: %s (dex_method_idx=%d)\n",
                                   class_method_index, pretty_method.c_str(),
                                   dex_method_idx);
    if (options_.list_methods_) {
      return success;
    }

    uint32_t oat_method_offsets_offset = oat_class.GetOatMethodOffsetsOffset(class_method_index);
    const OatMethodOffsets* oat_method_offsets = oat_class.GetOatMethodOffsets(class_method_index);
    const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_index);
    uint32_t code_offset = oat_method.GetCodeOffset();
    uint32_t code_size = oat_method.GetQuickCodeSize();
    if (resolved_addr2instr_ != 0) {
      if (resolved_addr2instr_ > code_offset + code_size) {
        return success;
      } else {
        *addr_found = true;  // stop analyzing file at next iteration
      }
    }

    // Everything below is indented at least once.
    ScopedIndentation indent1(vios);

    {
      vios->Stream() << "DEX CODE:\n";
      ScopedIndentation indent2(vios);
      if (code_item_accessor.HasCodeItem()) {
        uint32_t max_pc = code_item_accessor.InsnsSizeInCodeUnits();
        for (const DexInstructionPcPair& inst : code_item_accessor) {
          if (inst.DexPc() + inst->SizeInCodeUnits() > max_pc) {
            LOG(WARNING) << "GLITCH: run-away instruction at idx=0x" << std::hex << inst.DexPc();
            break;
          }
          vios->Stream() << StringPrintf("0x%04x: ", inst.DexPc()) << inst->DumpHexLE(5)
                         << StringPrintf("\t| %s\n", inst->DumpString(&dex_file).c_str());
        }
      }
    }

    std::unique_ptr<StackHandleScope<1>> hs;
    std::unique_ptr<verifier::MethodVerifier> verifier;
    if (Runtime::Current() != nullptr) {
      // We need to have the handle scope stay live until after the verifier since the verifier has
      // a handle to the dex cache from hs.
      ScopedObjectAccess soa(Thread::Current());
      hs.reset(new StackHandleScope<1>(Thread::Current()));
      vios->Stream() << "VERIFIER TYPE ANALYSIS:\n";
      ScopedIndentation indent2(vios);
      DumpVerifier(vios,
                   soa,
                   hs.get(),
                   dex_method_idx,
                   &dex_file,
                   class_def,
                   code_item,
                   method_access_flags);
    }
    {
      vios->Stream() << "OatMethodOffsets ";
      if (options_.absolute_addresses_) {
        vios->Stream() << StringPrintf("%p ", oat_method_offsets);
      }
      vios->Stream() << StringPrintf("(offset=0x%08zx)\n", AdjustOffset(oat_method_offsets_offset));
      if (oat_method_offsets_offset > oat_file_.Size()) {
        vios->Stream() << StringPrintf(
            "WARNING: oat method offsets offset 0x%08zx is past end of file 0x%08zx.\n",
            AdjustOffset(oat_method_offsets_offset),
            AdjustOffset(oat_file_.Size()));
        // If we can't read OatMethodOffsets, the rest of the data is dangerous to read.
        vios->Stream() << std::flush;
        return false;
      }

      ScopedIndentation indent2(vios);
      vios->Stream() << StringPrintf("code_offset: 0x%08zx ", AdjustOffset(code_offset));
      uint32_t aligned_code_begin = AlignCodeOffset(oat_method.GetCodeOffset());
      if (aligned_code_begin > oat_file_.Size()) {
        vios->Stream() << StringPrintf(
            "WARNING: code offset 0x%08zx is past end of file 0x%08zx.\n",
            AdjustOffset(aligned_code_begin),
            AdjustOffset(oat_file_.Size()));
        success = false;
      }
      vios->Stream() << "\n";
    }
    {
      vios->Stream() << "OatQuickMethodHeader ";
      uint32_t method_header_offset = oat_method.GetOatQuickMethodHeaderOffset();
      const OatQuickMethodHeader* method_header = oat_method.GetOatQuickMethodHeader();
      if (method_header != nullptr && AddStatsObject(method_header)) {
        stats_["QuickMethodHeader"].AddBytes(sizeof(*method_header));
      }
      if (options_.absolute_addresses_) {
        vios->Stream() << StringPrintf("%p ", method_header);
      }
      vios->Stream() << StringPrintf("(offset=0x%08zx)\n", AdjustOffset(method_header_offset));
      if (method_header_offset > oat_file_.Size() ||
          sizeof(OatQuickMethodHeader) > oat_file_.Size() - method_header_offset) {
        vios->Stream() << StringPrintf(
            "WARNING: oat quick method header at offset 0x%08zx is past end of file 0x%08zx.\n",
            AdjustOffset(method_header_offset),
            AdjustOffset(oat_file_.Size()));
        // If we can't read the OatQuickMethodHeader, the rest of the data is dangerous to read.
        vios->Stream() << std::flush;
        return false;
      }

      ScopedIndentation indent2(vios);
      vios->Stream() << "vmap_table: ";
      if (options_.absolute_addresses_) {
        vios->Stream() << StringPrintf("%p ", oat_method.GetVmapTable());
      }
      uint32_t vmap_table_offset =
          (method_header == nullptr) ? 0 : method_header->GetCodeInfoOffset();
      vios->Stream() << StringPrintf("(offset=0x%08zx)\n", AdjustOffset(vmap_table_offset));

      size_t vmap_table_offset_limit =
          (method_header == nullptr) ? 0 : (method_header->GetCode() - oat_file_.Begin());
      if (method_header != nullptr && vmap_table_offset >= vmap_table_offset_limit) {
        vios->Stream() << StringPrintf(
            "WARNING: vmap table offset 0x%08zx is past end of file 0x%08zx. ",
            AdjustOffset(vmap_table_offset),
            AdjustOffset(vmap_table_offset_limit));
        success = false;
      } else if (options_.dump_vmap_) {
        DumpVmapData(vios, oat_method, code_item_accessor);
      }
    }
    {
      vios->Stream() << "QuickMethodFrameInfo\n";

      ScopedIndentation indent2(vios);
      vios->Stream()
          << StringPrintf("frame_size_in_bytes: %zd\n", oat_method.GetFrameSizeInBytes());
      vios->Stream() << StringPrintf("core_spill_mask: 0x%08x ", oat_method.GetCoreSpillMask());
      DumpSpillMask(vios->Stream(), oat_method.GetCoreSpillMask(), false);
      vios->Stream() << "\n";
      vios->Stream() << StringPrintf("fp_spill_mask: 0x%08x ", oat_method.GetFpSpillMask());
      DumpSpillMask(vios->Stream(), oat_method.GetFpSpillMask(), true);
      vios->Stream() << "\n";
    }
    {
      // Based on spill masks from QuickMethodFrameInfo so placed
      // after it is dumped, but useful for understanding quick
      // code, so dumped here.
      ScopedIndentation indent2(vios);
      DumpVregLocations(vios->Stream(), oat_method, code_item_accessor);
    }
    {
      vios->Stream() << "CODE: ";
      {
        const void* code = oat_method.GetQuickCode();
        uint32_t aligned_code_begin = AlignCodeOffset(code_offset);
        uint32_t aligned_code_end = aligned_code_begin + code_size;
        if (AddStatsObject(code)) {
          stats_["Code"].AddBytes(code_size);
        }

        if (options_.absolute_addresses_) {
          vios->Stream() << StringPrintf("%p ", code);
        }
        vios->Stream() << StringPrintf("(code_offset=0x%08zx size=%u)%s\n",
                                       AdjustOffset(code_offset),
                                       code_size,
                                       code != nullptr ? "..." : "");

        ScopedIndentation indent2(vios);
        if (aligned_code_begin > oat_file_.Size()) {
          vios->Stream() << StringPrintf(
              "WARNING: start of code at 0x%08zx is past end of file 0x%08zx.",
              AdjustOffset(aligned_code_begin),
              AdjustOffset(oat_file_.Size()));
          success = false;
        } else if (aligned_code_end > oat_file_.Size()) {
          vios->Stream() << StringPrintf(
              "WARNING: end of code at 0x%08zx is past end of file 0x%08zx. code size is 0x%08x.\n",
              AdjustOffset(aligned_code_end),
              AdjustOffset(oat_file_.Size()),
              code_size);
          success = false;
          if (options_.disassemble_code_) {
            if (aligned_code_begin + kPrologueBytes <= oat_file_.Size()) {
              DumpCode(vios, oat_method, code_item_accessor, true, kPrologueBytes);
            }
          }
        } else if (code_size > kMaxCodeSize) {
          vios->Stream() << StringPrintf(
              "WARNING: "
              "code size %d is bigger than max expected threshold of %d. "
              "code size is 0x%08x.\n",
              code_size,
              kMaxCodeSize,
              code_size);
          success = false;
          if (options_.disassemble_code_) {
            if (aligned_code_begin + kPrologueBytes <= oat_file_.Size()) {
              DumpCode(vios, oat_method, code_item_accessor, true, kPrologueBytes);
            }
          }
        } else if (options_.disassemble_code_) {
          DumpCode(vios, oat_method, code_item_accessor, !success, 0);
        }
      }
    }
    vios->Stream() << std::flush;
    return success;
  }

  void DumpSpillMask(std::ostream& os, uint32_t spill_mask, bool is_float) {
    if (spill_mask == 0) {
      return;
    }
    os << "(";
    for (size_t i = 0; i < 32; i++) {
      if ((spill_mask & (1 << i)) != 0) {
        if (is_float) {
          os << "fr" << i;
        } else {
          os << "r" << i;
        }
        spill_mask ^= 1 << i;  // clear bit
        if (spill_mask != 0) {
          os << ", ";
        } else {
          break;
        }
      }
    }
    os << ")";
  }

  // Display data stored at the the vmap offset of an oat method.
  void DumpVmapData(VariableIndentationOutputStream* vios,
                    const OatFile::OatMethod& oat_method,
                    const CodeItemDataAccessor& code_item_accessor) {
    if (IsMethodGeneratedByOptimizingCompiler(oat_method, code_item_accessor)) {
      // The optimizing compiler outputs its CodeInfo data in the vmap table.
      const uint8_t* raw_code_info = oat_method.GetVmapTable();
      if (raw_code_info != nullptr) {
        CodeInfo code_info(raw_code_info);
        DCHECK(code_item_accessor.HasCodeItem());
        ScopedIndentation indent1(vios);
        DumpCodeInfo(vios, code_info, oat_method);
      }
    } else {
      // Otherwise, there is nothing to display.
    }
  }

  // Display a CodeInfo object emitted by the optimizing compiler.
  void DumpCodeInfo(VariableIndentationOutputStream* vios,
                    const CodeInfo& code_info,
                    const OatFile::OatMethod& oat_method) {
    code_info.Dump(vios,
                   oat_method.GetCodeOffset(),
                   options_.dump_code_info_stack_maps_,
                   instruction_set_);
  }

  static int GetOutVROffset(uint16_t out_num, InstructionSet isa) {
    // According to stack model, the first out is above the Method referernce.
    return static_cast<size_t>(InstructionSetPointerSize(isa)) + out_num * sizeof(uint32_t);
  }

  static uint32_t GetVRegOffsetFromQuickCode(const CodeItemDataAccessor& code_item_accessor,
                                             uint32_t core_spills,
                                             uint32_t fp_spills,
                                             size_t frame_size,
                                             int reg,
                                             InstructionSet isa) {
    PointerSize pointer_size = InstructionSetPointerSize(isa);
    if (kIsDebugBuild) {
      auto* runtime = Runtime::Current();
      if (runtime != nullptr) {
        CHECK_EQ(runtime->GetClassLinker()->GetImagePointerSize(), pointer_size);
      }
    }
    DCHECK_ALIGNED(frame_size, kStackAlignment);
    DCHECK_NE(reg, -1);
    int spill_size = POPCOUNT(core_spills) * GetBytesPerGprSpillLocation(isa)
        + POPCOUNT(fp_spills) * GetBytesPerFprSpillLocation(isa)
        + sizeof(uint32_t);  // Filler.
    int num_regs = code_item_accessor.RegistersSize() - code_item_accessor.InsSize();
    int temp_threshold = code_item_accessor.RegistersSize();
    const int max_num_special_temps = 1;
    if (reg == temp_threshold) {
      // The current method pointer corresponds to special location on stack.
      return 0;
    } else if (reg >= temp_threshold + max_num_special_temps) {
      /*
       * Special temporaries may have custom locations and the logic above deals with that.
       * However, non-special temporaries are placed relative to the outs.
       */
      int temps_start = code_item_accessor.OutsSize() * sizeof(uint32_t)
          + static_cast<size_t>(pointer_size) /* art method */;
      int relative_offset = (reg - (temp_threshold + max_num_special_temps)) * sizeof(uint32_t);
      return temps_start + relative_offset;
    } else if (reg < num_regs) {
      int locals_start = frame_size - spill_size - num_regs * sizeof(uint32_t);
      return locals_start + (reg * sizeof(uint32_t));
    } else {
      // Handle ins.
      return frame_size + ((reg - num_regs) * sizeof(uint32_t))
          + static_cast<size_t>(pointer_size) /* art method */;
    }
  }

  void DumpVregLocations(std::ostream& os, const OatFile::OatMethod& oat_method,
                         const CodeItemDataAccessor& code_item_accessor) {
    if (code_item_accessor.HasCodeItem()) {
      size_t num_locals_ins = code_item_accessor.RegistersSize();
      size_t num_ins = code_item_accessor.InsSize();
      size_t num_locals = num_locals_ins - num_ins;
      size_t num_outs = code_item_accessor.OutsSize();

      os << "vr_stack_locations:";
      for (size_t reg = 0; reg <= num_locals_ins; reg++) {
        // For readability, delimit the different kinds of VRs.
        if (reg == num_locals_ins) {
          os << "\n\tmethod*:";
        } else if (reg == num_locals && num_ins > 0) {
          os << "\n\tins:";
        } else if (reg == 0 && num_locals > 0) {
          os << "\n\tlocals:";
        }

        uint32_t offset = GetVRegOffsetFromQuickCode(code_item_accessor,
                                                     oat_method.GetCoreSpillMask(),
                                                     oat_method.GetFpSpillMask(),
                                                     oat_method.GetFrameSizeInBytes(),
                                                     reg,
                                                     GetInstructionSet());
        os << " v" << reg << "[sp + #" << offset << "]";
      }

      for (size_t out_reg = 0; out_reg < num_outs; out_reg++) {
        if (out_reg == 0) {
          os << "\n\touts:";
        }

        uint32_t offset = GetOutVROffset(out_reg, GetInstructionSet());
        os << " v" << out_reg << "[sp + #" << offset << "]";
      }

      os << "\n";
    }
  }

  // Has `oat_method` -- corresponding to the Dex `code_item` -- been compiled by
  // the optimizing compiler?
  static bool IsMethodGeneratedByOptimizingCompiler(
      const OatFile::OatMethod& oat_method,
      const CodeItemDataAccessor& code_item_accessor) {
    // If the native GC map is null and the Dex `code_item` is not
    // null, then this method has been compiled with the optimizing
    // compiler.
    return oat_method.GetQuickCode() != nullptr &&
           oat_method.GetVmapTable() != nullptr &&
           code_item_accessor.HasCodeItem();
  }

  void DumpVerifier(VariableIndentationOutputStream* vios,
                    ScopedObjectAccess& soa,
                    StackHandleScope<1>* hs,
                    uint32_t dex_method_idx,
                    const DexFile* dex_file,
                    const dex::ClassDef& class_def,
                    const dex::CodeItem* code_item,
                    uint32_t method_access_flags)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if ((method_access_flags & kAccNative) == 0) {
      Runtime* const runtime = Runtime::Current();
      DCHECK(options_.class_loader_ != nullptr);
      Handle<mirror::DexCache> dex_cache = hs->NewHandle(
          runtime->GetClassLinker()->RegisterDexFile(*dex_file, options_.class_loader_->Get()));
      CHECK(dex_cache != nullptr);
      ArtMethod* method = runtime->GetClassLinker()->ResolveMethodId(
          dex_method_idx, dex_cache, *options_.class_loader_);
      if (method == nullptr) {
        soa.Self()->ClearException();
        return;
      }
      verifier::MethodVerifier::VerifyMethodAndDump(
          soa.Self(),
          vios,
          dex_method_idx,
          dex_file,
          dex_cache,
          *options_.class_loader_,
          class_def,
          code_item,
          method_access_flags,
          /* api_level= */ 0);
    }
  }

  void DumpCode(VariableIndentationOutputStream* vios,
                const OatFile::OatMethod& oat_method,
                const CodeItemDataAccessor& code_item_accessor,
                bool bad_input, size_t code_size) {
    const void* quick_code = oat_method.GetQuickCode();

    if (code_size == 0) {
      code_size = oat_method.GetQuickCodeSize();
    }
    if (code_size == 0 || quick_code == nullptr) {
      vios->Stream() << "NO CODE!\n";
      return;
    } else if (!bad_input && IsMethodGeneratedByOptimizingCompiler(oat_method,
                                                                   code_item_accessor)) {
      // The optimizing compiler outputs its CodeInfo data in the vmap table.
      CodeInfo code_info(oat_method.GetVmapTable());
      if (AddStatsObject(oat_method.GetVmapTable())) {
        code_info.CollectSizeStats(oat_method.GetVmapTable(), stats_["CodeInfo"]);
      }
      std::unordered_map<uint32_t, std::vector<StackMap>> stack_maps;
      for (const StackMap& it : code_info.GetStackMaps()) {
        stack_maps[it.GetNativePcOffset(instruction_set_)].push_back(it);
      }

      const uint8_t* quick_native_pc = reinterpret_cast<const uint8_t*>(quick_code);
      size_t offset = 0;
      while (offset < code_size) {
        offset += disassembler_->Dump(vios->Stream(), quick_native_pc + offset);
        auto it = stack_maps.find(offset);
        if (it != stack_maps.end()) {
          ScopedIndentation indent1(vios);
          for (StackMap stack_map : it->second) {
            stack_map.Dump(vios, code_info, oat_method.GetCodeOffset(), instruction_set_);
          }
          stack_maps.erase(it);
        }
      }
      DCHECK_EQ(stack_maps.size(), 0u);  // Check that all stack maps have been printed.
    } else {
      const uint8_t* quick_native_pc = reinterpret_cast<const uint8_t*>(quick_code);
      size_t offset = 0;
      while (offset < code_size) {
        offset += disassembler_->Dump(vios->Stream(), quick_native_pc + offset);
      }
    }
  }

  std::pair<const uint8_t*, const uint8_t*> GetBootImageLiveObjectsDataRange(gc::Heap* heap) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const std::vector<gc::space::ImageSpace*>& boot_image_spaces = heap->GetBootImageSpaces();
    const ImageHeader& main_header = boot_image_spaces[0]->GetImageHeader();
    ObjPtr<mirror::ObjectArray<mirror::Object>> boot_image_live_objects =
        ObjPtr<mirror::ObjectArray<mirror::Object>>::DownCast(
            main_header.GetImageRoot<kWithoutReadBarrier>(ImageHeader::kBootImageLiveObjects));
    DCHECK(boot_image_live_objects != nullptr);
    DCHECK(heap->ObjectIsInBootImageSpace(boot_image_live_objects));
    const uint8_t* boot_image_live_objects_address =
        reinterpret_cast<const uint8_t*>(boot_image_live_objects.Ptr());
    uint32_t begin_offset = mirror::ObjectArray<mirror::Object>::OffsetOfElement(0).Uint32Value();
    uint32_t end_offset = mirror::ObjectArray<mirror::Object>::OffsetOfElement(
        boot_image_live_objects->GetLength()).Uint32Value();
    return std::make_pair(boot_image_live_objects_address + begin_offset,
                          boot_image_live_objects_address + end_offset);
  }

  void DumpDataImgRelRoEntries(std::ostream& os) {
    os << ".data.img.rel.ro: ";
    if (oat_file_.GetBootImageRelocations().empty()) {
      os << "empty.\n\n";
      return;
    }

    os << oat_file_.GetBootImageRelocations().size() << " entries.\n";
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr && !runtime->GetHeap()->GetBootImageSpaces().empty()) {
      const std::vector<gc::space::ImageSpace*>& boot_image_spaces =
          runtime->GetHeap()->GetBootImageSpaces();
      ScopedObjectAccess soa(Thread::Current());
      auto live_objects = GetBootImageLiveObjectsDataRange(runtime->GetHeap());
      const uint8_t* live_objects_begin = live_objects.first;
      const uint8_t* live_objects_end = live_objects.second;
      for (const uint32_t& object_offset : oat_file_.GetBootImageRelocations()) {
        uint32_t entry_index = &object_offset - oat_file_.GetBootImageRelocations().data();
        uint32_t entry_offset = entry_index * sizeof(oat_file_.GetBootImageRelocations()[0]);
        os << StringPrintf("  0x%x: 0x%08x", entry_offset, object_offset);
        uint8_t* address = boot_image_spaces[0]->Begin() + object_offset;
        bool found = false;
        for (gc::space::ImageSpace* space : boot_image_spaces) {
          uint64_t local_offset = address - space->Begin();
          if (local_offset < space->GetImageHeader().GetImageSize()) {
            if (space->GetImageHeader().GetObjectsSection().Contains(local_offset)) {
              if (address >= live_objects_begin && address < live_objects_end) {
                size_t index =
                    (address - live_objects_begin) / sizeof(mirror::HeapReference<mirror::Object>);
                os << StringPrintf("   0x%08x BootImageLiveObject[%zu]",
                                   object_offset,
                                   index);
              } else {
                ObjPtr<mirror::Object> o = reinterpret_cast<mirror::Object*>(address);
                if (o->IsString()) {
                  os << "   String: " << o->AsString()->ToModifiedUtf8();
                } else if (o->IsClass()) {
                  os << "   Class: " << o->AsClass()->PrettyDescriptor();
                } else {
                  os << StringPrintf("   0x%08x %s",
                                     object_offset,
                                     o->GetClass()->PrettyDescriptor().c_str());
                }
              }
            } else if (space->GetImageHeader().GetMethodsSection().Contains(local_offset)) {
              ArtMethod* m = reinterpret_cast<ArtMethod*>(address);
              os << "   ArtMethod: " << m->PrettyMethod();
            } else {
              os << StringPrintf("   0x%08x <unexpected section in %s>",
                                 object_offset,
                                 space->GetImageFilename().c_str());
            }
            found = true;
            break;
          }
        }
        if (!found) {
          os << StringPrintf("   0x%08x <outside boot image spaces>", object_offset);
        }
        os << "\n";
      }
    } else {
      for (const uint32_t& object_offset : oat_file_.GetBootImageRelocations()) {
        uint32_t entry_index = &object_offset - oat_file_.GetBootImageRelocations().data();
        uint32_t entry_offset = entry_index * sizeof(oat_file_.GetBootImageRelocations()[0]);
        os << StringPrintf("  0x%x: 0x%08x\n", entry_offset, object_offset);
      }
    }
    os << "\n";
  }

  template <typename NameGetter>
  void DumpBssEntries(std::ostream& os,
                      const char* slot_type,
                      const IndexBssMapping* mapping,
                      uint32_t number_of_indexes,
                      size_t slot_size,
                      NameGetter name) {
    os << ".bss mapping for " << slot_type << ": ";
    if (mapping == nullptr) {
      os << "empty.\n";
      return;
    }
    size_t index_bits = IndexBssMappingEntry::IndexBits(number_of_indexes);
    size_t num_valid_indexes = 0u;
    for (const IndexBssMappingEntry& entry : *mapping) {
      num_valid_indexes += 1u + POPCOUNT(entry.GetMask(index_bits));
    }
    os << mapping->size() << " entries for " << num_valid_indexes << " valid indexes.\n";
    os << std::hex;
    for (const IndexBssMappingEntry& entry : *mapping) {
      uint32_t index = entry.GetIndex(index_bits);
      uint32_t mask = entry.GetMask(index_bits);
      size_t bss_offset = entry.bss_offset - POPCOUNT(mask) * slot_size;
      for (uint32_t n : LowToHighBits(mask)) {
        size_t current_index = index - (32u - index_bits) + n;
        os << "  0x" << bss_offset << ": " << slot_type << ": " << name(current_index) << "\n";
        bss_offset += slot_size;
      }
      DCHECK_EQ(bss_offset, entry.bss_offset);
      os << "  0x" << bss_offset << ": " << slot_type << ": " << name(index) << "\n";
    }
    os << std::dec;
  }

  void DumpBssMappings(std::ostream& os,
                       const DexFile* dex_file,
                       const IndexBssMapping* method_bss_mapping,
                       const IndexBssMapping* type_bss_mapping,
                       const IndexBssMapping* public_type_bss_mapping,
                       const IndexBssMapping* package_type_bss_mapping,
                       const IndexBssMapping* string_bss_mapping,
                       const IndexBssMapping* method_type_bss_mapping) {
    DumpBssEntries(os,
                   "ArtMethod",
                   method_bss_mapping,
                   dex_file->NumMethodIds(),
                   static_cast<size_t>(GetInstructionSetPointerSize(instruction_set_)),
                   [=](uint32_t index) { return dex_file->PrettyMethod(index); });
    DumpBssEntries(os,
                   "Class",
                   type_bss_mapping,
                   dex_file->NumTypeIds(),
                   sizeof(GcRoot<mirror::Class>),
                   [=](uint32_t index) { return dex_file->PrettyType(dex::TypeIndex(index)); });
    DumpBssEntries(os,
                   "Public Class",
                   public_type_bss_mapping,
                   dex_file->NumTypeIds(),
                   sizeof(GcRoot<mirror::Class>),
                   [=](uint32_t index) { return dex_file->PrettyType(dex::TypeIndex(index)); });
    DumpBssEntries(os,
                   "Package Class",
                   package_type_bss_mapping,
                   dex_file->NumTypeIds(),
                   sizeof(GcRoot<mirror::Class>),
                   [=](uint32_t index) { return dex_file->PrettyType(dex::TypeIndex(index)); });
    DumpBssEntries(
        os,
        "String",
        string_bss_mapping,
        dex_file->NumStringIds(),
        sizeof(GcRoot<mirror::Class>),
        [=](uint32_t index) { return dex_file->GetStringData(dex::StringIndex(index)); });
    DumpBssEntries(os,
                   "MethodType",
                   method_type_bss_mapping,
                   dex_file->NumProtoIds(),
                   sizeof(GcRoot<mirror::MethodType>),
                   [=](uint32_t index) {
                     const dex::ProtoId& proto_id = dex_file->GetProtoId(dex::ProtoIndex(index));
                     return dex_file->GetProtoSignature(proto_id).ToString();
                   });
  }

  void DumpBssOffsets(std::ostream& os, const char* slot_type, const IndexBssMapping* mapping) {
    os << ".bss offset for " << slot_type << ": ";
    if (mapping == nullptr) {
      os << "empty.\n";
      return;
    }

    os << "Mapping size: " << mapping->size() << "\n";
    for (size_t i = 0; i < mapping->size(); ++i) {
      os << "Entry[" << i << "]: index_and_mask: "
         << mapping->At(i).index_and_mask
         << ", bss_offset: "
         << mapping->At(i).bss_offset << "\n";
    }

    // TODO(solanes, 154012332): We are dumping the raw values but we could make assumptions about
    // ordering of the entries and deconstruct even the `index_and_mask`. This would allow us to use
    // DumpBssEntries and dump more information. The size and alignment of the entry (ArtMethod*
    // depends on instruction set but Class and String references are 32-bit) and the difference
    // from the previous `bss_offset` (or from the "oatbss" symbol for the first item) tell us how
    // many .bss entries a single `IndexBssMappingEntry` should describe. So we know how many most
    // significant set bits represent the mask and the rest is the actual index. And the position of
    // the mask bits would allow reconstructing the other indexes.
  }

  // Adjusts an offset relative to the OAT file begin to an offset relative to the ELF file begin.
  size_t AdjustOffset(size_t offset) const { return (offset > 0) ? (oat_offset_ + offset) : 0; }

  const OatFile& oat_file_;
  size_t oat_offset_;
  const std::vector<const OatDexFile*> oat_dex_files_;
  const OatDumperOptions& options_;
  uint32_t resolved_addr2instr_;
  const InstructionSet instruction_set_;
  std::set<uintptr_t> offsets_;
  Disassembler* disassembler_;
  Stats stats_;
  std::unordered_set<const void*> seen_stats_objects_;
};

class ImageDumper {
 public:
  ImageDumper(std::ostream* os,
              gc::space::ImageSpace& image_space,
              const ImageHeader& image_header,
              OatDumperOptions* oat_dumper_options)
      : os_(os),
        vios_(os),
        indent1_(&vios_),
        image_space_(image_space),
        image_header_(image_header),
        oat_dumper_options_(oat_dumper_options) {}

  bool Dump() REQUIRES_SHARED(Locks::mutator_lock_) {
    std::ostream& os = *os_;
    std::ostream& indent_os = vios_.Stream();

    os << "MAGIC: " << image_header_.GetMagic() << "\n\n";

    os << "IMAGE LOCATION: " << image_space_.GetImageLocation() << "\n\n";

    os << "IMAGE BEGIN: " << reinterpret_cast<void*>(image_header_.GetImageBegin()) << "\n";
    os << "IMAGE SIZE: " << image_header_.GetImageSize() << "\n";
    os << "IMAGE CHECKSUM: " << std::hex << image_header_.GetImageChecksum() << std::dec << "\n\n";

    os << "OAT CHECKSUM: " << StringPrintf("0x%08x\n\n", image_header_.GetOatChecksum()) << "\n";
    os << "OAT FILE BEGIN:" << reinterpret_cast<void*>(image_header_.GetOatFileBegin()) << "\n";
    os << "OAT DATA BEGIN:" << reinterpret_cast<void*>(image_header_.GetOatDataBegin()) << "\n";
    os << "OAT DATA END:" << reinterpret_cast<void*>(image_header_.GetOatDataEnd()) << "\n";
    os << "OAT FILE END:" << reinterpret_cast<void*>(image_header_.GetOatFileEnd()) << "\n\n";

    os << "BOOT IMAGE BEGIN: " << reinterpret_cast<void*>(image_header_.GetBootImageBegin())
        << "\n";
    os << "BOOT IMAGE SIZE: " << image_header_.GetBootImageSize() << "\n\n";

    for (size_t i = 0; i < ImageHeader::kSectionCount; ++i) {
      auto section = static_cast<ImageHeader::ImageSections>(i);
      os << "IMAGE SECTION " << section << ": " << image_header_.GetImageSection(section) << "\n\n";
    }

    {
      os << "ROOTS: " << reinterpret_cast<void*>(image_header_.GetImageRoots().Ptr()) << "\n";
      static_assert(arraysize(image_roots_descriptions_) ==
          static_cast<size_t>(ImageHeader::kImageRootsMax), "sizes must match");
      DCHECK_LE(image_header_.GetImageRoots()->GetLength(), ImageHeader::kImageRootsMax);
      for (int32_t i = 0, size = image_header_.GetImageRoots()->GetLength(); i != size; ++i) {
        ImageHeader::ImageRoot image_root = static_cast<ImageHeader::ImageRoot>(i);
        const char* image_root_description = image_roots_descriptions_[i];
        ObjPtr<mirror::Object> image_root_object = image_header_.GetImageRoot(image_root);
        indent_os << StringPrintf("%s: %p\n", image_root_description, image_root_object.Ptr());
        if (image_root_object != nullptr && image_root_object->IsObjectArray()) {
          ObjPtr<mirror::ObjectArray<mirror::Object>> image_root_object_array
              = image_root_object->AsObjectArray<mirror::Object>();
          ScopedIndentation indent2(&vios_);
          for (int j = 0; j < image_root_object_array->GetLength(); j++) {
            ObjPtr<mirror::Object> value = image_root_object_array->Get(j);
            size_t run = 0;
            for (int32_t k = j + 1; k < image_root_object_array->GetLength(); k++) {
              if (value == image_root_object_array->Get(k)) {
                run++;
              } else {
                break;
              }
            }
            if (run == 0) {
              indent_os << StringPrintf("%d: ", j);
            } else {
              indent_os << StringPrintf("%d to %zd: ", j, j + run);
              j = j + run;
            }
            if (value != nullptr) {
              PrettyObjectValue(indent_os, value->GetClass(), value);
            } else {
              indent_os << j << ": null\n";
            }
          }
        }
      }
    }

    {
      os << "METHOD ROOTS\n";
      static_assert(arraysize(image_methods_descriptions_) ==
          static_cast<size_t>(ImageHeader::kImageMethodsCount), "sizes must match");
      for (int i = 0; i < ImageHeader::kImageMethodsCount; i++) {
        auto image_root = static_cast<ImageHeader::ImageMethod>(i);
        const char* description = image_methods_descriptions_[i];
        auto* image_method = image_header_.GetImageMethod(image_root);
        indent_os << StringPrintf("%s: %p\n", description, image_method);
      }
    }
    os << "\n";

    Runtime* const runtime = Runtime::Current();
    std::string image_filename = image_space_.GetImageFilename();
    std::string oat_location = ImageHeader::GetOatLocationFromImageLocation(image_filename);
    os << "OAT LOCATION: " << oat_location;
    os << "\n";
    std::string error_msg;
    const OatFile* oat_file = image_space_.GetOatFile();
    if (oat_file == nullptr) {
      oat_file = runtime->GetOatFileManager().FindOpenedOatFileFromOatLocation(oat_location);
    }
    if (oat_file == nullptr) {
      oat_file = OatFile::Open(/*zip_fd=*/ -1,
                               oat_location,
                               oat_location,
                               /*executable=*/ false,
                               /*low_4gb=*/ false,
                               &error_msg);
    }
    if (oat_file == nullptr) {
      os << "OAT FILE NOT FOUND: " << error_msg << "\n";
      return EXIT_FAILURE;
    }
    os << "\n";

    stats_.oat_file_bytes = oat_file->Size();
    stats_.oat_file_stats.AddBytes(oat_file->Size());

    oat_dumper_.reset(new OatDumper(*oat_file, *oat_dumper_options_));

    for (const OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
      CHECK(oat_dex_file != nullptr);
      stats_.oat_dex_file_sizes.push_back(std::make_pair(oat_dex_file->GetDexFileLocation(),
                                                         oat_dex_file->FileSize()));
    }

    os << "OBJECTS:\n" << std::flush;

    // Loop through the image space and dump its objects.
    gc::Heap* heap = runtime->GetHeap();
    Thread* self = Thread::Current();
    {
      {
        WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
        heap->FlushAllocStack();
      }
      // Since FlushAllocStack() above resets the (active) allocation
      // stack. Need to revoke the thread-local allocation stacks that
      // point into it.
      ScopedThreadSuspension sts(self, ThreadState::kNative);
      ScopedSuspendAll ssa(__FUNCTION__);
      heap->RevokeAllThreadLocalAllocationStacks(self);
    }
    {
      auto dump_visitor = [&](mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
        DumpObject(obj);
      };
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      // Dump the normal objects before ArtMethods.
      image_space_.GetLiveBitmap()->Walk(dump_visitor);
      indent_os << "\n";
      // TODO: Dump fields.
      // Dump methods after.
      image_header_.VisitPackedArtMethods([&](ArtMethod& method)
          REQUIRES_SHARED(Locks::mutator_lock_) {
        std::ostream& indent_os = vios_.Stream();
        indent_os << &method << " " << " ArtMethod: " << method.PrettyMethod() << "\n";
        DumpMethod(&method, indent_os);
        indent_os << "\n";
      },  image_space_.Begin(), image_header_.GetPointerSize());
      // Dump the large objects separately.
      heap->GetLargeObjectsSpace()->GetLiveBitmap()->Walk(dump_visitor);
      indent_os << "\n";
    }
    os << "STATS:\n" << std::flush;
    std::unique_ptr<File> file(OS::OpenFileForReading(image_filename.c_str()));
    size_t data_size = image_header_.GetDataSize();  // stored size in file.
    if (file == nullptr) {
      LOG(WARNING) << "Failed to find image in " << image_filename;
    } else {
      size_t file_bytes = file->GetLength();
      // If the image is compressed, adjust to decompressed size.
      size_t uncompressed_size = image_header_.GetImageSize() - sizeof(ImageHeader);
      if (!image_header_.HasCompressedBlock()) {
        DCHECK_EQ(uncompressed_size, data_size) << "Sizes should match for uncompressed image";
      }
      file_bytes += uncompressed_size - data_size;
      stats_.art_file_stats.AddBytes(file_bytes);
      stats_.art_file_stats["Header"].AddBytes(sizeof(ImageHeader));
    }

    size_t pointer_size = static_cast<size_t>(image_header_.GetPointerSize());
    CHECK_ALIGNED(image_header_.GetFieldsSection().Offset(), 4);
    CHECK_ALIGNED_PARAM(image_header_.GetMethodsSection().Offset(), pointer_size);
    CHECK_ALIGNED(image_header_.GetInternedStringsSection().Offset(), 8);
    CHECK_ALIGNED(image_header_.GetImageBitmapSection().Offset(), kElfSegmentAlignment);

    for (size_t i = 0; i < ImageHeader::ImageSections::kSectionCount; i++) {
      ImageHeader::ImageSections index = ImageHeader::ImageSections(i);
      const char* name = ImageHeader::GetImageSectionName(index);
      stats_.art_file_stats[name].AddBytes(image_header_.GetImageSection(index).Size());
    }

    stats_.object_stats.AddBytes(image_header_.GetObjectsSection().Size());
    stats_.Dump(os);
    os << "\n";

    os << std::flush;

    return oat_dumper_->Dump(os);
  }

 private:
  static void PrettyObjectValue(std::ostream& os,
                                ObjPtr<mirror::Class> type,
                                ObjPtr<mirror::Object> value)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    CHECK(type != nullptr);
    if (value == nullptr) {
      os << StringPrintf("null   %s\n", type->PrettyDescriptor().c_str());
    } else if (type->IsStringClass()) {
      ObjPtr<mirror::String> string = value->AsString();
      os << StringPrintf("%p   String: %s\n",
                         string.Ptr(),
                         PrintableString(string->ToModifiedUtf8().c_str()).c_str());
    } else if (type->IsClassClass()) {
      ObjPtr<mirror::Class> klass = value->AsClass();
      os << StringPrintf("%p   Class: %s\n",
                         klass.Ptr(),
                         mirror::Class::PrettyDescriptor(klass).c_str());
    } else {
      os << StringPrintf("%p   %s\n", value.Ptr(), type->PrettyDescriptor().c_str());
    }
  }

  static void PrintField(std::ostream& os, ArtField* field, ObjPtr<mirror::Object> obj)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    os << StringPrintf("%s: ", field->GetName());
    switch (field->GetTypeAsPrimitiveType()) {
      case Primitive::kPrimLong:
        os << StringPrintf("%" PRId64 " (0x%" PRIx64 ")\n", field->Get64(obj), field->Get64(obj));
        break;
      case Primitive::kPrimDouble:
        os << StringPrintf("%f (%a)\n", field->GetDouble(obj), field->GetDouble(obj));
        break;
      case Primitive::kPrimFloat:
        os << StringPrintf("%f (%a)\n", field->GetFloat(obj), field->GetFloat(obj));
        break;
      case Primitive::kPrimInt:
        os << StringPrintf("%d (0x%x)\n", field->Get32(obj), field->Get32(obj));
        break;
      case Primitive::kPrimChar:
        os << StringPrintf("%u (0x%x)\n", field->GetChar(obj), field->GetChar(obj));
        break;
      case Primitive::kPrimShort:
        os << StringPrintf("%d (0x%x)\n", field->GetShort(obj), field->GetShort(obj));
        break;
      case Primitive::kPrimBoolean:
        os << StringPrintf("%s (0x%x)\n", field->GetBoolean(obj) ? "true" : "false",
            field->GetBoolean(obj));
        break;
      case Primitive::kPrimByte:
        os << StringPrintf("%d (0x%x)\n", field->GetByte(obj), field->GetByte(obj));
        break;
      case Primitive::kPrimNot: {
        // Get the value, don't compute the type unless it is non-null as we don't want
        // to cause class loading.
        ObjPtr<mirror::Object> value = field->GetObj(obj);
        if (value == nullptr) {
          os << StringPrintf("null   %s\n", PrettyDescriptor(field->GetTypeDescriptor()).c_str());
        } else {
          // Grab the field type without causing resolution.
          ObjPtr<mirror::Class> field_type = field->LookupResolvedType();
          if (field_type != nullptr) {
            PrettyObjectValue(os, field_type, value);
          } else {
            os << StringPrintf("%p   %s\n",
                               value.Ptr(),
                               PrettyDescriptor(field->GetTypeDescriptor()).c_str());
          }
        }
        break;
      }
      default:
        os << "unexpected field type: " << field->GetTypeDescriptor() << "\n";
        break;
    }
  }

  static void DumpFields(std::ostream& os, mirror::Object* obj, ObjPtr<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    ObjPtr<mirror::Class> super = klass->GetSuperClass();
    if (super != nullptr) {
      DumpFields(os, obj, super);
    }
    for (ArtField& field : klass->GetFields()) {
      if (!field.IsStatic()) {
        PrintField(os, &field, obj);
      }
    }
  }

  bool InDumpSpace(const mirror::Object* object) {
    return image_space_.Contains(object);
  }

  const void* GetQuickOatCodeBegin(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_) {
    const void* quick_code = m->GetEntryPointFromQuickCompiledCodePtrSize(
        image_header_.GetPointerSize());
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    if (class_linker->IsQuickResolutionStub(quick_code) ||
        class_linker->IsQuickToInterpreterBridge(quick_code) ||
        class_linker->IsNterpTrampoline(quick_code) ||
        class_linker->IsQuickGenericJniStub(quick_code) ||
        class_linker->IsJniDlsymLookupStub(quick_code) ||
        class_linker->IsJniDlsymLookupCriticalStub(quick_code)) {
      quick_code = oat_dumper_->GetQuickOatCode(m);
    }
    if (oat_dumper_->GetInstructionSet() == InstructionSet::kThumb2) {
      quick_code = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(quick_code) & ~0x1);
    }
    return quick_code;
  }

  uint32_t GetQuickOatCodeSize(ArtMethod* m)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const uint32_t* oat_code_begin = reinterpret_cast<const uint32_t*>(GetQuickOatCodeBegin(m));
    if (oat_code_begin == nullptr) {
      return 0;
    }
    OatQuickMethodHeader* method_header = reinterpret_cast<OatQuickMethodHeader*>(
        reinterpret_cast<uintptr_t>(oat_code_begin) - sizeof(OatQuickMethodHeader));
    return method_header->GetCodeSize();
  }

  const void* GetQuickOatCodeEnd(ArtMethod* m)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const uint8_t* oat_code_begin = reinterpret_cast<const uint8_t*>(GetQuickOatCodeBegin(m));
    if (oat_code_begin == nullptr) {
      return nullptr;
    }
    return oat_code_begin + GetQuickOatCodeSize(m);
  }

  void DumpObject(mirror::Object* obj) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(obj != nullptr);
    if (!InDumpSpace(obj)) {
      return;
    }

    std::ostream& os = vios_.Stream();

    ObjPtr<mirror::Class> obj_class = obj->GetClass();
    if (obj_class->IsArrayClass()) {
      os << StringPrintf("%p: %s length:%d\n", obj, obj_class->PrettyDescriptor().c_str(),
                         obj->AsArray()->GetLength());
    } else if (obj_class->IsClassClass()) {
      ObjPtr<mirror::Class> klass = obj->AsClass();
      os << StringPrintf("%p: java.lang.Class \"%s\" (",
                         obj,
                         mirror::Class::PrettyDescriptor(klass).c_str())
         << klass->GetStatus() << ")\n";
    } else if (obj_class->IsStringClass()) {
      os << StringPrintf("%p: java.lang.String %s\n",
                         obj,
                         PrintableString(obj->AsString()->ToModifiedUtf8().c_str()).c_str());
    } else {
      os << StringPrintf("%p: %s\n", obj, obj_class->PrettyDescriptor().c_str());
    }
    ScopedIndentation indent1(&vios_);
    DumpFields(os, obj, obj_class);
    if (obj->IsObjectArray()) {
      ObjPtr<mirror::ObjectArray<mirror::Object>> obj_array = obj->AsObjectArray<mirror::Object>();
      for (int32_t i = 0, length = obj_array->GetLength(); i < length; i++) {
        ObjPtr<mirror::Object> value = obj_array->Get(i);
        size_t run = 0;
        for (int32_t j = i + 1; j < length; j++) {
          if (value == obj_array->Get(j)) {
            run++;
          } else {
            break;
          }
        }
        if (run == 0) {
          os << StringPrintf("%d: ", i);
        } else {
          os << StringPrintf("%d to %zd: ", i, i + run);
          i = i + run;
        }
        ObjPtr<mirror::Class> value_class =
            (value == nullptr) ? obj_class->GetComponentType() : value->GetClass();
        PrettyObjectValue(os, value_class, value);
      }
    } else if (obj_class->IsClassClass()) {
      ObjPtr<mirror::Class> klass = obj->AsClass();

      if (kBitstringSubtypeCheckEnabled) {
        os << "SUBTYPE_CHECK_BITS: ";
        SubtypeCheck<ObjPtr<mirror::Class>>::Dump(klass, os);
        os << "\n";
      }

      if (klass->ShouldHaveEmbeddedVTable()) {
        os << "EMBEDDED VTABLE:\n";
        ScopedIndentation indent2(&vios_);
        const PointerSize pointer_size = image_header_.GetPointerSize();
        for (size_t i = 0, length = klass->GetEmbeddedVTableLength(); i != length; ++i) {
          os << i << ": "
             << ArtMethod::PrettyMethod(klass->GetEmbeddedVTableEntry(i, pointer_size)) << '\n';
        }
      }

      if (klass->HasStaticFields()) {
        os << "STATICS:\n";
        ScopedIndentation indent2(&vios_);
        for (ArtField& field : klass->GetFields()) {
          if (field.IsStatic()) {
            PrintField(os, &field, field.GetDeclaringClass());
          }
        }
      }
    }
    std::string temp;
    const char* desc = obj_class->GetDescriptor(&temp);
    desc = stats_.descriptors.emplace(desc).first->c_str();  // Dedup and keep alive.
    stats_.object_stats[desc].AddBytes(obj->SizeOf());
  }

  void DumpMethod(ArtMethod* method, std::ostream& indent_os)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(method != nullptr);
    const PointerSize pointer_size = image_header_.GetPointerSize();
    if (method->IsNative()) {
      const void* quick_oat_code_begin = GetQuickOatCodeBegin(method);
      bool first_occurrence;
      uint32_t quick_oat_code_size = GetQuickOatCodeSize(method);
      ComputeOatSize(quick_oat_code_begin, &first_occurrence);
      if (first_occurrence) {
        stats_.oat_file_stats["native_code"].AddBytes(quick_oat_code_size);
      }
      if (quick_oat_code_begin != method->GetEntryPointFromQuickCompiledCodePtrSize(
          image_header_.GetPointerSize())) {
        indent_os << StringPrintf("OAT CODE: %p\n", quick_oat_code_begin);
      }
    } else if (method->IsAbstract() || method->IsClassInitializer()) {
      // Don't print information for these.
    } else if (method->IsRuntimeMethod()) {
      if (method == Runtime::Current()->GetResolutionMethod()) {
        const void* resolution_trampoline =
            method->GetEntryPointFromQuickCompiledCodePtrSize(image_header_.GetPointerSize());
        indent_os << StringPrintf("Resolution trampoline: %p\n", resolution_trampoline);
        const void* critical_native_resolution_trampoline =
            method->GetEntryPointFromJniPtrSize(image_header_.GetPointerSize());
        indent_os << StringPrintf("Resolution trampoline for @CriticalNative: %p\n",
                                  critical_native_resolution_trampoline);
      } else {
        ImtConflictTable* table = method->GetImtConflictTable(image_header_.GetPointerSize());
        if (table != nullptr) {
          indent_os << "IMT conflict table " << table << " method: ";
          for (size_t i = 0, count = table->NumEntries(pointer_size); i < count; ++i) {
            indent_os << ArtMethod::PrettyMethod(table->GetImplementationMethod(i, pointer_size))
                      << " ";
          }
        }
      }
    } else {
      CodeItemDataAccessor code_item_accessor(method->DexInstructionData());
      size_t dex_instruction_bytes = code_item_accessor.InsnsSizeInCodeUnits() * 2;
      stats_.dex_instruction_bytes += dex_instruction_bytes;

      const void* quick_oat_code_begin = GetQuickOatCodeBegin(method);
      const void* quick_oat_code_end = GetQuickOatCodeEnd(method);

      bool first_occurrence;
      size_t vmap_table_bytes = 0u;
      if (quick_oat_code_begin != nullptr) {
        OatQuickMethodHeader* method_header = reinterpret_cast<OatQuickMethodHeader*>(
            reinterpret_cast<uintptr_t>(quick_oat_code_begin) - sizeof(OatQuickMethodHeader));
        vmap_table_bytes = ComputeOatSize(method_header->GetOptimizedCodeInfoPtr(),
                                          &first_occurrence);
        if (first_occurrence) {
          stats_.vmap_table_bytes += vmap_table_bytes;
        }
      }

      uint32_t quick_oat_code_size = GetQuickOatCodeSize(method);
      ComputeOatSize(quick_oat_code_begin, &first_occurrence);
      if (first_occurrence) {
        stats_.managed_code_bytes += quick_oat_code_size;
        art::Stats& managed_code_stats = stats_.oat_file_stats["managed_code"];
        managed_code_stats.AddBytes(quick_oat_code_size);
        if (method->IsConstructor()) {
          if (method->IsStatic()) {
            managed_code_stats["class_initializer"].AddBytes(quick_oat_code_size);
          } else if (dex_instruction_bytes > kLargeConstructorDexBytes) {
            managed_code_stats["large_initializer"].AddBytes(quick_oat_code_size);
          }
        } else if (dex_instruction_bytes > kLargeMethodDexBytes) {
          managed_code_stats["large_method"].AddBytes(quick_oat_code_size);
        }
      }
      stats_.managed_code_bytes_ignoring_deduplication += quick_oat_code_size;

      uint32_t method_access_flags = method->GetAccessFlags();

      indent_os << StringPrintf("OAT CODE: %p-%p\n", quick_oat_code_begin, quick_oat_code_end);
      indent_os << StringPrintf("SIZE: Dex Instructions=%zd StackMaps=%zd AccessFlags=0x%x\n",
                                dex_instruction_bytes,
                                vmap_table_bytes,
                                method_access_flags);

      size_t total_size = dex_instruction_bytes +
          vmap_table_bytes + quick_oat_code_size + ArtMethod::Size(image_header_.GetPointerSize());

      double expansion =
      static_cast<double>(quick_oat_code_size) / static_cast<double>(dex_instruction_bytes);
      stats_.ComputeOutliers(total_size, expansion, method);
    }
  }

  std::set<const void*> already_seen_;
  // Compute the size of the given data within the oat file and whether this is the first time
  // this data has been requested
  size_t ComputeOatSize(const void* oat_data, bool* first_occurrence) {
    if (already_seen_.count(oat_data) == 0) {
      *first_occurrence = true;
      already_seen_.insert(oat_data);
    } else {
      *first_occurrence = false;
    }
    return oat_dumper_->ComputeSize(oat_data);
  }

 public:
  struct Stats {
    art::Stats art_file_stats;
    art::Stats oat_file_stats;
    art::Stats object_stats;
    std::set<std::string> descriptors;

    size_t oat_file_bytes = 0u;
    size_t managed_code_bytes = 0u;
    size_t managed_code_bytes_ignoring_deduplication = 0u;

    size_t vmap_table_bytes = 0u;

    size_t dex_instruction_bytes = 0u;

    std::vector<ArtMethod*> method_outlier;
    std::vector<size_t> method_outlier_size;
    std::vector<double> method_outlier_expansion;
    std::vector<std::pair<std::string, size_t>> oat_dex_file_sizes;

    Stats() {}

    double PercentOfOatBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(oat_file_bytes)) * 100;
    }

    void ComputeOutliers(size_t total_size, double expansion, ArtMethod* method) {
      method_outlier_size.push_back(total_size);
      method_outlier_expansion.push_back(expansion);
      method_outlier.push_back(method);
    }

    void DumpOutliers(std::ostream& os)
        REQUIRES_SHARED(Locks::mutator_lock_) {
      size_t sum_of_sizes = 0;
      size_t sum_of_sizes_squared = 0;
      size_t sum_of_expansion = 0;
      size_t sum_of_expansion_squared = 0;
      size_t n = method_outlier_size.size();
      if (n <= 1) {
        return;
      }
      for (size_t i = 0; i < n; i++) {
        size_t cur_size = method_outlier_size[i];
        sum_of_sizes += cur_size;
        sum_of_sizes_squared += cur_size * cur_size;
        double cur_expansion = method_outlier_expansion[i];
        sum_of_expansion += cur_expansion;
        sum_of_expansion_squared += cur_expansion * cur_expansion;
      }
      size_t size_mean = sum_of_sizes / n;
      size_t size_variance = (sum_of_sizes_squared - sum_of_sizes * size_mean) / (n - 1);
      double expansion_mean = sum_of_expansion / n;
      double expansion_variance =
          (sum_of_expansion_squared - sum_of_expansion * expansion_mean) / (n - 1);

      // Dump methods whose size is a certain number of standard deviations from the mean
      size_t dumped_values = 0;
      size_t skipped_values = 0;
      for (size_t i = 100; i > 0; i--) {  // i is the current number of standard deviations
        size_t cur_size_variance = i * i * size_variance;
        bool first = true;
        for (size_t j = 0; j < n; j++) {
          size_t cur_size = method_outlier_size[j];
          if (cur_size > size_mean) {
            size_t cur_var = cur_size - size_mean;
            cur_var = cur_var * cur_var;
            if (cur_var > cur_size_variance) {
              if (dumped_values > 20) {
                if (i == 1) {
                  skipped_values++;
                } else {
                  i = 2;  // jump to counting for 1 standard deviation
                  break;
                }
              } else {
                if (first) {
                  os << "\nBig methods (size > " << i << " standard deviations the norm):\n";
                  first = false;
                }
                os << ArtMethod::PrettyMethod(method_outlier[j]) << " requires storage of "
                    << PrettySize(cur_size) << "\n";
                method_outlier_size[j] = 0;  // don't consider this method again
                dumped_values++;
              }
            }
          }
        }
      }
      if (skipped_values > 0) {
        os << "... skipped " << skipped_values
           << " methods with size > 1 standard deviation from the norm\n";
      }
      os << std::flush;

      // Dump methods whose expansion is a certain number of standard deviations from the mean
      dumped_values = 0;
      skipped_values = 0;
      for (size_t i = 10; i > 0; i--) {  // i is the current number of standard deviations
        double cur_expansion_variance = i * i * expansion_variance;
        bool first = true;
        for (size_t j = 0; j < n; j++) {
          double cur_expansion = method_outlier_expansion[j];
          if (cur_expansion > expansion_mean) {
            size_t cur_var = cur_expansion - expansion_mean;
            cur_var = cur_var * cur_var;
            if (cur_var > cur_expansion_variance) {
              if (dumped_values > 20) {
                if (i == 1) {
                  skipped_values++;
                } else {
                  i = 2;  // jump to counting for 1 standard deviation
                  break;
                }
              } else {
                if (first) {
                  os << "\nLarge expansion methods (size > " << i
                      << " standard deviations the norm):\n";
                  first = false;
                }
                os << ArtMethod::PrettyMethod(method_outlier[j]) << " expanded code by "
                   << cur_expansion << "\n";
                method_outlier_expansion[j] = 0.0;  // don't consider this method again
                dumped_values++;
              }
            }
          }
        }
      }
      if (skipped_values > 0) {
        os << "... skipped " << skipped_values
           << " methods with expansion > 1 standard deviation from the norm\n";
      }
      os << "\n" << std::flush;
    }

    void Dump(std::ostream& os)
        REQUIRES_SHARED(Locks::mutator_lock_) {
      VariableIndentationOutputStream vios(&os);
      art_file_stats.DumpSizes(vios, "ArtFile");
      os << "\n" << std::flush;
      object_stats.DumpSizes(vios, "Objects");
      os << "\n" << std::flush;
      oat_file_stats.DumpSizes(vios, "OatFile");
      os << "\n" << std::flush;

      for (const std::pair<std::string, size_t>& oat_dex_file_size : oat_dex_file_sizes) {
        os << StringPrintf("%s = %zd (%2.0f%% of oat file bytes)\n",
                           oat_dex_file_size.first.c_str(), oat_dex_file_size.second,
                           PercentOfOatBytes(oat_dex_file_size.second));
      }

      os << "\n" << StringPrintf("vmap_table_bytes       = %7zd (%2.0f%% of oat file bytes)\n\n",
                                 vmap_table_bytes, PercentOfOatBytes(vmap_table_bytes))
         << std::flush;

      os << StringPrintf("dex_instruction_bytes = %zd\n", dex_instruction_bytes)
         << StringPrintf("managed_code_bytes expansion = %.2f (ignoring deduplication %.2f)\n\n",
                         static_cast<double>(managed_code_bytes) /
                             static_cast<double>(dex_instruction_bytes),
                         static_cast<double>(managed_code_bytes_ignoring_deduplication) /
                             static_cast<double>(dex_instruction_bytes))
         << std::flush;

      DumpOutliers(os);
    }
  } stats_;

 private:
  enum {
    // Number of bytes for a constructor to be considered large. Based on the 1000 basic block
    // threshold, we assume 2 bytes per instruction and 2 instructions per block.
    kLargeConstructorDexBytes = 4000,
    // Number of bytes for a method to be considered large. Based on the 4000 basic block
    // threshold, we assume 2 bytes per instruction and 2 instructions per block.
    kLargeMethodDexBytes = 16000
  };

  // For performance, use the *os_ directly for anything that doesn't need indentation
  // and prepare an indentation stream with default indentation 1.
  std::ostream* os_;
  VariableIndentationOutputStream vios_;
  ScopedIndentation indent1_;

  gc::space::ImageSpace& image_space_;
  const ImageHeader& image_header_;
  std::unique_ptr<OatDumper> oat_dumper_;
  OatDumperOptions* oat_dumper_options_;

  DISALLOW_COPY_AND_ASSIGN(ImageDumper);
};

static std::unique_ptr<OatFile> OpenOat(const std::string& oat_filename,
                                        const std::optional<std::string>& dex_filename,
                                        std::string* error_msg) {
  if (!dex_filename.has_value()) {
    LOG(WARNING) << "No dex filename provided, "
                 << "oatdump might fail if the oat file does not contain the dex code.";
  }
  ArrayRef<const std::string> dex_filenames =
      dex_filename.has_value() ? ArrayRef<const std::string>(&dex_filename.value(), /*size=*/1) :
                                 ArrayRef<const std::string>();
  return std::unique_ptr<OatFile>(OatFile::Open(/*zip_fd=*/-1,
                                                oat_filename,
                                                oat_filename,
                                                /*executable=*/false,
                                                /*low_4gb=*/false,
                                                dex_filenames,
                                                /*dex_files=*/{},
                                                /*reservation=*/nullptr,
                                                error_msg));
}

static int DumpImage(gc::space::ImageSpace* image_space,
                     OatDumperOptions* options,
                     std::ostream* os) REQUIRES_SHARED(Locks::mutator_lock_) {
  const ImageHeader& image_header = image_space->GetImageHeader();
  if (!image_header.IsValid()) {
    LOG(ERROR) << "Invalid image header " << image_space->GetImageLocation();
    return EXIT_FAILURE;
  }
  ImageDumper image_dumper(os, *image_space, image_header, options);
  if (!image_dumper.Dump()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

static int DumpImages(Runtime* runtime, OatDumperOptions* options, std::ostream* os) {
  // Dumping the image, no explicit class loader.
  ScopedNullHandle<mirror::ClassLoader> null_class_loader;
  options->class_loader_ = &null_class_loader;

  if (options->app_image_ != nullptr) {
    if (!options->oat_filename_.has_value()) {
      LOG(ERROR) << "Can not dump app image without app oat file";
      return EXIT_FAILURE;
    }
    // We can't know if the app image is 32 bits yet, but it contains pointers into the oat file.
    // We need to map the oat file in the low 4gb or else the fixup wont be able to fit oat file
    // pointers into 32 bit pointer sized ArtMethods.
    std::string error_msg;
    std::unique_ptr<OatFile> oat_file =
        OpenOat(*options->oat_filename_, options->dex_filename_, &error_msg);
    if (oat_file == nullptr) {
      LOG(ERROR) << "Failed to open oat file " << *options->oat_filename_ << " with error "
                 << error_msg;
      return EXIT_FAILURE;
    }
    std::unique_ptr<gc::space::ImageSpace> space(
        gc::space::ImageSpace::CreateFromAppImage(options->app_image_, oat_file.get(), &error_msg));
    if (space == nullptr) {
      LOG(ERROR) << "Failed to open app image " << options->app_image_ << " with error "
                 << error_msg;
      return EXIT_FAILURE;
    }
    // Open dex files for the image.
    ScopedObjectAccess soa(Thread::Current());
    std::vector<std::unique_ptr<const DexFile>> dex_files;
    if (!runtime->GetClassLinker()->OpenImageDexFiles(space.get(), &dex_files, &error_msg)) {
      LOG(ERROR) << "Failed to open app image dex files " << options->app_image_ << " with error "
                 << error_msg;
      return EXIT_FAILURE;
    }
    // Dump the actual image.
    return DumpImage(space.get(), options, os);
  }

  gc::Heap* heap = runtime->GetHeap();
  if (!heap->HasBootImageSpace()) {
    LOG(ERROR) << "No image spaces";
    return EXIT_FAILURE;
  }
  ScopedObjectAccess soa(Thread::Current());
  for (gc::space::ImageSpace* image_space : heap->GetBootImageSpaces()) {
    int result = DumpImage(image_space, options, os);
    if (result != EXIT_SUCCESS) {
      return result;
    }
  }
  return EXIT_SUCCESS;
}

static jobject InstallOatFile(Runtime* runtime,
                              std::unique_ptr<OatFile> oat_file,
                              std::vector<const DexFile*>* class_path)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  CHECK(self != nullptr);
  // Need well-known-classes.
  WellKnownClasses::Init(self->GetJniEnv());

  // Open dex files.
  OatFile* oat_file_ptr = oat_file.get();
  ClassLinker* class_linker = runtime->GetClassLinker();
  runtime->GetOatFileManager().RegisterOatFile(std::move(oat_file));
  for (const OatDexFile* odf : oat_file_ptr->GetOatDexFiles()) {
    std::string error_msg;
    const DexFile* const dex_file = OpenDexFile(odf, &error_msg);
    CHECK(dex_file != nullptr) << error_msg;
    class_path->push_back(dex_file);
  }

  // Need a class loader. Fake that we're a compiler.
  // Note: this will run initializers through the unstarted runtime, so make sure it's
  //       initialized.
  interpreter::UnstartedRuntime::Initialize();

  jobject class_loader = class_linker->CreatePathClassLoader(self, *class_path);

  // Need to register dex files to get a working dex cache.
  for (const DexFile* dex_file : *class_path) {
    ObjPtr<mirror::DexCache> dex_cache = class_linker->RegisterDexFile(
        *dex_file, self->DecodeJObject(class_loader)->AsClassLoader());
    CHECK(dex_cache != nullptr);
  }

  return class_loader;
}

static int DumpOatWithRuntime(Runtime* runtime,
                              std::unique_ptr<OatFile> oat_file,
                              OatDumperOptions* options,
                              std::ostream* os) {
  CHECK(runtime != nullptr && oat_file != nullptr && options != nullptr);
  ScopedObjectAccess soa(Thread::Current());

  OatFile* oat_file_ptr = oat_file.get();
  std::vector<const DexFile*> class_path;
  jobject class_loader = InstallOatFile(runtime, std::move(oat_file), &class_path);

  // Use the class loader while dumping.
  StackHandleScope<1> scope(soa.Self());
  Handle<mirror::ClassLoader> loader_handle = scope.NewHandle(
      soa.Decode<mirror::ClassLoader>(class_loader));
  options->class_loader_ = &loader_handle;

  OatDumper oat_dumper(*oat_file_ptr, *options);
  bool success = oat_dumper.Dump(*os);
  return (success) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int DumpOatWithoutRuntime(OatFile* oat_file, OatDumperOptions* options, std::ostream* os) {
  CHECK(oat_file != nullptr && options != nullptr);
  // No image = no class loader.
  ScopedNullHandle<mirror::ClassLoader> null_class_loader;
  options->class_loader_ = &null_class_loader;

  OatDumper oat_dumper(*oat_file, *options);
  bool success = oat_dumper.Dump(*os);
  return (success) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int DumpOat(Runtime* runtime, OatDumperOptions* options, std::ostream* os) {
  std::string error_msg;
  std::unique_ptr<OatFile> oat_file =
      OpenOat(*options->oat_filename_, options->dex_filename_, &error_msg);
  if (oat_file == nullptr) {
    LOG(ERROR) << "Failed to open oat file from '" << *options->oat_filename_ << "': " << error_msg;
    return EXIT_FAILURE;
  }

  if (runtime != nullptr) {
    return DumpOatWithRuntime(runtime, std::move(oat_file), options, os);
  } else {
    return DumpOatWithoutRuntime(oat_file.get(), options, os);
  }
}

static int SymbolizeOat(const char* oat_filename,
                        const char* dex_filename,
                        std::string& output_name,
                        bool no_bits) {
  std::string error_msg;
  std::unique_ptr<OatFile> oat_file =
      OpenOat(oat_filename,
              dex_filename != nullptr ? std::make_optional(dex_filename) : std::nullopt,
              &error_msg);
  if (oat_file == nullptr) {
    LOG(ERROR) << "Failed to open oat file from '" << oat_filename << "': " << error_msg;
    return EXIT_FAILURE;
  }

  bool result;
  // Try to produce an ELF file of the same type. This is finicky, as we have used 32-bit ELF
  // files for 64-bit code in the past.
  if (Is64BitInstructionSet(oat_file->GetOatHeader().GetInstructionSet())) {
    OatSymbolizer<ElfTypes64> oat_symbolizer(oat_file.get(), output_name, no_bits);
    result = oat_symbolizer.Symbolize();
  } else {
    OatSymbolizer<ElfTypes32> oat_symbolizer(oat_file.get(), output_name, no_bits);
    result = oat_symbolizer.Symbolize();
  }
  if (!result) {
    LOG(ERROR) << "Failed to symbolize";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

class IMTDumper {
 public:
  static bool Dump(Runtime* runtime,
                   const std::string& imt_file,
                   bool dump_imt_stats,
                   const char* oat_filename,
                   const char* dex_filename) {
    Thread* self = Thread::Current();

    ScopedObjectAccess soa(self);
    StackHandleScope<1> scope(self);
    MutableHandle<mirror::ClassLoader> class_loader = scope.NewHandle<mirror::ClassLoader>(nullptr);
    std::vector<const DexFile*> class_path;

    if (oat_filename != nullptr) {
      std::string error_msg;
      std::unique_ptr<OatFile> oat_file =
          OpenOat(oat_filename,
                  dex_filename != nullptr ? std::make_optional(dex_filename) : std::nullopt,
                  &error_msg);
      if (oat_file == nullptr) {
        LOG(ERROR) << "Failed to open oat file from '" << oat_filename << "': " << error_msg;
        return false;
      }

      class_loader.Assign(soa.Decode<mirror::ClassLoader>(
          InstallOatFile(runtime, std::move(oat_file), &class_path)));
    } else {
      class_loader.Assign(nullptr);  // Boot classloader. Just here for explicit documentation.
      class_path = runtime->GetClassLinker()->GetBootClassPath();
    }

    if (!imt_file.empty()) {
      return DumpImt(runtime, imt_file, class_loader);
    }

    if (dump_imt_stats) {
      return DumpImtStats(runtime, class_path, class_loader);
    }

    LOG(FATAL) << "Should not reach here";
    UNREACHABLE();
  }

 private:
  static bool DumpImt(Runtime* runtime,
                      const std::string& imt_file,
                      Handle<mirror::ClassLoader> h_class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    std::vector<std::string> lines = ReadCommentedInputFromFile(imt_file);
    std::unordered_set<std::string> prepared;

    for (const std::string& line : lines) {
      // A line should be either a class descriptor, in which case we will dump the complete IMT,
      // or a class descriptor and an interface method, in which case we will lookup the method,
      // determine its IMT slot, and check the class' IMT.
      size_t first_space = line.find(' ');
      if (first_space == std::string::npos) {
        DumpIMTForClass(runtime, line, h_class_loader, &prepared);
      } else {
        DumpIMTForMethod(runtime,
                         line.substr(0, first_space),
                         line.substr(first_space + 1, std::string::npos),
                         h_class_loader,
                         &prepared);
      }
      std::cerr << std::endl;
    }

    return true;
  }

  static bool DumpImtStats(Runtime* runtime,
                           const std::vector<const DexFile*>& dex_files,
                           Handle<mirror::ClassLoader> h_class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    size_t without_imt = 0;
    size_t with_imt = 0;
    std::map<size_t, size_t> histogram;

    ClassLinker* class_linker = runtime->GetClassLinker();
    const PointerSize pointer_size = class_linker->GetImagePointerSize();
    std::unordered_set<std::string> prepared;

    Thread* self = Thread::Current();
    StackHandleScope<1> scope(self);
    MutableHandle<mirror::Class> h_klass(scope.NewHandle<mirror::Class>(nullptr));

    for (const DexFile* dex_file : dex_files) {
      for (uint32_t class_def_index = 0;
           class_def_index != dex_file->NumClassDefs();
           ++class_def_index) {
        const dex::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
        h_klass.Assign(
            class_linker->FindClass(self, *dex_file, class_def.class_idx_, h_class_loader));
        if (h_klass == nullptr) {
          std::cerr << "Warning: could not load "
                    << dex_file->GetTypeDescriptor(class_def.class_idx_) << std::endl;
          continue;
        }

        if (HasNoIMT(runtime, h_klass, pointer_size, &prepared)) {
          without_imt++;
          continue;
        }

        ImTable* im_table = PrepareAndGetImTable(runtime, h_klass, pointer_size, &prepared);
        if (im_table == nullptr) {
          // Should not happen, but accept.
          without_imt++;
          continue;
        }

        with_imt++;
        for (size_t imt_index = 0; imt_index != ImTable::kSize; ++imt_index) {
          ArtMethod* ptr = im_table->Get(imt_index, pointer_size);
          if (ptr->IsRuntimeMethod()) {
            if (ptr->IsImtUnimplementedMethod()) {
              histogram[0]++;
            } else {
              ImtConflictTable* current_table = ptr->GetImtConflictTable(pointer_size);
              histogram[current_table->NumEntries(pointer_size)]++;
            }
          } else {
            histogram[1]++;
          }
        }
      }
    }

    std::cerr << "IMT stats:"
              << std::endl << std::endl;

    std::cerr << "  " << with_imt << " classes with IMT."
              << std::endl << std::endl;
    std::cerr << "  " << without_imt << " classes without IMT (or copy from Object)."
              << std::endl << std::endl;

    double sum_one = 0;
    size_t count_one = 0;

    std::cerr << "  " << "IMT histogram" << std::endl;
    for (auto& bucket : histogram) {
      std::cerr << "    " << bucket.first << " " << bucket.second << std::endl;
      if (bucket.first > 0) {
        sum_one += bucket.second * bucket.first;
        count_one += bucket.second;
      }
    }

    double count_zero = count_one + histogram[0];
    std::cerr << "   Stats:" << std::endl;
    std::cerr << "     Average depth (including empty): " << (sum_one / count_zero) << std::endl;
    std::cerr << "     Average depth (excluding empty): " << (sum_one / count_one) << std::endl;

    return true;
  }

  // Return whether the given class has no IMT (or the one shared with java.lang.Object).
  static bool HasNoIMT(Runtime* runtime,
                       Handle<mirror::Class> klass,
                       const PointerSize pointer_size,
                       std::unordered_set<std::string>* prepared)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (klass->IsObjectClass() || !klass->ShouldHaveImt()) {
      return true;
    }

    if (klass->GetImt(pointer_size) == nullptr) {
      PrepareClass(runtime, klass, prepared);
    }

    ObjPtr<mirror::Class> object_class = GetClassRoot<mirror::Object>();
    DCHECK(object_class->IsObjectClass());

    bool result = klass->GetImt(pointer_size) == object_class->GetImt(pointer_size);

    if (klass->GetIfTable()->Count() == 0) {
      DCHECK(result);
    }

    return result;
  }

  static void PrintTable(ImtConflictTable* table, PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (table == nullptr) {
      std::cerr << "    <No IMT?>" << std::endl;
      return;
    }
    size_t table_index = 0;
    for (;;) {
      ArtMethod* ptr = table->GetInterfaceMethod(table_index, pointer_size);
      if (ptr == nullptr) {
        return;
      }
      table_index++;
      std::cerr << "    " << ptr->PrettyMethod(true) << std::endl;
    }
  }

  static ImTable* PrepareAndGetImTable(Runtime* runtime,
                                       Thread* self,
                                       Handle<mirror::ClassLoader> h_loader,
                                       const std::string& class_name,
                                       const PointerSize pointer_size,
                                       /*out*/ ObjPtr<mirror::Class>* klass_out,
                                       /*inout*/ std::unordered_set<std::string>* prepared)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (class_name.empty()) {
      return nullptr;
    }

    std::string descriptor;
    if (class_name[0] == 'L') {
      descriptor = class_name;
    } else {
      descriptor = DotToDescriptor(class_name);
    }

    ObjPtr<mirror::Class> klass = runtime->GetClassLinker()->FindClass(
        self, descriptor.c_str(), descriptor.length(), h_loader);

    if (klass == nullptr) {
      self->ClearException();
      std::cerr << "Did not find " <<  class_name << std::endl;
      *klass_out = nullptr;
      return nullptr;
    }

    StackHandleScope<1> scope(Thread::Current());
    Handle<mirror::Class> h_klass = scope.NewHandle<mirror::Class>(klass);

    ImTable* ret = PrepareAndGetImTable(runtime, h_klass, pointer_size, prepared);
    *klass_out = h_klass.Get();
    return ret;
  }

  static ImTable* PrepareAndGetImTable(Runtime* runtime,
                                       Handle<mirror::Class> h_klass,
                                       const PointerSize pointer_size,
                                       /*inout*/ std::unordered_set<std::string>* prepared)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    PrepareClass(runtime, h_klass, prepared);
    return h_klass->GetImt(pointer_size);
  }

  static void DumpIMTForClass(Runtime* runtime,
                              const std::string& class_name,
                              Handle<mirror::ClassLoader> h_loader,
                              std::unordered_set<std::string>* prepared)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const PointerSize pointer_size = runtime->GetClassLinker()->GetImagePointerSize();
    ObjPtr<mirror::Class> klass;
    ImTable* imt = PrepareAndGetImTable(runtime,
                                        Thread::Current(),
                                        h_loader,
                                        class_name,
                                        pointer_size,
                                        &klass,
                                        prepared);
    if (imt == nullptr) {
      return;
    }

    std::cerr << class_name << std::endl << " IMT:" << std::endl;
    for (size_t index = 0; index < ImTable::kSize; ++index) {
      std::cerr << "  " << index << ":" << std::endl;
      ArtMethod* ptr = imt->Get(index, pointer_size);
      if (ptr->IsRuntimeMethod()) {
        if (ptr->IsImtUnimplementedMethod()) {
          std::cerr << "    <empty>" << std::endl;
        } else {
          ImtConflictTable* current_table = ptr->GetImtConflictTable(pointer_size);
          PrintTable(current_table, pointer_size);
        }
      } else {
        std::cerr << "    " << ptr->PrettyMethod(true) << std::endl;
      }
    }

    std::cerr << " Interfaces:" << std::endl;
    // Run through iftable, find methods that slot here, see if they fit.
    ObjPtr<mirror::IfTable> if_table = klass->GetIfTable();
    for (size_t i = 0, num_interfaces = klass->GetIfTableCount(); i < num_interfaces; ++i) {
      ObjPtr<mirror::Class> iface = if_table->GetInterface(i);
      std::string iface_name;
      std::cerr << "  " << iface->GetDescriptor(&iface_name) << std::endl;

      for (ArtMethod& iface_method : iface->GetVirtualMethods(pointer_size)) {
        uint32_t class_hash, name_hash, signature_hash;
        ImTable::GetImtHashComponents(*iface_method.GetDexFile(),
                                      iface_method.GetDexMethodIndex(),
                                      &class_hash,
                                      &name_hash,
                                      &signature_hash);
        uint32_t imt_slot = ImTable::GetImtIndex(&iface_method);
        // Note: For default methods we use the dex method index for calculating the slot.
        // For abstract methods the compile-time constant `kImTableHashUseName` determines
        // whether we use the component hashes (current behavior) or the dex method index.
        std::cerr << "    " << iface_method.PrettyMethod(true)
            << " slot=" << imt_slot
            << " dex_method_index=" << iface_method.GetDexMethodIndex()
            << std::hex
            << " class_hash=0x" << class_hash
            << " name_hash=0x" << name_hash
            << " signature_hash=0x" << signature_hash
            << std::dec
            << std::endl;
      }
    }
  }

  static void DumpIMTForMethod(Runtime* runtime,
                               const std::string& class_name,
                               const std::string& method,
                               Handle<mirror::ClassLoader> h_loader,
                               /*inout*/ std::unordered_set<std::string>* prepared)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const PointerSize pointer_size = runtime->GetClassLinker()->GetImagePointerSize();
    ObjPtr<mirror::Class> klass;
    ImTable* imt = PrepareAndGetImTable(runtime,
                                        Thread::Current(),
                                        h_loader,
                                        class_name,
                                        pointer_size,
                                        &klass,
                                        prepared);
    if (imt == nullptr) {
      return;
    }

    std::cerr << class_name << " <" << method << ">" << std::endl;
    for (size_t index = 0; index < ImTable::kSize; ++index) {
      ArtMethod* ptr = imt->Get(index, pointer_size);
      if (ptr->IsRuntimeMethod()) {
        if (ptr->IsImtUnimplementedMethod()) {
          continue;
        }

        ImtConflictTable* current_table = ptr->GetImtConflictTable(pointer_size);
        if (current_table == nullptr) {
          continue;
        }

        size_t table_index = 0;
        for (;;) {
          ArtMethod* ptr2 = current_table->GetInterfaceMethod(table_index, pointer_size);
          if (ptr2 == nullptr) {
            break;
          }
          table_index++;

          std::string p_name = ptr2->PrettyMethod(true);
          if (p_name.starts_with(method)) {
            std::cerr << "  Slot "
                      << index
                      << " ("
                      << current_table->NumEntries(pointer_size)
                      << ")"
                      << std::endl;
            PrintTable(current_table, pointer_size);
            return;
          }
        }
      } else {
        std::string p_name = ptr->PrettyMethod(true);
        if (p_name.starts_with(method)) {
          std::cerr << "  Slot " << index << " (1)" << std::endl;
          std::cerr << "    " << p_name << std::endl;
        } else {
          // Run through iftable, find methods that slot here, see if they fit.
          ObjPtr<mirror::IfTable> if_table = klass->GetIfTable();
          for (size_t i = 0, num_interfaces = klass->GetIfTableCount(); i < num_interfaces; ++i) {
            ObjPtr<mirror::Class> iface = if_table->GetInterface(i);
            size_t num_methods = iface->NumDeclaredVirtualMethods();
            if (num_methods > 0) {
              for (ArtMethod& iface_method : iface->GetMethods(pointer_size)) {
                if (ImTable::GetImtIndex(&iface_method) == index) {
                  std::string i_name = iface_method.PrettyMethod(true);
                  if (i_name.starts_with(method)) {
                    std::cerr << "  Slot " << index << " (1)" << std::endl;
                    std::cerr << "    " << p_name << " (" << i_name << ")" << std::endl;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Read lines from the given stream, dropping comments and empty lines
  static std::vector<std::string> ReadCommentedInputStream(std::istream& in_stream) {
    std::vector<std::string> output;
    while (in_stream.good()) {
      std::string dot;
      std::getline(in_stream, dot);
      if (dot.starts_with("#") || dot.empty()) {
        continue;
      }
      output.push_back(dot);
    }
    return output;
  }

  // Read lines from the given file, dropping comments and empty lines.
  static std::vector<std::string> ReadCommentedInputFromFile(const std::string& input_filename) {
    std::unique_ptr<std::ifstream> input_file(new std::ifstream(input_filename, std::ifstream::in));
    if (input_file.get() == nullptr) {
      LOG(ERROR) << "Failed to open input file " << input_filename;
      return std::vector<std::string>();
    }
    std::vector<std::string> result = ReadCommentedInputStream(*input_file);
    input_file->close();
    return result;
  }

  // Prepare a class, i.e., ensure it has a filled IMT. Will do so recursively for superclasses,
  // and note in the given set that the work was done.
  static void PrepareClass(Runtime* runtime,
                           Handle<mirror::Class> h_klass,
                           /*inout*/ std::unordered_set<std::string>* done)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (!h_klass->ShouldHaveImt()) {
      return;
    }

    std::string name;
    name = h_klass->GetDescriptor(&name);

    if (done->find(name) != done->end()) {
      return;
    }
    done->insert(name);

    if (h_klass->HasSuperClass()) {
      StackHandleScope<1> h(Thread::Current());
      PrepareClass(runtime, h.NewHandle<mirror::Class>(h_klass->GetSuperClass()), done);
    }

    if (!h_klass->IsTemp()) {
      runtime->GetClassLinker()->FillIMTAndConflictTables(h_klass.Get());
    }
  }
};

enum class OatDumpMode {
  kSymbolize,
  kDumpImt,
  kDumpImage,
  kDumpOat,
};

struct OatdumpArgs : public CmdlineArgs {
 protected:
  using Base = CmdlineArgs;

  ParseStatus ParseCustom(const char* raw_option,
                          size_t raw_option_length,
                          std::string* error_msg) override {
    DCHECK_EQ(strlen(raw_option), raw_option_length);
    {
      ParseStatus base_parse = Base::ParseCustom(raw_option, raw_option_length, error_msg);
      if (base_parse != kParseUnknownArgument) {
        return base_parse;
      }
    }

    std::string_view option(raw_option, raw_option_length);
    if (option.starts_with("--oat-file=")) {
      oat_filename_ = raw_option + strlen("--oat-file=");
    } else if (option.starts_with("--dex-file=")) {
      dex_filename_ = raw_option + strlen("--dex-file=");
    } else if (option.starts_with("--image=")) {
      image_location_ = raw_option + strlen("--image=");
    } else if (option == "--no-dump:vmap") {
      dump_vmap_ = false;
    } else if (option =="--dump:code_info_stack_maps") {
      dump_code_info_stack_maps_ = true;
    } else if (option == "--no-disassemble") {
      disassemble_code_ = false;
    } else if (option =="--header-only") {
      dump_header_only_ = true;
    } else if (option.starts_with("--symbolize=")) {
      oat_filename_ = raw_option + strlen("--symbolize=");
      symbolize_ = true;
    } else if (option.starts_with("--only-keep-debug")) {
      only_keep_debug_ = true;
    } else if (option.starts_with("--class-filter=")) {
      class_filter_ = raw_option + strlen("--class-filter=");
    } else if (option.starts_with("--method-filter=")) {
      method_filter_ = raw_option + strlen("--method-filter=");
    } else if (option.starts_with("--list-classes")) {
      list_classes_ = true;
    } else if (option.starts_with("--list-methods")) {
      list_methods_ = true;
    } else if (option.starts_with("--export-dex-to=")) {
      export_dex_location_ = raw_option + strlen("--export-dex-to=");
    } else if (option.starts_with("--addr2instr=")) {
      if (!android::base::ParseUint(raw_option + strlen("--addr2instr="), &addr2instr_)) {
        *error_msg = "Address conversion failed";
        return kParseError;
      }
    } else if (option.starts_with("--app-image=")) {
      app_image_ = raw_option + strlen("--app-image=");
    } else if (option.starts_with("--app-oat=")) {
      app_oat_ = raw_option + strlen("--app-oat=");
    } else if (option.starts_with("--dump-imt=")) {
      imt_dump_ = std::string(option.substr(strlen("--dump-imt=")));
    } else if (option == "--dump-imt-stats") {
      imt_stat_dump_ = true;
    } else if (option == "--dump-method-and-offset-as-json") {
      dump_method_and_offset_as_json = true;
    } else {
      return kParseUnknownArgument;
    }

    return kParseOk;
  }

  ParseStatus ParseChecks(std::string* error_msg) override {
    if (image_location_ != nullptr) {
      if (!boot_image_locations_.empty()) {
        std::cerr << "Warning: Invalid combination of --boot-image and --image\n";
        std::cerr << "Use --image alone to dump boot image(s)\n";
        std::cerr << "Ignoring --boot-image\n";
        std::cerr << "\n";
        boot_image_locations_.clear();
      }
      Split(image_location_, ':', &boot_image_locations_);
    }

    // Perform the parent checks.
    ParseStatus parent_checks = Base::ParseChecks(error_msg);
    if (parent_checks != kParseOk) {
      return parent_checks;
    }

    // Perform our own checks.
    if (image_location_ == nullptr && app_image_ == nullptr && oat_filename_ == nullptr) {
      *error_msg = "Either --image, --app-image, --oat-file, or --symbolize must be specified";
      return kParseError;
    }

    if (app_image_ != nullptr && image_location_ != nullptr) {
      std::cerr << "Warning: Combining --app-image with --image is no longer supported\n";
      std::cerr << "Use --app-image alone to dump an app image, and optionally pass --boot-image "
                   "to specify the boot image that the app image is based on\n";
      std::cerr << "Use --image alone to dump boot image(s)\n";
      std::cerr << "Ignoring --image\n";
      std::cerr << "\n";
      image_location_ = nullptr;
    }

    if (image_location_ != nullptr && oat_filename_ != nullptr) {
      *error_msg =
          "--image and --oat-file must not be specified together\n"
          "Use --image alone to dump both boot image(s) and their oat file(s)\n"
          "Use --oat-file alone to dump an oat file";
      return kParseError;
    }

    if (app_oat_ != nullptr) {
      std::cerr << "Warning: --app-oat is deprecated. Use --oat-file instead\n";
      std::cerr << "\n";
      oat_filename_ = app_oat_;
    }

    if (boot_image_locations_.empty() && app_image_ != nullptr) {
      // At this point, boot image inference is impossible or has failed, and the user has been
      // warned about the failure.
      // When dumping an app image, we need at least one valid boot image, so we have to stop.
      // When dumping other things, we can continue to start the runtime in imageless mode.
      *error_msg = "--boot-image must be specified";
      return kParseError;
    }

    return kParseOk;
  }

  std::string GetUsage() const override {
    std::string usage;

    usage += R"(
Usage: oatdump [options] ...

Examples:
- Dump a primary boot image with its oat file.
    oatdump --image=/system/framework/boot.art

- Dump a primary boot image and extension(s) with their oat files.
    oatdump --image=/system/framework/boot.art:/system/framework/boot-framework-adservices.art

- Dump an app image with its oat file.
    oatdump --app-image=app.art --oat-file=app.odex [--dex-file=app.apk] [--boot-image=boot.art]

- Dump an app oat file.
    oatdump --oat-file=app.odex [--dex-file=app.apk] [--boot-image=boot.art]

- Dump IMT collisions. (See --dump-imt for details.)
    oatdump --oat-file=app.odex --dump-imt=imt.txt [--dex-file=app.apk] [--boot-image=boot.art]
        [--dump-imt-stats]

- Symbolize an oat file. (See --symbolize for details.)
    oatdump --symbolize=app.odex [--dex-file=app.apk] [--only-keep-debug]

Options:
  --oat-file=<file.oat>: dumps an oat file with the given filename.
      Example: --oat-file=/system/framework/arm64/boot.oat

  --image=<file.art>: dumps boot image(s) specified at the given location.
      Example: --image=/system/framework/boot.art

  --app-image=<file.art>: dumps an app image with the given filename.
      Must also have a specified app oat file (with --oat-file).
      Example: --app-image=app.art

  --app-oat=<file.odex>: deprecated. Use --oat-file instead.

)";

    usage += Base::GetUsage();

    usage +=  // Optional.
        "  --no-dump:vmap may be used to disable vmap dumping.\n"
        "      Example: --no-dump:vmap\n"
        "\n"
        "  --dump:code_info_stack_maps enables dumping of stack maps in CodeInfo sections.\n"
        "      Example: --dump:code_info_stack_maps\n"
        "\n"
        "  --no-disassemble may be used to disable disassembly.\n"
        "      Example: --no-disassemble\n"
        "\n"
        "  --header-only may be used to print only the oat header.\n"
        "      Example: --header-only\n"
        "\n"
        "  --list-classes may be used to list target file classes (can be used with filters).\n"
        "      Example: --list-classes\n"
        "      Example: --list-classes --class-filter=com.example.foo\n"
        "\n"
        "  --list-methods may be used to list target file methods (can be used with filters).\n"
        "      Example: --list-methods\n"
        "      Example: --list-methods --class-filter=com.example --method-filter=foo\n"
        "\n"
        "  --symbolize=<file.oat>: output a copy of file.oat with elf symbols included.\n"
        "      Example: --symbolize=/system/framework/boot.oat\n"
        "\n"
        "  --only-keep-debug: modifies the behaviour of --symbolize so that\n"
        "      .rodata and .text sections are omitted in the output file to save space.\n"
        "      Example: --symbolize=/system/framework/boot.oat --only-keep-debug\n"
        "\n"
        "  --class-filter=<class name>: only dumps classes that contain the filter.\n"
        "      Example: --class-filter=com.example.foo\n"
        "\n"
        "  --method-filter=<method name>: only dumps methods that contain the filter.\n"
        "      Example: --method-filter=foo\n"
        "\n"
        "  --dump-method-and-offset-as-json: dumps fully qualified method names and\n"
        "                                    signatures ONLY, in a standard json format.\n"
        "      Example: --dump-method-and-offset-as-json\n"
        "\n"
        "  --export-dex-to=<directory>: may be used to export oat embedded dex files.\n"
        "      Example: --export-dex-to=/data/local/tmp\n"
        "\n"
        "  --addr2instr=<address>: output matching method disassembled code from relative\n"
        "                          address (e.g. PC from crash dump)\n"
        "      Example: --addr2instr=0x00001a3b\n"
        "\n"
        "  --dump-imt=<file.txt>: output IMT collisions (if any) for the given receiver\n"
        "                         types and interface methods in the given file. The file\n"
        "                         is read line-wise, where each line should either be a class\n"
        "                         name or descriptor, or a class name/descriptor and a prefix\n"
        "                         of a complete method name (separated by a whitespace).\n"
        "      Example: --dump-imt=imt.txt\n"
        "\n"
        "  --dump-imt-stats: modifies the behavior of --dump-imt to also output IMT statistics\n"
        "      for the boot image.\n"
        "      Example: --dump-imt-stats"
        "\n";

    return usage;
  }

 public:
  OatDumpMode GetMode() {
    // Keep the order of precedence for backward compatibility.
    if (symbolize_) {
      return OatDumpMode::kSymbolize;
    }
    if (!imt_dump_.empty()) {
      return OatDumpMode::kDumpImt;
    }
    if (image_location_ != nullptr || app_image_ != nullptr) {
      return OatDumpMode::kDumpImage;
    }
    CHECK_NE(oat_filename_, nullptr);
    return OatDumpMode::kDumpOat;
  }

  const char* oat_filename_ = nullptr;
  const char* dex_filename_ = nullptr;
  const char* class_filter_ = "";
  const char* method_filter_ = "";
  const char* image_location_ = nullptr;
  std::string elf_filename_prefix_;
  std::string imt_dump_;
  bool dump_vmap_ = true;
  bool dump_code_info_stack_maps_ = false;
  bool disassemble_code_ = true;
  bool symbolize_ = false;
  bool only_keep_debug_ = false;
  bool list_classes_ = false;
  bool list_methods_ = false;
  bool dump_header_only_ = false;
  bool imt_stat_dump_ = false;
  bool dump_method_and_offset_as_json = false;
  uint32_t addr2instr_ = 0;
  const char* export_dex_location_ = nullptr;
  const char* app_image_ = nullptr;
  const char* app_oat_ = nullptr;
};

struct OatdumpMain : public CmdlineMain<OatdumpArgs> {
  bool NeedsRuntime() override {
    CHECK(args_ != nullptr);

    OatDumpMode mode = args_->GetMode();

    // Only enable absolute_addresses for image dumping.
    bool absolute_addresses = mode == OatDumpMode::kDumpImage;

    oat_dumper_options_.reset(new OatDumperOptions(args_->dump_vmap_,
                                                   args_->dump_code_info_stack_maps_,
                                                   args_->disassemble_code_,
                                                   absolute_addresses,
                                                   args_->class_filter_,
                                                   args_->method_filter_,
                                                   args_->list_classes_,
                                                   args_->list_methods_,
                                                   args_->dump_header_only_,
                                                   args_->dump_method_and_offset_as_json,
                                                   args_->export_dex_location_,
                                                   args_->app_image_,
                                                   args_->oat_filename_,
                                                   args_->dex_filename_,
                                                   args_->addr2instr_));

    switch (mode) {
      case OatDumpMode::kDumpImt:
      case OatDumpMode::kDumpImage:
        return true;
      case OatDumpMode::kSymbolize:
        return false;
      case OatDumpMode::kDumpOat:
        std::string error_msg;
        if (CanDumpWithRuntime(&error_msg)) {
          LOG(INFO) << "Dumping oat file with runtime";
          return true;
        } else {
          LOG(INFO) << ART_FORMAT("Cannot dump oat file with runtime: {}. Dumping without runtime",
                                  error_msg);
          return false;
        }
    }
  }

  bool ExecuteWithoutRuntime() override {
    CHECK(args_ != nullptr);

    OatDumpMode mode = args_->GetMode();
    CHECK(mode == OatDumpMode::kSymbolize || mode == OatDumpMode::kDumpOat);

    MemMap::Init();

    if (mode == OatDumpMode::kSymbolize) {
      // ELF has special kind of section called SHT_NOBITS which allows us to create
      // sections which exist but their data is omitted from the ELF file to save space.
      // This is what "strip --only-keep-debug" does when it creates separate ELF file
      // with only debug data. We use it in similar way to exclude .rodata and .text.
      bool no_bits = args_->only_keep_debug_;
      return SymbolizeOat(
                 args_->oat_filename_, args_->dex_filename_, args_->output_name_, no_bits) ==
             EXIT_SUCCESS;
    }

    return DumpOat(nullptr, oat_dumper_options_.get(), args_->os_) == EXIT_SUCCESS;
  }

  bool ExecuteWithRuntime(Runtime* runtime) override {
    CHECK(args_ != nullptr);
    OatDumpMode mode = args_->GetMode();
    CHECK(mode == OatDumpMode::kDumpImt || mode == OatDumpMode::kDumpImage ||
          mode == OatDumpMode::kDumpOat);

    if (mode == OatDumpMode::kDumpImt) {
      return IMTDumper::Dump(runtime,
                             args_->imt_dump_,
                             args_->imt_stat_dump_,
                             args_->oat_filename_,
                             args_->dex_filename_);
    }

    if (mode == OatDumpMode::kDumpOat) {
      return DumpOat(runtime, oat_dumper_options_.get(), args_->os_) == EXIT_SUCCESS;
    }

    return DumpImages(runtime, oat_dumper_options_.get(), args_->os_) == EXIT_SUCCESS;
  }

  bool CanDumpWithRuntime(std::string* error_msg) {
    std::unique_ptr<OatFileAssistantContext> ofa_context =
        args_->GetOatFileAssistantContext(error_msg);
    if (ofa_context == nullptr) {
      return false;
    }

    std::unique_ptr<OatFile> oat_file =
        OpenOat(*oat_dumper_options_->oat_filename_, oat_dumper_options_->dex_filename_, error_msg);
    if (oat_file == nullptr) {
      *error_msg = ART_FORMAT(
          "Failed to open oat file from '{}': {}", *oat_dumper_options_->oat_filename_, *error_msg);
      return false;
    }

    const std::vector<const OatDexFile*>& dex_files = oat_file->GetOatDexFiles();
    if (dex_files.empty()) {
      // Dump header only. Don't need a runtime.
      *error_msg = "No dex code";
      return false;
    }

    OatFileAssistant oat_file_assistant(dex_files[0]->GetLocation().c_str(),
                                        args_->instruction_set_,
                                        /*context=*/nullptr,
                                        /*load_executable=*/false,
                                        /*only_load_trusted_executable=*/false,
                                        ofa_context.get());

    if (!oat_file_assistant.ValidateBootClassPathChecksums(*oat_file, error_msg)) {
      return false;
    }

    return true;
  }

  std::unique_ptr<OatDumperOptions> oat_dumper_options_;
};

}  // namespace art

int main(int argc, char** argv) {
  // Output all logging to stderr.
  android::base::SetLogger(android::base::StderrLogger);

  art::OatdumpMain main;
  return main.Main(argc, argv);
}

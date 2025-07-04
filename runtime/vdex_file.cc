/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "vdex_file.h"

#include <sys/mman.h>  // For the PROT_* and MAP_* constants.
#include <sys/stat.h>  // for mkdir()
#include <sys/types.h>

#include <memory>
#include <unordered_set>

#include "android-base/logging.h"
#include "android-base/stringprintf.h"
#include "base/bit_utils.h"
#include "base/leb128.h"
#include "base/macros.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/unix_file/fd_file.h"
#include "base/zip_archive.h"
#include "class_linker.h"
#include "class_loader_context.h"
#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/dex_file_loader.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "handle_scope-inl.h"
#include "log/log.h"
#include "mirror/class-inl.h"
#include "runtime.h"
#include "verifier/verifier_deps.h"

namespace art HIDDEN {

using android::base::StringPrintf;

bool VdexFile::VdexFileHeader::IsMagicValid() const {
  return (memcmp(magic_, kVdexMagic, sizeof(kVdexMagic)) == 0);
}

bool VdexFile::VdexFileHeader::IsVdexVersionValid() const {
  return (memcmp(vdex_version_, kVdexVersion, sizeof(kVdexVersion)) == 0);
}

VdexFile::VdexFileHeader::VdexFileHeader([[maybe_unused]] bool has_dex_section)
    : number_of_sections_(static_cast<uint32_t>(VdexSection::kNumberOfSections)) {
  memcpy(magic_, kVdexMagic, sizeof(kVdexMagic));
  memcpy(vdex_version_, kVdexVersion, sizeof(kVdexVersion));
  DCHECK(IsMagicValid());
  DCHECK(IsVdexVersionValid());
}

std::unique_ptr<VdexFile> VdexFile::OpenAtAddress(uint8_t* mmap_addr,
                                                  size_t mmap_size,
                                                  bool mmap_reuse,
                                                  const std::string& vdex_filename,
                                                  bool low_4gb,
                                                  std::string* error_msg) {
  ScopedTrace trace(("VdexFile::OpenAtAddress " + vdex_filename).c_str());
  if (!OS::FileExists(vdex_filename.c_str())) {
    *error_msg = "File " + vdex_filename + " does not exist.";
    return nullptr;
  }

  std::unique_ptr<File> vdex_file(OS::OpenFileForReading(vdex_filename.c_str()));
  if (vdex_file == nullptr) {
    *error_msg = "Could not open file for reading";
    return nullptr;
  }

  int64_t vdex_length = vdex_file->GetLength();
  if (vdex_length == -1) {
    *error_msg = "Could not read the length of file " + vdex_filename;
    return nullptr;
  }

  return OpenAtAddress(mmap_addr,
                       mmap_size,
                       mmap_reuse,
                       vdex_file->Fd(),
                       /*start=*/0,
                       vdex_length,
                       vdex_filename,
                       low_4gb,
                       error_msg);
}

std::unique_ptr<VdexFile> VdexFile::OpenAtAddress(uint8_t* mmap_addr,
                                                  size_t mmap_size,
                                                  bool mmap_reuse,
                                                  int file_fd,
                                                  off_t start,
                                                  size_t vdex_length,
                                                  const std::string& vdex_filename,
                                                  bool low_4gb,
                                                  std::string* error_msg) {
  if (mmap_addr != nullptr && mmap_size < vdex_length) {
    *error_msg = StringPrintf("Insufficient pre-allocated space to mmap vdex: %zu and %zu",
                              mmap_size,
                              vdex_length);
    return nullptr;
  }
  CHECK_IMPLIES(mmap_reuse, mmap_addr != nullptr);
  // Start as PROT_WRITE so we can mprotect back to it if we want to.
  MemMap mmap = MemMap::MapFileAtAddress(mmap_addr,
                                         vdex_length,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE,
                                         file_fd,
                                         start,
                                         low_4gb,
                                         vdex_filename.c_str(),
                                         mmap_reuse,
                                         /*reservation=*/nullptr,
                                         error_msg);
  if (!mmap.IsValid()) {
    *error_msg = "Failed to mmap file " + vdex_filename + " : " + *error_msg;
    return nullptr;
  }

  std::unique_ptr<VdexFile> vdex(new VdexFile(std::move(mmap)));
  if (!vdex->IsValid()) {
    *error_msg = "Vdex file is not valid";
    return nullptr;
  }

  return vdex;
}

std::unique_ptr<VdexFile> VdexFile::OpenFromDm(const std::string& filename,
                                               const ZipArchive& archive,
                                               std::string* error_msg) {
  std::unique_ptr<ZipEntry> zip_entry(archive.Find(VdexFile::kVdexNameInDmFile, error_msg));
  if (zip_entry == nullptr) {
    *error_msg = ART_FORMAT("No {} file in DexMetadata archive. Not doing fast verification: {}",
                            VdexFile::kVdexNameInDmFile,
                            *error_msg);
    return nullptr;
  }
  MemMap input_file = zip_entry->MapDirectlyOrExtract(
      filename.c_str(), VdexFile::kVdexNameInDmFile, error_msg, alignof(VdexFile));
  if (!input_file.IsValid()) {
    *error_msg = "Could not open vdex file in DexMetadata archive: " + *error_msg;
    return nullptr;
  }
  std::unique_ptr<VdexFile> vdex_file = std::make_unique<VdexFile>(std::move(input_file));
  if (!vdex_file->IsValid()) {
    *error_msg = "The dex metadata .vdex is not valid. Ignoring it.";
    return nullptr;
  }
  if (vdex_file->HasDexSection()) {
    *error_msg = "The dex metadata is not allowed to contain dex files";
    android_errorWriteLog(0x534e4554, "178055795");  // Report to SafetyNet.
    return nullptr;
  }
  return vdex_file;
}

std::unique_ptr<VdexFile> VdexFile::OpenFromDm(const std::string& filename,
                                               uint8_t* vdex_begin_,
                                               uint8_t* vdex_end_,
                                               std::string* error_msg) {
  std::string vdex_filename = filename + OatFile::kZipSeparator + kVdexNameInDmFile;
  // This overload of `OpenFromDm` is for loading both odex and vdex. We need to map the vdex at the
  // address required by the odex, so the vdex must be uncompressed and page-aligned.
  // To load vdex only, use the other overload.
  FileWithRange vdex_file_with_range = OS::OpenFileDirectlyOrFromZip(
      vdex_filename, OatFile::kZipSeparator, /*alignment=*/MemMap::GetPageSize(), error_msg);
  if (vdex_file_with_range.file == nullptr) {
    return nullptr;
  }
  std::unique_ptr<VdexFile> vdex_file =
      VdexFile::OpenAtAddress(vdex_begin_,
                              vdex_end_ - vdex_begin_,
                              /*mmap_reuse=*/vdex_begin_ != nullptr,
                              vdex_file_with_range.file->Fd(),
                              vdex_file_with_range.start,
                              vdex_file_with_range.length,
                              vdex_filename,
                              /*low_4gb=*/false,
                              error_msg);
  if (vdex_file == nullptr) {
    return nullptr;
  }
  if (vdex_file->HasDexSection()) {
    *error_msg = "The dex metadata is not allowed to contain dex files";
    return nullptr;
  }
  return vdex_file;
}

bool VdexFile::IsValid() const {
  if (mmap_.Size() < sizeof(VdexFileHeader) || !GetVdexFileHeader().IsValid()) {
    return false;
  }

  // Invalidate vdex files that contain dex files in the no longer supported
  // compact dex format. Revert this whenever the vdex version is bumped.
  size_t i = 0;
  for (const uint8_t* dex_file_start = GetNextDexFileData(nullptr, i); dex_file_start != nullptr;
       dex_file_start = GetNextDexFileData(dex_file_start, ++i)) {
    if (!DexFileLoader::IsMagicValid(dex_file_start)) {
      return false;
    }
  }
  return true;
}

const uint8_t* VdexFile::GetNextDexFileData(const uint8_t* cursor, uint32_t dex_file_index) const {
  DCHECK(cursor == nullptr || (cursor > Begin() && cursor <= End()));
  if (cursor == nullptr) {
    // Beginning of the iteration, return the first dex file if there is one.
    return HasDexSection() ? DexBegin() : nullptr;
  } else if (dex_file_index >= GetNumberOfDexFiles()) {
    return nullptr;
  } else {
    // Fetch the next dex file. Return null if there is none.
    const uint8_t* data = cursor + reinterpret_cast<const DexFile::Header*>(cursor)->file_size_;
    // Dex files are required to be 4 byte aligned. the OatWriter makes sure they are, see
    // OatWriter::SeekToDexFiles.
    return AlignUp(data, 4);
  }
}

const uint8_t* VdexFile::GetNextTypeLookupTableData(const uint8_t* cursor,
                                                    uint32_t dex_file_index) const {
  if (cursor == nullptr) {
    // Beginning of the iteration, return the first dex file if there is one.
    return HasTypeLookupTableSection() ? TypeLookupTableDataBegin() : nullptr;
  } else if (dex_file_index >= GetNumberOfDexFiles()) {
    return nullptr;
  } else {
    const uint8_t* data = cursor + sizeof(uint32_t) + reinterpret_cast<const uint32_t*>(cursor)[0];
    // TypeLookupTables are required to be 4 byte aligned. the OatWriter makes sure they are.
    // We don't check this here to be defensive against corrupted vdex files.
    // Callers should check the returned value matches their expectations.
    return data;
  }
}

bool VdexFile::OpenAllDexFiles(std::vector<std::unique_ptr<const DexFile>>* dex_files,
                               std::string* error_msg) const {
  size_t i = 0;
  auto dex_file_container = std::make_shared<MemoryDexFileContainer>(Begin(), End());
  for (const uint8_t* dex_file_start = GetNextDexFileData(nullptr, i);
       dex_file_start != nullptr;
       dex_file_start = GetNextDexFileData(dex_file_start, ++i)) {
    // TODO: Supply the location information for a vdex file.
    static constexpr char kVdexLocation[] = "";
    std::string location = DexFileLoader::GetMultiDexLocation(i, kVdexLocation);
    ArtDexFileLoader dex_file_loader(dex_file_container, location);
    std::unique_ptr<const DexFile> dex(dex_file_loader.OpenOne(dex_file_start - Begin(),
                                                               GetLocationChecksum(i),
                                                               /*oat_dex_file=*/nullptr,
                                                               /*verify=*/false,
                                                               /*verify_checksum=*/false,
                                                               error_msg));
    if (dex == nullptr) {
      return false;
    }
    dex_files->push_back(std::move(dex));
  }
  return true;
}

static bool CreateDirectories(const std::string& child_path, /* out */ std::string* error_msg) {
  size_t last_slash_pos = child_path.find_last_of('/');
  CHECK_NE(last_slash_pos, std::string::npos) << "Invalid path: " << child_path;
  std::string parent_path = child_path.substr(0, last_slash_pos);
  if (OS::DirectoryExists(parent_path.c_str())) {
    return true;
  } else if (CreateDirectories(parent_path, error_msg)) {
    if (mkdir(parent_path.c_str(), 0700) == 0) {
      return true;
    }
    *error_msg = "Could not create directory " + parent_path;
    return false;
  } else {
    return false;
  }
}

bool VdexFile::WriteToDisk(const std::string& path,
                           const std::vector<const DexFile*>& dex_files,
                           const verifier::VerifierDeps& verifier_deps,
                           std::string* error_msg) {
  std::vector<uint8_t> verifier_deps_data;
  verifier_deps.Encode(dex_files, &verifier_deps_data);
  uint32_t verifier_deps_size = verifier_deps_data.size();
  // Add padding so the type lookup tables are 4 byte aligned.
  uint32_t verifier_deps_with_padding_size = RoundUp(verifier_deps_data.size(), 4);
  DCHECK_GE(verifier_deps_with_padding_size, verifier_deps_data.size());
  verifier_deps_data.resize(verifier_deps_with_padding_size, 0);

  size_t type_lookup_table_size = 0u;
  for (const DexFile* dex_file : dex_files) {
    type_lookup_table_size +=
        sizeof(uint32_t) + TypeLookupTable::RawDataLength(dex_file->NumClassDefs());
  }

  VdexFile::VdexFileHeader vdex_header(/* has_dex_section= */ false);
  VdexFile::VdexSectionHeader sections[static_cast<uint32_t>(VdexSection::kNumberOfSections)];

  // Set checksum section.
  sections[VdexSection::kChecksumSection].section_kind = VdexSection::kChecksumSection;
  sections[VdexSection::kChecksumSection].section_offset = GetChecksumsOffset();
  sections[VdexSection::kChecksumSection].section_size =
      sizeof(VdexFile::VdexChecksum) * dex_files.size();

  // Set dex section.
  sections[VdexSection::kDexFileSection].section_kind = VdexSection::kDexFileSection;
  sections[VdexSection::kDexFileSection].section_offset = 0u;
  sections[VdexSection::kDexFileSection].section_size = 0u;

  // Set VerifierDeps section.
  sections[VdexSection::kVerifierDepsSection].section_kind = VdexSection::kVerifierDepsSection;
  sections[VdexSection::kVerifierDepsSection].section_offset =
      GetChecksumsOffset() + sections[kChecksumSection].section_size;
  sections[VdexSection::kVerifierDepsSection].section_size = verifier_deps_size;

  // Set TypeLookupTable section.
  sections[VdexSection::kTypeLookupTableSection].section_kind =
      VdexSection::kTypeLookupTableSection;
  sections[VdexSection::kTypeLookupTableSection].section_offset =
      sections[VdexSection::kVerifierDepsSection].section_offset + verifier_deps_with_padding_size;
  sections[VdexSection::kTypeLookupTableSection].section_size = type_lookup_table_size;

  if (!CreateDirectories(path, error_msg)) {
    return false;
  }

  std::unique_ptr<File> out(OS::CreateEmptyFileWriteOnly(path.c_str()));
  if (out == nullptr) {
    *error_msg = "Could not open " + path + " for writing";
    return false;
  }

  // Write header.
  if (!out->WriteFully(reinterpret_cast<const char*>(&vdex_header), sizeof(vdex_header))) {
    *error_msg = "Could not write vdex header to " + path;
    out->Unlink();
    return false;
  }

  // Write section infos.
  if (!out->WriteFully(reinterpret_cast<const char*>(&sections), sizeof(sections))) {
    *error_msg = "Could not write vdex sections to " + path;
    out->Unlink();
    return false;
  }

  // Write checksum section.
  for (const DexFile* dex_file : dex_files) {
    uint32_t checksum = dex_file->GetLocationChecksum();
    const uint32_t* checksum_ptr = &checksum;
    static_assert(sizeof(*checksum_ptr) == sizeof(VdexFile::VdexChecksum));
    if (!out->WriteFully(reinterpret_cast<const char*>(checksum_ptr),
                         sizeof(VdexFile::VdexChecksum))) {
      *error_msg = "Could not write dex checksums to " + path;
      out->Unlink();
      return false;
    }
  }

  if (!out->WriteFully(reinterpret_cast<const char*>(verifier_deps_data.data()),
                       verifier_deps_with_padding_size)) {
    *error_msg = "Could not write verifier deps to " + path;
    out->Unlink();
    return false;
  }

  size_t written_type_lookup_table_size = 0;
  for (const DexFile* dex_file : dex_files) {
    TypeLookupTable type_lookup_table = TypeLookupTable::Create(*dex_file);
    uint32_t size = type_lookup_table.RawDataLength();
    DCHECK_ALIGNED(size, 4);
    if (!out->WriteFully(reinterpret_cast<const char*>(&size), sizeof(uint32_t)) ||
        !out->WriteFully(reinterpret_cast<const char*>(type_lookup_table.RawData()), size)) {
      *error_msg = "Could not write type lookup table " + path;
      out->Unlink();
      return false;
    }
    written_type_lookup_table_size += sizeof(uint32_t) + size;
  }
  DCHECK_EQ(written_type_lookup_table_size, type_lookup_table_size);

  if (out->FlushClose() != 0) {
    *error_msg = "Could not flush and close " + path;
    out->Unlink();
    return false;
  }

  return true;
}

bool VdexFile::MatchesDexFileChecksums(const std::vector<const DexFile::Header*>& dex_headers)
    const {
  if (dex_headers.size() != GetNumberOfDexFiles()) {
    LOG(WARNING) << "Mismatch of number of dex files in vdex (expected="
        << GetNumberOfDexFiles() << ", actual=" << dex_headers.size() << ")";
    return false;
  }
  const VdexChecksum* checksums = GetDexChecksumsArray();
  for (size_t i = 0; i < dex_headers.size(); ++i) {
    if (checksums[i] != dex_headers[i]->checksum_) {
      LOG(WARNING) << "Mismatch of dex file checksum in vdex (index=" << i << ")";
      return false;
    }
  }
  return true;
}

static ObjPtr<mirror::Class> FindClassAndClearException(ClassLinker* class_linker,
                                                        Thread* self,
                                                        const char* descriptor,
                                                        size_t descriptor_length,
                                                        Handle<mirror::ClassLoader> class_loader)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Class> result =
      class_linker->FindClass(self, descriptor, descriptor_length, class_loader);
  if (result == nullptr) {
    DCHECK(self->IsExceptionPending());
    self->ClearException();
  }
  return result;
}

static const char* GetStringFromIndex(const DexFile& dex_file,
                                      dex::StringIndex string_id,
                                      uint32_t number_of_extra_strings,
                                      const uint32_t* extra_strings_offsets,
                                      const uint8_t* verifier_deps,
                                      /*out*/ size_t* utf8_length) {
  uint32_t num_ids_in_dex = dex_file.NumStringIds();
  if (string_id.index_ < num_ids_in_dex) {
    uint32_t utf16_length;
    const char* str = dex_file.GetStringDataAndUtf16Length(string_id, &utf16_length);
    *utf8_length = DexFile::Utf8Length(str, utf16_length);
    return str;
  } else {
    CHECK_LT(string_id.index_ - num_ids_in_dex, number_of_extra_strings);
    uint32_t offset = extra_strings_offsets[string_id.index_ - num_ids_in_dex];
    const char* str = reinterpret_cast<const char*>(verifier_deps) + offset;
    *utf8_length = strlen(str);
    return str;
  }
}

// Returns an array of offsets where the assignability checks for each class
// definition are stored.
static const uint32_t* GetDexFileClassDefs(const uint8_t* verifier_deps, uint32_t index) {
  uint32_t dex_file_offset = reinterpret_cast<const uint32_t*>(verifier_deps)[index];
  return reinterpret_cast<const uint32_t*>(verifier_deps + dex_file_offset);
}

// Returns an array of offsets where extra strings are stored.
static const uint32_t* GetExtraStringsOffsets(const DexFile& dex_file,
                                              const uint8_t* verifier_deps,
                                              const uint32_t* dex_file_class_defs,
                                              /*out*/ uint32_t* number_of_extra_strings) {
  // The information for strings is right after dex_file_class_defs, 4-byte
  // aligned
  uint32_t end_of_assignability_types = dex_file_class_defs[dex_file.NumClassDefs()];
  const uint8_t* strings_data_start =
      AlignUp(verifier_deps + end_of_assignability_types, sizeof(uint32_t));
  // First entry is the number of extra strings for this dex file.
  *number_of_extra_strings = *reinterpret_cast<const uint32_t*>(strings_data_start);
  // Then an array of offsets in `verifier_deps` for the extra strings.
  return reinterpret_cast<const uint32_t*>(strings_data_start + sizeof(uint32_t));
}

ClassStatus VdexFile::ComputeClassStatus(Thread* self, Handle<mirror::Class> cls) const {
  const DexFile& dex_file = cls->GetDexFile();
  uint16_t class_def_index = cls->GetDexClassDefIndex();

  // Find which dex file index from within the vdex file.
  uint32_t index = 0;
  for (; index < GetNumberOfDexFiles(); ++index) {
    if (dex_file.GetLocationChecksum() == GetLocationChecksum(index)) {
      break;
    }
  }

  DCHECK_NE(index, GetNumberOfDexFiles());

  const uint8_t* verifier_deps = GetVerifierDepsData().data();
  const uint32_t* dex_file_class_defs = GetDexFileClassDefs(verifier_deps, index);

  // Fetch type checks offsets.
  uint32_t class_def_offset = dex_file_class_defs[class_def_index];
  if (class_def_offset == verifier::VerifierDeps::kNotVerifiedMarker) {
    // Return a status that needs re-verification.
    return ClassStatus::kResolved;
  }
  // End offset for this class's type checks. We know there is one and the loop
  // will terminate.
  uint32_t end_offset = verifier::VerifierDeps::kNotVerifiedMarker;
  for (uint32_t i = class_def_index + 1; i < dex_file.NumClassDefs() + 1; ++i) {
    end_offset = dex_file_class_defs[i];
    if (end_offset != verifier::VerifierDeps::kNotVerifiedMarker) {
      break;
    }
  }
  DCHECK_NE(end_offset, verifier::VerifierDeps::kNotVerifiedMarker);

  uint32_t number_of_extra_strings = 0;
  // Offset where extra strings are stored.
  const uint32_t* extra_strings_offsets = GetExtraStringsOffsets(dex_file,
                                                                 verifier_deps,
                                                                 dex_file_class_defs,
                                                                 &number_of_extra_strings);

  // Loop over and perform each assignability check.
  StackHandleScope<3> hs(self);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(cls->GetClassLoader()));
  MutableHandle<mirror::Class> source(hs.NewHandle<mirror::Class>(nullptr));
  MutableHandle<mirror::Class> destination(hs.NewHandle<mirror::Class>(nullptr));

  const uint8_t* cursor = verifier_deps + class_def_offset;
  const uint8_t* end = verifier_deps + end_offset;
  while (cursor < end) {
    uint32_t destination_index;
    uint32_t source_index;
    if (UNLIKELY(!DecodeUnsignedLeb128Checked(&cursor, end, &destination_index) ||
                 !DecodeUnsignedLeb128Checked(&cursor, end, &source_index))) {
      // Error parsing the data, just return that we are not verified.
      return ClassStatus::kResolved;
    }
    size_t destination_desc_length;
    const char* destination_desc = GetStringFromIndex(dex_file,
                                                      dex::StringIndex(destination_index),
                                                      number_of_extra_strings,
                                                      extra_strings_offsets,
                                                      verifier_deps,
                                                      &destination_desc_length);
    destination.Assign(FindClassAndClearException(
        class_linker, self, destination_desc, destination_desc_length, class_loader));

    size_t source_desc_length;
    const char* source_desc = GetStringFromIndex(dex_file,
                                                 dex::StringIndex(source_index),
                                                 number_of_extra_strings,
                                                 extra_strings_offsets,
                                                 verifier_deps,
                                                 &source_desc_length);
    source.Assign(FindClassAndClearException(
        class_linker, self, source_desc, source_desc_length, class_loader));

    if (destination == nullptr || source == nullptr) {
      cls->SetHasTypeChecksFailure();
      // The interpreter / compiler can handle a missing class.
      continue;
    }

    DCHECK(destination->IsResolved() && source->IsResolved());
    if (!destination->IsAssignableFrom(source.Get())) {
      VLOG(verifier) << "Vdex checking failed for " << cls->PrettyClass()
                     << ": expected " << destination->PrettyClass()
                     << " to be assignable from " << source->PrettyClass();
      // An implicit assignability check is failing in the code, return that the
      // class is not verified.
      return ClassStatus::kResolved;
    }
  }

  return ClassStatus::kVerifiedNeedsAccessChecks;
}

}  // namespace art

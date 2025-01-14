// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_binary_impl.h"

#include <lib/syslog/cpp/macros.h>

#include <limits>

#include "llvm/DebugInfo/DIContext.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugAranges.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "src/developer/debug/zxdb/common/file_util.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit_impl.h"
#include "src/lib/elflib/elflib.h"

namespace zxdb {

namespace {

uint64_t ComputeMappedLength(elflib::ElfLib& elf) {
  uint64_t max = 0;
  for (const elflib::Elf64_Phdr& header : elf.GetSegmentHeaders()) {
    // Only check segments that are loaded. Some segments contain things like DWARF symbols that
    // won't be loaded. Here we only want the size in-memory to resolve addresses in the program's
    // address space.
    if (header.p_type == elflib::PT_LOAD)
      max = std::max(max, header.p_vaddr + header.p_memsz);
  }
  return max;
}

// Merges the "symbols" and "dynamic symbols" into a single map. Returns an empty map if the symbols
// couldn't be loaded.
//
// The ".dynsym" table is normally described as containing a subset of the information (just the
// global symbols) in the ".symtab" section. But in a stripped binary, there will be only a
// ".dynsym" section. To handle all the cases, this merges both tables. If a name is the same, this
// assumes the symbols are the same. The non-dynamic one will be used in the case of duplicates.
std::map<std::string, elflib::Elf64_Sym> GetMergedElfSymbols(elflib::ElfLib& elf) {
  std::map<std::string, elflib::Elf64_Sym> result;

  if (auto dyn = elf.GetAllDynamicSymbols())
    result = std::move(*dyn);

  // Merge in the ".symtab" section, overwriting any definitions that are duplicates.
  if (auto sym = elf.GetAllSymbols()) {
    for (const auto& pair : *sym) {
      result.insert(pair);
    }
  }

  return result;
}

}  // namespace

DwarfBinaryImpl::DwarfBinaryImpl(const std::string& name, const std::string& binary_name,
                                 const std::string& build_id)
    : name_(name), binary_name_(binary_name), build_id_(build_id), weak_factory_(this) {}

DwarfBinaryImpl::~DwarfBinaryImpl() {}

fxl::WeakPtr<DwarfBinaryImpl> DwarfBinaryImpl::GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

std::string DwarfBinaryImpl::GetName() const { return name_; }

std::string DwarfBinaryImpl::GetBuildID() const { return build_id_; }

std::time_t DwarfBinaryImpl::GetModificationTime() const { return modification_time_; }

bool DwarfBinaryImpl::HasBinary() const {
  if (!binary_name_.empty())
    return true;

  if (auto debug = elflib::ElfLib::Create(name_))
    return debug->ProbeHasProgramBits();

  return false;
}

Err DwarfBinaryImpl::Load() {
  if (auto debug = elflib::ElfLib::Create(name_)) {
    if (debug->ProbeHasProgramBits()) {
      // Found in ".debug" file.
      plt_symbols_ = debug->GetPLTOffsets();
      elf_symbols_ = GetMergedElfSymbols(*debug);
      mapped_length_ = ComputeMappedLength(*debug);
    } else if (auto elf = elflib::ElfLib::Create(binary_name_)) {
      // Found in binary file.
      plt_symbols_ = elf->GetPLTOffsets();
      elf_symbols_ = GetMergedElfSymbols(*elf);
      mapped_length_ = ComputeMappedLength(*elf);
    }
  }

  llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> bin_or_err =
      llvm::object::createBinary(name_);
  if (!bin_or_err) {
    auto err_str = llvm::toString(bin_or_err.takeError());
    return Err("Error loading symbols for \"" + name_ + "\": " + err_str);
  }

  modification_time_ = GetFileModificationTime(name_);

  auto binary_pair = bin_or_err->takeBinary();
  binary_buffer_ = std::move(binary_pair.second);
  binary_ = std::move(binary_pair.first);

  context_ = llvm::DWARFContext::create(*object_file());
  context_->getDWARFObj().forEachInfoSections([this](const llvm::DWARFSection& s) {
    compile_units_.addUnitsForSection(*context_, s, llvm::DW_SECT_INFO);
  });

  return Err();
}

llvm::object::ObjectFile* DwarfBinaryImpl::GetLLVMObjectFile() { return object_file(); }

llvm::DWARFContext* DwarfBinaryImpl::GetLLVMContext() { return context(); }

uint64_t DwarfBinaryImpl::GetMappedLength() const { return mapped_length_; }

const std::map<std::string, llvm::ELF::Elf64_Sym>& DwarfBinaryImpl::GetELFSymbols() const {
  return elf_symbols_;
}

const std::map<std::string, uint64_t> DwarfBinaryImpl::GetPLTSymbols() const {
  return plt_symbols_;
}

size_t DwarfBinaryImpl::GetUnitCount() const {
  auto unit_range = context_->normal_units();
  return unit_range.end() - unit_range.begin();
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::GetUnitAtIndex(size_t i) {
  FX_DCHECK(i < GetUnitCount());
  return FromLLVMUnit(context_->getUnitAtIndex(i));
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::UnitForRelativeAddress(uint64_t relative_address) {
  return FromLLVMUnit(
      compile_units_.getUnitForOffset(context_->getDebugAranges()->findAddress(relative_address)));
}

fxl::RefPtr<DwarfUnit> DwarfBinaryImpl::FromLLVMUnit(llvm::DWARFUnit* llvm_unit) {
  if (!llvm_unit)
    return fxl::RefPtr<DwarfUnit>();

  auto found = unit_map_.find(llvm_unit);
  if (found == unit_map_.end()) {
    auto unit = fxl::MakeRefCounted<DwarfUnitImpl>(this, llvm_unit);
    unit_map_[llvm_unit] = unit;
    return unit;
  }
  return found->second;
}

std::optional<uint64_t> DwarfBinaryImpl::GetDebugAddrEntry(uint64_t addr_base,
                                                           uint64_t index) const {
  const llvm::DWARFObject& object = context_->getDWARFObj();
  llvm::StringRef string_ref = object.getAddrSection().Data;

  // From the DWARF 5 spec: "The DW_AT_addr_base attribute points to the first entry following the
  // header. The entries are indexed sequentially from this base entry, starting from 0. So the
  // addr_base is a byte offset, but the index is an index into the address table from there.
  //
  // Here we assume the addresses are always 64 bits. The address table header that precedes the
  // array has this size as a field which we need to consult if we support non-64 bit platforms.
  uint64_t offset = addr_base + (index * kTargetPointerSize);

  if (offset > std::numeric_limits<uint64_t>::max() - kTargetPointerSize ||
      string_ref.size() < offset + kTargetPointerSize)
    return std::nullopt;  // No room in table data.

  uint64_t result;
  memcpy(&result, string_ref.bytes_begin() + offset, kTargetPointerSize);
  return result;
}

}  // namespace zxdb

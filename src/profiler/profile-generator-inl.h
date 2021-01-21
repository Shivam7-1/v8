// Copyright 2010 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_PROFILER_PROFILE_GENERATOR_INL_H_
#define V8_PROFILER_PROFILE_GENERATOR_INL_H_

#include <memory>

#include "src/base/lazy-instance.h"
#include "src/profiler/profile-generator.h"

namespace v8 {
namespace internal {

CodeEntry::CodeEntry(CodeEventListener::LogEventsAndTags tag, const char* name,
                     const char* resource_name, int line_number,
                     int column_number,
                     std::unique_ptr<SourcePositionTable> line_info,
                     bool is_shared_cross_origin, CodeType code_type)
    : bit_field_(TagField::encode(tag) |
                 BuiltinIdField::encode(Builtins::builtin_count) |
                 CodeTypeField::encode(code_type) |
                 SharedCrossOriginField::encode(is_shared_cross_origin)),
      name_(name),
      resource_name_(resource_name),
      line_number_(line_number),
      column_number_(column_number),
      script_id_(v8::UnboundScript::kNoScriptId),
      position_(0),
      line_info_(std::move(line_info)) {}

ProfileNode::ProfileNode(ProfileTree* tree, CodeEntry* entry,
                         ProfileNode* parent, int line_number)
    : tree_(tree),
      entry_(entry),
      self_ticks_(0),
      line_number_(line_number),
      parent_(parent),
      id_(tree->next_node_id()) {
  tree_->EnqueueNode(this);
}

// static
inline CodeEntry* CodeEntry::program_entry() {
  static base::LeakyObject<CodeEntry> kProgramEntry(
      CodeEventListener::FUNCTION_TAG, CodeEntry::kProgramEntryName,
      CodeEntry::kEmptyResourceName, v8::CpuProfileNode::kNoLineNumberInfo,
      v8::CpuProfileNode::kNoColumnNumberInfo, nullptr, false,
      CodeEntry::CodeType::OTHER);
  return kProgramEntry.get();
}

// static
inline CodeEntry* CodeEntry::idle_entry() {
  static base::LeakyObject<CodeEntry> kIdleEntry(
      CodeEventListener::FUNCTION_TAG, CodeEntry::kIdleEntryName,
      CodeEntry::kEmptyResourceName, v8::CpuProfileNode::kNoLineNumberInfo,
      v8::CpuProfileNode::kNoColumnNumberInfo, nullptr, false,
      CodeEntry::CodeType::OTHER);
  return kIdleEntry.get();
}

// static
inline CodeEntry* CodeEntry::gc_entry() {
  static base::LeakyObject<CodeEntry> kGcEntry(
      CodeEventListener::BUILTIN_TAG, CodeEntry::kGarbageCollectorEntryName,
      CodeEntry::kEmptyResourceName, v8::CpuProfileNode::kNoLineNumberInfo,
      v8::CpuProfileNode::kNoColumnNumberInfo, nullptr, false,
      CodeEntry::CodeType::OTHER);
  return kGcEntry.get();
}

// static
inline CodeEntry* CodeEntry::unresolved_entry() {
  static base::LeakyObject<CodeEntry> kUnresolvedEntry(
      CodeEventListener::FUNCTION_TAG, CodeEntry::kUnresolvedFunctionName,
      CodeEntry::kEmptyResourceName, v8::CpuProfileNode::kNoLineNumberInfo,
      v8::CpuProfileNode::kNoColumnNumberInfo, nullptr, false,
      CodeEntry::CodeType::OTHER);
  return kUnresolvedEntry.get();
}

// static
inline CodeEntry* CodeEntry::root_entry() {
  static base::LeakyObject<CodeEntry> kRootEntry(
      CodeEventListener::FUNCTION_TAG, CodeEntry::kRootEntryName,
      CodeEntry::kEmptyResourceName, v8::CpuProfileNode::kNoLineNumberInfo,
      v8::CpuProfileNode::kNoColumnNumberInfo, nullptr, false,
      CodeEntry::CodeType::OTHER);
  return kRootEntry.get();
}

inline CpuProfileNode::SourceType ProfileNode::source_type() const {
  // Handle metadata and VM state code entry types.
  if (entry_ == CodeEntry::program_entry() ||
      entry_ == CodeEntry::idle_entry() || entry_ == CodeEntry::gc_entry() ||
      entry_ == CodeEntry::root_entry()) {
    return CpuProfileNode::kInternal;
  }
  if (entry_ == CodeEntry::unresolved_entry())
    return CpuProfileNode::kUnresolved;

  // Otherwise, resolve based on logger tag.
  switch (entry_->tag()) {
    case CodeEventListener::EVAL_TAG:
    case CodeEventListener::SCRIPT_TAG:
    case CodeEventListener::LAZY_COMPILE_TAG:
    case CodeEventListener::FUNCTION_TAG:
    case CodeEventListener::INTERPRETED_FUNCTION_TAG:
      return CpuProfileNode::kScript;
    case CodeEventListener::BUILTIN_TAG:
    case CodeEventListener::HANDLER_TAG:
    case CodeEventListener::BYTECODE_HANDLER_TAG:
    case CodeEventListener::NATIVE_FUNCTION_TAG:
    case CodeEventListener::NATIVE_SCRIPT_TAG:
    case CodeEventListener::NATIVE_LAZY_COMPILE_TAG:
      return CpuProfileNode::kBuiltin;
    case CodeEventListener::CALLBACK_TAG:
      return CpuProfileNode::kCallback;
    case CodeEventListener::REG_EXP_TAG:
    case CodeEventListener::STUB_TAG:
    case CodeEventListener::CODE_CREATION_EVENT:
    case CodeEventListener::CODE_DISABLE_OPT_EVENT:
    case CodeEventListener::CODE_MOVE_EVENT:
    case CodeEventListener::CODE_DELETE_EVENT:
    case CodeEventListener::CODE_MOVING_GC:
    case CodeEventListener::SHARED_FUNC_MOVE_EVENT:
    case CodeEventListener::SNAPSHOT_CODE_NAME_EVENT:
    case CodeEventListener::TICK_EVENT:
    case CodeEventListener::NUMBER_OF_LOG_EVENTS:
      return CpuProfileNode::kInternal;
  }
}

inline Isolate* ProfileNode::isolate() const { return tree_->isolate(); }

}  // namespace internal
}  // namespace v8

#endif  // V8_PROFILER_PROFILE_GENERATOR_INL_H_

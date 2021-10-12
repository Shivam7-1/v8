// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if V8_TARGET_ARCH_LOONG64

#include "src/regexp/loong64/regexp-macro-assembler-loong64.h"

#include "src/codegen/macro-assembler.h"
#include "src/heap/factory.h"
#include "src/logging/log.h"
#include "src/objects/code-inl.h"
#include "src/regexp/regexp-stack.h"
#include "src/snapshot/embedded/embedded-data.h"

namespace v8 {
namespace internal {

/* clang-format off
 *
 * This assembler uses the following register assignment convention
 * - t3 : Temporarily stores the index of capture start after a matching pass
 *        for a global regexp.
 * - a5 : Pointer to current Code object including heap object tag.
 * - a6 : Current position in input, as negative offset from end of string.
 *        Please notice that this is the byte offset, not the character offset!
 * - a7 : Currently loaded character. Must be loaded using
 *        LoadCurrentCharacter before using any of the dispatch methods.
 * - t0 : Points to tip of backtrack stack
 * - t1 : Unused.
 * - t2 : End of input (points to byte after last character in input).
 * - fp : Frame pointer. Used to access arguments, local variables and
 *         RegExp registers.
 * - sp : Points to tip of C stack.
 *
 * The remaining registers are free for computations.
 * Each call to a public method should retain this convention.
 *
 * The stack will have the following structure:
 *
 *  - fp[80]  Isolate* isolate   (address of the current isolate)               kIsolate
 *                                                                              kStackFrameHeader
 *  --- sp when called ---
 *  - fp[72]  ra                 Return from RegExp code (ra).                  kReturnAddress
 *  - fp[64]  old-fp             Old fp, callee saved.
 *  - fp[0..63]  s0..s7          Callee-saved registers s0..s7.
 *  --- frame pointer ----
 *  - fp[-8]  direct_call        (1 = direct call from JS, 0 = from runtime)    kDirectCall
 *  - fp[-16] capture array size (may fit multiple sets of matches)             kNumOutputRegisters
 *  - fp[-24] int* capture_array (int[num_saved_registers_], for output).       kRegisterOutput
 *  - fp[-32] end of input       (address of end of string).                    kInputEnd
 *  - fp[-40] start of input     (address of first character in string).        kInputStart
 *  - fp[-48] start index        (character index of start).                    kStartIndex
 *  - fp[-56] void* input_string (location of a handle containing the string).  kInputString
 *  - fp[-64] success counter    (only for global regexps to count matches).    kSuccessfulCaptures
 *  - fp[-72] Offset of location before start of input (effectively character   kStringStartMinusOne
 *            position -1). Used to initialize capture registers to a
 *            non-position.
 *  --------- The following output registers are 32-bit values. ---------
 *  - fp[-80] register 0         (Only positions must be stored in the first    kRegisterZero
 *  -         register 1          num_saved_registers_ registers)
 *  -         ...
 *  -         register num_registers-1
 *  --- sp ---
 *
 * The first num_saved_registers_ registers are initialized to point to
 * "character -1" in the string (i.e., char_size() bytes before the first
 * character of the string). The remaining registers start out as garbage.
 *
 * The data up to the return address must be placed there by the calling
 * code and the remaining arguments are passed in registers, e.g. by calling the
 * code entry as cast to a function with the signature:
 * int (*match)(String input_string,
 *              int start_index,
 *              Address start,
 *              Address end,
 *              int* capture_output_array,
 *              int num_capture_registers,
 *              bool direct_call = false,
 *              Isolate* isolate);
 * The call is performed by NativeRegExpMacroAssembler::Execute()
 * (in regexp-macro-assembler.cc) via the GeneratedCode wrapper.
 *
 * clang-format on
 */

#define __ ACCESS_MASM(masm_)

const int RegExpMacroAssemblerLOONG64::kRegExpCodeSize;

RegExpMacroAssemblerLOONG64::RegExpMacroAssemblerLOONG64(Isolate* isolate,
                                                         Zone* zone, Mode mode,
                                                         int registers_to_save)
    : NativeRegExpMacroAssembler(isolate, zone),
      masm_(std::make_unique<MacroAssembler>(
          isolate, CodeObjectRequired::kYes,
          NewAssemblerBuffer(kRegExpCodeSize))),
      no_root_array_scope_(masm_.get()),
      mode_(mode),
      num_registers_(registers_to_save),
      num_saved_registers_(registers_to_save),
      entry_label_(),
      start_label_(),
      success_label_(),
      backtrack_label_(),
      exit_label_(),
      internal_failure_label_() {
  DCHECK_EQ(0, registers_to_save % 2);
  __ jmp(&entry_label_);  // We'll write the entry code later.
  // If the code gets too big or corrupted, an internal exception will be
  // raised, and we will exit right away.
  __ bind(&internal_failure_label_);
  __ li(a0, Operand(FAILURE));
  __ Ret();
  __ bind(&start_label_);  // And then continue from here.
}

RegExpMacroAssemblerLOONG64::~RegExpMacroAssemblerLOONG64() {
  // Unuse labels in case we throw away the assembler without calling GetCode.
  entry_label_.Unuse();
  start_label_.Unuse();
  success_label_.Unuse();
  backtrack_label_.Unuse();
  exit_label_.Unuse();
  check_preempt_label_.Unuse();
  stack_overflow_label_.Unuse();
  internal_failure_label_.Unuse();
  fallback_label_.Unuse();
}

int RegExpMacroAssemblerLOONG64::stack_limit_slack() {
  return RegExpStack::kStackLimitSlack;
}

void RegExpMacroAssemblerLOONG64::AdvanceCurrentPosition(int by) {
  if (by != 0) {
    __ Add_d(current_input_offset(), current_input_offset(),
             Operand(by * char_size()));
  }
}

void RegExpMacroAssemblerLOONG64::AdvanceRegister(int reg, int by) {
  DCHECK_LE(0, reg);
  DCHECK_GT(num_registers_, reg);
  if (by != 0) {
    __ Ld_d(a0, register_location(reg));
    __ Add_d(a0, a0, Operand(by));
    __ St_d(a0, register_location(reg));
  }
}

void RegExpMacroAssemblerLOONG64::Backtrack() {
  CheckPreemption();
  if (has_backtrack_limit()) {
    Label next;
    __ Ld_d(a0, MemOperand(frame_pointer(), kBacktrackCount));
    __ Add_d(a0, a0, Operand(1));
    __ St_d(a0, MemOperand(frame_pointer(), kBacktrackCount));
    __ Branch(&next, ne, a0, Operand(backtrack_limit()));

    // Backtrack limit exceeded.
    if (can_fallback()) {
      __ jmp(&fallback_label_);
    } else {
      // Can't fallback, so we treat it as a failed match.
      Fail();
    }

    __ bind(&next);
  }
  // Pop Code offset from backtrack stack, add Code and jump to location.
  Pop(a0);
  __ Add_d(a0, a0, code_pointer());
  __ Jump(a0);
}

void RegExpMacroAssemblerLOONG64::Bind(Label* label) { __ bind(label); }

void RegExpMacroAssemblerLOONG64::CheckCharacter(uint32_t c, Label* on_equal) {
  BranchOrBacktrack(on_equal, eq, current_character(), Operand(c));
}

void RegExpMacroAssemblerLOONG64::CheckCharacterGT(base::uc16 limit,
                                                   Label* on_greater) {
  BranchOrBacktrack(on_greater, gt, current_character(), Operand(limit));
}

void RegExpMacroAssemblerLOONG64::CheckAtStart(int cp_offset,
                                               Label* on_at_start) {
  __ Ld_d(a1, MemOperand(frame_pointer(), kStringStartMinusOne));
  __ Add_d(a0, current_input_offset(),
           Operand(-char_size() + cp_offset * char_size()));
  BranchOrBacktrack(on_at_start, eq, a0, Operand(a1));
}

void RegExpMacroAssemblerLOONG64::CheckNotAtStart(int cp_offset,
                                                  Label* on_not_at_start) {
  __ Ld_d(a1, MemOperand(frame_pointer(), kStringStartMinusOne));
  __ Add_d(a0, current_input_offset(),
           Operand(-char_size() + cp_offset * char_size()));
  BranchOrBacktrack(on_not_at_start, ne, a0, Operand(a1));
}

void RegExpMacroAssemblerLOONG64::CheckCharacterLT(base::uc16 limit,
                                                   Label* on_less) {
  BranchOrBacktrack(on_less, lt, current_character(), Operand(limit));
}

void RegExpMacroAssemblerLOONG64::CheckGreedyLoop(Label* on_equal) {
  Label backtrack_non_equal;
  __ Ld_w(a0, MemOperand(backtrack_stackpointer(), 0));
  __ Branch(&backtrack_non_equal, ne, current_input_offset(), Operand(a0));
  __ Add_d(backtrack_stackpointer(), backtrack_stackpointer(),
           Operand(kIntSize));
  __ bind(&backtrack_non_equal);
  BranchOrBacktrack(on_equal, eq, current_input_offset(), Operand(a0));
}

void RegExpMacroAssemblerLOONG64::CheckNotBackReferenceIgnoreCase(
    int start_reg, bool read_backward, bool unicode, Label* on_no_match) {
  Label fallthrough;
  __ Ld_d(a0, register_location(start_reg));      // Index of start of capture.
  __ Ld_d(a1, register_location(start_reg + 1));  // Index of end of capture.
  __ Sub_d(a1, a1, a0);                           // Length of capture.

  // At this point, the capture registers are either both set or both cleared.
  // If the capture length is zero, then the capture is either empty or cleared.
  // Fall through in both cases.
  __ Branch(&fallthrough, eq, a1, Operand(zero_reg));

  if (read_backward) {
    __ Ld_d(t1, MemOperand(frame_pointer(), kStringStartMinusOne));
    __ Add_d(t1, t1, a1);
    BranchOrBacktrack(on_no_match, le, current_input_offset(), Operand(t1));
  } else {
    __ Add_d(t1, a1, current_input_offset());
    // Check that there are enough characters left in the input.
    BranchOrBacktrack(on_no_match, gt, t1, Operand(zero_reg));
  }

  if (mode_ == LATIN1) {
    Label success;
    Label fail;
    Label loop_check;

    // a0 - offset of start of capture.
    // a1 - length of capture.
    __ Add_d(a0, a0, Operand(end_of_input_address()));
    __ Add_d(a2, end_of_input_address(), Operand(current_input_offset()));
    if (read_backward) {
      __ Sub_d(a2, a2, Operand(a1));
    }
    __ Add_d(a1, a0, Operand(a1));

    // a0 - Address of start of capture.
    // a1 - Address of end of capture.
    // a2 - Address of current input position.

    Label loop;
    __ bind(&loop);
    __ Ld_bu(a3, MemOperand(a0, 0));
    __ addi_d(a0, a0, char_size());
    __ Ld_bu(a4, MemOperand(a2, 0));
    __ addi_d(a2, a2, char_size());

    __ Branch(&loop_check, eq, a4, Operand(a3));

    // Mismatch, try case-insensitive match (converting letters to lower-case).
    __ Or(a3, a3, Operand(0x20));  // Convert capture character to lower-case.
    __ Or(a4, a4, Operand(0x20));  // Also convert input character.
    __ Branch(&fail, ne, a4, Operand(a3));
    __ Sub_d(a3, a3, Operand('a'));
    __ Branch(&loop_check, ls, a3, Operand('z' - 'a'));
    // Latin-1: Check for values in range [224,254] but not 247.
    __ Sub_d(a3, a3, Operand(224 - 'a'));
    // Weren't Latin-1 letters.
    __ Branch(&fail, hi, a3, Operand(254 - 224));
    // Check for 247.
    __ Branch(&fail, eq, a3, Operand(247 - 224));

    __ bind(&loop_check);
    __ Branch(&loop, lt, a0, Operand(a1));
    __ jmp(&success);

    __ bind(&fail);
    GoTo(on_no_match);

    __ bind(&success);
    // Compute new value of character position after the matched part.
    __ Sub_d(current_input_offset(), a2, end_of_input_address());
    if (read_backward) {
      __ Ld_d(t1, register_location(start_reg));  // Index of start of capture.
      __ Ld_d(a2,
              register_location(start_reg + 1));  // Index of end of capture.
      __ Add_d(current_input_offset(), current_input_offset(), Operand(t1));
      __ Sub_d(current_input_offset(), current_input_offset(), Operand(a2));
    }
  } else {
    DCHECK(mode_ == UC16);
    // Put regexp engine registers on stack.
    RegList regexp_registers_to_retain = current_input_offset().bit() |
                                         current_character().bit() |
                                         backtrack_stackpointer().bit();
    __ MultiPush(regexp_registers_to_retain);

    int argument_count = 4;
    __ PrepareCallCFunction(argument_count, a2);

    // a0 - offset of start of capture.
    // a1 - length of capture.

    // Put arguments into arguments registers.
    // Parameters are
    //   a0: Address byte_offset1 - Address captured substring's start.
    //   a1: Address byte_offset2 - Address of current character position.
    //   a2: size_t byte_length - length of capture in bytes(!).
    //   a3: Isolate* isolate.

    // Address of start of capture.
    __ Add_d(a0, a0, Operand(end_of_input_address()));
    // Length of capture.
    __ mov(a2, a1);
    // Save length in callee-save register for use on return.
    __ mov(s3, a1);
    // Address of current input position.
    __ Add_d(a1, current_input_offset(), Operand(end_of_input_address()));
    if (read_backward) {
      __ Sub_d(a1, a1, Operand(s3));
    }
    // Isolate.
    __ li(a3, Operand(ExternalReference::isolate_address(masm_->isolate())));

    {
      AllowExternalCallThatCantCauseGC scope(masm_.get());
      ExternalReference function =
          unicode
              ? ExternalReference::re_case_insensitive_compare_unicode()
              : ExternalReference::re_case_insensitive_compare_non_unicode();
      __ CallCFunction(function, argument_count);
    }

    // Restore regexp engine registers.
    __ MultiPop(regexp_registers_to_retain);
    __ li(code_pointer(), Operand(masm_->CodeObject()), CONSTANT_SIZE);
    __ Ld_d(end_of_input_address(), MemOperand(frame_pointer(), kInputEnd));

    // Check if function returned non-zero for success or zero for failure.
    BranchOrBacktrack(on_no_match, eq, a0, Operand(zero_reg));
    // On success, increment position by length of capture.
    if (read_backward) {
      __ Sub_d(current_input_offset(), current_input_offset(), Operand(s3));
    } else {
      __ Add_d(current_input_offset(), current_input_offset(), Operand(s3));
    }
  }

  __ bind(&fallthrough);
}

void RegExpMacroAssemblerLOONG64::CheckNotBackReference(int start_reg,
                                                        bool read_backward,
                                                        Label* on_no_match) {
  Label fallthrough;

  // Find length of back-referenced capture.
  __ Ld_d(a0, register_location(start_reg));
  __ Ld_d(a1, register_location(start_reg + 1));
  __ Sub_d(a1, a1, a0);  // Length to check.

  // At this point, the capture registers are either both set or both cleared.
  // If the capture length is zero, then the capture is either empty or cleared.
  // Fall through in both cases.
  __ Branch(&fallthrough, eq, a1, Operand(zero_reg));

  if (read_backward) {
    __ Ld_d(t1, MemOperand(frame_pointer(), kStringStartMinusOne));
    __ Add_d(t1, t1, a1);
    BranchOrBacktrack(on_no_match, le, current_input_offset(), Operand(t1));
  } else {
    __ Add_d(t1, a1, current_input_offset());
    // Check that there are enough characters left in the input.
    BranchOrBacktrack(on_no_match, gt, t1, Operand(zero_reg));
  }

  // Compute pointers to match string and capture string.
  __ Add_d(a0, a0, Operand(end_of_input_address()));
  __ Add_d(a2, end_of_input_address(), Operand(current_input_offset()));
  if (read_backward) {
    __ Sub_d(a2, a2, Operand(a1));
  }
  __ Add_d(a1, a1, Operand(a0));

  Label loop;
  __ bind(&loop);
  if (mode_ == LATIN1) {
    __ Ld_bu(a3, MemOperand(a0, 0));
    __ addi_d(a0, a0, char_size());
    __ Ld_bu(a4, MemOperand(a2, 0));
    __ addi_d(a2, a2, char_size());
  } else {
    DCHECK(mode_ == UC16);
    __ Ld_hu(a3, MemOperand(a0, 0));
    __ addi_d(a0, a0, char_size());
    __ Ld_hu(a4, MemOperand(a2, 0));
    __ addi_d(a2, a2, char_size());
  }
  BranchOrBacktrack(on_no_match, ne, a3, Operand(a4));
  __ Branch(&loop, lt, a0, Operand(a1));

  // Move current character position to position after match.
  __ Sub_d(current_input_offset(), a2, end_of_input_address());
  if (read_backward) {
    __ Ld_d(t1, register_location(start_reg));  // Index of start of capture.
    __ Ld_d(a2, register_location(start_reg + 1));  // Index of end of capture.
    __ Add_d(current_input_offset(), current_input_offset(), Operand(t1));
    __ Sub_d(current_input_offset(), current_input_offset(), Operand(a2));
  }
  __ bind(&fallthrough);
}

void RegExpMacroAssemblerLOONG64::CheckNotCharacter(uint32_t c,
                                                    Label* on_not_equal) {
  BranchOrBacktrack(on_not_equal, ne, current_character(), Operand(c));
}

void RegExpMacroAssemblerLOONG64::CheckCharacterAfterAnd(uint32_t c,
                                                         uint32_t mask,
                                                         Label* on_equal) {
  __ And(a0, current_character(), Operand(mask));
  Operand rhs = (c == 0) ? Operand(zero_reg) : Operand(c);
  BranchOrBacktrack(on_equal, eq, a0, rhs);
}

void RegExpMacroAssemblerLOONG64::CheckNotCharacterAfterAnd(
    uint32_t c, uint32_t mask, Label* on_not_equal) {
  __ And(a0, current_character(), Operand(mask));
  Operand rhs = (c == 0) ? Operand(zero_reg) : Operand(c);
  BranchOrBacktrack(on_not_equal, ne, a0, rhs);
}

void RegExpMacroAssemblerLOONG64::CheckNotCharacterAfterMinusAnd(
    base::uc16 c, base::uc16 minus, base::uc16 mask, Label* on_not_equal) {
  DCHECK_GT(String::kMaxUtf16CodeUnit, minus);
  __ Sub_d(a0, current_character(), Operand(minus));
  __ And(a0, a0, Operand(mask));
  BranchOrBacktrack(on_not_equal, ne, a0, Operand(c));
}

void RegExpMacroAssemblerLOONG64::CheckCharacterInRange(base::uc16 from,
                                                        base::uc16 to,
                                                        Label* on_in_range) {
  __ Sub_d(a0, current_character(), Operand(from));
  // Unsigned lower-or-same condition.
  BranchOrBacktrack(on_in_range, ls, a0, Operand(to - from));
}

void RegExpMacroAssemblerLOONG64::CheckCharacterNotInRange(
    base::uc16 from, base::uc16 to, Label* on_not_in_range) {
  __ Sub_d(a0, current_character(), Operand(from));
  // Unsigned higher condition.
  BranchOrBacktrack(on_not_in_range, hi, a0, Operand(to - from));
}

void RegExpMacroAssemblerLOONG64::CheckBitInTable(Handle<ByteArray> table,
                                                  Label* on_bit_set) {
  __ li(a0, Operand(table));
  if (mode_ != LATIN1 || kTableMask != String::kMaxOneByteCharCode) {
    __ And(a1, current_character(), Operand(kTableSize - 1));
    __ Add_d(a0, a0, a1);
  } else {
    __ Add_d(a0, a0, current_character());
  }

  __ Ld_bu(a0, FieldMemOperand(a0, ByteArray::kHeaderSize));
  BranchOrBacktrack(on_bit_set, ne, a0, Operand(zero_reg));
}

bool RegExpMacroAssemblerLOONG64::CheckSpecialCharacterClass(
    base::uc16 type, Label* on_no_match) {
  // Range checks (c in min..max) are generally implemented by an unsigned
  // (c - min) <= (max - min) check.
  switch (type) {
    case 's':
      // Match space-characters.
      if (mode_ == LATIN1) {
        // One byte space characters are '\t'..'\r', ' ' and \u00a0.
        Label success;
        __ Branch(&success, eq, current_character(), Operand(' '));
        // Check range 0x09..0x0D.
        __ Sub_d(a0, current_character(), Operand('\t'));
        __ Branch(&success, ls, a0, Operand('\r' - '\t'));
        // \u00a0 (NBSP).
        BranchOrBacktrack(on_no_match, ne, a0, Operand(0x00A0 - '\t'));
        __ bind(&success);
        return true;
      }
      return false;
    case 'S':
      // The emitted code for generic character classes is good enough.
      return false;
    case 'd':
      // Match Latin1 digits ('0'..'9').
      __ Sub_d(a0, current_character(), Operand('0'));
      BranchOrBacktrack(on_no_match, hi, a0, Operand('9' - '0'));
      return true;
    case 'D':
      // Match non Latin1-digits.
      __ Sub_d(a0, current_character(), Operand('0'));
      BranchOrBacktrack(on_no_match, ls, a0, Operand('9' - '0'));
      return true;
    case '.': {
      // Match non-newlines (not 0x0A('\n'), 0x0D('\r'), 0x2028 and 0x2029).
      __ Xor(a0, current_character(), Operand(0x01));
      // See if current character is '\n'^1 or '\r'^1, i.e., 0x0B or 0x0C.
      __ Sub_d(a0, a0, Operand(0x0B));
      BranchOrBacktrack(on_no_match, ls, a0, Operand(0x0C - 0x0B));
      if (mode_ == UC16) {
        // Compare original value to 0x2028 and 0x2029, using the already
        // computed (current_char ^ 0x01 - 0x0B). I.e., check for
        // 0x201D (0x2028 - 0x0B) or 0x201E.
        __ Sub_d(a0, a0, Operand(0x2028 - 0x0B));
        BranchOrBacktrack(on_no_match, ls, a0, Operand(1));
      }
      return true;
    }
    case 'n': {
      // Match newlines (0x0A('\n'), 0x0D('\r'), 0x2028 and 0x2029).
      __ Xor(a0, current_character(), Operand(0x01));
      // See if current character is '\n'^1 or '\r'^1, i.e., 0x0B or 0x0C.
      __ Sub_d(a0, a0, Operand(0x0B));
      if (mode_ == LATIN1) {
        BranchOrBacktrack(on_no_match, hi, a0, Operand(0x0C - 0x0B));
      } else {
        Label done;
        BranchOrBacktrack(&done, ls, a0, Operand(0x0C - 0x0B));
        // Compare original value to 0x2028 and 0x2029, using the already
        // computed (current_char ^ 0x01 - 0x0B). I.e., check for
        // 0x201D (0x2028 - 0x0B) or 0x201E.
        __ Sub_d(a0, a0, Operand(0x2028 - 0x0B));
        BranchOrBacktrack(on_no_match, hi, a0, Operand(1));
        __ bind(&done);
      }
      return true;
    }
    case 'w': {
      if (mode_ != LATIN1) {
        // Table is 256 entries, so all Latin1 characters can be tested.
        BranchOrBacktrack(on_no_match, hi, current_character(), Operand('z'));
      }
      ExternalReference map = ExternalReference::re_word_character_map();
      __ li(a0, Operand(map));
      __ Add_d(a0, a0, current_character());
      __ Ld_bu(a0, MemOperand(a0, 0));
      BranchOrBacktrack(on_no_match, eq, a0, Operand(zero_reg));
      return true;
    }
    case 'W': {
      Label done;
      if (mode_ != LATIN1) {
        // Table is 256 entries, so all Latin1 characters can be tested.
        __ Branch(&done, hi, current_character(), Operand('z'));
      }
      ExternalReference map = ExternalReference::re_word_character_map();
      __ li(a0, Operand(map));
      __ Add_d(a0, a0, current_character());
      __ Ld_bu(a0, MemOperand(a0, 0));
      BranchOrBacktrack(on_no_match, ne, a0, Operand(zero_reg));
      if (mode_ != LATIN1) {
        __ bind(&done);
      }
      return true;
    }
    case '*':
      // Match any character.
      return true;
    // No custom implementation (yet): s(UC16), S(UC16).
    default:
      return false;
  }
}

void RegExpMacroAssemblerLOONG64::Fail() {
  __ li(a0, Operand(FAILURE));
  __ jmp(&exit_label_);
}

void RegExpMacroAssemblerLOONG64::LoadRegExpStackPointerFromMemory(
    Register dst) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_stack_pointer(isolate());
  __ li(dst, ref);
  __ Ld_d(dst, MemOperand(dst, 0));
}

void RegExpMacroAssemblerLOONG64::StoreRegExpStackPointerToMemory(
    Register src, Register scratch) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_stack_pointer(isolate());
  __ li(scratch, ref);
  __ St_d(src, MemOperand(scratch, 0));
}

void RegExpMacroAssemblerLOONG64::PushRegExpBasePointer(Register scratch1,
                                                        Register scratch2) {
  LoadRegExpStackPointerFromMemory(scratch1);
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_memory_top_address(isolate());
  __ li(scratch2, ref);
  __ Ld_d(scratch2, MemOperand(scratch2, 0));
  __ Sub_d(scratch2, scratch1, scratch2);
  __ St_d(scratch2, MemOperand(frame_pointer(), kRegExpStackBasePointer));
}

void RegExpMacroAssemblerLOONG64::PopRegExpBasePointer(Register scratch1,
                                                       Register scratch2) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_memory_top_address(isolate());
  __ Ld_d(scratch1, MemOperand(frame_pointer(), kRegExpStackBasePointer));
  __ li(scratch2, ref);
  __ Ld_d(scratch2, MemOperand(scratch2, 0));
  __ Add_d(scratch1, scratch1, scratch2);
  StoreRegExpStackPointerToMemory(scratch1, scratch2);
}

Handle<HeapObject> RegExpMacroAssemblerLOONG64::GetCode(Handle<String> source) {
  Label return_v0;
  if (0 /* todo masm_->has_exception()*/) {
    // If the code gets corrupted due to long regular expressions and lack of
    // space on trampolines, an internal exception flag is set. If this case
    // is detected, we will jump into exit sequence right away.
    //__ bind_to(&entry_label_, internal_failure_label_.pos());
  } else {
    // Finalize code - write the entry point code now we know how many
    // registers we need.

    // Entry code:
    __ bind(&entry_label_);

    // Tell the system that we have a stack frame.  Because the type is MANUAL,
    // no is generated.
    FrameScope scope(masm_.get(), StackFrame::MANUAL);

    // Actually emit code to start a new stack frame.
    // Push arguments
    // Save callee-save registers.
    // Start new stack frame.
    // Store link register in existing stack-cell.
    // Order here should correspond to order of offset constants in header file.
    // TODO(plind): we save s0..s7, but ONLY use s3 here - use the regs
    // or dont save.
    RegList registers_to_retain = s0.bit() | s1.bit() | s2.bit() | s3.bit() |
                                  s4.bit() | s5.bit() | s6.bit() | s7.bit();
    RegList argument_registers = a0.bit() | a1.bit() | a2.bit() | a3.bit();

    argument_registers |= a4.bit() | a5.bit() | a6.bit() | a7.bit();

    __ MultiPush(ra.bit(), fp.bit(), argument_registers | registers_to_retain);
    // Set frame pointer in space for it if this is not a direct call
    // from generated code.
    // TODO(plind): this 8 is the # of argument regs, should have definition.
    __ Add_d(frame_pointer(), sp, Operand(8 * kPointerSize));
    STATIC_ASSERT(kSuccessfulCaptures == kInputString - kSystemPointerSize);
    __ mov(a0, zero_reg);
    __ Push(a0);  // Make room for success counter and initialize it to 0.
    STATIC_ASSERT(kStringStartMinusOne ==
                  kSuccessfulCaptures - kSystemPointerSize);
    __ Push(a0);  // Make room for "string start - 1" constant.
    STATIC_ASSERT(kBacktrackCount == kStringStartMinusOne - kSystemPointerSize);
    __ Push(a0);  // The backtrack counter
    STATIC_ASSERT(kRegExpStackBasePointer ==
                  kBacktrackCount - kSystemPointerSize);
    __ Push(a0);  // The regexp stack base ptr.

    // Store the regexp base pointer - we'll later restore it / write it to
    // memory when returning from this irregexp code object.
    PushRegExpBasePointer(a0, a1);

    // Check if we have space on the stack for registers.
    Label stack_limit_hit;
    Label stack_ok;

    ExternalReference stack_limit =
        ExternalReference::address_of_jslimit(masm_->isolate());
    __ li(a0, Operand(stack_limit));
    __ Ld_d(a0, MemOperand(a0, 0));
    __ Sub_d(a0, sp, a0);
    // Handle it if the stack pointer is already below the stack limit.
    __ Branch(&stack_limit_hit, le, a0, Operand(zero_reg));
    // Check if there is room for the variable number of registers above
    // the stack limit.
    __ Branch(&stack_ok, hs, a0, Operand(num_registers_ * kPointerSize));
    // Exit with OutOfMemory exception. There is not enough space on the stack
    // for our working registers.
    __ li(a0, Operand(EXCEPTION));
    __ jmp(&return_v0);

    __ bind(&stack_limit_hit);
    CallCheckStackGuardState(a0);
    // If returned value is non-zero, we exit with the returned value as result.
    __ Branch(&return_v0, ne, a0, Operand(zero_reg));

    __ bind(&stack_ok);
    // Allocate space on stack for registers.
    __ Sub_d(sp, sp, Operand(num_registers_ * kPointerSize));
    // Load string end.
    __ Ld_d(end_of_input_address(), MemOperand(frame_pointer(), kInputEnd));
    // Load input start.
    __ Ld_d(a0, MemOperand(frame_pointer(), kInputStart));
    // Find negative length (offset of start relative to end).
    __ Sub_d(current_input_offset(), a0, end_of_input_address());
    // Set a0 to address of char before start of the input string
    // (effectively string position -1).
    __ Ld_d(a1, MemOperand(frame_pointer(), kStartIndex));
    __ Sub_d(a0, current_input_offset(), Operand(char_size()));
    __ slli_d(t1, a1, (mode_ == UC16) ? 1 : 0);
    __ Sub_d(a0, a0, t1);
    // Store this value in a local variable, for use when clearing
    // position registers.
    __ St_d(a0, MemOperand(frame_pointer(), kStringStartMinusOne));

    // Initialize code pointer register
    __ li(code_pointer(), Operand(masm_->CodeObject()), CONSTANT_SIZE);

    Label load_char_start_regexp, start_regexp;
    // Load newline if index is at start, previous character otherwise.
    __ Branch(&load_char_start_regexp, ne, a1, Operand(zero_reg));
    __ li(current_character(), Operand('\n'));
    __ jmp(&start_regexp);

    // Global regexp restarts matching here.
    __ bind(&load_char_start_regexp);
    // Load previous char as initial value of current character register.
    LoadCurrentCharacterUnchecked(-1, 1);
    __ bind(&start_regexp);

    // Initialize on-stack registers.
    if (num_saved_registers_ > 0) {  // Always is, if generated from a regexp.
      // Fill saved registers with initial value = start offset - 1.
      if (num_saved_registers_ > 8) {
        // Address of register 0.
        __ Add_d(a1, frame_pointer(), Operand(kRegisterZero));
        __ li(a2, Operand(num_saved_registers_));
        Label init_loop;
        __ bind(&init_loop);
        __ St_d(a0, MemOperand(a1, 0));
        __ Add_d(a1, a1, Operand(-kPointerSize));
        __ Sub_d(a2, a2, Operand(1));
        __ Branch(&init_loop, ne, a2, Operand(zero_reg));
      } else {
        for (int i = 0; i < num_saved_registers_; i++) {
          __ St_d(a0, register_location(i));
        }
      }
    }

    // Initialize backtrack stack pointer.
    LoadRegExpStackPointerFromMemory(backtrack_stackpointer());

    __ jmp(&start_label_);

    // Exit code:
    if (success_label_.is_linked()) {
      // Save captures when successful.
      __ bind(&success_label_);
      if (num_saved_registers_ > 0) {
        // Copy captures to output.
        __ Ld_d(a1, MemOperand(frame_pointer(), kInputStart));
        __ Ld_d(a0, MemOperand(frame_pointer(), kRegisterOutput));
        __ Ld_d(a2, MemOperand(frame_pointer(), kStartIndex));
        __ Sub_d(a1, end_of_input_address(), a1);
        // a1 is length of input in bytes.
        if (mode_ == UC16) {
          __ srli_d(a1, a1, 1);
        }
        // a1 is length of input in characters.
        __ Add_d(a1, a1, Operand(a2));
        // a1 is length of string in characters.

        DCHECK_EQ(0, num_saved_registers_ % 2);
        // Always an even number of capture registers. This allows us to
        // unroll the loop once to add an operation between a load of a register
        // and the following use of that register.
        for (int i = 0; i < num_saved_registers_; i += 2) {
          __ Ld_d(a2, register_location(i));
          __ Ld_d(a3, register_location(i + 1));
          if (i == 0 && global_with_zero_length_check()) {
            // Keep capture start in a4 for the zero-length check later.
            __ mov(t3, a2);
          }
          if (mode_ == UC16) {
            __ srai_d(a2, a2, 1);
            __ Add_d(a2, a2, a1);
            __ srai_d(a3, a3, 1);
            __ Add_d(a3, a3, a1);
          } else {
            __ Add_d(a2, a1, Operand(a2));
            __ Add_d(a3, a1, Operand(a3));
          }
          // V8 expects the output to be an int32_t array.
          __ St_w(a2, MemOperand(a0, 0));
          __ Add_d(a0, a0, kIntSize);
          __ St_w(a3, MemOperand(a0, 0));
          __ Add_d(a0, a0, kIntSize);
        }
      }

      if (global()) {
        // Restart matching if the regular expression is flagged as global.
        __ Ld_d(a0, MemOperand(frame_pointer(), kSuccessfulCaptures));
        __ Ld_d(a1, MemOperand(frame_pointer(), kNumOutputRegisters));
        __ Ld_d(a2, MemOperand(frame_pointer(), kRegisterOutput));
        // Increment success counter.
        __ Add_d(a0, a0, 1);
        __ St_d(a0, MemOperand(frame_pointer(), kSuccessfulCaptures));
        // Capture results have been stored, so the number of remaining global
        // output registers is reduced by the number of stored captures.
        __ Sub_d(a1, a1, num_saved_registers_);
        // Check whether we have enough room for another set of capture results.
        //__ mov(v0, a0);
        __ Branch(&return_v0, lt, a1, Operand(num_saved_registers_));

        __ St_d(a1, MemOperand(frame_pointer(), kNumOutputRegisters));
        // Advance the location for output.
        __ Add_d(a2, a2, num_saved_registers_ * kIntSize);
        __ St_d(a2, MemOperand(frame_pointer(), kRegisterOutput));

        // Prepare a0 to initialize registers with its value in the next run.
        __ Ld_d(a0, MemOperand(frame_pointer(), kStringStartMinusOne));

        if (global_with_zero_length_check()) {
          // Special case for zero-length matches.
          // t3: capture start index
          // Not a zero-length match, restart.
          __ Branch(&load_char_start_regexp, ne, current_input_offset(),
                    Operand(t3));
          // Offset from the end is zero if we already reached the end.
          __ Branch(&exit_label_, eq, current_input_offset(),
                    Operand(zero_reg));
          // Advance current position after a zero-length match.
          Label advance;
          __ bind(&advance);
          __ Add_d(current_input_offset(), current_input_offset(),
                   Operand((mode_ == UC16) ? 2 : 1));
          if (global_unicode()) CheckNotInSurrogatePair(0, &advance);
        }

        __ Branch(&load_char_start_regexp);
      } else {
        __ li(a0, Operand(SUCCESS));
      }
    }
    // Exit and return v0.
    __ bind(&exit_label_);
    if (global()) {
      __ Ld_d(a0, MemOperand(frame_pointer(), kSuccessfulCaptures));
    }

    __ bind(&return_v0);
    // Restore the original regexp stack pointer value (effectively, pop the
    // stored base pointer).
    PopRegExpBasePointer(a1, a2);

    // Skip sp past regexp registers and local variables..
    __ mov(sp, frame_pointer());
    // Restore registers s0..s7 and return (restoring ra to pc).
    __ MultiPop(ra.bit(), fp.bit(), registers_to_retain);
    __ Ret();

    // Backtrack code (branch target for conditional backtracks).
    if (backtrack_label_.is_linked()) {
      __ bind(&backtrack_label_);
      Backtrack();
    }

    Label exit_with_exception;

    // Preempt-code.
    if (check_preempt_label_.is_linked()) {
      SafeCallTarget(&check_preempt_label_);
      // Put regexp engine registers on stack.
      StoreRegExpStackPointerToMemory(backtrack_stackpointer(), a1);

      RegList regexp_registers_to_retain = current_input_offset().bit() |
                                           current_character().bit() |
                                           backtrack_stackpointer().bit();
      __ MultiPush(regexp_registers_to_retain);
      CallCheckStackGuardState(a0);
      __ MultiPop(regexp_registers_to_retain);
      // If returning non-zero, we should end execution with the given
      // result as return value.
      __ Branch(&return_v0, ne, a0, Operand(zero_reg));

      LoadRegExpStackPointerFromMemory(backtrack_stackpointer());

      // String might have moved: Reload end of string from frame.
      __ Ld_d(end_of_input_address(), MemOperand(frame_pointer(), kInputEnd));
      __ li(code_pointer(), Operand(masm_->CodeObject()), CONSTANT_SIZE);

      SafeReturn();
    }

    // Backtrack stack overflow code.
    if (stack_overflow_label_.is_linked()) {
      SafeCallTarget(&stack_overflow_label_);
      StoreRegExpStackPointerToMemory(backtrack_stackpointer(), a1);
      // Reached if the backtrack-stack limit has been hit.
      // Put regexp engine registers on stack first.
      RegList regexp_registers =
          current_input_offset().bit() | current_character().bit();
      __ MultiPush(regexp_registers);

      // Call GrowStack(isolate).
      static const int kNumArguments = 1;
      __ PrepareCallCFunction(kNumArguments, a0);
      __ li(a0, Operand(ExternalReference::isolate_address(masm_->isolate())));
      ExternalReference grow_stack = ExternalReference::re_grow_stack();
      __ CallCFunction(grow_stack, kNumArguments);
      // Restore regexp registers.
      __ MultiPop(regexp_registers);
      // If nullptr is returned, we have failed to grow the stack, and must exit
      // with a stack-overflow exception.
      __ Branch(&exit_with_exception, eq, a0, Operand(zero_reg));
      // Otherwise use return value as new stack pointer.
      __ mov(backtrack_stackpointer(), a0);
      // Restore saved registers and continue.
      __ li(code_pointer(), Operand(masm_->CodeObject()), CONSTANT_SIZE);
      __ Ld_d(end_of_input_address(), MemOperand(frame_pointer(), kInputEnd));
      SafeReturn();
    }

    if (exit_with_exception.is_linked()) {
      // If any of the code above needed to exit with an exception.
      __ bind(&exit_with_exception);
      // Exit with Result EXCEPTION(-1) to signal thrown exception.
      __ li(a0, Operand(EXCEPTION));
      __ jmp(&return_v0);
    }

    if (fallback_label_.is_linked()) {
      __ bind(&fallback_label_);
      __ li(a0, Operand(FALLBACK_TO_EXPERIMENTAL));
      __ jmp(&return_v0);
    }
  }

  CodeDesc code_desc;
  masm_->GetCode(isolate(), &code_desc);
  Handle<Code> code =
      Factory::CodeBuilder(isolate(), code_desc, CodeKind::REGEXP)
          .set_self_reference(masm_->CodeObject())
          .Build();
  LOG(masm_->isolate(),
      RegExpCodeCreateEvent(Handle<AbstractCode>::cast(code), source));
  return Handle<HeapObject>::cast(code);
}

void RegExpMacroAssemblerLOONG64::GoTo(Label* to) {
  if (to == nullptr) {
    Backtrack();
    return;
  }
  __ jmp(to);
  return;
}

void RegExpMacroAssemblerLOONG64::IfRegisterGE(int reg, int comparand,
                                               Label* if_ge) {
  __ Ld_d(a0, register_location(reg));
  BranchOrBacktrack(if_ge, ge, a0, Operand(comparand));
}

void RegExpMacroAssemblerLOONG64::IfRegisterLT(int reg, int comparand,
                                               Label* if_lt) {
  __ Ld_d(a0, register_location(reg));
  BranchOrBacktrack(if_lt, lt, a0, Operand(comparand));
}

void RegExpMacroAssemblerLOONG64::IfRegisterEqPos(int reg, Label* if_eq) {
  __ Ld_d(a0, register_location(reg));
  BranchOrBacktrack(if_eq, eq, a0, Operand(current_input_offset()));
}

RegExpMacroAssembler::IrregexpImplementation
RegExpMacroAssemblerLOONG64::Implementation() {
  return kLOONG64Implementation;
}

void RegExpMacroAssemblerLOONG64::PopCurrentPosition() {
  Pop(current_input_offset());
}

void RegExpMacroAssemblerLOONG64::PopRegister(int register_index) {
  Pop(a0);
  __ St_d(a0, register_location(register_index));
}

void RegExpMacroAssemblerLOONG64::PushBacktrack(Label* label) {
  if (label->is_bound()) {
    int target = label->pos();
    __ li(a0, Operand(target + Code::kHeaderSize - kHeapObjectTag));
  } else {
    Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_.get());
    Label after_constant;
    __ Branch(&after_constant);
    int offset = masm_->pc_offset();
    int cp_offset = offset + Code::kHeaderSize - kHeapObjectTag;
    //__ emit(0);
    __ nop();
    masm_->label_at_put(label, offset);
    __ bind(&after_constant);
    if (is_int12(cp_offset)) {
      __ Ld_wu(a0, MemOperand(code_pointer(), cp_offset));
    } else {
      __ Add_d(a0, code_pointer(), cp_offset);
      __ Ld_wu(a0, MemOperand(a0, 0));
    }
  }
  Push(a0);
  CheckStackLimit();
}

void RegExpMacroAssemblerLOONG64::PushCurrentPosition() {
  Push(current_input_offset());
}

void RegExpMacroAssemblerLOONG64::PushRegister(
    int register_index, StackCheckFlag check_stack_limit) {
  __ Ld_d(a0, register_location(register_index));
  Push(a0);
  if (check_stack_limit) CheckStackLimit();
}

void RegExpMacroAssemblerLOONG64::ReadCurrentPositionFromRegister(int reg) {
  __ Ld_d(current_input_offset(), register_location(reg));
}

void RegExpMacroAssemblerLOONG64::WriteStackPointerToRegister(int reg) {
  ExternalReference stack_top_address =
      ExternalReference::address_of_regexp_stack_memory_top_address(isolate());
  __ li(a0, stack_top_address);
  __ Ld_d(a0, MemOperand(a0, 0));
  __ Sub_d(a0, backtrack_stackpointer(), a0);
  __ St_d(a0, register_location(reg));
}

void RegExpMacroAssemblerLOONG64::ReadStackPointerFromRegister(int reg) {
  ExternalReference stack_top_address =
      ExternalReference::address_of_regexp_stack_memory_top_address(isolate());
  __ li(backtrack_stackpointer(), stack_top_address);
  __ Ld_d(backtrack_stackpointer(), MemOperand(backtrack_stackpointer(), 0));
  __ Ld_d(a0, register_location(reg));
  __ Add_d(backtrack_stackpointer(), backtrack_stackpointer(), Operand(a0));
}

void RegExpMacroAssemblerLOONG64::SetCurrentPositionFromEnd(int by) {
  Label after_position;
  __ Branch(&after_position, ge, current_input_offset(),
            Operand(-by * char_size()));
  __ li(current_input_offset(), -by * char_size());
  // On RegExp code entry (where this operation is used), the character before
  // the current position is expected to be already loaded.
  // We have advanced the position, so it's safe to read backwards.
  LoadCurrentCharacterUnchecked(-1, 1);
  __ bind(&after_position);
}

void RegExpMacroAssemblerLOONG64::SetRegister(int register_index, int to) {
  DCHECK(register_index >= num_saved_registers_);  // Reserved for positions!
  __ li(a0, Operand(to));
  __ St_d(a0, register_location(register_index));
}

bool RegExpMacroAssemblerLOONG64::Succeed() {
  __ jmp(&success_label_);
  return global();
}

void RegExpMacroAssemblerLOONG64::WriteCurrentPositionToRegister(
    int reg, int cp_offset) {
  if (cp_offset == 0) {
    __ St_d(current_input_offset(), register_location(reg));
  } else {
    __ Add_d(a0, current_input_offset(), Operand(cp_offset * char_size()));
    __ St_d(a0, register_location(reg));
  }
}

void RegExpMacroAssemblerLOONG64::ClearRegisters(int reg_from, int reg_to) {
  DCHECK(reg_from <= reg_to);
  __ Ld_d(a0, MemOperand(frame_pointer(), kStringStartMinusOne));
  for (int reg = reg_from; reg <= reg_to; reg++) {
    __ St_d(a0, register_location(reg));
  }
}

// Private methods:

void RegExpMacroAssemblerLOONG64::CallCheckStackGuardState(Register scratch) {
  DCHECK(!isolate()->IsGeneratingEmbeddedBuiltins());
  DCHECK(!masm_->options().isolate_independent_code);

  int stack_alignment = base::OS::ActivationFrameAlignment();

  // Align the stack pointer and save the original sp value on the stack.
  __ mov(scratch, sp);
  __ Sub_d(sp, sp, Operand(kPointerSize));
  DCHECK(base::bits::IsPowerOfTwo(stack_alignment));
  __ And(sp, sp, Operand(-stack_alignment));
  __ St_d(scratch, MemOperand(sp, 0));

  __ mov(a2, frame_pointer());
  // Code of self.
  __ li(a1, Operand(masm_->CodeObject()), CONSTANT_SIZE);

  // We need to make room for the return address on the stack.
  DCHECK(IsAligned(stack_alignment, kPointerSize));
  __ Sub_d(sp, sp, Operand(stack_alignment));

  // The stack pointer now points to cell where the return address will be
  // written. Arguments are in registers, meaning we treat the return address as
  // argument 5. Since DirectCEntry will handle allocating space for the C
  // argument slots, we don't need to care about that here. This is how the
  // stack will look (sp meaning the value of sp at this moment):
  // [sp + 3] - empty slot if needed for alignment.
  // [sp + 2] - saved sp.
  // [sp + 1] - second word reserved for return value.
  // [sp + 0] - first word reserved for return value.

  // a0 will point to the return address, placed by DirectCEntry.
  __ mov(a0, sp);

  ExternalReference stack_guard_check =
      ExternalReference::re_check_stack_guard_state();
  __ li(t7, Operand(stack_guard_check));

  EmbeddedData d = EmbeddedData::FromBlob();
  CHECK(Builtins::IsIsolateIndependent(Builtin::kDirectCEntry));
  Address entry = d.InstructionStartOfBuiltin(Builtin::kDirectCEntry);
  __ li(kScratchReg, Operand(entry, RelocInfo::OFF_HEAP_TARGET));
  __ Call(kScratchReg);

  // DirectCEntry allocated space for the C argument slots so we have to
  // drop them with the return address from the stack with loading saved sp.
  // At this point stack must look:
  // [sp + 7] - empty slot if needed for alignment.
  // [sp + 6] - saved sp.
  // [sp + 5] - second word reserved for return value.
  // [sp + 4] - first word reserved for return value.
  // [sp + 3] - C argument slot.
  // [sp + 2] - C argument slot.
  // [sp + 1] - C argument slot.
  // [sp + 0] - C argument slot.
  __ Ld_d(sp, MemOperand(sp, stack_alignment));

  __ li(code_pointer(), Operand(masm_->CodeObject()));
}

// Helper function for reading a value out of a stack frame.
template <typename T>
static T& frame_entry(Address re_frame, int frame_offset) {
  return reinterpret_cast<T&>(Memory<int32_t>(re_frame + frame_offset));
}

template <typename T>
static T* frame_entry_address(Address re_frame, int frame_offset) {
  return reinterpret_cast<T*>(re_frame + frame_offset);
}

int64_t RegExpMacroAssemblerLOONG64::CheckStackGuardState(
    Address* return_address, Address raw_code, Address re_frame) {
  Code re_code = Code::cast(Object(raw_code));
  return NativeRegExpMacroAssembler::CheckStackGuardState(
      frame_entry<Isolate*>(re_frame, kIsolate),
      static_cast<int>(frame_entry<int64_t>(re_frame, kStartIndex)),
      static_cast<RegExp::CallOrigin>(
          frame_entry<int64_t>(re_frame, kDirectCall)),
      return_address, re_code,
      frame_entry_address<Address>(re_frame, kInputString),
      frame_entry_address<const byte*>(re_frame, kInputStart),
      frame_entry_address<const byte*>(re_frame, kInputEnd));
}

MemOperand RegExpMacroAssemblerLOONG64::register_location(int register_index) {
  DCHECK(register_index < (1 << 30));
  if (num_registers_ <= register_index) {
    num_registers_ = register_index + 1;
  }
  return MemOperand(frame_pointer(),
                    kRegisterZero - register_index * kPointerSize);
}

void RegExpMacroAssemblerLOONG64::CheckPosition(int cp_offset,
                                                Label* on_outside_input) {
  if (cp_offset >= 0) {
    BranchOrBacktrack(on_outside_input, ge, current_input_offset(),
                      Operand(-cp_offset * char_size()));
  } else {
    __ Ld_d(a1, MemOperand(frame_pointer(), kStringStartMinusOne));
    __ Add_d(a0, current_input_offset(), Operand(cp_offset * char_size()));
    BranchOrBacktrack(on_outside_input, le, a0, Operand(a1));
  }
}

void RegExpMacroAssemblerLOONG64::BranchOrBacktrack(Label* to,
                                                    Condition condition,
                                                    Register rs,
                                                    const Operand& rt) {
  if (condition == al) {  // Unconditional.
    if (to == nullptr) {
      Backtrack();
      return;
    }
    __ jmp(to);
    return;
  }
  if (to == nullptr) {
    __ Branch(&backtrack_label_, condition, rs, rt);
    return;
  }
  __ Branch(to, condition, rs, rt);
}

void RegExpMacroAssemblerLOONG64::SafeCall(Label* to, Condition cond,
                                           Register rs, const Operand& rt) {
  __ Branch(to, cond, rs, rt, true);
}

void RegExpMacroAssemblerLOONG64::SafeReturn() {
  __ Pop(ra);
  __ Add_d(t1, ra, Operand(masm_->CodeObject()));
  __ Jump(t1);
}

void RegExpMacroAssemblerLOONG64::SafeCallTarget(Label* name) {
  __ bind(name);
  __ Sub_d(ra, ra, Operand(masm_->CodeObject()));
  __ Push(ra);
}

void RegExpMacroAssemblerLOONG64::Push(Register source) {
  DCHECK(source != backtrack_stackpointer());
  __ Add_d(backtrack_stackpointer(), backtrack_stackpointer(),
           Operand(-kIntSize));
  __ St_w(source, MemOperand(backtrack_stackpointer(), 0));
}

void RegExpMacroAssemblerLOONG64::Pop(Register target) {
  DCHECK(target != backtrack_stackpointer());
  __ Ld_w(target, MemOperand(backtrack_stackpointer(), 0));
  __ Add_d(backtrack_stackpointer(), backtrack_stackpointer(), kIntSize);
}

void RegExpMacroAssemblerLOONG64::CheckPreemption() {
  // Check for preemption.
  ExternalReference stack_limit =
      ExternalReference::address_of_jslimit(masm_->isolate());
  __ li(a0, Operand(stack_limit));
  __ Ld_d(a0, MemOperand(a0, 0));
  SafeCall(&check_preempt_label_, ls, sp, Operand(a0));
}

void RegExpMacroAssemblerLOONG64::CheckStackLimit() {
  ExternalReference stack_limit =
      ExternalReference::address_of_regexp_stack_limit_address(
          masm_->isolate());

  __ li(a0, Operand(stack_limit));
  __ Ld_d(a0, MemOperand(a0, 0));
  SafeCall(&stack_overflow_label_, ls, backtrack_stackpointer(), Operand(a0));
}

void RegExpMacroAssemblerLOONG64::LoadCurrentCharacterUnchecked(
    int cp_offset, int characters) {
  Register offset = current_input_offset();

  // If unaligned load/stores are not supported then this function must only
  // be used to load a single character at a time.
  if (!CanReadUnaligned()) {
    DCHECK_EQ(1, characters);
  }

  if (cp_offset != 0) {
    // t3 is not being used to store the capture start index at this point.
    __ Add_d(t3, current_input_offset(), Operand(cp_offset * char_size()));
    offset = t3;
  }

  if (mode_ == LATIN1) {
    if (characters == 4) {
      __ Ld_wu(current_character(), MemOperand(end_of_input_address(), offset));
    } else if (characters == 2) {
      __ Ld_hu(current_character(), MemOperand(end_of_input_address(), offset));
    } else {
      DCHECK_EQ(1, characters);
      __ Ld_bu(current_character(), MemOperand(end_of_input_address(), offset));
    }
  } else {
    DCHECK(mode_ == UC16);
    if (characters == 2) {
      __ Ld_wu(current_character(), MemOperand(end_of_input_address(), offset));
    } else {
      DCHECK_EQ(1, characters);
      __ Ld_hu(current_character(), MemOperand(end_of_input_address(), offset));
    }
  }
}

#undef __

}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_LOONG64

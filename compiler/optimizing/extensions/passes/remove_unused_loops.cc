/*
 * INTEL CONFIDENTIAL
 * Copyright (c) 2015, Intel Corporation All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to the
 * source code ("Material") are owned by Intel Corporation or its suppliers or
 * licensors. Title to the Material remains with Intel Corporation or its suppliers
 * and licensors. The Material contains trade secrets and proprietary and
 * confidential information of Intel or its suppliers and licensors. The Material
 * is protected by worldwide copyright and trade secret laws and treaty provisions.
 * No part of the Material may be used, copied, reproduced, modified, published,
 * uploaded, posted, transmitted, distributed, or disclosed in any way without
 * Intel's prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery of
 * the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be express
 * and approved by Intel in writing.
 *
 */
#include "remove_unused_loops.h"

#include "base/stringprintf.h"
#include "ext_utility.h"
#include "graph_x86.h"
#include "loop_formation.h"
#include "loop_iterators.h"

namespace art {

void HRemoveUnusedLoops::Run() {
  PRINT_PASS_OSTREAM_MESSAGE(this, "start " << GetMethodName(graph_));

  HGraph_X86* graph = GRAPH_TO_GRAPH_X86(graph_);
  HLoopInformation_X86 *graph_loop_info = graph->GetLoopInformation();

  // Walk all the inner loops in the graph.
  bool changed = false;
  for (HOnlyInnerLoopIterator it(graph_loop_info); !it.Done(); it.Advance()) {
    HLoopInformation_X86* loop_info = it.Current();
    HBasicBlock* pre_header = loop_info->GetPreHeader();

    PRINT_PASS_OSTREAM_MESSAGE(this, "Visit " << loop_info->GetHeader()->GetBlockId()
                                     << ", preheader = " << pre_header->GetBlockId());

    // The exit block from the loop.
    HBasicBlock* exit_block = loop_info->GetExitBlock();

    if (exit_block == nullptr) {
      // We need exactly 1 exit block from the loop.
      PRINT_PASS_MESSAGE(this, "Too many or too few exit blocks");
      continue;
    }

    // This must be a countable loop.
    if (!loop_info->HasKnownNumIterations()) {
      PRINT_PASS_MESSAGE(this, "Loop is not countable");
      continue;
    }

    // Walk through the blocks in the loop.
    bool loop_is_empty = true;
    external_loop_phis_.clear();
    for (HBlocksInLoopIterator it_loop(*loop_info); !it_loop.Done(); it_loop.Advance()) {
      HBasicBlock* loop_block = it_loop.Current();

      if (CheckPhisInBlock(loop_info, loop_block) == false) {
        // Phi result is used outside the loop.
        loop_is_empty = false;
        break;
      }

      if (CheckInstructionsInBlock(loop_info, loop_block) == false) {
        // Instruction result is used outside the loop.
        loop_is_empty = false;
        break;
      }
    }

    PRINT_PASS_OSTREAM_MESSAGE(this, "Loop end: is_empty = " << (loop_is_empty ? "true" : "false"));

    if (loop_is_empty) {
      UpdateExternalPhis();
      RemoveLoop(loop_info, pre_header, exit_block);
      MaybeRecordStat(MethodCompilationStat::kIntelRemoveUnusedLoops);
      changed = true;
    }
  }
  PRINT_PASS_OSTREAM_MESSAGE(this, "end " << GetMethodName(graph_));
  if (changed) {
    // We have to rebuild our loops properly, now that we have removed loops.
    HLoopFormation form_loops(graph_);
    form_loops.Run();
  }
}

bool HRemoveUnusedLoops::CheckInstructionsInBlock(HLoopInformation_X86* loop_info,
                                                 HBasicBlock* loop_block) {
  // Walk through the instructions in the loop, and see if the instruction
  // has any side effect, or it can throw.  If neither, see if the result is used
  // outside the loop.
  for (HInstructionIterator inst_it(loop_block->GetInstructions());
       !inst_it.Done();
       inst_it.Advance()) {
    HInstruction* instruction = inst_it.Current();
    PRINT_PASS_OSTREAM_MESSAGE(this, "Look at: " << instruction
                    << (instruction->HasSideEffects() ? " <has side effects>" : "")
                    << (!instruction->CanBeMoved() ? " <can't be moved>" : "")
                    << (instruction->CanThrow() ? " <can throw>" : "")
                    << (instruction->IsControlFlow() ? " <is control flow>" : ""));

    // Special case SuspendCheck.  We don't care about it.
    if (instruction->IsSuspendCheck()) {
      continue;
    }

    // Special case ControlFlow.  If it exits the loop, we would catch it above.
    // Throw is a special case.
    if (instruction->IsControlFlow() && !instruction->CanThrow()) {
      continue;
    }

    if (instruction->HasSideEffects() || instruction->CanThrow() ||
        !instruction->CanBeMoved()) {
      // Not an empty loop.
      PRINT_PASS_MESSAGE(this, "need this instruction");
      return false;
    }

    // Check that all the uses of the instruction are used in the loop.
    if (instruction->GetType() == Primitive::kPrimVoid) {
      // Isn't used by anyone.
      continue;
    }

    bool no_side_effects = true;  // All instructions are okay.
    std::unordered_set<HPhi*> worklist;
    for (HUseIterator<HInstruction*> it2(instruction->GetUses()); !it2.Done(); it2.Advance()) {
      HInstruction* insn = it2.Current()->GetUser();
      HBasicBlock* insn_block = insn->GetBlock();
      HLoopInformation* li = insn_block->GetLoopInformation();
      PRINT_PASS_OSTREAM_MESSAGE(this, "Result is used by: " << insn);
      if (li != loop_info) {
        // We are being used in a different loop.  Is it REALLY used?
        // Only special case Phis for this check.
        HPhi* insn_as_phi = insn->AsPhi();
        if (insn_as_phi != nullptr && !insn->HasUses()) {
          PRINT_PASS_MESSAGE(this, "Used by Phi in different loop -- has no uses (removing)");
          worklist.insert(insn_as_phi);
        } else {
          PRINT_PASS_MESSAGE(this, "Used in different loop");
          no_side_effects = false;
        }
      }
    }
    for (auto insn_as_phi : worklist) {
      insn_as_phi->GetBlock()->RemovePhi(insn_as_phi);
    }
    if (!no_side_effects) {
      return false;  // Other insn may be skipped.
    }
  }

  // All instructions are okay.
  return true;
}

static bool BothInputsAreFromOutsideInnerLoop(HPhi* phi,
                                              HLoopInformation_X86* loop_info) {
  // Only handle 2 input Phis.
  if (phi->InputCount() != 2) {
    return false;
  }

  // Check that the inputs are not within the loop.  Since we only handle
  // inner loops, any input which has a different loop information is outside.
  HLoopInformation* li = phi->InputAt(0)->GetBlock()->GetLoopInformation();
  if (li == loop_info) {
    return false;
  }
  li = phi->InputAt(1)->GetBlock()->GetLoopInformation();
  return li != loop_info;
}

bool HRemoveUnusedLoops::CheckPhisInBlock(HLoopInformation_X86* loop_info,
                                         HBasicBlock* loop_block) {
  // We are only looking at inner loops.
  DCHECK(loop_info->IsInner());

  // Walk through the Phis in the loop, and see if the result
  // is used outside the loop.
  for (HInstructionIterator inst_it(loop_block->GetPhis());
       !inst_it.Done();
       inst_it.Advance()) {
    HPhi* phi = inst_it.Current()->AsPhi();
    DCHECK(phi != nullptr);
    PRINT_PASS_OSTREAM_MESSAGE(this, "Look at: " << phi);
    // Special case the case where both inputs are from outside the loop.
    // Only valid in loop header.
    if (phi->IsLoopHeaderPhi() && BothInputsAreFromOutsideInnerLoop(phi, loop_info)) {
      PRINT_PASS_OSTREAM_MESSAGE(this, "Phi has 2 external inputs: "
                                        << phi->InputAt(0) << ' ' << phi->InputAt(1));
      external_loop_phis_.insert(phi);
      continue;
    }

    for (HUseIterator<HInstruction*> it2(phi->GetUses()); !it2.Done(); it2.Advance()) {
      HInstruction* insn = it2.Current()->GetUser();
      HBasicBlock* insn_block = insn->GetBlock();
      HLoopInformation* li = insn_block->GetLoopInformation();
      PRINT_PASS_OSTREAM_MESSAGE(this, "Result is used by: " << insn);
      if (li != loop_info) {
        // We are being used in a different loop (could be out of the loop).
        PRINT_PASS_MESSAGE(this, "Used in different loop");
        return false;
      }
    }
  }

  // All okay.
  return true;
}

void HRemoveUnusedLoops::RemoveLoop(HLoopInformation_X86* loop_info,
                                   HBasicBlock* pre_header,
                                   HBasicBlock* exit_block) {
  HGraph_X86* graph = GRAPH_TO_GRAPH_X86(graph_);
  HBasicBlock* loop_header = loop_info->GetHeader();
  PRINT_PASS_OSTREAM_MESSAGE(this, "Remove loop blocks: "
                                   << loop_header->GetBlockId()
                                   << ", preheader = " << pre_header->GetBlockId());

  // TODO: Use kind of arena specific for optimization.
  ArenaVector<HBasicBlock*> blocks_in_loop(
      graph->GetArena()->Adapter(kArenaAllocOptimization));
  for (HBlocksInLoopIterator it_loop(*loop_info); !it_loop.Done(); it_loop.Advance()) {
    HBasicBlock* loop_block = it_loop.Current();
    DCHECK(loop_block->IsInLoop());
    DCHECK(loop_block->GetLoopInformation() == loop_info);
    blocks_in_loop.push_back(loop_block);
  }

  // Change the successor to the preheader to the exit block.
  DCHECK_EQ(pre_header->GetSuccessors().size(), 1u);
  PRINT_PASS_OSTREAM_MESSAGE(this, "Set preheader to successor " << exit_block->GetBlockId());
  pre_header->ReplaceSuccessor(loop_header, exit_block);
  pre_header->ReplaceDominatedBlock(loop_header, exit_block);
  exit_block->SetDominator(pre_header);

  for (HBasicBlock* loop_block : blocks_in_loop) {
    PRINT_PASS_OSTREAM_MESSAGE(this, "Remove block " << loop_block->GetBlockId());
    graph->DeleteBlock(loop_block);
  }

  HLoopInformation_X86* tmp = loop_info;
  while (tmp != nullptr) {
    for (size_t i = 0, e = blocks_in_loop.size(); i < e; ++i) {
      tmp->Remove(blocks_in_loop[i]);
    }
    tmp = tmp->GetParent();
  }
}

void HRemoveUnusedLoops::UpdateExternalPhis() {
  for (auto it : external_loop_phis_) {
    // Replace each phi with the value computed in the loop.
    PRINT_PASS_OSTREAM_MESSAGE(this, "Replace Phi " << it << " with " << it->InputAt(1));
    it->ReplaceWith(it->InputAt(1));
  }
}

}  // namespace art

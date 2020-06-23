/*
 * Copyright 2016 - 2019  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "DOALL.hpp"
#include "DOALLTask.hpp"

void DOALL::rewireLoopToIterateChunks (
  LoopDependenceInfo *LDI
  ){

  /*
   * Fetch the task.
   */
  auto task = (DOALLTask *)tasks[0];

  /*
   * Fetch loop and IV information.
   */
  auto loopSummary = LDI->getLoopSummary();
  auto loopHeader = loopSummary->getHeader();
  auto loopPreHeader = loopSummary->getPreHeader();
  auto preheaderClone = task->getCloneOfOriginalBasicBlock(loopPreHeader);
  auto headerClone = task->getCloneOfOriginalBasicBlock(loopHeader);
  auto allIVInfo = LDI->getInductionVariableManager();

  /*
   * Hook up preheader to header to enable induction variable manipulation
   */
  IRBuilder<> entryBuilder(task->getEntry());
  auto temporaryBrToLoop = entryBuilder.CreateBr(headerClone);
  entryBuilder.SetInsertPoint(temporaryBrToLoop);

  /*
   * Generate PHI to track progress on the current chunk
   */
  auto chunkCounterType = task->chunkSizeArg->getType();
  auto chunkPHI = IVUtility::createChunkPHI(preheaderClone, headerClone, chunkCounterType, task->chunkSizeArg);

  /*
   * Collect clones of step size deriving values for all induction variables
   * of the top level loop
   */
  std::unordered_map<InductionVariable *, Value *> clonedStepSizeMap;
  for (auto ivInfo : allIVInfo->getInductionVariables(*loopSummary)) {
    Value *clonedStepValue = nullptr;
    if (ivInfo->getSingleComputedStepValue()) {
      clonedStepValue = fetchClone(ivInfo->getSingleComputedStepValue());
    } else {

      /*
       * The step size is a composite SCEV. Fetch its instruction expansion,
       * cloning into the entry block of the function
       * 
       * NOTE: The step size is expected to be loop invariant
       */
      auto expandedInsts = ivInfo->getComputationOfStepValue();
      assert(expandedInsts.size() > 0);
      for (auto expandedInst : expandedInsts) {
        auto clonedInst = expandedInst->clone();
        task->addInstruction(expandedInst, clonedInst);
        entryBuilder.Insert(clonedInst);
      }

      /*
       * Wire the instructions in the expansion to use the cloned values
       */
      for (auto expandedInst : expandedInsts) {
        adjustDataFlowToUseClones(task->getCloneOfOriginalInstruction(expandedInst), 0);
      }
      clonedStepValue = task->getCloneOfOriginalInstruction(expandedInsts.back());
    }

    clonedStepSizeMap.insert(std::make_pair(ivInfo, clonedStepValue));
  }

  /*
   * Determine start value of the IV for the task
   * core_start: original_start + original_step_size * core_id * chunk_size
   */
  for (auto ivInfo : allIVInfo->getInductionVariables(*loopSummary)) {
    auto startOfIV = fetchClone(ivInfo->getStartValue());
    auto stepOfIV = clonedStepSizeMap.at(ivInfo);
    auto ivPHI = cast<PHINode>(fetchClone(ivInfo->getLoopEntryPHI()));

    auto nthCoreOffset = entryBuilder.CreateMul(
      stepOfIV,
      entryBuilder.CreateZExtOrTrunc(
        entryBuilder.CreateMul(task->coreArg, task->chunkSizeArg, "coreIdx_X_chunkSize"),
        ivPHI->getType()
      ),
      "stepSize_X_coreIdx_X_chunkSize"
    );
    auto offsetStartValue = entryBuilder.CreateAdd(startOfIV, nthCoreOffset, "startPlusOffset");

    ivPHI->setIncomingValueForBlock(preheaderClone, offsetStartValue);
  }

  /*
   * Determine additional step size from the beginning of the next core's chunk
   * to the start of this core's next chunk
   * chunk_step_size: original_step_size * (num_cores - 1) * chunk_size
   */
  for (auto ivInfo : allIVInfo->getInductionVariables(*loopSummary)) {
    auto stepOfIV = clonedStepSizeMap.at(ivInfo);
    auto ivPHI = cast<PHINode>(fetchClone(ivInfo->getLoopEntryPHI()));

    auto onesValueForChunking = ConstantInt::get(chunkCounterType, 1);
    auto chunkStepSize = entryBuilder.CreateMul(
      stepOfIV,
      entryBuilder.CreateZExtOrTrunc(
        entryBuilder.CreateMul(
          entryBuilder.CreateSub(task->numCoresArg, onesValueForChunking, "numCoresMinus1"),
          task->chunkSizeArg,
          "numCoresMinus1_X_chunkSize"
        ),
        ivPHI->getType()
      ),
      "stepSizeToNextChunk"
    );

    IVUtility::chunkInductionVariablePHI(preheaderClone, ivPHI, chunkPHI, chunkStepSize);
  }

  /*
   * The exit condition needs to be made non-strict to catch iterating past it
   */
  auto loopGoverningIVAttr = LDI->getLoopGoverningIVAttribution();
  LoopGoverningIVUtility ivUtility(loopGoverningIVAttr->getInductionVariable(), *loopGoverningIVAttr);
  auto cmpInst = cast<CmpInst>(task->getCloneOfOriginalInstruction(loopGoverningIVAttr->getHeaderCmpInst()));
  auto brInst = cast<BranchInst>(task->getCloneOfOriginalInstruction(loopGoverningIVAttr->getHeaderBrInst()));
  ivUtility.updateConditionAndBranchToCatchIteratingPastExitValue(cmpInst, brInst, task->getLastBlock(0));
  auto updatedCmpInst = cmpInst;

  /*
   * The exit condition value does not need to be computed each iteration
   * and so the value's derivation can be hoisted into the preheader
   */
  auto exitConditionValue = fetchClone(loopGoverningIVAttr->getHeaderCmpInstConditionValue());
  if (auto exitConditionInst = dyn_cast<Instruction>(exitConditionValue)) {
    auto &derivation = ivUtility.getConditionValueDerivation();
    for (auto I : derivation) {
      auto cloneI = task->getCloneOfOriginalInstruction(I);
      cloneI->removeFromParent();
      entryBuilder.Insert(cloneI);
    }

    exitConditionInst->removeFromParent();
    entryBuilder.Insert(exitConditionInst);
  }

  /*
   * NOTE: When loop governing IV attribution allows for any bther instructions in the header
   * other than those of the IV and its comparison, those unrelated instructions should be
   * copied into the body and the exit block (to preserve the number of times they execute)
   * 
   * The logic in the exit block must be guarded so only the "last" iteration executes it,
   * not any cores that pass the last iteration. This is further complicated because the mapping
   * of live-out environment producing instructions might need to be updated with the peeled
   * instructions in the exit block
   * 
   * A temporary mitigation is to transform loop latches with conditional branches that
   * verify if the next iteration would ever occur. This still requires live outs to be propagated
   * from both the header and the latches
   */

  /*
   * Move any non-IV instructions in the header to the loop body and the header exit block
   */
  std::set<Instruction *> ivInstructions;
  auto sccdag = LDI->sccdagAttrs.getSCCDAG();
  for (auto ivInfo : allIVInfo->getInductionVariables(*loopSummary)) {
    for (auto I : ivInfo->getAllInstructions()) {
      ivInstructions.insert(task->getCloneOfOriginalInstruction(I));
    }
  }
  auto nonDOALLSCCs = LDI->sccdagAttrs.getSCCsWithLoopCarriedDataDependencies();
  for (auto scc : nonDOALLSCCs) {
    auto sccInfo = LDI->sccdagAttrs.getSCCAttrs(scc);
    if (!sccInfo->canExecuteReducibly()) continue;
    assert(sccInfo->getSingleHeaderPHI());
    ivInstructions.insert(task->getCloneOfOriginalInstruction(sccInfo->getSingleHeaderPHI()));
  }
  ivInstructions.insert(chunkPHI);
  ivInstructions.insert(cmpInst);
  ivInstructions.insert(brInst);

  bool requiresConditionBeforeEnteringHeader = false;
  for (auto &I : *headerClone) {
    if (ivInstructions.find(&I) == ivInstructions.end()) {
      assert(!isa<PHINode>(I)
        && "All PHIs (which have loop carried dependencies) must be chunked or reducible");
      requiresConditionBeforeEnteringHeader = true;
      break;
    }
  }

  if (requiresConditionBeforeEnteringHeader) {
    auto &loopGoverningIV = loopGoverningIVAttr->getInductionVariable();
    auto loopGoverningPHI = task->getCloneOfOriginalInstruction(loopGoverningIV.getLoopEntryPHI());
    auto stepSize = clonedStepSizeMap.at(&loopGoverningIV);

    /*
     * In each latch, assert that the previous iteration would have executed
     */
    for (auto latch : loopSummary->getLatches()) {
      BasicBlock *cloneLatch = task->getCloneOfOriginalBasicBlock(latch);
      // cloneLatch->print(errs() << "Addressing latch:\n");
      auto latchTerminator = cloneLatch->getTerminator();
      latchTerminator->eraseFromParent();
      IRBuilder<> latchBuilder(cloneLatch);

      auto currentIVValue = cast<PHINode>(loopGoverningPHI)->getIncomingValueForBlock(cloneLatch);
      auto prevIterationValue = latchBuilder.CreateSub(currentIVValue, stepSize);
      auto clonedCmpInst = updatedCmpInst->clone();
      clonedCmpInst->replaceUsesOfWith(loopGoverningPHI, prevIterationValue);
      latchBuilder.Insert(clonedCmpInst);
      latchBuilder.CreateCondBr(clonedCmpInst, task->getLastBlock(0), headerClone);
    }

    /*
     * In the preheader, assert that either the first iteration is being executed OR
     * that the previous iteration would have executed. The reason we must also check
     * if this is the first iteration is if the IV condition is such that <= 1
     * iteration would ever occur
     */
    auto preheaderTerminator = preheaderClone->getTerminator();
    preheaderTerminator->eraseFromParent();
    IRBuilder<> preheaderBuilder(preheaderClone);
    auto offsetStartValue = cast<PHINode>(loopGoverningPHI)->getIncomingValueForBlock(preheaderClone);
    auto prevIterationValue = preheaderBuilder.CreateSub(offsetStartValue, stepSize);

    auto clonedExitCmpInst = updatedCmpInst->clone();
    clonedExitCmpInst->replaceUsesOfWith(loopGoverningPHI, prevIterationValue);
    preheaderBuilder.Insert(clonedExitCmpInst);
    auto startValue = fetchClone(loopGoverningIV.getStartValue());
    auto isNotFirstIteration = preheaderBuilder.CreateICmpNE(offsetStartValue, startValue);
    preheaderBuilder.CreateCondBr(
      preheaderBuilder.CreateAnd(isNotFirstIteration, clonedExitCmpInst),
      task->getExit(),
      headerClone
    );
  }
}

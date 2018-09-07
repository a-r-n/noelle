#include "SCCDAGAttrs.hpp"

using namespace llvm;

AccumulatorOpInfo::AccumulatorOpInfo () {
  this->sideEffectFreeOps = {
    Instruction::Add,
    Instruction::FAdd,
    Instruction::Mul,
    Instruction::FMul,
    Instruction::Sub,
    Instruction::FSub
  };
  this->accumOps = std::set<unsigned>(sideEffectFreeOps.begin(), sideEffectFreeOps.end());
  this->opIdentities = {
    { Instruction::Add, 0 },
    { Instruction::FAdd, 0 },
    { Instruction::Mul, 1 },
    { Instruction::FMul, 1 },
    { Instruction::Sub, 0 },
    { Instruction::FSub, 0 }
  };
  this->equivAddOp = {
    { Instruction::Sub, Instruction::Add },
    { Instruction::FSub, Instruction::FAdd }
  };
}

bool AccumulatorOpInfo::isSubOp (unsigned op) {
  return Instruction::Sub == op || Instruction::FSub == op;
}

bool AccumulatorOpInfo::isMulOp (unsigned op) {
  return Instruction::Mul == op || Instruction::FMul == op;
}

bool AccumulatorOpInfo::isAddOp (unsigned op) {
  return Instruction::Add == op || Instruction::FAdd == op;
}

unsigned AccumulatorOpInfo::equivalentAddOp (unsigned subOp) {
  if (!isSubOp(subOp)) return subOp;
  return equivAddOp[subOp];
}

std::set<SCC *> SCCDAGAttrs::getSCCsWithLoopCarriedDataDependencies (void) const {
  std::set<SCC *> sccs;
  for (auto &sccInfoPair : this->sccToInfo) {
    if (!sccInfoPair.second->isIndependent) continue ;
    sccs.insert(sccInfoPair.first);
  }
  return sccs;
}

bool SCCDAGAttrs::loopHasInductionVariable () {
  
  /*
   * Assumption(angelo): An induction variable will be the root SCC of the loop
   */
  auto topLevelNodes = sccdag->getTopLevelNodes();
  if (topLevelNodes.size() != 1) return false;

  auto scc = (*topLevelNodes.begin())->getT();

  return isInductionVariableSCC(scc);
}

bool SCCDAGAttrs::isSCCContainedInSubloop (LoopInfoSummary &LIS, SCC *scc) const {
  bool instInSubloops = true;
  for (auto iNodePair : scc->internalNodePairs()) {
    auto bb = cast<Instruction>(iNodePair.first)->getParent();
    instInSubloops &= LIS.bbToLoop[bb] != LIS.topLoop;
  }
  return instInSubloops;
}

bool SCCDAGAttrs::checkIfInductionVariableSCC (SCC *scc, ScalarEvolution &SE) {
  auto isIvSCC = true;
  for (auto iNodePair : scc->internalNodePairs()) {
    auto V = iNodePair.first;
    auto canBePartOfSCC = isa<CmpInst>(V) || isa<TerminatorInst>(V);

    auto scev = SE.getSCEV(V);
    switch (scev->getSCEVType()) {
    case scConstant:
    case scTruncate:
    case scZeroExtend:
    case scSignExtend:
    case scSMaxExpr:
    case scUMaxExpr:
    case scUDivExpr:
    case scAddExpr:
    case scMulExpr:
    case scAddRecExpr:
      continue;
    case scUnknown:
    case scCouldNotCompute:
      isIvSCC &= canBePartOfSCC;
      continue;
    default:
     llvm_unreachable("DSWP: Unknown SCEV type!");
    }
  }

  return this->getSCCAttrs(scc)->isIVSCC = isIvSCC;
}

/*
 * Assumptions:
 * IV is described by single PHI with a start and recurrence incoming value
 * Only one accumulator, only one conditional branch and its comparison
 */
bool SCCDAGAttrs::checkIfSimpleIV (SCC *scc, LoopInfoSummary &LIS) {
  auto &sccInfo = this->getSCCAttrs(scc);
  if (!sccInfo->singlePHI) return false;
  if (sccInfo->singlePHI->getNumIncomingValues() != 2) return false;
  if (sccInfo->PHIAccumulators.size() != 1) return false;

  SimpleIVInfo IVInfo;
  for (auto iNodePair : scc->internalNodePairs()) {
    auto V = iNodePair.first;
    if (auto cmp = dyn_cast<CmpInst>(V)) {
      if (IVInfo.cmp) return false;
      IVInfo.cmp = cmp;
    } else if (auto br = dyn_cast<BranchInst>(V)) {
      if (br->isUnconditional()) continue;
      if (IVInfo.br) return false;
      IVInfo.br = br;
    }
  }
  if (!IVInfo.cmp || !IVInfo.br) return false;

  Instruction *accum = *sccInfo->PHIAccumulators.begin();
  auto incomingStart = sccInfo->singlePHI->getIncomingValue(0);
  if (incomingStart == accum) incomingStart = sccInfo->singlePHI->getIncomingValue(1);
  IVInfo.start = incomingStart;

  auto stepValue = accum->getOperand(0);
  if (stepValue == sccInfo->singlePHI) stepValue = accum->getOperand(1);
  if (!isa<ConstantInt>(stepValue)) return false;
  IVInfo.step = (ConstantInt*)stepValue;
  auto stepSize = IVInfo.step->getValue();
  if (stepSize != 1 && stepSize != -1) return false;

  if (!isa<ICmpInst>(IVInfo.cmp)) return false;
  auto cmpLHS = IVInfo.cmp->getOperand(0);
  unsigned cmpToInd = cmpLHS == sccInfo->singlePHI || cmpLHS == accum;
  IVInfo.cmpIVTo = IVInfo.cmp->getOperand(cmpToInd);
  IVInfo.isCmpOnAccum = IVInfo.cmp->getOperand((cmpToInd + 1) % 2) == accum;
  IVInfo.isCmpIVLHS = cmpToInd;

  if (!checkSimpleIVEndVal(IVInfo, LIS)) return false;

  sccInfo->simpleIVInfo = IVInfo;
  return sccInfo->isSimpleIV = true;
}

bool SCCDAGAttrs::checkSimpleIVEndVal (SimpleIVInfo &ivInfo, LoopInfoSummary &LIS) {
  std::set<BasicBlock *> &loopBBs = LIS.topLoop->bbs; 
  bool exitOnCmp = loopBBs.find(ivInfo.br->getSuccessor(0)) == loopBBs.end();
  auto signedPred = ivInfo.cmp->isUnsigned() ? ivInfo.cmp->getSignedPredicate() : ivInfo.cmp->getPredicate();
  signedPred = ivInfo.isCmpIVLHS ? signedPred : ICmpInst::getSwappedPredicate(signedPred);
  int stepSize = ivInfo.step->getValue().getSExtValue();

  auto cmpPredAbort = [&]() -> void {
    errs() << "SCCDAGAttrs:   Error: comparison and branch of top level IV misunderstood\n";
    abort();
  };

  errs() << "Is CMP IV on LHS: " << ivInfo.isCmpIVLHS << "\n";
  errs() << "Signed predicate: " << ICmpInst::getPredicateName(signedPred) << "\n";
  ivInfo.cmp->print(errs() << "Cmp: "); errs() << "\n";
  errs() << "Exit on cmp: " << exitOnCmp << "\n";
  errs() << "step size: " << stepSize << "\n";
  if (!exitOnCmp) { 
    if (stepSize == 1) {
      switch (signedPred) {
        case CmpInst::Predicate::ICMP_SLE:
          ivInfo.endOffset = 1;
        case CmpInst::Predicate::ICMP_NE:
        case CmpInst::Predicate::ICMP_SLT:
          break;
        default:
          cmpPredAbort();
      }
    } else {
      switch (signedPred) {
        case CmpInst::Predicate::ICMP_SGE:
          ivInfo.endOffset = -1;
        case CmpInst::Predicate::ICMP_NE:
        case CmpInst::Predicate::ICMP_SGT:
          break;
        default:
          cmpPredAbort();
      }
    }
  } else {
    if (stepSize == 1) {
      switch (signedPred) {
        case CmpInst::Predicate::ICMP_SGT:
          ivInfo.endOffset = 1;
        case CmpInst::Predicate::ICMP_SGE:
        case CmpInst::Predicate::ICMP_EQ:
          break;
        default:
          cmpPredAbort();
      }
    } else {
      switch (signedPred) {
        case CmpInst::Predicate::ICMP_SLT:
          ivInfo.endOffset = -1;
        case CmpInst::Predicate::ICMP_SLE:
        case CmpInst::Predicate::ICMP_EQ:
          break;
        default:
          cmpPredAbort();
      }
    }
  }

  ivInfo.endOffset -= stepSize * ivInfo.isCmpOnAccum;
  return true;
}

std::set<BasicBlock *> & SCCDAGAttrs::getBasicBlocks (SCC *scc){
  auto &sccInfo = this->sccToInfo[scc];

  return sccInfo->bbs;
}

bool SCCDAGAttrs::allPostLoopEnvValuesAreReducable (LoopEnvironment *env) const {
  for (auto envIndex : env->getPostEnvIndices()) {
    auto producer = env->producerAt(envIndex);
    auto scc = sccdag->sccOfValue(producer);

    if (scc->getType() != SCC::SCCType::COMMUTATIVE) {
      return false;
    }
  }
  return true;
}

// REFACTOR(angelo): find better workaround than just a getter for SCCAttrs
std::unique_ptr<SCCAttrs> & SCCDAGAttrs::getSCCAttrs (SCC *scc){
  return this->sccToInfo[scc];
}

void SCCDAGAttrs::populate (SCCDAG *loopSCCDAG, LoopInfoSummary &LIS, ScalarEvolution &SE) {
  this->sccdag = loopSCCDAG;
  for (auto node : loopSCCDAG->getNodes()) {
    auto scc = node->getT();
    this->sccToInfo[scc] = std::move(std::make_unique<SCCAttrs>(scc));

    this->collectSinglePHIAndAccumulators(scc);
    this->checkIfInductionVariableSCC(scc, SE);
    if (isInductionVariableSCC(scc)) this->checkIfSimpleIV(scc, LIS);
    this->checkIfClonable(scc, SE);

    if (this->checkIfIndependent(scc)) {
      scc->setType(SCC::SCCType::INDEPENDENT);
    } else if (this->checkIfCommutative(scc)) {
      scc->setType(SCC::SCCType::COMMUTATIVE);
    } else {
      scc->setType(SCC::SCCType::SEQUENTIAL);
    }
  }
}

void SCCDAGAttrs::collectSinglePHIAndAccumulators (SCC *scc) {
  PHINode *singlePHI = nullptr;
  std::set<Instruction *> accums;
  for (auto iNodePair : scc->internalNodePairs()) {
    auto V = iNodePair.first;
    if (isa<CmpInst>(V) || isa<TerminatorInst>(V)) continue ;
    if (isa<GetElementPtrInst>(V) || isa<CastInst>(V)) continue ;

    if (auto phi = dyn_cast<PHINode>(V)) {
      if (singlePHI) return ;
      singlePHI = phi;
      continue;
    }
    if (auto I = dyn_cast<Instruction>(V)) {
      auto binOp = I->getOpcode();
      if (accumOpInfo.accumOps.find(binOp) != accumOpInfo.accumOps.end()) {
        accums.insert(I);
      }
      continue;
    }

    return ;
  }

  auto &sccInfo = this->getSCCAttrs(scc);
  sccInfo->singlePHI = singlePHI;
  sccInfo->PHIAccumulators = std::set<Instruction *>(accums.begin(), accums.end());
}

bool SCCDAGAttrs::checkIfCommutative (SCC *scc) {

  /*
   * Requirement: SCC has no dependent SCCs
   */
  errs() << "Checking for dependent SCCs of SCC:\n";
  scc->print(errs()) << "\n";
  for (auto iNodePair : scc->externalNodePairs()) {
    if (iNodePair.second->numIncomingEdges() > 0) {
      return false;
    }
  }
  errs() << "------------------------------- Phew, isn't dependent\n";

  auto &sccInfo = this->getSCCAttrs(scc);
  if (!sccInfo->singlePHI) return false;

  // for (auto iNodePair : scc->internalNodePairs()) {
    // Instruction *val = cast<Instruction>(iNodePair.first);
  if (sccInfo->PHIAccumulators.size() == 0) return false;
  for (auto accum : sccInfo->PHIAccumulators) {
    accum->print(errs() << "Checking accum: "); errs() << "\n";

    /*
     * Requirement: instructions are side effect free
     */
    unsigned opCode = accum->getOpcode();
    if (accumOpInfo.sideEffectFreeOps.find(opCode) == accumOpInfo.sideEffectFreeOps.end()) {
      return false;
    }

    /*
     * Requirement: commutative instruction has one use and is used once
     */
    auto iNode = scc->fetchNode(accum);
    if (iNode->numIncomingEdges() != 1 || iNode->numOutgoingEdges() != 1) {
      return false;
    }
  }

  /*
   * Requirement: instructions are all Add/Sub or all Mul
   */
  auto firstAccum = *sccInfo->PHIAccumulators.begin();
  bool isFirstMul = accumOpInfo.isMulOp(firstAccum->getOpcode());
  for (auto accum : sccInfo->PHIAccumulators) {
    bool isMul = accumOpInfo.isMulOp(accum->getOpcode());
    if (isMul ^ isFirstMul) return false;

    /*
     * Requirement: second operand of subtraction must be external
     */
    if (accumOpInfo.isSubOp(accum->getOpcode())) {
      if (scc->isInternal(accum->getOperand(1))) return false;
    }
  }

  errs() << "SUCCESS THIS IS A COMMUTATIVE SCC!\n";
  return this->getSCCAttrs(scc)->isReducable = true;
}

bool SCCDAGAttrs::checkIfIndependent (SCC *scc) {

  /*
   * The SCC is independent if it doesn't have loop carried data dependencies
   */
  return this->getSCCAttrs(scc)->isIndependent = scc->hasCycle(/*ignoreControlDep=*/false);
}

void SCCDAGAttrs::checkIfClonable (SCC *scc, ScalarEvolution &SE) {
  if (checkIfClonableByInductionVars(scc) ||
      checkIfClonableBySyntacticSugarInstrs(scc)) {
    this->getSCCAttrs(scc)->isClonable = true;
    clonableSCCs.insert(scc);
  }
}

bool SCCDAGAttrs::checkIfClonableByInductionVars (SCC *scc) {

  /*
   * Check if the current node of the SCCDAG is an SCC used by other nodes.
   */
  if (scc->numInternalNodes() == 1 || sccdag->fetchNode(scc)->numOutgoingEdges() == 0) {
    return false;
  }

  /*
   * The current node of the SCCDAG is an SCC.
   *
   * Check if this SCC can be removed exploiting induction variables.
   * In more detail, this SCC can be removed if the loop-carried data dependence, which has created this SCC in the PDG, is due to updates to induction variables.
   */
  if (this->isInductionVariableSCC(scc)) {
    return true;
  }

  return false;
}

bool SCCDAGAttrs::checkIfClonableBySyntacticSugarInstrs (SCC *scc) {

  /*
   * Check if the current node of the SCCDAG is an SCC used by other nodes.
   */
  if (scc->numInternalNodes() > 1 || sccdag->fetchNode(scc)->numOutgoingEdges() == 0) {
    return false;
  }

  auto I = scc->begin_internal_node_map()->first;
  if (isa<PHINode>(I) || isa<GetElementPtrInst>(I) || isa<CastInst>(I)) {
    return true;
  }

  return false;
}

bool SCCDAGAttrs::executesCommutatively (SCC *scc) {
  return this->getSCCAttrs(scc)->isReducable;
}

bool SCCDAGAttrs::executesIndependently (SCC *scc) {
  return this->getSCCAttrs(scc)->isIndependent;
}

bool SCCDAGAttrs::canBeCloned (SCC *scc) {
  return this->getSCCAttrs(scc)->isClonable;
}

bool SCCDAGAttrs::isInductionVariableSCC (SCC *scc) {
  return this->getSCCAttrs(scc)->isIVSCC;
}

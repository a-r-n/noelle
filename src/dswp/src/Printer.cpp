/*
 * Copyright 2016 - 2019  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "SCCDAG.hpp"
#include "DSWP.hpp"

void llvm::DSWP::printStageSCCs (LoopDependenceInfo *LDI) const {
  if (this->verbose == Verbosity::Disabled) {
    return ;
  }

  errs() << "DSWP:  Pipeline stages\n";
  for (auto techniqueTask : this->tasks) {
    auto task = (DSWPTask *)techniqueTask;
    errs() << "DSWP:    Stage: " << task->order << "\n";
    for (auto scc : task->stageSCCs) {
      scc->print(errs(), "DSWP:     ", /*maxEdges=*/15);
      errs() << "DSWP:    \n" ;
    }
  }

  return ;
}

void llvm::DSWP::printStageQueues (LoopDependenceInfo *LDI) const {

  /*
   * Check if we should print.
   */
  if (this->verbose == Verbosity::Disabled) {
    return ;
  }

  /*
   * Print the IDs of the queues.
   */
  errs() << "DSWP:  Queues that connect the pipeline stages\n";
  for (auto techniqueTask : this->tasks) {
    auto task = (DSWPTask *)techniqueTask;
    errs() << "DSWP:    Stage: " << task->order << "\n";

    errs() << "DSWP:      Push value queues: ";
    for (auto qInd : task->pushValueQueues) {
      errs() << qInd << " ";
    }
    errs() << "\n" ;

    errs() << "DSWP:      Pop value queues: ";
    for (auto qInd : task->popValueQueues) {
      errs() << qInd << " ";
    }
    errs() << "\n";
  }

  /*
   * Print the queues.
   */
  int count = 0;
  for (auto &queue : this->queues) {
    errs() << "DSWP:    Queue: " << count++ << "\n";
    queue->producer->print(errs() << "DSWP:     Producer:\t"); errs() << "\n";
    for (auto consumer : queue->consumers) {
      consumer->print(errs() << "DSWP:     Consumer:\t"); errs() << "\n";
    }
  }

  return ;
}

void llvm::DSWP::printEnv (LoopDependenceInfo *LDI) const {

  /*
   * Check if we should print.
   */
  if (this->verbose == Verbosity::Disabled){
    return ;
  }

  /*
   * Print the environment.
   */
  errs() << "DSWP:  Environment\n";
  int count = 1;
  for (auto envIndex : LDI->environment->getEnvIndicesOfLiveInVars()) {
    LDI->environment->producerAt(envIndex)->print(errs()
      << "DSWP:    Pre loop env " << count++ << ", producer:\t");
    errs() << "\n";
  }
  for (auto envIndex : LDI->environment->getEnvIndicesOfLiveOutVars()) {
    LDI->environment->producerAt(envIndex)->print(errs()
      << "DSWP:    Post loop env " << count++ << ", producer:\t");
    errs() << "\n";
  }

  return ;
}

void llvm::DSWP::writeStageGraphsAsDot (const LoopDependenceInfo &LDI) const {

  DG<DGString> stageGraph;
  std::set<DGString *> elements;

  auto addNode = [&](std::string val) -> DGNode<DGString> * {
    auto element = new DGString(val);
    elements.insert(element);
    return stageGraph.addNode(element, true);
  };

  for (auto techniqueTask : this->tasks) {

    /*
     * Produce node indicating the task index that owns the following SCCs
     */
    auto task = (DSWPTask *)techniqueTask;
    auto headerNode = addNode("Stage: " + std::to_string(task->order));

    for (auto scc : task->stageSCCs) {
      std::string sccDescription;
      raw_string_ostream ros(sccDescription);
      scc->printMinimal(ros, "");
      ros.flush();
      auto sccNode = addNode(sccDescription);
      stageGraph.addEdge(headerNode->getT(), sccNode->getT());
    }
  }

  DGPrinter::writeGraph<DG<DGString>>("dswpStagesForLoop_" + std::to_string(LDI.getID()) + ".dot", &stageGraph);
  for (auto elem : elements) delete elem;
}

void llvm::DSWP::writeStageQueuesAsDot (const LoopDependenceInfo &LDI) const {

  DG<DGString> queueGraph;
  std::set<DGString *> elements;

  /*
   * Add a stage's queue producer or consumer as a node to the graph
   */
  auto addNode = [&](int stageIndex, Instruction *I) -> DGNode<DGString> * {
    std::string queueDesc;
    raw_string_ostream ros(queueDesc);
    I->print(ros << "Stage: " + std::to_string(stageIndex) << "\n");
    ros.flush();
    auto element = new DGString(queueDesc);
    elements.insert(element);
    return queueGraph.addNode(element, true);
  };

  for (auto &queue : this->queues) {
    auto producerNode = addNode(queue->fromStage, queue->producer);
    for (auto consumerI : queue->consumers) {
      auto consumerNode = addNode(queue->toStage, consumerI);
      queueGraph.addEdge(producerNode->getT(), consumerNode->getT());
    }
  }

  DGPrinter::writeGraph<DG<DGString>>("dswpQueuesForLoop_" + std::to_string(LDI.getID()) + ".dot", &queueGraph);
  for (auto elem : elements) delete elem;
}
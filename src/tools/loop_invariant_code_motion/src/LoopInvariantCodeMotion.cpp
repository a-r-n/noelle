/*
 * Copyright 2019 - 2020  Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "LoopInvariantCodeMotion.hpp"
#include "Mem2RegNonAlloca.hpp"

using namespace llvm;
using namespace llvm::noelle;

LoopInvariantCodeMotion::LoopInvariantCodeMotion(Noelle &noelle)
  : noelle{noelle}
  {
  return ;
}

bool LoopInvariantCodeMotion::promoteMemoryLocationsToRegisters (
  LoopDependenceInfo const &LDI
  ){
  Mem2RegNonAlloca mem2Reg(LDI, this->noelle);

  auto result = mem2Reg.promoteMemoryToRegister();

  return result;
}

bool LoopInvariantCodeMotion::extractInvariantsFromLoop (
  LoopDependenceInfo const &LDI
  ){
  
  if (hoistInvariantValues(LDI)) {
    return true;
  }

  Mem2RegNonAlloca mem2Reg(LDI, noelle);
  if (mem2Reg.promoteMemoryToRegister()) {
    return true;
  }

  return false;
}

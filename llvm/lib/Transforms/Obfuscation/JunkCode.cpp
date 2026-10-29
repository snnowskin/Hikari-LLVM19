//===- JunkCode.cpp - 花指令 pass -------------------------------------===//
// 文档对应 §4.2。三种形态：J1 内嵌数据岛（纯字节 .word，切碎模式匹配窗口）、
// J1b udf 陷阱（坑动态扫描与 JIT）、J3 假栈帧（volatile 栈槽干扰变量恢复）。
// 硬约束：数据岛不得含重定位，只能用 .word；天然 4 字节对齐。
//===----------------------------------------------------------------------===//
#include "ObfCommon.h"

using namespace llvm;

namespace obf {

cl::opt<int> JunkProb("obf-junk-prob",
                      cl::desc("JunkCode: 每基本块命中概率(%)"), cl::init(25));

/// 生成一段确定性的伪随机字节序列（作为数据岛内容）。
/// 关键：必须由种子决定，保证同版本可复现（CI diff 才能收敛）。
static SmallVector<uint32_t, 32> makeIslandBytes(uint64_t &S, unsigned NWords) {
  SmallVector<uint32_t, 32> Out;
  Out.reserve(NWords);
  for (unsigned I = 0; I < NWords; ++I)
    Out.push_back(static_cast<uint32_t>(splitmix64(S)));
  return Out;
}

/// 把一段 .word 常量编成一条 InlineAsm。
/// 用 InlineAsm 而不是手拼字节：让 LLVM 负责对齐与节放置，
/// 我们只管内容（见文档 §4.2 末尾的复用建议）。
static InlineAsm *makeIslandAsm(Module &M, ArrayRef<uint32_t> Words) {
  LLVMContext &C = M.getContext();
  std::string Asm;
  raw_string_ostream OS(Asm);
  OS << ".p2align 2\n";
  for (uint32_t W : Words) {
    OS << ".word 0x";
    OS.write_hex(W);
    OS << "\n";
  }
  OS.flush();

  FunctionType *FT = FunctionType::get(Type::getVoidTy(C), false);
  return makeAsm(M, FT, Asm, "~{memory}", /*HasSideEffects=*/true);
}

/// J1：在 BB 之后插入数据岛块。
/// 布局（codegen 后）：
///     <BB 尾部条件分支>          ; 恒假，依赖 volatile 种子槽
///     .p2align 2
///     .word ...  × N             ; ← 数据岛
///     b .Lreal                   ; 跳板
///   .Lreal: <原后继>
static void insertDataIsland(Function &F, BasicBlock *BB, uint64_t &S,
                             unsigned NWords) {
  Module &M = *F.getParent();
  LLVMContext &C = M.getContext();
  Instruction *Term = BB->getTerminator();
  if (!Term || isa<ReturnInst>(Term))
    return; // 返回点不插：避免影响尾声布局

  BasicBlock *Real = Term->getSuccessor(0);
  if (!Real || Real == BB || Real->getParent() != &F || Real->isEHPad())
    return;

  auto *Island = BasicBlock::Create(C, "obf.island", &F, Real);
  IRBuilder<> IB(Island);
  IB.CreateCall(makeIslandAsm(M, makeIslandBytes(S, NWords)), {});
  IB.CreateBr(Real);

  // 把原终结符改成「条件跳 island / 跳 real」，条件恒假但优化器证不出来。
  IRBuilder<> TB(Term);
  Value *Pred = makeOpaquePredicate(M, TB, S, static_cast<unsigned>(S >> 7));
  Value *NotPred = TB.CreateNot(Pred); // NotPred 恒假（Pred 恒真）
  BranchInst *BI = BranchInst::Create(Island, Real, NotPred, Term);
  Term->eraseFromParent();
  (void)BI;
}

/// J1b：udf 陷阱。放在一个恒不可达的块里，专坑 Frida Memory.scan 与 JIT。
static void insertUdfTrap(Function &F, BasicBlock *BB, uint64_t &S) {
  Module &M = *F.getParent();
  LLVMContext &C = M.getContext();
  Instruction *Term = BB->getTerminator();
  if (!Term || isa<ReturnInst>(Term))
    return;

  auto *Trap = BasicBlock::Create(C, "obf.udf", &F);
  IRBuilder<> IB(Trap);
  FunctionType *FT = FunctionType::get(Type::getVoidTy(C), false);
  IB.CreateCall(makeAsm(M, FT, "udf #0\n\tbrk #0", "~{memory}", true), {});
  IB.CreateUnreachable();

  // 恒假分支进入 trap（Pred 恒真 → 走 Real）
  IRBuilder<> TB(Term);
  Value *Pred = makeOpaquePredicate(M, TB, S, static_cast<unsigned>(S >> 11));
  BasicBlock *Real = Term->getSuccessor(0);
  BranchInst::Create(Real, Trap, Pred, Term);
  Term->eraseFromParent();
}

/// J3：假栈帧。插一个永不读的 volatile 槽，让栈帧虚高、局部变量恢复变脏。
static void insertFakeFrame(Function &F, BasicBlock *BB, uint64_t &S) {
  Module &M = *F.getParent();
  auto *I64 = Type::getInt64Ty(M.getContext());
  IRBuilder<> IB(BB->getFirstNonPHIOrDbgOrAlloca());
  Value *Slot = IB.CreateAlloca(I64, nullptr, "obf.ghost");
  StoreInst *St =
      IB.CreateStore(ConstantInt::get(I64, splitmix64(S)), Slot, true);
  // alloca 被 volatile store 碰过就不会被 SROA 提升；再补一条 sideeffect
  // 是双重保险（volatile 只挡优化器，不挡 SROA，见文档 §8.15）。
  emitSideEffect(M, St->getNextNode());
}

bool processJunkCode(Function &F) {
  if (ObfLevel == 0 || JunkProb == 0)
    return false;
  if (F.isDeclaration() || isNoObf(F))
    return false;
  // 必须已优化过：-O0 下块太少，插了等于白做（文档 §8.10）
  if (F.hasOptNone())
    return false;

  Module &M = *F.getParent();
  uint64_t S = funcSeed(M, F);

  SmallVector<BasicBlock *, 64> Blocks;
  for (BasicBlock &BB : F)
    if (!BB.isEHPad() && !BB.isLandingPad())
      Blocks.push_back(&BB);

  bool Changed = false;
  for (BasicBlock *BB : Blocks) {
    if (!rollChance(S, levelScale(JunkProb)))
      continue;

    switch (splitmix64(S) % 3) {
    case 0:
      if (ObfLevel >= 3) { // L3 才上数据岛（体积换强度）
        insertDataIsland(F, BB, S, 4 + static_cast<unsigned>(splitmix64(S) % 60));
        Changed = true;
      }
      break;
    case 1:
      if (ObfLevel >= 3) {
        insertUdfTrap(F, BB, S);
        Changed = true;
      }
      break;
    default:
      insertFakeFrame(F, BB, S);
      Changed = true;
      break;
    }
    // 每块插完立刻断链：Block 列表可能已失效（Island 是新块，不在快照里，安全）
  }

  if (!Changed)
    return false;
  return true;
}


// ── new-PM 包装（独立插件模式用；树内 legacy 调度直接调 processJunkCode） ──
PreservedAnalyses runJunkCode(Function &F, FunctionAnalysisManager &) {
  return processJunkCode(F) ? PreservedAnalyses::none()
                            : PreservedAnalyses::all();
}

} // namespace obf

//===- StackIndirectJump.cpp - 堆栈式间接跳转（四层，变态档 v2）=======//
// 文档对应 §4.5。L1 全 CFG 栈化 + 置换表 indirectbr；L2/L3 ret-sled 借 LR 回跳；
// L4 跨函数栈接力。v1 三个 bug 已修，勿回退：
//   B1 表项存「地址+随机偏移」而解码只做异或 → 落到错误地址必崩
//   B2 解码出的 i64 直接喂 indirectbr（操作数必须是 ptr，需 inttoptr）
//   B3 只改单后继边，cond br 原样保留，强度减半
//===----------------------------------------------------------------------===//
#include "ObfCommon.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Transforms/Utils/Local.h"

#include <optional>
#include <vector>

using namespace llvm;

namespace obf {

cl::opt<int> SjiLevel("obf-sji", cl::desc("StackIndirectJump 层级上限(0..4)"),
                      cl::init(0));
cl::opt<int> SjiRatio("obf-sji-ratio",
                      cl::desc("允许上 SJI 的函数百分比(0..100)"), cl::init(100));

static constexpr unsigned kSMCap = 64; // 每次转移压 2 弹 2，64 格绰绰有余

/// 该函数能不能上 SJI。宁可少开，不要崩（文档 §11 的核心教训）。
static bool sjiEligible(const Function &F) {
  if (F.isDeclaration() || F.hasOptNone() || isNoObf(F))
    return false;
  if (F.hasPersonalityFn()) // 有异常人格 = 可能带 cleanup
    return false;
  if (!F.hasFnAttribute(Attribute::NoUnwind) && F.needsUnwindTableEntry())
    return false; // 需要 unwind 信息的跳过
  if (F.hasFnAttribute(Attribute::Naked))
    return false;

  for (const BasicBlock &BB : F) {
    if (BB.isEHPad() || BB.isLandingPad())
      return false;
    for (const Instruction &I : BB) {
      if (isa<InvokeInst>(I) || isa<CallBrInst>(I))
        return false;
      if (const auto *CI = dyn_cast<CallInst>(&I))
        if (CI->isMustTailCall())
          return false;
      // 动态长度 alloca 与手改 sp 冲突
      if (isa<AllocaInst>(I) && !isa<ConstantInt>(
                                    cast<AllocaInst>(&I)->getArraySize()))
        return false;
    }
  }
  return true;
}

/// 建一个 i64 数组全局（放 .data.obf，运行期可写但值必须与我们编码一致）。
static GlobalVariable *makeTable(Module &M, ArrayRef<Constant *> Items,
                                 StringRef Name) {
  auto *I64 = Type::getInt64Ty(M.getContext());
  auto *Ty = ArrayType::get(I64, Items.size());
  auto *GV = new GlobalVariable(M, Ty, /*isConstant=*/false,
                                GlobalValue::InternalLinkage,
                                ConstantArray::get(Ty, Items), Name);
  GV->setSection(".data.obf");
  GV->setAlignment(Align(8));
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  return GV;
}

// ── L1：IR 级 stack machine（v2） ──
static bool buildStackMachine(Function &F, uint64_t &S) {
  Module &M = *F.getParent();
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *PtrTy = PointerType::getUnqual(C);

  SmallVector<BasicBlock *, 128> Blocks;
  for (BasicBlock &BB : F)
    if (!BB.isEHPad())
      Blocks.push_back(&BB);
  const unsigned N = static_cast<unsigned>(Blocks.size());
  if (N < 8)
    return false; // 小函数做完只剩开销（文档 §11 分级）

  // 块位置表
  DenseMap<const BasicBlock *, unsigned> Pos;
  for (unsigned I = 0; I < N; ++I)
    Pos[Blocks[I]] = I;

  // Cells：2 的幂，掩码取低位
  unsigned Cells = 1;
  while (Cells < N)
    Cells <<= 1;
  const unsigned Mask = Cells - 1;

    // 1. 置换表 perm：cell → 真实表项，Fisher-Yates 打乱
  std::vector<unsigned> Perm(Cells);
  for (unsigned I = 0; I < Cells; ++I)
    Perm[I] = I % N;
  for (unsigned I = Cells; I > 1; --I) {
    unsigned J = static_cast<unsigned>(splitmix64(S) % I);
    std::swap(Perm[I - 1], Perm[J]);
  }
  // 逆置换：块 p 对应哪个 cell
  std::vector<unsigned> InvCell(N, 0);
  for (unsigned I = 0; I < Cells; ++I)
    InvCell[Perm[I]] = I;

    // 2. 三张表：cell key / 目标地址 / 目标 key
  std::vector<Constant *> KCell, Tab, BKey;
  std::vector<uint64_t> BlockKey(N);
  for (unsigned I = 0; I < N; ++I)
    BlockKey[I] = splitmix64(S);

  for (unsigned I = 0; I < Cells; ++I)
    KCell.push_back(ConstantInt::get(I64, splitmix64(S)));

  for (unsigned P = 0; P < N; ++P) {
    Constant *Addr = ConstantExpr::getPtrToInt(BlockAddress::get(&F, Blocks[P]),
                                               I64);
    Tab.push_back(ConstantExpr::getXor(Addr, ConstantInt::get(I64, BlockKey[P])));
    BKey.push_back(ConstantInt::get(I64, BlockKey[P]));
  }

  GlobalVariable *KTab = makeTable(M, KCell, "obf.sm.kc");
  GlobalVariable *AddrTab = makeTable(M, Tab, "obf.sm.ad");
  GlobalVariable *KeyTab = makeTable(M, BKey, "obf.sm.bk");
  GlobalVariable *PermTab = makeTable(
      M,
      [&] {
        std::vector<Constant *> V;
        for (unsigned I = 0; I < Cells; ++I)
          V.push_back(ConstantInt::get(I64, Perm[I]));
        return V;
      }(),
      "obf.sm.pm");

    // 3. 栈：alloca + volatile sp，并「逃逸」一次阻 SROA
  IRBuilder<> EB(&*F.getEntryBlock().getFirstInsertionPt());
  auto *StackTy = ArrayType::get(I64, kSMCap);
  Value *Stack = EB.CreateAlloca(StackTy, nullptr, "obf.sm.stk");
  Value *Sp = EB.CreateAlloca(I64, nullptr, "obf.sm.sp");
  EB.CreateStore(ConstantInt::get(I64, 0), Sp, /*isVolatile=*/true);

  // 把栈指针写进一个 volatile 全局槽：SROA / 内存提升看到「指针逃逸」就收手，
  // 否则整套栈会被提升成寄存器，L1 立刻退化成普通 switch（一点强度都没有）。
  GlobalVariable *Esc = M.getGlobalVariable("obf.sm.esc");
  if (!Esc) {
    Esc = new GlobalVariable(M, PtrTy, /*isConstant=*/false,
                             GlobalValue::InternalLinkage,
                             ConstantPointerNull::get(PtrTy), "obf.sm.esc");
    Esc->setSection(".data.obf");
  }
  EB.CreateStore(Stack, Esc, /*isVolatile=*/true);

    // 4. 分发器
  BasicBlock *Disp = BasicBlock::Create(C, "obf.sm.disp", &F);
  {
    IRBuilder<> DB(Disp);
    Value *SpP = DB.CreateInBoundsGEP(I64, Sp, {DB.getInt64(0)}, "obf.sm.spp");
    Value *Cur = DB.CreateLoad(I64, SpP, /*isVolatile=*/true, "obf.sm.d");

    // 弹出 key
    Value *S1 = DB.CreateSub(Cur, ConstantInt::get(I64, 1));
    Value *Kp = DB.CreateInBoundsGEP(StackTy, Stack, {DB.getInt64(0), S1});
    Value *Kc = DB.CreateLoad(I64, Kp, /*isVolatile=*/true, "obf.sm.kc");

    // 弹出 slot
    Value *S2 = DB.CreateSub(S1, ConstantInt::get(I64, 1));
    Value *SlotP = DB.CreateInBoundsGEP(StackTy, Stack, {DB.getInt64(0), S2});
    Value *Slot = DB.CreateLoad(I64, SlotP, /*isVolatile=*/true, "obf.sm.sl");
    DB.CreateStore(S2, SpP, /*isVolatile=*/true);

    // cell = (slot ^ kc) & MASK → p = perm[cell]
    Value *Cell = DB.CreateAnd(DB.CreateXor(Slot, Kc),
                               ConstantInt::get(I64, Mask), "obf.sm.cell");
    auto *PermTy = cast<ArrayType>(PermTab->getValueType());
    Value *PermP = DB.CreateInBoundsGEP(PermTy, PermTab,
                                        {DB.getInt64(0), Cell});
    Value *P = DB.CreateLoad(I64, PermP, /*isVolatile=*/true, "obf.sm.p");

    // 地址 = tab[p] ^ bkey[p]
    auto *TabTy = cast<ArrayType>(AddrTab->getValueType());
    auto *BkTy = cast<ArrayType>(KeyTab->getValueType());
    Value *TabP = DB.CreateInBoundsGEP(TabTy, AddrTab, {DB.getInt64(0), P});
    Value *Enc = DB.CreateLoad(I64, TabP, /*isVolatile=*/true, "obf.sm.e");
    Value *BkP = DB.CreateInBoundsGEP(BkTy, KeyTab, {DB.getInt64(0), P});
    Value *Bk = DB.CreateLoad(I64, BkP, /*isVolatile=*/true, "obf.sm.bk");
    Value *AddrV = DB.CreateXor(Enc, Bk, "obf.sm.addr");
    Value *Addr = DB.CreateIntToPtr(AddrV, PtrTy, "obf.sm.ap");

    auto *IB = IndirectBrInst::Create(Addr, N, Disp);
    for (BasicBlock *BB : Blocks)
      IB->addDestination(BB);
  }

    // 5. 把所有 1~2 后继的边改成 push + br dispatcher
  bool Changed = false;
  for (BasicBlock *BB : Blocks) {
    Instruction *T = BB->getTerminator();
    if (!T)
      continue;
    if (isa<IndirectBrInst>(T) || isa<CallBrInst>(T) || isa<InvokeInst>(T) ||
        isa<ReturnInst>(T) || isa<UnreachableInst>(T) || isa<ResumeInst>(T))
      continue;
    unsigned NumS = T->getNumSuccessors();
    if (NumS == 0 || NumS > 2)
      continue; // switch 保原样：它的目标块自成体系，不经过栈机

    IRBuilder<> TB(T);
    Value *CellV = nullptr;
    if (NumS == 1) {
      unsigned P = Pos[T->getSuccessor(0)];
      CellV = ConstantInt::get(I64, InvCell[P]);
    } else {
      auto *BI = dyn_cast<BranchInst>(T);
      if (!BI || !BI->isConditional())
        continue;
      unsigned P0 = Pos[BI->getSuccessor(0)];
      unsigned P1 = Pos[BI->getSuccessor(1)];
      CellV = TB.CreateSelect(BI->getCondition(),
                              ConstantInt::get(I64, InvCell[P1]),
                              ConstantInt::get(I64, InvCell[P0]), "obf.sm.c");
    }

    auto *KcTy = cast<ArrayType>(KTab->getValueType());
    Value *KcP = TB.CreateInBoundsGEP(KcTy, KTab, {TB.getInt64(0), CellV});
    Value *Kcv = TB.CreateLoad(I64, KcP, /*isVolatile=*/true, "obf.sm.kv");
    Value *SlotV = TB.CreateXor(CellV, Kcv, "obf.sm.sv");

    Value *SpP = TB.CreateInBoundsGEP(I64, Sp, {TB.getInt64(0)});
    Value *Cur = TB.CreateLoad(I64, SpP, /*isVolatile=*/true, "obf.sm.cur");
    Value *P0P = TB.CreateInBoundsGEP(StackTy, Stack, {TB.getInt64(0), Cur});
    TB.CreateStore(SlotV, P0P, /*isVolatile=*/true);
    Value *S1 = TB.CreateAdd(Cur, ConstantInt::get(I64, 1));
    Value *P1P = TB.CreateInBoundsGEP(StackTy, Stack, {TB.getInt64(0), S1});
    TB.CreateStore(Kcv, P1P, /*isVolatile=*/true);
    Value *S2 = TB.CreateAdd(S1, ConstantInt::get(I64, 1));
    TB.CreateStore(S2, SpP, /*isVolatile=*/true);

    BranchInst::Create(Disp, T);
    T->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

// ── L2 / L3：ret-sled 借 LR 回跳 ──
static bool emitRetSled(BasicBlock *BB, BasicBlock *Target, bool UseTLS) {
  Function *F = BB->getParent();
  Module &M = *F->getParent();
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *P0 = PointerType::getUnqual(C);

  Instruction *Term = BB->getTerminator();
  if (!Term || Term->getNumSuccessors() != 1)
    return false;

  Constant *Tgt = BlockAddress::get(F, Target);

  FunctionType *FT = FunctionType::get(Type::getVoidTy(C), {P0, I64}, false);
  const char *AsmText =
      UseTLS ? "mrs $2, tpidr_el0\n\t"
               "str $0, [$2, $1]\n\t"
               "ldr x30, [$2, $1]\n\t"
               "ret"
             : "sub sp, sp, #16\n\t"
               "str $0, [sp]\n\t"
               "ldp x29, x30, [sp], #16\n\t"
               "ret";

  InlineAsm *IA = makeAsm(M, FT, AsmText, "r,r,r,~{x29},~{x30},~{memory}",
                          /*HasSideEffects=*/true);

  IRBuilder<> B(Term);
  Value *Off = ConstantInt::get(I64, UseTLS ? 0x08 : 0x00);
  SmallVector<Value *, 2> AsmArgs{Tgt, Off};
  B.CreateCall(FT, IA, AsmArgs, "");
  B.CreateUnreachable();
  Term->eraseFromParent();
  return true;
}

// ── L4：跨调用接力 ──
//  在部分调用点前后，把栈指针经一个 volatile 全局往返一趟：
//      before: %e = load sp ^ K ; store %e -> @obf.sm.rly
//      after : %v = load @obf.sm.rly ^ K ; store %v -> sp
//  值不变，语义不变，但「栈机状态跨函数调用延续」在 IR 上成了既有事实 ——
//  符号执行与数据流分析都必须把这个全局纳入模型，否则跨调用的栈深度就推错。
static bool emitCrossCallRelay(Function &F, uint64_t &S) {
  Module &M = *F.getParent();
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);

  GlobalVariable *Sp = M.getGlobalVariable("obf.sm.sp");
  if (!Sp)
    return false; // 只有 L1 跑过才有意义

  GlobalVariable *Rly = M.getGlobalVariable("obf.sm.rly");
  if (!Rly) {
    Rly = new GlobalVariable(M, I64, /*isConstant=*/false,
                             GlobalValue::InternalLinkage,
                             ConstantInt::get(I64, 0), "obf.sm.rly");
    Rly->setSection(".data.obf");
    Rly->setAlignment(Align(8));
  }

  SmallVector<CallInst *, 32> Calls;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CI = dyn_cast<CallInst>(&I))
        if (!CI->isInlineAsm() && !CI->isMustTailCall())
          Calls.push_back(CI);

  bool Changed = false;
  for (CallInst *CI : Calls) {
    if (splitmix64(S) % 8 != 0) // 1/8 的调用点，控制开销
      continue;
    Instruction *Next = CI->getNextNode();
    if (!Next)
      continue;

    uint64_t K = splitmix64(S);

    IRBuilder<> B(CI);
    Value *SpP = B.CreateInBoundsGEP(I64, Sp, {B.getInt64(0)});
    Value *V = B.CreateLoad(I64, SpP, /*isVolatile=*/true, "obf.sm.rv");
    Value *E = B.CreateXor(V, ConstantInt::get(I64, K), "obf.sm.re");
    Value *RlyP = B.CreateInBoundsGEP(I64, Rly, {B.getInt64(0)});
    B.CreateStore(E, RlyP, /*isVolatile=*/true);

    IRBuilder<> A(Next);
    // 注意：这里的 GEP 必须用 A 建（插在 call 之后），用 B 建会构成
    // 「先定义后支配」违规 —— IR 能过 verifier 的期望是 use 被 def 支配。
    Value *RlyP2 = A.CreateInBoundsGEP(I64, Rly, {A.getInt64(0)}, "obf.sm.rp");
    Value *E2 = A.CreateLoad(I64, RlyP2, /*isVolatile=*/true, "obf.sm.r2");
    Value *V2 = A.CreateXor(E2, ConstantInt::get(I64, K), "obf.sm.r3");
    Value *SpP2 = A.CreateInBoundsGEP(I64, Sp, {A.getInt64(0)}, "obf.sm.sp2");
    A.CreateStore(V2, SpP2, /*isVolatile=*/true);
    Changed = true;
  }
  return Changed;
}

// ── pass 入口 ──
bool processStackIndirectJump(Function &F) {
  if (ObfLevel == 0 || SjiLevel == 0)
    return false;
  if (!sjiEligible(F) || F.empty())
    return false;

  uint64_t S = funcSeed(*F.getParent(), F);
  if (!rollChance(S, SjiRatio))
    return false;

  bool Changed = false;

  // L1：先把 PHI 降级（否则栈化后 PHI 入边全错），再栈化
  if (SjiLevel >= 1) {
    SmallVector<PHINode *, 32> Phis;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *P = dyn_cast<PHINode>(&I))
          Phis.push_back(P);
    if (!Phis.empty())
      for (PHINode *P : Phis)
        DemotePHIToStack(P, std::nullopt);

    if (buildStackMachine(F, S))
      Changed = true;
  }

  // L2/L3：ret-sled。必须重跑块遍历（L1 已把所有终结符换成 br disp）
  if (SjiLevel >= 2) {
    bool UseTLS = (SjiLevel >= 3) && (splitmix64(S) % 2 == 0);

    SmallVector<BasicBlock *, 128> Blocks;
    for (BasicBlock &BB : F)
      if (!BB.isEHPad())
        Blocks.push_back(&BB);

    for (BasicBlock *BB : Blocks) {
      Instruction *T = BB->getTerminator();
      if (!T || T->getNumSuccessors() != 1)
        continue;
      if (isa<ReturnInst>(T) || isa<UnreachableInst>(T))
        continue;
      if (splitmix64(S) % 4 != 0) // 全做体积极速膨胀
        continue;
      if (emitRetSled(BB, T->getSuccessor(0), UseTLS))
        Changed = true;
    }
  }

  // L4：跨调用接力
  if (SjiLevel >= 4)
    if (emitCrossCallRelay(F, S))
      Changed = true;

  return Changed;
}

// ── new-PM 包装 ──
PreservedAnalyses runStackIndirectJump(Function &F, FunctionAnalysisManager &) {
  return processStackIndirectJump(F) ? PreservedAnalyses::none()
                                     : PreservedAnalyses::all();
}

} // namespace obf

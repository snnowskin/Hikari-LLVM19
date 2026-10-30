//===- ObfBcf.cpp - 虚假控制流（边守卫 + 跳板栈）---------------------===//
// 文档对应 §4.1。不动原条件，只在每条边上串守卫块；守卫内谓词由运行期种子槽
// 驱动，假块必须带副作用，否则被 GVN/InstCombine 抹平。守卫内部统一汇到 Join
// 块，保证后继 PHI 入边一致（B4）。
//===----------------------------------------------------------------------===//
#include "ObfCommon.h"

using namespace llvm;

namespace obf {

cl::opt<int> BcfProb("obf-bcf-prob",
                     cl::desc("虚假控制流：每条边被守卫的概率(0..100)"),
                     cl::init(100));
cl::opt<int> BcfDepth("obf-bcf-depth",
                      cl::desc("守卫嵌套深度，0=按档位自动(1/2/3)"),
                      cl::init(0));

// ── 假块内容 ──
/// 内联汇编垃圾：nop 链 + 一个不会被优化的内存屏障。
/// hasSideEffects=true + ~{memory} 是必须的，否则 InstCombine 会整条删掉。
static void emitJunkAsm(Module &M, IRBuilder<> &B, uint64_t &S) {
  LLVMContext &C = M.getContext();
  unsigned N = static_cast<unsigned>(splitmix64(S) % 3) + 1;
  std::string Txt;
  for (unsigned I = 0; I < N; ++I)
    Txt += "nop\n\t";
  // 加一条带数据依赖的空操作，让反汇编器看到「有效指令流」而不是填充
  Txt += "add x9, x9, #1\n\tsub x9, x9, #1";
  FunctionType *FT = FunctionType::get(Type::getVoidTy(C), {}, false);
  InlineAsm *IA = makeAsm(M, FT, Txt, "~{x9},~{memory}", /*HasSideEffects=*/true);
  SmallVector<Value *, 0> NoArgs;
  B.CreateCall(FT, IA, NoArgs, "");
}

/// 假块：副作用 + 恒假谓词 + 更大的假块（链式），最后回到真目标。
/// 返回值是链头，调用方把边接到链头上。
static BasicBlock *makeBogusChain(BasicBlock *Real, uint64_t &S,
                                  unsigned Rings) {
  Function *F = Real->getParent();
  Module &M = *F->getParent();
  LLVMContext &C = M.getContext();

  BasicBlock *Head = nullptr;
  BasicBlock *Prev = nullptr;

  for (unsigned R = 0; R < Rings; ++R) {
    BasicBlock *Bo = BasicBlock::Create(C, "obf.bcf.x", F);
    if (!Head)
      Head = Bo;

    IRBuilder<> B(Bo);
    emitJunkAsm(M, B, S);

    // 恒真谓词取反 → 恒假。用它把「死块」挂到 CFG 上：
    // 死块在静态上看是可达的（cond br 的另一支），优化器证不出它是死路。
    Value *T = makeOpaquePredicate(M, B, S, static_cast<unsigned>(splitmix64(S)));
    Value *Fl = B.CreateNot(T);

    BasicBlock *Tail = (R + 1 == Rings) ? Real : nullptr;
    BasicBlock *Dead = BasicBlock::Create(C, "obf.bcf.d", F);
    if (Tail)
      B.CreateCondBr(Fl, Dead, Tail);
    else
      B.CreateCondBr(Fl, Dead, Real);

    // 死块：再来一层垃圾 + 伪 dispatcher（indirectbr 表只有 Real 一个真项）
    IRBuilder<> D(Dead);
    emitJunkAsm(M, D, S);
    if (effLevel() >= 3) {
      Value *X = loadSeedSlot(M, D);
      Value *Bit =
          D.CreateTrunc(D.CreateAnd(X, ConstantInt::get(Type::getInt64Ty(C), 1)),
                        Type::getInt1Ty(C));
      // 用 select 造一个「地址由运行期值决定」的假象：两个目的块都是真块，
      // 但地址在编译期不可算 —— 符号执行必须把这条边当两次可行性检查。
      BasicBlock *Alt = BasicBlock::Create(C, "obf.bcf.a", F);
      Value *Addr = D.CreateSelect(Bit, BlockAddress::get(F, Alt),
                                   BlockAddress::get(F, Real));
      auto *IB = IndirectBrInst::Create(Addr, 2);
      IB->addDestination(Alt);
      IB->addDestination(Real);
      D.Insert(IB);

      // Alt：只用来给 indirectbr 凑第二个目的块，跳回链头（形成伪环）
      IRBuilder<> A(Alt);
      emitJunkAsm(M, A, S);
      A.CreateBr(Head ? Head : Real);
    } else {
      // 低档位不铺伪环，只留一条死边，体积友好
      D.CreateBr(Real);
    }

    Prev = Bo;
    (void)Prev;
  }
  return Head ? Head : Real;
}

// ── 边守卫 ──
/// 在 From --(idx)--> Succ 这条边上插守卫。返回守卫入口块（给 setSuccessor 用），
/// 并通过 JoinOut 返回「唯一进入 Succ 的汇合块」（给 PHI 改入边用）。
/// 三种形态按种子轮换，避免连续两条边长得一样（模式匹配攻击的软肋）。
///
/// 为什么必须有 Join：守卫内部有多条路径（真边/假链/伪环）都会走到 Succ，
/// 如果直接让它们各自连 Succ，Succ 里原有的 PHI 就会出现「同一个前驱多条入边、
/// 值还不一样」的非法形态，verifier 直接报错。加一个 Join 当唯一入口，
/// PHI 只需把「来自 From」改成「来自 Join」，一次到位。
static BasicBlock *makeEdgeGuard(BasicBlock *From, BasicBlock *Succ,
                                 unsigned Depth, uint64_t &S,
                                 BasicBlock *&JoinOut) {
  Function *F = From->getParent();
  Module &M = *F->getParent();
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);

  BasicBlock *Join = BasicBlock::Create(C, "obf.bcf.j", F);
  JoinOut = Join;

  // B9：先把所有守卫块建出来，再逐个填内容。
  // 旧写法在「给上一层补终结符」时用 IRBuilder 定位到上一层的末尾，
  // 可那一层在上一次迭代里已经写好了终结符 —— 于是新指令全被追加到
  // 终结符后面，verifier 直接报 "Terminator found in the middle of a
  // basic block!"（一报就是上百个块）。
  SmallVector<BasicBlock *, 4> Gs;
  for (unsigned D = 0; D < Depth; ++D)
    Gs.push_back(BasicBlock::Create(C, "obf.bcf.g", F));

  for (unsigned D = 0; D < Depth; ++D) {
    BasicBlock *G = Gs[D];
    // 真边一路往下一层走，最后一层落到 Join；假边永远进 Bogus 链
    BasicBlock *Next = (D + 1 < Depth) ? Gs[D + 1] : Join;

    IRBuilder<> B(G);
    BasicBlock *Bogus = makeBogusChain(Join, S, effLevel() >= 3 ? 2 : 1);

    unsigned Shape = static_cast<unsigned>(splitmix64(S) % 3);
    if (effLevel() < 2)
      Shape = 0;

    switch (Shape) {
    case 0: { // 恒真条件分支
      Value *P = makeOpaquePredicate(M, B, S, static_cast<unsigned>(splitmix64(S)));
      B.CreateCondBr(P, Next, Bogus);
      break;
    }
    case 1: { // 假 switch：default 走真边，case 值是随机常量永不命中
      Value *X = loadSeedSlot(M, B);
      auto *Sw = SwitchInst::Create(X, Next, 2, G);
      Sw->addCase(ConstantInt::get(I64, splitmix64(S)), Bogus);
      Sw->addCase(ConstantInt::get(I64, splitmix64(S)), Bogus);
      break;
    }
    default: { // 假 indirectbr：目的表里一真一假，地址由运行期位决定
      Value *X = loadSeedSlot(M, B);
      Value *T = B.CreateTrunc(X, Type::getInt1Ty(C));
      // BlockAddress 本身就是指针类型，select 出来直接给 indirectbr 用，
      // 不需要 inttoptr（对指针做 inttoptr 是非法 IR）。
      Value *Addr = B.CreateSelect(T, BlockAddress::get(F, Bogus),
                                   BlockAddress::get(F, Next));
      auto *IB = IndirectBrInst::Create(Addr, 2);
      IB->addDestination(Bogus);
      IB->addDestination(Next);
      B.Insert(IB);
      break;
    }
    }
  }

  // Join 的唯一出口：它就是 Succ 的唯一新前驱
  BranchInst::Create(Succ, Join);
  return Gs.empty() ? Join : Gs[0];
}

// ── 资格判定 ──
static bool bcfEligible(const Function &F) {
  if (F.isDeclaration() || F.empty() || F.hasOptNone() || isNoObf(F))
    return false;
  // naked 函数只允许内联汇编，插块会直接编不过
  if (F.hasFnAttribute(Attribute::Naked))
    return false;
  // 协程的 ret 是定制的，插块会破坏 coro 变体
  if (F.hasFnAttribute("presplitcoroutine"))
    return false;
  // musttail 要求 call 后紧跟 ret，插块即失效
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (const auto *CI = dyn_cast<CallInst>(&I))
        if (CI->isMustTailCall())
          return false;
  return true;
}

static bool isBcfTarget(const BasicBlock &BB) {
  if (BB.isEHPad())
    return false;
  const Instruction *T = BB.getTerminator();
  if (!T)
    return false;
  if (isa<IndirectBrInst>(T) || isa<CallBrInst>(T))
    return false; // setSuccessor 语义不同，不碰
  if (isa<CatchSwitchInst>(T) || isa<CleanupReturnInst>(T))
    return false;
  return T->getNumSuccessors() > 0;
}

// ── pass 入口 ──
bool processBogusControlFlow(Function &F) {
  if (ObfLevel == 0 && BcfProb.getNumOccurrences() == 0)
    return false;
  if (!bcfEligible(F))
    return false;

  int P = probOf(BcfProb, 60);
  if (P <= 0)
    return false;

  unsigned Depth = BcfDepth.getValue() > 0
                       ? static_cast<unsigned>(BcfDepth.getValue())
                       : (effLevel() >= 3 ? 3u : effLevel() >= 2 ? 2u : 1u);

  uint64_t S = funcSeed(*F.getParent(), F);

  // 先快照，后续会往 F 里不断插块（range-for 期间改 CFG 是 UB）
  SmallVector<BasicBlock *, 128> Blocks;
  for (BasicBlock &BB : F)
    if (isBcfTarget(BB))
      Blocks.push_back(&BB);
  if (Blocks.size() < 2)
    return false;

  bool Changed = false;
  for (BasicBlock *BB : Blocks) {
    if (!rollChance(S, P))
      continue;
    Instruction *T = BB->getTerminator();
    if (!T)
      continue;
    unsigned N = T->getNumSuccessors();
    for (unsigned I = 0; I < N; ++I) {
      BasicBlock *Succ = T->getSuccessor(I);
      if (!Succ || Succ == BB)
        continue;
      // 同一终结符有两个后继指向同一块（如 cond br 两支同目标）时只处理一次：
      // 否则 PHI 会出现两条来自 Join 的入边，verifier 不接受。
      bool DupSucc = false;
      for (unsigned J = 0; J < I; ++J)
        if (T->getSuccessor(J) == Succ)
          DupSucc = true;
      if (DupSucc)
        continue;

      BasicBlock *Join = nullptr;
      BasicBlock *Guard = makeEdgeGuard(BB, Succ, Depth, S, Join);
      if (!Guard || !Join)
        continue;

      // PHI 修正（B8）：这条边从 BB 改到了 Guard→Join→Succ，所以 Succ 里
      // 「来自 BB」的入边要挪一条给 Join —— 只挪一条。
      // 原先写的是「把所有来自 BB 的入边都改成 Join」，当 BB 有两条边指向同一个
      // Succ 时（cond br 两支同目标），两条都变成 Join，而 Join 只有一条边，
      // verifier 报「phi 里的入边比前驱多」，落到 codegen 就是 0x80000003。
      for (PHINode &Phi : Succ->phis())
        for (unsigned K = 0; K < Phi.getNumIncomingValues(); ++K)
          if (Phi.getIncomingBlock(K) == BB) {
            Phi.setIncomingBlock(K, Join);
            break;
          }

      T->setSuccessor(I, Guard);
      Changed = true;
    }
  }

  return Changed;
}

// ── new-PM 包装（-fpass-plugin 形态用） ──
PreservedAnalyses runBogusControlFlow(Function &F, FunctionAnalysisManager &) {
  return processBogusControlFlow(F) ? PreservedAnalyses::none()
                                    : PreservedAnalyses::all();
}

} // namespace obf

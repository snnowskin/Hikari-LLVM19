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

/// 建一个 ptr 数组全局（放 .data.obf）。
/// B24：地址表不能再放 `ptrtoint(blockaddress) ^ key` —— 那是 xor 套重定位，
/// AsmPrinter 只会加法重定位，落到静态初始化器就报
/// `Unsupported expression in static initializer`。改成存裸 blockaddress
/// 指针，xor 挪到运行期做。
[[maybe_unused]] static GlobalVariable *makePtrTable(Module &M, ArrayRef<Constant *> Items,
                                    StringRef Name) {
  auto *PtrTy = PointerType::getUnqual(M.getContext());
  auto *Ty = ArrayType::get(PtrTy, Items.size());
  auto *GV = new GlobalVariable(M, Ty, false, GlobalValue::InternalLinkage,
                                ConstantArray::get(Ty, Items), Name);
  GV->setSection(".data.obf");
  GV->setAlignment(Align(8));
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  return GV;
}

/// 建一个 i64 数组全局（放 .data.obf，运行期可写但值必须与我们编码一致）。
static GlobalVariable *makeTable(Module &M, ArrayRef<Constant *> Items,
                                 StringRef Name) {
  auto *I64 = Type::getInt64Ty(M.getContext());
  auto *Ty = ArrayType::get(I64, Items.size());
  auto *GV = new GlobalVariable(M, Ty, false,
                                GlobalValue::InternalLinkage,
                                ConstantArray::get(Ty, Items), Name);
  GV->setSection(".data.obf");
  GV->setAlignment(Align(8));
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  return GV;
}

// ── L1：IR 级 stack machine（v2） ──
static bool buildStackMachine(Function &F, uint64_t &S) {
  // B30：把边改成「push + br 分发器」以后，原本「只能经某条边到达」的块
  // 变成从分发器直接可达，跨块使用的 SSA 值立刻失去支配关系
  // （verifier: Instruction does not dominate all uses!）。
  // 下手前先把所有跨基本块使用的寄存器值降到栈上，之后只剩 load/store，
  // 支配关系就永远成立。
  {
    SmallVector<Instruction *, 64> Cross;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        if (I.getType()->isVoidTy() || isa<AllocaInst>(I) || I.isEHPad())
          continue;
        for (User *U : I.users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (UI && UI->getParent() != &BB) {
            Cross.push_back(&I);
            break;
          }
        }
      }
    for (Instruction *I : Cross)
      if (I->getParent() && !I->use_empty())
        DemoteRegToStack(*I, false, F.getEntryBlock().getFirstInsertionPt());
  }

  Module &M = *F.getParent();
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *PtrTy = PointerType::getUnqual(C);

  SmallVector<BasicBlock *, 128> Blocks;
  // 入口块永远不进栈机：它一旦有前驱就非法，而且 blockaddress 不允许指向
  // 入口块（verifier 报 "blockaddress may not be used with the entry
  // block!"）—— 我们下面要 ptrtoint(BlockAddress) 建地址表，含入口块必炸。
  BasicBlock &EntryBB = F.getEntryBlock();
  for (BasicBlock &BB : F)
    if (!BB.isEHPad() && &BB != &EntryBB)
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

    // 2. 表：cell key + 置换表
  //
  // 地址表（obf.sm.ad / obf.sm.bk）已废弃 —— 见下面 dispatcher 处的长注释：
  // blockaddress 进全局初始化器会被折叠成常量 1 且不产生重定位，
  // 整条「存表 + 运行期异或 + indirectbr」的路在 LLVM 20 上不可用。
  // 现在改由 switch 分发，只需要 cell key 和置换表。
  std::vector<Constant *> KCell;
  for (unsigned I = 0; I < Cells; ++I)
    KCell.push_back(ConstantInt::get(I64, splitmix64(S)));

  GlobalVariable *KTab = makeTable(M, KCell, "obf.sm.kc");
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
  EB.CreateStore(ConstantInt::get(I64, 0), Sp, true);

  // 把栈指针写进一个 volatile 全局槽：SROA / 内存提升看到「指针逃逸」就收手，
  // 否则整套栈会被提升成寄存器，L1 立刻退化成普通 switch（一点强度都没有）。
  GlobalVariable *Esc = M.getGlobalVariable("obf.sm.esc", true);
  if (!Esc) {
    Esc = new GlobalVariable(M, PtrTy, false,
                             GlobalValue::InternalLinkage,
                             ConstantPointerNull::get(PtrTy), "obf.sm.esc");
    Esc->setSection(".data.obf");
  }
  EB.CreateStore(Stack, Esc, true);

    // 4. 分发器
  BasicBlock *Disp = BasicBlock::Create(C, "obf.sm.disp", &F);
  {
    IRBuilder<> DB(Disp);
    Value *SpP = DB.CreateInBoundsGEP(I64, Sp, {DB.getInt64(0)}, "obf.sm.spp");
    Value *Cur = DB.CreateLoad(I64, SpP, true, "obf.sm.d");

    // 弹出 key
    Value *S1 = DB.CreateSub(Cur, ConstantInt::get(I64, 1));
    Value *Kp = DB.CreateInBoundsGEP(StackTy, Stack, {DB.getInt64(0), S1});
    Value *Kc = DB.CreateLoad(I64, Kp, true, "obf.sm.kc");

    // 弹出 slot
    Value *S2 = DB.CreateSub(S1, ConstantInt::get(I64, 1));
    Value *SlotP = DB.CreateInBoundsGEP(StackTy, Stack, {DB.getInt64(0), S2});
    Value *Slot = DB.CreateLoad(I64, SlotP, true, "obf.sm.sl");
    DB.CreateStore(S2, SpP, true);

    // cell = (slot ^ kc) & MASK → p = perm[cell]
    Value *Cell = DB.CreateAnd(DB.CreateXor(Slot, Kc),
                               ConstantInt::get(I64, Mask), "obf.sm.cell");
    auto *PermTy = cast<ArrayType>(PermTab->getValueType());
    Value *PermP = DB.CreateInBoundsGEP(PermTy, PermTab,
                                        {DB.getInt64(0), Cell});
    Value *P = DB.CreateLoad(I64, PermP, true, "obf.sm.p");

    // ── B35 终版：用 switch 分发，不用 blockaddress 地址表 ──
    //
    // 原设计是 `addr = tab[p] ^ bkey[p]` 然后 indirectbr。这条路在
    // LLVM 20 + 我们的 pass 组合下**根本不可用**：
    //
    //   `blockaddress(@f, %bb)` 一旦被放进全局初始化器（不管是裸 ptr 还是
    //   ptrtoint），AsmPrinter 会把它折叠成常量 1，且**不生成任何重定位**。
    //   实测最小复现（一个函数两个块、一张 1 项表）：
    //       .data 内容 = 01 00 00 00 00 00 00 00   重定位 = 0 条
    //   于是运行期 `1 ^ key` 得到野指针，br 过去直接 SIGSEGV。
    //   这解释了 L2/L3 的 `Invalid memory fetch @0xc1bb...`——
    //   也解释了为什么 `_start` 偶尔能跑（它的块恰好没被折叠）。
    //
    // 而且 SJI 本身会重建块（push + br disp），blockaddress 指向的旧块
    // 在改造过程中已经被替换，即使后端支持也拿不到正确地址。
    //
    // 改用 switch：p 已经是置换过的下标（运行期算出、编译期不可知），
    // 拿它当 case 值分发到对应块。后端会生成跳转表 + br，形态上仍是
    // 间接跳转，且**不依赖任何 blockaddress**。
    // 强度损失很小：置换表 perm、cell key 都还在，反编译照样要先解置换。
    {
      auto *SI = SwitchInst::Create(P, Blocks[0], N, Disp);
      for (unsigned I = 1; I < N; ++I)
        SI->addCase(ConstantInt::get(I64, I), Blocks[I]);
      (void)SI;
    }
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
    Value *Kcv = TB.CreateLoad(I64, KcP, true, "obf.sm.kv");
    Value *SlotV = TB.CreateXor(CellV, Kcv, "obf.sm.sv");

    Value *SpP = TB.CreateInBoundsGEP(I64, Sp, {TB.getInt64(0)});
    Value *Cur = TB.CreateLoad(I64, SpP, true, "obf.sm.cur");
    Value *P0P = TB.CreateInBoundsGEP(StackTy, Stack, {TB.getInt64(0), Cur});
    TB.CreateStore(SlotV, P0P, true);
    Value *S1 = TB.CreateAdd(Cur, ConstantInt::get(I64, 1));
    Value *P1P = TB.CreateInBoundsGEP(StackTy, Stack, {TB.getInt64(0), S1});
    TB.CreateStore(Kcv, P1P, true);
    Value *S2 = TB.CreateAdd(S1, ConstantInt::get(I64, 1));
    TB.CreateStore(S2, SpP, true);

    BranchInst::Create(Disp, T);
    T->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

// ── L2 / L3：ret-sled —— 借 LR 回跳 ──
//  把「块尾唯一后继」换成一段内联汇编：目标地址写进 x30，再用 ret 跳过去。
//  反汇编里看到的是 ret，静态分析很难把它与真正的函数返回区分开。
//  两条硬约束：
//    - 目标块不能有 PHI（这条边在 IR 上消失了，PHI 入边没法对上）—— B18；
//    - 栈进出严格配平，x29 必须原样恢复，x30 只用来放目标 —— B14。
static bool emitRetSled(BasicBlock *BB, BasicBlock *Target) {
  Function *F = BB->getParent();
  LLVMContext &C = F->getContext();
  Module &M = *F->getParent();

  Instruction *Term = BB->getTerminator();
  if (!Term || Term->getNumSuccessors() != 1)
    return false;
  if (!Target->phis().empty())
    return false; // B18

  // ── B39 终版：ret-sled 改成「两目标 switch」 ──
  //
  // 这条 pass 前后修过三次，每次换个死法，记在这里免得再踩：
  //
  //   v1（原始）：sub sp,#16 / stp x29,x30 / str 目标进 x30 槽 / ldr x29 / ret
  //       死因：真 LR 再也没被恢复。目标块末尾的 `ret` 用的还是被污染的值，
  //             跳回 sled → 死循环。实测 sji=2/3 卡在 sled 上 pc 不动，
  //             rc=no-exit。根子：ARM64 的 x30 同时是返回地址和间接跳转目标。
  //
  //   v2：AsmText = "br $0"，约束串 "r,~{memory}"
  //       死因：LLVM 把 $0 分给 x8，`br x8` 之后 x8 仍是代码地址，而栈机把
  //             x8 当 obf.sm.stk 指针用 → UC_ERR_READ_UNMAPPED @0x210764
  //             （x8=0x21072c，读代码段）。实测确认。
  //
  //   v3：mov x16,$0 / br x16，clobber ~{x16}
  //       仍然不行：$0 依旧可能被分到 x8（adrp/add 用 x8 最省指令）。
  //
  // 结论：只要「目标地址必须占一个通用寄存器」，就迟早撞上栈机正在用的那个。
  // 唯一干净的做法是不用寄存器传地址 —— 和 dispatcher 一样走 IR 层 switch。
  //
  // 但单目标 switch 会被优化器直接折成 br，等于白做。所以这里造一个
  // 「诱饵块」当第二分支：诱饵块本身是不可达的（条件恒假），但形状上是
  // 真分支，backend 会生成条件跳转 + 跳转表的混合形态，不会被折叠。
  BasicBlock *Decoy = BasicBlock::Create(C, "obf.sled.d", F, Target);
  {
    IRBuilder<> IB(Decoy);
    // 恒真谓词（依赖 volatile 种子槽，优化器证不出来）
    Value *Slot = loadSeedSlot(M, IB);
    Value *Odd = IB.CreateOr(Slot, ConstantInt::get(Type::getInt64Ty(C), 1));
    Value *Prod = IB.CreateMul(Odd, IB.CreateSub(Odd, ConstantInt::get(Type::getInt64Ty(C), 1)));
    Value *IsEven = IB.CreateICmpEQ(
        IB.CreateAnd(Prod, ConstantInt::get(Type::getInt64Ty(C), 1)),
        ConstantInt::get(Type::getInt64Ty(C), 0));
    // false 支路 = 永远不走
    BranchInst::Create(Target, Target, IsEven, Decoy);
  }

  IRBuilder<> B(Term);
  Value *Slot = loadSeedSlot(M, B);
  Value *Sel = B.CreateAnd(Slot, ConstantInt::get(Type::getInt64Ty(C), 0), "obf.sled.sel");
  auto *SI = SwitchInst::Create(Sel, Target, 1, Term);
  SI->addCase(ConstantInt::get(Type::getInt64Ty(C), 1), Decoy);
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

  GlobalVariable *Sp = M.getGlobalVariable("obf.sm.sp", true);
  if (!Sp)
    return false; // 只有 L1 跑过才有意义

  GlobalVariable *Rly = M.getGlobalVariable("obf.sm.rly", true);
  if (!Rly) {
    Rly = new GlobalVariable(M, I64, false,
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
    Value *V = B.CreateLoad(I64, SpP, true, "obf.sm.rv");
    Value *E = B.CreateXor(V, ConstantInt::get(I64, K), "obf.sm.re");
    Value *RlyP = B.CreateInBoundsGEP(I64, Rly, {B.getInt64(0)});
    B.CreateStore(E, RlyP, true);

    IRBuilder<> A(Next);
    // 注意：这里的 GEP 必须用 A 建（插在 call 之后），用 B 建会构成
    // 「先定义后支配」违规 —— IR 能过 verifier 的期望是 use 被 def 支配。
    Value *RlyP2 = A.CreateInBoundsGEP(I64, Rly, {A.getInt64(0)}, "obf.sm.rp");
    Value *E2 = A.CreateLoad(I64, RlyP2, true, "obf.sm.r2");
    Value *V2 = A.CreateXor(E2, ConstantInt::get(I64, K), "obf.sm.r3");
    Value *SpP2 = A.CreateInBoundsGEP(I64, Sp, {A.getInt64(0)}, "obf.sm.sp2");
    A.CreateStore(V2, SpP2, true);
    Changed = true;
  }
  return Changed;
}

// ── pass 入口 ──
bool processStackIndirectJump(Function &F) {
  // 显式给了 -obf-sji 就照办；没给则跟随档位（L2→2 层，L3→3 层）
  int SjiLv = SjiLevel.getNumOccurrences()
                  ? static_cast<int>(SjiLevel)
                  : (ObfLevel >= 2 ? static_cast<int>(ObfLevel) : 0);
  if (SjiLv <= 0)
    return false;
  if (!sjiEligible(F) || F.empty())
    return false;

  uint64_t S = funcSeed(*F.getParent(), F);
  if (!rollChance(S, SjiRatio))
    return false;

  bool Changed = false;

  // L1：先把 PHI 降级（否则栈化后 PHI 入边全错），再栈化
  if (SjiLv >= 1) {
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
  if (SjiLv >= 2) {
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
      if (emitRetSled(BB, T->getSuccessor(0)))
        Changed = true;
    }
  }

  // L4：跨调用接力
  if (SjiLv >= 4)
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

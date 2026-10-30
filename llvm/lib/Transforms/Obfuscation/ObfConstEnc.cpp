//===- ObfConstEnc.cpp - 常量加密（整数完善 + 浮点通道）--------------===//
// 文档对应 §4.7。整数走 rotl/add/xor 多段链；浮点先 bitcastToAPInt 再走同一条
// 链，回写时 bitcast 回原类型。key 表必须 volatile 读，否则链会被折回常量。
// 禁改：switch case、indirectbr 目的块、ImmArg 实参、landingpad 系列、musttail 实参。
//===----------------------------------------------------------------------===//
#include "ObfCommon.h"

using namespace llvm;

namespace obf {

cl::opt<int> CeProb("obf-ce-prob",
                    cl::desc("常量加密：每个常量被加密的概率(0..100)"),
                    cl::init(100));
cl::opt<int> CeStrength("obf-ce-strength",
                        cl::desc("常量加密强度 0=按档位(1/2/3 段链)"),
                        cl::init(0));

static constexpr unsigned kKeySlots = 256;

// ── key 表 ──
static GlobalVariable *getKeyTable(Module &M, uint64_t &S) {
  if (GlobalVariable *GV = M.getGlobalVariable("obf.ce.keys"))
    return GV;

  auto *I64 = Type::getInt64Ty(M.getContext());
  auto *Ty = ArrayType::get(I64, kKeySlots);
  SmallVector<Constant *, kKeySlots> Items;
  // 表值由种子派生：同版本可复现，不同版本必然不同
  uint64_t T = S ^ 0xC0FFEE0BADF00Dull;
  for (unsigned I = 0; I < kKeySlots; ++I)
    Items.push_back(ConstantInt::get(I64, splitmix64(T)));

  auto *GV = new GlobalVariable(M, Ty, false,
                                GlobalValue::InternalLinkage,
                                ConstantArray::get(Ty, Items), "obf.ce.keys");
  GV->setSection(".data.obf");
  GV->setAlignment(Align(8));
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  return GV;
}

/// 取第 Idx 项 key，并裁到 Ty 的位宽。
/// 必须 volatile：否则整条解密链会被常量折叠抹掉。
static Value *loadKey(Module &M, IRBuilder<> &B, IntegerType *Ty, unsigned Idx) {
  GlobalVariable *GV = M.getGlobalVariable("obf.ce.keys");
  if (!GV)
    return ConstantInt::get(Ty, 0);
  auto *I64 = Type::getInt64Ty(M.getContext());
  auto *ArrTy = cast<ArrayType>(GV->getValueType());
  Value *P = B.CreateInBoundsGEP(ArrTy, GV,
                                 {B.getInt64(0), B.getInt64(Idx % kKeySlots)});
  Value *V = B.CreateLoad(I64, P, true, "obf.ce.k");
  unsigned W = Ty->getIntegerBitWidth();
  if (W == 64)
    return V;
  if (W < 64)
    return B.CreateTrunc(V, Ty, "obf.ce.kt");
  return B.CreateZExt(V, Ty, "obf.ce.kz");
}

// ── 译码链：dec(enc) = rotr((enc ^ k1) - k2, r) ──
//  编码侧（编译期，APInt 上算）：
//      enc = ((rotl(C, r) + k2) ^ k1)
//  解码侧（运行期，IR 里拼）：
//      t1 = enc ^ k1        == rotl(C, r) + k2
//      t2 = t1  - k2        == rotl(C, r)
//      t3 = rotr(t2, r)     == C
//  r 取 [1, W-1]，W==1 时退化成纯异或。
static void apRotate(APInt &V, unsigned R, bool Left) {
  unsigned W = V.getBitWidth();
  if (W <= 1)
    return;
  R %= W;
  if (R == 0)
    return;
  V = Left ? V.rotl(R) : V.rotr(R);
}

/// 把整数常量 Bits 加密成「待写入指令流的立即数」。
/// 必须与 emitDecodeChain 逐段镜像，否则解出来是垃圾（v1 就在这里翻过车：
/// 编码用 K1i/K2i/R，解码却另算一套）。Rings 决定链长，两处必须同源。
static APInt encodeBits(const APInt &Bits, const APInt &K1, const APInt &K2,
                        unsigned R, unsigned Rings) {
  APInt V = Bits;
  if (Rings >= 3)
    apRotate(V, R, /*Left=*/true); // 解码侧 rotr，这里先 rotl 抵消
  if (Rings >= 2)
    V += K2;                        // 解码侧 sub，这里先加
  V ^= K1;                          // 解码侧先 xor，这里最后 xor
  return V;
}

/// 在 B 处插入译码链，返回与 C 等值的运行期值。
/// 链形态（与 encodeBits 严格镜像）：t = enc ^ k1; t -= k2; t = rotr(t, r)
static Value *emitDecodeChain(Module &M, IRBuilder<> &B, IntegerType *Ty,
                              const APInt &Enc, unsigned K1i, unsigned K2i,
                              unsigned R, unsigned Rings) {
  unsigned W = Ty->getIntegerBitWidth();

  Value *V = ConstantInt::get(Ty, Enc);
  Value *K1 = loadKey(M, B, Ty, K1i);
  Value *T = B.CreateXor(V, K1, "obf.ce.d1");
  if (W == 1)
    return T; // i1：只有一段异或，且 key 是运行期位（不引入非法移位）

  if (Rings >= 2) {
    Value *K2 = loadKey(M, B, Ty, K2i);
    T = B.CreateSub(T, K2, "obf.ce.d2");
  }

  if (Rings >= 3) {
    // rotr(T, R) = (T >> R) | (T << (W - R))，手搓旋转（宽度不必是 2 的幂）
    Value *Lo = B.CreateLShr(T, ConstantInt::get(Ty, R), "obf.ce.rl");
    Value *Hi = B.CreateShl(T, ConstantInt::get(Ty, W - R), "obf.ce.rh");
    T = B.CreateOr(Lo, Hi, "obf.ce.d3");
  }

  return T;
}

// ── 常量提取：整数 / 浮点 / splat 向量 统一成 (整数类型, 位) ──
static bool extractBits(Constant *C, APInt &Bits, IntegerType *&IntTy,
                        Type *&OrigTy) {
  LLVMContext &Ctx = C->getContext();
  OrigTy = C->getType();

  // 标量整数
  if (auto *CI = dyn_cast<ConstantInt>(C)) {
    IntTy = CI->getIntegerType();
    Bits = CI->getValue();
    return true;
  }

  // 标量浮点：bitcastToAPInt 拿到 IEEE 位模式（f80 是 80 位，也能处理）
  if (auto *CF = dyn_cast<ConstantFP>(C)) {
    APInt Raw = CF->getValueAPF().bitcastToAPInt();
    IntTy = IntegerType::get(Ctx, Raw.getBitWidth());
    Bits = Raw;
    return true;
  }

  // splat 向量：<N x iM> / <N x float>，总位宽 ≤128 才做（bitcast 要等宽）
  auto *VT = dyn_cast<VectorType>(OrigTy);
  if (!VT)
    return false;
  Type *ElemTy = VT->getElementType();
  if (!ElemTy->isIntegerTy() && !ElemTy->isFloatingPointTy())
    return false;

  unsigned N = VT->getElementCount().getKnownMinValue();
  // LLVM 20 的 Type::getPrimitiveSizeInBits() 返回 TypeSize，不是裸整数；
  // 直接赋给 unsigned 会编译失败，必须 getFixedValue()。
  unsigned ElemBits =
      static_cast<unsigned>(ElemTy->getPrimitiveSizeInBits().getFixedValue());
  unsigned Total = N * ElemBits;
  if (Total == 0 || Total > 128 || (Total % 8) != 0)
    return false;

  // 只处理 splat（全元素相同），逐元素不同的向量留给后续 pass
  APInt Elem;
  if (auto *CDV = dyn_cast<ConstantDataVector>(C)) {
    Elem = CDV->getElementAsAPInt(0);
    for (unsigned I = 1; I < N; ++I)
      if (CDV->getElementAsAPInt(I) != Elem)
        return false;
  } else if (auto *CV = dyn_cast<ConstantVector>(C)) {
    auto *First = dyn_cast<Constant>(CV->getOperand(0));
    if (!First)
      return false;
    if (auto *CI = dyn_cast<ConstantInt>(First))
      Elem = CI->getValue();
    else if (auto *CF = dyn_cast<ConstantFP>(First))
      Elem = CF->getValueAPF().bitcastToAPInt();
    else
      return false;
    for (unsigned I = 1; I < N; ++I) {
      Constant *E = dyn_cast<Constant>(CV->getOperand(I));
      if (!E)
        return false;
      APInt Other;
      if (auto *CI = dyn_cast<ConstantInt>(E))
        Other = CI->getValue();
      else if (auto *CF = dyn_cast<ConstantFP>(E))
        Other = CF->getValueAPF().bitcastToAPInt();
      else
        return false;
      if (Other != Elem)
        return false;
    }
  } else {
    return false;
  }

  APInt Wide(Total, 0);
  for (unsigned I = 0; I < N; ++I)
    Wide.insertBits(Elem, I * ElemBits);
  Bits = Wide;
  IntTy = IntegerType::get(Ctx, Total);
  return true;
}

// ── 单个 operand 的加密 ──
static bool encryptOperand(Instruction &I, unsigned OpIdx, uint64_t &S,
                           unsigned Rings) {
  Value *Op = I.getOperand(OpIdx);
  auto *C = dyn_cast<Constant>(Op);
  if (!C || isa<GlobalValue>(C) || isa<ConstantExpr>(C))
    return false;

  APInt Bits;
  IntegerType *IntTy = nullptr;
  Type *OrigTy = nullptr;
  if (!extractBits(C, Bits, IntTy, OrigTy))
    return false;
  if (Bits.getBitWidth() == 0)
    return false;

  Module &M = *I.getModule();
  LLVMContext &Ctx = M.getContext();

  // key 从表里取两份，用它们的常量值参与编码（运行期再 volatile 读回来）
  GlobalVariable *Tab = getKeyTable(M, S);
  auto *ArrTy = cast<ArrayType>(Tab->getValueType());
  unsigned K1i = static_cast<unsigned>(splitmix64(S) % kKeySlots);
  unsigned K2i = (K1i + 1 + static_cast<unsigned>(splitmix64(S) % 127)) % kKeySlots;
  const auto *Arr = cast<ConstantArray>(Tab->getInitializer());
  APInt K1Full = cast<ConstantInt>(Arr->getOperand(K1i))->getValue();
  APInt K2Full = cast<ConstantInt>(Arr->getOperand(K2i))->getValue();

  unsigned W = IntTy->getIntegerBitWidth();
  APInt K1 = K1Full.zextOrTrunc(W);
  APInt K2 = K2Full.zextOrTrunc(W);
  // W==1 不许旋转（R 无意义），且只用一段异或；此时 K2 不参与
  unsigned R = (W <= 1 || Rings < 3)
                   ? 0
                   : 1 + static_cast<unsigned>(splitmix64(S) % (W - 1));
  if (W == 1)
    Rings = 1; // 编解码必须同时退化，否则链段数对不上

  APInt Enc = encodeBits(Bits, K1, K2, R, Rings);
  (void)ArrTy;

  // 插入点：PHI 必须插在「前驱块末尾」，别的指令插在自己前面
  IRBuilder<> B(&I);
  if (auto *Phi = dyn_cast<PHINode>(&I)) {
    BasicBlock *Pred = Phi->getIncomingBlock(OpIdx);
    if (!Pred)
      return false;
    B.SetInsertPoint(Pred->getTerminator());
  }

  Value *Dec = emitDecodeChain(M, B, IntTy, Enc, K1i, K2i, R, Rings);
  if (OrigTy != IntTy) {
    if (OrigTy->isVectorTy() || OrigTy->isFloatingPointTy())
      Dec = B.CreateBitCast(Dec, OrigTy, "obf.ce.bc");
    else
      return false;
  }

  I.setOperand(OpIdx, Dec);
  (void)Ctx;
  return true;
}

// ── 资格判定 ──
static bool opIndexForbidden(const Instruction &I, unsigned OpIdx) {
  // inline asm：全部参数跳过（"i" 约束要立即数）
  if (const auto *CB = dyn_cast<CallBase>(&I))
    if (isa<InlineAsm>(CB->getCalledOperand()))
      return true;
  // intrinsic 的 immarg 参数
  if (const auto *CB = dyn_cast<CallBase>(&I))
    if (OpIdx < CB->arg_size() && CB->paramHasAttr(OpIdx, Attribute::ImmArg))
      return true;
  // alloca 的数组长度 / 对齐
  if (isa<AllocaInst>(I))
    return true;
  // EH：clause 与 catchpad 参数是有特殊约束的常量
  if (isa<LandingPadInst>(I) || isa<CatchPadInst>(I) || isa<CleanupPadInst>(I) ||
      isa<CatchSwitchInst>(I) || isa<CatchReturnInst>(I) ||
      isa<CleanupReturnInst>(I))
    return true;
  // switch / indirectbr：case 与目的块不是普通 operand，整体跳过
  if (isa<SwitchInst>(I) || isa<IndirectBrInst>(I) || isa<CallBrInst>(I))
    return true;
  // musttail 的实参不能改（改了就破坏 ABI 承诺）
  if (const auto *CI = dyn_cast<CallInst>(&I))
    if (CI->isMustTailCall())
      return true;
  // 结构性下标：LangRef 要求 extractvalue / insertvalue 的索引、shufflevector
  // 的 mask 必须是常量。换成运行期值 verifier 不拦，但 codegen 会崩。
  if (isa<ExtractValueInst>(I))
    return OpIdx >= 1; // op0 是聚合体，能改
  if (isa<InsertValueInst>(I))
    return OpIdx >= 2; // op0 聚合体、op1 插入值，都能改
  if (isa<ShuffleVectorInst>(I))
    return OpIdx >= 2; // mask 必须是常量
  return false;
}

bool processConstantEncryption(Function &F) {
  if (ObfLevel == 0)
    return false;
  if (F.isDeclaration() || F.empty())
    return false;
  if (isNoObf(F) || F.getName().starts_with("obf_") ||
      F.getName().starts_with("obf."))
    return false; // 自家运行时不做，避免把译码链自己加密一遍（会指数膨胀）

  int P = levelScale(CeProb.getValue());
  if (P <= 0)
    return false;

  unsigned Rings = CeStrength.getValue() > 0
                       ? static_cast<unsigned>(CeStrength.getValue())
                       : (ObfLevel >= 3 ? 3u : ObfLevel >= 2 ? 2u : 1u);
  if (Rings > 3)
    Rings = 3;

  Module &M = *F.getParent();
  uint64_t S = funcSeed(M, F);
  getKeyTable(M, S);

  bool Changed = false;

  // 快照指令（加密时只改 operand，不增删指令，但仍按快照走更稳）
  SmallVector<Instruction *, 256> Insts;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      Insts.push_back(&I);

  for (Instruction *I : Insts) {
    if (I->getOpcode() == Instruction::Ret && ObfLevel < 2)
      continue;
    unsigned N = I->getNumOperands();
    for (unsigned O = 0; O < N; ++O) {
      if (opIndexForbidden(*I, O))
        continue;
      if (!rollChance(S, P))
        continue;
      if (encryptOperand(*I, O, S, Rings))
        Changed = true;
    }
  }

  return Changed;
}

// ── new-PM 包装 ──
PreservedAnalyses runConstantEncryption(Function &F, FunctionAnalysisManager &) {
  return processConstantEncryption(F) ? PreservedAnalyses::none()
                                      : PreservedAnalyses::all();
}

} // namespace obf

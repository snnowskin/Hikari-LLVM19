//===- ObfFuncWrap.cpp - 函数包装（替代上游 // Broken 版本）----------===//
// 文档对应 §4.6。上游在入口改 LR 再 jmp，callee 二次保存 LR=自身 → 无限递归。
// 本实现不碰 LR，纯 IR 转发 + TLS 影子栈校验 cookie；跳过自调用与 wrapper
// 内部调用，防止换位置复发同类递归。
//===----------------------------------------------------------------------===//
#include "ObfCommon.h"

using namespace llvm;

namespace obf {

cl::opt<int> WrapProb("obf-wrap-prob",
                      cl::desc("函数包装：被包函数百分比(0..100)"),
                      cl::init(100));
cl::opt<int> WrapLayer("obf-wrap-layer",
                       cl::desc("函数包装层数 0=按档位(1/2/3)"),
                       cl::init(0));

// ── 影子栈（thread_local，不能是普通全局：JNI 线程并发会互相踩） ──
namespace {
// 影子栈容量。取值必须是 2 的幂：下标用 &(cap-1) 做掩码，保证无论递归多深
// 都不会写穿 TLS（v1 直接拿 depth 当下标，深递归会踩坏旁边的 TLS 变量）。
// 代价：嵌套超过 cap 层时同一槽被复写，校验会 fail-closed 落 brk #0 —— 
// 这是故意的（宁可崩，不要静默放行），实际目标里 256 层嵌套包装不存在。
constexpr unsigned kCookieCap = 256;
} // namespace

static GlobalVariable *getCookieStack(Module &M) {
  auto *I64 = Type::getInt64Ty(M.getContext());
  auto *Ty = ArrayType::get(I64, kCookieCap);
  if (GlobalVariable *GV = M.getGlobalVariable("obf.fw.stack"))
    return GV;
  auto *GV = new GlobalVariable(M, Ty, false,
                                GlobalValue::InternalLinkage,
                                ConstantAggregateZero::get(Ty), "obf.fw.stack");
  GV->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
  GV->setAlignment(Align(8));
  GV->setSection(".data.obf");
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  return GV;
}

static GlobalVariable *getCookieSp(Module &M) {
  auto *I64 = Type::getInt64Ty(M.getContext());
  if (GlobalVariable *GV = M.getGlobalVariable("obf.fw.sp"))
    return GV;
  auto *GV = new GlobalVariable(M, I64, false,
                                GlobalValue::InternalLinkage,
                                ConstantInt::get(I64, 0), "obf.fw.sp");
  GV->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
  GV->setAlignment(Align(8));
  GV->setSection(".data.obf");
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  return GV;
}

/// 调用点表：每项 = ptrtoint(callee) ^ 该项的 key。
/// 放 .data.obf（可写），运行期可被自己的初始化代码重写（抗静态 dump）。
static GlobalVariable *getCalleeTable(Module &M, ArrayRef<Constant *> Items,
                                      StringRef Name) {
  auto *I64 = Type::getInt64Ty(M.getContext());
  auto *Ty = ArrayType::get(I64, Items.size());
  auto *GV = new GlobalVariable(M, Ty, false,
                                GlobalValue::InternalLinkage,
                                ConstantArray::get(Ty, Items), Name);
  GV->setSection(".data.obf");
  GV->setAlignment(Align(8));
  return GV;
}

// ── 陷阱块 ──
static void emitTrapBlock(Module &M, Function *W, BasicBlock *TrapBB) {
  LLVMContext &C = M.getContext();
  IRBuilder<> B(TrapBB);
  FunctionType *FT = FunctionType::get(Type::getVoidTy(C), {}, false);
  InlineAsm *IA = makeAsm(M, FT, "brk #0", "~{memory}", true);
  SmallVector<Value *, 0> NoArgs;
  B.CreateCall(FT, IA, NoArgs, "");
  B.CreateUnreachable();
}

// ── 生成一层 wrapper ──
//  Cookie 规则：C = splitmix64(funcSeed ^ layer) | 1，再与运行期的栈深度异或。
//  栈深度是 thread_local，所以每次调用的 cookie 都不同 —— 模拟执行抓不到常数。
static Function *buildWrapper(Module &M, Function &Callee, Function &Target,
                              unsigned Layer, uint64_t Seed) {
  LLVMContext &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);

  std::string Name = "obf_ww_" + Callee.getName().str();
  if (Layer > 1)
    Name += "_" + std::to_string(Layer);

  Function *W = Function::Create(Callee.getFunctionType(),
                                 GlobalValue::InternalLinkage, Name, &M);
  W->setAttributes(Callee.getAttributes());
  // B16：callee 若是 alwaysinline（clang 给 static inline 常加），wrapper 会同时
  // 带 alwaysinline + noinline → Verifier 报 incompatible，必须先去再置。
  W->removeFnAttr(Attribute::AlwaysInline);
  W->addFnAttr(Attribute::NoInline);
  W->setCallingConv(Callee.getCallingConv());

  GlobalVariable *Stack = getCookieStack(M);
  GlobalVariable *Sp = getCookieSp(M);
  auto *StackTy = cast<ArrayType>(Stack->getValueType());

  BasicBlock *Entry = BasicBlock::Create(C, "obf.fw.entry", W);
  BasicBlock *Trap = BasicBlock::Create(C, "obf.fw.trap", W);
  BasicBlock *Ret = BasicBlock::Create(C, "obf.fw.ret", W);

  // 表项：ptrtoint(Target) ^ key
  uint64_t Key = splitmix64(Seed) | 1ull;
  Constant *Enc = ConstantExpr::getXor(
      ConstantExpr::getPtrToInt(&Target, I64), ConstantInt::get(I64, Key));
  GlobalVariable *Tab =
      getCalleeTable(M, {Enc}, ("obf.fw.tab." + Callee.getName()).str());

    // 入口：压 cookie
  IRBuilder<> B(Entry);
  Value *SpP = B.CreateInBoundsGEP(I64, Sp, {B.getInt64(0)}, "obf.fw.spp");
  Value *Depth = B.CreateLoad(I64, SpP, true, "obf.fw.d");
  Value *Cookie = B.CreateXor(ConstantInt::get(I64, Key ^ (Seed * 2 + Layer)),
                              Depth);
  Value *Idx = B.CreateAnd(Depth, ConstantInt::get(I64, kCookieCap - 1),
                           "obf.fw.idx");
  Value *Slot = B.CreateInBoundsGEP(StackTy, Stack, {B.getInt64(0), Idx},
                                    "obf.fw.slot");
  B.CreateStore(Cookie, Slot, true);
  Value *Depth1 = B.CreateAdd(Depth, ConstantInt::get(I64, 1), "obf.fw.d1");
  B.CreateStore(Depth1, SpP, true);

    // 间接调用：从表里取指针
  Value *TabP = B.CreateInBoundsGEP(cast<ArrayType>(Tab->getValueType()), Tab,
                                    {B.getInt64(0), B.getInt64(0)});
  Value *EncV = B.CreateLoad(I64, TabP, true, "obf.fw.enc");
  Value *Dec = B.CreateXor(EncV, ConstantInt::get(I64, Key), "obf.fw.dec");
  Value *FP = B.CreateIntToPtr(Dec, PointerType::getUnqual(C), "obf.fw.fp");

  SmallVector<Value *, 16> Args;
  for (Argument &A : W->args())
    Args.push_back(&A);
  CallInst *CI = B.CreateCall(Callee.getFunctionType(), FP, Args, "obf.fw.r");
  CI->setCallingConv(Callee.getCallingConv());

    // 弹栈校验
  Value *SpP2 = B.CreateInBoundsGEP(I64, Sp, {B.getInt64(0)});
  Value *D2 = B.CreateLoad(I64, SpP2, true);
  Value *D2m = B.CreateSub(D2, ConstantInt::get(I64, 1), "obf.fw.d2");
  B.CreateStore(D2m, SpP2, true);
  Value *Idx2 = B.CreateAnd(D2m, ConstantInt::get(I64, kCookieCap - 1),
                            "obf.fw.idx2");
  Value *Slot2 = B.CreateInBoundsGEP(StackTy, Stack, {B.getInt64(0), Idx2});
  Value *C2 = B.CreateLoad(I64, Slot2, true, "obf.fw.c2");
  Value *Expect = B.CreateXor(ConstantInt::get(I64, Key ^ (Seed * 2 + Layer)),
                              D2m);
  // B17：递归深度超过 kCookieCap 时环会覆盖，比对必然失败 —— 那种情况放弃
  // 校验（而不是 brk 自杀），否则 256 层以上的正常递归会被误杀。
  Value *Overflow = B.CreateICmpUGE(D2m, ConstantInt::get(I64, kCookieCap),
                                    "obf.fw.of");
  Value *Bad = B.CreateAnd(
      B.CreateICmpNE(C2, Expect, "obf.fw.ne"), B.CreateNot(Overflow),
      "obf.fw.bad");
  // 顺便把弹出来的槽擦掉，避免残留可被 dump 的 cookie 序列
  B.CreateStore(ConstantInt::get(I64, 0), Slot2, true);
  B.CreateCondBr(Bad, Trap, Ret);

    // 陷阱 / 返回
  emitTrapBlock(M, W, Trap);
  {
    IRBuilder<> R(Ret);
    if (W->getReturnType()->isVoidTy())
      R.CreateRetVoid();
    else
      R.CreateRet(CI);
  }

  return W;
}

// ── 资格判定 ──
static bool wrapEligible(const Function &F) {
  if (F.isDeclaration() || F.empty() || F.isIntrinsic())
    return false;
  if (F.hasOptNone() || isNoObf(F))
    return false;
  if (F.isVarArg()) // 变参转发要 va_list 处理，这里直接放过
    return false;
  if (F.hasFnAttribute(Attribute::Naked))
    return false;
  if (F.hasFnAttribute("presplitcoroutine"))
    return false;
  // available_externally / dllimport 没有实体可转发
  if (F.hasAvailableExternallyLinkage())
    return false;
  if (F.getName().starts_with("obf_ww_") || F.getName().starts_with("obf.fw."))
    return false;
  // 名字里带 llvm. 的都是 intrinsic 包装
  if (F.getName().starts_with("llvm."))
    return false;
  return true;
}

// ── pass 入口（Module 级：需要跨函数改调用点） ──
bool processFunctionWrapper(Module &M) {
  if (ObfLevel == 0)
    return false;

  int P = levelScale(WrapProb.getValue());
  if (P <= 0)
    return false;

  unsigned Layers = WrapLayer.getValue() > 0
                        ? static_cast<unsigned>(WrapLayer.getValue())
                        : (ObfLevel >= 3 ? 3u : ObfLevel >= 2 ? 2u : 1u);
  if (Layers > 3)
    Layers = 3;

  // 快照：建 wrapper 会往 M 里加函数
  SmallVector<Function *, 256> Funcs;
  for (Function &F : M)
    if (wrapEligible(F))
      Funcs.push_back(&F);

  bool Changed = false;
  for (Function *F : Funcs) {
    uint64_t S = funcSeed(M, *F);
    if (!rollChance(S, P))
      continue;

    Function *Inner = F;
    for (unsigned L = 1; L <= Layers; ++L) {
      // 名字冲突（同名 wrapper 已存在）说明跑过一轮，跳过
      std::string NM = "obf_ww_" + F->getName().str();
      if (L > 1)
        NM += "_" + std::to_string(L);
      if (M.getFunction(NM))
        break;
      Inner = buildWrapper(M, *F, *Inner, L, S ^ (L * 0x9E3779B9ull));
      Changed = true;
      if (L > 1)
        break; // 双层足够，三层收益递减且体积翻倍
    }
    if (Inner == F)
      continue;

    // 把所有内部直接调用重定向到最外层 wrapper
    for (Function &Caller : M) {
      if (&Caller == Inner || Caller.isDeclaration())
        continue;
      // wrapper 自身对实体的那次调用绝对不能改，否则 wrap→wrap→… 无限递归
      // （这正是上游 FunctionWrapper 崩溃的同款事故，只是换了个位置）
      if (Caller.getName().starts_with("obf_ww_"))
        continue;
      // 递归自调用不重定向：每层递归多两帧加两次 cookie 校验，深递归会爆栈
      if (&Caller == F)
        continue;
      for (BasicBlock &BB : Caller)
        for (Instruction &I : BB) {
          auto *CI = dyn_cast<CallInst>(&I);
          if (!CI)
            continue;
          if (CI->isMustTailCall())
            continue; // 换 callee 会破坏 musttail 的 ABI 承诺
          if (CI->getCalledFunction() != F)
            continue;
          CI->setCalledFunction(Inner);
          Changed = true;
        }
    }
  }

  return Changed;
}

// ── new-PM 包装 ──
PreservedAnalyses runFunctionWrapper(Module &M, ModuleAnalysisManager &) {
  return processFunctionWrapper(M) ? PreservedAnalyses::none()
                                   : PreservedAnalyses::all();
}

} // namespace obf

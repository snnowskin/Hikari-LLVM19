//===- ObfCommon.h - 魔改混淆公共设施 ----------------------*- C++ -*-===//
// 基座：Aethereux/Hikari-LLVM19（实际 LLVM 20.1.5），上游调度是 legacy PM，
// 因此每个 pass 同时提供 processXxx() 与 runXxx() 两个入口。
// 统一提供：按函数定种、no-obf 判定、恒真谓词、运行期种子槽、内联汇编构造。
//===----------------------------------------------------------------------===//
#ifndef OBF_MAGIC_COMMON_H
#define OBF_MAGIC_COMMON_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>

namespace obf {

// ── 命令行：全部 pass 共享一套档位，不再散落在各 .cpp 顶部 ──
// -mllvm -obf-level=<0..3>   0=关 1=标准 2=专业 3=变态
// -mllvm -obf-seed=<u64>     构建种子，CI 里必须传，保证可复现
// -mllvm -obf-junk-prob=<1..100>
// -mllvm -obf-sji=<0..4>     StackIndirectJump 层级上限
// -mllvm -obf-sji-ratio=<1..100>  允许上 SJI 的函数百分比（白名单宽度）
extern llvm::cl::opt<int>      ObfLevel;
extern llvm::cl::opt<uint64_t> ObfSeed;
extern llvm::cl::opt<int>      JunkProb;
extern llvm::cl::opt<int>      SjiLevel;
extern llvm::cl::opt<int>      SjiRatio;

// ── 种子派生 ──
/// splitmix64：小、快、分布够。禁止用来做密码学，这里只是打散。
inline uint64_t splitmix64(uint64_t &S) {
  uint64_t Z = (S += 0x9E3779B97F4A7C15ull);
  Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
  Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
  return Z ^ (Z >> 31);
}

/// FNV-1a，用于把函数名折进种子。
inline uint64_t fnv1a(llvm::StringRef S, uint64_t H = 0xCBF29CE484222325ull) {
  for (unsigned char C : S) {
    H ^= static_cast<uint64_t>(C);
    H *= 0x100000001B3ull;
  }
  return H;
}

/// 每函数独立种子：同 module 同函数名在不同构建种子下不同，
/// 同一构建下稳定（CI 可复现）。
inline uint64_t funcSeed(const llvm::Module &M, const llvm::Function &F) {
  uint64_t S = ObfSeed;
  S = fnv1a(M.getModuleIdentifier(), S);
  S = fnv1a(F.getName(), S);
  S = fnv1a("ollvm-magic-v2", S); // 方案版本也进种子：换版本必然换全部随机布局
  return S;
}

// ── no-obf 判定（双路：名字前缀 + annotate） ──
inline bool isNoObf(const llvm::Function &F) {
  // 路 1：名字前缀。上游 Hikari 的 StringEncryption 等 pass 走这条。
  llvm::StringRef N = F.getName();
  if (N.starts_with("obf_no_") || N.starts_with("hikari_no_") ||
      N.starts_with("__obf_runtime_") || N.contains("obf_skip_"))
    return true;

  // 路 2：注解。上游部分分支只认这条，两路都写才稳（文档 §8.19）。
  //   C 侧：__attribute__((annotate("no_obf")))
  //   IR 侧：Hikari 的 annotation2Metadata 会把它折成 Function 上的
  //          !annotation 元数据；折之前则躺在 llvm.global.annotations 数组里。
  //   注意：注解不是指令（没有 llvm::AnnotateInst 这种东西），别找错地方。
  llvm::SmallVector<llvm::MDNode *, 4> Ann;
  F.getMetadata("annotation", Ann);
  for (llvm::MDNode *N : Ann)
    for (const llvm::MDOperand &Op : N->operands())
      if (const auto *S = llvm::dyn_cast_or_null<llvm::MDString>(Op.get()))
        if (S->getString() == "no_obf")
          return true;

  return false;
}

// ── 概率判定 ──
/// 档位 → 概率倍率。L1=1/3, L2=2/3, L3=1.0（100%）
inline int levelScale(int BaseProb) {
  switch (static_cast<int>(ObfLevel)) {
  case 1: return BaseProb * 1 / 3;
  case 2: return BaseProb * 2 / 3;
  case 3: return BaseProb;
  default: return 0;
  }
}

/// 用「种子 + 计数器」现算随机数，不引入全局可变状态（避免并行 backend race）。
inline bool rollChance(uint64_t &S, int Percent) {
  if (Percent >= 100) return true;
  if (Percent <= 0) return false;
  return (splitmix64(S) % 100) < static_cast<uint64_t>(Percent);
}

// ── 不可折叠副作用：切断优化器的折叠边 ──
/// llvm.sideeffect 是优化器唯一"不敢动"的零成本屏障。
/// 用法：在关键点插一条，让常量传播/ DCE 不能跨过去。
inline void emitSideEffect(llvm::Module &M, llvm::Instruction *IP) {
  llvm::Function *SE = llvm::Intrinsic::getDeclaration(
      &M, llvm::Intrinsic::sideeffect);
  llvm::CallInst::Create(SE, {}, "", IP);
}

/// volatile 读一个 u64 全局（运行期未知源）。
/// 与 §4.1 的 obf_seed_slot 配合：编译期不知值，但真值恒 0/恒真由我们保证。
inline llvm::Value *loadSeedSlot(llvm::Module &M, llvm::IRBuilder<> &B,
                                 llvm::StringRef Name = "obf_seed_slot") {
  llvm::LLVMContext &C = M.getContext();
  auto *I64 = llvm::Type::getInt64Ty(C);
  auto *Ty  = llvm::ArrayType::get(I64, 1);

  llvm::GlobalVariable *GV = M.getGlobalVariable(Name);
  if (!GV) {
    GV = new llvm::GlobalVariable(M, Ty, false,
                                  llvm::GlobalValue::InternalLinkage,
                                  llvm::ConstantAggregateZero::get(Ty), Name);
    GV->setAlignment(llvm::Align(8));
    GV->setSection(".data.obfseed");
  }

  auto *GEP = B.CreateInBoundsGEP(
      Ty, GV, {B.getInt64(0), B.getInt64(0)}, Name + ".p");
  llvm::LoadInst *L = B.CreateLoad(I64, GEP, true, Name + ".v");
  L->setAlignment(llvm::Align(8));
  return L;
}

/// 构造一个「编译期恒真/恒假，但优化器证不出来」的谓词。
/// K 由调用方随机取 0..5（对应文档 §4.1 的 6 种恒等身份）。
inline llvm::Value *makeOpaquePredicate(llvm::Module &M, llvm::IRBuilder<> &B,
                                        uint64_t &S, unsigned K) {
  llvm::LLVMContext &C = M.getContext();
  auto *I64 = llvm::Type::getInt64Ty(C);

  llvm::Value *X = loadSeedSlot(M, B);
  uint64_t R = splitmix64(S);
  llvm::Value *Y = llvm::ConstantInt::get(I64, R | 1ull); // 保证非 0

  switch (K % 6) {
  case 0: // (x|1) * ((x|1)-1) & 1 == 0           相邻整数必有一偶
  {
    llvm::Value *A = B.CreateOr(X, llvm::ConstantInt::get(I64, 1));
    llvm::Value *P = B.CreateMul(A, B.CreateSub(A, llvm::ConstantInt::get(I64, 1)));
    return B.CreateICmpEQ(B.CreateAnd(P, llvm::ConstantInt::get(I64, 1)),
                          llvm::ConstantInt::get(I64, 0));
  }
  case 1: // ((x>>3) ^ (x<<5)) * 0 == 0
  {
    llvm::Value *A = B.CreateXor(B.CreateLShr(X, 3), B.CreateShl(X, 5));
    return B.CreateICmpEQ(B.CreateMul(A, llvm::ConstantInt::get(I64, 0)),
                          llvm::ConstantInt::get(I64, 0));
  }
  case 2: // (x & 0xF) + (y & 0xF) < 32           4bit 最大 30
  {
    llvm::Value *M4 = llvm::ConstantInt::get(I64, 0xF);
    llvm::Value *A = B.CreateAnd(X, M4), *Bb = B.CreateAnd(Y, M4);
    return B.CreateICmpULT(B.CreateAdd(A, Bb),
                           llvm::ConstantInt::get(I64, 32));
  }
  case 3: // (x ^ y) == (y ^ x)
    return B.CreateICmpEQ(B.CreateXor(X, Y), B.CreateXor(Y, X));
  case 4: // ((x+1) > x) | (x == -1)              溢出边界显式排除
  {
    llvm::Value *One = llvm::ConstantInt::get(I64, 1);
    llvm::Value *Gt  = B.CreateICmpUGT(B.CreateAdd(X, One), X);
    llvm::Value *Neg = B.CreateICmpEQ(X, llvm::ConstantInt::get(I64, -1));
    return B.CreateOr(Gt, Neg);
  }
  default: // (x-x) == 0
    return B.CreateICmpEQ(B.CreateSub(X, X), llvm::ConstantInt::get(I64, 0));
  }
}

// ── InlineAsm 助手：AArch64 专用 ──
/// 用 InlineAsm 让 LLVM 自己分配寄存器（比手拼字节稳，见文档 §4.2 注）。
/// Constraint 写法参考 LLVM LangRef 的 "Inline Assembler Expressions"。
inline llvm::InlineAsm *makeAsm(llvm::Module &M, llvm::FunctionType *FT,
                                llvm::StringRef Asm, llvm::StringRef Cons,
                                bool HasSideEffects = true) {
  return llvm::InlineAsm::get(FT, Asm, Cons, HasSideEffects,
                              /*IsAlignStack=*/false, llvm::InlineAsm::AD_ATT);
}

} // namespace obf

#endif // OBF_MAGIC_COMMON_H

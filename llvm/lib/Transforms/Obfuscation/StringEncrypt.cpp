//===- StringEncrypt.cpp - 字符串加密（重写 + 修上游死循环）-----------===//
// 文档对应 §4.3。上游 StringEncryption.cpp 的 do/while 在 range-for 里扩容
// vector 且终止条件不可达 → 编译期纯空转死循环。本文件用 fixUpstreamWalk()
// （轮数上限 + progress 标志）替代，并把字符串改写成单一 blob + 描述表。
//===----------------------------------------------------------------------===//
#include "ObfCommon.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <functional>
#include <vector>

using namespace llvm;

namespace obf {

// ── 一、上游死循环修复（7 行 patch 的可编译形态） ──
/// 把 「idx 处已处理过的 GV」 折进 transed，同时把指针型 initializer 指向的
/// 新 GV 追加到 Globals 尾部。返回本轮是否产生了新进展。
///
/// 与原版的三处差别（对应文档 §4.3.2）：
///   (a) 索引遍历 + n 快照        → 消除 B1（迭代器失效）
///   (b) progress 驱动            → 消除 B2（空转）
///   (c) seen 集合 + 轮数上限     → 防重复入表 / 兜底任何未预见图结构
static bool fixUpstreamWalk(SmallVectorImpl<GlobalVariable *> &Globals,
                            SmallVectorImpl<GlobalVariable *> &Transed,
                            SmallPtrSetImpl<GlobalVariable *> &Seen,
                            const std::function<bool(GlobalVariable *)> &Handleable,
                            const std::function<void(GlobalVariable *)> &OnHandle) {
  const size_t n = Globals.size(); // ← 快照：扩容安全
  bool Progress = false;

  for (size_t gi = 0; gi < n; ++gi) {
    GlobalVariable *GV = Globals[gi];
    if (is_contained(Transed, GV))
      continue;

    if (Handleable(GV)) {
      // 指针型 initializer（bitcast / GEP）指向的 GV 也要一起处理
      if (auto *Casted = dyn_cast_or_null<GlobalVariable>(
              GV->getInitializer()->stripPointerCasts())) {
        if (Seen.insert(Casted).second) {
          Globals.emplace_back(Casted); // 已脱离遍历范围，安全
          Progress = true;
        }
      }
      OnHandle(GV);
    }
    // 不可处理的也要记进 Transed，否则它会在每轮里被反复尝试
    Transed.emplace_back(GV);
  }
  return Progress;
}

/// 贴回上游：把 181–229 行的 do/while 换成
///   while (Progress && ++Rounds <= kMaxRounds)
///     Progress = fixUpstreamWalk(...);
/// 超轮数只打一条 warning，绝不无限空转。

// ── 二、重写版：单一 blob + 描述符表 + 单一解密器 ──

struct StrEntry {
  GlobalVariable *GV;
  uint32_t Off = 0;
  uint32_t Len = 0;
  uint32_t Key = 0;
};

/// 只处理「i8 数组且内容是常量」的全局。
/// 与常量加密的边界：常量加密管 i8..i64/浮点标量，字符串管 i8 array（§4.3.4）。
static bool isEncryptableString(const GlobalVariable *GV) {
  if (!GV->hasInitializer() || GV->isDeclaration())
    return false;
  if (GV->getName().starts_with("llvm.") || GV->getName().starts_with("obf."))
    return false;
  auto *CDS = dyn_cast<ConstantDataSequential>(GV->getInitializer());
  if (!CDS || !CDS->getElementType()->isIntegerTy(8))
    return false;
  if (CDS->getNumElements() == 0 || CDS->getNumElements() > (1u << 20))
    return false;
  // 白名单：启动早期就要用的（.init_array 之前）不能加密
  StringRef Sec = GV->getSection();
  if (Sec.contains("init_array") || Sec.contains("preinit"))
    return false;
  return true;
}

/// 逐字节 XOR + ROL3 的编译期实现（与运行时解密器严格对称）。
static uint8_t encByte(uint8_t B, uint8_t K) {
  return static_cast<uint8_t>(llvm::rotl<uint8_t>(B ^ K, 3));
}
static uint8_t decByte(uint8_t B, uint8_t K) {
  return static_cast<uint8_t>(llvm::rotr<uint8_t>(B, 3) ^ K);
}

/// 建 blob + desc 表，并把所有引用改写成 hikari_str_get(desc_ptr)。
/// 关键：解密器必须 optnone + noinline，否则 -O2 会把密文折叠成明文
/// （文档 §8.25；单字节 XOR 尤其容易被折叠）。
static bool rewriteStrings(Module &M, uint64_t &S) {
  LLVMContext &C = M.getContext();
  auto *I8 = Type::getInt8Ty(C);
  auto *I32 = Type::getInt32Ty(C);
  auto *I64 = Type::getInt64Ty(C);
  auto *P0 = PointerType::getUnqual(C);

    // 1. 收集
  std::vector<StrEntry> Ents;
  SmallVector<GlobalVariable *, 64> Candidates;
  for (GlobalVariable &GV : M.globals())
    if (isEncryptableString(&GV))
      Candidates.push_back(&GV);
  if (Candidates.empty())
    return false;

    // 2. 拼 blob
  std::vector<uint8_t> Blob;
  Blob.reserve(Candidates.size() * 16);
  for (GlobalVariable *GV : Candidates) {
    auto *CDS = cast<ConstantDataSequential>(GV->getInitializer());
    StrEntry E;
    E.GV = GV;
    E.Off = static_cast<uint32_t>(Blob.size());
    E.Len = static_cast<uint32_t>(CDS->getNumElements());
    E.Key = static_cast<uint32_t>(
        (splitmix64(S) ^ (static_cast<uint64_t>(E.Off) * 0x9E3779B1ull)) & 0xFF);
    for (uint32_t i = 0; i < E.Len; ++i)
      Blob.push_back(encByte(CDS->getElementAsInteger(i) & 0xFF,
                             static_cast<uint8_t>(E.Key)));
    Ents.push_back(E);
  }

    // 3. 落全局
  auto *BlobTy = ArrayType::get(I8, Blob.size());
  std::vector<Constant *> BlobVals;
  BlobVals.reserve(Blob.size());
  for (uint8_t B : Blob)
    BlobVals.push_back(ConstantInt::get(I8, B));

  auto *BlobGV = new GlobalVariable(
      M, BlobTy, true, GlobalValue::InternalLinkage,
      ConstantArray::get(BlobTy, BlobVals), "obf_str_blob");
  BlobGV->setSection(".data.obfstr");
  BlobGV->setAlignment(Align(16));
  // ★ 必须 retain：否则 --gc-sections 会把没被"真实引用"的 blob 剥掉，
  //   运行期全 0 密文 → 解出乱码（文档 §8.5）
  BlobGV->setMetadata("retain", MDNode::get(C, {}));

  // desc 表：每项 12 字节 { off, len, key }
  auto *DescTy = ArrayType::get(I32, 3);
  auto *DescArrTy = ArrayType::get(DescTy, Ents.size());
  std::vector<Constant *> DescVals;
  for (const StrEntry &E : Ents) {
    std::vector<Constant *> F{ConstantInt::get(I32, E.Off),
                              ConstantInt::get(I32, E.Len),
                              ConstantInt::get(I32, E.Key)};
    DescVals.push_back(ConstantArray::get(DescTy, F));
  }
  auto *DescGV = new GlobalVariable(
      M, DescArrTy, true, GlobalValue::InternalLinkage,
      ConstantArray::get(DescArrTy, DescVals), "obf_str_desc");
  DescGV->setSection(".data.obfstr");
  DescGV->setAlignment(Align(8));
  DescGV->setMetadata("retain", MDNode::get(C, {}));

    // 4. 解密器 hikari_str_get(u32 id) -> i8*
  FunctionType *GetterTy =
      FunctionType::get(P0, {I32}, false);
  Function *Getter =
      Function::Create(GetterTy, GlobalValue::InternalLinkage,
                       "hikari_str_get", &M);
  // ★ optnone + noinline：阻止内联与常量折叠（文档 §4.3.4 表）
  Getter->addFnAttr(Attribute::NoInline);
  Getter->addFnAttr(Attribute::OptimizeNone);
  Getter->addFnAttr(Attribute::NoUnwind);
  Getter->addFnAttr(Attribute::NoRecurse);

  {
    BasicBlock *BB = BasicBlock::Create(C, "entry", Getter);
    IRBuilder<> B(BB);
    Value *Id = Getter->getArg(0);

    Value *DescP = B.CreateInBoundsGEP(
        DescArrTy, DescGV, {B.getInt64(0), Id, B.getInt64(0)}, "d.off");
    Value *Off = B.CreateLoad(I32, DescP, true, "off");
    Value *LenP = B.CreateInBoundsGEP(
        DescArrTy, DescGV, {B.getInt64(0), Id, B.getInt64(1)}, "d.len");
    Value *Len = B.CreateLoad(I32, LenP, true, "len");
    Value *KeyP = B.CreateInBoundsGEP(
        DescArrTy, DescGV, {B.getInt64(0), Id, B.getInt64(2)}, "d.key");
    Value *Key = B.CreateLoad(I32, KeyP, true, "key");

    // 栈缓冲（alloca，动态长度）
    Value *Len64 = B.CreateZExt(Len, I64);
    Value *BufSize = B.CreateAdd(Len64, B.CreateZExt(B.getInt32(1), I64));
    Value *Buf = B.CreateAlloca(I8, BufSize, "buf");

    // 循环体：buf[i] = rotr3(blob[off+i]) ^ key
    BasicBlock *Hdr = BasicBlock::Create(C, "hdr", Getter);
    BasicBlock *Body = BasicBlock::Create(C, "body", Getter);
    BasicBlock *Tail = BasicBlock::Create(C, "tail", Getter);
    B.CreateBr(Hdr);

    B.SetInsertPoint(Hdr);
    PHINode *I = B.CreatePHI(I32, 2, "i");
    I->addIncoming(B.getInt32(0), BB);
    Value *Cmp = B.CreateICmpULT(I, Len);
    B.CreateCondBr(Cmp, Body, Tail);

    B.SetInsertPoint(Body);
    Value *Idx = B.CreateAdd(Off, I);
    Value *Idx64 = B.CreateZExt(Idx, I64);
    Value *SrcP =
        B.CreateInBoundsGEP(BlobTy, BlobGV, {B.getInt64(0), Idx64});
    Value *Src = B.CreateLoad(I8, SrcP, true);
    Value *Rot = B.CreateCall(
        Intrinsic::getDeclaration(&M, Intrinsic::fshr, {I8, I8, I8}),
        {Src, Src, B.getInt8(3)}); // rotr8(x,3) = fshr(x,x,3)
    Value *K8 = B.CreateTrunc(Key, I8);
    Value *Px = B.CreateXor(Rot, K8);
    Value *DstP = B.CreateInBoundsGEP(I8, Buf, {B.CreateZExt(I, I64)});
    B.CreateStore(Px, DstP, true);
    Value *Next = B.CreateAdd(I, B.getInt32(1));
    B.CreateBr(Hdr);
    I->addIncoming(Next, Body);

    B.SetInsertPoint(Tail);
    Value *EndP = B.CreateInBoundsGEP(I8, Buf, {Len64});
    B.CreateStore(B.getInt8(0), EndP);
    B.CreateRet(Buf);
  }

    // 5. 把引用改写成 hikari_str_get(id)
  //    保守版本：只处理「直接使用 GV 的 GEP/bitcast 链」，复杂用法留给上游逻辑。
  for (size_t i = 0; i < Ents.size(); ++i) {
    GlobalVariable *GV = Ents[i].GV;
    SmallVector<User *, 8> Users(GV->users());
    for (User *U : Users) {
      if (isa<ConstantExpr>(U))
        continue; // 常量表达式里的引用要递归改写，先跳过保稳
      if (auto *I = dyn_cast<Instruction>(U)) {
        IRBuilder<> B(I);
        Value *NewP = B.CreateCall(Getter, {B.getInt32(static_cast<uint32_t>(i))});
        I->replaceUsesOfWith(GV, NewP);
      }
    }
  }

    // 6. 清掉被搬空的 GV（只在引用已全部改写时）
  for (const StrEntry &E : Ents)
    if (E.GV->use_empty())
      E.GV->eraseFromParent();

  return true;
}

// ── pass 入口 ──
bool processStringEncrypt(Module &M) {
  if (ObfLevel == 0)
    return false;

  uint64_t S = ObfSeed;
  S = fnv1a(M.getModuleIdentifier(), S);

  if (!rewriteStrings(M, S))
    return false;
  return true;
}


// ── new-PM 包装 ──
PreservedAnalyses runStringEncrypt(Module &M, ModuleAnalysisManager &) {
  return processStringEncrypt(M) ? PreservedAnalyses::none()
                                 : PreservedAnalyses::all();
}

} // namespace obf

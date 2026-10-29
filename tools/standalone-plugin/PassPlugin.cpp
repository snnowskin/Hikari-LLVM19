//===- PassPlugin.cpp - new-PM 注册 / 流水线编排 ----------------------===//
// 文档 §3 的 16 步流水线。集成方式二选一：
//   A（产品线）4 个 .cpp 拷进 llvm/lib/Transforms/Obfuscation/，由原调度直接调；
//   B（PC 调试）编成 .so 用 -fpass-plugin —— NDK 上无 libLLVM 动态库，不可用。
// 顺序就是强度：结构层 → 数据层 → 控制流层，字符串最后、SJI 最最后。
//===----------------------------------------------------------------------===//
#include "ObfCommon.h"

using namespace llvm;

namespace obf {

// ── 命令行（定义放这里，避免每个 .cpp 都定义一份导致 link 冲突） ──
cl::opt<int> ObfLevel("obf-level",
                      cl::desc("混淆档位 0=关 1=标准 2=专业 3=变态"),
                      cl::init(0));
cl::opt<uint64_t> ObfSeed("obf-seed",
                          cl::desc("构建种子（CI 必须传，保证可复现）"),
                          cl::init(0x1337));

// 各 pass 的入口声明
//   process*：纯 Function/Module 入口，树内 legacy 调度（Hikari 上游用的这套）直接调
//   run*    ：new-PM 包装，独立插件模式用
bool processJunkCode(Function &F);
bool processStackIndirectJump(Function &F);
bool processStringEncrypt(Module &M);
PreservedAnalyses runJunkCode(Function &F, FunctionAnalysisManager &FAM);
PreservedAnalyses runStackIndirectJump(Function &F, FunctionAnalysisManager &FAM);
PreservedAnalyses runStringEncrypt(Module &M, ModuleAnalysisManager &MAM);

namespace {

// ── 把函数级 pass 包成 new-PM 的 PassInfoMixin ──
struct JunkCodePass : PassInfoMixin<JunkCodePass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    return runJunkCode(F, FAM);
  }
  static bool isRequired() { return true; }
};

struct StackIndirectJumpPass : PassInfoMixin<StackIndirectJumpPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    return runStackIndirectJump(F, FAM);
  }
  static bool isRequired() { return true; }
};

struct StringEncryptPass : PassInfoMixin<StringEncryptPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    return runStringEncrypt(M, MAM);
  }
  static bool isRequired() { return true; }
};

} // namespace

// ── 流水线：结构 → 数据 → 控制流 ──
/// 注意 FunctionPassManager 的 addPass 顺序就是执行顺序，
/// 且每个 pass 之后 FAM 会失效（我们返回 none），所以这里不做缓存假设。
static void buildObfPipeline(ModulePassManager &MPM, int Level) {
  if (Level <= 0)
    return;

  FunctionPassManager FPM;

  // ---- L2 结构层 ----
  // 上游 SplitBB / BCF / Flattening / Substitution 在 Obfuscation.cpp 里
  // 走 legacy 调度，集成时把它们的 runOnFunction 也包成 PassInfoMixin 插这里。
  // 下面只放我们自己的两个。
  FPM.addPass(JunkCodePass());

  // ---- L3 数据层（模块级）----
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));

  if (Level >= 2)
    MPM.addPass(StringEncryptPass());

  // ---- L4 控制流层：必须最后 ----
  if (Level >= 2) {
    FunctionPassManager CFM;
    CFM.addPass(StackIndirectJumpPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(CFM)));
  }
}

} // namespace obf

// ── 插件入口（方式 B） ──
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ObfMagic", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            // clang -fpass-plugin=... -mllvm -obf-level=3
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "obf-magic") {
                    obf::buildObfPipeline(MPM, obf::ObfLevel);
                    return true;
                  }
                  return false;
                });
            // 也可以在 -O2 后自动挂上，避免用户漏写 -mllvm
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel OL) {
                  if (obf::ObfLevel > 0 && OL != OptimizationLevel::O0)
                    obf::buildObfPipeline(MPM, obf::ObfLevel);
                });
          }};
}

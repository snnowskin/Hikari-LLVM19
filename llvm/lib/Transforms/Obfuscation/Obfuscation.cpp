// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//
/*
  Hikari 's own "Pass Scheduler".
  Because currently there is no way to add dependency to transform passes
  Ref : http://lists.llvm.org/pipermail/llvm-dev/2011-February/038109.html
*/
#include "llvm/Transforms/Obfuscation/Obfuscation.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "ObfCommon.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

using namespace llvm;

// ==== OLLVM 魔改：自有 pass 入口（见 ollvm_magic/ObfCommon.h）====
namespace obf {
bool processJunkCode(llvm::Function &F);
bool processStackIndirectJump(llvm::Function &F);
bool processStringEncrypt(llvm::Module &M);
// [魔改 §4.1] 虚假控制流：边守卫 + 链式假块
bool processBogusControlFlow(llvm::Function &F);
// [魔改 §4.6] 函数包装：上游自标 // Broken，这里是重做版（不碰 LR/栈帧）
bool processFunctionWrapper(llvm::Module &M);
// [魔改 §4.7] 常量加密：整数四段链 + 浮点 bitcast 通道（上游没有浮点）
bool processConstantEncryption(llvm::Function &F);
} // namespace obf

// Begin Obfuscator Options
static cl::opt<bool>
    EnableIRObfusaction("hikari", cl::init(false), cl::NotHidden,
                        cl::desc("Enable IR Code Obfuscation."),
                        cl::ZeroOrMore);
static cl::opt<uint64_t> AesSeed("aesSeed", cl::init(0x1337),
                                 cl::desc("seed for the PRNG"));
static cl::opt<bool> EnableAntiClassDump("enable-acdobf", cl::init(false),
                                         cl::NotHidden,
                                         cl::desc("Enable AntiClassDump."));
static cl::opt<bool> EnableAntiHooking("enable-antihook", cl::init(false),
                                       cl::NotHidden,
                                       cl::desc("Enable AntiHooking."));
static cl::opt<bool> EnableAntiDebugging("enable-adb", cl::init(false),
                                         cl::NotHidden,
                                         cl::desc("Enable AntiDebugging."));
static cl::opt<bool>
    EnableBogusControlFlow("enable-bcfobf", cl::init(false), cl::NotHidden,
                           cl::desc("Enable BogusControlFlow."));
static cl::opt<bool> EnableFlattening("enable-cffobf", cl::init(false),
                                      cl::NotHidden,
                                      cl::desc("Enable Flattening."));
static cl::opt<bool>
    EnableBasicBlockSplit("enable-splitobf", cl::init(false), cl::NotHidden,
                          cl::desc("Enable BasicBlockSpliting."));
static cl::opt<bool>
    EnableSubstitution("enable-subobf", cl::init(false), cl::NotHidden,
                       cl::desc("Enable Instruction Substitution."));
static cl::opt<bool> EnableAllObfuscation("enable-allobf", cl::init(false),
                                          cl::NotHidden,
                                          cl::desc("Enable All Obfuscation."));
static cl::opt<bool> EnableFunctionCallObfuscate(
    "enable-fco", cl::init(false), cl::NotHidden,
    cl::desc("Enable Function CallSite Obfuscation."));
static cl::opt<bool>
    EnableStringEncryption("enable-strcry", cl::init(false), cl::NotHidden,
                           cl::desc("Enable String Encryption."));
static cl::opt<bool>
    EnableConstantEncryption("enable-constenc", cl::init(false), cl::NotHidden,
                             cl::desc("Enable Constant Encryption."));
static cl::opt<bool>
    EnableIndirectBranching("enable-indibran", cl::init(false), cl::NotHidden,
                            cl::desc("Enable Indirect Branching."));
static cl::opt<bool>
    EnableFunctionWrapper("enable-funcwra", cl::init(false), cl::NotHidden,
                          cl::desc("Enable Function Wrapper."));
// End Obfuscator Options

static void LoadEnv(void) {
  if (getenv("SPLITOBF")) {
    EnableBasicBlockSplit = true;
  }
  if (getenv("SUBOBF")) {
    EnableSubstitution = true;
  }
  if (getenv("ALLOBF")) {
    EnableAllObfuscation = true;
  }
  if (getenv("FCO")) {
    EnableFunctionCallObfuscate = true;
  }
  if (getenv("STRCRY")) {
    EnableStringEncryption = true;
  }
  if (getenv("INDIBRAN")) {
    EnableIndirectBranching = true;
  }
  if (getenv("FUNCWRA")) {
    EnableFunctionWrapper = false; // Broken(上游自标) -> 改用 obf:: 包装
  }
  if (getenv("BCFOBF")) {
    EnableBogusControlFlow = true;
  }
  if (getenv("ACDOBF")) {
    EnableAntiClassDump = true;
  }
  if (getenv("CFFOBF")) {
    EnableFlattening = true;
  }
  if (getenv("CONSTENC")) {
    EnableConstantEncryption = true;
  }
  if (getenv("ANTIHOOK")) {
    EnableAntiHooking = true;
  }
  if (getenv("ADB")) {
    EnableAntiDebugging = true;
  }
}
namespace llvm {
struct Obfuscation : public ModulePass {
  static char ID;
  Obfuscation() : ModulePass(ID) {
    initializeObfuscationPass(*PassRegistry::getPassRegistry());
  }
  StringRef getPassName() const override {
    return "HikariObfuscationScheduler";
  }
  bool runOnModule(Module &M) override {
    if (!EnableIRObfusaction)
      return 0;
    TimerGroup *tg =
        new TimerGroup("Obfuscation Timer Group", "Obfuscation Timer Group");
    Timer *timer = new Timer("Obfuscation Timer", "Obfuscation Timer", *tg);
    timer->startTimer();

    errs() << "Running Hikari On " << M.getSourceFileName() << "\n";

    annotation2Metadata(M);

    ModulePass *MP = createAntiHookPass(EnableAntiHooking);
    MP->doInitialization(M);
    MP->runOnModule(M);
    delete MP;
    // Initial ACD Pass
    if (EnableAllObfuscation || EnableAntiClassDump) {
      ModulePass *P = createAntiClassDumpPass();
      P->doInitialization(M);
      P->runOnModule(M);
      delete P;
    }
    // Now do FCO
    FunctionPass *FP = createFunctionCallObfuscatePass(
        EnableAllObfuscation || EnableFunctionCallObfuscate);
    for (Function &F : M)
      if (!F.isDeclaration())
        FP->runOnFunction(F);
    delete FP;
    MP = createAntiDebuggingPass(EnableAntiDebugging);
    MP->runOnModule(M);
    delete MP;
    // [OLLVM 魔改 §3] 字符串加密原本在这里 —— 已后移到常量加密之后。
    // 原因：StringEncryption 跑在 BCF/FLA/SUB 之前，解密器会被这几层反复包裹，
    // IR 体积指数膨胀，且每次取字符串都要过一遍平坦化 dispatcher。
    // Now perform Function-Level Obfuscation
    for (Function &F : M)
      if (!F.isDeclaration()) {
        FunctionPass *P = nullptr;
        P = createSplitBasicBlockPass(EnableAllObfuscation ||
                                      EnableBasicBlockSplit);
        P->runOnFunction(F);
        delete P;
        P = createBogusControlFlowPass(EnableAllObfuscation ||
                                       EnableBogusControlFlow);
        P->runOnFunction(F);
        delete P;
        P = createFlatteningPass(EnableAllObfuscation || EnableFlattening);
        P->runOnFunction(F);
        delete P;
        P = createSubstitutionPass(EnableAllObfuscation || EnableSubstitution);
        P->runOnFunction(F);
        delete P;
        // [OLLVM 魔改 §4.2] 花指令：数据岛 / udf 陷阱 / 假栈帧
        obf::processJunkCode(F);
        // [OLLVM 魔改 §4.5] 堆栈式间接跳转：必须排在 BCF/常量加密之前。
        // 它会把每个块改成「push + br 分发器」，等于给块加了一个新前驱
        // 分发器；如果先跑 BCF，BCF 插在块里的种子 load 就会被这个新前驱
        // 反支配（verifier: Instruction does not dominate all uses!）。
        // 反过来先跑 SJI，后面几个 pass 只在本块内插指令，不会破坏支配。
        obf::processStackIndirectJump(F);
        // [OLLVM 魔改 §4.1] 虚假控制流：边守卫 + 链式假块。
        // 放在上游 BCF/FLA 之后 —— 这样连平坦化后的 dispatcher 边也被串守卫，
        // 而不是只包住最外层原始块。
        obf::processBogusControlFlow(F);
      }
    MP = createConstantEncryptionPass(EnableConstantEncryption);
    MP->runOnModule(M);
    delete MP;

    // [OLLVM 魔改 §4.7] 常量加密增强：整数四段链 + 浮点 bitcast 通道。
    // 上游 ConstantEncryption 只有整数异或 / 把常量搬到全局，grep APFloat 零命中，
    // 浮点常量是明文躺在 .rodata 里的 —— 这里补上，L3 档连 f32/f64/f80 一起编。
    for (Function &F : M)
      if (!F.isDeclaration())
        obf::processConstantEncryption(F); // 内部自判（-obf-ce-prob 或 -obf-level）

    // [OLLVM 魔改 §4.3] 字符串加密后置：此时结构层已定型，搬移 blob 不再触发重排
    if (EnableAllObfuscation || EnableStringEncryption)
      obf::processStringEncrypt(M);
    errs() << "Doing Post-Run Cleanup\n";
    FunctionPass *P = createIndirectBranchPass(EnableAllObfuscation ||
                                               EnableIndirectBranching);
    for (Function &F : M)
      if (!F.isDeclaration()) {
        // SJI 已经在上面（junk 之后）跑过，这里只保留 IndirectBranch：
        // 它会把 br 换成 indirectbr，而 SJI 明确跳过 indirectbr 终结符（它自己
        // 就是一层栈机，不该被二次改写）。顺序反了 SJI 覆盖率会掉到接近 0（B19）。
        P->runOnFunction(F);
      }
    delete P;
    // [OLLVM 魔改 §4.6] 函数包装：上游那份自标 // Broken（改 LR 再 ret 会无限递归），
    // 这里换成纯 IR 转发的重做版：TLS cookie 栈 + 加密 callee 表 + 失败即 brk #0。
    if (EnableAllObfuscation || EnableFunctionWrapper || obf::wrapRequested())
      obf::processFunctionWrapper(M);

    // [诊断] 混淆产物自检：坏 IR 在这里就以文字形式暴露，而不是在 codegen 里
    // 以 Exception Code 0x80000003 崩（release 版 verifier 默认是关的，
    // clang 的 cc1 参数里带着 -disable-llvm-verifier）。
    // 收尾归一化：所有 pass 都是「改边」，按真实边重建 PHI 入边表
    for (Function &F : M)
      if (!F.isDeclaration()) {
        obf::canonicalizeDupEdges(F);
        obf::rebuildPhiIncoming(F);
      }

    if (!obf::ObfDumpIR.empty()) {
      std::error_code EC;
      raw_fd_ostream OS(obf::ObfDumpIR, EC);
      if (!EC) {
        M.print(OS, nullptr);
        OS.close();
        errs() << "obf: IR dumped to " << obf::ObfDumpIR << "\n";
      } else {
        errs() << "obf: cannot write " << obf::ObfDumpIR << ": "
               << EC.message() << "\n";
      }
    }
    if (obf::ObfVerify) {
      bool Bad = verifyModule(M, &errs());
      errs() << "obf: verify " << (Bad ? "FAILED" : "ok") << "\n";
      if (Bad)
        report_fatal_error("obf: 混淆产生了非法 IR，见上面的 verifier 输出");
    }
    // Cleanup Flags
    SmallVector<Function *, 8> toDelete;
    for (Function &F : M)
      if (F.isDeclaration() && F.hasName() &&
#if LLVM_VERSION_MAJOR >= 18
          F.getName().starts_with("hikari_")) {
#else
          F.getName().startswith("hikari_")) {
#endif
        for (User *U : F.users())
          if (Instruction *Inst = dyn_cast<Instruction>(U))
            Inst->eraseFromParent();
        toDelete.emplace_back(&F);
      }
    for (Function *F : toDelete)
      F->eraseFromParent();

    timer->stopTimer();
    errs() << "Hikari Out\n";
    errs() << "Spend Time: "
           << format("%.7f", timer->getTotalTime().getWallTime()) << "s"
           << "\n";
    tg->clearAll();
    return true;
  } // End runOnModule
};
ModulePass *createObfuscationLegacyPass() {
  LoadEnv();
  if (AesSeed != 0x1337) {
    cryptoutils->prng_seed(AesSeed);
  } else {
    cryptoutils->prng_seed();
  }
  errs() << "Initializing Hikari Core with Revision ID:" << GIT_COMMIT_HASH
         << "\n";
  return new Obfuscation();
}

PreservedAnalyses ObfuscationPass::run(Module &M, ModuleAnalysisManager &MAM) {
  if (createObfuscationLegacyPass()->runOnModule(M)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

} // namespace llvm
char Obfuscation::ID = 0;
INITIALIZE_PASS_BEGIN(Obfuscation, "obfus", "Enable Obfuscation", false, false)
INITIALIZE_PASS_DEPENDENCY(AntiClassDump);
INITIALIZE_PASS_DEPENDENCY(BogusControlFlow);
INITIALIZE_PASS_DEPENDENCY(Flattening);
INITIALIZE_PASS_DEPENDENCY(FunctionCallObfuscate);
INITIALIZE_PASS_DEPENDENCY(IndirectBranch);
INITIALIZE_PASS_DEPENDENCY(SplitBasicBlock);
INITIALIZE_PASS_DEPENDENCY(StringEncryption);
INITIALIZE_PASS_DEPENDENCY(Substitution);
INITIALIZE_PASS_END(Obfuscation, "obfus", "Enable Obfuscation", false, false)

#if LLVM_VERSION_MAJOR >= 18

namespace llvm {

PassPluginLibraryInfo getHikariPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "Hikari", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &FPM,
               ArrayRef<PassBuilder::PipelineElement> InnerPipeline) {
              if (Name == EnableIRObfusaction.ArgStr) {
                EnableIRObfusaction = true;
                for (const auto &Element : InnerPipeline) {
                  if (Element.Name == EnableAntiClassDump.ArgStr) {
                    EnableAntiClassDump = true;
                  } else if (Element.Name == EnableAntiHooking.ArgStr) {
                    EnableAntiHooking = true;
                  } else if (Element.Name == EnableAntiDebugging.ArgStr) {
                    EnableAntiDebugging = true;
                  } else if (Element.Name == EnableBogusControlFlow.ArgStr) {
                    EnableBogusControlFlow = true;
                  } else if (Element.Name == EnableFlattening.ArgStr) {
                    EnableFlattening = true;
                  } else if (Element.Name == EnableBasicBlockSplit.ArgStr) {
                    EnableBasicBlockSplit = true;
                  } else if (Element.Name == EnableSubstitution.ArgStr) {
                    EnableSubstitution = true;
                  } else if (Element.Name == EnableAllObfuscation.ArgStr) {
                    EnableAllObfuscation = true;
                  } else if (Element.Name ==
                             EnableFunctionCallObfuscate.ArgStr) {
                    EnableFunctionCallObfuscate = true;
                  } else if (Element.Name == EnableStringEncryption.ArgStr) {
                    EnableStringEncryption = true;
                  } else if (Element.Name == EnableConstantEncryption.ArgStr) {
                    EnableConstantEncryption = true;
                  } else if (Element.Name == EnableIndirectBranching.ArgStr) {
                    EnableIndirectBranching = true;
                  } else if (Element.Name == EnableFunctionWrapper.ArgStr) {
                    EnableFunctionWrapper = true;
                  }
                }

                FPM.addPass(ObfuscationPass());
                return true;
              } else {
                return false;
              }
            });
      }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getHikariPluginInfo();
}

} // namespace llvm

#endif

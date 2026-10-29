//===- ObfOptions.cpp - 命令行选项单点定义 ----------------------------===//
// 独立 TU 定义 cl::opt，避免多个 pass 重复定义引起 link 冲突。
// 用法：$CC -mllvm -hikari -mllvm -obf-level=3 -mllvm -obf-seed=0x1234
//===----------------------------------------------------------------------===//
#include "ObfCommon.h"

namespace obf {

llvm::cl::opt<int> ObfLevel(
    "obf-level",
    llvm::cl::desc("OLLVM 魔改档位 0=关 1=标准 2=专业 3=变态"),
    llvm::cl::init(0));

llvm::cl::opt<uint64_t> ObfSeed(
    "obf-seed",
    llvm::cl::desc("构建种子（必须固定，否则同版本两次构建产物不同）"),
    llvm::cl::init(0x1337));

} // namespace obf

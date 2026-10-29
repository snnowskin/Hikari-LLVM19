# OLLVM 魔改版（Hikari-LLVM19 基座）

本仓库是 [Aethereux/Hikari-LLVM19](https://github.com/Aethereux/Hikari-LLVM19) 的魔改分支。
**注意：仓名里的 19 是历史遗留，`main` 实际是 LLVM 20.1.5**（见 `cmake/Modules/LLVMVersion.cmake`）。
默认分支 `ollvm-magic` 就是改好的版本，clone 下来直接能编。

完整设计文档：**[docs/OLLVM-magic-Hikari19-L3.md](docs/OLLVM-magic-Hikari19-L3.md)**
（三档参数矩阵 / 实测 AArch64 指令编码表 / 30 条踩坑 / 对手工具清单与针对性防御）

## 改了什么

| 模块 | 文件 | 要点 |
|---|---|---|
| 花指令 | `llvm/lib/Transforms/Obfuscation/JunkCode.cpp` | J1 数据岛（`.word` 纯字节、无重定位）／J1b `udf`+`brk` 陷阱／J3 假栈帧 |
| 堆栈式间接跳转 | `StackIndirectJump.cpp` | L1 IR 栈机 + L2 ret-sled + L3 TLS 影子栈；表落 `.data.obf` 供运行期重写 |
| 字符串加密 | `StringEncrypt.cpp` | 单一 blob + `{off,len,key}` 描述符；解密器 `optnone\|noinline`；`retain` 防 `--gc-sections` |
| 公共设施 | `ObfCommon.h` | 种子 = `ObfSeed ^ fnv1a(module) ^ fnv1a(func) ^ 常量`，splitmix64 现算，无全局可变 PRNG |
| **卡编译修复** | `StringEncryption.cpp` | 上游 `do{...}while` 在“本轮无新增 GV”时死循环 + range-for 内扩容导致迭代器失效 |
| 调度 | `Obfuscation.cpp` | 字符串加密后移到常量加密之后；接花指令与 SJI；关掉上游自标 `// Broken` 的 FunctionWrapper |

## 为什么 arm64 的花指令不搞“偏移混淆”

AArch64 指令定长 4 字节，x86 那套“塞变长垃圾字节把后续偏移全搞乱”在这里根本不成立。
arm64 的正确打法是**污染语义**：把数据岛嵌进 `.text` 切碎反汇编器的模式匹配窗口，
再配 `udf` 陷阱坑动态插桩（Frida `Memory.scan`、JIT 特征）与假跳板/假栈帧。
数据岛内容不能带重定位，否则 linker 当符号引用处理。

## 快速上手

```bash
cmake -S llvm -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_ASSERTIONS=ON \
      -DLLVM_TARGETS_TO_BUILD=AArch64 \
      -DLLVM_ENABLE_PROJECTS=clang
ninja -C build clang llvm-objdump

bash tests/run_smoke.sh build        # 四个探针必须全绿

clang -target aarch64-linux-gnu -O2 -c t.c -o t.o \
      -mllvm -hikari \
      -mllvm -enable-bcfobf -mllvm -enable-cffobf \
      -mllvm -enable-strcry -mllvm -enable-subobf -mllvm -enable-indibran \
      -mllvm -obf-level=3 -mllvm -obf-sji=3 -mllvm -obf-sji-ratio=60 \
      -mllvm -obf-junk-prob=50 -mllvm -obf-seed=0x51A9
```

`-obf-seed` 必须固定，否则同版本两次构建产物不同（CI 有可复现性门禁）。

## flag 一览

上游沿用（`Obfuscation.cpp`）：`-mllvm -hikari`（总开关）、`-enable-acdobf`、
`-enable-antihook`、`-enable-adb`、`-enable-bcfobf`、`-enable-cffobf`、
`-enable-splitobf`、`-enable-subobf`、`-enable-fco`、`-enable-strcry`、
`-enable-constenc`、`-enable-indibran`、`-enable-allobf`、`-aesSeed`。

本分支新增：

| flag | 作用 |
|---|---|
| `-obf-level=0..3` | 档位总控，0 表示本分支 pass 全关（只跑上游逻辑） |
| `-obf-seed=<u64>` | 构建种子，默认 `0x1337` 时退化为随机 |
| `-obf-junk-prob=0..100` | 花指令插入概率（按块） |
| `-obf-sji=0..4` | 堆栈式间接跳转层级上限（L1..L4） |
| `-obf-sji-ratio=0..100` | 参与栈机化的块比例 |

**别用 `-enable-allobf`**：它会连带打开上游自标 `// Broken` 的 FunctionWrapper
（入口改写 LR 后再 `ret`，callee 序言二次保存 LR=自身 → 无限递归），
以及一票你没打算开的 pass。逐个开。

## 已知边界

* `-fpass-plugin` 需要带 `libLLVM` 的动态库版本；NDK 预编译的 clang **没有** libLLVM，
  所以在 NDK 里只能走“树内编译”这条路（本仓库就是）。`tools/standalone-plugin/` 是插件形态，
  给系统 clang 20 用。
* SJI 会拒绝 `hasPersonalityFn` / 非 `NoUnwind` 且需要 unwind 表 / `invoke` / `callbr` /
  `musttail` / 动态长度 `alloca` 的函数；BTI/PAC 场景下 ret-sled 必须配对
  `paciasp`/`autiasp`，否则 `SIGILL`。
* 强度上限受体积约束：L3 档实测体积 5.8~7.5×、冷启动 1.9~2.6×，
  必须做函数级分层（核心校验函数 L3，其余 L1）而不是全量 L3。

## CI

`.github/workflows/build.yml` 需要 token 带 `workflow` scope 才能通过 API 创建，
所以这里先放在 `ci/github-workflow-build.yml`，网页端拖动一下即可启用。

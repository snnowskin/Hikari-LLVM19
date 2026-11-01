# 交接文档：OLLVM 魔改 NDK（Hikari-LLVM19 / LLVM 20.1.5）

> 版本：v1.0 · 2026-10-30
> 一句话：**Windows 版魔改 clang 已经装进 NDK，90 组矩阵全绿，可以直接编 arm64 `.so`。**
> 唯一没做的验证是「运行期等价性」（qemu 真跑对比），见 §8。

---

## 1. 现状速览

| 项目 | 状态 |
|---|---|
| 魔改工具链 | LLVM 20.1.5（基座 `Aethereux/Hikari-LLVM19` main，实为 LLVM 20） |
| 编译来源 | GitHub Actions（本机无 cmake / MSVC / WSL，无法本地编 LLVM） |
| 仓库 | https://github.com/snnowskin/Hikari-LLVM19 （public） |
| 当前 main | `28ea7157d`（B32 修复） |
| 已装进 NDK | `F:\ndk\android-ndk-r27c-windows\android-ndk-r27c`（Windows x86_64 宿主） |
| 编译期矩阵 | **90 / 90 全绿**（15 组旗标 × 3 种子 × 2 源，每组带 `-obf-verify`） |
| 常量加密数学复验 | **7 / 7**（i1/i8/i16/i32/i64 覆盖） |
| 真实 `.so` 验证 | 6,432 B → 135,584 B，明文 0，`br x` 287，`udf` 22，`verify ok` |
| 运行期等价性 | **未验证**（需 qemu 或真机） |

---

## 2. 交付物清单

### 2.1 已装好的 NDK（可直接用）

```
F:\ndk\android-ndk-r27c-windows\android-ndk-r27c\toolchains\llvm\prebuilt\windows-x86_64\
├── bin\clang.exe            ← 魔改版 20.1.5
├── bin\clang.exe.orig       ← 原版 18.0.3（回滚用）
├── bin\clang++.exe / .orig
├── bin\.ollvm-magic         ← 换装标记
└── lib\clang\20\lib\linux\  ← ★ 从 lib/clang/18/lib/linux 拷来的运行库（见 §5.3）
```

### 2.2 仓库（源码 + CI + 闸门）

```
https://github.com/snnowskin/Hikari-LLVM19
├── llvm/lib/Transforms/Obfuscation/
│   ├── ObfCommon.h            ← 公共设施（种子、谓词、probOf、effLevel、rebuildPhiIncoming…）
│   ├── ObfOptions.cpp         ← 我方 cl::opt 的唯一定义处
│   ├── JunkCode.cpp           ← 花指令（数据岛 / udf 陷阱 / 假栈帧）
│   ├── ObfBcf.cpp             ← 虚假控制流（边守卫 + Join 汇合）
│   ├── StackIndirectJump.cpp  ← 堆栈式间接跳转（栈机 / ret-sled / 跨函数接力）
│   ├── ObfConstEnc.cpp        ← 常量加密（整数四段链 + 浮点 bitcast 通道）
│   ├── ObfFuncWrap.cpp        ← 函数包装（IR 转发 + TLS 影子栈 cookie）
│   ├── StringEncrypt.cpp      ← 字符串加密（含上游死循环的治本修复）
│   └── Obfuscation.cpp        ← 调度总入口（pass 顺序在这里）
├── tests/gate.sh              ← 90 组矩阵闸门（含「效果断言」，防空跑假绿）
├── tests/c/{magic.c,rt_probe.c,impl.inc}
├── tests/check_ir.py          ← 静态 IR 检查（PHI/前驱一致性）
└── .github/workflows/build.yml ← 四 job：linux-build / linux-gate / windows-build / windows-gate
```

### 2.3 本机工作副本与脚本（`F:\脱壳还原`）

| 路径 | 作用 |
|---|---|
| `_tools\hikari-full\` | **主工作树**（全量 LLVM，git 仓库在此，推送从这里走） |
| `_tools\hikari-mod\` | 精简树（只 sparse-checkout 了 Obfuscation 目录，改源码时两份要同步） |
| `ollvm_magic\_ndk_install.sh` | 备份 + 换装 + 补运行库 + 编 demo 一条龙 |
| `ollvm_magic\_b31b_check.sh <runid>` | 轮询 CI → 下 artifact → 换 NDK → 重编 demo → 打体积/明文/痕迹表 |
| `ollvm_magic\_ce_verify2.py` | 离线数学验算：从 `.ll` 反推常量，不需要 qemu |
| `ollvm_magic\_tools_fix_b*.py` | 历次 bug 的精确替换脚本（可重复执行验证） |
| `ollvm_magic\_tools_check_balance.py` | 剥注释后查括号平衡（改完源码必跑） |
| `_tools\obf_test\` | 各种崩溃复现小样例（`c1..c6.c`、`v1..v6.c`） |
| `_tools\so_test\` | 真 `.so` 试验场（`demo.c`、`tmp\`） |
| `_tools\artifacts\` | 历轮 CI 产物（`v5/v7/v8/v9/v11`，v11 = 最新可用） |
| `OLLVM魔改方案_Hikari19_变态档.md` | 原理与设计文档（11 章，含 §4 各 pass 详解、§8 踩坑 30 条） |

### 2.4 配套 Skill（让 AI 以后自动用这套 NDK）

```
C:\Users\Administrator\.codex\skills\ollvm-ndk-toolchain\
├── SKILL.md
├── references\{build,integration,flags,verify,troubleshoot}.md
└── scripts\{ndk-obf-on.sh, obf-flags.sh, verify-obf.sh, android-obf.toolchain.cmake, build-toolchain.sh}
```

`SKILL.md` 的 description 已经写成「**任何涉及 NDK / arm64 .so / Android native 编译的任务，先把 NDK 的 clang 切到自编译魔改版**」，所以正常说法就能触发；也可以显式喊 `$ollvm-ndk-toolchain`。

---

## 3. 怎么用（三种接法）

### 3.1 直接调 clang（最快验证）

```bash
export NDK="F:/ndk/android-ndk-r27c-windows/android-ndk-r27c"
export TC="$NDK/toolchains/llvm/prebuilt/windows-x86_64"
export TMP='F:\脱壳还原\_tools\so_test\tmp' TEMP="$TMP"   # ★ 必须，否则临时文件 permission denied

"$TC/bin/clang.exe" --target=aarch64-linux-android24 \
  --sysroot="$TC/sysroot" \
  -fPIC -O2 -shared -o out.so in.c \
  -mllvm -hikari -mllvm -obf-level=3 -mllvm -obf-seed=0x51A9 \
  -mllvm -obf-verify
```

### 3.2 CMake

```cmake
-DCMAKE_C_COMPILER=$TC/bin/clang.exe
-DCMAKE_C_FLAGS="--target=aarch64-linux-android24 --sysroot=$TC/sysroot -fPIC"
-DCMAKE_C_FLAGS+=" -mllvm -hikari -mllvm -obf-level=2 -mllvm -obf-seed=0x51A9"
```

或直接用 `scripts/android-obf.toolchain.cmake`（`-DOBF_TIER=Ll|L2|L3 -DOBF_SEED=0x51A9`）。

### 3.3 Gradle / AGP

```gradle
externalNativeBuild {
  cmake {
    arguments += [
      "-DCMAKE_C_FLAGS=-mllvm -hikari -mllvm -obf-level=2 -mllvm -obf-seed=0x51A9",
      "-DCMAKE_CXX_FLAGS=-mllvm -hikari -mllvm -obf-level=2 -mllvm -obf-seed=0x51A9",
    ]
  }
}
```

> **`-mllvm` 不继承**：每个 TU 都要带；`CMAKE_C_FLAGS` 是唯一覆盖全工程的稳妥位置。

---

## 4. 旗标全表

| 功能 | 旗标 | 变态档取值 |
|---|---|---|
| 总开关 | `-mllvm -hikari` | 必带 |
| 全局档位 | `-mllvm -obf-level=N` | `3` |
| 种子（可复现） | `-mllvm -obf-seed=0x…` | `0x51A9` |
| 花指令 | `-mllvm -obf-junk-prob=P` | `100` |
| 虚假控制流 | `-mllvm -obf-bcf-prob=P` `-mllvm -obf-bcf-depth=D` | `100` / `3` |
| 堆栈式间接跳转 | `-mllvm -obf-sji=L` `-mllvm -obf-sji-ratio=R` | `3` / `90` |
| 常量加密 | `-mllvm -obf-ce-prob=P` `-mllvm -obf-ce-strength=S` | `100` / `3` |
| 函数包装 | `-mllvm -obf-wrap-prob=P` `-mllvm -obf-wrap-layer=L` | `100` / `2` |
| 字符串加密 | `-mllvm -enable-strcry` | 必带 |
| 平坦化 | `-mllvm -enable-cffobf` | 必带 |
| 指令替换 | `-mllvm -enable-subobf` | 带上 |
| 上游间接跳转 | `-mllvm -enable-indibran` | 带上 |
| 自查 | `-mllvm -obf-verify` | **建议永远带** |
| 导出 IR 取证 | `-mllvm -obf-dump-ir=path.ll` | 排查用 |
| 分步 trace | `-mllvm -obf-debug` | 排查用 |

**B27 之后：单给 `-obf-*` 也生效，`-obf-level=0` 只是「关掉默认档位」。**

档位与体积（经验值，基线 6~7 MB）：L1 ≈ 1.4×，L2 ≈ 2.8×，L3 ≈ 5.8~7.5×；L3 冷启 1.9~2.6×、编译 9~14×。

**保护名单（不要混淆）**：`JNI_OnLoad` / `JNI_OnUnload` / 字符串解密桩 / 待 hook 的函数。两种写法：函数名加前缀 `obf_no_` / `hikari_no_`，或函数上挂 `__attribute__((annotate("no_obf")))`。

---

## 5. 必须知道的坑（按踩到的次数排序）

### 5.1 `TMP` / `TEMP`（必踩）

Windows 上 clang 写临时文件到 `C:\Users\...\AppData\Local\Temp` 会被拒：

```
clang: error: unable to make temporary file: permission denied
```

解法：`export TMP='F:\脱壳还原\_tools\so_test\tmp' TEMP="$TMP"`（目录要先存在）。只设 `TMPDIR` 无效。

### 5.2 链接器参数

- `-Wl,--max-page-size=16384` **不被 lld 接受**，要用 `-Wl,-z,max-page-size=16384`。
- 16 KB 页是 Android 15 起的硬要求，别漏。

### 5.3 运行库缺失（换 clang 必踩）

```
ld.lld: error: cannot open .../lib/clang/20/lib/aarch64-unknown-linux-android24/libclang_rt.builtins.a
ld.lld: error: unable to find library -l:libunwind.a
```

原因：我们只编了 `clang`，没编 compiler-rt；NDK r27 的这些东西在 `lib/clang/18/lib/linux/`。

解法（一次性）：

```bash
cp -rf "$TC/lib/clang/18/lib/linux" "$TC/lib/clang/20/lib/"
```

`_ndk_install.sh` 已经自动做了这一步。

### 5.4 `-verify-each` 在 clang 里不存在

`Unknown command line argument '-verify-each'` —— 那是 `opt` 的 flag。用我们的 `-mllvm -obf-verify`（pass 结束时跑 `verifyModule`）。

### 5.5 ccache 与 CI 缓存

`actions/cache` 的 post 步骤只在 **job 成功** 时执行。之前「自检失败 = job 失败」，导致缓存从来没存过，每轮都从头编 2.4 小时。

现在的结构是 **build 与 gate 拆成两个 job**：build 只要编出来就 success（缓存因此存得下），gate 单独跑矩阵。

缓存 key 用 `ccache-linux-${{ github.run_id }}` + `restore-keys: ccache-linux-`，**不要**把源码 hash 放进 key（改一行就冷缓存）。

### 5.6 并发组

GitHub 的并发组是「workflow + ref」。main 上卡着一个 run 时，新 push 会一直排队。

绕法一：推到新分支 `b31-fix` 再 `gh workflow run build.yml --ref b31-fix`。
绕法二：`gh api --method POST "repos/{o}/{r}/actions/runs/<id>/cancel"`（返回 `{}` = 接受；若临近结束会 409 `Cannot cancel a workflow run that is not in progress.`）。

### 5.7 拿日志

run 没结束时日志接口返回 0 字节；要么等它结束，要么先 cancel。

```bash
gh api "repos/{o}/{r}/actions/jobs/<jobid>/logs" --allow-escape-sequences
```

---

## 6. 已修的 32 个 bug（按类型归档，细节见文档 §12）

**PHI / CFG 一致性（最致命的一类）**
- B4/B7/B8/B12/B13/B18/B22/B23/B26/B30 —— 插块后没修后继 PHI、同块多边给了不同值、入口块进了栈机或 blockaddress、SJI 分发器反支配、DemoteRegToStack 缺失。
- **LLVM 20 硬规则**（`Verifier.cpp:3100`）：同一前驱块的多条入边必须携带**完全相同的值**。`canonicalizeDupEdges()`（把 `br %c, %X, %X` 压成 `br %X`）+ `rebuildPhiIncoming()` 是最终解法。

**汇编 / 内联 asm**
- B14：`FunctionType` 参数个数与约束串 `"r,~{x29},~{x30},~{memory}"` 必须严格配平，否则 AsmPrinter 报 `invalid operand number`。
- B20：数据岛前加 `b 1f`、后加 `1:`，防止守卫被折叠后顺序执行进垃圾。

**静态初始化器**
- B24/B29：**LLVM 只支持加法重定位**，`xor (i64 ptrtoint(...), i64 K)` 会直接 `fatal error: Unsupported expression in static initializer`。表里存裸指针，运行期再 `ptrtoint → xor → inttoptr`。

**LLVM API 误用**
- B15：PHI operand 是 `[v0,b0,v1,b1,…]`，`getIncomingBlock(OpIdx)` 要除以 2。
- B16：`alwaysinline` + `noinline` 冲突，要先 `removeFnAttr`。
- B21：`ConstantArray::getOperand()` 是野指针，必须走 `getAggregateElement()`。
- B25：`removeIncomingValue(0)` 重载二义 → 显式 `unsigned Zero = 0;`。
- B31：`Module::getGlobalVariable(name)` **默认不返回 internal 链接的全局**，要传 `true`（否则每次新建一份 key 表，`.data` 涨到 6 MB，且解码用错 key → 返回值是垃圾）。
- B32：译码链跨块被 PHI 使用时，收尾要 `DemoteRegToStack`。

**语义 / 逻辑**
- B19/B27/B28：pass 顺序与「单给 flag 不生效」（`-obf-level=0` 把 flag 全吞了）。
- B9/B10/B11：守卫终结符位置、入口块前驱、`Intrinsic::fshr` 名字（`{I8}` 一个参数就够，别写三个）。

**闸门自身**
- `grep -q 'udf|\.word'` 是 BRE，`|` 当字面量 → 永远匹配不到，必须 `grep -qE`。
- SJI 要求函数块数 ≥8，小探针必然「无效果」→ 应判跳过而不是失败。

---

## 7. 怎么验证你手上的东西是好的

```bash
cd F:/脱壳还原/_tools/hikari-full
export TMP='F:\脱壳还原\_tools\so_test\tmp' TEMP="$TMP"
bash tests/gate.sh "$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe" "$PWD/.gate"
```

期望输出：`== 结果：通过 90 / 90，失败 0`。

单项速查：

```bash
# 1) 明文是否还在
grep -c "YOUR_SECRET_STRING" out.so        # 混淆后应为 0
# 2) 是否有间接跳转与花指令
"$TC/bin/llvm-objdump.exe" -d out.so | grep -c "br  *x"   # 应远大于 0
"$TC/bin/llvm-objdump.exe" -d out.so | grep -cE "udf|brk"
# 3) IR 层面
clang -S -emit-llvm ... -mllvm -obf-verify                 # 应打印 obf: verify ok
# 4) 常量还原数学
python ollvm_magic/_ce_verify2.py                          # 应 7/7
```

---

## 8. 没做完的三件事（接手第一优先）

1. **运行期等价性**：`tests/c/rt_probe.c` 是「同一函数体编两遍（一遍 `no_obf`）逐对比对」的探针，`_start` 里直接 `svc` 退出。本机没 qemu，CI 的 `gate (linux-x86_64)` 会装 `qemu-user-static` 真跑。**这是唯一还没绿的维度**，建议先在 CI 上跑绿，再在真机上跑一次。
2. **真机 `.so` 四项验证**：明文消失 / IR 有 indirectbr / 换档位能关 / 装载后逻辑正确。可以用 `adb push` + 一个最小 APK 或 `app_process` 起。
3. **`b31-fix` 分支收尾**：`main` 已快进到 `28ea7157d`，`b31-fix` 可以删了。

可选增强（想做再做）：

- Linux ccache `--max-size` 从 2G 提到 8G（现在 Linux 每轮仍是冷编 1.5~2.5 h）。
- J2 假跳板（`adrp + br`）还没实现，花指令只有 J1 数据岛 / J1b udf / J3 假栈帧。
- 去特征化：`obf.` / `obf_ww_` / `hikari_str_get` 这些名字可被指纹识别，改名是低成本收益。
- 表加 CRC 自校验（现在 `.data.obf` 可写，可被运行期改）。

---

## 9. 接手第一小时 checklist

- [ ] `clang --version` 确认是 `20.1.5 (...)`，不是 `18.0.3`
- [ ] `ls $TC/bin | grep orig` 确认备份在
- [ ] `bash tests/gate.sh` 跑出 90/90
- [ ] 编一个自己的 `.so`，`-mllvm -obf-verify` 不报错
- [ ] `grep` 一下明文，确认没了
- [ ] 读 `OLLVM魔改方案_Hikari19_变态档.md` 的 §4（各 pass 怎么工作）与 §8（30 条踩坑）

---

## 10. 关键路径速查

| 要找什么 | 去哪 |
|---|---|
| pass 之间谁先谁后 | `Obfuscation.cpp` 的 `runOnModule` |
| 种子怎么算 | `ObfCommon.h` 的 `funcSeed` / `splitmix64` |
| 概率与档位 | `ObfCommon.h` 的 `probOf` / `effLevel` / `levelScale` |
| 花指令长什么样 | `JunkCode.cpp` 的 `makeIslandAsm` / `insertDataIsland` / `insertUdfTrap` |
| 栈机 | `StackIndirectJump.cpp` 的 `buildStackMachine` / `emitRetSled` / `emitCrossCallRelay` |
| 常量链 | `ObfConstEnc.cpp` 的 `emitDecodeChain` / `encodeBits` / `extractBits` |
| 包装与 cookie | `ObfFuncWrap.cpp` 的 `buildWrapper` / `getCalleeTable` |
| 上游字符串加密死循环 | `StringEncryption.cpp` 的 `isValidStringTry`（TRY-GUARD）+ 原始 `do{...}while` 上方注释 |
| CI 编排 | `.github/workflows/build.yml`（四 job） |
| 闸门矩阵 | `tests/gate.sh`（含效果断言） |

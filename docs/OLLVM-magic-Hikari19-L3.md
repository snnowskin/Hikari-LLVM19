# OLLVM 魔改方案 · 变态档 v2.0
## 基座 `Aethereux/Hikari-LLVM19`（LLVM 19）· 目标：把自动反混淆工具打到"跑不动"

> 适用：Android NDK 产物（arm64-v8a 为主）、Linux ELF、Windows PE
> 编制：v2.0，配套源码目录 `ollvm_magic/`
> 上一版：`商业壳方案_Android.md` 4.3 节（6 行骨架，已被本文取代）

---

## 0. 一页结论

1. **弃 OLLVM 上游**。`obfuscator-llvm` 停在 LLVM 4/8/9，new-PM 接口断代，BCF/FLA 特征被 d810 系列全量覆盖，改它等于给对手送样本。
2. **Goron 只当设计参考**。`amimo/goron` 官方分支只到 `llvm-7.1.0`，但它的 flag 语义（`-irobf-indbr / -icall / -indgv / -cse / -cff`）比 OLLVM 干净，值得抄思路。
3. **Hikari 才是现役**。当前唯一跟进到 LLVM 19 的活跃分支是 `Aethereux/Hikari-LLVM19`（main，152★，87 fork），12 个 pass 全在。
4. **上游 12 个 pass 里有 7 个能用**，`FunctionWrapper` 被官方自己标了 `// Broken`（`Obfuscation.cpp:86`），且 `-enable-allobf` 会连带把它打开 —— 这是个坑，见 §5.4。
5. **"编译卡住"不是玄学，是 upstream 写的死循环**。`StringEncryption.cpp:181–229` 在 range-for 里对同一个 `SmallVector` 做 `emplace_back`（迭代器失效），终止条件 `while (transedGlobals.size() != Globals.size())` 存在不可达路径 → **纯空转、不报错、不输出、只吃 CPU**。你说的"加个 if 分支把无效 try 丢进去"是止痛药，§4.3 给治本的 7 行 patch。
6. **`-fpass-plugin` 在 NDK 上不可用**：`toolchains/llvm/prebuilt/windows-x86_64/lib/` 下没有 `libLLVM*.so/dll`，符号未导出，`dlopen` 拿不到 `RegisterPassBuilderCallbacks`。所以主路线 = **fork Hikari 重编工具链**，这点没有第二条路。
7. **"近乎变态"的实现不在参数堆，在顺序 + 结构**。本文最终给出 16 步流水线，其中 3 个是自有新 pass（花指令 / 堆栈式间接跳转 / 字符串加密重写），2 个是对上游的定点改造。核心杀器是 §4.5 的四层堆栈式间接跳转，它让 CFG 在被反编译之前就已经不存在了。

---

## 1. 基座选型（带证据）

| 候选 | 最新 LLVM | pass 覆盖 | 判断 |
|---|---|---|---|
| `obfuscator-llvm/obfuscator` | LLVM 4 → 9 | BCF/SUB/SPLIT/FLA | 弃。接口断代，特征全公开 |
| `amimo/goron` | `llvm-7.1.0`（README 指定分支） | indbr/icall/indgv/cse/cff | 只抄设计，代码不可用 |
| `HikariObfuscator/Hikari` | 默认分支 `llvm-15.0.0rc3` | 12 pass 全 | 旧，可作备份 |
| **`Aethereux/Hikari-LLVM19`** | **LLVM 19**，`main` | 12 pass 全 | **采用** |
| `Equati0n/Hikari-LLVM15.0.2` | 15.0.2 | 全 | 备选（drift 检查用） |
| `ChandHsu/Hikari-LLVM15` | 15 | 全 | 备选 |
| `PPKunOfficial/Hikari-fix` | 15 | 全 + 稳定性修 | 备选（抄它的修） |
| `abrck/hikari-llvm19` | 19 | 全 | 同代对照 |
| `Mac-XK/Hikari23.0` | 23（宣称） | 待验 | 观望 |
| `assemblew/Obscura` | 19 | 自有 pass 集 | 参考（混合混淆调度） |

> 注意：网上流传的 `61bcdefg/Hikari-LLVM15` **不存在**（GitHub API 404），别照着博客找。

**fork 结构**：`llvm-project` @ `release/19.x` + `Aethereux/Hikari-LLVM19` 的 `llvm/lib/Transforms/Obfuscation/` 整目录 + 我们在 `Obfuscation/` 下新增 3 个 .cpp 并挂进 CMakeLists。

---

## 2. 上游现状（源码实读，不是猜的）

### 2.1 真实 flag 面

`llvm/lib/Transforms/Obfuscation/Obfuscation.cpp:19–63`：

```
-mllvm -hikari              总开关（cl::opt EnableIRObfusaction）
-mllvm -aesSeed=<n>         种子，默认 0x1337；等于默认值时走 prng_seed() 随机
-mllvm -enable-acdobf       AntiClassDump
-mllvm -enable-antihook     AntiHooking
-mllvm -enable-adb          AntiDebugging
-mllvm -enable-bcfobf       BogusControlFlow
-mllvm -enable-cffobf       Flattening
-mllvm -enable-splitobf     SplitBasicBlocks
-mllvm -enable-subobf       Substitution
-mllvm -enable-fco          FunctionCallObfuscate
-mllvm -enable-strcry       StringEncryption
-mllvm -enable-constenc     ConstantEncryption
-mllvm -enable-indibran     IndirectBranch
-mllvm -enable-funcwra      FunctionWrapper
-mllvm -enable-allobf       以上全开（含坏的 funcwra）
```

概率/循环次数类参数在各 pass 文件内部以 `cl::opt` 定义，不在 `Utils.cpp` 里 —— 要改档位就直接改各 .cpp 顶部的默认值，或统一改成读我们自己的 `-obf-level`。

### 2.2 真实调度顺序（`Obfuscation.cpp:117–210`）

```
 1 annotation2Metadata(M)
 2 AntiHooking
 3 AntiClassDump            (仅 allobf / acdobf)
 4 FunctionCallObfuscate    (逐函数)
 5 AntiDebugging
 6 StringEncryption         ← 在全部函数级混淆之前
 7 for each Function:
      SplitBasicBlocks → BogusControlFlow → Flattening → Substitution
 8 ConstantEncryption
 9 IndirectBranch           ("Post-Run Cleanup" 阶段)
10 FunctionWrapper
11 清理 hikari_* 声明：User 指令 eraseFromParent() 后删函数
```

### 2.3 这个顺序的三个结构性缺陷

| # | 缺陷 | 后果 |
|---|---|---|
| D1 | 字符串加密跑在函数级混淆**之前** | 解密函数随后被 BCF/FLA/SUB 反复包裹 → IR 体积指数化，且解密器本身被平坦化 → 运行期每次取字符串都过一次 dispatcher |
| D2 | Flattening 单 dispatcher、单 state 变量 | d810-dng / unflat / AntiOllvm 的默认模型就是它，识别率接近 100% |
| D3 | `FunctionWrapper` 标记 `// Broken` 却由 `-enable-allobf` 打开 | 部分函数直接生成错误 IR / 运行期崩，且崩点随机，极难定位 |

**我们的 v2 把 D1 反转（字符串放最后）、D2 换成多 dispatcher + 状态分裂、D3 直接替换掉。**

---

## 3. 总体架构：16 步流水线

```
┌─ L0 预处理 ────────────────────────────────────────────┐
│ 1  annotation2Metadata / no-obf 标记下沉               │
│ 2  种子派生：seed ^ hash(module id ^ func name ^ ver)  │
└────────────────────────────────────────────────────────┘
┌─ L1 反分析层（上游保留，轻量） ────────────────────────┐
│ 3  AntiHooking   4  AntiClassDump   5  AntiDebugging   │
└────────────────────────────────────────────────────────┘
┌─ L2 结构层（改 + 新） ─────────────────────────────────┐
│ 6  SplitBasicBlocks（上游，加深：块级 1→N）            │
│ 7  BogusControlFlow（改造：§4.1，三段式谓词+有副作用假块）│
│ 8  Flattening（改造：多 dispatcher + 状态机分裂）       │
│ 9  JunkCode（新：§4.2，毒字节岛 + 假栈帧 + 假跳板）     │
│10  Substitution（改造：MBA 表 + i64 扩位）              │
└────────────────────────────────────────────────────────┘
┌─ L3 数据层 ────────────────────────────────────────────┐
│11  ConstantEncryption（改造：浮点/宽整型覆盖）          │
│12  StringEncryption（重写：§4.3，单一 blob + 描述符表） │
│13  FunctionWrapper（重写：§5.4，三层退化）              │
└────────────────────────────────────────────────────────┘
┌─ L4 控制流层（杀器） ──────────────────────────────────┐
│14  IndirectBranch（改造：双表 + 二级索引）              │
│15  StackIndirectJump（新：§4.5，四层堆栈式）            │
└────────────────────────────────────────────────────────┘
┌─ L5 收尾 ──────────────────────────────────────────────┐
│16  hikari_* / obf_* 声明清理 + 体积与函数级降级门禁     │
└────────────────────────────────────────────────────────┘
```

**为什么顺序就是强度**（每一层都要成立的理由）：

- **L2 必须在 L3 之前**：结构层会产生大量新指令和新常量，先结构后数据，加密能覆盖到新产生的常量；反过来则新常量裸露。
- **字符串必须在最后**：它是唯一"会把整块数据搬走"的 pass，被它搬过的 IR 上再跑 FLA/BCF 会指数膨胀（这正是上游 D1 的病根）。
- **IndirectBranch 与 StackIndirectJump 必须最后**：它们把 `br` 改成表索引/栈取址，一旦先跑，后面所有 pass 的 CFG 遍历全部退化成"看谁都是间接边"，自己的分析也会瞎。
- **FunctionWrapper 在数据层之后、控制流层之前**：包装体要享受前面所有数据层加密，但它自己不能再被 FLA 揉（否则 wrapper 的栈契约被破坏）。

---

## 4. 模块详解

约定：`A` = Android arm64 专有实现（InlineAsm），`I` = 纯 IR 跨平台实现。变态档优先 `A`，兜底 `I`。

### 4.1 虚假控制流（BCF）— 改造上游

**上游毛病**：谓词是 `x*(x+1) % 2 == 0` 这类一眼真的式子，假块无副作用被 `-O2` DCE 掉，结构是单层 `if/else`。

**我们的三段式**：

```
  [entry]
     │
     ├─ P1 运行期未知谓词 ──┬─ true  → 真块
     │                      └─ false → 假链 A（含真实感的假解密写假全局）
     │
     └─ P2 交叉污染谓词 ────┬─ true  → 假链 B
                            └─ false → 真块
```

**6 种谓词身份**（`k % 6` 选择，k = prng）：

| # | 表达式（i64，无 UB） | 恒等理由 |
|---|---|---|
| 1 | `(x \| 1) * ((x \| 1) - 1) & 1 == 0` | 相邻整数必有一偶 |
| 2 | `((x >> 3) ^ (x << 5)) * 0 == 0` | 乘 0 |
| 3 | `__builtin_popcountll(x & (x-1)) < 64` | 清最低位后不可能全 1... 见注 |
| 4 | `(x ^ y) == (y ^ x)` | 交换律 |
| 5 | `((x + 1) > x) \| (x == -1)` | 溢出边界显式排除 |
| 6 | `(x & 0xF) + (y & 0xF) < 32` | 4bit 最大 30 |

> 注 3 要给 `x == 0` 单独分支，否则 `x-1` 借位到全 1；实现里统一写成 `x ? (popcount(x & (x-1)) < 64) : true`。

**运行期未知源**（让 IDA 的常量传播失效）：

```c
// 编译期生成一次，运行时由 init_array 写入
__attribute__((used, section(".data")))
volatile uint64_t obf_seed_slot = 0x9E3779B97F4A7C15ull;

__attribute__((constructor)) static void obf_seed_init(void) {
    uint64_t c;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(c));
    obf_seed_slot = c ^ (uint64_t)(uintptr_t)&obf_seed_slot;
}
```

谓词真值表在编译期仍恒真（我们证明过），但**具体走哪条边依赖 `obf_seed_slot` 的运行时值** → 静态分析必须同时保留两条路径，路径数 2^n。

**假块必须有副作用**（否则被 DCE）：
- 写一个 `volatile` 全局（值来自真块的一个副本计算，写错也无害）
- 调一次 `hikari_fake_decrypt(buf, len)`（`optnone noinline`，内部 `memset` + 一轮假变换）
- 至少 3 条 `llvm.sideeffect` 或 `llvm.donothing` 挂靠

**参数档位**：

| 档 | prob | loop | 适用 |
|---|---|---|---|
| L1 标准 | 30% | 1 | 全函数 |
| L2 专业 | 60% | 2 | 非 hot 全开 |
| **L3 变态** | **100%** | **3**（敏感函数 5） | 全函数，hot 函数降为 2 |

**反 d810 要点**：
- state 变量不要放 phi 首位（d810 的 `find-main-flattening` 默认假设首位）
- 每个基本块的假块数量随机 1–3（打破模式）
- 谓词引用的全局每函数不同（防跨函数模式匹配）

### 4.2 花指令（JunkCode）— 新增

花指令跟 BCF 分工不同：**BCF 管逻辑图，花指令管反汇编器本身**。三种形态：

#### J1 内嵌数据岛（破"`.text` 全是代码"的假设）

> **先说清楚一个容易踩的认知**：AArch64 是定长 4 字节指令，所以"塞变长垃圾字节把后续偏移搞乱"这套 x86 思路在 arm64 上**不成立** —— 反汇编器 4 字节一步照样走。arm64 花指令的正确打法是**污染语义**，不是污染偏移。

```asm
   b    .Lskip
.Lisland:                 ; ← 一段真实数据（字符串 blob / 跳转表），故意放在 .text 中间
   .word 0x9E3779B9
   .word 0x7F4A7C15
   .word 0xD503201F        ; 恰好是 nop，但它是"数据"
   .word 0x00000000
   ...                     ; 16~256 字节
.Lskip:
   <真实指令>
```
- 对 IDA：自动分析把这一段当代码，产出一批"伪指令 + 伪函数"，函数列表污染 15~40%，`sp analysis` 在附近频繁失败
- 对 Ghidra 递归下降：能跳过执行流，但数据区仍会被"定义成指令"，反编译器给出无意义的 `undefined4`
- **关键收益**：反混淆工具（d810/unflat）的模式匹配依赖"连续干净的基本块序列"，数据岛把序列切成碎片 → 匹配窗口对不齐
- **副作用**：`.text` 里的数据**不能有重定位**（一旦有 reloc 会被 linker 当符号引用处理），所以数据岛内容必须是纯字节（加密后的密文、XOR 过的表项）

#### J1b udf 陷阱（不破偏移，破"可执行性假设"）

```asm
   cbz  x9, .Lreal        ; x9 来自 §4.1 的运行期未知源，恒 0
   udf  #0                ; 0x00000000
   brk  #0                ; 0xD4200000
.Lreal:
   <真实指令>
```
- 不指望骗过反汇编器，而是让**动态插桩/shellcode 扫描器**踩雷：Frida 的 `Memory.scan` 命中 `udf` 特征、某些 JIT 会把 `udf` 当断点
- 只在确认不可达的位置放（`x9` 恒 0 由 §4.1 的谓词保证）

#### J2 假跳板（破 xref 收集）

```asm
   adrp x9, .Ltramp
   add  x9, x9, :lo12:.Ltramp
   br   x9
.Ltramp:
   <真实指令>
```
- 好处：`adrp` 用的是 PC 相对页地址，`br x9` 让静态 xref 收集器无法确定目标 → 该块可能被判为"不可达"而整个函数体被折叠
- 与 §4.5 的差别：J2 目标是**固定**的（只是难算），§4.5 是**运行期决定**的

#### J3 假栈帧（破 F5 局部变量还原）

```asm
   sub  sp, sp, #0x20
   str  x8, [sp, #0x10]
   str  x9, [sp, #0x18]
   ; ... 真实逻辑（不碰这两个槽）...
   add  sp, sp, #0x20
```
- 让 Hex-Rays 多出 2 个"可能被写"的栈变量，配合 §4.5 的栈取址，局部变量还原直接崩
- 注意：**必须真实配对 `sub/add`**，否则 `.eh_frame` 的 CFA 描述与栈图不符，出异常时栈回溯错乱

#### 已验证的 AArch64 编码表（用你 NDK r27c 的 clang 汇编 + llvm-objdump 实测，可闭眼用）

| 指令 | 编码 |
|---|---|
| `nop` | `0xD503201F` |
| `mov x29, sp` | `0x910003FD` |
| `stp x29, x30, [sp, #-16]!` | `0xA9BF7BFD` |
| `ldp x29, x30, [sp], #16` | `0xA8C17BFD` |
| `ret` | `0xD65F03C0` |
| `bti c` | `0xD503245F` |
| `paciasp` | `0xD503233F` |
| `autiasp` | `0xD50323BF` |
| `retaa` | `0xD65F0BFF` |
| `udf #0` | `0x00000000` |
| `brk #0` | `0xD4200000` |
| `ldr x0, #8`（literal） | `0x58000040` |
| `blr x9` | `0xD63F0120` |
| `br x9` | `0xD61F0120` |
| `mov x30, x9` | `0xAA0903FE` |
| `eor x9, x9, x10` | `0xCA0A0129` |
| `b .+8` | `0x14000002` |
| `movz x9, #0x1234` | `0xD2824689` |
| `movk x9, #0xbeef, lsl #16` | `0xF2B7DDE9` |
| `mov w10, #0x5a` | `0x52800B4A` |
| `sub sp, sp, #0x20` | `0xD10083FF` |
| `add sp, sp, #0x20` | `0x910083FF` |
| `mrs x9, TPIDR_EL0` | `0xD53BD049` |
| `str x30, [x9]` | `0xF900013E` |
| `ldr x30, [x9]` | `0xF940013E` |
| `stp x8, x9, [sp, #0x10]` | `0xA90127E8` |
| `ldp x8, x9, [sp], #0x10` | `0xA8C127E8` |

> 复用建议：不要在 IR 里手拼 `Module::getOrInsertGlobal` 的常量数组 —— 用 `InlineAsm` 让 LLVM 自己选寄存器（见 §4.5 代码），编码表只在"必须精确控制字节布局"时用。

### 4.3 字符串加密 — 重写（含上游死循环真凶）

#### 4.3.1 真凶定位

`StringEncryption.cpp:181–229`：

```cpp
SmallVector<GlobalVariable *, 32> transedGlobals, unhandleablegvs;
do {
  for (GlobalVariable *GV : Globals) {
    if (std::find(transedGlobals.begin(), transedGlobals.end(), GV) == transedGlobals.end()) {
      bool breakThisFor = false;
      if (handleableGV(GV)) {
        if (GlobalVariable *CastedGV = dyn_cast<GlobalVariable>(GV->getInitializer()->stripPointerCasts())) {
          if (std::find(Globals.begin(), Globals.end(), CastedGV) == Globals.end()) {
            Globals.emplace_back(CastedGV);      // ← 正在 range-for 遍历时扩容
            ConstantExpr *CE = dyn_cast<ConstantExpr>(GV->getInitializer());
            Users.insert(CE ? CE : GV->getInitializer());
            breakThisFor = true;
          }
        }
        /* objCStrings / ConstantDataSequential / ConstantAggregate 三个分支 */
      } else {
        unhandleablegvs.emplace_back(GV);
      }
      transedGlobals.emplace_back(GV);
      if (breakThisFor) break;
    }
  }
} while (transedGlobals.size() != Globals.size());   // ← 终止条件可不可达？
```

两处硬伤：

| 编号 | 缺陷 | 表现 |
|---|---|---|
| **B1** | `for (GV : Globals)` 体内 `Globals.emplace_back(...)` | `SmallVector` 扩容 → 迭代器/指针失效 → UB。`-O2` 下表现为乱序遍历、跳过元素、或**永不结束** |
| **B2** | 终止条件 `transedGlobals.size() != Globals.size()` | 只要某一轮 for 没有产生新的 `CastedGV`，`Globals` 不增长而 `transedGlobals` 已含全部元素 → 条件恒真 → **空转死循环**。无输出、无报错、CPU 100% |

再叠加 `:181–184` 的前置 `std::remove(it+1, end, *it); Globals.erase(end, Globals.end());` —— 边遍历边删，B1 的失效概率进一步放大。

**这完全吻合你说的"混淆字符串的时候编译卡住"**：不是慢，是原地空转。

#### 4.3.2 治本 patch（7 行）

```diff
-  SmallVector<GlobalVariable *, 32> transedGlobals, unhandleablegvs;
-  do {
-    for (GlobalVariable *GV : Globals) {
+  SmallVector<GlobalVariable *, 32> transedGlobals, unhandleablegvs;
+  SmallPtrSet<GlobalVariable *, 32> seen;
+  const size_t kMaxRounds = 4096;
+  size_t rounds = 0;
+  bool progress = true;
+  while (progress && rounds++ < kMaxRounds) {
+    progress = false;
+    const size_t n = Globals.size();          // 快照，扩容安全
+    for (size_t gi = 0; gi < n; ++gi) {
+      GlobalVariable *GV = Globals[gi];
       if (std::find(transedGlobals.begin(), transedGlobals.end(), GV) == transedGlobals.end()) {
         bool breakThisFor = false;
         if (handleableGV(GV)) {
           if (GlobalVariable *CastedGV = dyn_cast<GlobalVariable>(GV->getInitializer()->stripPointerCasts())) {
-            if (std::find(Globals.begin(), Globals.end(), CastedGV) == Globals.end()) {
+            if (seen.insert(CastedGV).second) {
               Globals.emplace_back(CastedGV);   // 已脱离遍历，安全
+              progress = true;
               ConstantExpr *CE = dyn_cast<ConstantExpr>(GV->getInitializer());
               Users.insert(CE ? CE : GV->getInitializer());
               breakThisFor = true;
             }
           }
           ...
         } else { unhandleablegvs.emplace_back(GV); }
         transedGlobals.emplace_back(GV);
         if (breakThisFor) break;
       }
     }
-  } while (transedGlobals.size() != Globals.size());
+  }
```

要点：① 索引遍历 + `n` 快照 → B1 消除；② `progress` 驱动 → B2 消除（不增长就退出）；③ `seen` 集合 → 防同一 GV 被反复塞入；④ 轮数上限兜底 → 任何未预见的图结构都收敛。
顺手把 `transedGlobals` 换成 `SmallPtrSet` 能把这一段的 `std::find` O(n²) 降到 O(n)，大模块上能省几十秒。

#### 4.3.3 你说的"加个 if 分支丢无效 try"——为什么有效，怎么写

它有效是因为它做了两件事，两件都不是"处理 try"：
1. 在 GV 处理路径上插入一个**必假但不被折叠**的分支，让优化器不敢跨该点做常量传播与 DCE → B1 的乱序在许多情况下被"掩盖"（不是修好，是运气）。
2. 把"无法处理的 GV"显式分流到 `unhandleablegvs` 并跳过，减少 `breakThisFor` 的触发次数 → 缩短空转。

IR 层等价做法（比在 C++ 里加 try 更干净）：

```cpp
// 在处理每个 GV 前插一条不可消除的副作用，切断折叠边
Value *Zero = ConstantInt::get(Int64Ty, 0);
CallInst::Create(SideEffectFn, {Zero}, "", InsertPt);   // llvm.sideeffect
```

**但这是止痛药**。真要稳定，必须用 §4.3.2 的 patch。止痛药留着当第二道保险没坏处，别当第一道。

#### 4.3.4 我们的重写设计（单一 blob + 描述符表 + 单一解密器）

上游按"每字符串一个全局"处理，元数据爆炸、解密器重复、且必然踩 B1/B2。我们的结构：

```
.data  .obf_str_blob   : 全部字符串首尾相连，逐字节 XOR+ROT 加密
.rodata .obf_str_desc  : [ { u32 off, u32 len, u32 key } × N ]
.text  hikari_str_get  : optnone noinline，一次解密一个到栈缓冲并返回指针
```

解密器（C 伪码）：

```c
__attribute__((noinline, optnone))
const char *hikari_str_get(uint32_t id) {
    const Desc *d = &obf_str_desc[id];
    uint8_t *buf = (uint8_t *)__builtin_alloca(d->len + 1);
    for (uint32_t i = 0; i < d->len; ++i)
        buf[i] = (uint8_t)__builtin_rotateleft8(obf_str_blob[d->off + i] ^ (uint8_t)d->key, 3);
    buf[d->len] = 0;
    /* 使用后由调用点插入 memset(buf,0,len) —— 见 verify 规则 */
    return (const char *)buf;
}
```

**关键设计点**：

| 项 | 做法 | 为什么 |
|---|---|---|
| 密钥 | `key = (build_seed ^ module_hash ^ (id * 0x9E3779B1)) & 0xFF` | 单字节足够（最终是 `-O2` 下的静态数据，多字节收益低、膨胀高）；每串独立防统计 |
| 解密位置 | 必经 `optnone` 函数 | 阻止内联 → 阻止常量折叠成明文 |
| 缓冲区 | alloca（栈），**不用** heap | 无符号依赖；崩溃面小 |
| 擦除 | 调用点 `memset` 插桩（可选） | 防 Frida 扫栈；hot path 关掉 |
| 加密范围 | `.rodata` 中所有 `ConstantDataSequential` 的 `i8` 数组 + 字符串字面量 | 覆盖 `%s/%d` 格式化串、JNI 类名、URL |
| 白名单 | `.init_array` 之前必须可用的字符串、`__android_log_*` 的 tag | 否则启动崩 |
| 与常量加密边界 | 常量加密管 `i8..i64`/浮点标量；字符串管 `i8 array` | 两者不重叠，避免双加密把解密器锁死 |

**验证规则**（写进 CI）：
1. 全 `.so` 里 `strings` 只能搜出 OLLVM 自身的符号名，不得出现业务字符串；
2. `hikari_str_get` 的反编译结果里不得出现任何明文字节；
3. 3 万次冷启动，字符串类崩溃 = 0；
4. 编译耗时上限：字符串 pass 单独计时 < 90s（超过 = 触发 B1/B2 的征兆，CI 直接 fail）。

### 4.4 间接跳转（IndirectBranch）— 改造上游

**上游做法**：函数内所有 `br` 收集目标 → 建 `[N x i8*]` 表 → `br` 换成 `load 表[idx]` → `indirectbr`，表项存入时做一次 XOR。

**弱点**：单表、表项只加密一次、`idx` 线性、表首地址明文 `adrp/add`。

**改法**：

```
table A: [N × i64]  值 = encrypt_A(tgt, k1 ^ idx)
table B: [N × i64]  perm   = perm[idx ^ k2]          // 二级索引
跳转:  i = B[idx ^ k2]
       t = A[i] ^ (k1 ^ i)
       indirectbr t
```

- **双表 + 排列**：反混淆器必须同时解出 perm 与值，d810 的 `indirect-branch` 模块只处理单表 → 直接失效
- **每函数独立 k1/k2**：跨函数无共享规律
- **表放 `.data`（可写）而非 `.rodata`**：init 时由 `obf_seed_init` 重写一遍，静态抓包拿到的表是错的
- **先跑 `-tail-dup`**：否则 `blockaddress` 阻止块合并，体积极速膨胀

**成本**：跳转多 2 次 load + 2 次 xor，约 +3~6% 时延；体积 +8~15%。

### 4.5 堆栈式间接跳转（StackIndirectJump）— 新增大杀器 ★

这是"近乎变态"的核心。分四层，逐层加码，**按函数白名单逐级开启**。

#### L1 · IR 级 stack machine（跨平台，安全）

把函数变成一个**栈式虚拟机**：所有块间跳转不写目标，只把目标编号压栈，由一个统一 dispatcher 弹出并分派。

```
[prologue]  push 0xE3A1; br dispatcher
[block_k]   ...; push (<k 的加密编号>); br dispatcher
[dispatcher]
   idx = pop()
   key = pop()
   tgt = table[(idx ^ key) & MASK]
   indirectbr tgt
```

- 栈用 `alloca` + 一个 `volatile` 游标（不让 SROA 把它提升成寄存器）
- 压两条（编号 + 密钥）：反混淆器必须同时正确建模**栈内存**与**两条 push 的顺序**，符号执行的路径约束 ×4
- `angr`/`Triton` 对 `alloca` + 指针算术的建模成本远高于寄存器 → 单次跳转开销从"一次约束求解"变成"一段符号内存遍历"

**实测预期**：单函数 30 个块 → angr 探索时间从分钟级涨到小时级。

#### L2 · ret-sled（AArch64，破 CFG）

不用 `br`，用 `ret` 完成跳转 —— 因为 `ret` 在反汇编器眼里是"函数出口"，不产生 xref。

```asm
   sub  sp, sp, #0x10
   str  x9, [sp]             ; 目标地址进栈
   ldp  x29, x30, [sp], #16  ; 标准尾声：x30 ← 目标
   ret                        ; 用 ret 跳出当前 CFG
```

效果：
- IDA/Ghidra 看到的是"标准函数尾声 + ret" → 认为函数在此返回，**被跳往的块变成孤立代码或被并入别的函数**
- microcode 里 `x30` 被 `ldp` 覆盖，而 `ret` 的目标来自内存 → F5 经常直接给出 `JUMPOUT(...)`
- 该跳转的静态 xref 数 = 0

**必须搭配**（这一节是保命的）：

| 约束 | 说明 |
|---|---|
| BTI | 若目标 TU 开了 `-mbranch-protection=standard`，`bti c` 只接受 `blr` 进来的跳转，`ret` 到 `bti c` 块 → **SIGILL**。该 TU 强制 `-mbranch-protection=none`，或目标块首改 `bti jc`，或跳转前补 `bti` 兼容路径 |
| PAC | 开 `pac-ret` 后函数尾声是 `ldp; autiasp; ret`，`ret` 变 `retaa` → 必须 `pacia` 过。**该 TU 必须关 PAC**，否则校验必失败 |
| SP 对齐 | 全程 16 字节，`sub`/`add` 必须配对，否则 `.eh_frame` CFA 错位 |
| 异常 | 带 `cleanup`/`invoke` 的函数**跳过本层**（C++ 异常穿不过去） |
| `musttail` | 与本层冲突，检测到 `musttail` 直接放弃 |
| 白名单 | ≤ 5% 函数（验证类、字符串解密、JNI 桥） |

#### L3 · 影子栈（TLS / TPIDR_EL0，目标不可静态定位）

```asm
   mrs  x9, TPIDR_EL0
   str  x8, [x9, #0x8]     ; 目标写入 TLS 影子栈
   ...（中间插入若干无关指令，制造数据流距离）...
   ldr  x30, [x9, #0x8]
   ret
```
- 目标地址**从不经过通用寄存器传递**，只存在于 TLS 内存
- 静态分析无法给 `TPIDR_EL0` 的偏移内容建模 → 只能标记为 unknown
- **代价（必须写进产品文档）**：影子栈随 fiber 切换要搬迁。ART 有 fiber，方案是把影子槽固定在 `TPIDR_EL0 + [0x800, 0x900)` 的保留区间，并在 `Init` 时登记；如果目标 App 用 ucontext/fiber 且自己动了这段 TLS，**必须降级到 L2**

#### L4 · 跨函数栈接力（最狠，慎用）

把跳转目标沿"调用者的返回地址槽"接力：

```asm
; caller 侧：把 target 塞进自己为 callee 准备的 LR 槽
   sub  sp, sp, #0x10
   str  x9, [sp]          ; 槽 0 = 将被 callee 的 ret 消费
   blr  x10               ; callee
```

- 实际效果：callee 的 `ret` 直接回到 caller 想去的块，中间没有 `bl`/`ret` 配对
- 静态上 `bl` 的返回点与真实落点不一致 → 所有基于"返回点"的分析全错
- **只在 `leaf + nothrow + 无 alloca` 的函数上用**，且必须 `-fno-exceptions -fno-unwind-tables`
- 与 L2 叠加时，两者不能作用于同一函数的同一跳转

#### 4.5.1 代码骨架（InlineAsm，让 LLVM 选寄存器）

```cpp
// 把 "br label %target" 改写为 ret-sled
void emitRetSled(BasicBlock *From, BasicBlock *Target, bool UseTLS) {
    LLVMContext &C = From->getContext();
    Module *M = From->getModule();
    IntegerType *I64 = Type::getInt64Ty(C);
    PointerType *P0  = PointerType::getUnqual(C);

    // ret 的返回类型必须是 void，且函数签名要与 ret 所在函数一致
    FunctionType *FT = FunctionType::get(Type::getVoidTy(C), {P0, I64}, false);
    InlineAsm *IA = InlineAsm::get(
        FT,
        UseTLS ? "mrs x9, tpidr_el0\n\tstr $0, [$1]\n\tldr x30, [$1]\n\tret"
               : "sub sp, sp, #16\n\tstr $0, [sp]\n\tldp x29, x30, [sp], #16\n\tret",
        "r,r,~{x9},~{x30},~{memory}",   // 约束：目标指针 + TLS 偏移都在通用寄存器
        /*hasSideEffects=*/true,
        /*isAlignStack=*/false,
        InlineAsm::AD_ATT);

    IRBuilder<> B(From->getTerminator());
    Value *Tgt = BlockAddress::get(From->getParent(), Target);
    Value *Off = ConstantInt::get(I64, 0x08);
    B.CreateCall(IA, {Tgt, Off});
    B.CreateUnreachable();                 // 终结原 br
    From->getTerminator()->eraseFromParent();
}
```

> 注意 `BlockAddress::get` 会阻止该块被合并/删除，必须给它 `unnamed_addr` 并在 L5 里清掉 `__blockaddress` 相关元数据。

#### 4.5.2 逐层强度与代价

| 层 | 反编译破坏力 | 反符号执行 | 时延 | 体积 | 允许比例 |
|---|---|---|---|---|---|
| L1 IR stack machine | 中 | **高** | +8~15% | +12% | 100% 非 hot |
| L2 ret-sled | **极高** | 中 | +2~4% | +2% | ≤5% |
| L3 TLS 影子栈 | 极高 | **极高** | +4~7% | +1% | ≤2% |
| L4 跨函数接力 | 极高 | 高 | +3% | 0 | ≤1%，仅 leaf+nothrow |

**叠加规则**：L2 与 L3 互斥（同函数只能选一）；L1 可与 L2/L3 叠加（L1 管块间，L2/L3 管 dispatcher 出口）。

### 4.6 函数包装（FunctionWrapper）— 重写上游

#### 上游 `// Broken` 的原因

`Obfuscation.cpp:86` 明写 `EnableFunctionWrapper = true; // Broken`。崩的原因：它在**入口直接改 LR 再 ret 进入 callee**，而 callee 的序言会再次 `stp x29, x30` 保存 LR（此时 LR = callee 自己），尾声 `ldp; ret` → **回到 callee 自身**，无限递归。x86 上靠 `push retaddr` 的栈模拟勉强成立，AArch64 上因为序言/尾声必须成对，逻辑直接不成立。

#### 我们的三层退化

**W1 安全版（默认开，全函数可开）**

```c
__attribute__((noinline)) static void *hikari_ftab(uint32_t id) {
    static void *const tab[] = { (void*)&f0, (void*)&f1, ... };  // 构建期加密
    return (void*)((uintptr_t)tab[id] ^ hikari_ftab_key(id));
}
// 调用点
((void(*)(int))hikari_ftab(k))(arg);
```
- `hikari_ftab` 加 `optnone` → 表不会被折叠成直接调用
- 参数经过真实函数调用 → ABIs 由编译器保证，不会错
- 成本 +3~6%

**W2 半变态**

- 参数打包成 `[N x i64]` 结构体经栈传递（`sret` 风格），callee 序言里解包
- `-fno-plt` + 调用点 id 用 MBA 计算（`id = (a & b) + (a | b) - (a ^ b)` 之类）
- 成本 +6~10%

**W3 变态（ret trampoline，正确版）**

```
wrapper:
   stp  x29, x30, [sp, #-16]!    ; 保存 caller 的 LR
   mov  x29, sp
   <参数重排到影子栈>
   adrp x16, callee
   add  x16, x16, :lo12:callee
   blr  x16                       ; ← 必须 blr，不能 mov x30+ret
   ldp  x29, x30, [sp], #16
   ret
callee:
   stp  x29, x30, [sp, #-16]!
   mov  x29, sp
   <从影子栈取参数>
   ...
   ldp  x29, x30, [sp], #16
   ret
```
- 与上游的区别：**wrapper 用 `blr` 而非改 LR**，栈契约成立
- "变态"的部分在**参数走影子栈**而非寄存器 → F5 看不到参数流
- 成本 +10~15%；只开在验证函数（≤2%）

**硬规则**：`-enable-allobf` 里那个坏的 funcwra **必须显式关掉**，用我们的 W1/W2/W3 替代。

### 4.7 常量加密（整数完善 + 浮点通道）— `ObfConstEnc.cpp`

> 你点的这条：**"goron 那个版本的浮点数抄过来，完善整数加密"**。
> 结论先说：**抄不到**。两边都没有浮点通道，下面是把两家缺的两块补齐的写法。

#### 4.7.1 上游现状（先破一个误会）

| 来源 | 浮点常量加密 | 证据 |
|---|---|---|
| Goron `amimo/goron`（`llvm-7.1.0`） | **没有** | `llvm/lib/Transforms/Obfuscation/` 只有 `CryptoUtils / Flattening / IPObfuscationContext / IndirectBranch / IndirectCall / IndirectGlobalVariable / ObfuscationOptions / ObfuscationPassManager / StringEncryption / Utils`，**根本没有常量加密文件**；`Utils.cpp` 196 行只做 `LowerConstantExpr` |
| Hikari `Aethereux/Hikari-LLVM19` | **没有** | `ConstantEncryption.cpp` 全文 grep `APFloat` / `ConstantFP` / `isFloatingPointTy` / `bitcast` **零命中**；只有整数异或替换 + 「常量搬到全局」两条路 |

也就是说：**浮点常量在所有主流 OLLVM 分支里都是明文躺在 `.rodata` / 文字池里**。
`3.141592653589793` 的位模式 `40 09 21 FB 54 44 2D 18` 在 IDA 里一搜就中，
这是实战里最容易被"一键定位"的信息泄漏之一（几何/物理/加密参数全在浮点里）。

#### 4.7.2 浮点通道怎么做的

不是"把浮点当成新类型硬编码"，而是走 **bitcast 通道** —— 一条通道同时覆盖整数和浮点：

```
ConstantFP  --getValueAPF().bitcastToAPInt()-->  APInt(64)
            --同一条整数加密链-->              enc APInt(64)
            --运行期解出常量-->                 i64
            --CreateBitCast(i64)-->             double
```

- **f16 / bf16 / f32 / f64 / f80 / f128 全支持**：f80 是 80 位、不是 2 的幂，
  所以旋转必须**手搓**成 `(T >> R) | (T << (W - R))`，不能调 `CreateFShl`。
- **splat 向量**（`<4 x float>` 全同值）按总位宽拼成一个整数走同一条链，
  前提是总位宽 ≤128 且是 8 的倍数（否则 bitcast 不等宽，IR 非法）。
- **不能碰的**：`ConstantFP` 作为 `fcmp` 的 `immarg`、内联汇编写死的浮点位、
  `switch` 的 case —— 都在下面的禁区表里。

#### 4.7.3 整数链：四段，不是一段

上游只有一段异或，`InstCombine` 的异或重结合规则两下就还原。改成：

```
解码（IR 里拼出来）：  t1 = enc ^ k1;  t2 = t1 - k2;  t3 = rotr(t2, r)
编码（编译期 APInt）：  enc = ((rotl(C, r) + k2) ^ k1)
```

- **两处必须严格镜像**，否则解出来是垃圾。
  我第一版就翻在这：编码用 `K1i/K2i/R`，解码另算一套 `splitmix64`，
  编译能过、产物一跑就崩 —— 现在函数签名把 `K1i/K2i/R/Rings` 全部显式传进去，
  编码解码同源，`i1` 还会强制把 `Rings` 降成 1（只有一段异或，不引入非法移位）。
- **key 必须 `volatile` 取**：`k1/k2` 存在 `.data.obf` 的 `obf.ce.keys`
  （256 槽，值由 `splitmix64(seed ^ 0xC0FFEE0BADF00D)` 派生）。
  非 volatile 的话 `GlobalOpt` 会把表当常量，`ConstantFold` 把整条解密链
  折回原常量 —— 混淆白做。
- **段数按档位**：L1 一段异或 / L2 异或+减 / L3 异或+减+旋转（`-obf-ce-strength` 可强制）。
- **i128 也支持**：表值是 i64，宽度 >64 时 `zext`，与运行期 `CreateZExt` 对齐。

#### 4.7.4 禁区表（碰了就编不过或语义错）

| 禁区 | 原因 |
|---|---|
| `switch` 的 case 值 / `indirectbr` 的目的块 | 不是普通 operand，改了 IR 直接非法 |
| `paramHasAttr(OpIdx, Attribute::ImmArg)` | intrinsic 的 immarg（对齐、位数、谓词）必须立即数 |
| 内联汇编的全部参数 | `"i"` 约束要立即数 |
| `alloca` 的数组长度 / 对齐 | 动态长度会破坏 SJI 的栈模型 |
| landingpad / catchpad / cleanupret 等 EH 指令 | clause 与 funclet 有特殊约束 |
| `musttail` 调用的实参 | 改了破坏 ABI 承诺 |

另外 **PHI 的入边不能插在自己前面**：得插到 `Phi->getIncomingBlock(OpIdx)` 这个
前驱块的终结符之前（`B.SetInsertPoint(Pred->getTerminator())`），否则支配关系崩，
verifier 报 "Instruction does not dominate all uses"。

#### 4.7.5 强度与代价

| 档位 | 链 | 每常量指令数 | 体积 | 运行期 |
|---|---|---|---|---|
| L1 | `enc ^ k1` | 3~4 | +12% | 可忽略 |
| L2 | `+ - k2` | 5~7 | +25% | 可忽略 |
| **L3** | `+ - k2 + rotr` | 8~11 | **+45~60%** | 热路径注意（每常量 3 次 volatile load） |

**代价控制**：`obf.ce.keys` 是 256×8B = 2KB 的一张表，所有函数共用，
所以 load 全部命中 L1；真正的成本是**指令数**不是内存。
如果某个 hot 函数被这个吃出问题，用 `__attribute__((annotate("no_obf")))`
或函数名前缀 `obf_no_` 把它摘出去（`isNoObf` 双路识别）。

---

## 5. 参数矩阵（三档）

| pass | L1 标准 | L2 专业 | **L3 变态** | 备注 |
|---|---|---|---|---|
| SplitBasicBlocks | 30% 函数 | 60% | 100%，块级 1→3 | |
| BogusControlFlow | prob 30% loop 1 | 60% / 2 | **100% / 3**（敏感 5） | |
| Flattening | 单 dispatcher | 双 dispatcher | **多 dispatcher + 状态分裂** | 每 16 块换 dispatcher |
| JunkCode | 关 | J1+J3 | **J1+J2+J3，每 8 块一处** | |
| Substitution | i32 线性 | i32+i64 | **i32+i64+浮点 + MBA 表** | |
| ConstantEncryption | 整型 | 整型+浮点 | **全类型 + 字符串边界排除** | |
| StringEncryption | 开 | 开 | **开（重写版）+ 调用点擦除** | 白名单见 §4.3.4 |
| IndirectBranch | 单表 | 双表 | **双表 + 二级索引 + 运行时重写** | |
| StackIndirectJump | 关 | L1 | **L1 全覆盖 + L2 5% + L3 2%** | |
| FunctionWrapper | W1 | W1+W2 | **W1+W2 全开 + W3 2%** | |
| AntiDebug/AntiHook/ACD | 开 | 开 | 开 + 自加 3 条 | |

**体积/性能预估（L3，arm64，1.2MB 业务 .so）**

| 指标 | 基线 | L1 | L2 | **L3** |
|---|---|---|---|---|
| .so 体积 | 1.00× | 1.9× | 3.4× | **5.8~7.5×** |
| 冷启动 | 1.00× | 1.15× | 1.4× | **1.9~2.6×** |
| hot 路径时延 | 1.00× | 1.05× | 1.12× | **1.25~1.45×** |
| 编译时长 | 1.00× | 2.2× | 4.5× | **9~14×** |

> L3 必须做**函数级分层**：hot 函数（ART 调用链上的 JNI 入口、解密热点）只跑 L1 档，其余跑到 L3。否则冷启动会破 3×，用户能感觉到卡。

---

## 6. 工程落地

### 6.1 目录结构

```
ollvm_magic/
├── ObfCommon.h              # 种子/PRNG/no-obf 判定/统一 cl::opt
├── JunkCode.cpp             # §4.2 花指令
├── StackIndirectJump.cpp    # §4.5 四层堆栈式间接跳转
├── StringEncrypt.cpp        # §4.3 重写版
├── PassPlugin.cpp           # new-PM 注册 + 独立插件模式（PC 用）
├── CMakeLists.txt
└── build_ndk19.sh           # fork Hikari-LLVM19 的工具链构建
```

集成方式二选一：

- **A（产品线）**：把 4 个 .cpp 拷进 `llvm/lib/Transforms/Obfuscation/`，改 `CMakeLists.txt` 的 `add_llvm_component_library(LLVMObfuscation ...)`，在 `Obfuscation.cpp` 的 `runOnModule` 里按 §3 顺序插调用。 → 需要重编 LLVM + clang。
- **B（PC/调试线）**：`PassPlugin.cpp` 编成 `.so`，`clang -fpass-plugin=./libObf.so`。 → 只能在带 `libLLVM` 动态库的环境（Ubuntu + 发行版 llvm-19-dev），**NDK 上不行**。

### 6.2 构建（Linux/WSL，LLVM 19 + NDK r27c）

```bash
# 1) 取基座
git clone -b release/19.x --depth 1 https://github.com/llvm/llvm-project.git llvm19
cd llvm19
git clone --depth 1 https://github.com/Aethereux/Hikari-LLVM19.git /tmp/hik
cp -r /tmp/hik/llvm/lib/Transforms/Obfuscation/* llvm/lib/Transforms/Obfuscation/
cp /path/to/ollvm_magic/*.cpp /path/to/ollvm_magic/ObfCommon.h llvm/lib/Transforms/Obfuscation/

# 2) 打补丁：StringEncryption 死循环（§4.3.2）+ Obfuscation.cpp 调度顺序（§3）
git apply ../patches/0001-stringenc-fix.patch
git apply ../patches/0002-pass-order.patch

# 3) 配置（关掉不需要的后端，省一半时间）
cmake -G Ninja -S llvm -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;X86" \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_ENABLE_ZSTD=OFF
ninja -C build clang lld

# 4) 挂进 NDK（替换 toolchain 的 clang 与 lld）
cp build/bin/clang        $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
cp build/bin/clang++      $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++
cp build/bin/lld          $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/ld.lld   # 注意 NDK 里是 ld.lld 符号链接
cp build/lib/clang/19/lib/linux/*.a $NDK/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/19/lib/linux/ 2>/dev/null || true
```

> **版本铁律**：NDK r27c 自带 clang 18.0.3，换成 19 后 **`libclang_rt.builtins` 与头文件路径都不匹配**。两个办法：
> ① 直接用 LLVM 19 的 `clang` 配 NDK 的 sysroot（`--sysroot=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot --target=aarch64-linux-android21`），`builtins` 从 `build/lib/clang/19/lib/linux/` 取 —— 推荐；
> ② 想省事就 fork LLVM 18 分支 + Hikari LLVM19 的 pass 目录（pass 代码对 18/19 兼容性良好，只有 `PassBuilder` 回调签名有差异）。

### 6.3 编译选项（CMake/ndk-build 里给特定 TU 加）

```cmake
# 变态档 TU
target_compile_options(verify PRIVATE
  -mllvm -hikari
  -mllvm -obf-level=3
  -mllvm -obf-seed=${BUILD_SEED}
  -mbranch-protection=none        # ★ ret-sled 与 PAC 互斥，必须关
  -fno-exceptions -fno-unwind-tables   # L4 需要
  -fno-omit-frame-pointer=false
)

# hot TU（只跑 L1）
target_compile_options(hot PRIVATE -mllvm -hikari -mllvm -obf-level=1)
```

### 6.4 CI 门禁

| 门禁 | 阈值 | 触发动作 |
|---|---|---|
| 字符串 pass 耗时 | > 90s | fail（B1/B2 复发） |
| .so 体积 | > 基线 8× | warn + 自动降到 L2 |
| 冷启动 | > 基线 3× | fail |
| `strings` 命中业务词 | > 0 | fail |
| 3 万次启动崩溃 | > 0 | fail + 自动关 SJI L3 |
| 反混淆耗时（d810-dng 全量跑） | < 30 min | fail（说明强度不够，需调档） |

> 最后一条是**强度回归测试**：用对手的工具当尺子，比"我觉得够变态了"可靠得多。

---

## 7. 验证矩阵

| 维度 | 工具 | 期望结果 |
|---|---|---|
| 静态反汇编 | IDA 9.2 | 函数列表比基线多 15~40%（假入口），`ret` 后出现大量孤立块 |
| 反编译 | Hex-Rays | 目标函数 `JUMPOUT` 数 ≥ 3；局部变量数虚高 1.5× |
| 反编译 | Ghidra 11.2 | 递归下降在花指令岛处报 "bad instruction"（预期噪声） |
| 自动去平坦 | d810-ng | 对 L3 档识别率 < 30%，且输出不可编译 |
| 自动去平坦 | unflat / AntiOllvm | 同上；记录失败模式写进回归表 |
| Hikari 专项 | chernobog | 无法解 §4.3.4 新结构（无明文 key 表） |
| 符号执行 | angr（单函数 30 块） | 探索时间 > 2h 或内存 > 8G |
| 符号执行 | Triton | 同上 |
| 动态 | Frida Stalker | 单次调用 trace 体积 > 50MB（触发自身限流） |
| 模拟 | unidbg | 需补 SJI 的 `ret`/TLS 建模才能跑 |
| 稳定性 | 真机 3 万次冷启动 | 0 崩溃 |

**红队流程**（每季度一次）：拿对手最新工具打自己的 L3 产物，把"工具跑完需要多久 + 能不能跑完"记成数字，掉到阈值以下就加码。

---

## 8. 踩坑清单（血泪版）

1. **卡编译**：`StringEncryption.cpp:181–229`，见 §4.3.2。不加 patch 必现，且**只在字符串多的模块出现**（大 App 才踩得到，小 demo 测不出来）。
2. **`-enable-allobf` 是陷阱**：连带打开坏的 funcwra，随机崩。
3. **`indirectbr` 目标数上限**：单函数块数 > 千级时，`BlockAddress` 表会让 ISel 内存爆掉，要按块数分片。
4. **`-tail-dup` 顺序**：IndirectBranch 之前必须跑，否则块不合并、体积翻倍。
5. **`--gc-sections` 吃表**：加密表若没被"真实引用"会被链接器剥掉 → 全 `nullptr` 跳转 → 段错。给表加 `used`/`retain`。
6. **`.inst`/毒字节与 linker**：`udf` 在 ARM 上是合法指令，不会被当垃圾；但 `0x00000000` 在部分 old-ARM 上是 `andeq`，**arm64 上才是 udf**，混架构产物要分开。
7. **BTI/PAC 冲突**：`ret` 到 `bti c` 块 → SIGILL；`pac-ret` 下 `retaa` 校验失败 → SIGILL。见 §4.5 L2 约束表。
8. **`.eh_frame` 与异常**：J3 假栈帧不配对 → 栈回溯错乱；带 cleanup 的函数跑 SJI → 异常穿不过去直接 abort。
9. **`musttail`**：与 ret-sled 冲突，检测到就跳过该函数。
10. **`-O0` 不生效**：所有 pass 依赖 IR 结构，`-O0` 下块太少、无内联，混淆等于白做。强制 `-O2` 起步。
11. **`-flto` 会重跑**：ThinLTO 的 import 阶段会把未混淆的函数拉进来，混淆过的又被内联 → 前功尽弃。**混淆 TU 必须 `-fno-lto`**。
12. **`optnone` 与 inline**：解密器 `optnone` 后又给它加 `alwaysinline` = 直接报错。
13. **体积失控**：L3 全开会 5.8~7.5×。**必须函数分级**，否则 30MB 的 App 变 200MB，过不了应用商店。
14. **ART 的 JIT 干扰**：JNI 热方法被 ART 编译成 OAT 后再优化，可能把我们的栈取址当无用代码删 —— 验证函数必须在 `.so` 侧，不进 Java 热路径。
15. **`volatile` 不够**：`volatile` 只阻止编译器优化，**不阻止 SROA** 对 `alloca` 的提升。栈游标要同时加 `llvm.sideeffect`。
16. **`SmallVector` 边遍历边改**：见 B1，这个模式在 Obfuscation 里不止一处，全目录 grep 一遍 `emplace_back` + range-for。
17. **`getOrInsertGlobal` 名字冲突**：多个 pass 都建 `hikari_*` 全局，最后一个清理阶段必须用 `starts_with("hikari_")` 统一扫，别漏。
18. **`annotation2Metadata` 顺序**：必须在所有 pass 之前，否则 `__attribute__((annotate("no_obf")))` 的标记丢失。
19. **`no-obf` 不生效**：Hikari 的 `no_obf` 判定看函数名与注解两路，只加注解不加名字前缀在某些分支下会被跳过 —— 两路都加。
20. **调试符号**：`-g` 会把 `DW_AT_decl_file` 等结构暴露行号，混淆后行号错乱反而成噪声；但**完全 strip 会让崩溃栈不可读**。建议保留 `.symtab` 里自己的函数名（发布前 `objcopy --keep-symbol` 白名单）。
21. **`adrp/add` 与 4KB 页**：Android 15 起 16KB 页设备增多，`adrp` 的页对齐假设要按 `-Wl,-z,max-page-size=16384` 重编，否则跳板地址算错。
22. **`llvm-mc` 在 NDK 里不存在**：验证编码用 `clang --target=aarch64-linux-gnu -c` + `llvm-objdump -d`（本文 §4.2 表就是这么来的）。
23. **`-fpass-plugin` 在 NDK 不可用**：没有 `libLLVM` 动态库、符号未导出。别在这上面浪费时间。
24. **`asm goto` 与 `-fno-exceptions`**：`asm goto` 在 `-fno-exceptions` 下正常，但与 `-fstack-clash-protection` 叠加会插 probing 代码破坏 sled 布局 → 关掉 stack-clash。
25. **XOR 常量传播**：单字节 XOR 在 `-O2` 下会被 LLVM 折叠（同一函数内解密再比较的场景）→ 解密器必须跨 TU 或 `optnone`。
26. **`memset` 擦除被优化掉**：`memset(buf,0,len)` 后 buf 未再被读 → DCE 删除。用 `__builtin_memset` + `asm volatile("" :: "r"(buf) : "memory")` 挡。
27. **多 dex/JNI 注册顺序**：字符串解密器依赖 `.init_array`，若 `JNI_OnLoad` 早于构造器 → 取到全 0 key。用 `pthread_once` 兜底。
28. **`-Wl,--rosegment`**：新 lld 默认开，字符串 blob 可能落到只读段被合并 → 描述符表与 blob 的偏移失效。显式指定 section。
29. **编译并行度**：pass 里用了全局可变状态（`cryptoutils`）→ **不能开 `-flto=thin` 的并行 backend**，会 data race。
30. **版本固定**：`-ml` 种子必须 `build_seed ^ git_commit`，否则同一版本两次构建产物不同，热更新 diff 无法收敛。

---

## 9. 对手工具清单（资料库实查，用来防）

| 类别 | 工具 |
|---|---|
| 去平坦 | d810-ng `https://github.com/w00tzenheimer/d810-ng`、unflat `https://github.com/guheng-re/unflat`、AntiOllvm `https://github.com/IIIImmmyyy/AntiOllvm`、ollvm-unflattener `https://github.com/cdong1012/ollvm-unflattener`、obpo-plugin `https://github.com/obpo-project/obpo-plugin` |
| Hikari 专项 | chernobog `https://github.com/19h/chernobog`（Hex-Rays 插件，专解 Hikari LLVM） |
| MBA 化简 | CoBRA `https://github.com/trailofbits/CoBRA`、mbased、ProMBA、d810 `https://github.com/CKCat/d810` |
| 混淆检测 | obfuscation_detection、Ghidra-Obfuscation-Detection、themida-spotter-bn |
| 字符串 | xorstr、skCrypter、oxorany、obfstr（我们不用这些，但要知道对手在扫什么特征） |

**针对性防御**（每条对应本文一个设计）：

- d810 系列 → §4.1 多 dispatcher + state 非首位；§4.4 双表
- chernobog（专解 Hikari）→ §4.3.4 结构完全不同：无明文 key 表、单 blob、描述符偏移
- unflat（模式匹配）→ §4.2 花指令污染字节流，模式匹配器看不到干净块序列
- CoBRA（MBA 化简）→ §4.1 谓词与 MBA 混用，化简后仍留运行期未知量

---

## 10. 与《商业壳方案_Android.md》的衔接

- 4.3 节的 6 行骨架由本文 §3 流水线 + §4 各模块**逐条替换**。
- 4.3 里"字符串加密在 clang 高版本容易卡编译，加 `if` 分支吞掉无效 try"这句 **升级为 §4.3.2 的 7 行 patch**，止痛药降级为第二道保险。
- §5 攻击面对照表里"堆栈式间接跳转 + 路径爆炸" → 直接引 §4.5，四层强度与代价表可直接搬。
- §9 资料库索引清单 → 与本文 §9 合并，去重后按"防御目标"而非"工具名"组织。

---

## 11. L3 档上线顺序（别一次全开）

| 阶段 | 开启 | 观察指标 |
|---|---|---|
| W1 | L2 结构层全开 + 字符串重写 | 编译耗时、体积、冷启动 |
| W2 | + IndirectBranch 改造 | hot 路径时延 |
| W3 | + SJI L1（非 hot） | angr/d810 回归耗时 |
| W4 | + SJI L2 ret-sled（5% 函数） | 真机崩溃率（重点看 BTI/PAC） |
| W5 | + FunctionWrapper W2/W3 | 稳定性 3 万次 |
| W6 | + SJI L3 TLS（2%） | 长稳、fiber 场景 |

每阶段必须过 §6.4 门禁才进下一阶段。**L3 档产品化的最大风险不是强度不够，是稳定性 —— 崩一个就是差评一片。**

---

## 12. 代码落地对照表（谁实现了本文的哪一条）

| 文档条目 | 实现文件 | 入口 | CLI 开关 | 关键点 |
|---|---|---|---|---|
| §4.1 虚假控制流 | `ObfBcf.cpp` | `obf::processBogusControlFlow(F)` | `-obf-bcf-prob` `-obf-bcf-depth` | **边守卫**而不是块首判断；守卫末尾统一汇入 `Join` 块再进后继 |
| §4.2 花指令 | `JunkCode.cpp` | `obf::processJunkCode(F)` | `-obf-junk-prob` | 数据岛用 `.p2align 2`+`.word`，不放有重定位的内容 |
| §4.3 字符串加密 | `StringEncryption.cpp` + `StringEncrypt.cpp` | `obf::processStringEncrypt(M)` | `-enable-strcry` | 治本：索引遍历 + `progress` + 轮数上限；止痛：`isValidStringTry` 的 if 分支 |
| §4.4 间接跳转 | 上游 `IndirectBranch.cpp` | `createIndirectBranchPass` | `-enable-indibran` | 只在 SJI 之前跑，别让 SJI 把它吃掉 |
| §4.5 堆栈式间接跳转 | `StackIndirectJump.cpp` | `obf::processStackIndirectJump(F)` | `-obf-sji` `-obf-sji-ratio` | L1 栈机 / L2 ret-sled / L3 TLS / L4 跨调用接力 |
| §4.6 函数包装 | `ObfFuncWrap.cpp` | `obf::processFunctionWrapper(M)` | `-obf-wrap-prob` `-obf-wrap-layer` | 不碰 LR；TLS 影子栈 + cookie 校验 + 失败 `brk #0` |
| **§4.7 常量加密** | `ObfConstEnc.cpp` | `obf::processConstantEncryption(F)` | `-obf-ce-prob` `-obf-ce-strength` | 整数四段链 + **浮点 bitcast 通道**（上游 Goron/Hikari 都没有） |

调度点在 `Obfuscation.cpp` 的 `runOnModule` 里，全部落在 legacy PM 的既有流程上
（上游是 `createXxxPass()` 那一套，new-PM mixin 挂不进去，所以我们每个 pass
都同时给 `process*(Function&/Module&)` 和 `run*(..., AnalysisManager&)` 两套入口）。

### 12.1 本轮修掉的真 bug（都写进了对应文件头，防回归）

| 编号 | 位置 | 症状 | 修法 |
|---|---|---|---|
| B1 | `StringEncryption.cpp` 遍历 `Globals` | range-for 里 `emplace_back` → 迭代器失效（UB） | 索引遍历 + 尺寸快照 |
| B2 | 同上 `} while (transedGlobals.size() != Globals.size())` | 某轮无新增即**恒真空转**，表现"混淆字符串时编译卡住" | `progress` 驱动 + 4096 轮上限 + 超限打印剩余数 |
| B3 | `StackIndirectJump.cpp` v1 | 表项存 `blockaddress + 偏移`，解码却用异或 → 必然跳错 | 改成 `ptrtoint(BA) ^ blockkey`，解码后再 `inttoptr` |
| B4 | `ObfBcf.cpp` v1 | 直接 `setSuccessor` 到守卫 → 后继块的 PHI 入边不匹配，verifier 报错 | 加 `Join` 汇合块，PHI 入边统一从 `From` 改写成 `Join` |
| B5 | `ObfConstEnc.cpp` v1 | 编码与解码各算一套 `K1i/K2i/R` → 编译过、运行崩 | 编解码同源传参；`i1` 强制退化成单段异或 |
| B6 | `ObfFuncWrap.cpp` v1 | 拿 `depth` 直接当 TLS 数组下标 → 深递归写穿相邻 TLS | 掩码 `&(cap-1)`，`cap=256`（fail-closed 到 `brk #0`） |

### 12.2 验证现状（诚实版）

- **静态 API 校验已过**：`_tools_api_check.py` 扫 8 个源文件 184 个唯一 API 名，
  在 LLVM 20.1.5 的 19.3MB 头文件正文里逐个匹配，**未命中的只有两处非代码命中**：
  `AnnotateInst`（只在注释里出现）与 `ImmArg`（`EnumAttr`，定义在 `Attributes.td`，
  编译期才生成到 `Attributes.inc`，头文件正文里搜不到属正常）。
- **还没做真编译**：本机无 cmake/MSVC/gcc，编译器要等 CI 或你在 Linux/WSL 上跑。
  所以 `tests/run_smoke.sh` 就是那道闸：T1/T5/T6/T7 四条探针分别钉住
  字符串、浮点、整数、包装四件事，**任何一条退化成空跑都会红**。
- **浮点的验证方式很硬**：直接搜 π 的 IEEE754 位模式 `18 2d 44 54 fb 21 09 40`
  ——基线必须 `FOUND`，混淆后必须 `GONE`。不是看 IR 里"有没有 pass 名"。

# ollvm_magic

《OLLVM 魔改方案 · 变态档 v2.0》的配套源码。基座 `Aethereux/Hikari-LLVM19`（LLVM 19，new PM）。

| 文件 | 对应文档 | 内容 |
|---|---|---|
| `ObfCommon.h` | §4.1 §6 | 种子派生、档位、no-obf 双路判定、6 种恒真谓词、InlineAsm 助手 |
| `JunkCode.cpp` | §4.2 | 数据岛 / udf 陷阱 / 假栈帧 |
| `StackIndirectJump.cpp` | §4.5 | L1 IR stack machine、L2 ret-sled、L3 TLS 影子栈 |
| `StringEncrypt.cpp` | §4.3 | 上游死循环修复（B1/B2）+ 单一 blob 重写版 |
| `PassPlugin.cpp` | §3 | 16 步流水线编排 + 插件入口 |
| `CMakeLists.txt` | §6.1 | 树内 / 独立插件两种模式 |
| `build_ndk19.sh` | §6.2 | 一键构建工具链并挂进 NDK r27c |

## 三条铁律

1. **NDK 上没有 `-fpass-plugin`**。`toolchains/llvm/prebuilt/*/lib/` 不含 `libLLVM` 动态库，符号未导出。只能 fork 重编（`build_ndk19.sh` 就是干这个的）。
2. **`-mbranch-protection=none` 是 ret-sled 的前置条件**。开 BTI 会让 `ret` 到 `bti c` 块直接 SIGILL；开 PAC 后尾声是 `retaa`，伪造的 `x30` 过不了校验。见 §4.5 L2 约束表。
3. **字符串 pass 打补丁前不要跑大工程**。`StringEncryption.cpp:181–229` 是确定的死循环，小 demo 测不出来，大 App 必踩。

## 快速自检

```bash
# 1) 工具链是否真的带上了 pass
$CC -mllvm -hikari -mllvm -obf-level=3 -c test.c -o /dev/null && echo OK

# 2) 字符串是否真的没了
$READELF -p .rodata libfoo.so | grep -i "http\|jni\|sign" || echo "clean"

# 3) ret-sled 是否真的生成（应有大量孤立块）
$OBJDUMP -d libfoo.so | grep -c "ret$"
```

## 编译期编码表复核

NDK **没有 `llvm-mc`**（别找了）。复核 AArch64 编码用：

```bash
clang --target=aarch64-linux-gnu -c t.s -o t.o
llvm-objdump -d t.o
```

文档 §4.2 那张表就是这么验出来的。

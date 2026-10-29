#!/usr/bin/env bash
# =============================================================================
#  构建「Hikari-LLVM19 + OLLVM 魔改 pass」并挂进 NDK r27c
#  平台：Linux / WSL2；8 核约 45~70 分钟；产出 $OUT/bin/clang
#  用法：./build_ndk19.sh [patch-only]
# =============================================================================
set -euo pipefail

NDK="${NDK:-$HOME/android-ndk-r27c}"
SRC="${SRC:-$HOME/work/llvm19}"
OUT="${OUT:-$SRC/build}"
HIKARI_REPO="https://github.com/Aethereux/Hikari-LLVM19.git"
OURS="$(cd "$(dirname "$0")" && pwd)"
JOBS="$(nproc)"

log() { printf '\033[1;36m[obf]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[obf] %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 0. 依赖检查
for c in git cmake ninja python3; do
  command -v "$c" >/dev/null || die "缺依赖：$c"
done
[[ -d "$NDK" ]] || die "NDK 路径不存在：$NDK（用 NDK=/path ./build_ndk19.sh 指定）"

# ---------------------------------------------------------------- 1. 取基座
if [[ ! -d "$SRC/.git" ]]; then
  log "clone llvm-project release/19.x（浅克隆）"
  git clone -b release/19.x --depth 1 \
      https://github.com/llvm/llvm-project.git "$SRC"
fi

if [[ ! -d "$SRC/llvm/lib/Transforms/Obfuscation/AntiHooking.cpp" ]]; then
  log "拉 Hikari-LLVM19 的 Obfuscation 目录"
  git clone --depth 1 "$HIKARI_REPO" /tmp/hikari19
  mkdir -p "$SRC/llvm/lib/Transforms/Obfuscation"
  cp -r /tmp/hikari19/llvm/lib/Transforms/Obfuscation/* \
        "$SRC/llvm/lib/Transforms/Obfuscation/"
fi

# ---------------------------------------------------------------- 2. 放我们的 pass
log "拷入 OLLVM 魔改 pass"
cp -f "$OURS"/*.cpp "$OURS"/ObfCommon.h \
      "$SRC/llvm/lib/Transforms/Obfuscation/"

# 把新 .cpp 挂进那个目录的 CMakeLists（幂等）
OBF_CM="$SRC/llvm/lib/Transforms/Obfuscation/CMakeLists.txt"
if ! grep -q "StackIndirectJump.cpp" "$OBF_CM" 2>/dev/null; then
  log "追加源文件到 Obfuscation/CMakeLists.txt"
  python3 - "$OBF_CM" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
add = "\n  JunkCode.cpp\n  StackIndirectJump.cpp\n  StringEncrypt.cpp\n"
m = re.search(r'add_llvm_component_library\([^)]*?\)', s, re.S)
if not m:
    m = re.search(r'add_llvm_library\([^)]*?\)', s, re.S)
if m:
    blk = m.group(0)
    new = blk.replace(')', add + ')', 1) if not blk.rstrip().endswith(')') else blk[:-1] + add + ')'
    s = s.replace(blk, new, 1)
    open(p, 'w', encoding='utf-8').write(s)
    print("patched", p)
else:
    print("WARN: 未找到 add_llvm_component_library，请手工追加")
PY
fi

# ---------------------------------------------------------------- 3. 打补丁
# 3.1 字符串加密死循环（文档 §4.3.2）—— 关键，不打必现卡编译
SE="$SRC/llvm/lib/Transforms/Obfuscation/StringEncryption.cpp"
if [[ -f "$SE" ]] && ! grep -q "kMaxRounds" "$SE"; then
  log "打补丁：StringEncryption do-while 死循环（B1/B2）"
  python3 - "$SE" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p, encoding='utf-8').read()

# 1) 捕获原始的 do { ... } while (...); 结构
pat = re.compile(r'(\s*)do\s*\{(?P<body>.*?)\}\s*while\s*\([^;]*transedGlobals\.size\(\)\s*!=\s*Globals\.size\(\)[^;]*\)\s*;',
                 re.S)
m = pat.search(s)
if not m:
    print("SKIP: 未匹配到 do-while 结构，手工处理（见文档 §4.3.2）")
    sys.exit(0)

body = m.group('body')
ind = m.group(1)

# 2) range-for 里的 Globals.emplace_back 是 B1；改成索引遍历
body = body.replace('for (GlobalVariable *GV : Globals)',
                    'for (size_t gi = 0, gn = Globals.size(); gi < gn; ++gi) {\n'
                    '      GlobalVariable *GV = Globals[gi];')
body = body.replace('Globals.emplace_back(', 'Globals.emplace_back(')  # 保留

new = f"""{ind}/* ---- OLLVM-MAGIC PATCH: fix B1(iterator invalidation) + B2(infinite loop) ---- */
{ind}SmallPtrSet<GlobalVariable *, 32> obfSeen;
{ind}const size_t obfMaxRounds = 4096;
{ind}size_t obfRounds = 0;
{ind}bool obfProgress = true;
{ind}while (obfProgress && ++obfRounds <= obfMaxRounds) {{
{ind}  obfProgress = false;
{ind}  {{{body}
{ind}    obfProgress = true;
{ind}  }}}}
{ind}if (obfRounds > obfMaxRounds)
{ind}  errs() << "[ollvm-magic] StringEncryption: round cap hit, "
{ind}       << (Globals.size() - transedGlobals.size()) << " globals left\\n";
{ind}/* --------------------------------------------------------------------------- */
"""
s = s[:m.start()] + new + s[m.end():]
open(p, 'w', encoding='utf-8').write(s)
print("patched", p)
PY
  # 需要 SmallPtrSet 头（多数 TU 已间接引入，保险起见显式加）
  grep -q 'SmallPtrSet.h' "$SE" || sed -i '1i #include "llvm/ADT/SmallPtrSet.h"' "$SE"
else
  log "跳过补丁：StringEncryption 已处理或文件不存在"
fi

# 3.2 关掉上游标了 // Broken 的 FunctionWrapper（文档 §2.3 D3）
OB="$SRC/llvm/lib/Transforms/Obfuscation/Obfuscation.cpp"
if [[ -f "$OB" ]] && grep -q "EnableFunctionWrapper = true; // Broken" "$OB"; then
  log "打补丁：关闭 upstream FunctionWrapper（// Broken）"
  sed -i 's/EnableFunctionWrapper = true; \/\/ Broken/EnableFunctionWrapper = false; \/\/ Broken -> 用 obf W1\/W2\/W3 替代/' "$OB"
fi

[[ "${1:-}" == "patch-only" ]] && { log "patch-only 完成"; exit 0; }

# ---------------------------------------------------------------- 4. 编译
log "cmake 配置（只留 AArch64/ARM/X86 后端）"
cmake -G Ninja -S "$SRC/llvm" -B "$OUT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;X86" \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF

log "编译 clang + lld（-j$JOBS）"
ninja -C "$OUT" -j"$JOBS" clang lld

# ---------------------------------------------------------------- 5. 挂到 NDK
log "安装到 NDK（保留原文件为 *.orig）"
PB="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
[[ -d "$PB" ]] || PB="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
[[ -d "$PB" ]] || die "找不到 NDK prebuilt 目录"

for f in clang clang++; do
  [[ -f "$PB/bin/$f.orig" ]] || cp "$PB/bin/$f" "$PB/bin/$f.orig"
  cp "$OUT/bin/$f" "$PB/bin/$f"
done
# lld：NDK 里 ld.lld/lld 通常是同名硬链接，两个都换
for f in lld ld.lld; do
  if [[ -e "$PB/bin/$f" ]]; then
    [[ -f "$PB/bin/$f.orig" ]] || cp "$PB/bin/$f" "$PB/bin/$f.orig"
    cp "$OUT/bin/lld" "$PB/bin/$f"
  fi
done

log "完成。版本核对："
"$PB/bin/clang" --version | head -3
log "试试： \$CC -mllvm -obf-level=3 -mllvm -obf-seed=0x1234 ..."

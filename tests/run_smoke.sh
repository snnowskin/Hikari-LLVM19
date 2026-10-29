#!/usr/bin/env bash
# 冒烟测试：证明「档位 3」不是空跑，且同种子可复现。
#   用法： bash tests/run_smoke.sh <build-dir>
#
#   T1 字符串加密     明文从产物里消失        MAGIC_SMOKE_STR_9f3a
#   T2 控制流/间接跳转 留下未解析的 br         compute()/classify()
#   T3 可复现          同种子两次逐字节相同
#   T4 反向对照        关档位时明文必须可见    （防 T1 假通过）
#   T5 浮点常量加密    π 的 IEEE754 位模式消失  obf_fp_probe()
#   T6 整数常量加密    IR 里出现 obf.ce.* 译码链
#   T7 函数包装        IR 里出现 obf_ww_* 与 obf.fw.dec 间接调用
set -uo pipefail

BUILD="${1:-build}"
CC="$BUILD/bin/clang"
OBJDUMP="$BUILD/bin/llvm-objdump"
SRC="tests/smoke/hello.c"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

COMMON=(-target aarch64-linux-gnu -O2 -c -ffreestanding -fno-builtin
        -fno-stack-protector -fno-asynchronous-unwind-tables)

OBF=(-mllvm -hikari
     -mllvm -enable-strcry -mllvm -enable-bcfobf -mllvm -enable-cffobf
     -mllvm -enable-subobf -mllvm -enable-indibran -mllvm -enable-constenc
     -mllvm -obf-level=3 -mllvm -obf-seed=0x51A9
     -mllvm -obf-sji=3 -mllvm -obf-sji-ratio=60 -mllvm -obf-junk-prob=50
     -mllvm -obf-bcf-prob=100 -mllvm -obf-bcf-depth=3
     -mllvm -obf-ce-prob=100 -mllvm -obf-ce-strength=3
     -mllvm -obf-wrap-prob=100 -mllvm -obf-wrap-layer=2)

pass=0; fail=0
ok()   { echo "  PASS  $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $1"; fail=$((fail+1)); }
check(){ if [ "$2" -ge "$3" ]; then ok "$1 ($2 >= $3)"; else bad "$1 ($2 < $3)"; fi; }
# 二进制里是否含某个字节序列（十六进制传入）
hasbytes() {
  python3 - "$1" "$2" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
pat = bytes.fromhex(sys.argv[2])
print("FOUND" if pat in d else "GONE")
PY
}
# π 的 double 位模式 0x400921FB54442D18，小端字节序
PI_BYTES="182d4454fb210940"

echo "== T4 基线：关档位时必须能看见明文 =="
"$CC" "${COMMON[@]}" -o "$OUT/base.o" "$SRC" || { bad "基线编译"; exit 1; }
n=$(grep -a -c "MAGIC_SMOKE_STR_9f3a" "$OUT/base.o" || true)
check "基线明文可见" "${n:-0}" 1
if [ "$(hasbytes "$OUT/base.o" "$PI_BYTES")" = "FOUND" ]; then
  ok "基线含 π 位模式（证明 T5 检测有效）"
else
  bad "基线里就没有 π 位模式，T5 无从判断"
fi

echo "== T1/T2 档位 3 =="
"$CC" "${COMMON[@]}" "${OBF[@]}" -o "$OUT/obf.o" "$SRC" || { bad "混淆编译（编译器崩了）"; exit 1; }
size=$(stat -c%s "$OUT/obf.o" 2>/dev/null || stat -f%z "$OUT/obf.o")
check "产物非空" "$size" 512

n=$(grep -a -c "MAGIC_SMOKE_STR_9f3a" "$OUT/obf.o" || true)
if [ "${n:-1}" -eq 0 ]; then ok "字符串明文已消失"; else bad "字符串明文仍在（加密没生效）"; fi

"$CC" "${COMMON[@]}" "${OBF[@]}" -S -emit-llvm -o "$OUT/obf.ll" "$SRC" 2>/dev/null
n=$(grep -c "indirectbr" "$OUT/obf.ll" || true)
check "IR 里有 indirectbr（SJI 生效）" "${n:-0}" 1
n=$(grep -c "obf\.island\|obf\.udf\|obf\.frame" "$OUT/obf.ll" || true)
check "IR 里有花指令块" "${n:-0}" 1

"$OBJDUMP" -d "$OUT/obf.o" > "$OUT/dis.txt" 2>/dev/null
n=$(grep -cE "[[:space:]]br[[:space:]]+x[0-9]+" "$OUT/dis.txt" || true)
check "反汇编里有间接 br" "${n:-0}" 1

echo "== T5 浮点常量加密（新增） =="
if [ "$(hasbytes "$OUT/obf.o" "$PI_BYTES")" = "GONE" ]; then
  ok "π 位模式已消失（浮点通道生效）"
else
  bad "π 位模式仍在（浮点常量是明文）"
fi
n=$(grep -c "obf\.ce\.bc" "$OUT/obf.ll" || true)
check "IR 里有浮点 bitcast 通道 obf.ce.bc" "${n:-0}" 1

echo "== T6 整数常量加密（新增） =="
n=$(grep -c "obf\.ce\.d" "$OUT/obf.ll" || true)
check "IR 里有整数译码链 obf.ce.d*" "${n:-0}" 3
n=$(grep -c "obf\.ce\.keys" "$OUT/obf.ll" || true)
check "IR 里有 key 表 obf.ce.keys" "${n:-0}" 1

echo "== T7 函数包装（新增） =="
n=$(grep -c "obf_ww_" "$OUT/obf.ll" || true)
check "IR 里有 wrapper 函数 obf_ww_*" "${n:-0}" 1
n=$(grep -c "obf\.fw\.dec" "$OUT/obf.ll" || true)
check "IR 里有加密 callee 表的解密" "${n:-0}" 1

echo "== T3 可复现：同种子两次必须逐字节相同 =="
"$CC" "${COMMON[@]}" "${OBF[@]}" -o "$OUT/obf2.o" "$SRC" 2>/dev/null
if cmp -s "$OUT/obf.o" "$OUT/obf2.o"; then ok "同种子产物一致"; else bad "同种子产物不一致（有隐藏随机源）"; fi

h1=$(sha256sum "$OUT/obf.o" 2>/dev/null | cut -c1-16 || shasum -a 256 "$OUT/obf.o" | cut -c1-16)
echo "  info  产物指纹 $h1 / 体积 ${size}B（基线 $(stat -c%s "$OUT/base.o" 2>/dev/null || stat -f%z "$OUT/base.o")B）"

echo
echo "结果：$pass 通过 / $fail 失败"
[ "$fail" -eq 0 ] || exit 1

#!/usr/bin/env bash
# gate.sh —— 混淆闸门：一次跑完全部组合，最后汇总
#
#   bash tests/gate.sh [clang 路径] [工作目录]
#
# 两种检查：
#   1) 编译期：每个 (旗标组 × 种子 × 源文件) 都加 -obf-verify 编一遍 -c，
#      非法 IR / 后端崩 立即算失败。一个组合失败不影响其它组合继续跑。
#   2) 运行期：rt_probe.c 用 qemu-aarch64 真跑，必须输出 RT-OK 且退出码 0
#      （抓「不崩但算错」的语义 bug）。
#
# 退出码 0 = 全绿，1 = 有失败（末尾会列出所有失败组合）。

set -uo pipefail

CC=${1:-./x/bin/clang}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${2:-$ROOT/.gate}
mkdir -p "$OUT"

emit()  { printf '%s\n' "$*"; }
pass()  { PASSED=$((PASSED + 1)); }
fail()  { FAILS=$((FAILS + 1)); FAILED_LIST+=("$1"); }

PASSED=0
FAILS=0
FAILED_LIST=()
TOTAL=0

QEMU=""
for q in qemu-aarch64-static qemu-aarch64; do
  if command -v "$q" >/dev/null 2>&1; then QEMU=$q; break; fi
done

SEEDS=(0x1337 0x51A9 0xDEADBEEF)

# 每组旗标带上总开关 -mllvm -hikari 和验 IR 的 -obf-verify
FLAGSETS=(
  "strcry|-mllvm -enable-strcry"
  "cff|-mllvm -enable-cffobf"
  "sub|-mllvm -enable-subobf"
  "indibran|-mllvm -enable-indibran"
  "junk|-mllvm -obf-junk-prob=100"
  "bcf|-mllvm -obf-bcf-prob=100 -mllvm -obf-bcf-depth=3"
  "ce|-mllvm -obf-ce-prob=100 -mllvm -obf-ce-strength=3"
  "wrap|-mllvm -obf-wrap-prob=100 -mllvm -obf-wrap-layer=2"
  "sji1|-mllvm -obf-sji=1 -mllvm -obf-sji-ratio=100"
  "sji2|-mllvm -obf-sji=2 -mllvm -obf-sji-ratio=100"
  "sji3|-mllvm -obf-sji=3 -mllvm -obf-sji-ratio=100"
  "L1|-mllvm -obf-level=1"
  "L2|-mllvm -obf-level=2"
  "L3|-mllvm -obf-level=3"
  "all|-mllvm -obf-level=3 -mllvm -obf-junk-prob=100 -mllvm -obf-bcf-prob=100 \
        -mllvm -obf-ce-prob=100 -mllvm -obf-wrap-prob=100 -mllvm -obf-sji=3 \
        -mllvm -obf-sji-ratio=90 -mllvm -enable-strcry -mllvm -enable-cffobf \
        -mllvm -enable-subobf -mllvm -enable-indibran"
)

SRCS=(tests/c/magic.c tests/c/rt_probe.c)

emit "== 编译期矩阵：${#FLAGSETS[@]} 组 × ${#SEEDS[@]} 种子 × ${#SRCS[@]} 源 = $(( ${#FLAGSETS[@]} * ${#SEEDS[@]} * ${#SRCS[@]} )) 次"

for entry in "${FLAGSETS[@]}"; do
  name=${entry%%|*}
  flags=${entry#*|}
  for seed in "${SEEDS[@]}"; do
    for src in "${SRCS[@]}"; do
      TOTAL=$((TOTAL + 1))
      base=$(basename "$src" .c)
      log=$OUT/${name}_${seed}_${base}.log
      if "$CC" --target=aarch64-linux-gnu -O2 -c "$src" \
           -o "$OUT/${name}_${seed}_${base}.o" \
           -mllvm -hikari $flags -mllvm -obf-seed=$seed -mllvm -obf-verify \
           >"$log" 2>&1; then
        # 效果断言：编译过不等于 pass 真的动过手。-obf-level=0 时单给
        # -obf-junk-prob=100 什么都不做，闸门却报 pass —— 那是空跑假绿。
        # SJI 只断言 rt_probe：magic.c 函数块数 <8，buildStackMachine 按
        # 设计跳过（跳过的组合不算通过，只是没有可断言的对象）。
        mk=""
        case $name in
          junk)          mk='udf|\.word' ;;
          sji1|sji2|sji3) [ "$base" = rt_probe ] && mk='indirectbr' ;;
          strcry)        mk='obf_str_blob' ;;
          ce)            mk='obf\.ce\.keys' ;;
          wrap)          mk='obf_ww_' ;;
          bcf)           mk='call void asm sideeffect' ;;
        esac
        if [ -n "$mk" ]; then
          "$CC" --target=aarch64-linux-gnu -O2 -S -emit-llvm "$src" \
            -o "$OUT/${name}_${seed}_${base}.ll" \
            -mllvm -hikari $flags -mllvm -obf-seed=$seed >>"$log" 2>&1
          if ! grep -qE "$mk" "$OUT/${name}_${seed}_${base}.ll" 2>/dev/null; then
            fail "$name seed=$seed $base (无效果)"
            emit "-- FAIL $name seed=$seed $base：IR 里找不到混淆痕迹（$mk）"
            continue
          fi
        fi
        pass
      else
        fail "$name seed=$seed $base"
        emit "-- FAIL $name seed=$seed $base"
        # 失败时用 -obf-debug 重跑一次，把 pass 内部的分步 trace 打出来
        "$CC" --target=aarch64-linux-gnu -O2 -c "$src" -o /dev/null \
          -mllvm -hikari $flags -mllvm -obf-seed=$seed -mllvm -obf-debug \
          >"$OUT/dbg_${name}_${seed}_${base}.log" 2>&1 || true
        tail -n 45 "$OUT/dbg_${name}_${seed}_${base}.log" | sed 's/^/   /'
      fi
    done
  done
done

emit "== 运行期：rt_probe 经 qemu 真跑（基线 + L1/L2/L3）"

if [ -z "$QEMU" ]; then
  emit "-- 跳过：没装 qemu-aarch64（CI 里 apt-get install -y qemu-user-static）"
else
  for tier in base L1 L2 L3; do
    TOTAL=$((TOTAL + 1))
    case $tier in
      base) tflags="" ;;
      L1)   tflags="-mllvm -obf-level=1" ;;
      L2)   tflags="-mllvm -obf-level=2" ;;
      L3)   tflags="-mllvm -obf-level=3 -mllvm -obf-junk-prob=100 \
                    -mllvm -obf-bcf-prob=100 -mllvm -obf-ce-prob=100 \
                    -mllvm -obf-wrap-prob=100 -mllvm -obf-sji=3 \
                    -mllvm -obf-sji-ratio=90 -mllvm -enable-strcry \
                    -mllvm -enable-cffobf -mllvm -enable-subobf \
                    -mllvm -enable-indibran" ;;
    esac
    elf=$OUT/rt_$tier.elf
    log=$OUT/rt_$tier.log
    hk=""
    [ "$tier" != base ] && hk="-mllvm -hikari"
    if ! "$CC" --target=aarch64-linux-gnu -O2 -nostdlib -static \
         -fuse-ld=lld "$ROOT/tests/c/rt_probe.c" -o "$elf" \
         $hk $tflags -mllvm -obf-seed=0x51A9 -mllvm -obf-verify >"$log" 2>&1; then
      fail "rt $tier 链接"
      emit "-- FAIL rt $tier 链接"
      tail -n 45 "$log" | sed 's/^/   /'
      continue
    fi
    out=$("$QEMU" "$elf" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q 'RT-OK'; then
      pass
    else
      fail "rt $tier 运行 rc=$rc"
      emit "-- FAIL rt $tier 运行 rc=$rc out=$out"
    fi
  done
fi

emit ""
emit "== 结果：通过 $PASSED / $TOTAL，失败 $FAILS"
if [ "$FAILS" -ne 0 ]; then
  for f in "${FAILED_LIST[@]}"; do emit "   失败: $f"; done
  exit 1
fi
emit "   全绿"

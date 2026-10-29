/* 冒烟目标：不依赖 libc，可被 -target aarch64-linux-gnu -c 独立编译。
 *
 * 设计意图（对应《OLLVM魔改方案》验证矩阵）：
 *   T1  字符串加密必须让明文从产物里消失        → MAGIC_SMOKE_STR_9f3a
 *   T2  间接跳转/平坦化必须留下未解析的 br      → compute() 的多层 switch
 *   T3  同种子两次构建产物必须逐字节相同        → 可复现性
 *   T4  关闭档位时必须能看见明文（证明检测有效）→ 防止 T1 假通过
 */

volatile int g_sink;

/* T1 探针：这个字面量必须是唯一的、足够长的、不会被优化的 */
const char *obf_smoke_msg(void) {
  return "MAGIC_SMOKE_STR_9f3a_MAGIC_SMOKE_STR_9f3a";
}

/* T2 探针：多层嵌套分派，给 BCF / FLA / SJI 足够的块和边 */
int compute(int a, int b) {
  int acc = a ^ b;
  for (int i = 0; i < 8; i++) {
    switch ((acc + i) & 7) {
    case 0: acc = acc * 3 + 1; break;
    case 1: acc ^= 0x5a5a; break;
    case 2: acc += b << 2; break;
    case 3: acc -= a >> 1; break;
    case 4: acc = (acc << 1) | (acc >> 31); break;
    case 5: acc = ~acc; break;
    case 6: acc += (int)(acc * 7); break;
    default: acc = acc / 3 + 11; break;
    }
  }
  return acc;
}

/* T5 探针：浮点常量。
 * 常量加密（ObfConstEnc.cpp）必须把 IEEE754 位模式一起编掉，
 * 所以产物里不该再出现 π 的 double 位模式 40 09 21 FB 54 44 2D 18
 * （小端字节序列 18 2d 44 54 fb 21 09 40）。
 * 用 double 而不是 float，是因为 8 字节模式在二进制里几乎不可能撞车。 */
double obf_fp_probe(double x) {
  return x * 3.141592653589793 + 2.718281828459045;
}

/* T6 探针：整数常量。0x7f4a7c15 是 xxhash 的黄金比例常数，
 * 加密后这些立即数必须从 .text 里消失（或至少数量显著下降）。 */
unsigned obf_int_probe(unsigned a) {
  return (a * 2654435761u) + 0x7f4a7c15u;
}

/* T5/T6 探针：再给一个带 early-exit 的函数，制造多出口与不可达块 */
int classify(int v) {
  if (v < 0) {
    g_sink = v;
    return -1;
  }
  if ((v & 1) == 0)
    return compute(v, 3);
  return compute(v, 7) + 1;
}

void _start(void) {
  g_sink = classify(0x1234) + (int)(long)obf_smoke_msg();
  /* 让浮点/整数探针真的被引用，避免被 LTO 或 PassManager 删掉 */
  g_sink += (int)obf_fp_probe((double)g_sink);
  g_sink ^= (int)obf_int_probe((unsigned)g_sink);
}

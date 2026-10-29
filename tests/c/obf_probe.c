// 交付验证用探针：编成 arm64-v8a 的 .so，检查字符串是否还明文、CFG 是否被平坦化。
// 不依赖 libc，纯计算 + 内嵌字面量。

#define OBF_NOOBF __attribute__((noinline, annotate("no_obf")))

const char *obf_tag(void) {
  return "OBF_PROBE_PLAINTEXT_9f3a__OBF_PROBE_PLAINTEXT_9f3a";
}

int obf_dispatch(int op, int a, int b) {
  int r = 0;
  switch (op & 7) {
  case 0: r = a + b; break;
  case 1: r = a - b; break;
  case 2: r = a * b; break;
  case 3: r = (b != 0) ? (a / b) : 0; break;
  case 4: r = a ^ b; break;
  case 5: r = a | (b << 3); break;
  case 6: r = (a < b) ? a : b; break;
  default: r = -a; break;
  }
  return r * 3 + 1;
}

extern int obf_dispatch(int, int, int);

int obf_loop(int n) {
  int acc = 0;
  for (int i = 0; i < n; ++i) {
    acc = obf_dispatch(i, acc, i + 1);
    if ((acc & 0x1000) != 0)
      acc ^= 0x55;
  }
  return acc;
}

double obf_fp(double x) { return x * 3.141592653589793 + 2.718281828459045; }

OBF_NOOBF int obf_entry(void) {
  return obf_loop(8) + (int)obf_fp(1.0) + (obf_tag()[0] != 0);
}

/* B27 验证探针。
 *
 * 刻意把函数名取成 f_*（不带 obf_ 前缀）—— 我们的 pass 有一份「自家运行时
 * 不混淆」的保护名单，前缀就是 obf_ / hikari_ / __obf_runtime_，用 obf_tag()
 * 这种名字测常量加密会一律被跳过，看起来像 pass 没生效。
 */

static const char kMsg[] = "MAGIC_PLAINTEXT_7c1e__MAGIC_PLAINTEXT_7c1e";

/* 用 volatile 下标，避免 -O2 把 kMsg 折成立即数、连字符串常量一起删掉 */
int f_msg(int i) {
  volatile int idx = i & 15;
  return kMsg[idx];
}

int f_loop(int a) {
  int acc = 0;
  for (int i = 0; i < 8; ++i)
    acc += (a ^ ((int)i * 0x9E3779B1u));
  return acc;
}

int f_switch(int op) {
  switch (op) {
  case 0:  return 0x1234;
  case 1:  return 0x89AB;
  case 2:  return 0xCDEF;
  case 3:  return 0x1357;
  case 4:  return 0x2468;
  default: return -1;
  }
}

double f_fp(double x) { return x * 3.141592653589793 + 2.718281828459045; }

unsigned f_mix(unsigned a, unsigned b) {
  a = (a * 2654435761u) + 0x7F4A7C15u;
  b = (b ^ 0xA5A5A5A5u) << 3;
  return a ^ b;
}

void _start(void) {
  volatile int s = f_msg(3);
  s += f_loop(s & 7);
  s += f_switch(s & 7);
  s += (int)f_fp((double)s);
  s += (int)f_mix((unsigned)s, 0x5A5A5A5Au);
  for (;;) {
  }
}

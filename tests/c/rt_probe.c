/* rt_probe.c —— 运行期正确性探针（无 libc，aarch64 静态可执行）
 *
 * 用法：
 *   clang --target=aarch64-linux-gnu -nostdlib -static -O2 rt_probe.c -o rt.elf <混淆旗标>
 *   qemu-aarch64-static ./rt.elf   期望退出码 0
 *
 * 原理：同一份函数体被编译两遍 —— 一遍会进混淆流水线，一遍带
 * annotate("no_obf") 被 isNoObf() 放过。运行期逐个比对，任何一个不一致
 * 就 exit(序号)。这样「不崩但算错」那类 bug 也能在 CI 里抓到。
 */

typedef unsigned int u32;
typedef unsigned long long u64;

static long sc(long n, long a, long b, long c) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}

static void wr(const char *s, u64 n) { sc(64, 1, (long)s, (long)n); }
static void ex(int code) __attribute__((noreturn));
static void ex(int code) {
  sc(94, code, 0, 0);
  for (;;) {
  }
}

/* ── 被两侧共用的常量 ── */
#define K1 0x9E3779B97F4A7C15ULL
#define K2 0x7F4A7C15u

/* ── 函数体只写一遍，靠宏把头文件包含两次 ── */
#define BODY(NM) static u64 NM(u64 x)

#define OBF_ATTR __attribute__((noinline))
#define SELF c_recur
#include "impl.inc"
#undef BODY
#undef OBF_ATTR
#undef SELF

#define BODY(NM) static u64 o_##NM(u64 x)
#define OBF_ATTR __attribute__((noinline, annotate("no_obf")))
#define SELF o_c_recur
#include "impl.inc"
#undef BODY
#undef OBF_ATTR
#undef SELF

static const char msg_ok[] = "RT-OK\n";
static const char msg_bad[] = "RT-BAD\n";

void _start(void) {
  int bad = 0;

  for (u64 x = 0; x < 4096; x += 37) {
    if (c_switch(x) != o_c_switch(x)) bad = 1;
    if (c_nestloop(x) != o_c_nestloop(x)) bad = 2;
    if (c_recur(x & 7) != o_c_recur(x & 7)) bad = 3;
    if (c_fp(x) != o_c_fp(x)) bad = 4;
    if (c_mix(x) != o_c_mix(x)) bad = 5;
    if (c_str(x) != o_c_str(x)) bad = 6;
    if (c_stack(x) != o_c_stack(x)) bad = 7;
  }

  if (bad) {
    wr(msg_bad, sizeof(msg_bad) - 1);
    ex(bad);
  }
  wr(msg_ok, sizeof(msg_ok) - 1);
  ex(0);
}

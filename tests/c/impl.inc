/* impl.inc —— 被 rt_probe.c 包含两次，靠 BODY(NM) 宏决定函数名与属性。
 * 只允许写「以一个 u64 入参、返回 u64」的函数体，不许用 libc。 */

BODY(c_switch) {
  u64 s = 0;
  u32 v = (u32)(x & 15);
  switch (v) {
  case 0:
    s += 3;
    /* fallthrough */
  case 1:
    s += 5;
    break;
  case 2:
  case 3:
    s ^= 0xABCDu;
    /* fallthrough */
  case 4:
    s += 11;
    break;
  case 5:
    s = s * 7 + 1;
    break;
  case 6:
    s -= 29;
    break;
  case 7:
  case 8:
  case 9:
    s += v * 13;
    break;
  case 10:
    s ^= s >> 3;
    break;
  case 11:
    s += 0x1000;
    /* fallthrough */
  case 12:
    s += 0x0100;
    break;
  default:
    s = s + 0x10000 - 1;
    break;
  }
  return (s ^ (u64)v) + K2;
}

BODY(c_nestloop) {
  u64 acc = 0;
  u64 n = (x % 9) + 2;
  for (u64 i = 0; i < n; ++i) {
    u64 row = 0;
    for (u64 j = 0; j < n; ++j) {
      u32 cell = (u32)(i * 31 + j * 17 + x);
      if ((cell & 3) == 0)
        row += cell;
      else if ((cell & 3) == 1)
        row ^= cell;
      else
        row -= cell >> 1;
    }
    for (u64 k = 0; k < 3; ++k)
      row = (row << 1) ^ (row >> 7);
    acc += row;
  }
  return acc ^ K1;
}

BODY(c_recur) {
  if (x == 0)
    return 1;
  u64 a = SELF(x - 1); /* 递归调用自己：SELF 由包含方定义，避免串到另一个副本 */
  return a * 3 + x;
}

BODY(c_fp) {
  double d = (double)x;
  double s = 0.0;
  for (int i = 0; i < 5; ++i)
    s = s * 1.5 + d / ((double)(i + 3)) + 3.141592653589793;
  volatile double vd = s;
  union {
    double d;
    u64 u;
  } cv;
  cv.d = (double)vd;
  return cv.u ^ (u64)x;
}

BODY(c_mix) {
  u32 a = (u32)x;
  for (int i = 0; i < 6; ++i) {
    a ^= a << 7;
    a = (a >> 3) | (a << 29);
    a *= 2654435761u;
    a += (u32)i * 0x9E3779B9u;
  }
  u64 b = ((u64)a << 32) ^ (u64)(a ^ 0x5A5A5A5Au);
  return b ^ K1;
}

BODY(c_str) {
  const char *p = "OBF_PROBE_PLAINTEXT_9f3a_marker_string_for_runtime_check";
  u64 h = 5381;
  u64 i = 0;
  while (p[i]) {
    h = h * 33 + (u64)(unsigned char)p[i];
    ++i;
  }
  return (h ^ x) + i;
}

BODY(c_stack) {
  volatile u32 buf[64];
  u32 n = (u32)(x & 63);
  for (u32 i = 0; i < 64; ++i)
    buf[i] = (u32)(i * 7 + n);
  u64 s = 0;
  for (u32 i = 0; i < 64; ++i)
    s += buf[(i + n) & 63];
  return s + n * 3 + (s >> 5);
}

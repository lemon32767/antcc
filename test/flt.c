typedef unsigned u32;
typedef signed s32;
typedef float f32;
typedef double f64;
typedef unsigned long long u64;
typedef signed long long s64;

f32 s32_to_f32(s32 x) { return x; }
f32 u32_to_f32(u32 x) { return x; }
f64 s32_to_f64(s32 x) { return x; }
f64 u32_to_f64(u32 x) { return x; }
f32 s64_to_f32(s64 x) { return x; }
//f32 u64_to_f32(u64 x) { return x; }
f64 s64_to_f64(s64 x) { return x; }
//f64 u64_to_f64(u64 x) { return x; }
s32 f32_to_s32(f32 x) { return x; }
u32 f32_to_u32(f32 x) { return x; }
s32 f64_to_s32(f64 x) { return x; }
u32 f64_to_u32(f64 x) { return x; }
s64 f32_to_s64(f32 x) { return x; }
u64 f32_to_u64(f32 x) { return x; }
s64 f64_to_s64(f64 x) { return x; }
u64 f64_to_u64(f64 x) { return x; }
f32 f64_to_f32(f64 x) { return x; }
f64 f32_to_f64(f32 x) { return x; }

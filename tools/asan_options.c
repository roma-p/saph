// AddressSanitizer's options for MillSaph's programs built with MILLSAPH_SANITIZE
// (tools/sanitize.sh), read when they start (no environment variable). CUDA's driver maps
// memory in the shadow gap ASan protects by default: cuInit then fails with out of memory and
// every render falls back to the CPU; leaving the gap unprotected lets CUDA run (and be checked).
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "protect_shadow_gap=0"; }

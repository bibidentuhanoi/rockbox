  /* First some generic implementations */
#if defined(HAVE_WIN32_FIBER_THREADS)
  #include "thread-win32.c"
#elif defined(HAVE_SIGALTSTACK_THREADS)
  #include "thread-unix.c"

  /* Now the CPU-specific implementations */
#elif defined(CPU_ARM_CLASSIC) || defined(CPU_ARM_APPLICATION)
  #include "arm/thread-classic.c"
#elif defined(CPU_ARM_MICRO)
  #include "arm/thread-micro.c"
#elif defined(CPU_COLDFIRE)
  #include "m68k/thread.c"
#elif defined(CPU_MIPS)
  #include "mips/thread.c"
#elif defined(ESP32)
  /* ESP32 uses FreeRTOS threads via thread-esp32.c — no asm context switch.
     Provide stubs so the generic thread.c compiles; the real implementations
     in thread-esp32.c override every function that calls these. */
  static FORCE_INLINE void store_context(void* addr) { (void)addr; }
  static FORCE_INLINE void load_context(const void* addr) { (void)addr; }
#else
  /* Nothing? OK, give up */
  #error Missing thread impl
#endif

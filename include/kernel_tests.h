#ifndef KERNEL_TESTS_H
#define KERNEL_TESTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run all Greg:: foundation tests and print results to VGA/GFX terminal.
   Call once from kmain after graphics are initialized, before the shell loop. */
void run_foundation_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_TESTS_H */

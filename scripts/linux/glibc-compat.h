/* Binds the math functions glibc 2.44 versioned anew (sinh, cosh) to their old versions, so a
   build made on glibc 2.44 runs on an older glibc: the Steam Deck's SteamOS 3.9 has 2.43.
   FFmpeg's libavutil/eval.c, linked into librexkernel, calls them. build-sdk.sh adds this file
   to every C file of the SDK (-include). */
#if defined(__linux__) && defined(__x86_64__) && !defined(__ASSEMBLER__)
__asm__(".symver sinh,sinh@GLIBC_2.2.5");
__asm__(".symver cosh,cosh@GLIBC_2.2.5");
#endif

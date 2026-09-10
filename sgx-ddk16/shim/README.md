# armhf shim implementation

The public shims will export the normal armhf EGL, GLESv2, and selected PVR2D
ABIs while calling the private DDK objects through base-AAPCS function-pointer
types (`__attribute__((pcs("aapcs")))`).

Generation remains blocked until `build/audit/elf.txt` provides the complete
DDK import/export inventory. In particular, the implementation must cover
`eglGetProcAddress`, reverse callbacks, variadic calls, aggregate ABI rules, and
all ABI-sensitive libc/libm imports before the `shims` target is enabled.
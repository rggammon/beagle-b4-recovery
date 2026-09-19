/* Shared entry point for the hard-float DDK shim veneers. See hf-shim-loader.c. */
#ifndef HF_SHIM_LOADER_H
#define HF_SHIM_LOADER_H

/* Resolve a symbol from the real soft-float DDK library `soname`, bringing up
 * the isolated dlmopen namespace (with libSGXm interposition) on first use.
 * Aborts if the namespace cannot be created. */
void *sgxhf_dlsym(const char *soname, const char *sym);

#endif

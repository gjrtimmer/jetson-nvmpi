#ifndef NVMPI_CTX_BASE_H
#define NVMPI_CTX_BASE_H

#include "nvmpi.h"

/*
 * Complete type behind the opaque nvmpictx* handle of the public API. It is
 * deliberately empty: the decoder (nvmpi_dec_ctx) and the encoder
 * (nvmpi_enc_ctx) derive from it with their own layouts. Two different
 * definitions of one global `struct nvmpictx` would be an ODR violation: the
 * linker merges their same-named inline constructors and constructs one
 * context with the other's constructor (heap overflow at -O0, found by the
 * ASan job). API entry points static_cast the handle to the derived type.
 */
struct nvmpictx {};

#endif

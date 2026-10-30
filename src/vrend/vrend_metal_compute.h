/* SPDX-License-Identifier: MIT */
#ifndef VREND_METAL_COMPUTE_H
#define VREND_METAL_COMPUTE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VREND_METAL_MAX_BUFFERS 4
#define VREND_METAL_MAX_BUFFER_BYTES (1024 * 1024)

struct vrend_metal_shader;
struct vrend_metal_buffer {
   void *data;
   size_t size;
};
struct vrend_metal_binding {
   uint32_t slot;
   uint32_t buffer;
   uint32_t offset;
   uint32_t size;
};
#ifdef __cplusplus
extern "C" {
#endif
bool vrend_metal_compute_available(void);
/* Actual completion timestamp of a retired Metal GPU marker, in nanoseconds.
 * The caller must retire preceding CGL work before inserting this marker. */
bool vrend_metal_timestamp_ns(uint64_t *value);
struct vrend_metal_shader *vrend_metal_compute_create(const char *glsl, uint32_t ssbo_mask);
void vrend_metal_compute_destroy(struct vrend_metal_shader *shader);
bool vrend_metal_compute_dispatch(struct vrend_metal_shader *shader,
   const uint32_t grid[3], const void *constants, size_t constant_size,
   struct vrend_metal_buffer *buffers, size_t buffer_count,
   const struct vrend_metal_binding *bindings, size_t binding_count);
#ifdef __cplusplus
}
#endif
#endif

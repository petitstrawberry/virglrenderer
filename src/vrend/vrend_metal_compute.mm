/* SPDX-License-Identifier: MIT
 * Buffer-only compute execution for CGL hosts without GL compute shaders.
 */
#include "config.h"
#include "vrend_metal_compute.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <shaderc/shaderc.h>
#include <spirv_cross_c.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

struct vrend_metal_shader {
   id<MTLComputePipelineState> pipeline;
   id<MTLCommandQueue> queue;
   uint32_t slots[VREND_METAL_MAX_BUFFERS];
   size_t slot_count;
   size_t constants_size;
   uint32_t block[3];
   bool buffer_sizes;
   std::string entry_name;
};

static id<MTLDevice> metal_device(void)
{
   static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
   return device;
}

static void safe_math(MTLCompileOptions *options)
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
   if (@available(macOS 15.0, *)) {
      options.mathMode = MTLMathModeSafe;
      return;
   }
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
   options.fastMathEnabled = NO;
#pragma clang diagnostic pop
}

bool vrend_metal_compute_available(void)
{
   @autoreleasepool { return metal_device() != nil; }
}

static bool translate(const char *glsl, uint32_t expected_mask,
                      vrend_metal_shader *shader, std::string &msl)
{
   shaderc_compiler_t frontend = shaderc_compiler_initialize();
   shaderc_compile_options_t options = shaderc_compile_options_initialize();
   if (!frontend || !options) {
      if (frontend) shaderc_compiler_release(frontend);
      if (options) shaderc_compile_options_release(options);
      return false;
   }
   shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                          shaderc_env_version_vulkan_1_1);
   shaderc_compilation_result_t result = shaderc_compile_into_spv(frontend,
      glsl, strlen(glsl), shaderc_compute_shader, "virgl-compute", "main", options);
   shaderc_compile_options_release(options);
   shaderc_compiler_release(frontend);
   if (!result) return false;
   if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
      fprintf(stderr, "virgl Metal compute GLSL: %s\n", shaderc_result_get_error_message(result));
      shaderc_result_release(result);
      return false;
   }
   spvc_context context = nullptr;
   if (spvc_context_create(&context) != SPVC_SUCCESS) {
      shaderc_result_release(result);
      return false;
   }
   const bool success = [&]() -> bool {
#define CROSS(call) do { if ((call) != SPVC_SUCCESS) { \
   fprintf(stderr, "virgl Metal compute SPIR-V: %s\n", spvc_context_get_last_error_string(context)); \
   return false; } } while (0)
      spvc_parsed_ir ir;
      CROSS(spvc_context_parse_spirv(context,
         reinterpret_cast<const SpvId *>(shaderc_result_get_bytes(result)),
         shaderc_result_get_length(result) / sizeof(SpvId), &ir));
      spvc_compiler compiler;
      CROSS(spvc_context_create_compiler(context, SPVC_BACKEND_MSL, ir,
                                          SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler));
      spvc_compiler_options opts;
      CROSS(spvc_compiler_create_compiler_options(compiler, &opts));
      CROSS(spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION,
                                           SPVC_MAKE_MSL_VERSION(2, 1, 0)));
      CROSS(spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_BUFFER_SIZE_BUFFER_INDEX, 30));
      CROSS(spvc_compiler_install_compiler_options(compiler, opts));
      spvc_resources resources;
      CROSS(spvc_compiler_create_shader_resources(compiler, &resources));
      const spvc_reflected_resource *list;
      size_t count;
      for (auto type : {SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, SPVC_RESOURCE_TYPE_STORAGE_IMAGE,
                        SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS,
                        SPVC_RESOURCE_TYPE_SUBPASS_INPUT, SPVC_RESOURCE_TYPE_ATOMIC_COUNTER,
                        SPVC_RESOURCE_TYPE_PUSH_CONSTANT}) {
         CROSS(spvc_resources_get_resource_list_for_type(resources, type, &list, &count));
         if (count) return false;
      }
      CROSS(spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &count));
      if (count > 1) return false;
      if (count) {
         if (spvc_compiler_get_decoration(compiler, list[0].id, SpvDecorationDescriptorSet) != 1 ||
             spvc_compiler_get_decoration(compiler, list[0].id, SpvDecorationBinding) != 0)
            return false;
         CROSS(spvc_compiler_get_declared_struct_size(compiler,
            spvc_compiler_get_type_handle(compiler, list[0].base_type_id), &shader->constants_size));
         if (!shader->constants_size || shader->constants_size > 16384) return false;
         spvc_msl_resource_binding binding = {};
         binding.stage = SpvExecutionModelGLCompute;
         binding.desc_set = 1;
         binding.binding = 0;
         binding.msl_buffer = 0;
         CROSS(spvc_compiler_msl_add_resource_binding(compiler, &binding));
      }
      CROSS(spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &list, &count));
      if (count > VREND_METAL_MAX_BUFFERS) return false;
      uint32_t seen = 0;
      for (size_t i = 0; i < count; ++i) {
         uint32_t slot = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
         if (spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet) != 0 ||
             slot >= 32 || !(expected_mask & (1u << slot)) || (seen & (1u << slot)) ||
             spvc_type_get_num_array_dimensions(spvc_compiler_get_type_handle(compiler, list[i].type_id)))
            return false;
         seen |= 1u << slot;
         shader->slots[i] = slot;
         spvc_msl_resource_binding binding = {};
         binding.stage = SpvExecutionModelGLCompute;
         binding.desc_set = 0;
         binding.binding = slot;
         binding.msl_buffer = (unsigned)i + 1;
         CROSS(spvc_compiler_msl_add_resource_binding(compiler, &binding));
      }
      shader->slot_count = count;
      for (unsigned i = 0; i < 3; ++i) {
         shader->block[i] = spvc_compiler_get_execution_mode_argument_by_index(compiler, SpvExecutionModeLocalSize, i);
         if (!shader->block[i] || shader->block[i] > (i == 2 ? 64 : 128)) return false;
      }
      if ((uint64_t)shader->block[0] * shader->block[1] * shader->block[2] > 128) return false;
      const char *source;
      CROSS(spvc_compiler_compile(compiler, &source));
      shader->entry_name = spvc_compiler_get_cleansed_entry_point_name(compiler, "main", SpvExecutionModelGLCompute);
      shader->buffer_sizes = spvc_compiler_msl_needs_buffer_size_buffer(compiler);
      msl = source;
      return true;
#undef CROSS
   }();
   spvc_context_destroy(context);
   shaderc_result_release(result);
   return success;
}

struct vrend_metal_shader *vrend_metal_compute_create(const char *glsl, uint32_t ssbo_mask)
{
   @autoreleasepool {
      if (!glsl || !metal_device()) return nullptr;
      auto *shader = new (std::nothrow) vrend_metal_shader();
      if (!shader) return nullptr;
      std::string source;
      if (!translate(glsl, ssbo_mask, shader, source)) { delete shader; return nullptr; }
      NSError *error = nil;
      MTLCompileOptions *options = [MTLCompileOptions new];
      options.languageVersion = MTLLanguageVersion2_1;
      safe_math(options);
      id<MTLLibrary> library = [metal_device() newLibraryWithSource:
         [NSString stringWithUTF8String:source.c_str()] options:options error:&error];
      id<MTLFunction> entry = [library newFunctionWithName:[NSString stringWithUTF8String:shader->entry_name.c_str()]];
      if (entry) shader->pipeline = [metal_device() newComputePipelineStateWithFunction:entry error:&error];
      shader->queue = [metal_device() newCommandQueue];
      if (!shader->pipeline || !shader->queue ||
          (uint64_t)shader->block[0] * shader->block[1] * shader->block[2] > shader->pipeline.maxTotalThreadsPerThreadgroup) {
         fprintf(stderr, "virgl Metal compute pipeline: %s\n", error ? error.localizedDescription.UTF8String : "unavailable");
         delete shader;
         return nullptr;
      }
      return shader;
   }
}

void vrend_metal_compute_destroy(struct vrend_metal_shader *shader) { delete shader; }

bool vrend_metal_compute_dispatch(struct vrend_metal_shader *shader,
   const uint32_t grid[3], const void *constants, size_t constant_size,
   struct vrend_metal_buffer *buffers, size_t buffer_count,
   const struct vrend_metal_binding *bindings, size_t binding_count)
{
   @autoreleasepool {
      if (!shader || !grid || buffer_count > VREND_METAL_MAX_BUFFERS ||
          binding_count > VREND_METAL_MAX_BUFFERS || (constant_size && !constants)) return false;
      for (unsigned i = 0; i < 3; ++i) if (!grid[i] || grid[i] > 65535) return false;
      id<MTLBuffer> backing[VREND_METAL_MAX_BUFFERS];
      for (size_t i = 0; i < buffer_count; ++i) {
         if (!buffers[i].data || !buffers[i].size || buffers[i].size > VREND_METAL_MAX_BUFFER_BYTES) return false;
         backing[i] = [metal_device() newBufferWithBytes:buffers[i].data length:buffers[i].size options:MTLResourceStorageModeShared];
         if (!backing[i]) return false;
      }
      id<MTLBuffer> uniform = nil;
      if (shader->constants_size) {
         uniform = [metal_device() newBufferWithLength:shader->constants_size options:MTLResourceStorageModeShared];
         if (!uniform) return false;
         memset(uniform.contents, 0, shader->constants_size);
         if (constant_size) memcpy(uniform.contents, constants, constant_size < shader->constants_size ? constant_size : shader->constants_size);
      }
      id<MTLCommandBuffer> command = [shader->queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      if (!command || !encoder) return false;
      [encoder setComputePipelineState:shader->pipeline];
      if (uniform) [encoder setBuffer:uniform offset:0 atIndex:0];
      uint32_t sizes[31] = {};
      for (size_t i = 0; i < shader->slot_count; ++i) {
         const vrend_metal_binding *binding = nullptr;
         for (size_t j = 0; j < binding_count; ++j)
            if (bindings[j].slot == shader->slots[i]) {
               if (binding) { [encoder endEncoding]; return false; }
               binding = &bindings[j];
            }
         if (!binding || binding->buffer >= buffer_count || !binding->size ||
             binding->offset % 4 || binding->offset > buffers[binding->buffer].size ||
             binding->size > buffers[binding->buffer].size - binding->offset) {
            [encoder endEncoding];
            return false;
         }
         [encoder setBuffer:backing[binding->buffer] offset:binding->offset atIndex:i + 1];
         sizes[i + 1] = binding->size;
      }
      if (shader->buffer_sizes) [encoder setBytes:sizes length:sizeof(sizes) atIndex:30];
      [encoder dispatchThreadgroups:MTLSizeMake(grid[0], grid[1], grid[2])
         threadsPerThreadgroup:MTLSizeMake(shader->block[0], shader->block[1], shader->block[2])];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
      if (command.status != MTLCommandBufferStatusCompleted) {
         fprintf(stderr, "virgl Metal compute command: %s\n", command.error.localizedDescription.UTF8String);
         return false;
      }
      for (size_t i = 0; i < buffer_count; ++i) memcpy(buffers[i].data, backing[i].contents, buffers[i].size);
      return true;
   }
}

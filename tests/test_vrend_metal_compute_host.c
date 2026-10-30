/* SPDX-License-Identifier: MIT
 * Send actual VirGL packets through the public renderer API on a CGL host.
 */
#undef NDEBUG
#define GL_SILENCE_DEPRECATION 1
#include "config.h"
#include "virglrenderer.h"
#include "virgl_hw.h"
#include "virgl_protocol.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "vrend/vrend_metal_compute.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static CGLContextObj share;
static unsigned last_fence;
static void fence(void *cookie, uint32_t value) { (void)cookie; last_fence = value; }
static virgl_renderer_gl_context create(void *cookie, int scanout, struct virgl_renderer_gl_ctx_param *p)
{
   (void)cookie; (void)scanout;
   if (p->major_ver > 4 || (p->major_ver == 4 && p->minor_ver > 1)) return NULL;
   CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile,
      (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core, kCGLPFAAccelerated, 0 };
   CGLPixelFormatObj format;
   CGLContextObj context;
   GLint count;
   if (CGLChoosePixelFormat(attrs, &format, &count) != kCGLNoError) return NULL;
   CGLError result = CGLCreateContext(format, share, &context);
   CGLDestroyPixelFormat(format);
   return result == kCGLNoError ? context : NULL;
}
static void destroy(void *cookie, virgl_renderer_gl_context ctx) { (void)cookie; CGLDestroyContext(ctx); }
static int current(void *cookie, int scanout, virgl_renderer_gl_context ctx) { (void)cookie; (void)scanout; return CGLSetCurrentContext(ctx) == kCGLNoError ? 0 : -1; }

struct commands { uint32_t words[32768]; size_t count; };
static void word(struct commands *c, uint32_t value) { assert(c->count < 32768); c->words[c->count++] = value; }
static void header(struct commands *c, unsigned op, unsigned object, unsigned size) { word(c, op | object << 8 | size << 16); }
static void shader(struct commands *c, const char *source)
{
   size_t bytes = strlen(source) + 1, words = (bytes + 3) / 4;
   header(c, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 5 + words);
   word(c, 1); word(c, PIPE_SHADER_COMPUTE); word(c, bytes); word(c, bytes); word(c, 0);
   assert(c->count + words < 32768);
   memset(c->words + c->count, 0, words * 4);
   memcpy(c->words + c->count, source, bytes);
   c->count += words;
   header(c, VIRGL_CCMD_BIND_SHADER, 0, 2); word(c, 1); word(c, PIPE_SHADER_COMPUTE);
}
static void bind(struct commands *c, unsigned slot, unsigned resource, unsigned offset, unsigned size)
{
   header(c, VIRGL_CCMD_SET_SHADER_BUFFERS, 0, 5);
   word(c, PIPE_SHADER_COMPUTE); word(c, slot); word(c, offset); word(c, size); word(c, resource);
}
static void constants(struct commands *c, unsigned salt)
{
   header(c, VIRGL_CCMD_SET_CONSTANT_BUFFER, 0, 6);
   word(c, PIPE_SHADER_COMPUTE); word(c, 0);
   word(c, salt); word(c, 0); word(c, 0); word(c, 0);
}
static void dispatch(struct commands *c)
{
   header(c, VIRGL_CCMD_LAUNCH_GRID, 0, 8);
   for (unsigned i = 0; i < 8; ++i) word(c, (uint32_t[]){4,1,1,2,1,1,0,0}[i]);
   header(c, VIRGL_CCMD_MEMORY_BARRIER, 0, 1); word(c, PIPE_BARRIER_SHADER_BUFFER | PIPE_BARRIER_UPDATE_BUFFER);
}
static void wait_fence(unsigned value)
{
   assert(virgl_renderer_create_fence(value, 1) == 0);
   for (unsigned i = 0; i < 1000 && last_fence != value; ++i) { virgl_renderer_poll(); usleep(1000); }
   assert(last_fence == value);
}
static const char program[] =
   "COMP\nPROPERTY CS_FIXED_BLOCK_WIDTH 4\nPROPERTY CS_FIXED_BLOCK_HEIGHT 1\nPROPERTY CS_FIXED_BLOCK_DEPTH 1\n"
   "DCL SV[0], BLOCK_ID\nDCL SV[1], THREAD_ID\nDCL BUFFER[3]\nDCL BUFFER[7]\nDCL CONST[0]\nDCL TEMP[0..3]\n"
   "IMM[0] UINT32 {4,3,7,0}\n"
   "UMUL TEMP[0].x, SV[0].xxxx, IMM[0].xxxx\nUADD TEMP[0].x, TEMP[0].xxxx, SV[1].xxxx\n"
   "UMUL TEMP[1].x, TEMP[0].xxxx, IMM[0].xxxx\nLOAD TEMP[2].x, BUFFER[7], TEMP[1].xxxx\n"
   "UMUL TEMP[2].x, TEMP[2].xxxx, IMM[0].yyyy\nUMUL TEMP[3].x, TEMP[0].xxxx, IMM[0].zzzz\n"
   "UADD TEMP[2].x, TEMP[2].xxxx, TEMP[3].xxxx\nUADD TEMP[2].x, TEMP[2].xxxx, CONST[0].xxxx\n"
   "STORE BUFFER[3].x, TEMP[1].xxxx, TEMP[2].xxxx\nEND\n";

static const char program3d[] =
   "COMP\nPROPERTY CS_FIXED_BLOCK_WIDTH 2\nPROPERTY CS_FIXED_BLOCK_HEIGHT 2\nPROPERTY CS_FIXED_BLOCK_DEPTH 2\n"
   "DCL SV[0], BLOCK_ID\nDCL SV[1], THREAD_ID\nDCL SV[2], GRID_SIZE\nDCL BUFFER[0]\nDCL TEMP[0..3]\n"
   "IMM[0] UINT32 {2,4,16,100}\n"
   "UMUL TEMP[0].xyz, SV[0].xyzx, IMM[0].xxxx\nUADD TEMP[0].xyz, TEMP[0].xyzx, SV[1].xyzx\n"
   "UMUL TEMP[1].y, TEMP[0].yyyy, IMM[0].yyyy\nUMUL TEMP[1].z, TEMP[0].zzzz, IMM[0].zzzz\n"
   "UADD TEMP[1].x, TEMP[0].xxxx, TEMP[1].yyyy\nUADD TEMP[1].x, TEMP[1].xxxx, TEMP[1].zzzz\n"
   "UMUL TEMP[2].x, TEMP[1].xxxx, IMM[0].yyyy\nUMUL TEMP[3].x, SV[2].xxxx, IMM[0].wwww\n"
   "UADD TEMP[3].x, TEMP[3].xxxx, TEMP[1].xxxx\nSTORE BUFFER[0].x, TEMP[2].xxxx, TEMP[3].xxxx\nEND\n";

int main(void)
{
   if (!vrend_metal_compute_available()) { fprintf(stderr, "SKIP no Metal device\n"); return 77; }
   struct virgl_renderer_gl_ctx_param param = { .major_ver = 4, .minor_ver = 1 };
   share = create(NULL, 0, &param);
   if (!share) { fprintf(stderr, "SKIP no accelerated CGL context\n"); return 77; }
   assert(current(NULL, 0, share) == 0);
   printf("Metal compute host GL=%s renderer=%s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
   struct virgl_renderer_callbacks callbacks = { .version = 1, .write_fence = fence,
      .create_gl_context = create, .destroy_gl_context = destroy, .make_current = current };
   assert(virgl_renderer_init(&callbacks, 0, &callbacks) == 0);
   assert(virgl_renderer_context_create(1, 5, "metal") == 0);
   union virgl_caps caps = {0};
   virgl_renderer_fill_caps(2, 2, &caps);
   assert(caps.v2.capability_bits & VIRGL_CAP_COMPUTE_SHADER);
   assert(caps.v2.max_compute_work_group_invocations == 128);
   assert(caps.v2.max_shader_storage_blocks[PIPE_SHADER_COMPUTE] == 4);
   assert(caps.v2.max_shader_storage_blocks[PIPE_SHADER_FRAGMENT] == 0);
   assert(glGetError() == GL_NO_ERROR);
   uint32_t data[16];
   for (unsigned i = 0; i < 16; ++i) data[i] = 13;
   struct iovec iov = { .iov_base = data, .iov_len = sizeof(data) };
   struct virgl_renderer_resource_create_args args = {
      .handle = 100, .target = PIPE_BUFFER, .format = VIRGL_FORMAT_R8_UNORM,
      .bind = VIRGL_BIND_VERTEX_BUFFER, .width = sizeof(data), .height = 1, .depth = 1, .array_size = 1,
   };
   assert(virgl_renderer_resource_create(&args, NULL, 0) == 0);
   assert(virgl_renderer_resource_attach_iov(100, &iov, 1) == 0);
   virgl_renderer_ctx_attach_resource(1, 100);
   struct virgl_box box = { .w = sizeof(data), .h = 1, .d = 1 };
   assert(virgl_renderer_transfer_write_iov(100, 1, 0, 0, 0, &box, 0, NULL, 0) == 0);
   struct commands c = {0};
   shader(&c, program);
   bind(&c, 3, 100, 16, 32); bind(&c, 7, 100, 16, 32);
   constants(&c, 6699); dispatch(&c);
   constants(&c, 9900); dispatch(&c);
   assert(virgl_renderer_submit_cmd(c.words, 1, c.count) == 0);
   wait_fence(11);
   assert(virgl_renderer_transfer_read_iov(100, 1, 0, 0, 0, &box, 0, NULL, 0) == 0);
   for (unsigned i = 0; i < 16; ++i) {
      uint32_t expected = i >= 4 && i < 12 ? (13*3+(i-4)*7+6699)*3+(i-4)*7+9900 : 13;
      if (data[i] != expected) { fprintf(stderr, "word %u got %u expected %u\n", i, data[i], expected); abort(); }
   }
   printf("PASS VirGL TGSI -> GLSL -> SPIR-V -> MSL -> Metal GPU: two dispatches, SSBO aliases at slots 3/7, offset=16, sentinels, fenced readback\n");
   uint32_t volume[68];
   for (unsigned i = 0; i < 68; ++i) volume[i] = 777;
   struct iovec volume_iov = { .iov_base = volume, .iov_len = sizeof(volume) };
   args.handle = 200; args.width = sizeof(volume);
   assert(virgl_renderer_resource_create(&args, NULL, 0) == 0);
   assert(virgl_renderer_resource_attach_iov(200, &volume_iov, 1) == 0);
   virgl_renderer_ctx_attach_resource(1, 200);
   box.w = sizeof(volume);
   assert(virgl_renderer_transfer_write_iov(200, 1, 0, 0, 0, &box, 0, NULL, 0) == 0);
   c.count = 0;
   header(&c, VIRGL_CCMD_DESTROY_OBJECT, VIRGL_OBJECT_SHADER, 1); word(&c, 1);
   shader(&c, program3d); bind(&c, 0, 200, 8, 256);
   header(&c, VIRGL_CCMD_LAUNCH_GRID, 0, 8);
   for (unsigned i = 0; i < 8; ++i) word(&c, (uint32_t[]){2,2,2,2,2,2,0,0}[i]);
   assert(virgl_renderer_submit_cmd(c.words, 1, c.count) == 0);
   wait_fence(12);
   assert(virgl_renderer_transfer_read_iov(200, 1, 0, 0, 0, &box, 0, NULL, 0) == 0);
   for (unsigned i = 0; i < 68; ++i) assert(volume[i] == (i >= 2 && i < 66 ? 200 + i - 2 : 777));
   printf("PASS 3D workgroup/global IDs, num-workgroups builtin and 64 Metal GPU results\n");
   virgl_renderer_ctx_detach_resource(1, 200);
   virgl_renderer_resource_unref(200);
   /* An unbound shader buffer must fail before dispatch instead of reporting
    * success or dereferencing a missing resource. Use an isolated context. */
   assert(virgl_renderer_context_create(2, 7, "invalid") == 0);
   c.count = 0; shader(&c, program); dispatch(&c);
   assert(virgl_renderer_submit_cmd(c.words, 2, c.count) != 0);
   virgl_renderer_context_destroy(2);
   printf("PASS unbound compute storage rejected\n");
   virgl_renderer_ctx_detach_resource(1, 100);
   virgl_renderer_resource_unref(100);
   virgl_renderer_context_destroy(1);
   virgl_renderer_cleanup(&callbacks);
   CGLSetCurrentContext(NULL);
   CGLDestroyContext(share);
   return 0;
}

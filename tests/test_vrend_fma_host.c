/*
 * SPDX-License-Identifier: MIT
 *
 * Exercise the real TGSI translator and native macOS shader compiler.  Values
 * come from a uniform, so constant folding cannot manufacture the fused result.
 */
#include "config.h"
#include "vrend/vrend_shader.h"
#include "tgsi/tgsi_text.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static const char tgsi[] =
   "FRAG\n"
   "DCL OUT[0], COLOR\n"
   "DCL CONST[0]\n"
   "FMA OUT[0], CONST[0].xxxx, CONST[0].yyyy, CONST[0].zzzz\n"
   "END\n";

static char *translate(const char *text, unsigned version, bool gles, bool supported)
{
   struct tgsi_token tokens[256];
   struct vrend_shader_cfg cfg = { 0 };
   struct vrend_shader_key key = { 0 };
   struct vrend_shader_info info = { 0 };
   struct vrend_variable_shader_info variable = { 0 };
   struct vrend_strarray output;
   assert(tgsi_text_translate(text, tokens, 256));
   cfg.glsl_version = version;
   cfg.use_gles = gles;
   cfg.use_core_profile = !gles;
   cfg.has_gpu_shader5 = supported;
   cfg.max_draw_buffers = 1;
   assert(strarray_alloc(&output, 3));
   bool ok = vrend_convert_shader(NULL, &cfg, tokens, 0, &key, &info,
                                 &variable, &output);
   if (!ok) {
      strarray_free(&output, true);
      return NULL;
   }
   size_t length = 1;
   for (int i = 0; i < output.num_strings; ++i)
      length += strlen(output.strings[i].buf);
   char *result = calloc(length, 1);
   assert(result);
   for (int i = 0; i < output.num_strings; ++i)
      strcat(result, output.strings[i].buf);
   strarray_free(&output, true);
   free(info.sampler_arrays);
   free(info.image_arrays);
   return result;
}

static GLuint compile(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);
   GLint ok;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[4096];
      glGetShaderInfoLog(shader, sizeof(log), NULL, log);
      fprintf(stderr, "%s\n%s\n", source, log);
      abort();
   }
   return shader;
}

int main(void)
{
   char *native = translate(tgsi, 410, false, true);
   assert(native && strstr(native, "#version 400"));
   assert(strstr(native, "fma("));
   assert(!strstr(native, "GL_ARB_gpu_shader5"));
   assert(!translate(tgsi, 410, false, false));

   char *extension = translate(tgsi, 330, false, true);
   assert(extension && strstr(extension, "#extension GL_ARB_gpu_shader5 : require"));
   free(extension);
   extension = translate(tgsi, 310, true, true);
   assert(extension && strstr(extension, "#extension GL_OES_gpu_shader5 : require"));
   assert(!strstr(extension, "multisample_interpolation"));
   free(extension);
   extension = translate(tgsi, 320, true, true);
   assert(extension && !strstr(extension, "#extension GL_OES_gpu_shader5"));
   free(extension);
   extension = translate("COMP\nDCL TEMP[0]\nDCL CONST[0]\n"
                         "FMA TEMP[0], CONST[0].xxxx, CONST[0].yyyy, CONST[0].zzzz\nEND\n",
                         430, false, true);
   assert(extension && strstr(extension, "#version 400"));
   free(extension);

   CGLPixelFormatAttribute attributes[] = {
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)0x4100,
      kCGLPFAAccelerated, (CGLPixelFormatAttribute)0
   };
   CGLPixelFormatObj format;
   CGLContextObj context;
   GLint formats;
   assert(CGLChoosePixelFormat(attributes, &format, &formats) == kCGLNoError);
   assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
   CGLDestroyPixelFormat(format);
   assert(CGLSetCurrentContext(context) == kCGLNoError);
   printf("FMA host GL=%s GLSL=%s renderer=%s\n", glGetString(GL_VERSION),
          glGetString(GL_SHADING_LANGUAGE_VERSION), glGetString(GL_RENDERER));

   const char *vertex = "#version 400\nvoid main() {"
      "vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
      "gl_Position=vec4(p*2.-1.,0.,1.); }";
   GLuint vs = compile(GL_VERTEX_SHADER, vertex);
   GLuint fs = compile(GL_FRAGMENT_SHADER, native);
   free(native);
   GLuint program = glCreateProgram();
   glAttachShader(program, vs);
   glAttachShader(program, fs);
   glLinkProgram(program);
   GLint linked;
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   assert(linked);
   glUseProgram(program);

   float values[] = { 1.0f + 0x1p-23f, 1.0f - 0x1p-23f, -1.0f, 0 };
   uint32_t bits[4];
   memcpy(bits, values, sizeof(bits));
   GLint location = glGetUniformLocation(program, "fsconst0[0]");
   assert(location >= 0);
   glUniform4uiv(location, 1, bits);

   GLuint image, framebuffer, vertex_array;
   glGenTextures(1, &image);
   glBindTexture(GL_TEXTURE_2D, image);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0,
                GL_RGBA, GL_FLOAT, NULL);
   glGenFramebuffers(1, &framebuffer);
   glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, image, 0);
   assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
   glGenVertexArrays(1, &vertex_array);
   glBindVertexArray(vertex_array);
   glViewport(0, 0, 1, 1);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   float result[4];
   glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, result);
   assert(glGetError() == GL_NO_ERROR);
   float expected = fmaf(values[0], values[1], values[2]);
   for (int i = 0; i < 4; ++i)
      assert(result[i] == expected && result[i] == -0x1p-46f);

   /* Separately rounded operations produce zero for the same runtime inputs. */
   volatile float product = values[0] * values[1];
   assert(product + values[2] == 0.0f);
   printf("PASS TGSI FMA -> host GLSL fma -> real GPU readback=%a, ordinary=0\n",
          result[0]);
   glDeleteVertexArrays(1, &vertex_array);
   glDeleteFramebuffers(1, &framebuffer);
   glDeleteTextures(1, &image);
   glDeleteProgram(program);
   glDeleteShader(fs);
   glDeleteShader(vs);
   assert(CGLSetCurrentContext(NULL) == kCGLNoError);
   CGLDestroyContext(context);
   return 0;
}

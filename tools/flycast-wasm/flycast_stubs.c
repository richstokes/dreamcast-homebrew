/* Source for the prebuilt flycast_stubs.o that flycast-wasm v1.0 links,
 * recovered from its bitcode: path helpers RetroArch's file_path.c no longer
 * has, a no-op bm_Reset for the WebAssembly JIT, and GL version strings that
 * Flycast's GLES detection accepts under WebGL 2. */
#include <stddef.h>
#include <string.h>

#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

const unsigned char *__real_glGetString(unsigned name);

static void short_name(char *out, const char *in_path, size_t size)
{
   const char *base = in_path;
   char *ext;
   char *slash;

   if (!out || !size)
      return;
   out[0] = '\0';
   if (!in_path)
      return;
   for (const char *p = in_path; *p; p++)
      if (*p == '/' || *p == '\\')
         base = p + 1;
   strncpy(out, base, size - 1);
   out[size - 1] = '\0';
   ext = strrchr(out, '.');
   slash = strrchr(out, '/');
   if (ext && ext > slash)
      *ext = '\0';
}

void fill_short_pathname_representation(char *out, const char *in_path, size_t size)
{
   short_name(out, in_path, size);
}

void fill_short_pathname_representation_noext(char *out, const char *in_path, size_t size)
{
   short_name(out, in_path, size);
}

void bm_Reset(void)
{
}

const unsigned char *__wrap_glGetString(unsigned name)
{
   if (name == GL_VERSION)
      return (const unsigned char *)"OpenGL ES 3.0 WebGL 2.0";
   if (name == GL_SHADING_LANGUAGE_VERSION)
      return (const unsigned char *)"OpenGL ES GLSL ES 3.00";
   return __real_glGetString(name);
}

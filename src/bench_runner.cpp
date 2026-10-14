// GPU benchmark runner for tests/benchmarks/run.py.
//
// Renders one draw per frame into offscreen color targets with either OpenGL
// (GLSL ES 3.00 sources on a desktop GL 4.3+ core context, which accepts
// `#version 300 es` through ARB_ES3_compatibility) or Vulkan (SPIR-V), and
// times every frame on the GPU: GL_TIME_ELAPSED queries on GL, timestamp
// queries around the render pass on Vulkan. CPU clocks are only used to cap
// how long a run may take, never for the reported times.
//
//   bench_runner --api gl|vulkan --vert FILE --frag FILE --width W --height H
//                [--targets N] [--format rgba8|rgba16f|rgba32f] [--depth]
//                [--vbo FILE STRIDE] [--attr GLNAME TYPE OFFSET VKLOCATION]...
//                [--ibo FILE]
//                [--ubo FILE GLNAME SET BINDING]... [--ssbo FILE SET BINDING]...
//                [--tex FILE W H FORMAT FILTER WRAP GLNAME SET BINDING]...
//                [--frames N] [--min-frames N] [--warmup N]
//                [--warmup-seconds S] [--budget S]
//                [--readback FILE] [--vk-device INDEX]
//
// GL takes GLSL ES source text for --vert/--frag and resolves resources by
// name (attributes with glGetAttribLocation, uniform blocks with
// glGetUniformBlockIndex, sampler uniforms with glGetUniformLocation). Vulkan
// takes SPIR-V and uses the given locations, sets and bindings. A name or
// location of "-" / -1 marks a resource the shader doesn't use.
//
// BWSL's SPIR-V doesn't read vertex inputs: it pulls every attribute from
// its own storage buffer indexed by gl_VertexIndex. Those buffers are passed
// with --ssbo (Vulkan only); the draw count still comes from the vertex (or
// index) buffer.
//
// TYPE is float, float2, float3 or float4. Index buffers hold uint32 indices.
// Texture files are tightly packed RGBA8 or RGBA16F (half floats) texels;
// FILTER is nearest|linear|trilinear, WRAP is clamp|repeat|mirror. A
// trilinear texture's file holds its whole mip chain, level 0 first, each
// level half the size of the previous one (rounded down, at least 1) down to
// 1x1; it is sampled with linear filtering between levels. UBO files are raw
// std140 bytes.
//
// Each frame clears every target (and depth) and issues one draw. Frames run
// until --frames frames are timed, or --budget seconds have passed and at
// least --min-frames are timed. --readback writes every target of the last
// frame as RGBA float32, targets concatenated, rows in GL order (row 0 is NDC
// y = -1 on both APIs).
//
// Prints one JSON object to stdout:
//   {"api": ..., "device": ..., "driver": ..., "warmup_frames": N,
//    "samples_ms": [...]}
// Exit codes: 0 on success, 1 on failure (message on stderr).

#include <vulkan/vulkan.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define BENCH_HAVE_GL 1
#elif defined(__linux__)
#include <EGL/egl.h>
#define BENCH_HAVE_GL 1
#else
#define BENCH_HAVE_GL 0
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
// Ask hybrid-graphics laptops to run GL on the discrete GPU, as Vulkan does.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

[[noreturn]] void die(const std::string &msg) {
  std::fprintf(stderr, "bench_runner: %s\n", msg.c_str());
  std::exit(1);
}

#define VK_CHECK(expr)                                                         \
  do {                                                                         \
    VkResult _r = (expr);                                                      \
    if (_r != VK_SUCCESS) {                                                    \
      std::fprintf(stderr, "bench_runner: %s:%d: %s failed: VkResult=%d\n",    \
                   __FILE__, __LINE__, #expr, (int)_r);                        \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

std::vector<char> read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) die("cannot read " + path);
  std::streamsize n = f.tellg();
  std::vector<char> data(static_cast<size_t>(n));
  f.seekg(0);
  f.read(data.data(), n);
  return data;
}

void write_file(const std::string &path, const void *bytes, size_t size) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) die("cannot write " + path);
  f.write(static_cast<const char *>(bytes), static_cast<std::streamsize>(size));
}

float half_to_float(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t mant = h & 0x3ff;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      // Subnormal: renormalize.
      exp = 127 - 15 + 1;
      while ((mant & 0x400) == 0) {
        mant <<= 1;
        exp--;
      }
      mant &= 0x3ff;
      bits = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000 | (mant << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// ============================================================================
// Arguments
// ============================================================================

enum class TargetFormat { RGBA8, RGBA16F, RGBA32F };
enum class TexFormat { RGBA8, RGBA16F };
enum class Filter { NEAREST, LINEAR, TRILINEAR };
enum class Wrap { CLAMP, REPEAT, MIRROR };

struct Attribute {
  std::string gl_name;
  uint32_t components = 4;
  uint32_t offset = 0;
  int vk_location = -1;
};

struct Ubo {
  std::string path;
  std::string gl_name;
  int set = -1;
  int binding = -1;
};

struct Ssbo {
  std::string path;
  int set = -1;
  int binding = -1;
};

struct Texture {
  std::string path;
  uint32_t width = 0;
  uint32_t height = 0;
  TexFormat format = TexFormat::RGBA8;
  Filter filter = Filter::LINEAR;
  Wrap wrap = Wrap::CLAMP;
  std::string gl_name;
  int set = -1;
  int binding = -1;
};

struct Args {
  std::string api;
  std::string vert_path;
  std::string frag_path;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t targets = 1;
  TargetFormat format = TargetFormat::RGBA8;
  bool depth = false;

  std::string vbo_path;
  uint32_t vbo_stride = 0;
  std::vector<Attribute> attributes;
  std::string ibo_path;

  std::vector<Ubo> ubos;
  std::vector<Ssbo> ssbos;
  std::vector<Texture> textures;

  uint32_t frames = 100;
  uint32_t min_frames = 5;
  uint32_t warmup = 10;
  double warmup_seconds = 0.25;
  double budget_seconds = 2.0;
  std::string readback_path;
  int vk_device = -1;
};

uint32_t parse_u32(const char *s) {
  return static_cast<uint32_t>(std::strtoul(s, nullptr, 10));
}

int parse_int(const char *s) { return static_cast<int>(std::strtol(s, nullptr, 10)); }

std::string parse_name(const char *s) { return std::strcmp(s, "-") == 0 ? "" : s; }

Args parse_args(int argc, char **argv) {
  Args a;
  int i = 1;
  auto next = [&]() -> const char * {
    if (i + 1 >= argc) die(std::string("missing value for ") + argv[i]);
    return argv[++i];
  };
  for (; i < argc; ++i) {
    std::string f = argv[i];
    if (f == "--api") {
      a.api = next();
    } else if (f == "--vert") {
      a.vert_path = next();
    } else if (f == "--frag") {
      a.frag_path = next();
    } else if (f == "--width") {
      a.width = parse_u32(next());
    } else if (f == "--height") {
      a.height = parse_u32(next());
    } else if (f == "--targets") {
      a.targets = parse_u32(next());
      if (a.targets == 0 || a.targets > 8) die("--targets must be in [1, 8]");
    } else if (f == "--format") {
      std::string v = next();
      if (v == "rgba8") a.format = TargetFormat::RGBA8;
      else if (v == "rgba16f") a.format = TargetFormat::RGBA16F;
      else if (v == "rgba32f") a.format = TargetFormat::RGBA32F;
      else die("unknown --format " + v);
    } else if (f == "--depth") {
      a.depth = true;
    } else if (f == "--vbo") {
      a.vbo_path = next();
      a.vbo_stride = parse_u32(next());
    } else if (f == "--attr") {
      Attribute at;
      at.gl_name = parse_name(next());
      std::string type = next();
      if (type == "float") at.components = 1;
      else if (type == "float2") at.components = 2;
      else if (type == "float3") at.components = 3;
      else if (type == "float4") at.components = 4;
      else die("unknown attribute type " + type);
      at.offset = parse_u32(next());
      at.vk_location = parse_int(next());
      a.attributes.push_back(at);
    } else if (f == "--ibo") {
      a.ibo_path = next();
    } else if (f == "--ubo") {
      Ubo u;
      u.path = next();
      u.gl_name = parse_name(next());
      u.set = parse_int(next());
      u.binding = parse_int(next());
      a.ubos.push_back(u);
    } else if (f == "--ssbo") {
      Ssbo b;
      b.path = next();
      b.set = parse_int(next());
      b.binding = parse_int(next());
      a.ssbos.push_back(b);
    } else if (f == "--tex") {
      Texture t;
      t.path = next();
      t.width = parse_u32(next());
      t.height = parse_u32(next());
      std::string fmt = next();
      if (fmt == "rgba8") t.format = TexFormat::RGBA8;
      else if (fmt == "rgba16f") t.format = TexFormat::RGBA16F;
      else die("unknown texture format " + fmt);
      std::string filter = next();
      if (filter == "nearest") t.filter = Filter::NEAREST;
      else if (filter == "linear") t.filter = Filter::LINEAR;
      else if (filter == "trilinear") t.filter = Filter::TRILINEAR;
      else die("unknown texture filter " + filter);
      std::string wrap = next();
      if (wrap == "clamp") t.wrap = Wrap::CLAMP;
      else if (wrap == "repeat") t.wrap = Wrap::REPEAT;
      else if (wrap == "mirror") t.wrap = Wrap::MIRROR;
      else die("unknown texture wrap " + wrap);
      t.gl_name = parse_name(next());
      t.set = parse_int(next());
      t.binding = parse_int(next());
      a.textures.push_back(t);
    } else if (f == "--frames") {
      a.frames = parse_u32(next());
    } else if (f == "--min-frames") {
      a.min_frames = parse_u32(next());
    } else if (f == "--warmup") {
      a.warmup = parse_u32(next());
    } else if (f == "--warmup-seconds") {
      a.warmup_seconds = std::strtod(next(), nullptr);
    } else if (f == "--budget") {
      a.budget_seconds = std::strtod(next(), nullptr);
    } else if (f == "--readback") {
      a.readback_path = next();
    } else if (f == "--vk-device") {
      a.vk_device = parse_int(next());
    } else {
      die("unknown argument " + f);
    }
  }
  if (a.api != "gl" && a.api != "vulkan") die("--api must be gl or vulkan");
  if (a.vert_path.empty() || a.frag_path.empty()) die("--vert and --frag are required");
  if (a.width == 0 || a.height == 0) die("--width and --height are required");
  if (a.vbo_path.empty()) die("--vbo is required");
  if (a.frames == 0) a.frames = 1;
  a.min_frames = std::max(1u, std::min(a.min_frames, a.frames));
  return a;
}

size_t texel_bytes(TexFormat f) { return f == TexFormat::RGBA8 ? 4 : 8; }

uint32_t mip_levels(const Texture &t) {
  if (t.filter != Filter::TRILINEAR) return 1;
  uint32_t levels = 1;
  for (uint32_t size = std::max(t.width, t.height); size > 1; size >>= 1) levels++;
  return levels;
}

uint32_t mip_size(uint32_t size, uint32_t level) { return std::max(1u, size >> level); }

size_t level_bytes(const Texture &t, uint32_t level) {
  return (size_t)mip_size(t.width, level) * mip_size(t.height, level) * texel_bytes(t.format);
}

struct Geometry {
  std::vector<char> vertices;
  std::vector<char> indices;
  uint32_t draw_count = 0;
};

Geometry load_geometry(const Args &a) {
  Geometry g;
  g.vertices = read_file(a.vbo_path);
  if (a.vbo_stride == 0 || g.vertices.size() % a.vbo_stride != 0) {
    die("vertex buffer size is not a multiple of the stride");
  }
  if (!a.ibo_path.empty()) {
    g.indices = read_file(a.ibo_path);
    if (g.indices.size() % 4 != 0) die("index buffer size is not a multiple of 4");
    g.draw_count = static_cast<uint32_t>(g.indices.size() / 4);
  } else {
    g.draw_count = static_cast<uint32_t>(g.vertices.size() / a.vbo_stride);
  }
  return g;
}

std::vector<char> load_texture(const Texture &t) {
  std::vector<char> pixels = read_file(t.path);
  size_t expected = 0;
  for (uint32_t level = 0; level < mip_levels(t); ++level) expected += level_bytes(t, level);
  if (pixels.size() != expected) {
    die("texture " + t.path + " has the wrong size for its extent, format and mip levels");
  }
  return pixels;
}

// Frame pacing shared by both APIs. The CPU clock only decides when to stop.
struct FramePacer {
  using Clock = std::chrono::steady_clock;
  const Args &args;
  Clock::time_point warmup_start = Clock::now();
  Clock::time_point measure_start;
  uint32_t warmup_frames = 0;
  bool measuring = false;

  explicit FramePacer(const Args &a) : args(a) {}

  static double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
  }

  // Called once per submitted frame. Returns true when the frame just
  // submitted should be timed.
  bool submit_frame() {
    if (measuring) return true;
    warmup_frames++;
    double elapsed = seconds_since(warmup_start);
    bool done = warmup_frames >= args.warmup && elapsed >= args.warmup_seconds;
    // Slow shaders: don't spend more than a quarter of the budget warming up.
    bool out_of_time = elapsed >= std::max(args.warmup_seconds, args.budget_seconds * 0.25);
    if (done || out_of_time) {
      measuring = true;
      measure_start = Clock::now();
    }
    return false;
  }

  bool finished(size_t timed_frames) const {
    if (!measuring) return false;
    if (timed_frames >= args.frames) return true;
    return timed_frames >= args.min_frames &&
           seconds_since(measure_start) >= args.budget_seconds;
  }
};

struct Result {
  std::string api;
  std::string device;
  std::string driver;
  uint32_t warmup_frames = 0;
  std::vector<double> samples_ms;
};

std::string json_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((unsigned char)c < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

void print_result(const Result &r) {
  std::printf("{\"api\": \"%s\", \"device\": \"%s\", \"driver\": \"%s\", "
              "\"warmup_frames\": %u, \"samples_ms\": [",
              json_escape(r.api).c_str(), json_escape(r.device).c_str(),
              json_escape(r.driver).c_str(), r.warmup_frames);
  for (size_t i = 0; i < r.samples_ms.size(); ++i) {
    std::printf("%s%.6f", i ? ", " : "", r.samples_ms[i]);
  }
  std::printf("]}\n");
}

// ============================================================================
// OpenGL
// ============================================================================

#if BENCH_HAVE_GL

#if defined(_WIN32)
#define GLAPIENTRY APIENTRY
#else
#define GLAPIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef uint64_t GLuint64;

constexpr GLenum GL_NO_ERROR = 0;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_DITHER = 0x0BD0;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_HALF_FLOAT = 0x140B;
constexpr GLenum GL_COLOR = 0x1800;
constexpr GLenum GL_DEPTH = 0x1801;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_REPEAT = 0x2901;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;
constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_RGBA32F = 0x8814;
constexpr GLenum GL_RGBA16F = 0x881A;
constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_QUERY_RESULT = 0x8866;
constexpr GLenum GL_TIME_ELAPSED = 0x88BF;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLuint GL_INVALID_INDEX = 0xFFFFFFFFu;

#define BENCH_GL_FUNCTIONS(X)                                                  \
  X(GLenum, glGetError, (void))                                                \
  X(const GLubyte *, glGetString, (GLenum))                                    \
  X(void, glEnable, (GLenum))                                                  \
  X(void, glDisable, (GLenum))                                                 \
  X(void, glDepthFunc, (GLenum))                                               \
  X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                        \
  X(void, glFlush, (void))                                                     \
  X(void, glFinish, (void))                                                    \
  X(void, glPixelStorei, (GLenum, GLint))                                      \
  X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
  X(void, glGenTextures, (GLsizei, GLuint *))                                  \
  X(void, glBindTexture, (GLenum, GLuint))                                     \
  X(void, glActiveTexture, (GLenum))                                           \
  X(void, glTexParameteri, (GLenum, GLenum, GLint))                            \
  X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, \
                         GLenum, const void *))                                \
  X(GLuint, glCreateShader, (GLenum))                                          \
  X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
  X(void, glCompileShader, (GLuint))                                           \
  X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                            \
  X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))          \
  X(GLuint, glCreateProgram, (void))                                           \
  X(void, glAttachShader, (GLuint, GLuint))                                    \
  X(void, glLinkProgram, (GLuint))                                             \
  X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                           \
  X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))         \
  X(void, glUseProgram, (GLuint))                                              \
  X(GLint, glGetAttribLocation, (GLuint, const GLchar *))                      \
  X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                     \
  X(void, glUniform1i, (GLint, GLint))                                         \
  X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *))                  \
  X(void, glUniformBlockBinding, (GLuint, GLuint, GLuint))                     \
  X(void, glGenBuffers, (GLsizei, GLuint *))                                   \
  X(void, glBindBuffer, (GLenum, GLuint))                                      \
  X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))            \
  X(void, glBindBufferBase, (GLenum, GLuint, GLuint))                          \
  X(void, glGenVertexArrays, (GLsizei, GLuint *))                              \
  X(void, glBindVertexArray, (GLuint))                                         \
  X(void, glEnableVertexAttribArray, (GLuint))                                 \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei,   \
                                  const void *))                               \
  X(void, glGenFramebuffers, (GLsizei, GLuint *))                              \
  X(void, glBindFramebuffer, (GLenum, GLuint))                                 \
  X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))     \
  X(GLenum, glCheckFramebufferStatus, (GLenum))                                \
  X(void, glGenRenderbuffers, (GLsizei, GLuint *))                             \
  X(void, glBindRenderbuffer, (GLenum, GLuint))                                \
  X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))           \
  X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))         \
  X(void, glDrawBuffers, (GLsizei, const GLenum *))                            \
  X(void, glReadBuffer, (GLenum))                                              \
  X(void, glClearBufferfv, (GLenum, GLint, const GLfloat *))                   \
  X(void, glDrawArrays, (GLenum, GLint, GLsizei))                              \
  X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))             \
  X(void, glGenQueries, (GLsizei, GLuint *))                                   \
  X(void, glBeginQuery, (GLenum, GLuint))                                      \
  X(void, glEndQuery, (GLenum))                                                \
  X(void, glGetQueryObjectui64v, (GLuint, GLenum, GLuint64 *))

#define BENCH_GL_DECLARE(ret, name, params) ret(GLAPIENTRY *name) params = nullptr;
BENCH_GL_FUNCTIONS(BENCH_GL_DECLARE)
#undef BENCH_GL_DECLARE

#if defined(_WIN32)

constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;
typedef HGLRC(WINAPI *CreateContextAttribsFn)(HDC, HGLRC, const int *);

HMODULE g_opengl32 = nullptr;

void *gl_proc(const char *name) {
  void *p = (void *)wglGetProcAddress(name);
  // wglGetProcAddress returns small sentinel values on some drivers and NULL
  // for the GL 1.1 entry points, which opengl32.dll exports directly.
  if (p == nullptr || p == (void *)1 || p == (void *)2 || p == (void *)3 ||
      p == (void *)-1) {
    p = (void *)GetProcAddress(g_opengl32, name);
  }
  return p;
}

void create_gl_context() {
  g_opengl32 = LoadLibraryA("opengl32.dll");
  if (!g_opengl32) die("cannot load opengl32.dll");

  WNDCLASSA wc{};
  wc.style = CS_OWNDC;
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "bwsl_bench_runner";
  RegisterClassA(&wc);
  // Never shown: rendering goes to framebuffer objects.
  HWND hwnd = CreateWindowA(wc.lpszClassName, "bench_runner", WS_OVERLAPPEDWINDOW,
                            0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) die("CreateWindow failed");
  HDC dc = GetDC(hwnd);

  PIXELFORMATDESCRIPTOR pfd{};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cDepthBits = 24;
  int format = ChoosePixelFormat(dc, &pfd);
  if (format == 0 || !SetPixelFormat(dc, format, &pfd)) die("SetPixelFormat failed");

  HGLRC legacy = wglCreateContext(dc);
  if (!legacy || !wglMakeCurrent(dc, legacy)) die("wglCreateContext failed");
  auto create_attribs =
      (CreateContextAttribsFn)wglGetProcAddress("wglCreateContextAttribsARB");
  if (!create_attribs) die("wglCreateContextAttribsARB is not available");

  HGLRC ctx = nullptr;
  const int versions[][2] = {{4, 6}, {4, 3}, {3, 3}};
  for (const auto &v : versions) {
    const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, v[0],
                           WGL_CONTEXT_MINOR_VERSION_ARB, v[1],
                           WGL_CONTEXT_PROFILE_MASK_ARB,
                           WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    ctx = create_attribs(dc, nullptr, attribs);
    if (ctx) break;
  }
  if (!ctx) die("cannot create a GL 3.3+ core context");
  wglMakeCurrent(dc, ctx);
  wglDeleteContext(legacy);
}

#else // EGL

void *gl_proc(const char *name) { return (void *)eglGetProcAddress(name); }

void create_gl_context() {
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
    die("cannot initialize EGL");
  }
  if (!eglBindAPI(EGL_OPENGL_API)) die("eglBindAPI(EGL_OPENGL_API) failed");
  const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                   EGL_NONE};
  EGLConfig config;
  EGLint config_count = 0;
  if (!eglChooseConfig(display, config_attribs, &config, 1, &config_count) ||
      config_count == 0) {
    die("no EGL config for desktop OpenGL");
  }
  const EGLint surface_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attribs);
  EGLContext ctx = EGL_NO_CONTEXT;
  const int versions[][2] = {{4, 6}, {4, 3}, {3, 3}};
  for (const auto &v : versions) {
    const EGLint ctx_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, v[0],
                                  EGL_CONTEXT_MINOR_VERSION, v[1],
                                  EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                  EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                  EGL_NONE};
    ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx != EGL_NO_CONTEXT) break;
  }
  if (ctx == EGL_NO_CONTEXT) die("cannot create a GL 3.3+ core context");
  if (!eglMakeCurrent(display, surface, surface, ctx)) die("eglMakeCurrent failed");
}

#endif

void load_gl_functions() {
#define BENCH_GL_LOAD(ret, name, params)                                       \
  name = (ret(GLAPIENTRY *) params)gl_proc(#name);                             \
  if (!name) die("missing GL function " #name);
  BENCH_GL_FUNCTIONS(BENCH_GL_LOAD)
#undef BENCH_GL_LOAD
}

void gl_check(const char *what) {
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%04x", err);
    die(std::string("GL error ") + buf + " after " + what);
  }
}

GLuint gl_compile(GLenum stage, const std::string &path) {
  std::vector<char> src = read_file(path);
  src.push_back('\0');
  GLuint shader = glCreateShader(stage);
  const GLchar *text = src.data();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    std::string log(std::max(len, 1), '\0');
    glGetShaderInfoLog(shader, len, nullptr, log.data());
    die("GL compile failed for " + path + ":\n" + log);
  }
  return shader;
}

int run_gl(const Args &a) {
  create_gl_context();
  load_gl_functions();

  Result r;
  r.api = "gl";
  r.device = (const char *)glGetString(GL_RENDERER);
  r.driver = (const char *)glGetString(GL_VERSION);

  GLuint vs = gl_compile(GL_VERTEX_SHADER, a.vert_path);
  GLuint fs = gl_compile(GL_FRAGMENT_SHADER, a.frag_path);
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  GLint linked = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLint len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    std::string log(std::max(len, 1), '\0');
    glGetProgramInfoLog(prog, len, nullptr, log.data());
    die("GL link failed:\n" + log);
  }
  glUseProgram(prog);

  // ===== Geometry =====
  Geometry geo = load_geometry(a);
  GLuint vao = 0, vbo = 0, ibo = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)geo.vertices.size(), geo.vertices.data(),
               GL_STATIC_DRAW);
  for (const Attribute &at : a.attributes) {
    if (at.gl_name.empty()) continue;
    GLint loc = glGetAttribLocation(prog, at.gl_name.c_str());
    if (loc < 0) continue;  // optimized out or unused
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, (GLint)at.components, GL_FLOAT, 0,
                          (GLsizei)a.vbo_stride, (const void *)(uintptr_t)at.offset);
  }
  if (!geo.indices.empty()) {
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)geo.indices.size(),
                 geo.indices.data(), GL_STATIC_DRAW);
  }
  gl_check("geometry setup");

  // ===== Uniform blocks =====
  for (size_t i = 0; i < a.ubos.size(); ++i) {
    const Ubo &u = a.ubos[i];
    if (u.gl_name.empty()) continue;
    GLuint index = glGetUniformBlockIndex(prog, u.gl_name.c_str());
    if (index == GL_INVALID_INDEX) continue;
    std::vector<char> data = read_file(u.path);
    GLuint buf = 0;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_UNIFORM_BUFFER, buf);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)data.size(), data.data(), GL_STATIC_DRAW);
    glUniformBlockBinding(prog, index, (GLuint)i);
    glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)i, buf);
  }
  gl_check("uniform block setup");

  // ===== Render targets =====
  // Created before the input textures: binding them here would replace the
  // inputs on the active texture unit.
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  GLenum internal = a.format == TargetFormat::RGBA8     ? GL_RGBA8
                    : a.format == TargetFormat::RGBA16F ? GL_RGBA16F
                                                        : GL_RGBA32F;
  GLenum upload_type = a.format == TargetFormat::RGBA8 ? GL_UNSIGNED_BYTE : GL_FLOAT;
  std::vector<GLenum> draw_buffers(a.targets);
  for (uint32_t i = 0; i < a.targets; ++i) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, (GLsizei)a.width, (GLsizei)a.height,
                 0, GL_RGBA, upload_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, tex, 0);
    draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
  }
  if (a.depth) {
    GLuint rb = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, (GLsizei)a.width,
                          (GLsizei)a.height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
  }
  glDrawBuffers((GLsizei)a.targets, draw_buffers.data());
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    die("GL framebuffer incomplete");
  }
  gl_check("framebuffer setup");

  // ===== Textures =====
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (size_t i = 0; i < a.textures.size(); ++i) {
    const Texture &t = a.textures[i];
    if (t.gl_name.empty()) continue;
    GLint loc = glGetUniformLocation(prog, t.gl_name.c_str());
    if (loc < 0) continue;
    std::vector<char> pixels = load_texture(t);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0 + (GLenum)i);
    glBindTexture(GL_TEXTURE_2D, tex);
    const uint32_t levels = mip_levels(t);
    size_t offset = 0;
    for (uint32_t level = 0; level < levels; ++level) {
      const GLint internal_format = t.format == TexFormat::RGBA8 ? (GLint)GL_RGBA8 : (GLint)GL_RGBA16F;
      const GLenum type = t.format == TexFormat::RGBA8 ? GL_UNSIGNED_BYTE : GL_HALF_FLOAT;
      glTexImage2D(GL_TEXTURE_2D, (GLint)level, internal_format,
                   (GLsizei)mip_size(t.width, level), (GLsizei)mip_size(t.height, level), 0,
                   GL_RGBA, type, pixels.data() + offset);
      offset += level_bytes(t, level);
    }
    GLint filter = t.filter == Filter::NEAREST ? (GLint)GL_NEAREST : (GLint)GL_LINEAR;
    GLint min_filter = t.filter == Filter::TRILINEAR ? (GLint)GL_LINEAR_MIPMAP_LINEAR : filter;
    GLint wrap = t.wrap == Wrap::CLAMP    ? (GLint)GL_CLAMP_TO_EDGE
                 : t.wrap == Wrap::REPEAT ? (GLint)GL_REPEAT
                                          : (GLint)GL_MIRRORED_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    glUniform1i(loc, (GLint)i);
  }
  gl_check("texture setup");

  glViewport(0, 0, (GLsizei)a.width, (GLsizei)a.height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DITHER);
  if (a.depth) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
  } else {
    glDisable(GL_DEPTH_TEST);
  }

  const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const GLfloat one = 1.0f;
  auto draw_frame = [&]() {
    for (uint32_t i = 0; i < a.targets; ++i) glClearBufferfv(GL_COLOR, (GLint)i, zero);
    if (a.depth) glClearBufferfv(GL_DEPTH, 0, &one);
    if (!geo.indices.empty()) {
      glDrawElements(GL_TRIANGLES, (GLsizei)geo.draw_count, GL_UNSIGNED_INT, nullptr);
    } else {
      glDrawArrays(GL_TRIANGLES, 0, (GLsizei)geo.draw_count);
    }
  };

  // Compile the program on the driver side before the clock starts.
  draw_frame();
  glFinish();
  gl_check("first draw");

  // A small ring of queries: reading frame N-RING's result before reusing
  // its query keeps at most RING frames in flight.
  constexpr uint32_t RING = 3;
  GLuint queries[RING];
  bool query_timed[RING] = {};
  bool query_pending[RING] = {};
  glGenQueries(RING, queries);

  FramePacer pacer(a);
  auto collect = [&](uint32_t slot) {
    if (!query_pending[slot]) return;
    GLuint64 ns = 0;
    glGetQueryObjectui64v(queries[slot], GL_QUERY_RESULT, &ns);
    query_pending[slot] = false;
    if (query_timed[slot]) r.samples_ms.push_back((double)ns / 1.0e6);
  };

  for (uint32_t frame = 0; !pacer.finished(r.samples_ms.size()); ++frame) {
    uint32_t slot = frame % RING;
    collect(slot);
    if (pacer.finished(r.samples_ms.size())) break;
    glBeginQuery(GL_TIME_ELAPSED, queries[slot]);
    draw_frame();
    glEndQuery(GL_TIME_ELAPSED);
    glFlush();
    query_pending[slot] = true;
    query_timed[slot] = pacer.submit_frame();
  }
  for (uint32_t slot = 0; slot < RING; ++slot) collect(slot);
  gl_check("timed frames");
  r.warmup_frames = pacer.warmup_frames;
  if (r.samples_ms.size() > a.frames) r.samples_ms.resize(a.frames);

  if (!a.readback_path.empty()) {
    size_t floats = (size_t)a.width * a.height * 4;
    std::vector<float> out(floats * a.targets);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (uint32_t i = 0; i < a.targets; ++i) {
      glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
      glReadPixels(0, 0, (GLsizei)a.width, (GLsizei)a.height, GL_RGBA, GL_FLOAT,
                   out.data() + floats * i);
    }
    gl_check("readback");
    write_file(a.readback_path, out.data(), out.size() * sizeof(float));
  }

  print_result(r);
  return 0;
}

#else

int run_gl(const Args &) { die("OpenGL is not supported on this platform"); }

#endif // BENCH_HAVE_GL

// ============================================================================
// Vulkan
// ============================================================================

struct VkCtx {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = UINT32_MAX;
  uint32_t timestamp_bits = 0;
  float timestamp_period = 1.0f;
};

uint32_t find_mem_type(const VkCtx &c, uint32_t type_bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(c.phys, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  die("no suitable Vulkan memory type");
}

struct VkBuf {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
};

VkBuf make_buffer(const VkCtx &c, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags flags) {
  VkBuf b;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_CHECK(vkCreateBuffer(c.dev, &bi, nullptr, &b.buffer));
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(c.dev, b.buffer, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = find_mem_type(c, req.memoryTypeBits, flags);
  VK_CHECK(vkAllocateMemory(c.dev, &ai, nullptr, &b.memory));
  VK_CHECK(vkBindBufferMemory(c.dev, b.buffer, b.memory, 0));
  if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    VK_CHECK(vkMapMemory(c.dev, b.memory, 0, size, 0, &b.mapped));
  }
  return b;
}

VkBuf make_host_buffer(const VkCtx &c, VkDeviceSize size, VkBufferUsageFlags usage) {
  return make_buffer(c, size, usage,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

struct VkImg {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

VkImg make_image(const VkCtx &c, uint32_t w, uint32_t h, VkFormat format,
                 VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t levels = 1) {
  VkImg img;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {w, h, 1};
  ici.mipLevels = levels;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(c.dev, &ici, nullptr, &img.image));
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(c.dev, img.image, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = find_mem_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(c.dev, &ai, nullptr, &img.memory));
  VK_CHECK(vkBindImageMemory(c.dev, img.image, img.memory, 0));
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = img.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = format;
  vci.subresourceRange.aspectMask = aspect;
  vci.subresourceRange.levelCount = levels;
  vci.subresourceRange.layerCount = 1;
  VK_CHECK(vkCreateImageView(c.dev, &vci, nullptr, &img.view));
  return img;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                   uint32_t levels = 1) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = levels;
  b.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkCtx make_vk_context(const Args &a, Result &r) {
  VkCtx c;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "bwsl_bench_runner";
  app.apiVersion = VK_API_VERSION_1_2;
  std::vector<const char *> inst_exts;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#ifdef __APPLE__
  inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
  ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = (uint32_t)inst_exts.size();
  ici.ppEnabledExtensionNames = inst_exts.data();
  VK_CHECK(vkCreateInstance(&ici, nullptr, &c.instance));

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(c.instance, &count, nullptr);
  if (count == 0) die("no Vulkan devices");
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(c.instance, &count, devices.data());

  // Prefer a discrete GPU unless --vk-device picks one.
  int chosen = -1;
  if (a.vk_device >= 0) {
    if ((uint32_t)a.vk_device >= count) die("--vk-device out of range");
    chosen = a.vk_device;
  } else {
    for (uint32_t i = 0; i < count && chosen < 0; ++i) {
      VkPhysicalDeviceProperties p{};
      vkGetPhysicalDeviceProperties(devices[i], &p);
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) chosen = (int)i;
    }
    if (chosen < 0) chosen = 0;
  }
  c.phys = devices[chosen];

  uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qf_count, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qf_count, qfs.data());
  for (uint32_t i = 0; i < qf_count; ++i) {
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      c.queue_family = i;
      c.timestamp_bits = qfs[i].timestampValidBits;
      break;
    }
  }
  if (c.queue_family == UINT32_MAX) die("Vulkan device has no graphics queue");
  if (c.timestamp_bits == 0) die("Vulkan graphics queue doesn't support timestamps");

  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props2.pNext = &driver;
  vkGetPhysicalDeviceProperties2(c.phys, &props2);
  c.timestamp_period = props2.properties.limits.timestampPeriod;
  r.device = props2.properties.deviceName;
  r.driver = std::string(driver.driverName) + " " + driver.driverInfo;

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = c.queue_family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  std::vector<const char *> dev_exts;
#ifdef __APPLE__
  dev_exts.push_back("VK_KHR_portability_subset");
#endif
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = (uint32_t)dev_exts.size();
  dci.ppEnabledExtensionNames = dev_exts.data();
  VK_CHECK(vkCreateDevice(c.phys, &dci, nullptr, &c.dev));
  vkGetDeviceQueue(c.dev, c.queue_family, 0, &c.queue);
  return c;
}

VkShaderModule make_shader(const VkCtx &c, const std::string &path) {
  std::vector<char> code = read_file(path);
  if (code.size() % 4 != 0) die(path + " is not SPIR-V");
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t *>(code.data());
  VkShaderModule m;
  VK_CHECK(vkCreateShaderModule(c.dev, &ci, nullptr, &m));
  return m;
}

int run_vulkan(const Args &a) {
  Result r;
  r.api = "vulkan";
  VkCtx c = make_vk_context(a, r);

  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = c.queue_family;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool;
  VK_CHECK(vkCreateCommandPool(c.dev, &cpi, nullptr, &pool));
  auto alloc_cmd = [&]() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(c.dev, &ai, &cmd));
    return cmd;
  };
  auto submit_and_wait = [&](VkCommandBuffer cmd) {
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(c.queue));
  };

  // ===== Geometry (device-local, uploaded once) =====
  Geometry geo = load_geometry(a);
  VkCommandBuffer setup = alloc_cmd();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(setup, &begin));
  std::vector<VkBuf> staging;
  auto upload_buffer = [&](const std::vector<char> &data, VkBufferUsageFlags usage) {
    VkBuf s = make_host_buffer(c, data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(s.mapped, data.data(), data.size());
    staging.push_back(s);
    VkBuf b = make_buffer(c, data.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkBufferCopy region{0, 0, data.size()};
    vkCmdCopyBuffer(setup, s.buffer, b.buffer, 1, &region);
    return b;
  };
  VkBuf vbo = upload_buffer(geo.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  VkBuf ibo{};
  if (!geo.indices.empty()) ibo = upload_buffer(geo.indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

  // ===== Descriptors =====
  struct Binding {
    uint32_t set;
    uint32_t binding;
    VkDescriptorType type;
    VkBuf buffer;
    VkImg image;
    VkSampler sampler = VK_NULL_HANDLE;
  };
  std::vector<Binding> bindings;
  for (const Ubo &u : a.ubos) {
    if (u.set < 0 || u.binding < 0) continue;
    std::vector<char> data = read_file(u.path);
    Binding b{};
    b.set = (uint32_t)u.set;
    b.binding = (uint32_t)u.binding;
    b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.buffer = upload_buffer(data, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    bindings.push_back(b);
  }
  for (const Ssbo &u : a.ssbos) {
    if (u.set < 0 || u.binding < 0) continue;
    std::vector<char> data = read_file(u.path);
    Binding b{};
    b.set = (uint32_t)u.set;
    b.binding = (uint32_t)u.binding;
    b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b.buffer = upload_buffer(data, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    bindings.push_back(b);
  }
  for (const Texture &t : a.textures) {
    if (t.set < 0 || t.binding < 0) continue;
    std::vector<char> pixels = load_texture(t);
    VkFormat format = t.format == TexFormat::RGBA8 ? VK_FORMAT_R8G8B8A8_UNORM
                                                   : VK_FORMAT_R16G16B16A16_SFLOAT;
    Binding b{};
    b.set = (uint32_t)t.set;
    b.binding = (uint32_t)t.binding;
    b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const uint32_t levels = mip_levels(t);
    b.image = make_image(c, t.width, t.height, format,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, levels);
    VkBuf s = make_host_buffer(c, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(s.mapped, pixels.data(), pixels.size());
    staging.push_back(s);
    image_barrier(setup, b.image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, levels);
    std::vector<VkBufferImageCopy> copies(levels);
    VkDeviceSize offset = 0;
    for (uint32_t level = 0; level < levels; ++level) {
      VkBufferImageCopy &copy = copies[level];
      copy = {};
      copy.bufferOffset = offset;
      copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.imageSubresource.mipLevel = level;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent = {mip_size(t.width, level), mip_size(t.height, level), 1};
      offset += level_bytes(t, level);
    }
    vkCmdCopyBufferToImage(setup, s.buffer, b.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, copies.data());
    image_barrier(setup, b.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  levels);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkFilter filter = t.filter == Filter::NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    VkSamplerAddressMode wrap = t.wrap == Wrap::CLAMP ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                : t.wrap == Wrap::REPEAT
                                    ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                    : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    sci.magFilter = filter;
    sci.minFilter = filter;
    sci.mipmapMode = t.filter == Filter::TRILINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                   : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.maxLod = (float)(levels - 1);
    sci.addressModeU = wrap;
    sci.addressModeV = wrap;
    sci.addressModeW = wrap;
    VK_CHECK(vkCreateSampler(c.dev, &sci, nullptr, &b.sampler));
    bindings.push_back(b);
  }
  VK_CHECK(vkEndCommandBuffer(setup));
  submit_and_wait(setup);

  uint32_t set_count = 0;
  for (const Binding &b : bindings) set_count = std::max(set_count, b.set + 1);
  std::vector<VkDescriptorSetLayout> layouts(set_count);
  for (uint32_t s = 0; s < set_count; ++s) {
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (const Binding &b : bindings) {
      if (b.set != s) continue;
      VkDescriptorSetLayoutBinding x{};
      x.binding = b.binding;
      x.descriptorType = b.type;
      x.descriptorCount = 1;
      x.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      lb.push_back(x);
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (uint32_t)lb.size();
    ci.pBindings = lb.data();
    VK_CHECK(vkCreateDescriptorSetLayout(c.dev, &ci, nullptr, &layouts[s]));
  }
  std::vector<VkDescriptorSet> sets(set_count);
  if (set_count > 0) {
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)a.ubos.size() + 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)a.ssbos.size() + 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)a.textures.size() + 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = set_count;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    VkDescriptorPool dpool;
    VK_CHECK(vkCreateDescriptorPool(c.dev, &dpci, nullptr, &dpool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = set_count;
    dsai.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(c.dev, &dsai, sets.data()));

    std::vector<VkDescriptorBufferInfo> buf_infos(bindings.size());
    std::vector<VkDescriptorImageInfo> img_infos(bindings.size());
    std::vector<VkWriteDescriptorSet> writes;
    for (size_t i = 0; i < bindings.size(); ++i) {
      const Binding &b = bindings[i];
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = sets[b.set];
      w.dstBinding = b.binding;
      w.descriptorCount = 1;
      w.descriptorType = b.type;
      if (b.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        buf_infos[i] = {b.buffer.buffer, 0, VK_WHOLE_SIZE};
        w.pBufferInfo = &buf_infos[i];
      } else {
        img_infos[i] = {b.sampler, b.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w.pImageInfo = &img_infos[i];
      }
      writes.push_back(w);
    }
    vkUpdateDescriptorSets(c.dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
  }

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = set_count;
  plci.pSetLayouts = layouts.data();
  VkPipelineLayout pipe_layout;
  VK_CHECK(vkCreatePipelineLayout(c.dev, &plci, nullptr, &pipe_layout));

  // ===== Render targets + render pass =====
  VkFormat color_format = a.format == TargetFormat::RGBA8     ? VK_FORMAT_R8G8B8A8_UNORM
                          : a.format == TargetFormat::RGBA16F ? VK_FORMAT_R16G16B16A16_SFLOAT
                                                              : VK_FORMAT_R32G32B32A32_SFLOAT;
  const VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
  std::vector<VkImg> targets;
  for (uint32_t i = 0; i < a.targets; ++i) {
    targets.push_back(make_image(c, a.width, a.height, color_format,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT));
  }
  VkImg depth{};
  if (a.depth) {
    depth = make_image(c, a.width, a.height, depth_format,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
  }

  std::vector<VkAttachmentDescription> atts;
  std::vector<VkAttachmentReference> color_refs;
  for (uint32_t i = 0; i < a.targets; ++i) {
    VkAttachmentDescription d{};
    d.format = color_format;
    d.samples = VK_SAMPLE_COUNT_1_BIT;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    d.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    d.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    atts.push_back(d);
    color_refs.push_back({i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
  }
  VkAttachmentReference depth_ref{a.targets, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  if (a.depth) {
    VkAttachmentDescription d{};
    d.format = depth_format;
    d.samples = VK_SAMPLE_COUNT_1_BIT;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // GL can't skip storing the depth buffer either; keep the work comparable.
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    d.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    d.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    atts.push_back(d);
  }
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = a.targets;
  sub.pColorAttachments = color_refs.data();
  sub.pDepthStencilAttachment = a.depth ? &depth_ref : nullptr;
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = (uint32_t)atts.size();
  rpci.pAttachments = atts.data();
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass render_pass;
  VK_CHECK(vkCreateRenderPass(c.dev, &rpci, nullptr, &render_pass));

  std::vector<VkImageView> views;
  for (const VkImg &t : targets) views.push_back(t.view);
  if (a.depth) views.push_back(depth.view);
  VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = render_pass;
  fbci.attachmentCount = (uint32_t)views.size();
  fbci.pAttachments = views.data();
  fbci.width = a.width;
  fbci.height = a.height;
  fbci.layers = 1;
  VkFramebuffer framebuffer;
  VK_CHECK(vkCreateFramebuffer(c.dev, &fbci, nullptr, &framebuffer));

  // ===== Pipeline =====
  VkShaderModule vs = make_shader(c, a.vert_path);
  VkShaderModule fs = make_shader(c, a.frag_path);
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";

  VkVertexInputBindingDescription vb{0, a.vbo_stride, VK_VERTEX_INPUT_RATE_VERTEX};
  std::vector<VkVertexInputAttributeDescription> vattrs;
  for (const Attribute &at : a.attributes) {
    if (at.vk_location < 0) continue;
    const VkFormat formats[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    vattrs.push_back({(uint32_t)at.vk_location, 0, formats[at.components - 1], at.offset});
  }
  const bool use_vbo = !vattrs.empty();
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = use_vbo ? 1 : 0;
  vi.pVertexBindingDescriptions = &vb;
  vi.vertexAttributeDescriptionCount = (uint32_t)vattrs.size();
  vi.pVertexAttributeDescriptions = vattrs.data();

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0.0f, 0.0f, (float)a.width, (float)a.height, 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {a.width, a.height}};
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.pViewports = &viewport;
  vp.scissorCount = 1;
  vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  std::vector<VkPipelineColorBlendAttachmentState> blend(a.targets);
  for (auto &b : blend) {
    b = {};
    b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = a.targets;
  cb.pAttachments = blend.data();
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = a.depth ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = a.depth ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = VK_COMPARE_OP_LESS;

  VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &cb;
  gpci.pDepthStencilState = &ds;
  gpci.layout = pipe_layout;
  gpci.renderPass = render_pass;
  VkPipeline pipeline;
  VK_CHECK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline));

  // ===== Per-frame command buffers =====
  // Two slots in flight, each with its own pair of timestamp queries. A full
  // barrier at the start of every frame keeps frames from overlapping, so
  // each timestamp pair covers exactly one frame's work.
  constexpr uint32_t SLOTS = 2;
  VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpci.queryCount = SLOTS * 2;
  VkQueryPool query_pool;
  VK_CHECK(vkCreateQueryPool(c.dev, &qpci, nullptr, &query_pool));

  std::vector<VkClearValue> clears(a.targets + (a.depth ? 1 : 0));
  for (uint32_t i = 0; i < a.targets; ++i) clears[i].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  if (a.depth) clears[a.targets].depthStencil = {1.0f, 0};

  VkCommandBuffer frame_cmds[SLOTS];
  VkFence fences[SLOTS];
  for (uint32_t s = 0; s < SLOTS; ++s) {
    frame_cmds[s] = alloc_cmd();
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(c.dev, &fci, nullptr, &fences[s]));

    VkCommandBuffer cmd = frame_cmds[s];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    vkCmdResetQueryPool(cmd, query_pool, s * 2, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, s * 2);
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = render_pass;
    rpbi.framebuffer = framebuffer;
    rpbi.renderArea = {{0, 0}, {a.width, a.height}};
    rpbi.clearValueCount = (uint32_t)clears.size();
    rpbi.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (set_count > 0) {
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0,
                              set_count, sets.data(), 0, nullptr);
    }
    VkDeviceSize offset = 0;
    if (use_vbo) vkCmdBindVertexBuffers(cmd, 0, 1, &vbo.buffer, &offset);
    if (!geo.indices.empty()) {
      vkCmdBindIndexBuffer(cmd, ibo.buffer, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cmd, geo.draw_count, 1, 0, 0, 0);
    } else {
      vkCmdDraw(cmd, geo.draw_count, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, s * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(cmd));
  }

  bool pending[SLOTS] = {};
  bool timed[SLOTS] = {};
  const uint64_t ts_mask =
      c.timestamp_bits >= 64 ? ~0ull : ((1ull << c.timestamp_bits) - 1ull);
  auto collect = [&](uint32_t s) {
    if (!pending[s]) return;
    VK_CHECK(vkWaitForFences(c.dev, 1, &fences[s], VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(c.dev, 1, &fences[s]));
    pending[s] = false;
    if (!timed[s]) return;
    uint64_t ts[2] = {};
    VK_CHECK(vkGetQueryPoolResults(c.dev, query_pool, s * 2, 2, sizeof(ts), ts,
                                   sizeof(uint64_t),
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    uint64_t ticks = ((ts[1] & ts_mask) - (ts[0] & ts_mask)) & ts_mask;
    r.samples_ms.push_back((double)ticks * c.timestamp_period / 1.0e6);
  };

  FramePacer pacer(a);
  for (uint32_t frame = 0; !pacer.finished(r.samples_ms.size()); ++frame) {
    uint32_t s = frame % SLOTS;
    collect(s);
    if (pacer.finished(r.samples_ms.size())) break;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &frame_cmds[s];
    VK_CHECK(vkQueueSubmit(c.queue, 1, &si, fences[s]));
    pending[s] = true;
    timed[s] = pacer.submit_frame();
  }
  for (uint32_t s = 0; s < SLOTS; ++s) collect(s);
  r.warmup_frames = pacer.warmup_frames;
  if (r.samples_ms.size() > a.frames) r.samples_ms.resize(a.frames);

  // ===== Readback of the last frame =====
  if (!a.readback_path.empty()) {
    const VkDeviceSize texel = a.format == TargetFormat::RGBA8     ? 4
                               : a.format == TargetFormat::RGBA16F ? 8
                                                                   : 16;
    const VkDeviceSize per_target = (VkDeviceSize)a.width * a.height * texel;
    VkBuf rb = make_host_buffer(c, per_target * a.targets, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer cmd = alloc_cmd();
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    for (uint32_t i = 0; i < a.targets; ++i) {
      image_barrier(cmd, targets[i].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy region{};
      region.bufferOffset = per_target * i;
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = {a.width, a.height, 1};
      vkCmdCopyImageToBuffer(cmd, targets[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             rb.buffer, 1, &region);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &mb, 0, nullptr, 0, nullptr);
    VK_CHECK(vkEndCommandBuffer(cmd));
    submit_and_wait(cmd);

    size_t values = (size_t)a.width * a.height * 4 * a.targets;
    std::vector<float> out(values);
    if (a.format == TargetFormat::RGBA8) {
      const uint8_t *src = static_cast<const uint8_t *>(rb.mapped);
      for (size_t i = 0; i < values; ++i) out[i] = (float)src[i] / 255.0f;
    } else if (a.format == TargetFormat::RGBA16F) {
      const uint16_t *src = static_cast<const uint16_t *>(rb.mapped);
      for (size_t i = 0; i < values; ++i) out[i] = half_to_float(src[i]);
    } else {
      std::memcpy(out.data(), rb.mapped, values * sizeof(float));
    }
    write_file(a.readback_path, out.data(), values * sizeof(float));
  }

  print_result(r);
  // The process exits right away; let the driver reclaim everything.
  vkDeviceWaitIdle(c.dev);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Args args = parse_args(argc, argv);
  if (args.api == "gl") return run_gl(args);
  return run_vulkan(args);
}

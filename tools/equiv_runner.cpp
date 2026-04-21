// Minimal Vulkan runner for BWSL equivalence testing.
//
// COMPUTE MODE (default):
//   Loads SPIR-V compute shaders, binds one optional input storage buffer
//   and one output storage buffer (descriptor set 1, bindings configurable),
//   dispatches the given workgroup count, and writes the output buffer.
//
//   equiv_runner --spirv file.spv --output out.bin --output-size BYTES \
//                --groups X Y Z [--input in.bin --input-binding N]
//                [--output-binding N] [--set N]
//
// RASTER MODE (--raster):
//   Loads a vertex + fragment SPIR-V pair, creates a RGBA32F color attachment
//   of the given size, draws a fullscreen triangle (3 vertices, no vertex
//   buffer - expects the vertex shader to synthesize positions from
//   gl_VertexIndex / vertex_id), then copies the rendered image to host and
//   writes it to --output. Output size must equal width*height*16 bytes.
//
//   equiv_runner --raster --vert-spirv v.spv --frag-spirv f.spv \
//                --width W --height H --output out.bin --output-size BYTES \
//                [--raster-ssbo FILE BINDING]... \
//                [--raster-ubo FILE BINDING]... \
//                [--set N]
//
//   Each --raster-ssbo / --raster-ubo pair uploads FILE into a storage or
//   uniform buffer at the given descriptor binding, visible to both the
//   vertex and fragment stages.
//
// Exit codes: 0 on success, 1 on failure.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

#define VK_CHECK(expr)                                                         \
  do {                                                                         \
    VkResult _r = (expr);                                                      \
    if (_r != VK_SUCCESS) {                                                    \
      std::fprintf(stderr, "%s:%d: %s failed: VkResult=%d\n", __FILE__,        \
                   __LINE__, #expr, (int)_r);                                  \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

[[noreturn]] void die(const char *msg) {
  std::fprintf(stderr, "equiv_runner: %s\n", msg);
  std::exit(1);
}

std::vector<char> read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    std::string m = "cannot read " + path;
    die(m.c_str());
  }
  std::streamsize n = f.tellg();
  std::vector<char> data(static_cast<size_t>(n));
  f.seekg(0);
  f.read(data.data(), n);
  return data;
}

void write_file(const std::string &path, const void *bytes, size_t size) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    std::string m = "cannot write " + path;
    die(m.c_str());
  }
  f.write(static_cast<const char *>(bytes), static_cast<std::streamsize>(size));
}

uint32_t find_mem_type(VkPhysicalDevice phys, uint32_t type_bits,
                       VkMemoryPropertyFlags required) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(phys, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags & required) == required) {
      return i;
    }
  }
  die("no suitable memory type");
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  VkDeviceSize size = 0;
};

Buffer make_host_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                        VkBufferUsageFlags usage) {
  Buffer b{};
  b.size = size;

  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_CHECK(vkCreateBuffer(dev, &bi, nullptr, &b.buffer));

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev, b.buffer, &req);

  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = find_mem_type(phys, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &b.memory));
  VK_CHECK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
  VK_CHECK(vkMapMemory(dev, b.memory, 0, size, 0, &b.mapped));
  return b;
}

struct PassSpec {
  std::string spirv_path;
  uint32_t groups[3] = {1, 1, 1};
};

struct Args {
  std::string input_path;
  std::string output_path;
  size_t output_size = 0;
  uint32_t input_binding = 0;
  uint32_t output_binding = 1;
  uint32_t descriptor_set = 1;
  // Passes. Single-pass tests set exactly one entry via the --spirv /
  // --groups top-level flags; multi-pass tests use repeated --pass-spirv /
  // --pass-groups pairs, each pair describing one compute dispatch. All
  // passes share the same output (and optional input) buffer, with a
  // memory barrier inserted between consecutive dispatches.
  std::vector<PassSpec> passes;

  // Raster mode.
  bool raster = false;
  std::string vert_spirv_path;
  std::string frag_spirv_path;
  uint32_t width = 0;
  uint32_t height = 0;

  // Raster resource bindings. Each entry is a host-side file to upload into
  // a single storage/uniform buffer at the given binding slot. The same
  // descriptor set layout is replicated across sets 0..descriptor_set so
  // SPIRV-Cross-reemitted HLSL/GLSL (which defaults to set=0) and native
  // BWSL SPIR-V (set=1) can both find it.
  struct RasterResource {
    std::string path;
    uint32_t binding = 0;
    bool is_ubo = false;  // false => storage buffer
  };
  std::vector<RasterResource> raster_resources;
};

Args parse_args(int argc, char **argv) {
  Args a{};
  auto need = [&](int i) {
    if (i >= argc) die("missing value for flag");
  };
  std::string top_spirv;
  uint32_t top_groups[3] = {1, 1, 1};
  bool top_groups_set = false;
  for (int i = 1; i < argc; ++i) {
    std::string f = argv[i];
    if (f == "--spirv") {
      need(++i);
      top_spirv = argv[i];
    } else if (f == "--input") {
      need(++i);
      a.input_path = argv[i];
    } else if (f == "--output") {
      need(++i);
      a.output_path = argv[i];
    } else if (f == "--output-size") {
      need(++i);
      a.output_size = static_cast<size_t>(std::strtoull(argv[i], nullptr, 10));
    } else if (f == "--input-binding") {
      need(++i);
      a.input_binding = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
    } else if (f == "--output-binding") {
      need(++i);
      a.output_binding = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
    } else if (f == "--set") {
      need(++i);
      a.descriptor_set = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
    } else if (f == "--groups") {
      for (int k = 0; k < 3; ++k) {
        need(++i);
        top_groups[k] = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
      }
      top_groups_set = true;
    } else if (f == "--pass-spirv") {
      need(++i);
      PassSpec p{};
      p.spirv_path = argv[i];
      a.passes.push_back(p);
    } else if (f == "--pass-groups") {
      if (a.passes.empty()) die("--pass-groups before --pass-spirv");
      PassSpec &p = a.passes.back();
      for (int k = 0; k < 3; ++k) {
        need(++i);
        p.groups[k] = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
      }
    } else if (f == "--raster") {
      a.raster = true;
    } else if (f == "--vert-spirv") {
      need(++i);
      a.vert_spirv_path = argv[i];
    } else if (f == "--frag-spirv") {
      need(++i);
      a.frag_spirv_path = argv[i];
    } else if (f == "--width") {
      need(++i);
      a.width = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
    } else if (f == "--height") {
      need(++i);
      a.height = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
    } else if (f == "--raster-ssbo" || f == "--raster-ubo") {
      Args::RasterResource r{};
      r.is_ubo = (f == "--raster-ubo");
      need(++i);
      r.path = argv[i];
      need(++i);
      r.binding = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
      a.raster_resources.push_back(r);
    } else {
      std::string m = "unknown arg: " + f;
      die(m.c_str());
    }
  }
  if (a.raster) {
    if (a.vert_spirv_path.empty() || a.frag_spirv_path.empty()) {
      die("raster: --vert-spirv and --frag-spirv required");
    }
    if (a.width == 0 || a.height == 0) {
      die("raster: --width and --height required");
    }
    if (a.output_path.empty() || a.output_size == 0) {
      die("raster: --output, --output-size required");
    }
    if (a.output_size != (size_t)a.width * a.height * 16) {
      die("raster: --output-size must equal width*height*16 (RGBA32F)");
    }
    return a;
  }
  if (a.passes.empty()) {
    if (top_spirv.empty()) die("required: --spirv (or --pass-spirv)");
    PassSpec p{};
    p.spirv_path = top_spirv;
    if (top_groups_set) {
      p.groups[0] = top_groups[0];
      p.groups[1] = top_groups[1];
      p.groups[2] = top_groups[2];
    }
    a.passes.push_back(p);
  }
  if (a.output_path.empty() || a.output_size == 0) {
    die("required: --output, --output-size");
  }
  return a;
}

// ============================================================================
// Vulkan instance/device helpers shared between compute and raster paths.
// ============================================================================

struct VulkanContext {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = UINT32_MAX;
};

VulkanContext make_context(bool need_graphics) {
  VulkanContext c{};

  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "bwsl_equiv_runner";
  ai.apiVersion = VK_API_VERSION_1_2;

  std::vector<const char *> inst_exts;
#ifdef __APPLE__
  inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &ai;
  ici.enabledExtensionCount = (uint32_t)inst_exts.size();
  ici.ppEnabledExtensionNames = inst_exts.data();
#ifdef __APPLE__
  ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
  VK_CHECK(vkCreateInstance(&ici, nullptr, &c.instance));

  uint32_t phys_count = 0;
  vkEnumeratePhysicalDevices(c.instance, &phys_count, nullptr);
  if (phys_count == 0) die("no Vulkan devices");
  std::vector<VkPhysicalDevice> phys_devs(phys_count);
  vkEnumeratePhysicalDevices(c.instance, &phys_count, phys_devs.data());

  VkQueueFlags needed_flags = VK_QUEUE_COMPUTE_BIT;
  if (need_graphics) needed_flags |= VK_QUEUE_GRAPHICS_BIT;

  for (auto d : phys_devs) {
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(d, &qf_count, qfs.data());
    for (uint32_t i = 0; i < qf_count; ++i) {
      if ((qfs[i].queueFlags & needed_flags) == needed_flags) {
        c.phys = d;
        c.queue_family = i;
        break;
      }
    }
    if (c.phys) break;
  }
  if (!c.phys) die("no suitable device");

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

// ============================================================================
// Raster mode: fullscreen-triangle rasterization into an RGBA32F target.
// ============================================================================

int run_raster(const Args &args) {
  auto vert_data = read_file(args.vert_spirv_path);
  if (vert_data.size() % 4 != 0) die("vert SPIR-V size not a multiple of 4");
  auto frag_data = read_file(args.frag_spirv_path);
  if (frag_data.size() % 4 != 0) die("frag SPIR-V size not a multiple of 4");

  VulkanContext ctx = make_context(/*need_graphics=*/true);

  VkFormat color_fmt = VK_FORMAT_R32G32B32A32_SFLOAT;

  // ===== Color image + view =====
  VkImageCreateInfo img_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img_ci.imageType = VK_IMAGE_TYPE_2D;
  img_ci.format = color_fmt;
  img_ci.extent = {args.width, args.height, 1};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage color_img;
  VK_CHECK(vkCreateImage(ctx.dev, &img_ci, nullptr, &color_img));

  VkMemoryRequirements img_req{};
  vkGetImageMemoryRequirements(ctx.dev, color_img, &img_req);
  VkMemoryAllocateInfo img_ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  img_ai.allocationSize = img_req.size;
  img_ai.memoryTypeIndex = find_mem_type(ctx.phys, img_req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory color_mem;
  VK_CHECK(vkAllocateMemory(ctx.dev, &img_ai, nullptr, &color_mem));
  VK_CHECK(vkBindImageMemory(ctx.dev, color_img, color_mem, 0));

  VkImageViewCreateInfo iv_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  iv_ci.image = color_img;
  iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  iv_ci.format = color_fmt;
  iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  iv_ci.subresourceRange.levelCount = 1;
  iv_ci.subresourceRange.layerCount = 1;
  VkImageView color_view;
  VK_CHECK(vkCreateImageView(ctx.dev, &iv_ci, nullptr, &color_view));

  // ===== Readback buffer =====
  Buffer readback = make_host_buffer(
      ctx.dev, ctx.phys, args.output_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  std::memset(readback.mapped, 0, args.output_size);

  // ===== Render pass =====
  VkAttachmentDescription att{};
  att.format = color_fmt;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  VkAttachmentReference color_ref{};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color_ref;

  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass render_pass;
  VK_CHECK(vkCreateRenderPass(ctx.dev, &rpci, nullptr, &render_pass));

  // ===== Framebuffer =====
  VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = render_pass;
  fbci.attachmentCount = 1;
  fbci.pAttachments = &color_view;
  fbci.width = args.width;
  fbci.height = args.height;
  fbci.layers = 1;
  VkFramebuffer framebuffer;
  VK_CHECK(vkCreateFramebuffer(ctx.dev, &fbci, nullptr, &framebuffer));

  // ===== Shader modules =====
  VkShaderModuleCreateInfo vsm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  vsm.codeSize = vert_data.size();
  vsm.pCode = reinterpret_cast<const uint32_t *>(vert_data.data());
  VkShaderModule vs_mod;
  VK_CHECK(vkCreateShaderModule(ctx.dev, &vsm, nullptr, &vs_mod));

  VkShaderModuleCreateInfo fsm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  fsm.codeSize = frag_data.size();
  fsm.pCode = reinterpret_cast<const uint32_t *>(frag_data.data());
  VkShaderModule fs_mod;
  VK_CHECK(vkCreateShaderModule(ctx.dev, &fsm, nullptr, &fs_mod));

  // ===== Resource buffers (SSBOs + UBOs) =====
  struct ResourceBuffer {
    Buffer buf;
    uint32_t binding;
    VkDescriptorType type;
  };
  std::vector<ResourceBuffer> resources;
  resources.reserve(args.raster_resources.size());
  for (const auto &rr : args.raster_resources) {
    auto bytes = read_file(rr.path);
    VkBufferUsageFlags usage =
        rr.is_ubo ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                  : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    size_t sz = bytes.empty() ? 16 : bytes.size();
    Buffer b = make_host_buffer(ctx.dev, ctx.phys, sz, usage);
    std::memset(b.mapped, 0, sz);
    if (!bytes.empty()) {
      std::memcpy(b.mapped, bytes.data(), bytes.size());
    }
    resources.push_back({b, rr.binding,
                         rr.is_ubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                   : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER});
  }

  // ===== Descriptor set layout =====
  std::vector<VkDescriptorSetLayoutBinding> dsl_bindings;
  dsl_bindings.reserve(resources.size());
  for (const auto &rb : resources) {
    VkDescriptorSetLayoutBinding b{};
    b.binding = rb.binding;
    b.descriptorType = rb.type;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    dsl_bindings.push_back(b);
  }

  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  uint32_t num_sets = 0;
  if (!resources.empty()) {
    VkDescriptorSetLayoutCreateInfo dslci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = (uint32_t)dsl_bindings.size();
    dslci.pBindings = dsl_bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.dev, &dslci, nullptr, &dsl));
    // Mirror the compute path: bind at every slot 0..descriptor_set so both
    // set=0 (HLSL/GLSL re-emit default) and set=1 (native BWSL) shaders see
    // the same resources.
    num_sets = args.descriptor_set + 1;
  }

  // ===== Pipeline layout =====
  std::vector<VkDescriptorSetLayout> all_layouts(num_sets, dsl);
  VkPipelineLayoutCreateInfo plci{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = num_sets;
  plci.pSetLayouts = all_layouts.empty() ? nullptr : all_layouts.data();
  VkPipelineLayout pipe_layout;
  VK_CHECK(vkCreatePipelineLayout(ctx.dev, &plci, nullptr, &pipe_layout));

  // ===== Descriptor pool + sets =====
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> dsets;
  if (!resources.empty()) {
    // Sum descriptor counts by type for the pool sizing.
    uint32_t ssbo_count = 0;
    uint32_t ubo_count = 0;
    for (const auto &rb : resources) {
      if (rb.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ssbo_count++;
      else ubo_count++;
    }
    std::vector<VkDescriptorPoolSize> pool_sizes;
    if (ssbo_count > 0) {
      pool_sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            ssbo_count * num_sets});
    }
    if (ubo_count > 0) {
      pool_sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                            ubo_count * num_sets});
    }
    VkDescriptorPoolCreateInfo dpci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = num_sets;
    dpci.poolSizeCount = (uint32_t)pool_sizes.size();
    dpci.pPoolSizes = pool_sizes.data();
    VK_CHECK(vkCreateDescriptorPool(ctx.dev, &dpci, nullptr, &dpool));

    std::vector<VkDescriptorSetLayout> alloc_layouts(num_sets, dsl);
    VkDescriptorSetAllocateInfo dsai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = num_sets;
    dsai.pSetLayouts = alloc_layouts.data();
    dsets.resize(num_sets);
    VK_CHECK(vkAllocateDescriptorSets(ctx.dev, &dsai, dsets.data()));

    // Stable storage for VkDescriptorBufferInfo: one per (set, resource).
    std::vector<VkDescriptorBufferInfo> buf_infos(
        resources.size() * num_sets);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(resources.size() * num_sets);
    for (uint32_t s = 0; s < num_sets; ++s) {
      for (size_t r = 0; r < resources.size(); ++r) {
        size_t idx = s * resources.size() + r;
        buf_infos[idx] = {resources[r].buf.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = dsets[s];
        w.dstBinding = resources[r].binding;
        w.descriptorCount = 1;
        w.descriptorType = resources[r].type;
        w.pBufferInfo = &buf_infos[idx];
        writes.push_back(w);
      }
    }
    vkUpdateDescriptorSets(ctx.dev, (uint32_t)writes.size(), writes.data(), 0,
                           nullptr);
  }

  // ===== Graphics pipeline =====
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs_mod;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs_mod;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi_ci{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo ia_ci{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia_ci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkViewport vp{0.0f, 0.0f, (float)args.width, (float)args.height, 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {args.width, args.height}};
  VkPipelineViewportStateCreateInfo vp_ci{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp_ci.viewportCount = 1;
  vp_ci.pViewports = &vp;
  vp_ci.scissorCount = 1;
  vp_ci.pScissors = &sc;

  VkPipelineRasterizationStateCreateInfo rs_ci{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs_ci.polygonMode = VK_POLYGON_MODE_FILL;
  rs_ci.cullMode = VK_CULL_MODE_NONE;
  rs_ci.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs_ci.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms_ci{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms_ci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend_att{};
  blend_att.blendEnable = VK_FALSE;
  blend_att.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo cb_ci{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb_ci.attachmentCount = 1;
  cb_ci.pAttachments = &blend_att;

  VkGraphicsPipelineCreateInfo gpci{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi_ci;
  gpci.pInputAssemblyState = &ia_ci;
  gpci.pViewportState = &vp_ci;
  gpci.pRasterizationState = &rs_ci;
  gpci.pMultisampleState = &ms_ci;
  gpci.pColorBlendState = &cb_ci;
  gpci.layout = pipe_layout;
  gpci.renderPass = render_pass;
  gpci.subpass = 0;

  VkPipeline pipeline;
  VK_CHECK(vkCreateGraphicsPipelines(ctx.dev, VK_NULL_HANDLE, 1, &gpci, nullptr,
                                     &pipeline));

  // ===== Command buffer =====
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = ctx.queue_family;
  VkCommandPool cmdpool;
  VK_CHECK(vkCreateCommandPool(ctx.dev, &cpi, nullptr, &cmdpool));

  VkCommandBufferAllocateInfo cbai{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cmdpool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK_CHECK(vkAllocateCommandBuffers(ctx.dev, &cbai, &cmd));

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpbi.renderPass = render_pass;
  rpbi.framebuffer = framebuffer;
  rpbi.renderArea = {{0, 0}, {args.width, args.height}};
  rpbi.clearValueCount = 1;
  rpbi.pClearValues = &clear;

  vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  if (!dsets.empty()) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout,
                            0, (uint32_t)dsets.size(), dsets.data(), 0,
                            nullptr);
  }
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);

  // Image layout is now TRANSFER_SRC_OPTIMAL (via finalLayout). Copy to
  // readback buffer.
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {args.width, args.height, 1};
  vkCmdCopyImageToBuffer(cmd, color_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         readback.buffer, 1, &region);

  // Make the buffer write visible to host.
  VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bmb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bmb.buffer = readback.buffer;
  bmb.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bmb, 0,
                       nullptr);

  VK_CHECK(vkEndCommandBuffer(cmd));

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(ctx.queue));

  write_file(args.output_path, readback.mapped, args.output_size);

  // Cleanup.
  vkDestroyCommandPool(ctx.dev, cmdpool, nullptr);
  vkDestroyPipeline(ctx.dev, pipeline, nullptr);
  vkDestroyPipelineLayout(ctx.dev, pipe_layout, nullptr);
  vkDestroyShaderModule(ctx.dev, vs_mod, nullptr);
  vkDestroyShaderModule(ctx.dev, fs_mod, nullptr);
  vkDestroyFramebuffer(ctx.dev, framebuffer, nullptr);
  vkDestroyRenderPass(ctx.dev, render_pass, nullptr);
  vkDestroyImageView(ctx.dev, color_view, nullptr);
  vkDestroyImage(ctx.dev, color_img, nullptr);
  vkFreeMemory(ctx.dev, color_mem, nullptr);
  if (dpool != VK_NULL_HANDLE) vkDestroyDescriptorPool(ctx.dev, dpool, nullptr);
  if (dsl != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(ctx.dev, dsl, nullptr);
  for (auto &rb : resources) {
    vkUnmapMemory(ctx.dev, rb.buf.memory);
    vkDestroyBuffer(ctx.dev, rb.buf.buffer, nullptr);
    vkFreeMemory(ctx.dev, rb.buf.memory, nullptr);
  }
  vkUnmapMemory(ctx.dev, readback.memory);
  vkDestroyBuffer(ctx.dev, readback.buffer, nullptr);
  vkFreeMemory(ctx.dev, readback.memory, nullptr);
  vkDestroyDevice(ctx.dev, nullptr);
  vkDestroyInstance(ctx.instance, nullptr);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Args args = parse_args(argc, argv);

  if (args.raster) {
    return run_raster(args);
  }

  // Read every pass's SPIR-V up front so errors happen before we touch
  // Vulkan (simpler cleanup path).
  std::vector<std::vector<char>> pass_spirvs;
  pass_spirvs.reserve(args.passes.size());
  for (const auto &p : args.passes) {
    auto data = read_file(p.spirv_path);
    if (data.size() % 4 != 0) die("SPIR-V size not a multiple of 4");
    pass_spirvs.push_back(std::move(data));
  }

  std::vector<char> input_bytes;
  if (!args.input_path.empty()) {
    input_bytes = read_file(args.input_path);
  }

  // ===== Instance =====
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "bwsl_equiv_runner";
  ai.apiVersion = VK_API_VERSION_1_2;

  std::vector<const char *> inst_exts;
#ifdef __APPLE__
  inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &ai;
  ici.enabledExtensionCount = (uint32_t)inst_exts.size();
  ici.ppEnabledExtensionNames = inst_exts.data();
#ifdef __APPLE__
  ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

  VkInstance instance;
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

  // ===== Physical device with compute queue =====
  uint32_t phys_count = 0;
  vkEnumeratePhysicalDevices(instance, &phys_count, nullptr);
  if (phys_count == 0) die("no Vulkan devices");
  std::vector<VkPhysicalDevice> phys_devs(phys_count);
  vkEnumeratePhysicalDevices(instance, &phys_count, phys_devs.data());

  VkPhysicalDevice phys = VK_NULL_HANDLE;
  uint32_t queue_family = UINT32_MAX;
  for (auto d : phys_devs) {
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(d, &qf_count, qfs.data());
    for (uint32_t i = 0; i < qf_count; ++i) {
      if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        phys = d;
        queue_family = i;
        break;
      }
    }
    if (phys) break;
  }
  if (!phys) die("no compute-capable device");

  // ===== Logical device =====
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queue_family;
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

  VkDevice dev;
  VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
  VkQueue queue;
  vkGetDeviceQueue(dev, queue_family, 0, &queue);

  // ===== Buffers =====
  Buffer out_buf = make_host_buffer(
      dev, phys, args.output_size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  std::memset(out_buf.mapped, 0, args.output_size);

  std::optional<Buffer> in_buf;
  if (!input_bytes.empty()) {
    in_buf = make_host_buffer(
        dev, phys, input_bytes.size(),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(in_buf->mapped, input_bytes.data(), input_bytes.size());
  }

  // ===== Descriptor set layout =====
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  VkDescriptorSetLayoutBinding outb{};
  outb.binding = args.output_binding;
  outb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  outb.descriptorCount = 1;
  outb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings.push_back(outb);
  if (in_buf) {
    VkDescriptorSetLayoutBinding inb{};
    inb.binding = args.input_binding;
    inb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    inb.descriptorCount = 1;
    inb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings.push_back(inb);
  }

  VkDescriptorSetLayoutCreateInfo dslci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = (uint32_t)bindings.size();
  dslci.pBindings = bindings.data();
  VkDescriptorSetLayout dsl;
  VK_CHECK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

  // BWSL emits storage buffers at descriptor set 1, but tools that re-compile
  // our cross-compiled GLSL/HLSL (glslang, dxc) default to set 0 because
  // SPIRV-Cross doesn't emit "set = N" qualifiers. To run each variant
  // without special-casing, bind the same descriptor set at BOTH slot 0 and
  // slot 1 (or up through args.descriptor_set).
  uint32_t max_set = args.descriptor_set;
  std::vector<VkDescriptorSetLayout> all_layouts(max_set + 1, dsl);

  // ===== Pipeline layout =====
  VkPipelineLayoutCreateInfo plci{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = (uint32_t)all_layouts.size();
  plci.pSetLayouts = all_layouts.data();
  VkPipelineLayout pl;
  VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));

  // ===== Shader modules + compute pipelines (one per pass) =====
  std::vector<VkShaderModule> shaders(pass_spirvs.size(), VK_NULL_HANDLE);
  std::vector<VkPipeline> pipelines(pass_spirvs.size(), VK_NULL_HANDLE);
  for (size_t p = 0; p < pass_spirvs.size(); ++p) {
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = pass_spirvs[p].size();
    smci.pCode = reinterpret_cast<const uint32_t *>(pass_spirvs[p].data());
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &shaders[p]));

    VkComputePipelineCreateInfo cpci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shaders[p];
    cpci.stage.pName = "main";
    cpci.layout = pl;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                      &pipelines[p]));
  }

  // ===== Descriptor pool + sets (one per bound slot) =====
  uint32_t num_sets = max_set + 1;
  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 (uint32_t)bindings.size() * num_sets};
  VkDescriptorPoolCreateInfo dpci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = num_sets;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &pool_size;
  VkDescriptorPool dpool;
  VK_CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));

  std::vector<VkDescriptorSetLayout> alloc_layouts(num_sets, dsl);
  VkDescriptorSetAllocateInfo dsai{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = dpool;
  dsai.descriptorSetCount = num_sets;
  dsai.pSetLayouts = alloc_layouts.data();
  std::vector<VkDescriptorSet> dsets(num_sets);
  VK_CHECK(vkAllocateDescriptorSets(dev, &dsai, dsets.data()));

  VkDescriptorBufferInfo out_info{out_buf.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo in_info{};
  if (in_buf) in_info = {in_buf->buffer, 0, VK_WHOLE_SIZE};

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(num_sets * 2);
  for (uint32_t s = 0; s < num_sets; ++s) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = dsets[s];
    w.dstBinding = args.output_binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &out_info;
    writes.push_back(w);
    if (in_buf) {
      VkWriteDescriptorSet wi{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wi.dstSet = dsets[s];
      wi.dstBinding = args.input_binding;
      wi.descriptorCount = 1;
      wi.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      wi.pBufferInfo = &in_info;
      writes.push_back(wi);
    }
  }
  vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0,
                         nullptr);

  // ===== Record + submit =====
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = queue_family;
  VkCommandPool cmdpool;
  VK_CHECK(vkCreateCommandPool(dev, &cpi, nullptr, &cmdpool));

  VkCommandBufferAllocateInfo cbi{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbi.commandPool = cmdpool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cbi, &cmd));

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0,
                          num_sets, dsets.data(), 0, nullptr);
  for (size_t p = 0; p < pipelines.size(); ++p) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[p]);
    vkCmdDispatch(cmd, args.passes[p].groups[0], args.passes[p].groups[1],
                  args.passes[p].groups[2]);
    // Barrier between passes: previous dispatch's SSBO writes must be
    // visible to the next dispatch's SSBO reads.
    if (p + 1 < pipelines.size()) {
      VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      mb.dstAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    }
  }

  VK_CHECK(vkEndCommandBuffer(cmd));

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(queue));

  // ===== Write output =====
  write_file(args.output_path, out_buf.mapped, args.output_size);

  // ===== Cleanup =====
  vkDestroyCommandPool(dev, cmdpool, nullptr);
  vkDestroyDescriptorPool(dev, dpool, nullptr);
  for (auto pipe : pipelines) vkDestroyPipeline(dev, pipe, nullptr);
  for (auto mod : shaders) vkDestroyShaderModule(dev, mod, nullptr);
  vkDestroyPipelineLayout(dev, pl, nullptr);
  vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
  if (in_buf) {
    vkUnmapMemory(dev, in_buf->memory);
    vkDestroyBuffer(dev, in_buf->buffer, nullptr);
    vkFreeMemory(dev, in_buf->memory, nullptr);
  }
  vkUnmapMemory(dev, out_buf.memory);
  vkDestroyBuffer(dev, out_buf.buffer, nullptr);
  vkFreeMemory(dev, out_buf.memory, nullptr);
  vkDestroyDevice(dev, nullptr);
  vkDestroyInstance(instance, nullptr);
  return 0;
}

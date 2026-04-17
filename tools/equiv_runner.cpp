// Minimal Vulkan compute runner for BWSL equivalence testing.
//
// Loads a SPIR-V compute shader, binds one optional input storage buffer
// and one output storage buffer (descriptor set 1, bindings configurable),
// dispatches the given workgroup count, and writes the output buffer to a file.
//
// Usage:
//   equiv_runner --spirv file.spv --output out.bin --output-size BYTES \
//                --groups X Y Z [--input in.bin --input-binding N]
//                [--output-binding N] [--set N]
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

struct Args {
  std::string spirv_path;
  std::string input_path;
  std::string output_path;
  size_t output_size = 0;
  uint32_t input_binding = 0;
  uint32_t output_binding = 1;
  uint32_t descriptor_set = 1;
  uint32_t groups[3] = {1, 1, 1};
};

Args parse_args(int argc, char **argv) {
  Args a{};
  auto need = [&](int i) {
    if (i >= argc) die("missing value for flag");
  };
  for (int i = 1; i < argc; ++i) {
    std::string f = argv[i];
    if (f == "--spirv") {
      need(++i);
      a.spirv_path = argv[i];
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
        a.groups[k] = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
      }
    } else {
      std::string m = "unknown arg: " + f;
      die(m.c_str());
    }
  }
  if (a.spirv_path.empty() || a.output_path.empty() || a.output_size == 0) {
    die("required: --spirv, --output, --output-size");
  }
  return a;
}

} // namespace

int main(int argc, char **argv) {
  Args args = parse_args(argc, argv);

  std::vector<char> spirv = read_file(args.spirv_path);
  if (spirv.size() % 4 != 0) die("SPIR-V size not a multiple of 4");

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

  // ===== Shader module + compute pipeline =====
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = spirv.size();
  smci.pCode = reinterpret_cast<const uint32_t *>(spirv.data());
  VkShaderModule shader;
  VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &shader));

  VkComputePipelineCreateInfo cpci{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = shader;
  cpci.stage.pName = "main";
  cpci.layout = pl;
  VkPipeline pipeline;
  VK_CHECK(
      vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));

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

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0,
                          num_sets, dsets.data(), 0, nullptr);
  vkCmdDispatch(cmd, args.groups[0], args.groups[1], args.groups[2]);

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
  vkDestroyPipeline(dev, pipeline, nullptr);
  vkDestroyShaderModule(dev, shader, nullptr);
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

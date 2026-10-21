// GpuChecker の実体。Windows 専用であるため、非 Windows では説明文だけにして
// ビルドを通す（Linux の輸送実装は将来の差し替え対象）。
#ifdef _WIN32

#include "transfer/gpu_check.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "check_comp_spv.h"
#include "common/log.h"

namespace xrift::transfer {
namespace {

// shared_texture 側と同粒度にする（既存 TU と重複する小関数は局所化する方針に従う）。
// 例外 (vk::SystemError) を投げる raii 呼び出しを従来の CheckVk と同じ粒度に揃える。
template <typename F>
void InvokeVk(const char* what, F&& f) {
  try {
    f();
  } catch (const vk::SystemError& e) {
    const vk::Result result = static_cast<vk::Result>(e.code().value());
    throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) +
                             ")");
  }
}

vk::ImageSubresourceRange FullColorRange() {
  vk::ImageSubresourceRange range{};
  range.aspectMask = vk::ImageAspectFlagBits::eColor;
  range.baseMipLevel = 0;
  range.levelCount = 1;
  range.baseArrayLayer = 0;
  range.layerCount = 1;
  return range;
}

// 検査に必要な最小寸法。隅ブロック (64x64) と番号ブロック ((64,64) から 32x32) が
// 重ならずに収まる大きさである。これ未満は黙って通さず失敗にする。
constexpr uint32_t kMinDimension = 128;

// 1 スレッド = 1 タイル (16x16)。ワークグループは 16x16 スレッドである。
constexpr uint32_t kTile = 16;
constexpr uint32_t kLocalX = 16;
constexpr uint32_t kLocalY = 16;

}  // namespace

// Report の配置がシェーダの CheckResult と一致することの担保。
// 不一致があると結果の読みがずれるため、ここで止める。
static_assert(sizeof(GpuChecker::Report) == GpuChecker::kResultSize,
              "GPU検査の結果構造体はシェーダと同配置であること");
static_assert(sizeof(GpuChecker::Record) == 20, "GPU検査の記録は 5 ワードであること");

GpuChecker::GpuChecker(const gfx::VulkanContext& vulkan)
    : device_(vulkan.device()), queue_(vulkan.queue()), queueFamily_(vulkan.queueFamilyIndex()) {
  const vk::raii::Device& device = device_;

  vk::SamplerCreateInfo sampler_info{};
  sampler_info.sType = vk::StructureType::eSamplerCreateInfo;
  sampler_info.magFilter = vk::Filter::eNearest;
  sampler_info.minFilter = vk::Filter::eNearest;
  sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
  sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
  sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
  sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  InvokeVk("vkCreateSampler(GPU検査)", [&] { sampler_ = vk::raii::Sampler(device, sampler_info); });

  std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
  bindings[1].binding = 1;
  bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
  vk::DescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = vk::StructureType::eDescriptorSetLayoutCreateInfo;
  layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
  layout_info.pBindings = bindings.data();
  InvokeVk("vkCreateDescriptorSetLayout(GPU検査)",
           [&] { setLayout_ = vk::raii::DescriptorSetLayout(device, layout_info); });

  vk::PushConstantRange push{};
  push.stageFlags = vk::ShaderStageFlagBits::eCompute;
  push.offset = 0;
  push.size = 8;  // width + height。
  const vk::DescriptorSetLayout raw_layout = *setLayout_;
  vk::PipelineLayoutCreateInfo pipeline_layout_info{};
  pipeline_layout_info.sType = vk::StructureType::ePipelineLayoutCreateInfo;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &raw_layout;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push;
  InvokeVk("vkCreatePipelineLayout(GPU検査)",
           [&] { pipelineLayout_ = vk::raii::PipelineLayout(device, pipeline_layout_info); });

  vk::ShaderModuleCreateInfo module_info{};
  module_info.sType = vk::StructureType::eShaderModuleCreateInfo;
  module_info.codeSize = sizeof(kCheckCompSpv);
  module_info.pCode = kCheckCompSpv;
  vk::raii::ShaderModule module{nullptr};
  InvokeVk("vkCreateShaderModule(GPU検査)",
           [&] { module = vk::raii::ShaderModule(device, module_info); });
  vk::PipelineShaderStageCreateInfo stage{};
  stage.sType = vk::StructureType::ePipelineShaderStageCreateInfo;
  stage.stage = vk::ShaderStageFlagBits::eCompute;
  stage.module = *module;
  stage.pName = "main";
  vk::ComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = vk::StructureType::eComputePipelineCreateInfo;
  pipeline_info.stage = stage;
  pipeline_info.layout = *pipelineLayout_;
  vk::PipelineCacheCreateInfo cache_info{};
  cache_info.sType = vk::StructureType::ePipelineCacheCreateInfo;
  vk::raii::PipelineCache cache{nullptr};
  InvokeVk("vkCreatePipelineCache(GPU検査)",
           [&] { cache = vk::raii::PipelineCache(device, cache_info); });
  InvokeVk("vkCreateComputePipelines(GPU検査)",
           [&] { pipeline_ = vk::raii::Pipeline(device, cache, pipeline_info); });

  std::array<vk::DescriptorPoolSize, 2> pool_sizes{};
  pool_sizes[0].type = vk::DescriptorType::eCombinedImageSampler;
  pool_sizes[0].descriptorCount = 1;
  pool_sizes[1].type = vk::DescriptorType::eStorageBuffer;
  pool_sizes[1].descriptorCount = 1;
  vk::DescriptorPoolCreateInfo pool_info{};
  pool_info.sType = vk::StructureType::eDescriptorPoolCreateInfo;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  InvokeVk("vkCreateDescriptorPool(GPU検査)",
           [&] { pool_ = vk::raii::DescriptorPool(device, pool_info); });
  vk::DescriptorSetAllocateInfo set_alloc{};
  set_alloc.sType = vk::StructureType::eDescriptorSetAllocateInfo;
  set_alloc.descriptorPool = *pool_;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &raw_layout;
  InvokeVk("vkAllocateDescriptorSets(GPU検査)", [&] {
    vk::raii::DescriptorSets sets = device.allocateDescriptorSets(set_alloc);
    set_ = std::move(sets[0]);
  });

  vk::BufferCreateInfo buffer_info{};
  buffer_info.sType = vk::StructureType::eBufferCreateInfo;
  buffer_info.size = kResultSize;
  buffer_info.usage = vk::BufferUsageFlagBits::eStorageBuffer;
  buffer_info.sharingMode = vk::SharingMode::eExclusive;
  InvokeVk("vkCreateBuffer(GPU検査結果)",
           [&] { resultBuffer_ = vk::raii::Buffer(device, buffer_info); });
  const vk::MemoryRequirements req = resultBuffer_.getMemoryRequirements();
  const vk::PhysicalDeviceMemoryProperties mem_props = vulkan.physicalDevice().getMemoryProperties();
  int32_t index = -1;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    const vk::MemoryPropertyFlags flags =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    if ((req.memoryTypeBits & (1u << i)) != 0u &&
        (mem_props.memoryTypes[i].propertyFlags & flags) == flags) {
      index = static_cast<int32_t>(i);
      break;
    }
  }
  if (index < 0) {
    throw std::runtime_error("GPU検査の結果バッファの host-visible メモリタイプが無い");
  }
  vk::MemoryAllocateInfo alloc{};
  alloc.sType = vk::StructureType::eMemoryAllocateInfo;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = static_cast<uint32_t>(index);
  InvokeVk("vkAllocateMemory(GPU検査結果)",
           [&] { resultMemory_ = vk::raii::DeviceMemory(device, alloc); });
  InvokeVk("vkBindBufferMemory(GPU検査結果)",
           [&] { resultBuffer_.bindMemory(*resultMemory_, 0); });
  InvokeVk("vkMapMemory(GPU検査結果)", [&] {
    mapped_ = resultMemory_.mapMemory(0, kResultSize);
  });

  vk::DescriptorBufferInfo result_info{};
  result_info.buffer = *resultBuffer_;
  result_info.offset = 0;
  result_info.range = kResultSize;
  vk::WriteDescriptorSet write_buffer{};
  write_buffer.sType = vk::StructureType::eWriteDescriptorSet;
  write_buffer.dstSet = *set_;
  write_buffer.dstBinding = 1;
  write_buffer.descriptorCount = 1;
  write_buffer.descriptorType = vk::DescriptorType::eStorageBuffer;
  write_buffer.pBufferInfo = &result_info;
  const std::array<vk::WriteDescriptorSet, 1> buffer_writes{write_buffer};
  device.updateDescriptorSets(buffer_writes, nullptr);

  vk::CommandPoolCreateInfo cmd_pool_info{};
  cmd_pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
  cmd_pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  cmd_pool_info.queueFamilyIndex = queueFamily_;
  InvokeVk("vkCreateCommandPool(GPU検査)",
           [&] { cmdPool_ = vk::raii::CommandPool(device, cmd_pool_info); });
  vk::CommandBufferAllocateInfo cmd_alloc{};
  cmd_alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
  cmd_alloc.commandPool = *cmdPool_;
  cmd_alloc.level = vk::CommandBufferLevel::ePrimary;
  cmd_alloc.commandBufferCount = 1;
  InvokeVk("vkAllocateCommandBuffers(GPU検査)", [&] {
    vk::raii::CommandBuffers buffers = device.allocateCommandBuffers(cmd_alloc);
    cmd_ = std::move(buffers[0]);
  });

  vk::FenceCreateInfo fence_info{};
  fence_info.sType = vk::StructureType::eFenceCreateInfo;
  InvokeVk("vkCreateFence(GPU検査)", [&] { fence_ = vk::raii::Fence(device, fence_info); });

  Info("GPU検査を初期化: 結果=%zuバイト 記録=%u件", kResultSize,
       static_cast<unsigned>(kMaxRecords));
}

GpuChecker::~GpuChecker() {
  // 永続マップを先に外す。以降は raii の逆順破棄に任せる。
  if (mapped_ != nullptr) {
    try {
      resultMemory_.unmapMemory();
    } catch (...) {
    }
    mapped_ = nullptr;
  }
}

void GpuChecker::BindImage(vk::Image image, uint32_t width, uint32_t height) {
  const VkImage raw = static_cast<VkImage>(image);
  if (hasBinding_ && raw == boundRaw_ && width == boundWidth_ && height == boundHeight_) {
    return;
  }
  const vk::raii::Device& device = device_;
  view_.clear();

  vk::ImageViewCreateInfo view_info{};
  view_info.sType = vk::StructureType::eImageViewCreateInfo;
  view_info.image = image;
  view_info.viewType = vk::ImageViewType::e2D;
  view_info.format = vk::Format::eB8G8R8A8Unorm;
  view_info.subresourceRange = FullColorRange();
  InvokeVk("vkCreateImageView(GPU検査)", [&] { view_ = vk::raii::ImageView(device, view_info); });

  vk::DescriptorImageInfo image_info{};
  image_info.sampler = *sampler_;
  image_info.imageView = *view_;
  image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  vk::WriteDescriptorSet write_image{};
  write_image.sType = vk::StructureType::eWriteDescriptorSet;
  write_image.dstSet = *set_;
  write_image.dstBinding = 0;
  write_image.descriptorCount = 1;
  write_image.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write_image.pImageInfo = &image_info;
  const std::array<vk::WriteDescriptorSet, 1> image_writes{write_image};
  device.updateDescriptorSets(image_writes, nullptr);

  boundRaw_ = raw;
  boundWidth_ = width;
  boundHeight_ = height;
  hasBinding_ = true;
}

GpuChecker::Report GpuChecker::Check(uint32_t width, uint32_t height) {
  if (!hasBinding_ || width != boundWidth_ || height != boundHeight_) {
    throw std::runtime_error("GPU検査の対象イメージが結び付いていない");
  }
  if (width < kMinDimension || height < kMinDimension) {
    throw std::runtime_error("GPU検査には 128x128 以上が必要");
  }
  const vk::raii::Device& device = device_;
  const vk::raii::Queue& queue = queue_;

  // 結果バッファはホスト側でゼロ埋めしてから提出する。
  // host-coherent のため flush は要らない。デバイス側の読みは submit 順で後になる。
  std::memset(mapped_, 0, kResultSize);

  InvokeVk("vkResetFences(GPU検査)", [&] { device.resetFences(*fence_); });
  InvokeVk("vkResetCommandBuffer(GPU検査)", [&] { cmd_.reset(); });
  vk::CommandBufferBeginInfo begin{};
  begin.sType = vk::StructureType::eCommandBufferBeginInfo;
  begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  InvokeVk("vkBeginCommandBuffer(GPU検査)", [&] { cmd_.begin(begin); });

  // 自前イメージは GENERAL で駐めてある。読み用レイアウトへ遷移する。
  // 直前の書き込みはコピーか検証のコピー（transfer）だけである。
  vk::ImageMemoryBarrier to_read{};
  to_read.sType = vk::StructureType::eImageMemoryBarrier;
  to_read.srcAccessMask =
      vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eTransferRead;
  to_read.dstAccessMask = vk::AccessFlagBits::eShaderRead;
  to_read.oldLayout = vk::ImageLayout::eGeneral;
  to_read.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_read.image = static_cast<vk::Image>(boundRaw_);
  to_read.subresourceRange = FullColorRange();
  cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                       vk::PipelineStageFlagBits::eComputeShader, {}, nullptr, nullptr, to_read);

  cmd_.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline_);
  cmd_.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout_, 0, *set_, nullptr);
  const std::array<uint32_t, 2> push{width, height};
  cmd_.pushConstants<uint32_t>(*pipelineLayout_, vk::ShaderStageFlagBits::eCompute, 0u, push);
  const uint32_t tiles_x = (width + kTile - 1u) / kTile;
  const uint32_t tiles_y = (height + kTile - 1u) / kTile;
  const uint32_t groups_x = (tiles_x + kLocalX - 1u) / kLocalX;
  const uint32_t groups_y = (tiles_y + kLocalY - 1u) / kLocalY;
  cmd_.dispatch(groups_x, groups_y, 1);

  // 自前イメージは GENERAL に戻す（verify 側の前提と合わせる）。
  vk::ImageMemoryBarrier park = to_read;
  park.srcAccessMask = vk::AccessFlagBits::eShaderRead;
  park.dstAccessMask = vk::AccessFlagBits::eShaderRead;
  park.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  park.newLayout = vk::ImageLayout::eGeneral;
  cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                       vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, park);

  // 結果バッファのデバイス書き込みをホスト読みへつなぐ。
  vk::BufferMemoryBarrier host_barrier{};
  host_barrier.sType = vk::StructureType::eBufferMemoryBarrier;
  host_barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
  host_barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
  host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host_barrier.buffer = *resultBuffer_;
  host_barrier.offset = 0;
  host_barrier.size = kResultSize;
  cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                       vk::PipelineStageFlagBits::eHost, {}, nullptr, host_barrier, nullptr);

  InvokeVk("vkEndCommandBuffer(GPU検査)", [&] { cmd_.end(); });
  vk::CommandBuffer raw = *cmd_;
  vk::SubmitInfo submit{};
  submit.sType = vk::StructureType::eSubmitInfo;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &raw;
  try {
    queue.submit(submit, *fence_);
  } catch (const vk::SystemError& e) {
    const vk::Result result = static_cast<vk::Result>(e.code().value());
    throw UnconfirmedCheck(std::string("vkQueueSubmit(GPU検査) に失敗 (VkResult=") +
                           vk::to_string(result) + ")");
  } catch (const std::exception& e) {
    throw UnconfirmedCheck(std::string("vkQueueSubmit(GPU検査) に失敗: ") + e.what());
  }
  vk::Result fence_result = vk::Result::eSuccess;
  try {
    fence_result = device.waitForFences(*fence_, vk::True, 2000000000u);
  } catch (const vk::SystemError& e) {
    const vk::Result result = static_cast<vk::Result>(e.code().value());
    throw UnconfirmedCheck(std::string("GPU検査の完了待ちに失敗 (VkResult=") +
                           vk::to_string(result) + ")");
  }
  if (fence_result != vk::Result::eSuccess) {
    try {
      device.waitIdle();
    } catch (const vk::SystemError& e) {
      const vk::Result result = static_cast<vk::Result>(e.code().value());
      throw UnconfirmedCheck(std::string("GPU検査の待避 waitIdle に失敗 (VkResult=") +
                             vk::to_string(result) + ")");
    } catch (const std::exception& e) {
      throw UnconfirmedCheck(std::string("GPU検査の待避 waitIdle に失敗: ") + e.what());
    }
    throw UnconfirmedCheck("GPU検査の完了待ちが 2 秒でタイムアウト");
  }

  Report report{};
  std::memcpy(&report, mapped_, kResultSize);
  return report;
}

}  // namespace xrift::transfer

#else

#include <stdexcept>

#include "transfer/gpu_check.h"

namespace xrift::transfer {

GpuChecker::GpuChecker(const gfx::VulkanContext&) {
  throw std::runtime_error("全画面 GPU 検査は Windows のみ（Linux は将来対応）");
}
GpuChecker::~GpuChecker() = default;
void GpuChecker::BindImage(vk::Image, uint32_t, uint32_t) {
  throw std::runtime_error("全画面 GPU 検査は Windows のみ（Linux は将来対応）");
}
GpuChecker::Report GpuChecker::Check(uint32_t, uint32_t) {
  throw std::runtime_error("全画面 GPU 検査は Windows のみ（Linux は将来対応）");
}

}  // namespace xrift::transfer

#endif

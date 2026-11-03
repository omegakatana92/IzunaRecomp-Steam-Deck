// [IzunaRecomp] The native renderer's pipelines (native_draw_internal.h): whole,
// or linked from per-shader parts; their build workers; the pipeline cache and
// the precompile of the pipelines earlier runs used.

#include "native/native_draw_internal.h"

namespace ng2native::draw {

VkBlendFactor BlendFactor(uint32_t x) {
  switch (x) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp BlendOp(uint32_t x) {
  switch (x) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

// Build one pipeline (on a worker thread).
// Render target i's blend state from RB_BLENDCONTROLi and RB_COLOR_MASK.
VkPipelineColorBlendAttachmentState BlendAttachment(const PipelineKey& key, uint32_t i) {
  VkPipelineColorBlendAttachmentState a = {};
  const uint32_t b = key.blend[i];
  a.srcColorBlendFactor = BlendFactor(b & 31);
  a.colorBlendOp = BlendOp((b >> 5) & 7);
  a.dstColorBlendFactor = BlendFactor((b >> 8) & 31);
  a.srcAlphaBlendFactor = BlendFactor((b >> 16) & 31);
  a.alphaBlendOp = BlendOp((b >> 21) & 7);
  a.dstAlphaBlendFactor = BlendFactor((b >> 24) & 31);
  a.blendEnable = !(a.srcColorBlendFactor == VK_BLEND_FACTOR_ONE && a.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
                    a.colorBlendOp == VK_BLEND_OP_ADD && a.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
                    a.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO && a.alphaBlendOp == VK_BLEND_OP_ADD);
  a.colorWriteMask = (key.color_mask >> (i * 4)) & 0xF;
  if (i == 0 && (key.raster & kRasterCoverageBlend)) {
    // [IzunaRecomp] Alpha to mask on a 2x MSAA surface: colour and alpha both
    // weighted by the shader's coverage output (index 1) - the hardware's
    // resolve of two samples, one kept and one drawn.
    a.blendEnable = VK_TRUE;
    a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC1_ALPHA;
    a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  return a;
}

VkPipeline BuildPipeline(const PipelineKey& key, VkShaderModule vs_module, VkShaderModule ps_module, uint32_t vs_size,
                         uint32_t ps_size) {
  VkPipelineShaderStageCreateInfo stages[3] = {};
  uint32_t spec = key.spec;
  VkSpecializationMapEntry me = {0, 0, sizeof(uint32_t)};
  VkSpecializationInfo si = {1, &me, sizeof(spec), &spec};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs_module;
  stages[0].pName = "shaderMain";
  stages[0].pSpecializationInfo = &si;
  uint32_t stage_count = 1;
  if (ps_module) {
    stages[stage_count] = stages[0];
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count++].module = ps_module;
  } else if ((key.spec & kSpecFloat24Depth) && g.depth24_fs) {  // (a depth-only draw on a 20e4 target)
    stages[stage_count] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = g.depth24_fs;
    stages[stage_count++].pName = "main";
  }
  if (key.topology & kRectList) {
    stages[stage_count] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    stages[stage_count].module = g.rect_gs;
    stages[stage_count++].pName = "main";
  }
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VkPrimitiveTopology(key.topology & 0xFF);
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  // PA_SU_SC_MODE_CNTL: face 0 = counter-clockwise is front, in screen space;
  // the output keeps the Xenos screen orientation, so it maps as it is (as in
  // the SDK's Vulkan backend).
  rs.cullMode = ((key.raster & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((key.raster & 2) ? VK_CULL_MODE_BACK_BIT : 0);
  rs.frontFace = (key.raster & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.depthBiasEnable = (key.raster & 8) ? VK_TRUE : VK_FALSE;
  rs.depthClampEnable = (key.raster & 16) ? VK_TRUE : VK_FALSE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  const uint32_t dc = key.depth_control;
  if (key.depth_format) {
    ds.depthTestEnable = (dc >> 1) & 1;
    ds.depthWriteEnable = (dc >> 2) & 1;
    ds.depthCompareOp = VkCompareOp((dc >> 4) & 7);
    ds.stencilTestEnable = dc & 1;
    auto face = [&](uint32_t shift) {
      VkStencilOpState s = {};
      s.compareOp = VkCompareOp((dc >> shift) & 7);
      s.failOp = VkStencilOp((dc >> (shift + 3)) & 7);
      s.passOp = VkStencilOp((dc >> (shift + 6)) & 7);
      s.depthFailOp = VkStencilOp((dc >> (shift + 9)) & 7);
      s.compareMask = (key.stencil_masks >> 8) & 0xFF;
      s.writeMask = (key.stencil_masks >> 16) & 0xFF;
      return s;
    };
    ds.front = face(8);
    ds.back = (dc >> 7) & 1 ? face(20) : ds.front;
  }
  std::array<VkPipelineColorBlendAttachmentState, 4> att{};
  std::array<VkFormat, 4> formats{};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!key.color_formats[i]) continue;
    color_count = i + 1;
    formats[i] = VkFormat(key.color_formats[i]);
    att[i] = BlendAttachment(key, i);
  }
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = color_count;
  cb.pAttachments = att.data();
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 5;
  dy.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = color_count;
  ri.pColorAttachmentFormats = formats.data();
  ri.depthAttachmentFormat = VkFormat(key.depth_format);
  ri.stencilAttachmentFormat = VkFormat(key.depth_format);
  VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.pNext = &ri;
  pci.stageCount = stage_count;
  pci.pStages = stages;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vp;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pDepthStencilState = &ds;
  pci.pColorBlendState = &cb;
  pci.pDynamicState = &dy;
  pci.layout = g.layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const auto t0 = std::chrono::steady_clock::now();
  const VkResult pr = g.vk.CreateGraphicsPipelines(g.ctx.device, g.pipeline_cache, 1, &pci, nullptr, &pipeline);
  const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (took > 100.0) {
    REXLOG_INFO("[native] pipeline VS {:016X} ({} bytes) PS {:016X} ({} bytes) took {:.0f} ms", key.vs, vs_size,
                key.ps, ps_size, took);
  }
  if (pr != VK_SUCCESS) {
    pipeline = VK_NULL_HANDLE;
    REXLOG_WARN("[native] pipeline failed: VS {:016X} PS {:016X}", key.vs, key.ps);
  }
  return pipeline;
}

// --- Graphics pipeline libraries --------------------------------------------

// The dynamic states of each part (only those of its own state).
constexpr VkDynamicState kPreRasterDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT,  VK_DYNAMIC_STATE_SCISSOR,
                                                VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                                                VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};
constexpr VkDynamicState kFragmentDynamic[] = {
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,   VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,          VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,  VK_DYNAMIC_STATE_STENCIL_REFERENCE};
constexpr VkDynamicState kOutputDynamic[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};

// One part. `part`: its VkGraphicsPipelineLibraryFlagBitsEXT.
VkPipeline BuildPart(VkGraphicsPipelineLibraryFlagsEXT part, const VkPipelineShaderStageCreateInfo* stages,
                     uint32_t stage_count, const PipelineKey& key) {
  VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
  li.flags = part;
  std::array<VkFormat, 4> formats{};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!key.color_formats[i]) continue;
    color_count = i + 1;
    formats[i] = VkFormat(key.color_formats[i]);
  }
  VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.pNext = &li;
  ri.colorAttachmentCount = color_count;
  ri.pColorAttachmentFormats = formats.data();
  ri.depthAttachmentFormat = VkFormat(key.depth_format);
  ri.stencilAttachmentFormat = VkFormat(key.depth_format);
  VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.pNext = &ri;
  pci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
  pci.layout = g.layout;
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VkPrimitiveTopology(key.topology & 0xFF);
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.depthClampEnable = (key.raster & 16) ? VK_TRUE : VK_FALSE;  // (static: the vertex part is keyed by it)
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  std::array<VkPipelineColorBlendAttachmentState, 4> att{};
  for (uint32_t i = 0; i < color_count; ++i) att[i] = BlendAttachment(key, i);
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = color_count;
  cb.pAttachments = att.data();
  VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  switch (part) {
    case VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT:
      pci.pVertexInputState = &vi;
      pci.pInputAssemblyState = &ia;
      break;
    case VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT:
      pci.pViewportState = &vp;
      pci.pRasterizationState = &rs;
      dy.dynamicStateCount = uint32_t(std::size(kPreRasterDynamic));
      dy.pDynamicStates = kPreRasterDynamic;
      break;
    case VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT:
      pci.pMultisampleState = &ms;
      pci.pDepthStencilState = &ds;
      dy.dynamicStateCount = uint32_t(std::size(kFragmentDynamic));
      dy.pDynamicStates = kFragmentDynamic;
      break;
    default:  // fragment output
      pci.pMultisampleState = &ms;
      pci.pColorBlendState = &cb;
      dy.dynamicStateCount = uint32_t(std::size(kOutputDynamic));
      dy.pDynamicStates = kOutputDynamic;
      break;
  }
  if (dy.dynamicStateCount) pci.pDynamicState = &dy;
  pci.stageCount = stage_count;
  pci.pStages = stage_count ? stages : nullptr;
  VkPipeline p = VK_NULL_HANDLE;
  if (g.vk.CreateGraphicsPipelines(g.ctx.device, g.pipeline_cache, 1, &pci, nullptr, &p) != VK_SUCCESS) {
    p = VK_NULL_HANDLE;
  }
  return p;
}

// A shader part: the vertex shader's (with the rectangle geometry shader for
// rect lists) or the pixel shader's (none: a depth-only part).
VkPipeline BuildShaderPart(bool vertex, VkShaderModule module, const PipelineKey& key) {
  uint32_t spec = key.spec;
  VkSpecializationMapEntry me = {0, 0, sizeof(uint32_t)};
  VkSpecializationInfo si = {1, &me, sizeof(spec), &spec};
  VkPipelineShaderStageCreateInfo stages[2] = {};
  uint32_t n = 0;
  if (module) {
    stages[n] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[n].stage = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[n].module = module;
    stages[n].pName = "shaderMain";
    stages[n++].pSpecializationInfo = &si;
  } else if (!vertex && (key.spec & kSpecFloat24Depth) && g.depth24_fs) {  // (a depth-only draw on a 20e4 target)
    stages[n] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[n].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[n].module = g.depth24_fs;
    stages[n++].pName = "main";
  }
  if (vertex && (key.topology & kRectList)) {
    stages[n] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[n].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    stages[n].module = g.rect_gs;
    stages[n++].pName = "main";
  }
  const auto t0 = std::chrono::steady_clock::now();
  const VkPipeline p = BuildPart(vertex ? VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
                                        : VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
                                 stages, n, key);
  const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (took > 100.0 || !p) {
    REXLOG_INFO("[native] {} shader part {:016X}: {} in {:.0f} ms", vertex ? "vertex" : "pixel",
                vertex ? key.vs : key.ps, p ? "built" : "FAILED", took);
  }
  return p;
}

VkPipeline LinkParts(const VkPipeline parts[4], bool optimize) {
  VkPipelineLibraryCreateInfoKHR li = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
  li.libraryCount = 4;
  li.pLibraries = parts;
  VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.pNext = &li;
  pci.flags = optimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
  pci.layout = g.layout;
  VkPipeline p = VK_NULL_HANDLE;
  if (g.vk.CreateGraphicsPipelines(g.ctx.device, g.pipeline_cache, 1, &pci, nullptr, &p) != VK_SUCCESS) {
    p = VK_NULL_HANDLE;
  }
  return p;
}

// The shader part for this key's vertex (or pixel) shader: ready, or queued
// (`urgent`: ahead of the precompile) - null and `*failed` false meanwhile.
// A shader part's identity: what it depends on - the shader, the
// specialisation (alpha test...), and the rectangle geometry shader / the
// depth format.
uint64_t PartId(bool vertex, const PipelineKey& key) {
  const uint64_t words[4] = {vertex ? key.vs : key.ps, uint64_t(vertex) | uint64_t(key.spec) << 8,
                             vertex ? uint64_t(key.topology & kRectList) : uint64_t(key.depth_format),
                             vertex ? uint64_t(key.raster & 16) : 0};
  return XXH3_64bits(words, sizeof(words));
}

VkPipeline ShaderPart(bool vertex, const PipelineKey& key, const ShaderInfo* shader, bool urgent, bool* failed,
                      bool precompile = false, bool* queued = nullptr) {
  *failed = false;
  const uint64_t id = PartId(vertex, key);
  std::shared_ptr<State::Lib> lib;
  {
    std::lock_guard<std::mutex> lock(g.lib_mutex);
    auto& slot = g.shader_libs[id];
    if (!slot) {
      slot = std::make_shared<State::Lib>();
      lib = slot;
    } else {
      const int state = slot->state.load(std::memory_order_acquire);
      if (state == 1) return slot->pipeline.load(std::memory_order_relaxed);
      *failed = state == 2;
      return VK_NULL_HANDLE;
    }
  }
  State::PipelineJob job{key, shader ? shader->module : VK_NULL_HANDLE, VK_NULL_HANDLE,
                         shader ? shader->entry->spirvSize : 0u, 0u, nullptr};
  job.kind = vertex ? 1 : 2;
  job.lib = lib;
  job.precompile = precompile;
  if (queued) *queued = true;
  {
    std::lock_guard<std::mutex> lock(g.job_mutex);
    (urgent ? g.urgent : g.jobs).push_back(std::move(job));
  }
  g.job_cv.notify_one();
  return VK_NULL_HANDLE;
}

// The vertex input or output part (no shader: built on the spot, kept).
VkPipeline StatePart(bool output, const PipelineKey& key) {
  uint32_t words[12] = {uint32_t(output)};
  if (output) {
    std::memcpy(words + 1, key.color_formats, sizeof(key.color_formats));
    words[5] = key.depth_format;
    std::memcpy(words + 6, key.blend, sizeof(key.blend));
    words[10] = key.color_mask;
    words[11] = key.raster & kRasterCoverageBlend;  // [IzunaRecomp] (RT0's dual-source blend: BlendAttachment)
  } else {
    words[1] = key.topology & 0xFF;
  }
  const uint64_t id = XXH3_64bits(words, sizeof(words));
  std::lock_guard<std::mutex> lock(g.state_lib_mutex);  // (the pre-links build them too)
  auto it = g.state_libs.find(id);
  if (it != g.state_libs.end()) return it->second;
  const VkPipeline p = BuildPart(output ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT
                                        : VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
                                 nullptr, 0, key);
  g.state_libs.emplace(id, p);
  return p;
}

// A pipeline from an earlier run, linked (optimised) as soon as its shader
// parts are built instead of at its first draw: a new area's first frames
// linked dozens on the drawing thread (test51: 101 pipelines in one load).
// Looked at again later while a part is still being built; given up if one
// failed (its first draw decides) or the draw linked it already.
void PreLink(State::PipelineJob& job) {
  if (job.slot->state.load(std::memory_order_acquire) != 0) return;
  auto part = [&](bool vertex) -> std::shared_ptr<State::Lib> {
    std::lock_guard<std::mutex> lock(g.lib_mutex);
    auto it = g.shader_libs.find(PartId(vertex, job.key));
    return it == g.shader_libs.end() ? nullptr : it->second;
  };
  const auto vs = part(true), ps = part(false);
  const int vs_state = vs ? vs->state.load(std::memory_order_acquire) : 2;
  const int ps_state = ps ? ps->state.load(std::memory_order_acquire) : 2;
  if (vs_state == 2 || ps_state == 2) return;
  if (vs_state == 0 || ps_state == 0) {
    if (++job.tries > 2000) return;  // (~2 s of looking: its first draw links it)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::lock_guard<std::mutex> lock(g.job_mutex);
    g.jobs.push_back(std::move(job));
    return;
  }
  VkPipeline libs[4] = {StatePart(false, job.key), vs->pipeline.load(std::memory_order_acquire),
                        ps->pipeline.load(std::memory_order_acquire), StatePart(true, job.key)};
  if (!libs[0] || !libs[3]) return;
  const VkPipeline p = LinkParts(libs, true);
  if (!p || job.slot->state.load(std::memory_order_acquire) != 0) return;  // (the draw linked it meanwhile)
  job.slot->pipeline.store(p, std::memory_order_release);
  job.slot->state.store(1, std::memory_order_release);
  g.prelinked.fetch_add(1, std::memory_order_relaxed);
  g.pipelines_built.fetch_add(1, std::memory_order_relaxed);
}

void PipelineWorker() {
  for (;;) {
    State::PipelineJob job;
    {
      std::unique_lock<std::mutex> lock(g.job_mutex);
      g.job_cv.wait(lock, [] { return !g.jobs.empty() || !g.urgent.empty(); });
      auto& from = g.urgent.empty() ? g.jobs : g.urgent;
      job = std::move(from.front());
      from.pop_front();
    }
    if (job.kind == 1 || job.kind == 2) {  // a shader part
      if (job.lib->claimed.exchange(true, std::memory_order_acq_rel)) continue;
      const VkPipeline p = BuildShaderPart(job.kind == 1, job.vs, job.key);
      job.lib->pipeline.store(p, std::memory_order_release);
      job.lib->state.store(p ? 1 : 2, std::memory_order_release);
      if (job.precompile && g.precompile_left.fetch_sub(1, std::memory_order_relaxed) == 1) {
        REXLOG_INFO("[native] shader part precompile done");
      }
      continue;
    }
    if (job.kind == 4) {  // a link ahead of the first draw (a key from an earlier run)
      PreLink(job);
      continue;
    }
    if (job.kind == 3) {  // the optimised link, replacing the fast one
      if (const VkPipeline p = LinkParts(job.libs, true)) {
        job.slot->pipeline.store(p, std::memory_order_release);
        g.optimized.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    if (job.slot->claimed.exchange(true, std::memory_order_acq_rel)) continue;  // (its other queue entry)
    const auto t0 = std::chrono::steady_clock::now();
    const VkPipeline p = BuildPipeline(job.key, job.vs, job.ps, job.vs_size, job.ps_size);
    job.slot->pipeline.store(p, std::memory_order_release);
    job.slot->state.store(p ? 1 : 2, std::memory_order_release);
    g.pipelines_built.fetch_add(1, std::memory_order_relaxed);
    if (job.precompile && g.precompile_left.fetch_sub(1, std::memory_order_relaxed) == 1) {
      REXLOG_INFO("[native] pipeline precompile done ({} built so far)", g.pipelines_built.load());
    }
    (void)t0;
  }
}

// The pipeline for a draw, or null: still being built (`pending`) or failed.
// With pipeline libraries: linked from the parts on the spot once both shader
// parts are built (a new render state costs a link), the optimised link
// queued behind it.
VkPipeline PipelineFromParts(const PipelineKey& key, const ShaderInfo* vs, const ShaderInfo* ps, bool* pending,
                             bool* whole_pipeline) {
  auto it = g.pipelines.find(key);
  if (it == g.pipelines.end()) {
    it = g.pipelines.emplace(key, std::make_shared<State::PipelineSlot>()).first;
    g.keys_seen.push_back(key);
    g.jobs_queued.fetch_add(1, std::memory_order_relaxed);
  }
  State::PipelineSlot& slot = *it->second;
  const int state = slot.state.load(std::memory_order_acquire);
  if (state == 1) {
    *whole_pipeline = slot.urgent;  // (built whole: its rasterisation / depth state is static)
    return slot.pipeline.load(std::memory_order_acquire);
  }
  if (state == 2) return VK_NULL_HANDLE;
  // (a part or link the driver refuses: the whole pipeline instead, built as
  // without libraries - its static state wins over the dynamic state set)
  auto whole = [&](const char* why) {
    if (slot.claimed.load(std::memory_order_acquire) || slot.urgent) return;
    slot.urgent = true;
    REXLOG_WARN("[native] pipeline VS {:016X} PS {:016X}: {}, built whole instead", key.vs, key.ps, why);
    std::lock_guard<std::mutex> lock(g.job_mutex);
    g.urgent.push_back({key, vs->module, ps ? ps->module : VK_NULL_HANDLE, vs->entry->spirvSize,
                        ps ? ps->entry->spirvSize : 0u, it->second});
    g.job_cv.notify_one();
  };
  if (slot.urgent) {  // (being built whole)
    *pending = true;
    return VK_NULL_HANDLE;
  }
  bool vs_failed, ps_failed;
  const VkPipeline vsp = ShaderPart(true, key, vs, true, &vs_failed);
  const VkPipeline psp = ShaderPart(false, key, ps, true, &ps_failed);
  if (vs_failed || ps_failed) {
    whole("a shader part failed");
    *pending = true;
    return VK_NULL_HANDLE;
  }
  if (!vsp || !psp) {
    *pending = true;
    return VK_NULL_HANDLE;
  }
  State::PipelineJob job{key, VK_NULL_HANDLE, VK_NULL_HANDLE, 0u, 0u, it->second};
  job.kind = 3;
  job.libs[0] = StatePart(false, key);
  job.libs[1] = vsp;
  job.libs[2] = psp;
  job.libs[3] = StatePart(true, key);
  const auto link_t0 = std::chrono::steady_clock::now();
  const VkPipeline p = job.libs[0] && job.libs[3] ? LinkParts(job.libs, false) : VK_NULL_HANDLE;
  {  // (the drawing thread waits for these: how long, for the report)
    const uint64_t ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - link_t0).count());
    g.link_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t worst = g.link_worst_ns.load(std::memory_order_relaxed);
    while (ns > worst && !g.link_worst_ns.compare_exchange_weak(worst, ns)) {
    }
  }
  if (!p) {
    whole("the link failed");
    *pending = true;
    return VK_NULL_HANDLE;
  }
  slot.pipeline.store(p, std::memory_order_release);
  slot.state.store(1, std::memory_order_release);
  g.links.fetch_add(1, std::memory_order_relaxed);
  g.pipelines_built.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g.job_mutex);
    g.jobs.push_back(std::move(job));
  }
  g.job_cv.notify_one();
  return p;
}

VkPipeline Pipeline(const PipelineKey& key, const ShaderInfo* vs, const ShaderInfo* ps, bool* pending,
                    bool* whole_pipeline) {
  const PhaseTimer phase(kPhasePipeline);
  *whole_pipeline = true;
  if (g.gpl) return PipelineFromParts(key, vs, ps, pending, whole_pipeline);
  auto it = g.pipelines.find(key);
  if (it == g.pipelines.end()) {
    auto slot = std::make_shared<State::PipelineSlot>();
    slot->urgent = true;
    it = g.pipelines.emplace(key, slot).first;
    {
      std::lock_guard<std::mutex> lock(g.job_mutex);
      g.urgent.push_back({key, vs->module, ps ? ps->module : VK_NULL_HANDLE, vs->entry->spirvSize,
                          ps ? ps->entry->spirvSize : 0u, slot});
    }
    g.keys_seen.push_back(key);
    g.jobs_queued.fetch_add(1, std::memory_order_relaxed);
    g.job_cv.notify_one();
  } else if (!it->second->urgent && it->second->state.load(std::memory_order_acquire) == 0) {
    // Precompiled pipeline a draw needs now: ahead of the rest of the list.
    it->second->urgent = true;
    {
      std::lock_guard<std::mutex> lock(g.job_mutex);
      g.urgent.push_back({key, vs->module, ps ? ps->module : VK_NULL_HANDLE, vs->entry->spirvSize,
                          ps ? ps->entry->spirvSize : 0u, it->second});
    }
    g.job_cv.notify_one();
  }
  const int state = it->second->state.load(std::memory_order_acquire);
  *pending = state == 0;
  return state == 1 ? it->second->pipeline.load(std::memory_order_relaxed) : VK_NULL_HANDLE;
}


// Where the pipeline cache lives: UserData on the PS5, cache\ beside the
// executable on a PC.
std::filesystem::path PipelineDir() {
#if defined(IZUNA_LINUX_DESKTOP)
  return ng2ps5::paths::Cache();
#elif defined(__PROSPERO__)
  return ng2ps5::paths::UserData();
#else
#if defined(_WIN32)
  // [IzunaRecomp] One cache for every copy of the game on this PC
  // (%LOCALAPPDATA%\IzunaRecomp\cache): a new build in a new folder starts
  // with the pipelines the last one built, instead of building them again in
  // play. (The driver checks the cache is its own; the keys file has a
  // version.) Beside the .exe when there is no such folder.
  static const std::filesystem::path dir = [] {
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && *local) {
      std::filesystem::path d = std::filesystem::path(local) / L"IzunaRecomp" / L"cache";
      std::error_code ec;
      std::filesystem::create_directories(d, ec);
      if (!ec && std::filesystem::is_directory(d, ec)) return d;
    }
    return rex::filesystem::GetExecutableFolder() / "cache";
  }();
  return dir;
#else
  return rex::filesystem::GetExecutableFolder() / "cache";
#endif
#endif
}

std::filesystem::path PipelineCachePath() { return PipelineDir() / "native_pipelines.bin"; }

void CreatePipelineCache() {
  std::vector<char> data;
  std::ifstream f(PipelineCachePath(), std::ios::binary);
  if (f && !REXCVAR_GET(native_ignore_pipeline_cache)) {
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  ci.initialDataSize = data.size();
  ci.pInitialData = data.data();
  if (g.vk.CreatePipelineCache(g.ctx.device, &ci, nullptr, &g.pipeline_cache) != VK_SUCCESS) {
    ci.initialDataSize = 0;  // (a cache from another driver: start empty)
    g.vk.CreatePipelineCache(g.ctx.device, &ci, nullptr, &g.pipeline_cache);
  }
  REXLOG_INFO("[native] pipeline cache: {} KB loaded", data.size() >> 10);
}

std::filesystem::path PipelineKeysPath() { return PipelineDir() / "native_pipeline_keys.bin"; }
constexpr uint32_t kKeysMagic = 0x4B504E4E;  // 'NNPK'
// Bump when PipelineKey's meaning changes (its size is checked as well).
constexpr uint32_t kKeysVersion = 1;

void SavePipelineKeys() {
  if (g.keys_seen.size() == g.keys_saved) return;
  const uint32_t header[4] = {kKeysMagic, kKeysVersion, uint32_t(sizeof(PipelineKey)), uint32_t(g.keys_seen.size())};
  std::vector<char> data(sizeof(header) + g.keys_seen.size() * sizeof(PipelineKey));
  std::memcpy(data.data(), header, sizeof(header));
  std::memcpy(data.data() + sizeof(header), g.keys_seen.data(), g.keys_seen.size() * sizeof(PipelineKey));
  SaveFileLater(PipelineKeysPath(), [data = std::move(data)] { return data; });
  g.keys_saved = g.keys_seen.size();
}

// The shipped key list (data/native/pipeline_keys.bin, built in:
// tools/native/pipeline_keys.py) - pipelines the owner's runs used.
constexpr uint8_t kShippedKeys[] = {
#include "pipeline_keys.inc"
};

// Queues the pipelines of one key file's bytes (header + keys); returns how
// many were queued (0 for a file of another version).
uint32_t QueueKeys(const uint8_t* data, size_t size, size_t* total) {
  uint32_t header[4] = {};
  if (size < sizeof(header)) return 0;
  std::memcpy(header, data, sizeof(header));
  if (header[0] != kKeysMagic || header[1] != kKeysVersion || header[2] != sizeof(PipelineKey) ||
      header[3] > 100000 || size < sizeof(header) + size_t(header[3]) * sizeof(PipelineKey)) {
    return 0;
  }
  *total += header[3];
  uint32_t queued = 0;
  std::vector<std::tuple<PipelineKey, const ShaderInfo*, const ShaderInfo*>> parts;  // (pipeline libraries)
  std::vector<std::shared_ptr<State::PipelineSlot>> prelinks;                        // (... and their slots)
  {
    std::lock_guard<std::mutex> lock(g.job_mutex);
    for (uint32_t i = 0; i < header[3]; ++i) {
      PipelineKey key;
      std::memcpy(&key, data + sizeof(header) + size_t(i) * sizeof(PipelineKey), sizeof(key));
      // (keys saved before test37 lack SPEC_CONSTANT_FLOAT24_DEPTH: NG2's depth
      // targets are 20e4, so a key testing or writing depth gets it - else
      // every such pipeline was built at its first draw, the objects missing
      // until then)
      if (key.depth_format && (key.depth_control & 6)) key.spec |= kSpecFloat24Depth;
      if (g.pipelines.count(key)) continue;
      const ShaderInfo* vs = ShaderByHash(key.vs);
      const ShaderInfo* ps = key.ps ? ShaderByHash(key.ps) : nullptr;
      if (!vs || (key.ps && !ps)) continue;  // (a shader no longer in the cache)
      g.keys_seen.push_back(key);
      if (g.gpl) {  // (its shader parts now, then its link - PreLink)
        auto slot = std::make_shared<State::PipelineSlot>();
        g.pipelines.emplace(key, slot);
        parts.emplace_back(key, vs, ps);
        prelinks.push_back(std::move(slot));
        continue;
      }
      auto slot = std::make_shared<State::PipelineSlot>();
      g.pipelines.emplace(key, slot);
      State::PipelineJob job{key, vs->module, ps ? ps->module : VK_NULL_HANDLE, vs->entry->spirvSize,
                             ps ? ps->entry->spirvSize : 0u, slot};
      job.precompile = true;
      g.jobs.push_back(std::move(job));
      ++queued;
    }
  }
  for (const auto& [key, vs, ps] : parts) {
    bool failed, q = false;
    ShaderPart(true, key, vs, false, &failed, true, &q);
    queued += q;
    q = false;
    ShaderPart(false, key, ps, false, &failed, true, &q);
    queued += q;
  }
  {  // (behind every part: the links come once their parts are built)
    std::lock_guard<std::mutex> lock(g.job_mutex);
    for (size_t i = 0; i < parts.size(); ++i) {
      State::PipelineJob job{std::get<0>(parts[i]), VK_NULL_HANDLE, VK_NULL_HANDLE, 0u, 0u, prelinks[i]};
      job.kind = 4;
      g.jobs.push_back(std::move(job));
    }
  }
  g.job_cv.notify_all();
  return queued;
}

// At start (before the game draws): queue every pipeline the shipped list and
// this player's earlier runs (UserData/native_pipeline_keys.bin) used.
void QueuePrecompile() {
  const auto t0 = std::chrono::steady_clock::now();
  size_t total = 0;
  const uint32_t shipped = QueueKeys(kShippedKeys, sizeof(kShippedKeys), &total);
  uint32_t own = 0;
  std::ifstream f(PipelineKeysPath(), std::ios::binary);
  if (f) {
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    own = QueueKeys(data.data(), data.size(), &total);
  }
  g.keys_saved = 0;  // (the player's file is rewritten with the merged list)
  const uint32_t queued = shipped + own;
  g.precompile_left.store(queued, std::memory_order_relaxed);
  g.jobs_queued.fetch_add(queued, std::memory_order_relaxed);
  g.job_cv.notify_all();
  REXLOG_INFO("[native] pipeline precompile: {} queued ({} shipped, {} from this player's runs; {} keys read) in {} ms",
              queued, shipped, own, total,
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

// Save the pipeline cache when pipelines were added since the last save.
void SavePipelineCache() {
  SavePipelineKeys();
  const size_t built = g.pipelines_built.load(std::memory_order_relaxed);
  if (!g.pipeline_cache || built == g.pipelines_saved) return;
  // (vkGetPipelineCacheData needs no external synchronisation: the pipeline
  // builders use the cache meanwhile, as they always do)
  SaveFileLater(PipelineCachePath(), [] {
    std::vector<char> data;
    size_t size = 0;
    if (g.vk.GetPipelineCacheData(g.ctx.device, g.pipeline_cache, &size, nullptr) != VK_SUCCESS || !size) {
      return data;
    }
    data.resize(size);
    if (g.vk.GetPipelineCacheData(g.ctx.device, g.pipeline_cache, &size, data.data()) != VK_SUCCESS) data.clear();
    data.resize(size);
    return data;
  });
  g.pipelines_saved = built;
}

}  // namespace ng2native::draw

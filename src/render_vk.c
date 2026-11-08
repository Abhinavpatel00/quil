#include "render_vk.h"
#include "spv.h"

#include <GLFW/glfw3.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#define VK_TRY(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "Vulkan error %d at %s:%d (%s)\n", _r, __FILE__, __LINE__, #call); \
    return false; } } while (0)

#define STAGING_SIZE (1u << 20)
#define SOLID_VERT_SIZE (QUILL_MAX_SOLIDS * 6u * 24u)
#define GLYPH_VERT_SIZE (QUILL_MAX_GLYPHS * 6u * 32u)
#define VFX_VERT_SIZE   ((VkDeviceSize)QUILL_MAX_VFX * 6u * sizeof(VfxVert))

/* uv/size/radius feed the rounded-rect SDF in solid.frag. Quads with
 * radius = 0 take the pixel-exact path, so text highlights and the cursor
 * keep the sharp edges they had before rounding existed. */
typedef struct SolidVert {
    float pos[2]; float color[4]; float uv[2]; float size[2]; float radius; float pad;
} SolidVert;
typedef struct GlyphVert { float pos[2]; float uv[2]; float color[4]; } GlyphVert;
typedef struct VfxVert { float pos[2]; float color[4]; float params[4]; float size[2]; float uv[2]; } VfxVert;

typedef struct FrameRes
{
    VkCommandBuffer cmd;
    VkFence         fence;
    VkSemaphore     acquire;
    VkBuffer        solid_buf, glyph_buf, vfx_buf;
    VkDeviceMemory  solid_mem, glyph_mem, vfx_mem;
    void*           solid_map;
    void*           glyph_map;
    void*           vfx_map;
} FrameRes;

struct VkRenderer
{
    VkInstance       instance;
    VkDebugUtilsMessengerEXT debug;
    VkPhysicalDevice phys;
    VkDevice         dev;
    VkQueue          queue;
    uint32_t         queue_family;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkFormat         format;
    VkExtent2D       extent;
    uint32_t         nimages;
    VkImage          images[8];
    VkImageView      views[8];
    VkFramebuffer    fbs[8];
    VkSemaphore      finished[8];
    bool             headless;
    VkImage          target;
    VkDeviceMemory   target_mem;
    VkImageView      target_view;
    VkFramebuffer    target_fb;
    VkBuffer         readback;
    VkDeviceMemory   readback_mem;
    void*            readback_map;

    VkCommandPool    pool;
    VkRenderPass     pass;
    VkDescriptorSetLayout desc_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc;
    VkPipelineLayout solid_layout, glyph_layout, vfx_layout;
    VkPipeline       solid_pipe, glyph_pipe, vfx_pipe;

    VkImage          atlas;
    VkDeviceMemory   atlas_mem;
    VkImageView      atlas_view;
    VkSampler        sampler;
    VkImageLayout    atlas_layout;
    uint32_t         atlas_size;

    /* Optional background image. All handles are VK_NULL_HANDLE / 0 when
     * disabled, so the frame path skips it without touching any texture. */
    VkImage          bg_image;
    VkDeviceMemory   bg_mem;
    VkImageView      bg_view;
    VkSampler        bg_sampler;
    VkDescriptorPool bg_pool;
    VkDescriptorSet  bg_desc;
    VkPipelineLayout bg_layout;
    VkPipeline       bg_pipe;
    uint32_t         bg_w, bg_h;
    BackgroundFit    bg_fit;
    float            bg_dim, bg_vignette;
    /* Whole-window opacity; 1 is opaque. Every shader premultiplies its
     * output by this, and the swapchain uses PRE_MULTIPLIED when available,
     * so the compositor can blend the window with the desktop. */
    float            window_opacity;

    FrameRes         frames[QUILL_FRAMES];
    uint32_t         frame_index;
    bool             first_frame_trimmed;
    char             device_name[256];
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user)
{
    (void)type; (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[vulkan] %s\n", data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

static uint32_t find_mem(VkRenderer* r, uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(r->phys, &mp);
    for (i = 0u; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

static bool make_buffer(VkRenderer* r, VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags props, VkBuffer* buf,
                        VkDeviceMemory* mem, void** map)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    uint32_t mt;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_TRY(vkCreateBuffer(r->dev, &bi, NULL, buf));
    vkGetBufferMemoryRequirements(r->dev, *buf, &req);
    mt = find_mem(r, req.memoryTypeBits, props);
    if (mt == UINT32_MAX) return false;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    VK_TRY(vkAllocateMemory(r->dev, &ai, NULL, mem));
    VK_TRY(vkBindBufferMemory(r->dev, *buf, *mem, 0));
    if (map)
        VK_TRY(vkMapMemory(r->dev, *mem, 0, size, 0, map));
    return true;
}

static bool make_image(VkRenderer* r, uint32_t w, uint32_t h, VkFormat format,
                       VkImageUsageFlags usage, VkImage* img, VkDeviceMemory* mem)
{
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    uint32_t mt;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1u;
    ii.mipLevels = 1u;
    ii.arrayLayers = 1u;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_TRY(vkCreateImage(r->dev, &ii, NULL, img));
    vkGetImageMemoryRequirements(r->dev, *img, &req);
    mt = find_mem(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) return false;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    VK_TRY(vkAllocateMemory(r->dev, &ai, NULL, mem));
    VK_TRY(vkBindImageMemory(r->dev, *img, *mem, 0));
    return true;
}

static bool make_view(VkRenderer* r, VkImage img, VkFormat format, VkImageView* view)
{
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1u;
    vi.subresourceRange.layerCount = 1u;
    VK_TRY(vkCreateImageView(r->dev, &vi, NULL, view));
    return true;
}

static bool make_shader(VkRenderer* r, const uint32_t* code, size_t bytes, VkShaderModule* mod)
{
    VkShaderModuleCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = bytes;
    si.pCode = code;
    VK_TRY(vkCreateShaderModule(r->dev, &si, NULL, mod));
    return true;
}

static bool make_pipeline(VkRenderer* r, const uint32_t* vs, size_t vs_size,
                          const uint32_t* fs, size_t fs_size,
                          VkVertexInputBindingDescription* binding,
                          VkVertexInputAttributeDescription* attrs, uint32_t nattrs,
                          VkPipelineLayout layout, VkPipeline* pipe)
{
    VkShaderModule vsm, fsm;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dyn;
    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkGraphicsPipelineCreateInfo pi;
    bool ok = false;

    if (!make_shader(r, vs, vs_size, &vsm)) return false;
    if (!make_shader(r, fs, fs_size, &fsm)) { vkDestroyShaderModule(r->dev, vsm, NULL); return false; }
    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName = "main";
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = binding ? 1u : 0u;
    vi.pVertexBindingDescriptions = binding;
    vi.vertexAttributeDescriptionCount = nattrs;
    vi.pVertexAttributeDescriptions = attrs;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1u;
    vp.scissorCount = 1u;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = VK_TRUE;
    /* Premultiplied source-over. Every shader multiplies rgb and alpha by the
     * quad's own alpha (and the window opacity) before writing, which makes
     * opaque frames render exactly as before while letting a translucent
     * window composite correctly: alpha accumulates toward the cleared value
     * instead of doubling. */
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    /* Leave the destination alpha alone. The window alpha is set once by the
     * clear (window_opacity) and must stay there; accumulating source alpha
     * would push it back toward 1 and cancel the transparency (a 0.5 window
     * measured 0.86 with the usual ONE / ONE_MINUS_SRC_ALPHA alpha factors).
     * Colour still composites normally, premultiplied. */
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1u;
    cb.pAttachments = &cba;
    memset(&dyn, 0, sizeof dyn);
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2u;
    dyn.pDynamicStates = dyn_states;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2u;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dyn;
    pi.layout = layout;
    pi.renderPass = r->pass;
    if (vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1u, &pi, NULL, pipe) == VK_SUCCESS)
        ok = true;
    vkDestroyShaderModule(r->dev, vsm, NULL);
    vkDestroyShaderModule(r->dev, fsm, NULL);
    return ok;
}

static bool create_pass(VkRenderer* r)
{
    VkAttachmentDescription att;
    VkAttachmentReference ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo rp;
    memset(&att, 0, sizeof att);
    att.format = r->format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ref.attachment = 0u;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1u;
    sub.pColorAttachments = &ref;
    memset(&dep, 0, sizeof dep);
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0u;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0u;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1u;
    rp.pAttachments = &att;
    rp.subpassCount = 1u;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1u;
    rp.pDependencies = &dep;
    VK_TRY(vkCreateRenderPass(r->dev, &rp, NULL, &r->pass));
    return true;
}

static bool create_atlas(VkRenderer* r)
{
    VkDescriptorSetLayoutBinding bind;
    VkDescriptorSetLayoutCreateInfo dl;
    VkDescriptorPoolSize ps;
    VkDescriptorPoolCreateInfo dp;
    VkDescriptorSetAllocateInfo da;
    VkSamplerCreateInfo si;
    VkDescriptorImageInfo img;
    VkWriteDescriptorSet wr;

    if (!make_image(r, QUILL_ATLAS_SMALL_SIZE, QUILL_ATLAS_SMALL_SIZE, VK_FORMAT_R8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    &r->atlas, &r->atlas_mem))
        return false;
    if (!make_view(r, r->atlas, VK_FORMAT_R8_UNORM, &r->atlas_view))
        return false;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = 1.0f;
    VK_TRY(vkCreateSampler(r->dev, &si, NULL, &r->sampler));

    memset(&bind, 0, sizeof bind);
    bind.binding = 0u;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1u;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&dl, 0, sizeof dl);
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1u;
    dl.pBindings = &bind;
    VK_TRY(vkCreateDescriptorSetLayout(r->dev, &dl, NULL, &r->desc_layout));
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 1u;
    memset(&dp, 0, sizeof dp);
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.poolSizeCount = 1u;
    dp.pPoolSizes = &ps;
    dp.maxSets = 1u;
    VK_TRY(vkCreateDescriptorPool(r->dev, &dp, NULL, &r->desc_pool));
    memset(&da, 0, sizeof da);
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = r->desc_pool;
    da.descriptorSetCount = 1u;
    da.pSetLayouts = &r->desc_layout;
    VK_TRY(vkAllocateDescriptorSets(r->dev, &da, &r->desc));
    memset(&img, 0, sizeof img);
    img.sampler = r->sampler;
    img.imageView = r->atlas_view;
    img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = r->desc;
    wr.dstBinding = 0u;
    wr.descriptorCount = 1u;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &img;
    vkUpdateDescriptorSets(r->dev, 1u, &wr, 0u, NULL);
    r->atlas_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    r->atlas_size = QUILL_ATLAS_SMALL_SIZE;
    return true;
}

static bool create_pipelines(VkRenderer* r)
{
    VkPushConstantRange solid_pc, glyph_pc, vfx_pc;
    VkPipelineLayoutCreateInfo pl;
    VkVertexInputBindingDescription sb, gb, vb;
    VkVertexInputAttributeDescription sa[5], ga[3], va[5];

    memset(&solid_pc, 0, sizeof solid_pc);
    solid_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    solid_pc.size = 16u; /* vec2 screen + float window + float pad */
    memset(&glyph_pc, 0, sizeof glyph_pc);
    glyph_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    glyph_pc.size = 24u; /* vec2 screen + gamma + contrast + window + pad */
    memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.pushConstantRangeCount = 1u;
    pl.pPushConstantRanges = &solid_pc;
    VK_TRY(vkCreatePipelineLayout(r->dev, &pl, NULL, &r->solid_layout));
    memset(&vfx_pc, 0, sizeof vfx_pc);
    vfx_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    vfx_pc.size = 16u; /* vec2 screen + float window + float pad */
    pl.pushConstantRangeCount = 1u;
    pl.pPushConstantRanges = &vfx_pc;
    pl.setLayoutCount = 0u;
    pl.pSetLayouts = NULL;
    VK_TRY(vkCreatePipelineLayout(r->dev, &pl, NULL, &r->vfx_layout));
    pl.pushConstantRangeCount = 1u;
    pl.pPushConstantRanges = &glyph_pc;
    pl.setLayoutCount = 1u;
    pl.pSetLayouts = &r->desc_layout;
    VK_TRY(vkCreatePipelineLayout(r->dev, &pl, NULL, &r->glyph_layout));

    sb.binding = 0u; sb.stride = sizeof(SolidVert); sb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    sa[0].binding = 0u; sa[0].location = 0u; sa[0].format = VK_FORMAT_R32G32_SFLOAT; sa[0].offset = 0u;
    sa[1].binding = 0u; sa[1].location = 1u; sa[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; sa[1].offset = 8u;
    sa[2].binding = 0u; sa[2].location = 2u; sa[2].format = VK_FORMAT_R32G32_SFLOAT; sa[2].offset = 24u;
    sa[3].binding = 0u; sa[3].location = 3u; sa[3].format = VK_FORMAT_R32G32_SFLOAT; sa[3].offset = 32u;
    sa[4].binding = 0u; sa[4].location = 4u; sa[4].format = VK_FORMAT_R32_SFLOAT; sa[4].offset = 40u;
    if (!make_pipeline(r, solid_vert, sizeof solid_vert, solid_frag, sizeof solid_frag,
                       &sb, sa, 5u, r->solid_layout, &r->solid_pipe))
        return false;
    gb.binding = 0u; gb.stride = sizeof(GlyphVert); gb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    ga[0].binding = 0u; ga[0].location = 0u; ga[0].format = VK_FORMAT_R32G32_SFLOAT; ga[0].offset = 0u;
    ga[1].binding = 0u; ga[1].location = 1u; ga[1].format = VK_FORMAT_R32G32_SFLOAT; ga[1].offset = 8u;
    ga[2].binding = 0u; ga[2].location = 2u; ga[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; ga[2].offset = 16u;
    if (!make_pipeline(r, glyph_vert, sizeof glyph_vert, glyph_frag, sizeof glyph_frag,
                       &gb, ga, 3u, r->glyph_layout, &r->glyph_pipe))
        return false;
    vb.binding = 0u; vb.stride = sizeof(VfxVert); vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    va[0].binding = 0u; va[0].location = 0u; va[0].format = VK_FORMAT_R32G32_SFLOAT; va[0].offset = 0u;
    va[1].binding = 0u; va[1].location = 1u; va[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; va[1].offset = 8u;
    va[2].binding = 0u; va[2].location = 2u; va[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; va[2].offset = 24u;
    va[3].binding = 0u; va[3].location = 3u; va[3].format = VK_FORMAT_R32G32_SFLOAT; va[3].offset = 40u;
    va[4].binding = 0u; va[4].location = 4u; va[4].format = VK_FORMAT_R32G32_SFLOAT; va[4].offset = 48u;
    if (!make_pipeline(r, vfx_vert, sizeof vfx_vert, vfx_frag, sizeof vfx_frag,
                       &vb, va, 5u, r->vfx_layout, &r->vfx_pipe))
        return false;
    return true;
}

/* Background image pipeline. Created lazily, only when an image is loaded,
 * so the unconfigured editor never pays for it. Re-callable: it rebuilds
 * the pipeline if the swapchain format changed. */
static bool create_bg_pipeline(VkRenderer* r)
{
    VkPushConstantRange pc;
    VkPipelineLayoutCreateInfo pl;
    if (r->bg_pipe) { vkDestroyPipeline(r->dev, r->bg_pipe, NULL); r->bg_pipe = VK_NULL_HANDLE; }
    if (r->bg_layout) { vkDestroyPipelineLayout(r->dev, r->bg_layout, NULL); r->bg_layout = VK_NULL_HANDLE; }
    memset(&pc, 0, sizeof pc);
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size = 32u; /* vec2 uv_scale + vec2 uv_offset + opacity + dim + vignette + pad */
    memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.pushConstantRangeCount = 1u;
    pl.pPushConstantRanges = &pc;
    pl.setLayoutCount = 1u;
    pl.pSetLayouts = &r->desc_layout;
    VK_TRY(vkCreatePipelineLayout(r->dev, &pl, NULL, &r->bg_layout));
    if (!make_pipeline(r, bg_vert, sizeof bg_vert, bg_frag, sizeof bg_frag,
                       NULL, NULL, 0u, r->bg_layout, &r->bg_pipe))
        return false;
    return true;
}

static void destroy_bg(VkRenderer* r)
{
    if (r->bg_pipe) { vkDestroyPipeline(r->dev, r->bg_pipe, NULL); r->bg_pipe = VK_NULL_HANDLE; }
    if (r->bg_layout) { vkDestroyPipelineLayout(r->dev, r->bg_layout, NULL); r->bg_layout = VK_NULL_HANDLE; }
    if (r->bg_pool) { vkDestroyDescriptorPool(r->dev, r->bg_pool, NULL); r->bg_pool = VK_NULL_HANDLE; r->bg_desc = VK_NULL_HANDLE; }
    if (r->bg_sampler) { vkDestroySampler(r->dev, r->bg_sampler, NULL); r->bg_sampler = VK_NULL_HANDLE; }
    if (r->bg_view) { vkDestroyImageView(r->dev, r->bg_view, NULL); r->bg_view = VK_NULL_HANDLE; }
    if (r->bg_image) { vkDestroyImage(r->dev, r->bg_image, NULL); r->bg_image = VK_NULL_HANDLE; }
    if (r->bg_mem) { vkFreeMemory(r->dev, r->bg_mem, NULL); r->bg_mem = VK_NULL_HANDLE; }
    r->bg_w = 0u; r->bg_h = 0u;
}

void vk_render_set_background_style(VkRenderer* r, float dim, float vignette)
{
    if (!r) return;
    r->bg_dim = dim < 0.0f ? 0.0f : (dim > 1.0f ? 1.0f : dim);
    r->bg_vignette = vignette < 0.0f ? 0.0f : (vignette > 1.0f ? 1.0f : vignette);
}

bool vk_render_set_background(VkRenderer* r, const char* path, BackgroundFit fit)
{
    unsigned char* pixels;
    int w = 0, h = 0, n = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_map = NULL;
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo ca;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkImageMemoryBarrier bar;
    VkBufferImageCopy copy;
    VkSamplerCreateInfo sci;
    VkDescriptorPoolSize ps;
    VkDescriptorPoolCreateInfo dp;
    VkDescriptorSetAllocateInfo da;
    VkDescriptorImageInfo img;
    VkWriteDescriptorSet wr;
    VkDeviceSize bytes;

    if (!r) return false;
    if (!path || !path[0] || strcmp(path, "none") == 0)
    {
        vkDeviceWaitIdle(r->dev);
        destroy_bg(r);
        return true;
    }

    pixels = stbi_load(path, &w, &h, &n, 4);
    if (!pixels || w <= 0 || h <= 0)
    {
        fprintf(stderr, "background: cannot load '%s'\n", path);
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    bytes = (VkDeviceSize)w * (VkDeviceSize)h * 4u;

    vkDeviceWaitIdle(r->dev);
    destroy_bg(r);

    if (!make_image(r, (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_SRGB,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    &r->bg_image, &r->bg_mem)) goto fail;
    if (!make_view(r, r->bg_image, VK_FORMAT_R8G8B8A8_SRGB, &r->bg_view)) goto fail;
    if (!make_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &staging, &staging_mem, &staging_map)) goto fail;
    memcpy(staging_map, pixels, (size_t)bytes);
    stbi_image_free(pixels);
    pixels = NULL;

    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = r->pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1u;
    if (vkAllocateCommandBuffers(r->dev, &ca, &cmd) != VK_SUCCESS) goto fail;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) goto fail;

    memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = r->bg_image;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1u;
    bar.subresourceRange.layerCount = 1u;
    bar.srcAccessMask = 0u;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0u, 0u, NULL, 0u, NULL, 1u, &bar);
    memset(&copy, 0, sizeof copy);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1u;
    copy.imageExtent.width = (uint32_t)w;
    copy.imageExtent.height = (uint32_t)h;
    copy.imageExtent.depth = 1u;
    vkCmdCopyBufferToImage(cmd, staging, r->bg_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0u, 0u, NULL, 0u, NULL, 1u, &bar);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) goto fail;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(r->queue, 1u, &si, VK_NULL_HANDLE) != VK_SUCCESS) goto fail;
    vkQueueWaitIdle(r->queue);
    vkFreeCommandBuffers(r->dev, r->pool, 1u, &cmd);
    vkUnmapMemory(r->dev, staging_mem);
    vkDestroyBuffer(r->dev, staging, NULL);
    vkFreeMemory(r->dev, staging_mem, NULL);
    staging = VK_NULL_HANDLE; staging_mem = VK_NULL_HANDLE;

    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.maxLod = 1.0f;
    if (vkCreateSampler(r->dev, &sci, NULL, &r->bg_sampler) != VK_SUCCESS) goto fail;

    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 1u;
    memset(&dp, 0, sizeof dp);
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.poolSizeCount = 1u;
    dp.pPoolSizes = &ps;
    dp.maxSets = 1u;
    if (vkCreateDescriptorPool(r->dev, &dp, NULL, &r->bg_pool) != VK_SUCCESS) goto fail;
    memset(&da, 0, sizeof da);
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = r->bg_pool;
    da.descriptorSetCount = 1u;
    da.pSetLayouts = &r->desc_layout;
    if (vkAllocateDescriptorSets(r->dev, &da, &r->bg_desc) != VK_SUCCESS) goto fail;
    memset(&img, 0, sizeof img);
    img.sampler = r->bg_sampler;
    img.imageView = r->bg_view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = r->bg_desc;
    wr.dstBinding = 0u;
    wr.descriptorCount = 1u;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &img;
    vkUpdateDescriptorSets(r->dev, 1u, &wr, 0u, NULL);

    if (!create_bg_pipeline(r)) goto fail;
    r->bg_w = (uint32_t)w;
    r->bg_h = (uint32_t)h;
    r->bg_fit = fit;
    return true;

fail:
    if (pixels) stbi_image_free(pixels);
    if (staging_map) vkUnmapMemory(r->dev, staging_mem);
    if (staging) vkDestroyBuffer(r->dev, staging, NULL);
    if (staging_mem) vkFreeMemory(r->dev, staging_mem, NULL);
    destroy_bg(r);
    return false;
}

static bool create_frames(VkRenderer* r)
{
    VkCommandPoolCreateInfo pc;
    VkCommandBufferAllocateInfo ca;
    VkFenceCreateInfo fc;
    VkSemaphoreCreateInfo sc;
    uint32_t i;
    memset(&pc, 0, sizeof pc);
    pc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = r->queue_family;
    VK_TRY(vkCreateCommandPool(r->dev, &pc, NULL, &r->pool));
    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = r->pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = QUILL_FRAMES;
    {
        VkCommandBuffer cmds[QUILL_FRAMES];
        VK_TRY(vkAllocateCommandBuffers(r->dev, &ca, cmds));
        for (i = 0u; i < QUILL_FRAMES; ++i)
            r->frames[i].cmd = cmds[i];
    }
    memset(&fc, 0, sizeof fc);
    fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    memset(&sc, 0, sizeof sc);
    sc.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0u; i < QUILL_FRAMES; ++i)
    {
        FrameRes* f = &r->frames[i];
        VK_TRY(vkCreateFence(r->dev, &fc, NULL, &f->fence));
        VK_TRY(vkCreateSemaphore(r->dev, &sc, NULL, &f->acquire));
        if (!make_buffer(r, SOLID_VERT_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &f->solid_buf, &f->solid_mem, &f->solid_map))
            return false;
        if (!make_buffer(r, GLYPH_VERT_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &f->glyph_buf, &f->glyph_mem, &f->glyph_map))
            return false;
    }
    return true;
}

static bool ensure_vfx_buffer(VkRenderer* r, FrameRes* f)
{
    if (f->vfx_buf) return true;
    if (make_buffer(r, VFX_VERT_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &f->vfx_buf, &f->vfx_mem, &f->vfx_map))
        return true;
    if (f->vfx_map) vkUnmapMemory(r->dev, f->vfx_mem);
    if (f->vfx_mem) vkFreeMemory(r->dev, f->vfx_mem, NULL);
    if (f->vfx_buf) vkDestroyBuffer(r->dev, f->vfx_buf, NULL);
    f->vfx_map = NULL;
    f->vfx_mem = VK_NULL_HANDLE;
    f->vfx_buf = VK_NULL_HANDLE;
    return false;
}

static bool create_target_headless(VkRenderer* r)
{
    VkFramebufferCreateInfo fb;
    if (!make_image(r, r->extent.width, r->extent.height, r->format,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    &r->target, &r->target_mem))
        return false;
    if (!make_view(r, r->target, r->format, &r->target_view))
        return false;
    memset(&fb, 0, sizeof fb);
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = r->pass;
    fb.attachmentCount = 1u;
    fb.pAttachments = &r->target_view;
    fb.width = r->extent.width;
    fb.height = r->extent.height;
    fb.layers = 1u;
    VK_TRY(vkCreateFramebuffer(r->dev, &fb, NULL, &r->target_fb));
    if (!make_buffer(r, (VkDeviceSize)r->extent.width * r->extent.height * 4u,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &r->readback, &r->readback_mem, &r->readback_map))
        return false;
    return true;
}

static bool create_swapchain(VkRenderer* r)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[32];
    VkPresentModeKHR modes[16];
    uint32_t nf = 32u, nm = 16u, i, count;
    VkSwapchainCreateInfoKHR sc;
    VkFramebufferCreateInfo fb;
    VkSemaphoreCreateInfo sem;
    VkSurfaceFormatKHR chosen;

    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->phys, r->surface, &caps));
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, r->surface, &nf, formats));
    VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(r->phys, r->surface, &nm, modes));
    chosen = formats[0];
    for (i = 0u; i < nf; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = formats[i];
            break;
        }
    r->format = chosen.format;
    if (caps.currentExtent.width != UINT32_MAX)
        r->extent = caps.currentExtent;
    else
    {
        r->extent.width = r->extent.width < caps.minImageExtent.width ? caps.minImageExtent.width : r->extent.width;
        r->extent.height = r->extent.height < caps.minImageExtent.height ? caps.minImageExtent.height : r->extent.height;
        if (r->extent.width > caps.maxImageExtent.width) r->extent.width = caps.maxImageExtent.width;
        if (r->extent.height > caps.maxImageExtent.height) r->extent.height = caps.maxImageExtent.height;
    }
    /* Keep just two swapchain images when the surface allows it; FIFO still
     * queues presentation, and the smaller image set saves framebuffers. */
    count = 2u;
    if (count < caps.minImageCount) count = caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count < caps.minImageCount) count = caps.minImageCount;
    if (count > 8u) count = 8u;
    memset(&sc, 0, sizeof sc);
    sc.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc.surface = r->surface;
    sc.minImageCount = count;
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = r->extent;
    sc.imageArrayLayers = 1u;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    /* Wayland advertises PRE_MULTIPLIED; use it only when the window really is
     * translucent, otherwise keep OPAQUE (some compositors are slower and X11
     * may not support alpha at all). */
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (r->window_opacity < 1.0f &&
        (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR))
        sc.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    VK_TRY(vkCreateSwapchainKHR(r->dev, &sc, NULL, &r->swapchain));
    VK_TRY(vkGetSwapchainImagesKHR(r->dev, r->swapchain, &r->nimages, NULL));
    if (r->nimages > 8u) r->nimages = 8u;
    VK_TRY(vkGetSwapchainImagesKHR(r->dev, r->swapchain, &r->nimages, r->images));
    memset(&sem, 0, sizeof sem);
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0u; i < r->nimages; ++i)
    {
        if (!make_view(r, r->images[i], r->format, &r->views[i])) return false;
        memset(&fb, 0, sizeof fb);
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = r->pass;
        fb.attachmentCount = 1u;
        fb.pAttachments = &r->views[i];
        fb.width = r->extent.width;
        fb.height = r->extent.height;
        fb.layers = 1u;
        VK_TRY(vkCreateFramebuffer(r->dev, &fb, NULL, &r->fbs[i]));
        VK_TRY(vkCreateSemaphore(r->dev, &sem, NULL, &r->finished[i]));
    }
    return true;
}

static void destroy_target(VkRenderer* r)
{
    uint32_t i;
    if (r->headless)
    {
        if (r->readback_map) vkUnmapMemory(r->dev, r->readback_mem);
        r->readback_map = NULL;
        if (r->readback) vkDestroyBuffer(r->dev, r->readback, NULL);
        if (r->readback_mem) vkFreeMemory(r->dev, r->readback_mem, NULL);
        r->readback = VK_NULL_HANDLE; r->readback_mem = VK_NULL_HANDLE;
        if (r->target_fb) vkDestroyFramebuffer(r->dev, r->target_fb, NULL);
        if (r->target_view) vkDestroyImageView(r->dev, r->target_view, NULL);
        if (r->target) vkDestroyImage(r->dev, r->target, NULL);
        if (r->target_mem) vkFreeMemory(r->dev, r->target_mem, NULL);
        r->target_fb = VK_NULL_HANDLE; r->target_view = VK_NULL_HANDLE;
        r->target = VK_NULL_HANDLE; r->target_mem = VK_NULL_HANDLE;
    }
    else
    {
        for (i = 0u; i < r->nimages; ++i)
        {
            if (r->fbs[i]) vkDestroyFramebuffer(r->dev, r->fbs[i], NULL);
            if (r->views[i]) vkDestroyImageView(r->dev, r->views[i], NULL);
            if (r->finished[i]) vkDestroySemaphore(r->dev, r->finished[i], NULL);
            r->fbs[i] = VK_NULL_HANDLE; r->views[i] = VK_NULL_HANDLE; r->finished[i] = VK_NULL_HANDLE;
        }
        r->nimages = 0u;
        if (r->swapchain) vkDestroySwapchainKHR(r->dev, r->swapchain, NULL);
        r->swapchain = VK_NULL_HANDLE;
    }
}

#if defined(__linux__)
typedef struct DrmVendors
{
    unsigned int ids[16];
    size_t       count;
    bool         all_readable;
} DrmVendors;

static DrmVendors scan_drm_vendors(void)
{
    DrmVendors info;
    DIR* dir;
    struct dirent* ent;

    memset(&info, 0, sizeof info);
    info.all_readable = true;
    dir = opendir("/sys/class/drm");
    if (!dir) { info.all_readable = false; return info; }
    while ((ent = readdir(dir)) != NULL)
    {
        char path[256];
        FILE* f;
        unsigned int v = 0u;
        int n;

        if (strncmp(ent->d_name, "renderD", 7) != 0) continue;
        n = snprintf(path, sizeof path, "/sys/class/drm/%s/device/vendor", ent->d_name);
        if (n < 0 || (size_t)n >= sizeof path) { info.all_readable = false; continue; }
        f = fopen(path, "r");
        if (!f) { info.all_readable = false; continue; }
        if (fscanf(f, "%x", &v) != 1)
            info.all_readable = false;
        else if (info.count < sizeof info.ids / sizeof info.ids[0])
            info.ids[info.count++] = v;
        fclose(f);
    }
    closedir(dir);
    return info;
}

static bool drm_has_vendor(const DrmVendors* info, unsigned int vendor)
{
    size_t i;
    for (i = 0; i < info->count; ++i)
        if (info->ids[i] == vendor) return true;
    return false;
}

static bool drm_render_node_present(void)
{
    DIR* dir = opendir("/dev/dri");
    bool found = false;
    struct dirent* ent;

    if (!dir) return false;
    while ((ent = readdir(dir)) != NULL)
    {
        char path[256];
        int n;

        if (strncmp(ent->d_name, "renderD", 7) != 0) continue;
        n = snprintf(path, sizeof path, "/dev/dri/%s", ent->d_name);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (access(path, R_OK | W_OK) == 0) { found = true; break; }
    }
    closedir(dir);
    return found;
}

static void append_driver_pattern(char* buf, size_t cap, const char* pattern)
{
    size_t len = strlen(buf);
    if (len < cap)
        snprintf(buf + len, cap - len, "%s%s", len ? "," : "", pattern);
}

/* The Vulkan loader eagerly loads every installed ICD. On Linux this can
 * pull in Mesa's lavapipe (libLLVM) and drivers for GPUs that are not
 * present, costing well over 100 MiB before the first frame.  Keep the
 * hardware drivers that match the DRM devices we actually have.  Explicit
 * user driver selection always wins. */
static void filter_unneeded_vulkan_drivers(void)
{
    char disable[192] = "";
    DrmVendors vendors;

    if (getenv("VK_LOADER_DRIVERS_DISABLE") ||
        getenv("VK_LOADER_DRIVERS_SELECT") ||
        getenv("VK_DRIVER_FILES") ||
        getenv("VK_ICD_FILENAMES"))
        return;
    if (!drm_render_node_present()) return;

    append_driver_pattern(disable, sizeof disable, "*lvp*");
    append_driver_pattern(disable, sizeof disable, "*lavapipe*");
    append_driver_pattern(disable, sizeof disable, "*swiftshader*");

    vendors = scan_drm_vendors();
    if (vendors.all_readable && vendors.count > 0u)
    {
        if (!drm_has_vendor(&vendors, 0x1002)) append_driver_pattern(disable, sizeof disable, "*radeon*");
        if (!drm_has_vendor(&vendors, 0x10de)) append_driver_pattern(disable, sizeof disable, "*nouveau*");
        if (!drm_has_vendor(&vendors, 0x8086)) append_driver_pattern(disable, sizeof disable, "*intel*");
    }
    setenv("VK_LOADER_DRIVERS_DISABLE", disable, 1);
}
#endif

VkRenderer* vk_render_create(const VkRenderCreateInfo* info)
{
    VkRenderer* r;
    VkApplicationInfo app;
    VkInstanceCreateInfo ic;
    const char* layers[1];
    const char* exts[16];
    uint32_t next = 0u, i, ndev = 0u;
    VkPhysicalDevice devs[16];
    VkDeviceQueueCreateInfo qci;
    float prio = 1.0f;
    VkDeviceCreateInfo dc;
    const char* dev_exts[2];
    VkDebugUtilsMessengerCreateInfoEXT dbg;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures feats;
    VkQueueFamilyProperties qprops[16];
    bool have_validation = false;
    uint32_t nlayers = 0u;
    VkLayerProperties layer_props[32];

    r = (VkRenderer*)MU_CALLOC(1u, sizeof *r);
    if (!r) return NULL;
    r->headless = info->headless || !info->window;
    r->extent.width = info->width ? info->width : 1280u;
    r->extent.height = info->height ? info->height : 800u;
    r->format = VK_FORMAT_B8G8R8A8_SRGB;
    /* Below 0.2 the editor would be a barely visible ghost, so clamp there:
     * the feature is for a translucent editor, not an invisible one. */
    r->window_opacity = info->window_opacity > 0.0f ? info->window_opacity : 1.0f;
    if (r->window_opacity < 0.2f) r->window_opacity = 0.2f;
    if (r->window_opacity > 1.0f) r->window_opacity = 1.0f;

#if defined(__linux__)
    filter_unneeded_vulkan_drivers();
#endif

    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Quill";
    app.apiVersion = VK_API_VERSION_1_2;
    memset(&ic, 0, sizeof ic);
    ic.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ic.pApplicationInfo = &app;

    if (info->validation)
    {
        vkEnumerateInstanceLayerProperties(&nlayers, NULL);
        if (nlayers > 32u) nlayers = 32u;
        vkEnumerateInstanceLayerProperties(&nlayers, layer_props);
        for (i = 0u; i < nlayers; ++i)
            if (strcmp(layer_props[i].layerName, "VK_LAYER_KHRONOS_validation") == 0)
                have_validation = true;
        if (have_validation)
        {
            layers[0] = "VK_LAYER_KHRONOS_validation";
            ic.enabledLayerCount = 1u;
            ic.ppEnabledLayerNames = layers;
            exts[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        }
    }
    if (!r->headless)
    {
        uint32_t ng = 0u;
        const char** ge = glfwGetRequiredInstanceExtensions(&ng);
        for (i = 0u; i < ng && next < 16u; ++i)
            exts[next++] = ge[i];
    }
    ic.enabledExtensionCount = next;
    ic.ppEnabledExtensionNames = exts;
    if (vkCreateInstance(&ic, NULL, &r->instance) != VK_SUCCESS)
        goto fail;

    if (have_validation)
    {
        PFN_vkCreateDebugUtilsMessengerEXT fn =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r->instance, "vkCreateDebugUtilsMessengerEXT");
        memset(&dbg, 0, sizeof dbg);
        dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback = debug_cb;
        if (fn) fn(r->instance, &dbg, NULL, &r->debug);
    }

    if (!r->headless)
    {
        if (glfwCreateWindowSurface(r->instance, info->window, NULL, &r->surface) != VK_SUCCESS)
            goto fail;
    }

    vkEnumeratePhysicalDevices(r->instance, &ndev, NULL);
    if (ndev == 0u) goto fail;
    if (ndev > 16u) ndev = 16u;
    vkEnumeratePhysicalDevices(r->instance, &ndev, devs);
    for (i = 0u; i < ndev; ++i)
    {
        uint32_t j, nqq = 16u;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nqq, qprops);
        for (j = 0u; j < nqq; ++j)
        {
            VkBool32 present = VK_TRUE;
            if (!(qprops[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!r->headless)
                vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], j, r->surface, &present);
            if (present)
            {
                r->phys = devs[i];
                r->queue_family = j;
                break;
            }
        }
        if (r->phys) break;
    }
    if (!r->phys) goto fail;
    vkGetPhysicalDeviceProperties(r->phys, &props);
    snprintf(r->device_name, sizeof r->device_name, "%s", props.deviceName);
    memset(&feats, 0, sizeof feats);

    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = r->queue_family;
    qci.queueCount = 1u;
    qci.pQueuePriorities = &prio;
    memset(&dc, 0, sizeof dc);
    dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dc.queueCreateInfoCount = 1u;
    dc.pQueueCreateInfos = &qci;
    dc.pEnabledFeatures = &feats;
    if (!r->headless)
    {
        dev_exts[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        dc.enabledExtensionCount = 1u;
        dc.ppEnabledExtensionNames = dev_exts;
    }
    if (vkCreateDevice(r->phys, &dc, NULL, &r->dev) != VK_SUCCESS)
        goto fail;
    vkGetDeviceQueue(r->dev, r->queue_family, 0u, &r->queue);

    if (!create_pass(r)) goto fail;
    if (!create_atlas(r)) goto fail;
    if (!create_pipelines(r)) goto fail;
    if (!create_frames(r)) goto fail;
    if (r->headless)
    {
        if (!create_target_headless(r)) goto fail;
    }
    else
    {
        VkFormat initial = r->format;
        if (!create_swapchain(r)) goto fail;
        if (r->format != initial)
        {
            vkDestroyPipeline(r->dev, r->solid_pipe, NULL);
            vkDestroyPipeline(r->dev, r->glyph_pipe, NULL);
            vkDestroyRenderPass(r->dev, r->pass, NULL);
            r->solid_pipe = VK_NULL_HANDLE;
            r->glyph_pipe = VK_NULL_HANDLE;
            r->pass = VK_NULL_HANDLE;
            if (!create_pass(r)) goto fail;
            if (!create_pipelines(r)) goto fail;
        }
    }
    quill_trim_allocator();
    return r;

fail:
    vk_render_destroy(r);
    return NULL;
}

void vk_render_destroy(VkRenderer* r)
{
    uint32_t i;
    if (!r) return;
    if (r->dev) vkDeviceWaitIdle(r->dev);
    destroy_target(r);
    for (i = 0u; i < QUILL_FRAMES; ++i)
    {
        FrameRes* f = &r->frames[i];
        if (f->solid_map) vkUnmapMemory(r->dev, f->solid_mem);
        if (f->glyph_map) vkUnmapMemory(r->dev, f->glyph_mem);
        if (f->vfx_map) vkUnmapMemory(r->dev, f->vfx_mem);
        if (f->solid_buf) vkDestroyBuffer(r->dev, f->solid_buf, NULL);
        if (f->solid_mem) vkFreeMemory(r->dev, f->solid_mem, NULL);
        if (f->glyph_buf) vkDestroyBuffer(r->dev, f->glyph_buf, NULL);
        if (f->glyph_mem) vkFreeMemory(r->dev, f->glyph_mem, NULL);
        if (f->vfx_buf) vkDestroyBuffer(r->dev, f->vfx_buf, NULL);
        if (f->vfx_mem) vkFreeMemory(r->dev, f->vfx_mem, NULL);
        if (f->fence) vkDestroyFence(r->dev, f->fence, NULL);
        if (f->acquire) vkDestroySemaphore(r->dev, f->acquire, NULL);
    }
    if (r->pool) vkDestroyCommandPool(r->dev, r->pool, NULL);
    if (r->solid_pipe) vkDestroyPipeline(r->dev, r->solid_pipe, NULL);
    if (r->glyph_pipe) vkDestroyPipeline(r->dev, r->glyph_pipe, NULL);
    if (r->vfx_pipe) vkDestroyPipeline(r->dev, r->vfx_pipe, NULL);
    if (r->solid_layout) vkDestroyPipelineLayout(r->dev, r->solid_layout, NULL);
    if (r->glyph_layout) vkDestroyPipelineLayout(r->dev, r->glyph_layout, NULL);
    if (r->vfx_layout) vkDestroyPipelineLayout(r->dev, r->vfx_layout, NULL);
    destroy_bg(r);
    if (r->sampler) vkDestroySampler(r->dev, r->sampler, NULL);
    if (r->atlas_view) vkDestroyImageView(r->dev, r->atlas_view, NULL);
    if (r->atlas) vkDestroyImage(r->dev, r->atlas, NULL);
    if (r->atlas_mem) vkFreeMemory(r->dev, r->atlas_mem, NULL);
    if (r->desc_pool) vkDestroyDescriptorPool(r->dev, r->desc_pool, NULL);
    if (r->desc_layout) vkDestroyDescriptorSetLayout(r->dev, r->desc_layout, NULL);
    if (r->pass) vkDestroyRenderPass(r->dev, r->pass, NULL);
    if (r->dev) vkDestroyDevice(r->dev, NULL);
    if (r->surface) vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->debug)
    {
        PFN_vkDestroyDebugUtilsMessengerEXT fn =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(r->instance, r->debug, NULL);
    }
    if (r->instance) vkDestroyInstance(r->instance, NULL);
    MU_FREE(r);
}

bool vk_render_resize(VkRenderer* r, uint32_t width, uint32_t height)
{
    if (!r || width == 0u || height == 0u) return false;
    vkDeviceWaitIdle(r->dev);
    destroy_target(r);
    r->extent.width = width;
    r->extent.height = height;
    if (r->headless)
        return create_target_headless(r);
    return create_swapchain(r);
}

static void write_vertices(FrameRes* f, const DrawList* dl)
{
    SolidVert* sv = (SolidVert*)f->solid_map;
    GlyphVert* gv = (GlyphVert*)f->glyph_map;
    uint32_t i;
    for (i = 0u; i < dl->nsolids; ++i)
    {
        const SolidQuad* q = &dl->solids[i];
        SolidVert* v = sv + (size_t)i * 6u;
        float x0 = q->x, y0 = q->y, x1 = q->x + q->w, y1 = q->y + q->h;
        v[0].pos[0] = x0; v[0].pos[1] = y0; v[0].uv[0] = 0.0f; v[0].uv[1] = 0.0f;
        v[1].pos[0] = x1; v[1].pos[1] = y0; v[1].uv[0] = 1.0f; v[1].uv[1] = 0.0f;
        v[2].pos[0] = x0; v[2].pos[1] = y1; v[2].uv[0] = 0.0f; v[2].uv[1] = 1.0f;
        v[3].pos[0] = x1; v[3].pos[1] = y0; v[3].uv[0] = 1.0f; v[3].uv[1] = 0.0f;
        v[4].pos[0] = x0; v[4].pos[1] = y1; v[4].uv[0] = 0.0f; v[4].uv[1] = 1.0f;
        v[5].pos[0] = x1; v[5].pos[1] = y1; v[5].uv[0] = 1.0f; v[5].uv[1] = 1.0f;
        for (uint32_t k = 0u; k < 6u; ++k)
        {
            v[k].color[0] = q->r; v[k].color[1] = q->g;
            v[k].color[2] = q->b; v[k].color[3] = q->a;
            v[k].size[0] = q->w; v[k].size[1] = q->h;
            v[k].radius = q->radius; v[k].pad = 0.0f;
        }
    }
    for (i = 0u; f->vfx_buf && i < dl->nvfx; ++i)
    {
        const VfxQuad* q = &dl->vfx[i];
        VfxVert* v = (VfxVert*)f->vfx_map + (size_t)i * 6u;
        float x0 = q->x, y0 = q->y, x1 = q->x + q->w, y1 = q->y + q->h;
        v[0].pos[0] = x0; v[0].pos[1] = y0; v[0].uv[0] = 0.0f; v[0].uv[1] = 0.0f;
        v[1].pos[0] = x1; v[1].pos[1] = y0; v[1].uv[0] = 1.0f; v[1].uv[1] = 0.0f;
        v[2].pos[0] = x0; v[2].pos[1] = y1; v[2].uv[0] = 0.0f; v[2].uv[1] = 1.0f;
        v[3].pos[0] = x1; v[3].pos[1] = y0; v[3].uv[0] = 1.0f; v[3].uv[1] = 0.0f;
        v[4].pos[0] = x0; v[4].pos[1] = y1; v[4].uv[0] = 0.0f; v[4].uv[1] = 1.0f;
        v[5].pos[0] = x1; v[5].pos[1] = y1; v[5].uv[0] = 1.0f; v[5].uv[1] = 1.0f;
        for (uint32_t k = 0u; k < 6u; ++k)
        {
            v[k].color[0] = q->r; v[k].color[1] = q->g;
            v[k].color[2] = q->b; v[k].color[3] = q->a;
            v[k].params[0] = q->shape; v[k].params[1] = q->stroke;
            v[k].params[2] = q->soft; v[k].params[3] = q->pad;
            v[k].size[0] = q->w; v[k].size[1] = q->h;
        }
    }
    for (i = 0u; i < dl->nglyphs; ++i)
    {
        const GlyphQuad* q = &dl->glyphs[i];
        GlyphVert* v = gv + (size_t)i * 6u;
        float x0 = q->x, y0 = q->y, x1 = q->x + q->w, y1 = q->y + q->h;
        v[0].pos[0] = x0; v[0].pos[1] = y0; v[0].uv[0] = q->u0; v[0].uv[1] = q->v0;
        v[1].pos[0] = x1; v[1].pos[1] = y0; v[1].uv[0] = q->u1; v[1].uv[1] = q->v0;
        v[2].pos[0] = x0; v[2].pos[1] = y1; v[2].uv[0] = q->u0; v[2].uv[1] = q->v1;
        v[3].pos[0] = x1; v[3].pos[1] = y0; v[3].uv[0] = q->u1; v[3].uv[1] = q->v0;
        v[4].pos[0] = x0; v[4].pos[1] = y1; v[4].uv[0] = q->u0; v[4].uv[1] = q->v1;
        v[5].pos[0] = x1; v[5].pos[1] = y1; v[5].uv[0] = q->u1; v[5].uv[1] = q->v1;
        for (uint32_t k = 0u; k < 6u; ++k)
        {
            v[k].color[0] = q->r; v[k].color[1] = q->g;
            v[k].color[2] = q->b; v[k].color[3] = q->a;
        }
    }
    /* Overlay layer: appended after the base layer in BOTH buffers so the
     * overlay's solids and glyphs are drawn in a later batch and composite on
     * top of every base-layer primitive. */
    for (i = 0u; i < dl->nov_solids; ++i)
    {
        const SolidQuad* q = &dl->ov_solids[i];
        SolidVert* v = sv + (size_t)(dl->nsolids + i) * 6u;
        float x0 = q->x, y0 = q->y, x1 = q->x + q->w, y1 = q->y + q->h;
        v[0].pos[0] = x0; v[0].pos[1] = y0; v[0].uv[0] = 0.0f; v[0].uv[1] = 0.0f;
        v[1].pos[0] = x1; v[1].pos[1] = y0; v[1].uv[0] = 1.0f; v[1].uv[1] = 0.0f;
        v[2].pos[0] = x0; v[2].pos[1] = y1; v[2].uv[0] = 0.0f; v[2].uv[1] = 1.0f;
        v[3].pos[0] = x1; v[3].pos[1] = y0; v[3].uv[0] = 1.0f; v[3].uv[1] = 0.0f;
        v[4].pos[0] = x0; v[4].pos[1] = y1; v[4].uv[0] = 0.0f; v[4].uv[1] = 1.0f;
        v[5].pos[0] = x1; v[5].pos[1] = y1; v[5].uv[0] = 1.0f; v[5].uv[1] = 1.0f;
        for (uint32_t k = 0u; k < 6u; ++k)
        {
            v[k].color[0] = q->r; v[k].color[1] = q->g;
            v[k].color[2] = q->b; v[k].color[3] = q->a;
            v[k].size[0] = q->w; v[k].size[1] = q->h;
            v[k].radius = q->radius; v[k].pad = 0.0f;
        }
    }
    for (i = 0u; i < dl->nov_glyphs; ++i)
    {
        const GlyphQuad* q = &dl->ov_glyphs[i];
        GlyphVert* v = gv + (size_t)(dl->nglyphs + i) * 6u;
        float x0 = q->x, y0 = q->y, x1 = q->x + q->w, y1 = q->y + q->h;
        v[0].pos[0] = x0; v[0].pos[1] = y0; v[0].uv[0] = q->u0; v[0].uv[1] = q->v0;
        v[1].pos[0] = x1; v[1].pos[1] = y0; v[1].uv[0] = q->u1; v[1].uv[1] = q->v0;
        v[2].pos[0] = x0; v[2].pos[1] = y1; v[2].uv[0] = q->u0; v[2].uv[1] = q->v1;
        v[3].pos[0] = x1; v[3].pos[1] = y0; v[3].uv[0] = q->u1; v[3].uv[1] = q->v0;
        v[4].pos[0] = x0; v[4].pos[1] = y1; v[4].uv[0] = q->u0; v[4].uv[1] = q->v1;
        v[5].pos[0] = x1; v[5].pos[1] = y1; v[5].uv[0] = q->u1; v[5].uv[1] = q->v1;
        for (uint32_t k = 0u; k < 6u; ++k)
        {
            v[k].color[0] = q->r; v[k].color[1] = q->g;
            v[k].color[2] = q->b; v[k].color[3] = q->a;
        }
    }

}

static bool ensure_atlas_image(VkRenderer* r, FontSystem* fs)
{
    uint32_t size = font_atlas_width(fs);
    VkDescriptorImageInfo img;
    VkWriteDescriptorSet wr;

    if (r->atlas_size == size) return true;
    if (!r->desc) return false;
    vkDeviceWaitIdle(r->dev);
    if (r->atlas_view) vkDestroyImageView(r->dev, r->atlas_view, NULL);
    if (r->atlas) vkDestroyImage(r->dev, r->atlas, NULL);
    if (r->atlas_mem) vkFreeMemory(r->dev, r->atlas_mem, NULL);
    r->atlas_view = VK_NULL_HANDLE;
    r->atlas = VK_NULL_HANDLE;
    r->atlas_mem = VK_NULL_HANDLE;
    r->atlas_size = 0u;
    if (!make_image(r, size, size, VK_FORMAT_R8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    &r->atlas, &r->atlas_mem))
        return false;
    if (!make_view(r, r->atlas, VK_FORMAT_R8_UNORM, &r->atlas_view))
        return false;
    memset(&img, 0, sizeof img);
    img.sampler = r->sampler;
    img.imageView = r->atlas_view;
    img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = r->desc;
    wr.dstBinding = 0u;
    wr.descriptorCount = 1u;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &img;
    vkUpdateDescriptorSets(r->dev, 1u, &wr, 0u, NULL);
    r->atlas_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    r->atlas_size = size;
    return true;
}

static bool flush_atlas_batch(VkRenderer* r, FrameRes* f, VkBuffer staging,
                              const VkBufferImageCopy* copies, uint32_t nc)
{
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkImageMemoryBarrier bar;
    VkResult res;

    if (nc == 0u) return true;
    if (vkResetCommandBuffer(f->cmd, 0u) != VK_SUCCESS) return false;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(f->cmd, &bi) != VK_SUCCESS) return false;

    memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = r->atlas_layout;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = r->atlas;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1u;
    bar.subresourceRange.layerCount = 1u;
    bar.srcAccessMask = r->atlas_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_SHADER_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(f->cmd,
        r->atlas_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, NULL, 0u, NULL, 1u, &bar);
    vkCmdCopyBufferToImage(f->cmd, staging, r->atlas, VK_IMAGE_LAYOUT_GENERAL, nc, copies);
    memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = r->atlas;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1u;
    bar.subresourceRange.layerCount = 1u;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0u, 0u, NULL, 0u, NULL, 1u, &bar);
    if (vkEndCommandBuffer(f->cmd) != VK_SUCCESS) return false;

    vkResetFences(r->dev, 1u, &f->fence);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &f->cmd;
    res = vkQueueSubmit(r->queue, 1u, &si, f->fence);
    if (res != VK_SUCCESS) return false;
    res = vkWaitForFences(r->dev, 1u, &f->fence, VK_TRUE, UINT64_MAX);
    if (res != VK_SUCCESS) return false;
    r->atlas_layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

static bool upload_atlas(VkRenderer* r, FrameRes* f, FontSystem* fs)
{
    uint32_t nd = font_atlas_dirty_count(fs);
    const DirtyRect* dr = font_atlas_dirty(fs);
    uint32_t aw = font_atlas_width(fs);
    const uint8_t* px = font_atlas_pixels(fs);
    uint32_t i;
    VkDeviceSize off = 0u;
    VkDeviceSize stage_size = 0u;
    VkBufferImageCopy copies[512];
    uint32_t nc = 0u;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_map = NULL;
    bool ok = false;

    if (nd == 0u) return true;
    for (i = 0u; i < nd; ++i)
    {
        if (dr[i].w == 0u || dr[i].h == 0u || dr[i].w > STAGING_SIZE) return false;
        stage_size += (VkDeviceSize)dr[i].w * dr[i].h;
        stage_size = (stage_size + 3u) & ~(VkDeviceSize)3u;
    }
    if (stage_size > STAGING_SIZE) stage_size = STAGING_SIZE;
    /* Font changes and atlas overflows are rare.  Allocate a staging buffer
     * only for this upload and release it afterwards instead of reserving
     * memory for every frame. */
    if (!make_buffer(r, stage_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &staging, &staging_mem, &staging_map))
        goto done;

    for (i = 0u; i < nd; ++i)
    {
        uint32_t x = dr[i].x, y = dr[i].y, w = dr[i].w, h = dr[i].h;
        uint32_t row = 0u;

        while (row < h)
        {
            uint32_t rows, rr;
            uint8_t* dst;

            if (off >= stage_size || nc >= (uint32_t)(sizeof copies / sizeof copies[0]))
            {
                if (!flush_atlas_batch(r, f, staging, copies, nc)) goto done;
                off = 0u;
                nc = 0u;
            }
            rows = (uint32_t)((stage_size - off) / w);
            if (rows == 0u)
            {
                if (nc == 0u || !flush_atlas_batch(r, f, staging, copies, nc)) goto done;
                off = 0u;
                nc = 0u;
                continue;
            }
            if (rows > h - row) rows = h - row;
            dst = (uint8_t*)staging_map + off;
            for (rr = 0u; rr < rows; ++rr)
                memcpy(dst + (size_t)rr * w,
                       px + (size_t)(y + row + rr) * aw + x, w);
            memset(&copies[nc], 0, sizeof copies[nc]);
            copies[nc].bufferOffset = off;
            copies[nc].bufferRowLength = w;
            copies[nc].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copies[nc].imageSubresource.layerCount = 1u;
            copies[nc].imageOffset.x = (int32_t)x;
            copies[nc].imageOffset.y = (int32_t)(y + row);
            copies[nc].imageExtent.width = w;
            copies[nc].imageExtent.height = rows;
            copies[nc].imageExtent.depth = 1u;
            nc++;
            off = (off + (VkDeviceSize)w * rows + 3u) & ~(VkDeviceSize)3u;
            row += rows;
        }
    }
    if (!flush_atlas_batch(r, f, staging, copies, nc)) goto done;
    font_atlas_clear_dirty(fs);
    ok = true;

done:
    if (staging_map) vkUnmapMemory(r->dev, staging_mem);
    if (staging) vkDestroyBuffer(r->dev, staging, NULL);
    if (staging_mem) vkFreeMemory(r->dev, staging_mem, NULL);
    return ok;
}

bool vk_render_frame(VkRenderer* r, const DrawList* dl, FontSystem* fs)
{
    FrameRes* f;
    uint32_t image_index = 0u;
    VkCommandBufferBeginInfo bi;
    VkClearValue clear;
    VkRenderPassBeginInfo rp;
    VkViewport vp;
    VkRect2D sc;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkImageMemoryBarrier bar;
    VkBufferImageCopy copy;
    struct { float screen[2]; float window; float pad; } spc;
    struct { float screen[2]; float gamma; float contrast; float window; float pad; } gpc;
    struct { float uv_scale[2]; float uv_offset[2]; float opacity;
             float dim; float vignette; float window; } bgpc;
    float bg[4];
    VkFramebuffer fb;
    VkResult res;

    if (!r || r->extent.width == 0u || r->extent.height == 0u) return false;
    f = &r->frames[r->frame_index];
    vkWaitForFences(r->dev, 1u, &f->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(r->dev, 1u, &f->fence);

    if (!r->headless)
    {
        res = vkAcquireNextImageKHR(r->dev, r->swapchain, UINT64_MAX, f->acquire, VK_NULL_HANDLE, &image_index);
        if (res == VK_ERROR_OUT_OF_DATE_KHR)
        {
            vk_render_resize(r, r->extent.width, r->extent.height);
            return false;
        }
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            return false;
    }

    if (!ensure_atlas_image(r, fs)) return false;
    if (dl->nvfx) (void)ensure_vfx_buffer(r, f);
    write_vertices(f, dl);
    if (!upload_atlas(r, f, fs)) return false;
    vkResetCommandBuffer(f->cmd, 0u);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(f->cmd, &bi) != VK_SUCCESS) return false;

    quill_color_linear(QUILL_THEME.bg, bg);
    memset(&clear, 0, sizeof clear);
    /* Premultiplied clear so a translucent window starts at exactly
     * window_opacity; opaque windows get the same values as before. */
    clear.color.float32[3] = bg[3] * r->window_opacity;
    clear.color.float32[0] = bg[0] * clear.color.float32[3];
    clear.color.float32[1] = bg[1] * clear.color.float32[3];
    clear.color.float32[2] = bg[2] * clear.color.float32[3];
    fb = r->headless ? r->target_fb : r->fbs[image_index];
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = r->pass;
    rp.framebuffer = fb;
    rp.renderArea.extent = r->extent;
    rp.clearValueCount = 1u;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(f->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)r->extent.width;
    vp.height = (float)r->extent.height;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    sc.offset.x = 0; sc.offset.y = 0; sc.extent = r->extent;
    vkCmdSetViewport(f->cmd, 0u, 1u, &vp);
    vkCmdSetScissor(f->cmd, 0u, 1u, &sc);

    spc.screen[0] = (float)r->extent.width;
    spc.screen[1] = (float)r->extent.height;
    spc.window = r->window_opacity;
    spc.pad = 0.0f;
    gpc.screen[0] = spc.screen[0];
    gpc.screen[1] = spc.screen[1];
    gpc.gamma = 1.0f;
    gpc.contrast = 0.0f;
    gpc.window = r->window_opacity;
    gpc.pad = 0.0f;

    /* Optional background image, drawn fullscreen beneath the chrome. When
     * no image is loaded bg_pipe is NULL and this whole block is skipped. */
    if (r->bg_pipe)
    {
        float sw = (float)r->extent.width, sh = (float)r->extent.height;
        float iw = (float)r->bg_w, ih = (float)r->bg_h;
        float sa = sw / sh, ia = iw / ih, s;
        if (r->bg_fit == BG_FIT_STRETCH)
        {
            bgpc.uv_scale[0] = 1.0f; bgpc.uv_scale[1] = 1.0f;
        }
        else if (r->bg_fit == BG_FIT_CONTAIN)
        {
            s = ia > sa ? ia / sa : sa / ia;
            if (ia > sa) { bgpc.uv_scale[0] = 1.0f; bgpc.uv_scale[1] = s; }
            else         { bgpc.uv_scale[0] = s;    bgpc.uv_scale[1] = 1.0f; }
        }
        else /* BG_FIT_COVER */
        {
            s = ia > sa ? sa / ia : ia / sa;
            if (ia > sa) { bgpc.uv_scale[0] = s;    bgpc.uv_scale[1] = 1.0f; }
            else         { bgpc.uv_scale[0] = 1.0f; bgpc.uv_scale[1] = s; }
        }
        bgpc.uv_offset[0] = (1.0f - bgpc.uv_scale[0]) * 0.5f;
        bgpc.uv_offset[1] = (1.0f - bgpc.uv_scale[1]) * 0.5f;
        bgpc.opacity = 1.0f;
        bgpc.dim = r->bg_dim;
        bgpc.vignette = r->bg_vignette;
        bgpc.window = r->window_opacity;
        vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->bg_pipe);
        vkCmdPushConstants(f->cmd, r->bg_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0u, 32u, &bgpc);
        vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->bg_layout,
                                0u, 1u, &r->bg_desc, 0u, NULL);
        vkCmdDraw(f->cmd, 3u, 1u, 0u, 0u);
    }

    if (dl->nsolids)
    {
        VkDeviceSize offset = 0u;
        vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->solid_pipe);
        vkCmdPushConstants(f->cmd, r->solid_layout, VK_SHADER_STAGE_VERTEX_BIT, 0u, 16u, &spc);
        vkCmdBindVertexBuffers(f->cmd, 0u, 1u, &f->solid_buf, &offset);
        vkCmdDraw(f->cmd, dl->nsolids * 6u, 1u, 0u, 0u);
    }
    if (dl->nglyphs)
    {
        VkDeviceSize offset = 0u;
        vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->glyph_pipe);
        vkCmdPushConstants(f->cmd, r->glyph_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0u, 24u, &gpc);
        vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->glyph_layout,
                                0u, 1u, &r->desc, 0u, NULL);
        vkCmdBindVertexBuffers(f->cmd, 0u, 1u, &f->glyph_buf, &offset);
        vkCmdDraw(f->cmd, dl->nglyphs * 6u, 1u, 0u, 0u);
    }
    if (dl->nvfx && f->vfx_buf)
    {
        VkDeviceSize offset = 0u;
        vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->vfx_pipe);
        vkCmdPushConstants(f->cmd, r->vfx_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0u, 16u, &spc);
        vkCmdBindVertexBuffers(f->cmd, 0u, 1u, &f->vfx_buf, &offset);
        vkCmdDraw(f->cmd, dl->nvfx * 6u, 1u, 0u, 0u);
    }
    /* Overlay pass: its vertices live after the base layer in the same two
     * rings, so draw them as a separate batch with a first-vertex offset. This
     * is what keeps the picker/search panel above the editor text instead of
     * underneath it. */
    if (dl->nov_solids)
    {
        VkDeviceSize off = (VkDeviceSize)dl->nsolids * 6u * sizeof(SolidVert);
        vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->solid_pipe);
        vkCmdPushConstants(f->cmd, r->solid_layout, VK_SHADER_STAGE_VERTEX_BIT, 0u, 16u, &spc);
        vkCmdBindVertexBuffers(f->cmd, 0u, 1u, &f->solid_buf, &off);
        vkCmdDraw(f->cmd, dl->nov_solids * 6u, 1u, 0u, 0u);
    }
    if (dl->nov_glyphs)
    {
        VkDeviceSize off = (VkDeviceSize)dl->nglyphs * 6u * sizeof(GlyphVert);
        vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->glyph_pipe);
        vkCmdPushConstants(f->cmd, r->glyph_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0u, 24u, &gpc);
        vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->glyph_layout,
                                0u, 1u, &r->desc, 0u, NULL);
        vkCmdBindVertexBuffers(f->cmd, 0u, 1u, &f->glyph_buf, &off);
        vkCmdDraw(f->cmd, dl->nov_glyphs * 6u, 1u, 0u, 0u);
    }
    vkCmdEndRenderPass(f->cmd);

    memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1u;
    bar.subresourceRange.layerCount = 1u;
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (r->headless)
    {
        bar.image = r->target;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &bar);
        memset(&copy, 0, sizeof copy);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1u;
        copy.imageExtent.width = r->extent.width;
        copy.imageExtent.height = r->extent.height;
        copy.imageExtent.depth = 1u;
        vkCmdCopyImageToBuffer(f->cmd, r->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               r->readback, 1u, &copy);
    }
    else
    {
        bar.image = r->images[image_index];
        bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        bar.dstAccessMask = 0u;
        vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &bar);
    }
    if (vkEndCommandBuffer(f->cmd) != VK_SUCCESS) return false;

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &f->cmd;
    if (!r->headless)
    {
        si.waitSemaphoreCount = 1u;
        si.pWaitSemaphores = &f->acquire;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1u;
        si.pSignalSemaphores = &r->finished[image_index];
    }
    vkResetFences(r->dev, 1u, &f->fence);
    if (vkQueueSubmit(r->queue, 1u, &si, f->fence) != VK_SUCCESS) return false;

    if (r->headless)
    {
        vkWaitForFences(r->dev, 1u, &f->fence, VK_TRUE, UINT64_MAX);
    }
    else
    {
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1u;
        pi.pWaitSemaphores = &r->finished[image_index];
        pi.swapchainCount = 1u;
        pi.pSwapchains = &r->swapchain;
        pi.pImageIndices = &image_index;
        res = vkQueuePresentKHR(r->queue, &pi);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
            vk_render_resize(r, r->extent.width, r->extent.height);
        else if (res != VK_SUCCESS)
            return false;
    }
    if (!r->first_frame_trimmed)
    {
        quill_trim_allocator();
        r->first_frame_trimmed = true;
    }
    r->frame_index = (r->frame_index + 1u) % QUILL_FRAMES;
    return true;
}

bool vk_render_screenshot(VkRenderer* r, const char* path)
{
    uint32_t y;
    uint8_t* pixels;
    int ok;
    if (!r || !r->headless || !r->readback_map) return false;
    pixels = (uint8_t*)MU_MALLOC((size_t)r->extent.width * r->extent.height * 4u);
    if (!pixels) return false;
    for (y = 0u; y < r->extent.height; ++y)
    {
        const uint8_t* src = (const uint8_t*)r->readback_map + (size_t)y * r->extent.width * 4u;
        uint8_t* dst = pixels + (size_t)y * r->extent.width * 4u;
        uint32_t x;
        for (x = 0u; x < r->extent.width; ++x)
        {
            dst[x * 4u + 0u] = src[x * 4u + 2u];
            dst[x * 4u + 1u] = src[x * 4u + 1u];
            dst[x * 4u + 2u] = src[x * 4u + 0u];
            dst[x * 4u + 3u] = src[x * 4u + 3u];
        }
    }
    ok = stbi_write_png(path, (int)r->extent.width, (int)r->extent.height, 4, pixels,
                        (int)(r->extent.width * 4u));
    MU_FREE(pixels);
    return ok != 0;
}

uint32_t vk_render_width(const VkRenderer* r) { return r->extent.width; }
float vk_render_window_opacity(const VkRenderer* r) { return r ? r->window_opacity : 1.0f; }
uint32_t vk_render_height(const VkRenderer* r) { return r->extent.height; }
const char* vk_render_device_name(const VkRenderer* r) { return r->device_name; }

#ifndef QUILL_RENDER_VK_H
#define QUILL_RENDER_VK_H

#include "frame.h"
#include <vulkan/vulkan.h>

struct GLFWwindow;

typedef struct VkRenderCreateInfo
{
    struct GLFWwindow* window;   /* NULL for headless */
    uint32_t width, height;
    bool     headless;
    bool     validation;
} VkRenderCreateInfo;

typedef struct VkRenderer VkRenderer;

VkRenderer* vk_render_create(const VkRenderCreateInfo* info);
void vk_render_destroy(VkRenderer* r);
/* Optional background image. NULL/empty/"none" disables and frees the
 * texture. Returns false if the image could not be loaded (renderer keeps
 * running without a background). */
bool vk_render_set_background(VkRenderer* r, const char* path, BackgroundFit fit);
bool vk_render_resize(VkRenderer* r, uint32_t width, uint32_t height);
bool vk_render_frame(VkRenderer* r, const DrawList* dl, FontSystem* fs);
bool vk_render_screenshot(VkRenderer* r, const char* path);
uint32_t vk_render_width(const VkRenderer* r);
uint32_t vk_render_height(const VkRenderer* r);
const char* vk_render_device_name(const VkRenderer* r);

#endif

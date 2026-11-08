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
    /* 1.0 (or 0 / unset) keeps the window opaque; < 1 asks the compositor to
     * blend the window with whatever is behind it. Requires
     * VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_KHR, which Wayland advertises. */
    float    window_opacity;
} VkRenderCreateInfo;

typedef struct VkRenderer VkRenderer;

VkRenderer* vk_render_create(const VkRenderCreateInfo* info);
void vk_render_destroy(VkRenderer* r);
/* Optional background image. NULL/empty/"none" disables and frees the
 * texture. Returns false if the image could not be loaded (renderer keeps
 * running without a background). */
bool vk_render_set_background(VkRenderer* r, const char* path, BackgroundFit fit);

/* Wallpaper readability controls: dim (0..1) darkens the image, vignette
 * (0..1) darkens its edges. Both default to 0 = untouched. Applies to the
 * next frame; no effect while no image is loaded. */
void vk_render_set_background_style(VkRenderer* r, float dim, float vignette);
/* Clamped whole-window opacity actually in use (0.2 .. 1.0, 1 = opaque). */
float vk_render_window_opacity(const VkRenderer* r);
bool vk_render_resize(VkRenderer* r, uint32_t width, uint32_t height);
bool vk_render_frame(VkRenderer* r, const DrawList* dl, FontSystem* fs);
bool vk_render_screenshot(VkRenderer* r, const char* path);
uint32_t vk_render_width(const VkRenderer* r);
uint32_t vk_render_height(const VkRenderer* r);
const char* vk_render_device_name(const VkRenderer* r);

#endif

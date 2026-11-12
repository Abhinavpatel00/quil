#ifndef QUILL_PLATFORM_H
#define QUILL_PLATFORM_H

#include "quill.h"

#include <GLFW/glfw3.h>
#include <mu/mu_input.h>
#include <vulkan/vulkan.h>

/* Thin platform layer over GLFW + mu_lib's mu_input.
 *
 * Quill owns the GLFW window lifecycle (creation, Vulkan surface, destroy,
 * terminate). The platform layer adopts that window and drives a mu_input
 * state so input can be queried platform-free (mu_input_key_down(Platform*,
 * MU_KEY_W) etc.) instead of scattering raw GLFW key/scancode checks through
 * the editor. GLFW callbacks still own immediate input (app_on_key, the char
 * callback for layout-aware text, ...); the platform layer is fed from those
 * callbacks via platform_push_key() and refreshed each frame by platform_poll().
 */
typedef struct Platform
{
    GLFWwindow   *window;
    MuInputState  state;
    MuInputMap    map;
    MuInputActions actions;
    uint8_t       pad_prev[MU_INPUT_MAX_PADS][MU_INPUT_PAD_BUTTONS];
    uint8_t       pad_of_jid[GLFW_JOYSTICK_LAST + 1];
    uint64_t      now_ns; /* single clock reading for this poll (P0-3) */
} Platform;

/* Adopt an already-created GLFWwindow. Quill remains the window owner. */
bool platform_init(Platform *p, GLFWwindow *window);
void platform_destroy(Platform *p);
GLFWwindow *platform_window(const Platform *p);
bool platform_should_close(const Platform *p);
void platform_request_close(Platform *p);

/* Pump OS events, refresh mouse/gamepad state, then begin+resolve the input
 * frame. Call once per main-loop iteration (after the frame's wait). */
void platform_poll(Platform *p);

/* Feed one GLFW key edge into the mu_input state. Called from the GLFW key
 * callback so mu_input keys stay current without platform owning callbacks. */
void platform_push_key(Platform *p, uint32_t scancode, bool down);

bool platform_key_down(const Platform *p, MuKey key);
bool platform_key_pressed(const Platform *p, MuKey key);
bool platform_key_released(const Platform *p, MuKey key);
bool platform_mouse_down(const Platform *p, MuMouseButton button);
bool platform_mouse_pressed(const Platform *p, MuMouseButton button);
bool platform_mouse_released(const Platform *p, MuMouseButton button);

/* Create a Vulkan surface on the adopted window. */
bool platform_create_surface(const Platform *p, VkInstance instance,
                             VkSurfaceKHR *out_surface);

#endif /* QUILL_PLATFORM_H */

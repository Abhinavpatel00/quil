#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* MuKey (logical, platform-free) -> GLFW key code. Indexed by MuKey; order
 * must match the MuKey enum in mu/mu_input.h. */
static const int k_mu_key_to_glfw[MU_KEY_COUNT] = {
    GLFW_KEY_UNKNOWN,      /* MU_KEY_UNKNOWN */
    GLFW_KEY_ESCAPE,       /* MU_KEY_ESCAPE */
    GLFW_KEY_SPACE,        /* MU_KEY_SPACE */
    GLFW_KEY_TAB,          /* MU_KEY_TAB */
    GLFW_KEY_ENTER,        /* MU_KEY_ENTER */
    GLFW_KEY_BACKSPACE,    /* MU_KEY_BACKSPACE */
    GLFW_KEY_DELETE,       /* MU_KEY_DELETE */
    GLFW_KEY_INSERT,       /* MU_KEY_INSERT */
    GLFW_KEY_HOME,         /* MU_KEY_HOME */
    GLFW_KEY_END,          /* MU_KEY_END */
    GLFW_KEY_PAGE_UP,      /* MU_KEY_PAGE_UP */
    GLFW_KEY_PAGE_DOWN,    /* MU_KEY_PAGE_DOWN */
    GLFW_KEY_LEFT,         /* MU_KEY_LEFT */
    GLFW_KEY_RIGHT,        /* MU_KEY_RIGHT */
    GLFW_KEY_UP,           /* MU_KEY_UP */
    GLFW_KEY_DOWN,         /* MU_KEY_DOWN */
    GLFW_KEY_LEFT_SHIFT,   /* MU_KEY_LEFT_SHIFT */
    GLFW_KEY_RIGHT_SHIFT,  /* MU_KEY_RIGHT_SHIFT */
    GLFW_KEY_LEFT_CONTROL, /* MU_KEY_LEFT_CTRL */
    GLFW_KEY_RIGHT_CONTROL,/* MU_KEY_RIGHT_CTRL */
    GLFW_KEY_LEFT_ALT,     /* MU_KEY_LEFT_ALT */
    GLFW_KEY_RIGHT_ALT,    /* MU_KEY_RIGHT_ALT */
    GLFW_KEY_MINUS,        /* MU_KEY_MINUS */
    GLFW_KEY_EQUAL,        /* MU_KEY_EQUAL */
    GLFW_KEY_LEFT_BRACKET, /* MU_KEY_LEFT_BRACKET */
    GLFW_KEY_RIGHT_BRACKET,/* MU_KEY_RIGHT_BRACKET */
    GLFW_KEY_COMMA,        /* MU_KEY_COMMA */
    GLFW_KEY_PERIOD,       /* MU_KEY_PERIOD */
    GLFW_KEY_SLASH,        /* MU_KEY_SLASH */
    GLFW_KEY_SEMICOLON,    /* MU_KEY_SEMICOLON */
    GLFW_KEY_APOSTROPHE,   /* MU_KEY_APOSTROPHE */
    GLFW_KEY_BACKSLASH,    /* MU_KEY_BACKSLASH */
    GLFW_KEY_GRAVE_ACCENT, /* MU_KEY_GRAVE */
    GLFW_KEY_F1, GLFW_KEY_F2, GLFW_KEY_F3, GLFW_KEY_F4, GLFW_KEY_F5, GLFW_KEY_F6,
    GLFW_KEY_F7, GLFW_KEY_F8, GLFW_KEY_F9, GLFW_KEY_F10, GLFW_KEY_F11, GLFW_KEY_F12,
    GLFW_KEY_A, GLFW_KEY_B, GLFW_KEY_C, GLFW_KEY_D, GLFW_KEY_E, GLFW_KEY_F,
    GLFW_KEY_G, GLFW_KEY_H, GLFW_KEY_I, GLFW_KEY_J, GLFW_KEY_K, GLFW_KEY_L,
    GLFW_KEY_M, GLFW_KEY_N, GLFW_KEY_O, GLFW_KEY_P, GLFW_KEY_Q, GLFW_KEY_R,
    GLFW_KEY_S, GLFW_KEY_T, GLFW_KEY_U, GLFW_KEY_V, GLFW_KEY_W, GLFW_KEY_X,
    GLFW_KEY_Y, GLFW_KEY_Z,
    GLFW_KEY_0, GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4,
    GLFW_KEY_5, GLFW_KEY_6, GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9,
};

static uint32_t slot_from_key(MuKey key)
{
    int scancode;
    if ((uint32_t)key >= (uint32_t)MU_KEY_COUNT) return MU_INPUT_SLOT_NONE;
    if (k_mu_key_to_glfw[key] == GLFW_KEY_UNKNOWN) return MU_INPUT_SLOT_NONE;
    scancode = glfwGetKeyScancode(k_mu_key_to_glfw[key]);
    return scancode < 0 ? MU_INPUT_SLOT_NONE : (uint32_t)scancode;
}

/* Fill the state's MuKey -> slot map so mu_input_key_down() works. */
static void fill_key_slots(MuInputState *state)
{
    uint32_t i;
    for (i = 0; i < (uint32_t)MU_KEY_COUNT; ++i)
    {
        const uint32_t slot = slot_from_key((MuKey)i);
        if (slot != MU_INPUT_SLOT_NONE)
            mu_input_key_set_slot(state, (MuKey)i, slot);
    }
}

/* ---- gamepads (polled; GLFW has no button events) ---------------------- */

static int find_free_pad_slot(const Platform *p)
{
    uint32_t slot;
    int jid;
    for (slot = 0; slot < MU_INPUT_MAX_PADS; ++slot)
    {
        bool used = false;
        for (jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
            if (p->pad_of_jid[jid] == (uint8_t)slot) { used = true; break; }
        if (!used) return (int)slot;
    }
    return -1;
}

static void poll_gamepads(Platform *p)
{
    int jid;
    for (jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
    {
        GLFWgamepadstate state;
        uint32_t axis, button;
        const bool is_pad = glfwJoystickIsGamepad(jid) == GLFW_TRUE &&
                            glfwGetGamepadState(jid, &state) == GLFW_TRUE;

        if (!is_pad)
        {
            if (p->pad_of_jid[jid] != 0xFFu)
            {
                const uint32_t slot = p->pad_of_jid[jid];
                mu_input_pad_connect(&p->state, slot, false, p->now_ns);
                memset(p->pad_prev[slot], 0, sizeof p->pad_prev[slot]);
                p->pad_of_jid[jid] = 0xFFu;
            }
            continue;
        }
        if (p->pad_of_jid[jid] == 0xFFu)
        {
            const int slot = find_free_pad_slot(p);
            if (slot < 0) continue; /* all 4 slots taken */
            p->pad_of_jid[jid] = (uint8_t)slot;
            memset(p->pad_prev[slot], 0, sizeof p->pad_prev[slot]);
            mu_input_pad_connect(&p->state, (uint32_t)slot, true, p->now_ns);
        }

        const uint32_t this_pad = p->pad_of_jid[jid];
        for (axis = 0; axis < MU_INPUT_PAD_AXES; ++axis)
            mu_input_pad_axis(&p->state, this_pad, axis, state.axes[axis]);

        for (button = 0; button <= (uint32_t)GLFW_GAMEPAD_BUTTON_LAST &&
                           button < MU_INPUT_PAD_BUTTONS;
             ++button)
        {
            const uint8_t down = (uint8_t)(state.buttons[button] == GLFW_PRESS);
            if (down != p->pad_prev[this_pad][button])
            {
                p->pad_prev[this_pad][button] = down;
                mu_input_push_pad_button(&p->state, this_pad, button, down != 0u,
                                         p->now_ns);
            }
        }
    }
}

/* ---- lifecycle --------------------------------------------------------- */

bool platform_init(Platform *p, GLFWwindow *window)
{
    double x = 0.0, y = 0.0;
    if (!p || !window) return false;
    memset(p, 0, sizeof *p);
    for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; ++jid) p->pad_of_jid[jid] = 0xFFu;

    p->window = window;
    mu_input_init(&p->state);
    fill_key_slots(&p->state);
    mu_input_map_init(&p->map, MU_INPUT_MAX_ACTIONS);
    mu_input_actions_reset(&p->actions);
    glfwGetCursorPos(window, &x, &y);
    mu_input_mouse_warp(&p->state, (float)x, (float)y);
    p->now_ns = (uint64_t)(glfwGetTime() * 1e9);
    return true;
}

void platform_destroy(Platform *p)
{
    if (!p) return;
    /* Quill owns the GLFW window and calls glfwDestroyWindow/glfwTerminate
     * itself; the platform layer only adopted the window. */
    memset(p, 0, sizeof *p);
}

GLFWwindow *platform_window(const Platform *p) { return p ? p->window : NULL; }
bool platform_should_close(const Platform *p)
{
    return p && p->window && glfwWindowShouldClose(p->window) == GLFW_TRUE;
}
void platform_request_close(Platform *p)
{
    if (p && p->window) glfwSetWindowShouldClose(p->window, GLFW_TRUE);
}

/* ---- input ------------------------------------------------------------- */

void platform_push_key(Platform *p, uint32_t scancode, bool down)
{
    if (!p) return;
    p->now_ns = (uint64_t)(glfwGetTime() * 1e9);
    mu_input_push_control(&p->state, scancode, down, p->now_ns);
}

void platform_poll(Platform *p)
{
    double x = 0.0, y = 0.0;
    if (!p || !p->window) return;

    /* Single clock reading for the whole poll: callbacks (which fire inside
     * glfwPollEvents) and pad edges all share it, so events are attributed
     * to exactly one frame. */
    p->now_ns = (uint64_t)(glfwGetTime() * 1e9);
    glfwPollEvents();

    glfwGetCursorPos(p->window, &x, &y);
    mu_input_mouse_pos(&p->state, (float)x, (float)y);
    poll_gamepads(p);

    mu_input_frame_begin(&p->state, p->now_ns);
    mu_input_resolve(&p->state, &p->map, &p->actions, p->state.dt);
}

bool platform_key_down(const Platform *p, MuKey key)
{
    return p && mu_input_key_down(&p->state, key);
}
bool platform_key_pressed(const Platform *p, MuKey key)
{
    return p && mu_input_key_pressed(&p->state, key);
}
bool platform_key_released(const Platform *p, MuKey key)
{
    return p && mu_input_key_released(&p->state, key);
}
bool platform_mouse_down(const Platform *p, MuMouseButton button)
{
    return p && mu_input_mouse_button_down(&p->state, button);
}
bool platform_mouse_pressed(const Platform *p, MuMouseButton button)
{
    return p && mu_input_mouse_button_pressed(&p->state, button);
}
bool platform_mouse_released(const Platform *p, MuMouseButton button)
{
    return p && mu_input_mouse_button_released(&p->state, button);
}

/* ---- Vulkan surface ---------------------------------------------------- */

bool platform_create_surface(const Platform *p, VkInstance instance,
                             VkSurfaceKHR *out_surface)
{
    VkResult result;
    *out_surface = VK_NULL_HANDLE;
    if (!p || !p->window) return false;
    result = glfwCreateWindowSurface(instance, p->window, NULL, out_surface);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "[platform] glfwCreateWindowSurface failed: %d\n",
                (int)result);
        *out_surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

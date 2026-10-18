#include <stdlib.h>
#define GLFW_INCLUDE_VULKAN
#include "runara/runara.h"
#include "runara/vulkan.h"
#include <GLFW/glfw3.h>

static const char *const *_extensions(void *user, uint32_t *count) {
  (void)user;
  return glfwGetRequiredInstanceExtensions(count);
}

static VkResult _surface(void *user, VkInstance instance,
                         VkSurfaceKHR *surface) {
  return glfwCreateWindowSurface(instance, (GLFWwindow *)user, NULL, surface);
}

static void _get_framebuffer_size(void *user, uint32_t *width,
                                  uint32_t *height) {
  int w = 0, h = 0;
  glfwGetFramebufferSize((GLFWwindow *)user, &w, &h);
  *width = w > 0 ? (uint32_t)w : 0;
  *height = h > 0 ? (uint32_t)h : 0;
}

int main(void) {
  if (!glfwInit() || !glfwVulkanSupported())
    return EXIT_FAILURE;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow *window = glfwCreateWindow(800, 600, "Runara Vulkan", NULL, NULL);

  if (!window) {
    glfwTerminate();
    return EXIT_FAILURE;
  }

  /* Supply vulkan init options. Use MANAGED for a Runara-managed swapchain &
   * presentation system. This is preferred for simple usecases like a window.
   * For more complex usecases, like a wayland compositor managing its own
   * DRM/GBM handles, use RN_VK_TARGET_EXTERNAL. */
  const struct rn_vk_init_options_t vk = {
      .mode = RN_VK_TARGET_MANAGED,
      .platform =
          {
              .user = window,
              .instance_extensions = _extensions,
              .create_surface = _surface,
              .framebuffer_size = _get_framebuffer_size,
          },
  };

  struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_VULKAN,
      .vk_options = &vk,
  };

  uint32_t width = 0, height = 0;
  _get_framebuffer_size(window, &width, &height);
  struct rn_state_t *state = rn_state_init(&options, width, height);

  if (!state) {
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_FAILURE;
  }

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    _get_framebuffer_size(window, &width, &height);
    if (!width || !height) {
      glfwWaitEvents();
      continue;
    }

    const struct rn_box_u32_t canvas = {.width = width, .height = height};
    rn_canvas_set_bounds(state, &canvas);

    if (rn_begin(state) != RN_OK)
      break;

    rn_clear_color(state, (rn_color_t){1, 1, 1, 1});
    rn_draw_rect(state, (struct rn_rect_t){50, 50, 100, 100},
                 (rn_color_t){1, 0, 0, 1});

    if (rn_end(state) != RN_OK)
      break;
  }

  rn_state_fini(state);
  glfwDestroyWindow(window);
  glfwTerminate();
  return EXIT_SUCCESS;
}

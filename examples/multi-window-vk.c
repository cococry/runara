#include <stdlib.h>

#define GLFW_INCLUDE_VULKAN
#include "runara/runara.h"
#include "runara/vulkan.h"
#include <GLFW/glfw3.h>

static const char *const *_extensions(void *user, uint32_t *count) {
  (void)user;
  return glfwGetRequiredInstanceExtensions(count);
}

static VkResult _create_vk_surface(void *user, VkInstance instance,
                         VkSurfaceKHR *vk_surface) {
  return glfwCreateWindowSurface(instance, (GLFWwindow *)user, NULL, vk_surface);
}

static void _get_framebuffer_size(void *user, uint32_t *width,
                                  uint32_t *height) {
  int w = 0, h = 0;
  glfwGetFramebufferSize((GLFWwindow *)user, &w, &h);
  *width = w > 0 ? (uint32_t)w : 0;
  *height = h > 0 ? (uint32_t)h : 0;
}

int main(void) {
  int result = EXIT_FAILURE;

  GLFWwindow *windows[3] = {0};

  struct rn_state_t *state = NULL;

  struct rn_vk_managed_surface_t *managed_surfaces[3] = {0};

  const char *titles[3] = {"Runara red", "Runara green", "Runara blue"};

  const rn_color_t colors[3] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};

  if (!glfwInit())
    return EXIT_FAILURE;

  if (!glfwVulkanSupported())
    goto cleanup;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  const struct rn_vk_init_options_t vk = {
      .mode = RN_VK_TARGET_MANAGED,
      .instance_extensions = _extensions,
  };

  const struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_VULKAN,
      .vk_options = &vk,
  };

  state = rn_state_init(&options);

  if (!state)
    goto cleanup;

  for (int i = 0; i < 3; ++i) {
    windows[i] = glfwCreateWindow(400, 300, titles[i], NULL, NULL);

    if (!windows[i])
      goto cleanup;

    const struct rn_vk_surface_impl_t surface_impl = {
        .user = windows[i],
        .create_vk_surface = _create_vk_surface,
        .framebuffer_size = _get_framebuffer_size,
    };

    if (rn_vk_surface_create(state, &surface_impl, &managed_surfaces[i]) != RN_OK)
      goto cleanup;
  }

  for (;;) {
    glfwPollEvents();
    int open_windows = 0;

    for (int i = 0; i < 3; ++i) {

      if (glfwWindowShouldClose(windows[i]))
        continue;

      ++open_windows;
      int width = 0, height = 0;
      glfwGetFramebufferSize(windows[i], &width, &height);

      if (width <= 0 || height <= 0)
        continue;

      rn_vk_surface_set_current(state, managed_surfaces[i]);

      const struct rn_box_u32_t bounds = {
          .width = (uint32_t)width, .height = (uint32_t)height,
      };

      rn_canvas_set_bounds(state, &bounds);

      if (rn_begin(state) != RN_OK)
        goto cleanup;

      rn_clear_color(state, (rn_color_t){0.15f, 0.15f, 0.15f, 1});

      rn_draw_rect(state, (struct rn_rect_t){50, 50, 200, 150}, colors[i]);

      if (rn_end(state) != RN_OK)
        goto cleanup;
    }

    if (!open_windows)
      break;
  }

  result = EXIT_SUCCESS;

cleanup:

  if (state)
    rn_state_fini(state);

  for (int i = 0; i < 3; ++i) {

    if (windows[i])
      glfwDestroyWindow(windows[i]);
  }

  glfwTerminate();
  return result;
}


#include <stdlib.h>
#include <stdint.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include "runara/runara.h"
#include "runara/vulkan.h"

static const char **instance_extensions = NULL;
static unsigned int instance_extension_count = 0;

static const char *const *_extensions(void *user, uint32_t *count) {
  (void)user;
  *count = (uint32_t)instance_extension_count;
  return (const char *const *)instance_extensions;
}

static VkResult _create_vk_surface(void *user, VkInstance instance,
                         VkSurfaceKHR *vk_surface) {
  SDL_Window *window = user;

  if (!SDL_Vulkan_CreateSurface(window, instance, vk_surface))
    return VK_ERROR_INITIALIZATION_FAILED;

  return VK_SUCCESS;
}

static void _get_framebuffer_size(void *user, uint32_t *width,
                                  uint32_t *height) {
  int w = 0, h = 0;

  SDL_Vulkan_GetDrawableSize((SDL_Window *)user, &w, &h);

  *width = w > 0 ? (uint32_t)w : 0;
  *height = h > 0 ? (uint32_t)h : 0;
}

int main(void) {

  if (SDL_Init(SDL_INIT_VIDEO) != 0)
    return EXIT_FAILURE;

  SDL_Window *window = SDL_CreateWindow(
      "Runara Vulkan",
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      800, 600,
      SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

  if (!window) {
    SDL_Quit();
    return EXIT_FAILURE;
  }

  /* Get the Vulkan instance extensions required by SDL */
  unsigned int count = 0;

  if (!SDL_Vulkan_GetInstanceExtensions(window, &count, NULL)) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  instance_extensions = calloc(count, sizeof(*instance_extensions));

  if (!instance_extensions) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  if (!SDL_Vulkan_GetInstanceExtensions(
          window, &count, instance_extensions)) {
    free(instance_extensions);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  instance_extension_count = count;

  /* Supply vulkan init options. Use MANAGED for a Runara-managed swapchain &
   * presentation system. This is preferred for simple usecases like a window.
   * For more complex usecases, like a wayland compositor managing its own
   * DRM/GBM handles, use RN_VK_TARGET_EXTERNAL. */

  const struct rn_vk_init_options_t vk = {
      .mode = RN_VK_TARGET_MANAGED,
      .user = window,
      .instance_extensions = _extensions,
  };

  const struct rn_vk_surface_impl_t surface_impl = {
      .user = window,
      .create_vk_surface = _create_vk_surface,
      .framebuffer_size = _get_framebuffer_size,
  };

  struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_VULKAN,
      .vk_options = &vk,
  };

  uint32_t width = 0, height = 0;
  _get_framebuffer_size(window, &width, &height);

  struct rn_state_t *state = rn_state_init(&options);

  if (!state) {
    free(instance_extensions);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  struct rn_vk_managed_surface_t *managed_surface = NULL;

  if (rn_vk_surface_create(state, &surface_impl, &managed_surface) != RN_OK) {
    rn_state_fini(state);
    free(instance_extensions);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  rn_vk_surface_set_current(state, managed_surface);

  bool running = true;
  bool failed = false;

  while (running) {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {

      if (event.type == SDL_QUIT)
        running = false;
    }

    if (!running)
      break;

    _get_framebuffer_size(window, &width, &height);

    if (!width || !height) {
      SDL_WaitEvent(NULL);
      continue;
    }

    const struct rn_box_u32_t canvas = {
        .width = width,
        .height = height,
    };

    rn_canvas_set_bounds(state, &canvas);

    if (rn_begin(state) != RN_OK) {
      failed = true;
      break;
    }

    rn_clear_color(state, (rn_color_t){1, 1, 1, 1});

    rn_draw_rect(state, (struct rn_rect_t){50, 50, 100, 100},
                 (rn_color_t){1, 0, 0, 1});

    if (rn_end(state) != RN_OK) {
      failed = true;
      break;
    }
  }

  rn_state_fini(state);

  free(instance_extensions);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}


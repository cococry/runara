
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "runara/runara.h"
#include "runara/vulkan.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define IMAGE_WIDTH 200.0f
#define IMAGE_SPEED 120.0f

static struct rn_state_t *state = NULL;

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

static bool load_image(const char *path, struct rn_image_t *image, int *width,
                       int *height) {
  int channels = 0;

  unsigned char *pixels =
      stbi_load(path, width, height, &channels, STBI_rgb_alpha);

  if (!pixels || *width <= 0 || *height <= 0) {
    fprintf(stderr, "Failed to load image '%s': %s\n", path,
            stbi_failure_reason());
    stbi_image_free(pixels);
    return false;
  }

  size_t pixel_bytes = (size_t)(*width) * (size_t)(*height) * 4;

  rn_result_t result =
      rn_image_dedicated(state, (uint32_t)(*width), (uint32_t)(*height), pixels,
                         pixel_bytes, image);

  stbi_image_free(pixels);

  if (result != RN_OK) {
    fprintf(stderr, "Failed to upload image '%s' (error %d)\n", path,
            (int)result);
    return false;
  }

  return true;
}

int main(int argc, char **argv) {
  const char *image_path = argc > 1 ? argv[1] : "image.png";
  const char *image2_path = argc > 2 ? argv[2] : "image2.jpg";

  GLFWwindow *window = NULL;

  struct rn_image_t image = {0};

  struct rn_image_t image2 = {0};

  bool image_uploaded = false;
  bool image2_uploaded = false;

  int exit_code = EXIT_FAILURE;

  if (!glfwInit() || !glfwVulkanSupported()) {
    fprintf(stderr, "Failed to initialize GLFW or Vulkan\n");
    return EXIT_FAILURE;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT,
                            "Runara Vulkan images", NULL, NULL);

  if (!window) {
    fprintf(stderr, "Failed to create GLFW window\n");
    goto cleanup;
  }

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
      .log = {.verbose = true, .logfile = true},
  };

  uint32_t width = 0, height = 0;
  _get_framebuffer_size(window, &width, &height);

  state = rn_state_init(&options);

  if (!state) {
    fprintf(stderr, "Failed to initialize Runara Vulkan\n");
    goto cleanup;
  }

  struct rn_vk_managed_surface_t *managed_surface = NULL;

  if (rn_vk_surface_create(state, &surface_impl, &managed_surface) != RN_OK) {
    rn_state_fini(state);
    state = NULL;
    fprintf(stderr, "Failed to initialize Runara Vulkan\n");
    goto cleanup;
  }

  rn_vk_surface_set_current(state, managed_surface);

  int pixel_width = 0;
  int pixel_height = 0;

  int pixel2_width = 0;
  int pixel2_height = 0;

  if (!load_image(image_path, &image, &pixel_width, &pixel_height))
    goto cleanup;

  image_uploaded = true;

  if (!load_image(image2_path, &image2, &pixel2_width, &pixel2_height))
    goto cleanup;

  image2_uploaded = true;

  const float image_height =
      IMAGE_WIDTH * (float)pixel_height / (float)pixel_width;

  const float image2_height =
      IMAGE_WIDTH * (float)pixel2_height / (float)pixel2_width;

  const struct rn_color_t white = {1.0f, 1.0f, 1.0f, 0.8f};

  const struct rn_color_t background = {0.08f, 0.09f, 0.12f, 1.0f};

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
      glfwSetWindowShouldClose(window, GLFW_TRUE);

    _get_framebuffer_size(window, &width, &height);

    if (!width || !height) {
      glfwWaitEvents();
      continue;
    }

    const struct rn_box_u32_t bounds = {
        .width = width,
        .height = height,
    };

    rn_canvas_set_bounds(state, &bounds);

    float available_width = (float)width + IMAGE_WIDTH;
    float image_x = (float)glfwGetTime() * IMAGE_SPEED;

    while (image_x >= available_width)
      image_x -= available_width;

    image_x -= IMAGE_WIDTH;

    rn_result_t result = rn_begin(state);

    if (result != RN_OK) {
      fprintf(stderr, "rn_begin failed (error %d)\n", (int)result);
      goto cleanup;
    }

    result = rn_draw_rect(
        state,
        (struct rn_rect_t){
            0.0f,
            0.0f,
            (float)width,
            (float)height,
        },
        background);

    /* Draw 10 alternating images. */

    for (uint32_t i = 0; i < 10 && result == RN_OK; i++) {
      bool use_image2 = (i % 2) != 0;

      struct rn_image_t *current_image =
          use_image2 ? &image2 : &image;

      float current_height =
          use_image2 ? image2_height : image_height;

      struct rn_rect_t rect = {
          .width = IMAGE_WIDTH,
          .height = current_height,
          .x = image_x + i * 20.0f,
          .y = ((float)height - current_height) * 0.5f + i * 20.0f,
      };

      result = rn_draw_texture(state, current_image, rect, white);
    }

    if (result == RN_OK)
      result = rn_end(state);

    if (result != RN_OK) {
      fprintf(stderr, "Runara Vulkan drawing failed (error %d)\n",
              (int)result);
      goto cleanup;
    }
    /* No glfwSwapBuffers():
     * Runara handles Vulkan presentation in rn_end().
     */
  }

  exit_code = EXIT_SUCCESS;

cleanup:

  if (state) {

    if (image2_uploaded)
      rn_texture_release(state, &image2);

    if (image_uploaded)
      rn_texture_release(state, &image);

    rn_state_fini(state);
    state = NULL;
  }

  if (window)
    glfwDestroyWindow(window);

  glfwTerminate();

  return exit_code;
}


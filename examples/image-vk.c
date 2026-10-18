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

static const char *const *_instance_extensions(void *user, uint32_t *count);
static VkResult _create_surface(void *user, VkInstance instance,
                                VkSurfaceKHR *out_surface);
static void _framebuffer_size(void *user, uint32_t *width, uint32_t *height);
static void _resize_cb(GLFWwindow *window, int width, int height);

static const char *const *_instance_extensions(void *user, uint32_t *count) {
  (void)user;

  return glfwGetRequiredInstanceExtensions(count);
}

static VkResult _create_surface(void *user, VkInstance instance,
                                VkSurfaceKHR *out_surface) {
  GLFWwindow *window = user;

  return glfwCreateWindowSurface(instance, window, NULL, out_surface);
}

static void _framebuffer_size(void *user, uint32_t *width, uint32_t *height) {
  GLFWwindow *window = user;
  int w = 0;
  int h = 0;

  glfwGetFramebufferSize(window, &w, &h);

  *width = w > 0 ? (uint32_t)w : 0;
  *height = h > 0 ? (uint32_t)h : 0;
}

static void _resize_cb(GLFWwindow *window, int width, int height) {
  struct rn_state_t *state = glfwGetWindowUserPointer(window);

  if (!state || width <= 0 || height <= 0)
    return;

  const struct rn_box_u32_t bounds = {
      .width = (uint32_t)width,
      .height = (uint32_t)height,
  };

  rn_canvas_set_bounds(state, &bounds);
}

int main(int argc, char **argv) {
  const char *image_path = argc > 1 ? argv[1] : "image.png";

  GLFWwindow *window = NULL;
  struct rn_state_t *state = NULL;
  struct rn_image_t image = {0};
  bool image_uploaded = false;
  bool glfw_initialized = false;
  int exit_code = EXIT_FAILURE;

  if (!glfwInit()) {
    fprintf(stderr, "Failed to initialize GLFW\n");
    return EXIT_FAILURE;
  }

  glfw_initialized = true;

  if (!glfwVulkanSupported()) {
    fprintf(stderr, "GLFW reports that Vulkan is unavailable\n");
    goto cleanup;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT,
                            "Runara Vulkan image (GLFW)", NULL, NULL);

  if (!window) {
    fprintf(stderr, "Failed to create GLFW window\n");
    goto cleanup;
  }

  glfwSetFramebufferSizeCallback(window, _resize_cb);

  uint32_t framebuffer_width = 0;
  uint32_t framebuffer_height = 0;

  _framebuffer_size(window, &framebuffer_width, &framebuffer_height);

  if (!framebuffer_width || !framebuffer_height) {
    fprintf(stderr, "Cannot initialize Vulkan with a zero-sized framebuffer\n");
    goto cleanup;
  }

  const struct rn_vk_init_options_t vk_options = {
      .mode = RN_VK_TARGET_MANAGED,
      .platform =
          {
              .user = window,
              .instance_extensions = _instance_extensions,
              .create_surface = _create_surface,
              .framebuffer_size = _framebuffer_size,
          },
  };

  const struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_VULKAN,
      .vk_options = &vk_options,
      .log =
          {
              .verbose = true,
              .logfile = true,
          },
  };

  state = rn_state_init(&options, framebuffer_width, framebuffer_height);

  if (!state) {
    fprintf(stderr, "Failed to initialize Runara Vulkan backend\n");
    goto cleanup;
  }

  glfwSetWindowUserPointer(window, state);

  int pixel_width = 0;
  int pixel_height = 0;
  int source_channels = 0;

  unsigned char *pixels = stbi_load(image_path, &pixel_width, &pixel_height,
                                    &source_channels, STBI_rgb_alpha);

  if (!pixels || pixel_width <= 0 || pixel_height <= 0) {
    fprintf(stderr, "Failed to load image '%s': %s\n", image_path,
            stbi_failure_reason());
    stbi_image_free(pixels);
    goto cleanup;
  }

  if ((size_t)pixel_width > SIZE_MAX / 4u / (size_t)pixel_height) {
    fprintf(stderr, "Image dimensions are too large\n");
    stbi_image_free(pixels);
    goto cleanup;
  }

  const size_t pixel_bytes = (size_t)pixel_width * (size_t)pixel_height * 4u;

  rn_result_t result =
      rn_texture_upload(state, (uint32_t)pixel_width, (uint32_t)pixel_height,
                        pixels, pixel_bytes, &image);

  stbi_image_free(pixels);

  if (result != RN_OK) {
    fprintf(stderr, "Failed to upload image (error %d)\n", (int)result);
    goto cleanup;
  }

  image_uploaded = true;

  const float image_height =
      IMAGE_WIDTH * (float)pixel_height / (float)pixel_width;
  const struct rn_color_t white = {1.0f, 1.0f, 1.0f, 1.0f};
  const struct rn_color_t background = {0.08f, 0.09f, 0.12f, 1.0f};

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
      glfwSetWindowShouldClose(window, GLFW_TRUE);

    _framebuffer_size(window, &framebuffer_width, &framebuffer_height);

    if (!framebuffer_width || !framebuffer_height) {
      glfwWaitEvents();
      continue;
    }

    const struct rn_box_u32_t bounds = {
        .width = framebuffer_width,
        .height = framebuffer_height,
    };

    rn_canvas_set_bounds(state, &bounds);

    const float available_width = (float)framebuffer_width + IMAGE_WIDTH;
    float image_x = (float)glfwGetTime() * IMAGE_SPEED;

    while (image_x >= available_width)
      image_x -= available_width;

    image_x -= IMAGE_WIDTH;

    const struct rn_rect_t image_rect = {
        .x = image_x,
        .y = ((float)framebuffer_height - image_height) * 0.5f,
        .width = IMAGE_WIDTH,
        .height = image_height,
    };

    result = rn_begin(state);

    if (result != RN_OK) {
      fprintf(stderr, "rn_begin failed (error %d)\n", (int)result);
      goto cleanup;
    }

    result = rn_draw_rect(state,
                          (struct rn_rect_t){
                              .width = (float)framebuffer_width,
                              .height = (float)framebuffer_height,
                          },
                          background);

    if (result == RN_OK)
      result = rn_draw_texture(state, &image, image_rect, white);

    rn_result_t end_result = rn_end(state);

    if (result == RN_OK)
      result = end_result;

    if (result != RN_OK) {
      fprintf(stderr, "Runara drawing failed (error %d)\n", (int)result);
      goto cleanup;
    }

    /* rn_end() submits the Vulkan commands and presents the swapchain. */
  }

  exit_code = EXIT_SUCCESS;

cleanup:
  if (window)
    glfwSetWindowUserPointer(window, NULL);

  if (state) {
    if (image_uploaded)
      rn_texture_release(state, &image);

    rn_state_fini(state);
  }

  if (window)
    glfwDestroyWindow(window);

  if (glfw_initialized)
    glfwTerminate();

  return exit_code;
}

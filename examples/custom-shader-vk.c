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

static const char *_frag_dedicated =
    "#version 450\n"
    "\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 1) in vec4 v_color;\n"
    "layout(location = 4) flat in int v_slot;\n"
    "layout(location = 5) flat in vec4 v_rect;\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_textures[16];\n"
    "layout(location = 0) out vec4 out_color;\n"
    "\n"
    "vec4 sample_texture(int slot, vec2 uv) {\n"
    "  switch (slot) {\n"
    "    case 0: return texture(u_textures[0], uv);\n"
    "    case 1: return texture(u_textures[1], uv);\n"
    "    case 2: return texture(u_textures[2], uv);\n"
    "    case 3: return texture(u_textures[3], uv);\n"
    "    case 4: return texture(u_textures[4], uv);\n"
    "    case 5: return texture(u_textures[5], uv);\n"
    "    case 6: return texture(u_textures[6], uv);\n"
    "    case 7: return texture(u_textures[7], uv);\n"
    "    case 8: return texture(u_textures[8], uv);\n"
    "    case 9: return texture(u_textures[9], uv);\n"
    "    case 10: return texture(u_textures[10], uv);\n"
    "    case 11: return texture(u_textures[11], uv);\n"
    "    case 12: return texture(u_textures[12], uv);\n"
    "    case 13: return texture(u_textures[13], uv);\n"
    "    case 14: return texture(u_textures[14], uv);\n"
    "    case 15: return texture(u_textures[15], uv);\n"
    "    default: return vec4(1.0);\n"
    "  }\n"
    "}\n"
    "\n"
    "float rounded_rect(vec2 p, vec2 size, float radius) {\n"
    "  radius = clamp(radius, 0.0, min(size.x, size.y) * 0.5);\n"
    "  vec2 q = abs(p) - (size * 0.5 - vec2(radius));\n"
    "  return length(max(q, 0.0)) +\n"
    "         min(max(q.x, q.y), 0.0) - radius;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  vec4 texel = sample_texture(v_slot, v_uv);\n"
    "  vec2 center = v_rect.xy + v_rect.zw * 0.5;\n"
    "  float d = rounded_rect(gl_FragCoord.xy - center, v_rect.zw, 20.0);\n"
    "  float aa = max(fwidth(d), 0.001);\n"
    "  float alpha = 1.0 - smoothstep(-aa, aa, d);\n"
    "  out_color = texel * v_color;\n"
    "  out_color.a *= alpha;\n"
    "}\n";

static const char *const *_instance_extensions(void *user, uint32_t *count) {
  (void)user;
  return glfwGetRequiredInstanceExtensions(count);
}

static VkResult _create_vk_surface(void *user, VkInstance instance,
                                VkSurfaceKHR *out_vk_surface) {
  return glfwCreateWindowSurface(instance, (GLFWwindow *)user, NULL,
                                 out_vk_surface);
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

static bool load_image(struct rn_state_t *state, const char *path,
                       struct rn_image_t *image, int *width, int *height) {
  int channels = 0;
  unsigned char *pixels =
      stbi_load(path, width, height, &channels, STBI_rgb_alpha);

  if (!pixels || *width <= 0 || *height <= 0) {
    fprintf(stderr, "Failed to load image '%s': %s\n", path,
            stbi_failure_reason());
    stbi_image_free(pixels);
    return false;
  }

  if ((size_t)*width > SIZE_MAX / 4u / (size_t)*height) {
    fprintf(stderr, "Image '%s' is too large\n", path);
    stbi_image_free(pixels);
    return false;
  }

  const size_t pixel_bytes = (size_t)*width * (size_t)*height * 4u;
  rn_result_t result = rn_image_dedicated(
      state, (uint32_t)*width, (uint32_t)*height, pixels, pixel_bytes, image);
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
  struct rn_state_t *state = NULL;

  struct rn_image_t image = {0};

  struct rn_image_t image2 = {0};

  bool image_uploaded = false;
  bool image2_uploaded = false;
  int exit_code = EXIT_FAILURE;

  if (!glfwInit()) {
    fprintf(stderr, "Failed to initialize GLFW\n");
    return EXIT_FAILURE;
  }

  if (!glfwVulkanSupported()) {
    fprintf(stderr, "GLFW reports that Vulkan is unavailable\n");
    goto cleanup;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT,
                            "Runara Vulkan rounded images", NULL, NULL);

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
      .user = window,
      .instance_extensions = _instance_extensions,
  };

  const struct rn_vk_surface_impl_t surface_impl = {
      .user = window,
      .create_vk_surface = _create_vk_surface,
      .framebuffer_size = _framebuffer_size,
  };

  const struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_VULKAN,
      .vk_options = &vk_options,
      .log = {.verbose = true, .logfile = true},
      .shader_fragment_dedicated_src = _frag_dedicated,
  };

  state = rn_state_init(&options);

  if (!state) {
    fprintf(stderr, "Failed to initialize Runara Vulkan backend\n");
    goto cleanup;
  }

  struct rn_vk_managed_surface_t *managed_surface = NULL;

  if (rn_vk_surface_create(state, &surface_impl, &managed_surface) != RN_OK) {
    rn_state_fini(state);
    state = NULL;
    fprintf(stderr, "Failed to initialize Runara Vulkan backend\n");
    goto cleanup;
  }

  rn_vk_surface_set_current(state, managed_surface);
  glfwSetWindowUserPointer(window, state);

  int pixel_width = 0, pixel_height = 0;
  int pixel2_width = 0, pixel2_height = 0;

  if (!load_image(state, image_path, &image, &pixel_width, &pixel_height))
    goto cleanup;
  image_uploaded = true;

  if (!load_image(state, image2_path, &image2, &pixel2_width, &pixel2_height))
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

    rn_result_t result = rn_begin(state);

    if (result != RN_OK) {
      fprintf(stderr, "rn_begin failed (error %d)\n", (int)result);
      goto cleanup;
    }

    result =
        rn_draw_rect(state,
                     (struct rn_rect_t){0.0f, 0.0f, (float)framebuffer_width,
                                        (float)framebuffer_height},
                     background);

    for (uint32_t i = 0; i < 10 && result == RN_OK; ++i) {
      const bool use_image2 = (i % 2) != 0;
      struct rn_image_t *current_image = use_image2 ? &image2 : &image;
      const float current_height = use_image2 ? image2_height : image_height;

      const struct rn_rect_t rect = {
          .x = image_x + (float)i * 20.0f,
          .y = ((float)framebuffer_height - current_height) * 0.5f +
               (float)i * 20.0f,
          .width = IMAGE_WIDTH,
          .height = current_height,
      };

      result = rn_draw_texture(state, current_image, rect, white);
    }

    rn_result_t end_result = rn_end(state);

    if (result == RN_OK)
      result = end_result;

    if (result != RN_OK) {
      fprintf(stderr, "Runara drawing failed (error %d)\n", (int)result);
      goto cleanup;
    }
    // rn_end() submits and presents the managed Vulkan swapchain.
  }

  exit_code = EXIT_SUCCESS;

cleanup:

  if (window)
    glfwSetWindowUserPointer(window, NULL);

  if (state) {

    if (image2_uploaded)
      rn_texture_release(state, &image2);

    if (image_uploaded)
      rn_texture_release(state, &image);
    rn_state_fini(state);
  }

  if (window)
    glfwDestroyWindow(window);
  glfwTerminate();
  return exit_code;
}

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "runara/runara.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define IMAGE_WIDTH 200.0f
#define IMAGE_SPEED 120.0f

static struct rn_state_t *state = NULL;

static void *_gl_load_proc(const char *name) {
  union {
    GLFWglproc function;
    void *pointer;
  } proc = {.function = glfwGetProcAddress(name)};

  return proc.pointer;
}

void resize_cb(GLFWwindow *window, int width, int height) {
  (void)window;

  struct rn_box_u32_t bounds = {
      .width = width,
      .height = height,
  };

  rn_canvas_set_bounds(state, &bounds);
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

  if (!glfwInit()) {
    fprintf(stderr, "Failed to initialize GLFW\n");
    return EXIT_FAILURE;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Runara images", NULL,
                            NULL);

  if (!window) {
    fprintf(stderr, "Failed to create GLFW window\n");
    goto cleanup;
  }

  glfwSetFramebufferSizeCallback(window, resize_cb);

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_OPENGL,
      .gl_load_proc = _gl_load_proc,
      .log = {.verbose = true, .logfile = true},
  };

  state = rn_state_init(&options, WINDOW_WIDTH, WINDOW_HEIGHT);

  if (!state) {
    fprintf(stderr, "Failed to initialize Runara\n");
    goto cleanup;
  }

  int pixel_width = 0;
  int pixel_height = 0;

  int pixel2_width = 0;
  int pixel2_height = 0;

  // Load first image
  if (!load_image(image_path, &image, &pixel_width, &pixel_height))
    goto cleanup;

  image_uploaded = true;

  // Load second image
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

    int framebuffer_width = 0;
    int framebuffer_height = 0;

    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);

    if (framebuffer_width <= 0 || framebuffer_height <= 0)
      continue;

    float available_width = (float)framebuffer_width + IMAGE_WIDTH;
    float image_x = (float)glfwGetTime() * IMAGE_SPEED;

    while (image_x >= available_width)
      image_x -= available_width;

    image_x -= IMAGE_WIDTH;

    rn_result_t result = rn_begin(state);

    if (result != RN_OK) {
      fprintf(stderr, "rn_begin failed (error %d)\n", (int)result);
      goto cleanup;
    }

    result = rn_draw_rect(state,
                          (struct rn_rect_t){
                              0.0f,
                              0.0f,
                              (float)framebuffer_width,
                              (float)framebuffer_height,
                          },
                          background);

    // Draw 10 images, alternating between image and image2
    for (uint32_t i = 0; i < 10 && result == RN_OK; i++) {
      bool use_image2 = (i % 2) != 0;

      struct rn_image_t *current_image = use_image2 ? &image2 : &image;

      float current_height = use_image2 ? image2_height : image_height;

      struct rn_rect_t rect = {
          .width = IMAGE_WIDTH,
          .height = current_height,
          .x = image_x + i * 20.0f,
          .y = ((float)framebuffer_height - current_height) * 0.5f + i * 20.0f,
      };

      result = rn_draw_texture(state, current_image, rect, white);
    }

    if (result == RN_OK)
      result = rn_end(state);

    if (result != RN_OK) {
      fprintf(stderr, "Runara drawing failed (error %d)\n", (int)result);
      goto cleanup;
    }

    glfwSwapBuffers(window);
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

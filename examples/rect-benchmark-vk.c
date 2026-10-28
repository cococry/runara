#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define GLFW_INCLUDE_VULKAN
#include "runara/runara.h"
#include "runara/vulkan.h"
#include <GLFW/glfw3.h>

#define RECT_COUNT 10000

static const char *const *_extensions(void *user, uint32_t *count) {
  (void)user;
  return glfwGetRequiredInstanceExtensions(count);
}

static VkResult _create_vk_surface(void *user, VkInstance instance,
                                   VkSurfaceKHR *out_vk_surface) {
  return glfwCreateWindowSurface(instance, user, NULL, out_vk_surface);
}

static void _framebuffer_size(void *user, uint32_t *width, uint32_t *height) {
  int w = 0, h = 0;
  glfwGetFramebufferSize(user, &w, &h);
  *width = w > 0 ? (uint32_t)w : 0;
  *height = h > 0 ? (uint32_t)h : 0;
}

static float _random_unit(void) {
  return (float)((double)rand() / ((double)RAND_MAX + 1.0));
}

int main(void) {
  int exit_code = EXIT_FAILURE;
  struct rn_state_t *state = NULL;
  GLFWwindow *window = NULL;

  if (!glfwInit())
    return EXIT_FAILURE;

  if (!glfwVulkanSupported()) {
    fprintf(stderr, "Vulkan is unavailable\n");
    goto cleanup;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  window = glfwCreateWindow(1280, 720, "Runara: Rectangles, vsync off", NULL, NULL);

  if (!window)
    goto cleanup;

  const struct rn_vk_init_options_t vk = {
      .mode = RN_VK_TARGET_MANAGED,
      .instance_extensions = _extensions,
  };

  const struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_VULKAN,
      .vk_options = &vk,
  };

  state = rn_state_init(&options);

  if (!state) {
    fprintf(stderr, "Failed to initialize Runara\n");
    goto cleanup;
  }

  const struct rn_vk_surface_impl_t surface_impl = {
      .user = window,
      .create_vk_surface = _create_vk_surface,
      .framebuffer_size = _framebuffer_size,
      .disable_vsync = true,
  };

  struct rn_vk_managed_surface_t *managed_surface = NULL;
  rn_result_t result = rn_vk_surface_create(state, &surface_impl, &managed_surface);

  if (result != RN_OK) {
    fprintf(stderr, "Could not create a surface without vsync: %s\n",
            rn_result_to_str(result));
    goto cleanup;
  }

  rn_vk_surface_set_current(state, managed_surface);

  /* generate normalized geometry once to keep random generation out of the benchmark */
  struct rn_rect_t rectangles[RECT_COUNT];
  rn_color_t colors[RECT_COUNT];
  srand((unsigned int)time(NULL));

  for (int i = 0; i < RECT_COUNT; ++i) {
    float width = 0.01f + _random_unit() * 0.04f;
    float height = 0.01f + _random_unit() * 0.04f;

    rectangles[i] = (struct rn_rect_t){
        _random_unit() * (1.0f - width),
        _random_unit() * (1.0f - height),
        width, height,
    };

    colors[i] = (rn_color_t){_random_unit(), _random_unit(), _random_unit(), 1};
  }

  unsigned int frames = 0;
  double report_start = glfwGetTime();
  char title[128];

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (glfwWindowShouldClose(window))
      break;

    uint32_t width = 0, height = 0;
    _framebuffer_size(window, &width, &height);

    if (!width || !height) {
      glfwWaitEvents();
      report_start = glfwGetTime();
      frames = 0;
      continue;
    }

    const struct rn_box_u32_t bounds = {.width = width, .height = height};

    rn_canvas_set_bounds(state, &bounds);
    result = rn_begin(state);

    if (result != RN_OK)
      goto render_error;

    result = rn_clear_color(state, (rn_color_t){0.06f, 0.06f, 0.06f, 1});

    if (result != RN_OK)
      goto render_error;

    for (int i = 0; i < RECT_COUNT; ++i) {
      const struct rn_rect_t rect = {
          rectangles[i].x * width, rectangles[i].y * height,
          rectangles[i].width * width, rectangles[i].height * height,
      };

      result = rn_draw_rect(state, rect, colors[i]);

      if (result != RN_OK)
        goto render_error;
    }

    result = rn_end(state);

    if (result != RN_OK)
      goto render_error;

    ++frames;
    double now = glfwGetTime();
    double elapsed = now - report_start;

    if (elapsed >= 1.0) {
      double fps = frames / elapsed;
      snprintf(title, sizeof(title), "Runara: %i rectangles, vsync off | %.1f FPS", RECT_COUNT, fps);
      glfwSetWindowTitle(window, title);
      printf("%i rectangles | %.1f FPS | %.3f ms/frame\n", RECT_COUNT, fps, 1000.0 / fps);
      fflush(stdout);
      report_start = now;
      frames = 0;
    }
  }

  exit_code = EXIT_SUCCESS;
  goto cleanup;

render_error:
  fprintf(stderr, "Rendering failed: %s\n", rn_result_to_str(result));

cleanup:

  if (state)
    rn_state_fini(state);

  if (window)
    glfwDestroyWindow(window);

  glfwTerminate();
  return exit_code;
}

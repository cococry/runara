
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include "runara/runara.h"
#include <GLFW/glfw3.h>

struct rn_state_t *state = NULL;

static void *_gl_load_proc(const char *name) {
  union {
    GLFWglproc function;
    void *pointer;
  } proc = {.function = glfwGetProcAddress(name)};
  return proc.pointer;
}

void resize_cb(GLFWwindow *window, int width, int height) {
  (void)window;
  struct rn_box_u32_t bounds = {.width = width, .height = height};
  rn_canvas_set_bounds(state, &bounds);
}

int main(void) {
  if (!glfwInit())
    return EXIT_FAILURE;

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  GLFWwindow *window = glfwCreateWindow(800, 600, "Runara rect", NULL, NULL);
  if (!window) {
    glfwTerminate();
    return EXIT_FAILURE;
  }

  glfwSetFramebufferSizeCallback(window, resize_cb);
  glfwMakeContextCurrent(window);

  struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_OPENGL,
      .gl_load_proc = _gl_load_proc,
  };

  state = rn_state_init(&options, 800, 600);

  if (!state) {
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_FAILURE;
  }

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    rn_begin(state);
    rn_clear_color(state, (rn_color_t){0.2, 0.2, 0.2, 1.0}); 
    rn_draw_rect(state, (struct rn_rect_t){50, 50, 100, 100},
                 (rn_color_t){1, 0, 0, 1});
    rn_end(state);

    glfwSwapBuffers(window);
  }

  rn_state_fini(state);
  glfwDestroyWindow(window);
  glfwTerminate();
  return EXIT_SUCCESS;
}

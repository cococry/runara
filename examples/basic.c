#include <stdio.h>
#include <stdlib.h>

#include <GLFW/glfw3.h>
#include <runara/runara.h>

int main(void) {
  if (!glfwInit()) {
    fprintf(stderr, "Failed to initialize GLFW\n");
    return EXIT_FAILURE;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  GLFWwindow *window =
      glfwCreateWindow(1280, 720, "Runara - Basic", NULL, NULL);

  if (!window) {
    fprintf(stderr, "Failed to create window\n");
    glfwTerminate();
    return EXIT_FAILURE;
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  struct rn_init_options_t opts = {.log = {.verbose = true, .logfile = true, .quiet = true}, .render_backend = RN_RENDER_BACKEND_OPENGL,
  .gl_load_proc = (rn_gl_load_proc)glfwGetProcAddress};
  struct rn_state_t *state = rn_state_init(&opts);

  if (!state) {
    fprintf(stderr, "Failed to initialize Runara\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_FAILURE;
  }

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    /* Rendering with runara goes here */

    glfwSwapBuffers(window);
  }

  rn_state_fini(state);

  glfwDestroyWindow(window);
  glfwTerminate();

  return EXIT_SUCCESS;
}

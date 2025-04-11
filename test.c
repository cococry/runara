#include <runara/runara.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <runara/runara.h>

RnState *state;

void resizecb(GLFWwindow* window, int w, int h) {
  rn_resize_display(state, w, h);
}

int main() {
  glfwInit();

  GLFWwindow* window = glfwCreateWindow(1280, 720, "Hello, World!", NULL, NULL);

  glfwSetFramebufferSizeCallback(window, resizecb);

  glfwMakeContextCurrent(window);

  // Initialize your state of the library
  state = rn_init(1280, 720, (RnGLLoader)glfwGetProcAddress);

  // Loading some fonts
  RnFont *heading = rn_load_font_ex(state, "/usr/share/fonts/TTF/VictorMonoNerdFont-Regular.ttf", 36, 1024, 1024, 4, RN_TEX_FILTER_LINEAR,0 );
  RnFont *paragraph = rn_load_font_ex(state, "/usr/share/fonts/TTF/VictorMonoNerdFont-Regular.ttf", 24, 1024, 1024, 4, RN_TEX_FILTER_LINEAR,0 );

  while(!glfwWindowShouldClose(window)) {
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Beginning a rende pass with Runara
    rn_begin(state);

    // Rendering some text with one font
    rn_text_render(state, "Hello, runara!", heading, (vec2s){20, 20}, RN_WHITE);

    // Render some text with another font
    rn_text_render(state, "Hey There!\nThis is a paragraph.", paragraph, (vec2s){20, 70}, RN_WHITE);

    // Rendering a basic rectangle
    rn_rect_render_ex(state, (vec2s){20, 130}, (vec2s){200, 100}, 0, RN_RED, RN_NO_COLOR, 0, 10);

    // Ending the render pass
    rn_end(state);

    // External application or game code can still run perfectly fine
    // here.

    glfwPollEvents();
    glfwSwapBuffers(window);
  }
  rn_free_font(state, heading);
  rn_free_font(state, paragraph);
  rn_terminate(state);
}

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
  RnFont *heading = rn_load_font(state, "/usr/share/fonts/noto/NotoColorEmoji.ttf", 48);
  RnFont *heading2 = rn_load_font(state, "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Bold.ttf", 48); 

  while(!glfwWindowShouldClose(window)) {
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Beginning a rende pass with Runara
    rn_begin(state);

    // Rendering some text with one font
    rn_text_render(state, "🌟🦉🎯🛸🍀🎸🐍🚀🌊🧠🍉🔥🎲🖤🦖", heading, (vec2s){20, 20}, RN_WHITE);
    rn_text_render(state, "fuckin ass emojis letsgo", heading2, (vec2s){20, 100}, RN_WHITE);


    // Ending the render pass
    rn_end(state);

    // External application or game code can still run perfectly fine
    // here.

    glfwPollEvents();
    glfwSwapBuffers(window);
  }
  rn_free_font(state, heading);
  rn_terminate(state);
}

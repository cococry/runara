
#include "vendor/glad/include/glad/glad.h"
#include <runara/runara.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <runara/runara.h>

RnState *state;
GLuint fbotex;
uint32_t fboid;
GLuint fborbo;
static uint32_t g_fbo_w = 0, g_fbo_h = 0;
RnState* fbostate;

void resizecb(GLFWwindow* window, int w, int h) {
  rn_resize_display(state, w, h);
}

GLuint createframebuffer(int width, int height, GLuint* texture_out) {
  if (*texture_out) glDeleteTextures(1, texture_out);
  if (fboid) glDeleteFramebuffers(1, &fboid);

  GLuint new_fbo, tex;
  glGenFramebuffers(1, &new_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, new_fbo);

  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  // For crisp text/UI during resize
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "FBO creation failed\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
  }

  *texture_out = tex;
  g_fbo_w = (uint32_t)width;
  g_fbo_h = (uint32_t)height;

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return new_fbo;
}

void renderframebuffer(
  RnState* state, 
  uint32_t texid,
  uint32_t width,
  uint32_t height,
  RnColor color,
  RnColor border_color,
  float border_width, 
  float corner_radius) {

  vec2s texcoords[4] = { 
    (vec2s){.x = 0.0f, .y = 1.0f}, 
    (vec2s){.x = 1.0f, .y = 1.0f}, 
    (vec2s){.x = 1.0f, .y = 0.0f}, 
    (vec2s){.x = 0.0f, .y = 0.0f}, 
  };
  rn_image_render_adv(state, (vec2s){.x = 0, .y = 0}, 0.0f, 
                      color, 
                      (RnTexture){.id = texid, .width = 
                      width, .height = height}, texcoords, false,
                      border_color, border_width,
                      corner_radius);
}

int main() {
  glfwInit();

  GLFWwindow* window = glfwCreateWindow(1280, 720, "Hello, World!", NULL, NULL);

  glfwSetFramebufferSizeCallback(window, resizecb);

  glfwMakeContextCurrent(window);

  // Initialize your state of the library
  state = rn_init(1280, 720, (RnGLLoader)glfwGetProcAddress);

  // Loading some fonts
  RnFont *heading2 = rn_load_font(state, "/usr/share/fonts/TTF/DejaVuSans.ttf", 48); 


  while(!glfwWindowShouldClose(window)) {
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Beginning a rende pass with Runara
    //

    rn_begin(state);


    rn_rect_render(state, (vec2s){100, 100}, (vec2s){100, 100}, RN_WHITE);

    // Ending the render pass
    rn_end(state);



    // External application or game code can still run perfectly fine
    // here.

    glfwSwapBuffers(window);
    glfwPollEvents();
  }
  rn_terminate(state);
}

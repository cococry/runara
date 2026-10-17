<img align="left" style="width:128px" src="https://github.com/cococry/runara/blob/master/branding/logo.png" width="128px">

**Runara is a simple, well documented, bloatless 2D texture library**

Runara is a high level abstraction over both Vulkan and OpenGL that renders and manages 2D textures and rectangles. 
It implements an instanced batch renderer that efficiently draws queued lists of textures and rectangles in an immediate-mode
manner. One can upload texture data into Runara's managed texture atlas or to directly to a dedicated texture.  

---

<br>

## Features
- Vulkan 1.0 backend implementation  
- OpenGL 3.3 backend implementation  
- Efficient instanced & batched 2D renderer 
- Merging of adjacent render batches
- Optimized 3D texture atlas  
- Ability to supply custom shaders 

## Example using GLFW

```c
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
  RnFont *heading = rn_load_font(state, "./Lora-Italic.ttf", 36);
  RnFont *paragraph = rn_load_font(state, "./Lora-Italic.ttf", 24);

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
    rn_rect_render(state, (vec2s){20, 130}, (vec2s){200, 100}, RN_RED);

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
```

## Building and installing

Make sure you have those build dependencies installed 
before installing Runara:

### Dependencies
```console
freetype, harfbuzz, gl, make, gcc, cglm
```

After installing the dependencies just run:

```console
sudo make install
```

## Documentation

For documentation of the library, just take a look at the Runara [header file](https://github.com/cococry/runara/blob/master/include/runara/runara.h) where every function is well documented.
This is in my opinion the best approach to documenting as it is straightly bound to actually deleloping with the library.

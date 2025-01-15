<img align="left" style="width:128px" src="https://github.com/cococry/runara/blob/master/branding/logo.png" width="128px">

**Runara is a simple, well documented, bloatless 2D graphics library**

Runara aims to be as small and as fast as possible. The code is contained
in ~1.4k lines of code that feature a complete batch rendering system, 
glyph loading with [freetype](http://freetype.org/), text shaping with [harfbuzz](https://harfbuzz.github.io/),
a glyph caching system and much more. 

---

<br>

## Features
- Efficient batch rendering system
- Support for shader operations like rounded corners
- Comple text rendering & loading engine
- Extensivly documented functions & structures
- Support for loading & rendering images
- No global or hidden state
- Built to make extending & modifing as straightforward as possible
- Small and readable codebase

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
freetype, harfbuzz, gl, make, gcc
```

After installing the dependencies just run:

```console
sudo make install
```

## Documentation

For documentation of the library, just take a look at the Runara [header file](https://github.com/cococry/runara/blob/master/include/runara/runara.h) where every function is well documented.
This is in my opinion the best approach to documenting as it is straightly bound to actually deleloping with the library.

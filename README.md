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
```

## Building and installing

Runara 2.0 uses **Meson** as its build system and supports both OpenGL and Vulkan rendering backends.

Make sure you have the following build dependencies installed before building Runara:

### Dependencies

```text
meson, ninja, gcc, pkg-config, cglm
```

Depending on the rendering backends you want, you will also need:

- **OpenGL:** OpenGL development libraries: `libgl-dev`
- **Vulkan:** Vulkan development libraries, GLSL, Python: `libvulkan-dev`, `glslang-tools`, `python3`
- **Examples:** GLFW3, X11 & GL development libraries: `libglfw3-dev`, `libx11-dev`, `libgl-dev`

### Building

Clone the repository:

```console
git clone https://github.com/cococry/runara.git
cd runara
```

Configure the build with both rendering backends and examples enabled:

```console
meson setup build -Dbackends=all -Dexamples=true
```

Compile Runara:

```console
meson compile -C build
```

Alternatively, you can build only one rendering backend:

```console
meson setup build -Dbackends=opengl -Dexamples=true
```

Or:

```console
meson setup build -Dbackends=vulkan -Dexamples=false
```

### Installing

After building Runara, install the library, headers and pkg-config file with:

```console
sudo meson install -C build
```

You can then link Runara in your own projects using `pkg-config`.

## Documentation

You can take a look at the [examples](https://github.com/cococry/runara/tree/master/examples) or read the [header file](https://github.com/cococry/runara/blob/master/include/runara/runara.h) to get started with using Runara. For Vulkan specific documentation, see the [Vulkan header file](https://github.com/cococry/runara/blob/master/include/runara/vulkan.h) or the [GLFW Vulkan example](https://github.com/cococry/runara/blob/master/examples/image-vk.c)

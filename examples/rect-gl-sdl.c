
#include <stdlib.h>

#include <SDL2/SDL.h>

#include "runara/runara.h"

static struct rn_state_t *state = NULL;

static void *_gl_load_proc(const char *name) {
  return SDL_GL_GetProcAddress(name);
}

static void resize_cb(int width, int height) {

  struct rn_box_u32_t bounds = {
      .width = (uint32_t)width,
      .height = (uint32_t)height,
  };

  rn_canvas_set_bounds(state, &bounds);
}

int main(void) {

  if (SDL_Init(SDL_INIT_VIDEO) != 0)
    return EXIT_FAILURE;

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                      SDL_GL_CONTEXT_PROFILE_CORE);

  SDL_Window *window = SDL_CreateWindow(
      "Runara rect",
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      800, 600,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

  if (!window) {
    SDL_Quit();
    return EXIT_FAILURE;
  }

  SDL_GLContext context = SDL_GL_CreateContext(window);

  if (!context) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  SDL_GL_MakeCurrent(window, context);

  struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_OPENGL,
      .gl_load_proc = _gl_load_proc,
  };

  int width, height;
  SDL_GL_GetDrawableSize(window, &width, &height);

  state = rn_state_init(&options);

  if (!state) {
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  SDL_GL_SetSwapInterval(1);

  bool running = true;

  while (running) {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {

      switch (event.type) {
      case SDL_QUIT:
        running = false;
        break;

      case SDL_WINDOWEVENT:

        if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
          SDL_GL_GetDrawableSize(window, &width, &height);
          resize_cb(width, height);
        }

        break;
      }
    }

    rn_begin(state);

    rn_clear_color(state, (rn_color_t){0.2, 0.2, 0.2, 1.0});

    rn_draw_rect(state, (struct rn_rect_t){50, 50, 100, 100},
                 (rn_color_t){1, 0, 0, 1});

    rn_end(state);

    SDL_GL_SwapWindow(window);
  }

  rn_state_fini(state);

  SDL_GL_DeleteContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return EXIT_SUCCESS;
}


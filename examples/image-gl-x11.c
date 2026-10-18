#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "runara/runara.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define IMAGE_WIDTH 200.0f
#define IMAGE_SPEED 120.0f

static void *_gl_load_proc(const char *name);
static double _elapsed_seconds(const struct timespec *start);

static void *_gl_load_proc(const char *name) {
  union {
    __GLXextFuncPtr function;
    void *pointer;
  } proc = {.function = glXGetProcAddressARB((const GLubyte *)name)};

  return proc.pointer;
}

static double _elapsed_seconds(const struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  return (double)(now.tv_sec - start->tv_sec) +
         (double)(now.tv_nsec - start->tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv) {
  const char *image_path = argc > 1 ? argv[1] : "image.png";

  Display *display = NULL;
  Window window = None;
  Colormap colormap = None;
  GLXContext context = NULL;
  XVisualInfo *visual = NULL;
  GLXFBConfig *configs = NULL;
  struct rn_state_t *state = NULL;
  struct rn_image_t image = {0};

  bool image_uploaded = false;
  bool running = true;
  int exit_code = EXIT_FAILURE;
  int width = WINDOW_WIDTH;
  int height = WINDOW_HEIGHT;
  Atom wm_delete;

  display = XOpenDisplay(NULL);
  if (!display) {
    fprintf(stderr, "Failed to open X11 display\n");
    return EXIT_FAILURE;
  }

  const int screen = DefaultScreen(display);
  const int fb_attributes[] = {
      GLX_X_RENDERABLE,
      True,
      GLX_DRAWABLE_TYPE,
      GLX_WINDOW_BIT,
      GLX_RENDER_TYPE,
      GLX_RGBA_BIT,
      GLX_X_VISUAL_TYPE,
      GLX_TRUE_COLOR,
      GLX_RED_SIZE,
      8,
      GLX_GREEN_SIZE,
      8,
      GLX_BLUE_SIZE,
      8,
      GLX_ALPHA_SIZE,
      8,
      GLX_DOUBLEBUFFER,
      True,
      None,
  };

  int config_count = 0;
  configs = glXChooseFBConfig(display, screen, fb_attributes, &config_count);
  if (!configs || config_count <= 0) {
    fprintf(stderr, "No suitable GLX framebuffer configuration\n");
    goto cleanup;
  }

  GLXFBConfig config = configs[0];
  visual = glXGetVisualFromFBConfig(display, config);
  if (!visual) {
    fprintf(stderr, "Failed to get GLX visual\n");
    goto cleanup;
  }

  colormap = XCreateColormap(display, RootWindow(display, screen),
                             visual->visual, AllocNone);
  if (colormap == None) {
    fprintf(stderr, "Failed to create X11 colormap\n");
    goto cleanup;
  }

  XSetWindowAttributes window_attributes = {0};
  window_attributes.colormap = colormap;
  window_attributes.event_mask =
      StructureNotifyMask | KeyPressMask | ExposureMask;

  window = XCreateWindow(display, RootWindow(display, screen), 0, 0,
                         WINDOW_WIDTH, WINDOW_HEIGHT, 0, visual->depth,
                         InputOutput, visual->visual, CWColormap | CWEventMask,
                         &window_attributes);
  if (window == None) {
    fprintf(stderr, "Failed to create X11 window\n");
    goto cleanup;
  }

  XStoreName(display, window, "Runara image (OpenGL/X11)");
  wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &wm_delete, 1);

  PFNGLXCREATECONTEXTATTRIBSARBPROC create_context =
      (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddressARB(
          (const GLubyte *)"glXCreateContextAttribsARB");

  if (!create_context) {
    fprintf(stderr, "GLX_ARB_create_context is unavailable\n");
    goto cleanup;
  }

  const int context_attributes[] = {
      GLX_CONTEXT_MAJOR_VERSION_ARB,
      3,
      GLX_CONTEXT_MINOR_VERSION_ARB,
      3,
      GLX_CONTEXT_PROFILE_MASK_ARB,
      GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
      None,
  };

  context = create_context(display, config, NULL, True, context_attributes);
  if (!context) {
    fprintf(stderr, "Failed to create OpenGL 3.3 core context\n");
    goto cleanup;
  }

  XMapWindow(display, window);

  if (!glXMakeCurrent(display, window, context)) {
    fprintf(stderr, "Failed to make GLX context current\n");
    goto cleanup;
  }

  typedef void (*glx_swap_interval_ext_fn)(Display *, GLXDrawable, int);
  glx_swap_interval_ext_fn swap_interval =
      (glx_swap_interval_ext_fn)glXGetProcAddressARB(
          (const GLubyte *)"glXSwapIntervalEXT");
  if (swap_interval)
    swap_interval(display, window, 1);

  struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_OPENGL,
      .gl_load_proc = _gl_load_proc,
      .log = {.verbose = true, .logfile = true},
  };

  state = rn_state_init(&options, WINDOW_WIDTH, WINDOW_HEIGHT);
  if (!state) {
    fprintf(stderr, "Failed to initialize Runara\n");
    goto cleanup;
  }

  int pixel_width = 0;
  int pixel_height = 0;
  int source_channels = 0;
  unsigned char *pixels = stbi_load(image_path, &pixel_width, &pixel_height,
                                    &source_channels, STBI_rgb_alpha);

  if (!pixels || pixel_width <= 0 || pixel_height <= 0) {
    fprintf(stderr, "Failed to load PNG '%s': %s\n", image_path,
            stbi_failure_reason());
    stbi_image_free(pixels);
    goto cleanup;
  }

  size_t pixel_bytes = (size_t)pixel_width * (size_t)pixel_height * 4u;
  rn_result_t result =
      rn_texture_upload(state, (uint32_t)pixel_width, (uint32_t)pixel_height,
                        pixels, pixel_bytes, &image);

  stbi_image_free(pixels);

  if (result != RN_OK) {
    fprintf(stderr, "Failed to upload image (error %d)\n", (int)result);
    goto cleanup;
  }

  image_uploaded = true;

  const float image_height =
      IMAGE_WIDTH * (float)pixel_height / (float)pixel_width;
  const struct rn_color_t white = {1.0f, 1.0f, 1.0f, 1.0f};
  const struct rn_color_t background = {0.08f, 0.09f, 0.12f, 1.0f};

  struct timespec start = {0};
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (running) {
    while (XPending(display)) {
      XEvent event;
      XNextEvent(display, &event);

      if (event.type == ClientMessage &&
          (Atom)event.xclient.data.l[0] == wm_delete) {
        running = false;
      } else if (event.type == KeyPress &&
                 XLookupKeysym(&event.xkey, 0) == XK_Escape) {
        running = false;
      } else if (event.type == ConfigureNotify) {
        width = event.xconfigure.width;
        height = event.xconfigure.height;

        if (width > 0 && height > 0) {
          struct rn_box_u32_t bounds = {
              .width = (uint32_t)width,
              .height = (uint32_t)height,
          };
          rn_canvas_set_bounds(state, &bounds);
        }
      }
    }

    if (!running)
      break;

    if (width <= 0 || height <= 0)
      continue;

    const float available_width = (float)width + IMAGE_WIDTH;
    float image_x = (float)(_elapsed_seconds(&start) * IMAGE_SPEED);

    while (image_x >= available_width)
      image_x -= available_width;

    image_x -= IMAGE_WIDTH;

    struct rn_rect_t image_rect = {
        .x = image_x,
        .y = ((float)height - image_height) * 0.5f,
        .width = IMAGE_WIDTH,
        .height = image_height,
    };

    result = rn_begin(state);
    if (result != RN_OK) {
      fprintf(stderr, "rn_begin failed (error %d)\n", (int)result);
      goto cleanup;
    }

    result = rn_draw_rect(
        state, (struct rn_rect_t){0.0f, 0.0f, (float)width, (float)height},
        background);

    if (result == RN_OK)
      result = rn_draw_texture(state, &image, image_rect, white);

    rn_result_t end_result = rn_end(state);
    if (result == RN_OK)
      result = end_result;

    if (result != RN_OK) {
      fprintf(stderr, "Runara drawing failed (error %d)\n", (int)result);
      goto cleanup;
    }

    glXSwapBuffers(display, window);
  }

  exit_code = EXIT_SUCCESS;

cleanup:
  if (state) {
    if (image_uploaded)
      rn_texture_release(state, &image);
    rn_state_fini(state);
  }

  if (display) {
    if (context) {
      glXMakeCurrent(display, None, NULL);
      glXDestroyContext(display, context);
    }
    if (window != None)
      XDestroyWindow(display, window);
    if (colormap != None)
      XFreeColormap(display, colormap);
    if (visual)
      XFree(visual);
    if (configs)
      XFree(configs);
    XCloseDisplay(display);
  }

  return exit_code;
}

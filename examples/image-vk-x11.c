#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

/* Must be defined before <vulkan.h> declares VkXlibSurfaceCreateInfoKHR */
#define VK_USE_PLATFORM_XLIB_KHR

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "runara/runara.h"
#include "runara/vulkan.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define IMAGE_WIDTH 200.0f
#define IMAGE_SPEED 120.0f

struct x11_window_t {
  Display *display;
  Window window;
  Atom delete_window;
  uint32_t width;
  uint32_t height;
  bool mapped;
  bool running;
};

static const char *const *_instance_extensions(void *user, uint32_t *count);
static VkResult _create_surface(void *user, VkInstance instance,
                                VkSurfaceKHR *out_surface);
static void _framebuffer_size(void *user, uint32_t *width, uint32_t *height);
static void _process_event(struct x11_window_t *x11, XEvent *event);
static double _elapsed_seconds(const struct timespec *start);

static const char *const *_instance_extensions(void *user, uint32_t *count) {
  (void)user;

  static const char *const extensions[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
  };

  *count = (uint32_t)(sizeof(extensions) / sizeof(extensions[0]));
  return extensions;
}

static VkResult _create_surface(void *user, VkInstance instance,
                                VkSurfaceKHR *out_surface) {
  struct x11_window_t *x11 = user;

  const VkXlibSurfaceCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
      .dpy = x11->display,
      .window = x11->window,
  };

  return vkCreateXlibSurfaceKHR(instance, &info, NULL, out_surface);
}

static void _framebuffer_size(void *user, uint32_t *width, uint32_t *height) {
  struct x11_window_t *x11 = user;

  *width = x11->width;
  *height = x11->height;
}

static void _process_event(struct x11_window_t *x11, XEvent *event) {
  switch (event->type) {
  case ConfigureNotify:
    if (event->xconfigure.width > 0 && event->xconfigure.height > 0) {
      x11->width = (uint32_t)event->xconfigure.width;
      x11->height = (uint32_t)event->xconfigure.height;
    }
    break;

  case MapNotify:
    x11->mapped = true;
    break;

  case UnmapNotify:
    x11->mapped = false;
    break;

  case KeyPress:
    if (XLookupKeysym(&event->xkey, 0) == XK_Escape)
      x11->running = false;
    break;

  case ClientMessage:
    if ((Atom)event->xclient.data.l[0] == x11->delete_window)
      x11->running = false;
    break;

  case DestroyNotify:
    x11->window = None;
    x11->running = false;
    break;

  default:
    break;
  }
}

static double _elapsed_seconds(const struct timespec *start) {
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0.0;

  return (double)(now.tv_sec - start->tv_sec) +
         (double)(now.tv_nsec - start->tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv) {
  const char *image_path = argc > 1 ? argv[1] : "image.png";

  struct x11_window_t x11 = {0};
  struct rn_state_t *state = NULL;
  struct rn_image_t image = {0};
  bool image_uploaded = false;
  int exit_code = EXIT_FAILURE;

  x11.display = XOpenDisplay(NULL);

  if (!x11.display) {
    fprintf(stderr, "Failed to open X11 display\n");
    return EXIT_FAILURE;
  }

  const int screen = DefaultScreen(x11.display);

  x11.window = XCreateSimpleWindow(x11.display, RootWindow(x11.display, screen),
                                   0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 0,
                                   BlackPixel(x11.display, screen),
                                   BlackPixel(x11.display, screen));

  if (x11.window == None) {
    fprintf(stderr, "Failed to create X11 window\n");
    goto cleanup;
  }

  x11.width = WINDOW_WIDTH;
  x11.height = WINDOW_HEIGHT;
  x11.running = true;

  XStoreName(x11.display, x11.window, "Runara Vulkan image (X11)");
  XSelectInput(x11.display, x11.window, StructureNotifyMask | KeyPressMask);

  x11.delete_window = XInternAtom(x11.display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(x11.display, x11.window, &x11.delete_window, 1);

  XMapWindow(x11.display, x11.window);
  XFlush(x11.display);

  /* The window must be mapped before surface/swapchain initialization. */
  while (x11.running && !x11.mapped) {
    XEvent event;
    XNextEvent(x11.display, &event);
    _process_event(&x11, &event);
  }

  if (!x11.running)
    goto cleanup;

  const struct rn_vk_init_options_t vk_options = {
      .mode = RN_VK_TARGET_MANAGED,
      .platform =
          {
              .user = &x11,
              .instance_extensions = _instance_extensions,
              .create_surface = _create_surface,
              .framebuffer_size = _framebuffer_size,
          },
  };

  const struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_VULKAN,
      .vk_options = &vk_options,
      .log =
          {
              .verbose = true,
              .logfile = true,
          },
  };

  state = rn_state_init(&options, x11.width, x11.height);

  if (!state) {
    fprintf(stderr, "Failed to initialize Runara Vulkan backend\n");
    goto cleanup;
  }

  int pixel_width = 0;
  int pixel_height = 0;
  int source_channels = 0;

  unsigned char *pixels = stbi_load(image_path, &pixel_width, &pixel_height,
                                    &source_channels, STBI_rgb_alpha);

  if (!pixels || pixel_width <= 0 || pixel_height <= 0) {
    fprintf(stderr, "Failed to load image '%s': %s\n", image_path,
            stbi_failure_reason());
    stbi_image_free(pixels);
    goto cleanup;
  }

  if ((size_t)pixel_width > SIZE_MAX / 4u / (size_t)pixel_height) {
    fprintf(stderr, "Image dimensions are too large\n");
    stbi_image_free(pixels);
    goto cleanup;
  }

  const size_t pixel_bytes = (size_t)pixel_width * (size_t)pixel_height * 4u;

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

  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    fprintf(stderr, "Failed to read monotonic clock\n");
    goto cleanup;
  }

  while (x11.running) {
    while (XPending(x11.display)) {
      XEvent event;
      XNextEvent(x11.display, &event);
      _process_event(&x11, &event);
    }

    if (!x11.running)
      break;

    if (!x11.mapped || !x11.width || !x11.height) {
      XEvent event;
      XNextEvent(x11.display, &event);
      _process_event(&x11, &event);
      continue;
    }

    const struct rn_box_u32_t bounds = {
        .width = x11.width,
        .height = x11.height,
    };

    rn_canvas_set_bounds(state, &bounds);

    const float available_width = (float)x11.width + IMAGE_WIDTH;
    float image_x = (float)_elapsed_seconds(&start) * IMAGE_SPEED;

    while (image_x >= available_width)
      image_x -= available_width;

    image_x -= IMAGE_WIDTH;

    const struct rn_rect_t image_rect = {
        .x = image_x,
        .y = ((float)x11.height - image_height) * 0.5f,
        .width = IMAGE_WIDTH,
        .height = image_height,
    };

    result = rn_begin(state);

    if (result != RN_OK) {
      fprintf(stderr, "rn_begin failed (error %d)\n", (int)result);
      goto cleanup;
    }

    result = rn_draw_rect(state,
                          (struct rn_rect_t){
                              .width = (float)x11.width,
                              .height = (float)x11.height,
                          },
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

    /* rn_end() submits and presents through Runara's managed swapchain. */
  }

  exit_code = EXIT_SUCCESS;

cleanup:
  if (state) {
    if (image_uploaded)
      rn_texture_release(state, &image);

    rn_state_fini(state);
  }

  if (x11.display) {
    if (x11.window != None)
      XDestroyWindow(x11.display, x11.window);

    XCloseDisplay(x11.display);
  }

  return exit_code;
}


#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "runara/runara.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define IMAGE_WIDTH 200.0f
#define IMAGE_SPEED 120.0f

#define RESET "\033[0m"
#define DIM "\033[2m"
#define BOLD "\033[1m"
#define RED "\033[38;5;203m"
#define YELLOW "\033[38;5;221m"
#define GREEN "\033[38;5;114m"
#define BLUE "\033[38;5;75m"
#define CYAN "\033[38;5;80m"
#define GRAY "\033[38;5;245m"

static struct rn_state_t *state = NULL;

static void *_gl_load_proc(const char *name);

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

void cool_custom_log(FILE *stream, enum rn_log_level_t level,
                     const char *subsystem, const char *function,
                     const char *file, int line, const char *fmt,
                     va_list args) {

  if (!stream || !fmt)
    return;

  const char *name = "UNKNOWN";
  const char *emoji = "❓";
  const char *color = GRAY;

  switch (level) {
  case RN_LL_INFO:
    name = "INFO";
    emoji = "✅";
    color = GREEN;
    break;
  case RN_LL_TRACE:
    name = "TRACE";
    emoji = "ℹ️";
    color = GREEN;
    break;

  case RN_LL_WARN:
    name = "WARN";
    emoji = "⚠️";
    color = YELLOW;
    break;

  case RN_LL_ERR:
    name = "ERROR";
    emoji = "❌";
    color = RED;
    break;
  case RN_LL_FATAL:
    name = "FATAL";
    emoji = "❌";
    color = RED;
    break;
  default:
    break;
  }

  // Current time with millisecond precision.
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    return;

  struct tm tm_info;

  if (!localtime_r(&ts.tv_sec, &tm_info))
    return;

  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm_info);

  // Disable ANSI colors when writing to a file.
  int use_color = isatty(fileno(stream));

  const char *c = use_color ? color : "";
  const char *reset = use_color ? RESET : "";
  const char *dim = use_color ? DIM : "";
  const char *blue = use_color ? BLUE : "";
  const char *bold = use_color ? BOLD : "";

  // Get basename from source file path.
  const char *filename = file ? file : "?";

  if (file) {
    const char *slash = strrchr(file, '/');
    const char *backslash = strrchr(file, '\\');

    if (slash && (!backslash || slash > backslash))
      filename = slash + 1;
    else if (backslash)
      filename = backslash + 1;
  }

  // Print timestamp, emoji, level and subsystem.
  fprintf(stream,
          "%s%s.%03ld%s "
          "%s %s%s%-5s%s "
          "%s[%-10s]%s ",
          dim, timestamp, ts.tv_nsec / 1000000, reset, emoji, c, bold, name,
          reset, blue, subsystem ? subsystem : "core", reset);

  // Print actual formatted log message.
  vfprintf(stream, fmt, args);

  // Print source location.
  fprintf(stream, " %s(%s:%d %s)%s\n", dim, filename, line,
          function ? function : "?", reset);

  fflush(stream);
}

int main(int argc, char **argv) {
  const char *image_path = argc > 1 ? argv[1] : "image.png";

  // Open our custom log file
  FILE *log_file = fopen("log.txt", "w");

  if (!log_file) {
    perror("Failed to open log.txt");
    return EXIT_FAILURE;
  }

  GLFWwindow *window = NULL;

  struct rn_image_t image = {0};

  bool image_uploaded = false;
  int exit_code = EXIT_FAILURE;

  if (!glfwInit()) {
    fprintf(stderr, "Failed to initialize GLFW\n");
    return EXIT_FAILURE;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  window =
      glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Runara image", NULL, NULL);

  if (!window) {
    fprintf(stderr, "Failed to create GLFW window\n");
    goto cleanup;
  }

  glfwSetFramebufferSizeCallback(window, resize_cb);

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  struct rn_init_options_t options = {
      .render_backend = RN_RENDER_BACKEND_OPENGL,
      .gl_load_proc = _gl_load_proc,
      .log = {.verbose = true,
              .logfile = true,
              .log_cb = cool_custom_log,
              /* Pass our custom log file, Runara should log to there */
              .log_stream = log_file},
  };

  state = rn_state_init(&options);

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

  size_t pixel_bytes = (size_t)pixel_width * (size_t)pixel_height * 4;

  rn_result_t result =
      rn_image_dedicated(state, (uint32_t)pixel_width, (uint32_t)pixel_height,
                         pixels, pixel_bytes, &image);

  stbi_image_free(pixels);

  if (result != RN_OK) {
    fprintf(stderr, "Failed to upload image to Runara (error %d)\n",
            (int)result);
    goto cleanup;
  }

  image_uploaded = true;

  const float image_height =
      IMAGE_WIDTH * (float)pixel_height / (float)pixel_width;

  const struct rn_color_t white = {1.0f, 1.0f, 1.0f, 1.0f};

  const struct rn_color_t background = {0.08f, 0.09f, 0.12f, 1.0f};

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
      glfwSetWindowShouldClose(window, GLFW_TRUE);

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);

    if (framebuffer_width <= 0 || framebuffer_height <= 0)
      continue;

    float available_width = (float)framebuffer_width + IMAGE_WIDTH;
    float image_x = (float)glfwGetTime() * IMAGE_SPEED;

    while (image_x >= available_width)
      image_x -= available_width;

    image_x -= IMAGE_WIDTH;

    struct rn_rect_t image_rect = {
        .x = image_x,
        .y = ((float)framebuffer_height - image_height) * 0.5f,
        .width = IMAGE_WIDTH,
        .height = image_height,
    };

    result = rn_begin(state);

    if (result != RN_OK) {
      fprintf(stderr, "rn_begin failed (error %d)\n", (int)result);
      goto cleanup;
    }

    result =
        rn_draw_rect(state,
                     (struct rn_rect_t){0.0f, 0.0f, (float)framebuffer_width,
                                        (float)framebuffer_height},
                     background);

    if (result == RN_OK)
      result = rn_draw_texture(state, &image, image_rect, white);

    if (result == RN_OK)
      result = rn_end(state);

    if (result != RN_OK) {
      fprintf(stderr, "Runara drawing failed (error %d)\n", (int)result);
      goto cleanup;
    }

    glfwSwapBuffers(window);
  }

  exit_code = EXIT_SUCCESS;

cleanup:

  if (state) {

    if (image_uploaded)
      rn_texture_release(state, &image);

    rn_state_fini(state);
  }

  if (window)
    glfwDestroyWindow(window);

  glfwTerminate();

  /* Close our custom log file, Runara only borrowed it */
  fclose(log_file);
  return exit_code;
}

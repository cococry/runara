project(
  'runara',
  'c',
  version: '0.1.0',
  meson_version: '>= 1.2.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=false',
    'optimization=3',
  ]
)

cc = meson.get_compiler('c')

# --- Dependencies ---
dep_glfw3      = dependency('glfw3', required: true)
dep_cglm       = dependency('cglm', required: true)
dep_freetype   = dependency('freetype2', required: true)
dep_harfbuzz   = dependency('harfbuzz', required: true)
dep_m          = cc.find_library('m', required: true)
dep_gl         = cc.find_library('GL', required: true)

# Include vendor subdirectories
subdir('vendor/glad')
subdir('vendor/stb_image')
subdir('vendor/linesky')

runara_inc = include_directories('include')

# --- Sources ---
runara_sources = [
  'runara.c',
  glad_c,        # from vendor/glad/meson.build
]

# --- Compiler flags ---
runara_cflags = [
  '-march=native',
  '-ffast-math',
  '-Wno-stringop-overflow',
  '-Wno-missing-braces',
  '-Wno-int-conversion',
  '-Wno-unused-variable',
  '-Wno-unused-but-set-variable',
]

# --- Static library build ---
runara_lib = static_library(
  'runara',
  sources: runara_sources,
  include_directories: [
    runara_inc,
    glad_inc,
    stb_inc,
  ],
  dependencies: [
    dep_glfw3,
    dep_cglm,
    dep_freetype,
    dep_harfbuzz,
    dep_gl,
    dep_m,
  ],
  c_args: runara_cflags,
  install: true
)

# --- Install headers ---
install_subdir('include/runara', install_dir: get_option('includedir') / 'runara')

# --- pkg-config file ---
runara_pc = import('pkgconfig')
runara_pc.generate(
  name: 'runara',
  filebase: 'runara',
  description: 'Runara rendering library (GL abstraction for Vortex compositor)',
  version: meson.project_version(),
  libraries: runara_lib,
  subdirs: 'runara',
  requires: ['glfw3', 'cglm', 'freetype2', 'harfbuzz'],
)

# --- Optional run target ---
run_target('build-runara',
  command: ['meson', 'compile', '-C', meson.current_build_dir()]
)


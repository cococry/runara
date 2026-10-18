project(
  'runara',
  'c',
  version: '2.0.0',
  meson_version: '>= 1.2.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=false',
    'optimization=3',
  ]
)

cc = meson.get_compiler('c')


dep_cglm = dependency('cglm', required: true)
dep_m = cc.find_library('m', required: true)

backend_option = get_option('backends')

build_opengl = backend_option == 'opengl' or backend_option == 'all'
build_vulkan = backend_option == 'vulkan' or backend_option == 'all'

runara_sources = [
  'src/runara.c',
  'src/log.c',
  'src/backend.c',
  'src/renderer.c',
]

runara_inc = include_directories('include')

runara_includes = [
  runara_inc,
]

runara_deps = [
  dep_cglm,
  dep_m,
]

runara_cflags = [
  '-march=native',
  '-ffast-math',
]

if build_opengl
  dep_gl = dependency('gl', required: true)

  subdir('vendor/glad')

  runara_sources += [
    'src/opengl/backend.c',
    glad_c,
  ]

  runara_includes += [
    glad_inc,
  ]

  runara_deps += [
    dep_gl,
  ]

  runara_cflags += [
    '-DRN_HAS_OPENGL=1',
  ]
endif

if build_vulkan
  add_languages('cpp')
  dep_vulkan = dependency('vulkan', required: true)
  glslang = find_program('glslangValidator', required: true)
  python = find_program('python3', required: true)

  vulkan_shaders = custom_target(
    'runara-vulkan-shaders',
    input: ['shaders/ui.vert', 'shaders/atlas.frag', 'shaders/dedicated.frag'],
    output: 'runara-vulkan-shaders.c',
    command: [python, files('tools/make_vk_shaders.py'), glslang,
              '@OUTPUT@', '@INPUT@'],
  )

  runara_sources += [
    'src/vulkan/backend.c',
    'src/vulkan/vma.cpp',
    vulkan_shaders,
  ]

  runara_includes += [include_directories('vendor/vma/include')]
  runara_deps += [dep_vulkan]
  runara_cflags += ['-DRN_HAS_VULKAN=1']
endif

runara_lib = static_library(
  'runara',
  sources: runara_sources,
  include_directories: runara_includes,
  dependencies: runara_deps,
  c_args: runara_cflags,
  install: true,
)

runara_dep = declare_dependency(
  link_with: runara_lib,
  include_directories: runara_includes,
  dependencies: runara_deps,
)

install_subdir(
  'include/runara',
  install_dir: get_option('includedir'),
)

runara_pc = import('pkgconfig')

runara_pc.generate(
  name: 'runara',
  filebase: 'runara',
  description: 'Runara backend-independent 2D rendering library',
  version: meson.project_version(),
  libraries: runara_lib,
  subdirs: 'runara',
  requires: ['cglm'],
)

if get_option('examples')
  subdir('examples')
endif

run_target(
  'build-runara',
  command: [
    'meson',
    'compile',
    '-C',
    meson.current_build_dir(),
  ],
)

summary(
  {
    'OpenGL backend': build_opengl,
    'Vulkan backend': build_vulkan,
    'Examples': get_option('examples'),
  },
  section: 'Runara configuration',
  bool_yn: true,
)


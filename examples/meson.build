example_sources = []

if get_option('glfw-examples')
  example_sources += [
    'image-gl',
    'multi-image-gl',
    'multi-image-vk',
    'image-vk',
    'rect-gl',
    'rect-vk',
    'multi-window-vk',
    'custom-log-gl',
    'custom-shader-gl',
    'custom-shader-vk',
  ]
endif

if get_option('x11-examples')
  example_sources += [
    'image-gl-x11',
    'image-vk-x11',
  ]
endif

if get_option('sdl-examples')
  example_sources += [
    'rect-gl-sdl',
    'rect-vk-sdl',
  ]
endif

configure_file(
  input: 'image.png',
  output: 'image.png',
  copy: true,
)

configure_file(
  input: 'image2.jpg',
  output: 'image2.jpg',
  copy: true,
)

dep_glfw = dependency('glfw3', required: get_option('glfw-examples'))
dep_x11 = dependency('x11', required: get_option('x11-examples'))
dep_sdl2 = dependency('sdl2', required: get_option('sdl-examples'))

foreach name : example_sources
  deps = [runara_dep]

  if name.endswith('-x11')
    deps += [dep_x11]
  elif name.endswith('-sdl')
    deps += [dep_sdl2]
  else 
    deps += [dep_glfw]
  endif

  executable(
    'runara-' + name,
    name + '.c',
    dependencies: deps,
    install: false,
  )
endforeach


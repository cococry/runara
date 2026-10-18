example_sources = [
  'image-gl',
  'image-vk',
  'image-gl-x11',
  'image-vk-x11',
  'rect-gl',
  'rect-vk',
]

configure_file(
  input: 'image.png',
  output: 'image.png',
  copy: true,
)

dep_glfw = dependency('glfw3', required: true)
dep_x11 = dependency('x11', required: true)
dep_gl = dependency('gl')

foreach name : example_sources
  executable(
    'runara-' + name,
    name + '.c',
    dependencies: [
      runara_dep,
      dep_glfw,
      dep_x11,
      dep_gl
    ],
    install: false,
  )
endforeach

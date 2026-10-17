example_sources = [
  'image',
]

configure_file(
  input: 'image.png',
  output: 'image.png',
  copy: true,
)

dep_glfw = dependency('glfw3', required: true)

foreach name : example_sources
  executable(
    'runara-' + name,
    name + '.c',
    dependencies: [
      runara_dep,
      dep_glfw,
    ],
    install: false,
  )
endforeach

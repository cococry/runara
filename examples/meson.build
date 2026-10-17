example_sources = [
  'basic',
]

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

project('fastfind', 'c',
  version : '1.0.0',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'optimization=2',
  ]
)

gtk4_dep   = dependency('gtk4')
thread_dep = dependency('threads')

sources = files(
  'src/main.c',
  'src/fileitem.c',
  'src/indexer.c',
)

executable('fastfind',
  sources,
  dependencies : [gtk4_dep, thread_dep],
  install : true,
)

project('minscan', 'c',
  version: '0.1.0',
  meson_version: '>=1.2',
  default_options: [
    'c_std=c23',
    'warning_level=3',
    'buildtype=debugoptimized',
    'b_lundef=true',
    'b_asneeded=true',
  ])

cc = meson.get_compiler('c')
add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-Werror',  # scoped to our targets; built-in werror=true would hit subprojects too
  '-Wshadow',
  '-Wconversion',
  language: 'c',
)

inc = include_directories('include')

core_src = files(
  'src/core/log.c',
  'src/core/proto.c',
  'src/core/descriptor.c',
  'src/core/exposure.c',
  'src/core/session.c',
  'src/core/image.c',
  'src/core/raw.c',
  'src/core/tiff.c',
  'src/core/ice.c',
  'src/core/flatdata.c',
)
core = static_library('mscore', core_src, include_directories: inc)

platform_src = files('src/platform/time.c')
if host_machine.system() == 'linux'
  platform_src += files('src/platform/time_posix.c')
else
  platform_src += files('src/platform/time_unsupported.c')
endif
platform = static_library('msplatform', platform_src, include_directories: inc)

transport_src = files('src/transport/transport.c')
if host_machine.system() == 'linux'
  transport_src += files('src/transport/linux_sg.c')
else
  transport_src += files('src/transport/unsupported.c')
endif

argtable3_dep = dependency('argtable3', version: '>=3.3.1', include_type: 'system')
m_dep = cc.find_library('m', required: false)
libtiff_dep = dependency('tiff', version: '>=4.7.2', include_type: 'system')

minscan = executable('minscan',
  ['src/main.c'] + transport_src,
  include_directories: inc,
  dependencies: [argtable3_dep, libtiff_dep, m_dep],
  link_with: [core, platform],
  install: true,
)


if get_option('build_tests')
  test_inc = include_directories('tests')
  testdata_dir = meson.current_source_dir() / 'tests' / 'testdata'
  foreach name : ['test_cdb', 'test_pages', 'test_descriptor', 'test_descriptor_build', 'test_session_guard', 'test_image', 'test_raw', 'test_ice', 'test_tiff', 'test_flat']
    test_src = files('tests/' + name + '.c')
    test_deps = [m_dep]
    if name == 'test_session_guard'
      test_src += transport_src
    endif
    if name == 'test_tiff'
      test_deps += libtiff_dep
    endif
    exe = executable(name,
      test_src,
      include_directories: [inc, test_inc],
      link_with: [core, platform],
      dependencies: test_deps,
      c_args: ['-DMS_TESTDATA_DIR="' + testdata_dir + '"'],
    )
    test(name, exe)
  endforeach
endif

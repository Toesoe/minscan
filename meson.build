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

# The embedding library has no TIFF writer, only the CLI and its tests need libtiff.
core_common_src = files(
  'src/core/log.c',
  'src/core/proto.c',
  'src/core/descriptor.c',
  'src/core/exposure.c',
  'src/core/session.c',
  'src/core/image.c',
  'src/core/ice.c',
)
core_src = core_common_src + files('src/core/tiff.c')

platform_src = files('src/platform/time.c')
if host_machine.system() == 'linux'
  platform_src += files('src/platform/time_posix.c')
else
  platform_src += files('src/platform/time_unsupported.c')
endif

# Static helpers are only for the CLI and its tests; the shared library compiles
# the same sources directly and must not pull in libtiff.
if get_option('build_cli') or get_option('build_tests')
  core = static_library('mscore', core_src, include_directories: inc)
  platform = static_library('msplatform', platform_src, include_directories: inc)
endif

transport_src = files('src/transport/transport.c')
if host_machine.system() == 'linux'
  transport_src += files('src/transport/linux_sg.c')
else
  transport_src += files('src/transport/unsupported.c')
endif

m_dep = cc.find_library('m', required: false)
if get_option('build_cli') or get_option('build_tests')
  libtiff_dep = dependency('tiff', version: '>=4.7.2', include_type: 'system')
endif
if get_option('build_cli')
  argtable3_dep = dependency('argtable3', version: '>=3.3.1', include_type: 'system')
endif

python_bindings = get_option('python_bindings')
if python_bindings
  py = import('python').find_installation()
  py.install_sources(
    'python/minscan/__init__.py',
    'python/minscan/_api.py',
    subdir: 'minscan',
    pure: false,
  )
  shared_install_dir = py.get_install_dir(pure: false) / 'minscan'
else
  install_headers('include/minscan/capi.h', subdir: 'minscan')
  shared_install_dir = get_option('libdir')
endif

minscan_shared = shared_library('minscan',
  ['src/capi.c'] + core_common_src + platform_src + transport_src,
  include_directories: inc,
  dependencies: [m_dep],
  install: true,
  install_dir: shared_install_dir,
)

if not python_bindings
  # `pkg-config --cflags --libs minscan` for embedders such as NegPy.
  pkg = import('pkgconfig')
  pkg.generate(minscan_shared,
    name: 'minscan',
    description: 'Minolta DiMAGE film scanner driver',
    version: meson.project_version(),
  )
endif

if get_option('build_cli')
  minscan = executable('minscan',
    ['src/main.c'] + transport_src,
    include_directories: inc,
    dependencies: [argtable3_dep, libtiff_dep, m_dep],
    link_with: [core, platform],
    install: true,
  )
endif


if get_option('build_tests')
  test_inc = include_directories('tests')
  testdata_dir = meson.current_source_dir() / 'tests' / 'testdata'
  foreach name : ['test_cdb', 'test_pages', 'test_descriptor', 'test_descriptor_build', 'test_session_guard', 'test_image', 'test_ice', 'test_tiff', 'test_linear', 'test_capi']
    test_src = files('tests/' + name + '.c')
    test_deps = [m_dep]
    if name == 'test_session_guard'
      test_src += transport_src
    endif
    if name == 'test_tiff'
      test_deps += libtiff_dep
    endif
    if name == 'test_capi'
      test_src += transport_src + files('src/capi.c')
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

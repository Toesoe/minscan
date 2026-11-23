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
project_args = [
  '-Werror',  # scoped to our targets; built-in werror=true would hit subprojects too
  '-Wshadow',
  '-Wconversion',
]
if host_machine.system() != 'windows'
  project_args += '-D_POSIX_C_SOURCE=200809L'
endif
add_project_arguments(project_args, language: 'c')

inc = include_directories('include')

# Layer boundaries: wire/ must not include scan/, session/ or image/; scan/ must not include
# session/ or image/; image/ must not include session/. Runs as part of `meson test`.
py_check = import('python').find_installation()
test('layer-check', py_check, args: [files('tools/check_layers.py')])

# The embedding library has no TIFF writer, only the CLI and its tests need libtiff.
core_common_src = files(
    'src/core/log.c',
    'src/core/wire/cdb.c',
    'src/core/wire/pages.c',
    'src/core/scan/exposure.c',
    'src/core/image/frame.c',
    'src/core/image/coreg.c',
    'src/core/image/ir.c',
    'src/core/image/shading.c',
    'src/core/image/gain.c',
    'src/core/image/balance.c',
    'src/core/image/ice.c',
    'src/core/image/pipeline.c',
    'src/core/wire/unpack.c',
    'src/core/wire/descriptor.c',
    'src/core/scan/window.c',
    'src/core/scan/ladder.c',
    'src/core/scan/job.c',
    'src/core/session/acquire.c',
    'src/core/session/calibrate.c',
    'src/core/session/exposure.c',
    'src/core/session/focus.c',
    'src/core/session/io.c',
    'src/core/session/probe.c',
    'src/core/session/stage.c',
)
core_src = core_common_src + files('src/core/image/tiff.c')

platform_src = files('src/platform/time.c')
if host_machine.system() == 'windows'
  platform_src += files('src/platform/time_win32.c')
elif cc.has_function('clock_gettime', prefix: '#define _POSIX_C_SOURCE 200809L\n#include <time.h>')
  platform_src += files('src/platform/time_posix.c')
else
  platform_src += files('src/platform/time_unsupported.c')
endif

m_dep = cc.find_library('m', required: false)
if get_option('build_cli') or get_option('build_tests')
  libtiff_dep = dependency(['libtiff-4', 'tiff'], version: '>=4.7.2', include_type: 'system')
endif

# Static helpers are only for the CLI and its tests; the shared library compiles
# the same sources directly and must not pull in libtiff.
if get_option('build_cli') or get_option('build_tests')
  core = static_library('mscore', core_src, include_directories: inc, dependencies: [libtiff_dep])
  platform = static_library('msplatform', platform_src, include_directories: inc)
endif

transport_src = files('src/transport/transport.c')
if host_machine.system() == 'windows'
  transport_src += files('src/transport/win32_spti.c')
elif host_machine.system() == 'linux'
  transport_src += files('src/transport/linux_sg.c')
else
  transport_src += files('src/transport/unsupported.c')
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
    [
      'src/cli/main.c',
      'src/cli/args.c',
      'src/cli/cancel.c',
      'src/cli/output.c',
      'src/cli/session_open.c',
      'src/cli/cmd_probe.c',
      'src/cli/cmd_scan.c',
      'src/cli/cmd_focus.c',
      'src/cli/cmd_eject.c',
      'src/cli/cmd_help.c',
    ] + transport_src,
    include_directories: inc,
    dependencies: [argtable3_dep, libtiff_dep, m_dep],
    link_with: [core, platform],
    install: true,
  )
endif


if get_option('build_tests')
  test_inc = include_directories('tests')
  testdata_dir = meson.current_source_dir() / 'tests' / 'scan' / 'testdata'
  foreach path : [
    'wire/test_cdb', 'wire/test_pages', 'wire/test_descriptor',
    'scan/test_descriptor_build',
    'session/test_session_guard',
    'image/test_image', 'image/test_ice', 'image/test_tiff', 'image/test_linear',
    'capi/test_capi',
  ]
    name = path.replace('/', '_')
    test_src = files('tests/' + path + '.c')
    test_deps = [m_dep]
    if path == 'session/test_session_guard'
      test_src += transport_src
    endif
    if path == 'image/test_tiff'
      test_deps += libtiff_dep
    endif
    if path == 'capi/test_capi'
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

project('xprism-control', 'c',
  version: '26.11.01',
  license: 'MIT',
  meson_version: '>= 0.60',
  default_options: ['c_std=c11', 'warning_level=2', 'buildtype=debugoptimized'])

add_project_arguments('-D_GNU_SOURCE', language: 'c')

xcb = dependency('xcb', version: '>= 1.13')
xcb_randr = dependency('xcb-randr')
xcb_res = dependency('xcb-res')

libxpctl = static_library('xpctl', 'lib/xpctl.c',
  dependencies: [xcb, xcb_randr, xcb_res])
xpctl = declare_dependency(link_with: libxpctl,
  include_directories: include_directories('lib'),
  dependencies: [xcb, xcb_randr, xcb_res])

xprismctl = executable('xprismctl', 'src/xprismctl.c',
  dependencies: xpctl, install: true)

gtk = dependency('gtk4', required: get_option('gui'))
if gtk.found()
  executable('xprism-control-panel', 'src/xprism-control-panel.c',
    dependencies: [xpctl, gtk], install: true)
  install_data('data/xprism-control-panel.desktop',
    install_dir: get_option('datadir') / 'applications')
endif

install_man('man/xprismctl.1')
# the layout kept in the control panel, back at login (XDG autostart)
install_data('data/xprism-displays-restore.desktop',
  install_dir: get_option('sysconfdir') / 'xdg' / 'autostart')

xwin = executable('xwin', 'tests/xwin.c', dependencies: xcb)
test('xprismctl', find_program('tests/run-tests.sh'),
  args: [xprismctl.full_path(), xwin.full_path()],
  depends: [xprismctl, xwin],
  env: ['XPRISM_PANEL=' + (gtk.found() ? meson.current_build_dir() / 'xprism-control-panel' : '')],
  timeout: 120, is_parallel: false)

# SPDX-License-Identifier: MIT

# Shared by the BGLView renderer add-on and by libEGL: both need to open the
# IntelGfx device and build an Iris pipe_screen. Exported as variables so
# src/egl/meson.build, which meson processes after src/gallium, can reuse them
# without repeating the dependency list.
inc_intelgfx = [inc_include, inc_src, inc_mapi, inc_mesa,
  inc_gallium, inc_gallium_aux, inc_util, inc_haikugl,
  inc_gallium_drivers, inc_gallium_winsys,
  include_directories('../../frontends/hgl'),
  include_directories('../../../intel/haiku/compat'),
  include_directories('.')]

deps_intelgfx = [driver_iris, cpp.find_library('be'),
  cpp.find_library('translation'), cpp.find_library('network'),
  dep_unwind, idep_mesautil, idep_nir, idep_xmlconfig]

link_intelgfx = [libglapi, libsthgl, libcompiler, libmesa, libglsl, libgallium]

libintelgfx_screen = static_library(
  'intelgfx_screen',
  files('IntelScreen.cpp', 'IntelWinsys.cpp'),
  include_directories : inc_intelgfx,
  cpp_args : ['-std=c++17'],
  gnu_symbol_visibility : 'hidden',
  dependencies : deps_intelgfx,
)

libhaiku_iris = shared_library(
  'haiku-iris',
  files('IntelRenderer.cpp', 'GalliumContext.cpp'),
  include_directories : inc_intelgfx,
  cpp_args : ['-std=c++17'],
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  link_with : [libintelgfx_screen, link_intelgfx, libgl],
  dependencies : deps_intelgfx,
  install : true,
  install_tag : 'haiku-iris',
)

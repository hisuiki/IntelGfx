# SPDX-License-Identifier: MIT
libhaiku_iris = shared_library(
  'haiku-iris',
  files('IntelRenderer.cpp', 'GalliumContext.cpp', 'IntelScreen.cpp', 'IntelWinsys.cpp'),
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa,
    inc_gallium, inc_gallium_aux, inc_util, inc_haikugl,
    inc_gallium_drivers, inc_gallium_winsys,
    include_directories('../../frontends/hgl'),
    include_directories('../../../intel/haiku/compat')],
  cpp_args : ['-std=c++17'],
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  link_with : [libglapi, libsthgl, libcompiler, libmesa, libglsl, libgallium, libgl],
  dependencies : [driver_iris, cpp.find_library('be'), cpp.find_library('translation'),
    cpp.find_library('network'), dep_unwind, idep_mesautil, idep_nir,
    idep_xmlconfig],
  install : true,
  install_tag : 'haiku-iris',
)

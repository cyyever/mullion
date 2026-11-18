# tests of source that also builds for the host, run without a GPU or Wine: the suite 'host', which CI runs whole
test('host_geometry_budget', executable('host_geometry_budget', 'geometry_budget.cpp',
                                      include_directories : mullion_native_include_path,
                                      override_options : ['cpp_std=c++23'], native : true), suite : 'host')
test('host_format', executable('host_format', ['format.cpp', '../../src/util/util_string.cpp'], include_directories : mullion_native_include_path,
                             override_options : ['cpp_std=c++23'], native : true), suite : 'host')
test('host_rc_self_move', executable('host_rc_self_move', 'rc.cpp',
                                    override_options : ['cpp_std=c++23'], native : true), suite : 'host')
test('host_packed_variant_list', executable('host_packed_variant_list', 'packed_variant_list.cpp',
                                           override_options : ['cpp_std=c++23'], native : true), suite : 'host')
test('host_denorm', executable('host_denorm', 'denorm.cpp', include_directories : include_directories('metal'),
                             override_options : ['cpp_std=c++23'], native : true), suite : 'host')
test('host_tessellator',
     executable('host_tessellator', ['tessellator.cpp', '../reference/reference.cpp'],
                include_directories : include_directories('metal', '../../src/airconv', '../../src/airconv/shaders', '../../libs'),
                # the reference's and the Metal source's, neither written for this compiler's warnings
                cpp_args : ['-w'], override_options : ['cpp_std=c++23'], native : true),
     suite : 'host', timeout : 300)
# the converter's own functions, built for the host as the bridge links them
test('host_readerio_frame', executable('host_readerio_frame', 'readerio_frame.cpp',
     include_directories : llvm_include_path_darwin, dependencies : airconv_dep_darwin,
     native : true), suite : 'host')
test('host_ray_dispatchers',
     executable('host_ray_dispatchers', 'ray_dispatchers.cpp', dependencies : airconv_dep_darwin,
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     suite : 'host', timeout : 120)
# which DXIL operations shaders use, for tests/coverage.py; it reads DXIL with the LLVM the converter is built with
executable('dxilops', ['dxilops.cpp'], include_directories : llvm_include_path_darwin,
           link_args : [ llvm_ld_flags_darwin, llvm_deps ], cpp_args : ['-w'], override_options : ['cpp_std=c++23'], native : true)
# the tests' own container hash against the hashes real shaders carry; it wants directories of them (MULLION_HASHED)
test('host_dxbc_hash', executable('host_dxbc_hash', ['dxbchash.cpp'], override_options : ['cpp_std=c++23'], native : true),
     suite : 'host')
# the bridge's shader store, which is Objective-C built as the bridge builds it
test('host_shader_store',
     executable('host_shader_store', ['shader_store.c', '../../src/winemetal/unix/cache.c'],
                include_directories : [mullion_include_path, include_directories('../../src/winemetal', '../../src/airconv')],
                c_args : ['-ObjC', '-w'],
                # the native architecture reaches only C++ links (the project's link arguments)
                link_args : ['-arch', native_compiler_arch, '-framework', 'Foundation', '-framework', 'Metal',
                             '-lsqlite3'],
                native : true),
     suite : 'host', timeout : 120)
test('host_diag_profile', find_program('python3', native : true), args : files('diag.py'), suite : 'host')
test('host_mutate', find_program('python3', native : true), args : files('mutate.py'), suite : 'host')
test('host_install', find_program('python3', native : true),
     args : files('install.py') + [meson.project_source_root() / 'install.sh'], suite : 'host')
test('host_lock', find_program('python3', native : true),
     args : files('lock.py') + [meson.project_source_root() / 'tests/run.py'], suite : 'host')
test('host_admission', find_program('python3', native : true),
     args : files('admission.py') + [meson.project_source_root() / 'tests/run.py'], suite : 'host')
test('host_frames', find_program('python3', native : true),
     args : files('frames.py') + [meson.project_source_root() / 'tools/frames.py'], suite : 'host')

# Requires a Metal device; the host suite intentionally needs no GPU.
test('air_bitcode', executable('air_bitcode', 'air_bitcode.cpp',
  include_directories : llvm_include_path_darwin,
  dependencies : airconv_dep_darwin,
  cpp_args : ['-x', 'objective-c++', '-fobjc-arc'],
  link_args : ['-framework', 'Foundation', '-framework', 'Metal'],
  native : true), suite : 'native-metal')

test('air_attributes', executable('air_attributes', 'air_attributes.cpp',
  include_directories : llvm_include_path_darwin,
  dependencies : airconv_dep_darwin,
  native : true), suite : 'host')

test('metal_icb_zero_row', executable('metal_icb_zero_row', 'metal_icb_zero_row.cpp',
  cpp_args : ['-x', 'objective-c++', '-fobjc-arc'],
  link_args : ['-framework', 'Foundation', '-framework', 'Metal'],
  native : true), suite : 'native-metal')

# The query-based rule requires a clang-tidy built with custom-check support.
air_tidy = find_program('clang-tidy', native : true, required : false)
if air_tidy.found()
  test('host_air_tidy', find_program('python3', native : true),
       args : [files('air_tidy.py'), air_tidy.full_path()], suite : 'host', timeout : 90)
endif

optimization_test = executable('host_optimization', 'optimization.cpp',
  include_directories : llvm_include_path_darwin, dependencies : airconv_dep_darwin, native : true)
test('host_optimization', optimization_test, suite : 'host')
test('host_optimization_verify_each', optimization_test, env : ['MULLION_VERIFY_EACH=1'], suite : 'host')

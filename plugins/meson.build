if not get_option('plugins')
  subdir_done()
endif

qemu_plugin_symbols = configure_file(
  input: files('../include/qemu/qemu-plugin.h'),
  output: 'qemu-plugin.symbols',
  capture: true,
  command: [files('../scripts/qemu-plugin-symbols.py'), '@INPUT@'])

# Modules need more symbols than just those in plugins/qemu-plugins.symbols
if not enable_modules
  if host_os == 'darwin'
    configure_file(
      input: qemu_plugin_symbols,
      output: 'qemu-plugins-ld64.symbols',
      capture: true,
      command: ['sed', '-ne', 's/^[[:space:]]*\\(qemu_.*\\);/_\\1/p', '@INPUT@'])
    emulator_link_args += ['-Wl,-exported_symbols_list,plugins/qemu-plugins-ld64.symbols']
  else
    emulator_link_args += ['-Xlinker', '--dynamic-list=' + qemu_plugin_symbols.full_path()]
  endif
endif

if host_os == 'windows'
  dlltool = find_program('dlltool', required: true)

  # Generate a .lib file for plugins to link against.
  # First, create a .def file listing all the symbols a plugin should expect to have
  # available in qemu
  win32_plugin_def = configure_file(
    input: qemu_plugin_symbols,
    output: 'qemu_plugin_api.def',
    capture: true,
    command: ['sed', '-e', '0,/^/s//EXPORTS/; s/[{};]//g', '@INPUT@'])
  # then use dlltool to assemble a delaylib.
  win32_qemu_plugin_api_lib = configure_file(
    input: win32_plugin_def,
    output: 'libqemu_plugin_api.a',
    command: [dlltool, '--input-def', '@INPUT@',
              '--output-delaylib', '@OUTPUT@', '--dllname', 'qemu.exe']
  )
endif
specific_ss.add(files(
  'loader.c',
  'core.c',
  'api.c',
))

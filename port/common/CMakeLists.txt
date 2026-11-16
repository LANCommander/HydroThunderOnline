# Windows XP stand-ins for the Vista+ functions the static CRT imports (winxp_crt.c), linked into both executables
# as objects so that they take precedence over kernel32.lib. They're built here, not from port/CMakeLists.txt, so
# that MASM's object lands in a flat directory: the Visual Studio generator doesn't create subdirectories for it.
add_library(winxp_crt OBJECT winxp_crt.c winxp_crt.asm)
target_compile_definitions(winxp_crt PRIVATE _WIN32_WINNT=0x0501 WINVER=0x0501 WIN32_LEAN_AND_MEAN)
set_source_files_properties(winxp_crt.asm PROPERTIES COMPILE_OPTIONS /safeseh)

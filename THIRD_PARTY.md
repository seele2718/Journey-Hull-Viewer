# Third-party code

MinHook is statically linked into the rendering DLL. Vendored source revision:
`c3fcafdc10146beb5919319d0683e44e3c30d537` from
<https://github.com/TsudaKageyu/minhook>.

Its source and license are in `third_party/minhook/`. Distributions include
`MinHook-LICENSE.txt`. Copyright notices for MinHook and the included Hacker
Disassembler Engine are retained.

The project builds with MinGW-w64 and links the Windows system runtime; GCC
support code is statically linked under its applicable runtime exception.

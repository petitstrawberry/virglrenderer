Metal compute buffers
=====================

``-Dmetal-compute=enabled`` builds a macOS-only buffer compute backend using
shaderc, SPIRV-Cross, Foundation and Metal. ``auto`` enables it when those build
dependencies are present; ``disabled`` retains the normal GL implementation.
The backend is selected only when the host desktop GL lacks compute shaders
and a Metal device is available. GL rendering and the VirGL protocol stay in
place; no new guest command or QEMU command-line option is required.

TGSI compute shaders use the existing GLSL translator, with Vulkan-compatible
constant block declarations, then shaderc produces SPIR-V and SPIRV-Cross
produces MSL. Metal libraries and pipeline states belong to the shader variant
and are released with it. Shader semantics are compiled, not matched to shader
names or replaced with fixed programs. Metal fast math is disabled.

Each dispatch retires prior GL operations and reads each distinct SSBO allocation
once. Descriptor slots referring to the same GL allocation share a Metal buffer,
including different descriptor offsets. After Metal command completion, bytes
are written into the same GL buffer objects before VirGL fencing can complete.
This deliberately synchronous bridge incurs CPU copies and does not provide
zero-copy GL/Metal interoperability. Normal GL copy-buffer bindings are restored.

The initial profile supports four SSBO bindings, buffer allocations up to 1 MiB,
up to 16 KiB of inline constants, fixed workgroups with at most 128 invocations,
axis limits of 128/128/64, and up to 65535 groups per axis. Indirect dispatch,
separate UBO objects, textures/images, descriptor arrays and dynamic shared
memory are rejected. Fragment SSBO support and a higher GL version are not
advertised. Compute limits are reported separately without pretending that CGL
implements the missing GL functions. Native GL compute hosts retain their
original path.

Build and validate using Meson with the dependencies on the pkg-config path::

  meson setup build -Dmetal-compute=enabled -Dplatforms=[]
  meson compile -C build
  meson test -C build --print-errorlogs

Or use ``nix build`` for the pinned native package. The public renderer API test
sends real shader/buffer/dispatch packets through a CGL-backed VirGL context,
checks two GPU read-modify-write dispatches, descriptor aliases at nonzero slots,
offsets, unchanged boundary bytes and fenced GPU readback. Hosts without Metal
or an accelerated CGL context skip the hardware test explicitly. A skip is not
positive GPU execution validation.

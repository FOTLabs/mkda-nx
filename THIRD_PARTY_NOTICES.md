# Third-party notices

## Shipped source

- **XboxRecomp**, sp00nz and contributors: MIT. The full text is `sdk/LICENSE`; contributors are listed in `sdk/CONTRIBUTORS.md`. `sdk/` is a modified copy of upstream XboxRecomp at commit `55be5cf (MKDA development fork)`, with this port's runtime and code-generator fixes applied.
- **xemu-derived MCPX APU and NV2A register definitions**: LGPL-2.1-or-later, copyright espes, Jannik Vogel, Matt Borgerson and other named contributors. The file list and copyrights are in `sdk/NOTICE`; the licence is `sdk/LICENSES/LGPL-2.1.txt`. Changes to these files stay under the same licence.
- Algorithms and hardware references credited by the toolkit keep the attribution in `sdk/NOTICE` and in the source headers.

## External build and runtime dependencies

These are not vendored. Keep the notices that ship with the versions you build against.

| Component | Licence |
| --- | --- |
| Mesa OpenGL, EGL, Nouveau | https://docs.mesa3d.org/license.html (mainly MIT, plus per-file notices) |
| libnx | https://github.com/switchbrew/libnx/blob/master/LICENSE.md (ISC) |
| SDL2 | https://github.com/libsdl-org/SDL/blob/SDL2/LICENSE.txt (zlib) |
| devkitA64 GCC runtime, newlib, zlib, DRM | Notices shipped with the installed packages |
| Capstone (build-time only) | https://github.com/capstone-engine/capstone/blob/next/LICENSE.TXT (BSD) |

Copies of the main licence texts are in `docs/licenses/`. Binary releases include corresponding source and relinking material for the LGPL portions.

The browser installer is adapted from FOTLabs/buffy-nx (MIT), originally inspired by StevensND/nfsmw-nx. It is project code with no bundled JavaScript dependencies, analytics or remote processing of game files.

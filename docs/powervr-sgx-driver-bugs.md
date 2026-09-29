# PowerVR SGX / Imagination GLES driver bug catalog

Curated survey of known Imagination PowerVR driver bugs — collected from Chromium's
`gpu_driver_bug_list.json` (a decade of production evidence from Google's fleet),
Imagination's own performance guides, and this project's own SGX530 + DDK 1.6
recovery work.

**Applicability to this board.** SGX530 + DDK 1.6.16.3977 (openpvrsgx) is an
Imagination PowerVR SGX 5.x-series driver reporting GLES 2.0. Any Chromium entry
matching `Imagination.*` at `gl_version <= 3.0`, or matching `PowerVR SGX 5.*`
specifically, is a direct hit; entries matching later families (`PowerVR G6xxx`,
`Rogue GE8*`, `BXM-*`) are the same vendor's driver stack and often share the
underlying architectural cause.

Chromium's blocklist file:
`chromium/src/gpu/config/gpu_driver_bug_list.json`
(https://chromium.googlesource.com/chromium/src/+/HEAD/gpu/config/gpu_driver_bug_list.json)

Chromium's workaround feature flags:
`chromium/src/gpu/config/gpu_driver_bug_workaround_type.h`

## What we've confirmed on THIS board

### `glClear(GL_DEPTH_BUFFER_BIT)` on an `EGL_DEPTH_SIZE=0` config → hardware recovery storm

- **Isolated 2026-09-23** by full state-bit bisection (`tools/tri-inline.c`,
  bitmask `STATE` narrowed to `STF_CLEAR_DS=1`, then CLR bisect to CD-only).
  Bisection chain:
  - Half A (0x8F) → STORM
  - HA-1 (0x09) → STORM
  - CLEAR_DS alone (0x01) → STORM
  - CLR=3 (color+depth) → STORM (HWR=3 in 5 frames, reproduced twice)
  - CLR=2 (color+stencil) → CLEAN (HWR=0 in 30 frames)
- **Mechanism** (from Imagination docs): on the tile-based deferred renderer,
  `glClear` is not a raster clear — it's a tile-store control signal instructing
  the driver **not to load the attachment's previous contents from system memory**.
  When the app asserts `GL_DEPTH_BUFFER_BIT` but the EGL config has no depth
  attachment, the fast-clear-path metadata handling for the missing attachment
  is undefined; DDK 1.6 programs the ISP to clear a nonexistent target, the TA
  render stalls, and the ukernel watchdog fires per frame.
- **Corresponds to Chromium entry id 95** (see below): "glClear does not always
  work on these drivers" → `gl_clear_broken`, matching `Imagination.*` at
  `gl_version <= 3.0`. Chromium has coded around this class of bug since ~2014.
- **Our workaround (kernel/tools/nanovg path):** patch nanovg's
  `glnvg__renderFlush` to strip `GL_DEPTH_BUFFER_BIT` from its per-frame
  `glClear` when the current EGL config reports no depth attachment. Narrower
  than Chromium's full "replace glClear with draw-quad" mitigation, but
  sufficient for our failure mode.

### Discipline note

- Never issue a reboot to a board that is still handling hardware recovery. The
  ukernel is in the middle of a poweroff/reset dance with the SGX core;
  a userspace `reboot -f` on top of that will wedge the kernel hard enough to
  need a physical power cycle. Schedule the reboot **before** the next storming
  test, not after.

## Chromium's confirmed-broken list for PowerVR / Imagination

Ordered by relevance to SGX 5.x + GLES 2.0. Each entry cites Chromium bug list
`id:` (canonical) and its Chromium workaround feature.

### Directly applicable to us (SGX 5.x / GLES ≤ 3.0)

|  id | Bug                                                                                       | Workaround feature                                        | Match                               |
| --: | ----------------------------------------------------------------------------------------- | --------------------------------------------------------- | ----------------------------------- |
|  95 | **glClear does not always work on these drivers**                                         | `gl_clear_broken` (replace glClear with full-screen draw) | `Imagination.*` + GLES ≤ 3.0        |
|  42 | Framebuffer discarding causes flickering                                                  | `disable_discard_framebuffer`                             | `Imagination.*` + `PowerVR SGX 5.*` |
|  91 | ETC1 non-power-of-two sized textures **crash** older IMG drivers                          | `etc1_power_of_two_only`                                  | `Imagination.*` + `PowerVR SGX 5.*` |
| 106 | `EXT_occlusion_query_boolean` **hangs** on PowerVR SGX 544                                | disable extension                                         | `PowerVR SGX 544`                   |
|  98 | PowerVR SGX 540 driver throws `GL_OUT_OF_MEMORY` when a buffer object's size is set to 0  | `use_non_zero_size_for_client_side_stream_buffers`        | `PowerVR SGX 540`                   |
|   1 | Imagination driver dislikes constant buffer uploads                                       | `use_client_side_arrays_for_stream_buffers`               | `Imagination.*` + GLES < 3.0        |
|  22 | Imagination drivers are buggy with context switching                                      | `unbind_fbo_on_context_switch`                            | `Imagination.*`                     |
| 263 | Program link fails in PowerVR SGX54x if `gl_Position` is not written in every path        | `init_gl_position_in_vertex_shader`                       | `PowerVR SGX 54.*`                  |
| 180 | `eglCreateImageKHR` fails for one-component textures on PowerVR                           | `avoid_one_component_egl_images`                          | `PowerVR .*`                        |
| 471 | IMG drivers can reference previously-bound complete framebuffers                          | `ensure_previous_framebuffer_not_deleted`                 | `Imagination.*`                     |
| 472 | Compiler bug validating `GL_MAX_*_UNIFORM_BLOCKS` at link time — validate at compile time | `validate_max_per_stage_uniform_blocks_at_compile_time`   | `Imagination.*`                     |
| 496 | Program binary cache entries can collide                                                  | `disable_program_cache`                                   | `Imagination.*` (ANGLE GLES)        |

### Same driver family / newer parts (Rogue, G6xxx, BXM)

Not directly applicable to SGX 5.x, but same vendor's driver architecture, so
often the same underlying bug class — worth checking if we ever port up.

|  id | Bug                                                                     | Workaround                                           |
| --: | ----------------------------------------------------------------------- | ---------------------------------------------------- |
| 262 | Program fails if `gl_Position` not set (`PowerVR .* GX6250`)            | `init_gl_position_in_vertex_shader`                  |
| 264 | Same, `PowerVR .* G6200`                                                | `init_gl_position_in_vertex_shader`                  |
| 299 | Context-lost **recovery often fails** on `PowerVR Rogue GE8.*`          | `exit_on_context_lost`                               |
| 313 | Context-lost recovery often fails on PowerVR (ChromeOS, all Rogue-era)  | `exit_on_context_lost`                               |
| 466 | Program binaries don't contain transform-feedback varyings              | `disable_program_caching_for_transform_feedback`     |
| 482 | Split full-image level-0 PBO uploads via `TexSubImage2D` into two calls | `split_level_0_pbo_full_sub_image_2d`                |
| 483 | Reset texture base level before `glCompressedTex*Image2D` for ASTC      | `reset_base_level_for_astc_image`                    |
| 485 | Compiler bug: limit declared varying output components at compile time  | `limit_output_varyings_at_compile_time`              |
| 486 | Changing `BASE_LEVEL` of NPOT immutable texture triggers driver bug     | `dont_change_base_level_for_npot_immutable_textures` |
| 487 | Reattach texture to FBO after layer-count increase                      | `reattach_texture_to_fbo_after_layer_increase`       |
| 490 | Reset texture base level before `glTexStorage2D`                        | `reset_tex_storage_base_level`                       |
| 491 | Limit max texture image units to 13                                     | `max_texture_image_units_13`                         |
| 492 | Upload oversized nonzero mip levels via unpack buffer                   | (feature-flag)                                       |
| 494 | Use `TexSubImage2D` (not `TexImage2D`) for NPOT client-side uploads     | `use_tex_sub_image_for_client_data_npot_uploads`     |
| 501 | Older PowerVR drivers mishandle large 3D NPOT sizes in `texStorage3D`   | `round_up_3d_texture_size_to_pot_for_limit`          |

### Patterns visible in the catalog

The bug set clusters on four architectural weak points, and this shape holds
from SGX 5.x all the way to modern BXM parts:

1. **Attachment-load / tile-store metadata (fast-clear path).** `glClear`,
   framebuffer discard/invalidate, previously-bound complete FBO references,
   FBO-after-layer-change reattachment. Entries 42, 95, 471, 487.
2. **Texture-storage state around `BASE_LEVEL` / `TexStorage*`.** ASTC
   compressed uploads, immutable NPOT, oversized mip levels, NPOT client-side
   uploads, large NPOT 3D. Entries 483, 486, 490, 492, 494, 501.
3. **Shader compiler / program cache.** `gl_Position` liveness, uniform-block
   count validation, output varying count, program binary cache collisions,
   transform-feedback caching. Entries 263, 466, 472, 485, 496.
4. **API-level "shape of the call" fragility.** Zero-sized buffer objects
   (OOM), one-component EGL images, occlusion query hang, streaming buffer
   uploads. Entries 1, 98, 106, 180.

### Not-quite-corroboration but on the same forum

Imagination's own developer forum has an ongoing multi-year "long list of driver
issues" thread for the modern BXM-8-256 part; a 2025 WebGPU issue tracker entry
(`issuetracker.google.com/issues/520126488`) notes missing textures on Motorola
G56 with the same GPU. The pattern is consistent: even current parts ship
GLES/Vulkan drivers with functional bugs that Chrome ends up shipping ANGLE-level
workarounds for. This is the vendor's standing character.

## Imagination's own guidance that matters

From <https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/using-glclear-and-glcolormask-on-powervr.html>
and adjacent tile-based-rendering topics:

- On PowerVR (tile-based deferred renderer), `glClear` is **not** a raster clear
  op. It's a tile-store control signal telling the driver **not to load** the
  attachment's previous contents from system memory into the tile buffer at
  frame start. A full clear at the start of a frame takes the "fast clear
  path"; a partial clear devolves to a full-screen quad and causes overdraw.
- Recommends `glInvalidateFramebuffer(GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT)`
  as the correct way to signal "throw away depth/stencil at end of frame."
  Requires GLES 3.0. **DDK 1.6 exposes only GLES 2.0**, so this API is not
  available to us — the corresponding GLES 2.0 extension is
  `GL_EXT_discard_framebuffer`, which Chromium (entry 42) says is broken on
  SGX 5.x anyway (`disable_discard_framebuffer`). So on our board we have
  neither a correct clear semantics nor a correct discard hint — we simply
  have to avoid the codepath.

## Practical rules-of-thumb for this project

Derived from the catalog + our own DDK 1.6 recovery evidence:

- **Never assert a `GL_*_BUFFER_BIT` in `glClear` for an attachment the EGL
  config doesn't have.** Query `EGL_DEPTH_SIZE` / `EGL_STENCIL_SIZE` once at
  init and mask the clear-bits accordingly.
- **Do not rely on `GL_EXT_discard_framebuffer`.** Even if the extension
  string is present, entry 42 says it flickers on this GPU family. Just don't
  use it.
- **Prefer immutable texture storage created up-front, don't churn `BASE_LEVEL`
  or `TexStorage*` at runtime.** (Even though we're on SGX 5.x, not the newer
  parts these entries target, the state-machine class is the same.)
- **Ensure `gl_Position` is written in every path of every vertex shader**
  (entries 263, 262, 264). SGX54x has been observed link-failing without it.
- **Don't rely on program binary cache** for correctness — entry 496 says
  cache-entry collisions have been observed on Imagination.
- **Don't rely on driver-side context-loss recovery.** Entries 299 and 313
  point at `exit_on_context_lost` — treat context loss as fatal, tear down and
  re-init the process instead of trying to resume.
- **Assume streaming buffer uploads are a driver hazard.** Entry 1 recommends
  client-side arrays for streaming data on GLES < 3.0 Imagination. Big
  per-frame `glBufferData` calls are the exact shape that entry says to avoid.
- **When investigating a new stall, check the tile-store metadata path
  first.** Attachment-load / fast-clear / discard-framebuffer is the single
  most bug-prone subsystem in this driver family across every product
  generation.

## Cross-references

- [ddk16-linux72-port-notes.md](./ddk16-linux72-port-notes.md) — port state for
  our DDK 1.6 stack.
- [sgx-ddk16-stall-followups.md](./sgx-ddk16-stall-followups.md) — recovery
  history / IRQ fix / APM fix / device-memory investigation.
- [sgx103-ddk-recovery-history.md](./sgx103-ddk-recovery-history.md) — earlier
  DDK 1.03 recovery history.
- [lessons-learned.md](./lessons-learned.md) — general early-B4 hard-won notes.
- [`tools/tri-inline.c`](../tools/tri-inline.c) — the zero-nanovg-dependency
  single-file storm repro used to isolate the `glClear` bug.

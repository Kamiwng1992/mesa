/*
 * Copyright (C) 2017-2019 Alyssa Rosenzweig
 * Copyright (C) 2017-2019 Connor Abbott
 * Copyright (C) 2019 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <genxml/gen_macros.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include "decode.h"

#include "midgard/disassemble.h"
#include "bifrost/disassemble.h"
#include "bifrost/valhall/disassemble.h"

#define DUMP_UNPACKED(T, var, ...) { \
        pandecode_log(__VA_ARGS__); \
        pan_print(pandecode_dump_stream, T, var, (pandecode_indent + 1) * 2); \
}

#define DUMP_CL(T, cl, ...) {\
        pan_unpack(cl, T, temp); \
        DUMP_UNPACKED(T, temp, __VA_ARGS__); \
}

#define DUMP_SECTION(A, S, cl, ...) { \
        pan_section_unpack(cl, A, S, temp); \
        pandecode_log(__VA_ARGS__); \
        pan_section_print(pandecode_dump_stream, A, S, temp, (pandecode_indent + 1) * 2); \
}

#define DUMP_SECTION_CS_V10(A, S, cl, buf, buf_unk, ...) { \
        pan_section_unpack_cs_v10(cl, buf, buf_unk, A, S, temp); \
        pandecode_log(__VA_ARGS__); \
        pan_section_print(pandecode_dump_stream, A, S, temp, (pandecode_indent + 1) * 2); \
}

#define MAP_ADDR(T, addr, cl) \
        const uint8_t *cl = 0; \
        { \
                struct pandecode_mapped_memory *mapped_mem = pandecode_find_mapped_gpu_mem_containing(addr); \
                cl = pandecode_fetch_gpu_mem(mapped_mem, addr, pan_size(T)); \
        }

#define DUMP_ADDR(T, addr, ...) {\
        MAP_ADDR(T, addr, cl) \
        DUMP_CL(T, cl, __VA_ARGS__); \
}

/* Semantic logging type.
 *
 * Raw: for raw messages to be printed as is.
 * Message: for helpful information to be commented out in replays.
 *
 * Use one of pandecode_log or pandecode_msg as syntax sugar.
 */

enum pandecode_log_type {
        PANDECODE_RAW,
        PANDECODE_MESSAGE,
};

#define pandecode_log(...)  pandecode_log_typed(PANDECODE_RAW,      __VA_ARGS__)
#define pandecode_msg(...)  pandecode_log_typed(PANDECODE_MESSAGE,  __VA_ARGS__)

static unsigned pandecode_indent = 0;

static void
pandecode_make_indent(void)
{
        for (unsigned i = 0; i < pandecode_indent; ++i)
                fprintf(pandecode_dump_stream, "  ");
}

static void PRINTFLIKE(2, 3)
pandecode_log_typed(enum pandecode_log_type type, const char *format, ...)
{
        va_list ap;

        pandecode_make_indent();

        if (type == PANDECODE_MESSAGE)
                fprintf(pandecode_dump_stream, "// ");

        va_start(ap, format);
        vfprintf(pandecode_dump_stream, format, ap);
        va_end(ap);
}

static void
pandecode_log_cont(const char *format, ...)
{
        va_list ap;

        va_start(ap, format);
        vfprintf(pandecode_dump_stream, format, ap);
        va_end(ap);
}

/* To check for memory safety issues, validates that the given pointer in GPU
 * memory is valid, containing at least sz bytes. The goal is to eliminate
 * GPU-side memory bugs (NULL pointer dereferences, buffer overflows, or buffer
 * overruns) by statically validating pointers.
 */

static void
pandecode_validate_buffer(mali_ptr addr, size_t sz)
{
        if (!addr) {
                pandecode_msg("XXX: null pointer deref\n");
                return;
        }

        /* Find a BO */

        struct pandecode_mapped_memory *bo =
                pandecode_find_mapped_gpu_mem_containing(addr);

        if (!bo) {
                pandecode_msg("XXX: invalid memory dereference\n");
                return;
        }

        /* Bounds check */

        unsigned offset = addr - bo->gpu_va;
        unsigned total = offset + sz;

        if (total > bo->length) {
                pandecode_msg("XXX: buffer overrun. "
                                "Chunk of size %zu at offset %d in buffer of size %zu. "
                                "Overrun by %zu bytes. \n",
                                sz, offset, bo->length, total - bo->length);
                return;
        }
}

#if PAN_ARCH <= 5
/* Midgard's tiler descriptor is embedded within the
 * larger FBD */

static void
pandecode_midgard_tiler_descriptor(
                const struct mali_tiler_context_packed *tp,
                const struct mali_tiler_weights_packed *wp)
{
        pan_unpack(tp, TILER_CONTEXT, t);
        DUMP_UNPACKED(TILER_CONTEXT, t, "Tiler:\n");

        /* We've never seen weights used in practice, but they exist */
        pan_unpack(wp, TILER_WEIGHTS, w);
        bool nonzero_weights = false;

        nonzero_weights |= w.weight0 != 0x0;
        nonzero_weights |= w.weight1 != 0x0;
        nonzero_weights |= w.weight2 != 0x0;
        nonzero_weights |= w.weight3 != 0x0;
        nonzero_weights |= w.weight4 != 0x0;
        nonzero_weights |= w.weight5 != 0x0;
        nonzero_weights |= w.weight6 != 0x0;
        nonzero_weights |= w.weight7 != 0x0;

        if (nonzero_weights)
                DUMP_UNPACKED(TILER_WEIGHTS, w, "Tiler Weights:\n");
}
#endif /* PAN_ARCH <= 5 */

/* Information about the framebuffer passed back for
 * additional analysis */

struct pandecode_fbd {
        unsigned width;
        unsigned height;
        unsigned rt_count;
        bool has_extra;
};

#if PAN_ARCH == 4
static struct pandecode_fbd
pandecode_sfbd(uint64_t gpu_va, bool is_fragment, unsigned gpu_id)
{
        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing(gpu_va);
        const void *PANDECODE_PTR_VAR(s, mem, (mali_ptr) gpu_va);

        struct pandecode_fbd info = {
                .has_extra = false,
                .rt_count = 1
        };

        pandecode_log("Framebuffer:\n");
        pandecode_indent++;

        DUMP_SECTION(FRAMEBUFFER, LOCAL_STORAGE, s, "Local Storage:\n");
        pan_section_unpack(s, FRAMEBUFFER, PARAMETERS, p);
        DUMP_UNPACKED(FRAMEBUFFER_PARAMETERS, p, "Parameters:\n");

        const void *t = pan_section_ptr(s, FRAMEBUFFER, TILER);
        const void *w = pan_section_ptr(s, FRAMEBUFFER, TILER_WEIGHTS);

        pandecode_midgard_tiler_descriptor(t, w);

        pandecode_indent--;

        /* Dummy unpack of the padding section to make sure all words are 0.
         * No need to call print here since the section is supposed to be empty.
         */
        pan_section_unpack(s, FRAMEBUFFER, PADDING_1, padding1);
        pan_section_unpack(s, FRAMEBUFFER, PADDING_2, padding2);
        pandecode_log("\n");

        return info;
}
#endif /* PAN_ARCH == 4 */

#if PAN_ARCH >= 5
static void
pandecode_local_storage(uint64_t gpu_va)
{
        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing(gpu_va);
        const struct mali_local_storage_packed *PANDECODE_PTR_VAR(s, mem, (mali_ptr) gpu_va);
        DUMP_CL(LOCAL_STORAGE, s, "Local Storage:\n");
}

static void
pandecode_render_target(uint64_t gpu_va, unsigned gpu_id,
                        const struct MALI_FRAMEBUFFER_PARAMETERS *fb)
{
        pandecode_log("Color Render Targets:\n");
        pandecode_indent++;

        for (int i = 0; i < (fb->render_target_count); i++) {
                mali_ptr rt_va = gpu_va + i * pan_size(RENDER_TARGET);
                struct pandecode_mapped_memory *mem =
                        pandecode_find_mapped_gpu_mem_containing(rt_va);
                const struct mali_render_target_packed *PANDECODE_PTR_VAR(rtp, mem, (mali_ptr) rt_va);
                DUMP_CL(RENDER_TARGET, rtp, "Color Render Target %d:\n", i);
        }

        pandecode_indent--;
        pandecode_log("\n");
}
#endif /* PAN_ARCH >= 5 */

#if PAN_ARCH >= 6
static void
pandecode_sample_locations(const void *fb)
{
        pan_section_unpack(fb, FRAMEBUFFER, PARAMETERS, params);

        struct pandecode_mapped_memory *smem =
                pandecode_find_mapped_gpu_mem_containing(params.sample_locations);

        const u16 *PANDECODE_PTR_VAR(samples, smem, params.sample_locations);

        pandecode_log("Sample locations:\n");
        for (int i = 0; i < 33; i++) {
                pandecode_log("  (%d, %d),\n",
                                samples[2 * i] - 128,
                                samples[2 * i + 1] - 128);
        }
}
#endif /* PAN_ARCH >= 6 */

static void
pandecode_dcd(const struct MALI_DRAW *p,
              enum mali_job_type job_type,
              char *suffix, unsigned gpu_id);

#if PAN_ARCH >= 5
static struct pandecode_fbd
pandecode_mfbd_bfr(uint64_t gpu_va, bool is_fragment, unsigned gpu_id)
{
        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing(gpu_va);
        const void *PANDECODE_PTR_VAR(fb, mem, (mali_ptr) gpu_va);
        pan_section_unpack(fb, FRAMEBUFFER, PARAMETERS, params);

        struct pandecode_fbd info;

#if PAN_ARCH >= 6
#if PAN_ARCH < 10
        pandecode_sample_locations(fb);

        pan_section_unpack(fb, FRAMEBUFFER, PARAMETERS, bparams);
        unsigned dcd_size = pan_size(FRAME_DRAW);
        struct pandecode_mapped_memory *dcdmem =
                pandecode_find_mapped_gpu_mem_containing(bparams.frame_shader_dcds);

        if (bparams.pre_frame_0 != MALI_PRE_POST_FRAME_SHADER_MODE_NEVER) {
                const void *PANDECODE_PTR_VAR(dcd, dcdmem, bparams.frame_shader_dcds + (0 * dcd_size));
                pan_unpack(dcd, FRAME_DRAW, draw);
                pandecode_log("Pre frame 0:\n");
                pandecode_dcd(&draw, MALI_JOB_TYPE_FRAGMENT, "", gpu_id);
        }

        if (bparams.pre_frame_1 != MALI_PRE_POST_FRAME_SHADER_MODE_NEVER) {
                const void *PANDECODE_PTR_VAR(dcd, dcdmem, bparams.frame_shader_dcds + (1 * dcd_size));
                pan_unpack(dcd, FRAME_DRAW, draw);
                pandecode_log("Pre frame 1:\n");
                pandecode_dcd(&draw, MALI_JOB_TYPE_FRAGMENT, "", gpu_id);
        }

        if (bparams.post_frame != MALI_PRE_POST_FRAME_SHADER_MODE_NEVER) {
                const void *PANDECODE_PTR_VAR(dcd, dcdmem, bparams.frame_shader_dcds + (2 * dcd_size));
                pan_unpack(dcd, FRAME_DRAW, draw);
                pandecode_log("Post frame:\n");
                pandecode_dcd(&draw, MALI_JOB_TYPE_FRAGMENT, "", gpu_id);
        }
#endif
#endif /* PAN_ARCH >= 6 */

        pandecode_log("Multi-Target Framebuffer:\n");
        pandecode_indent++;

#if PAN_ARCH <= 5
        DUMP_SECTION(FRAMEBUFFER, LOCAL_STORAGE, fb, "Local Storage:\n");
#endif

        info.width = params.width;
        info.height = params.height;
        info.rt_count = params.render_target_count;
        DUMP_UNPACKED(FRAMEBUFFER_PARAMETERS, params, "Parameters:\n");

#if PAN_ARCH <= 5
        const void *t = pan_section_ptr(fb, FRAMEBUFFER, TILER);
        const void *w = pan_section_ptr(fb, FRAMEBUFFER, TILER_WEIGHTS);
        pandecode_midgard_tiler_descriptor(t, w);
#endif

        pandecode_indent--;
        pandecode_log("\n");

        gpu_va += pan_size(FRAMEBUFFER);

        info.has_extra = params.has_zs_crc_extension;

        if (info.has_extra) {
                struct pandecode_mapped_memory *mem =
                        pandecode_find_mapped_gpu_mem_containing(gpu_va);
                const struct mali_zs_crc_extension_packed *PANDECODE_PTR_VAR(zs_crc, mem, (mali_ptr)gpu_va);
                DUMP_CL(ZS_CRC_EXTENSION, zs_crc, "ZS CRC Extension:\n");
                pandecode_log("\n");

                gpu_va += pan_size(ZS_CRC_EXTENSION);
        }

        if (is_fragment)
                pandecode_render_target(gpu_va, gpu_id, &params);

        return info;
}
#endif /* PAN_ARCH >= 5 */

#if PAN_ARCH <= 7
static void
pandecode_attributes(const struct pandecode_mapped_memory *mem,
                            mali_ptr addr, char *suffix,
                            int count, bool varying, enum mali_job_type job_type)
{
        char *prefix = varying ? "Varying" : "Attribute";
        assert(addr);

        if (!count) {
                pandecode_msg("warn: No %s records\n", prefix);
                return;
        }

        MAP_ADDR(ATTRIBUTE_BUFFER, addr, cl);

        for (int i = 0; i < count; ++i) {
                pan_unpack(cl + i * pan_size(ATTRIBUTE_BUFFER), ATTRIBUTE_BUFFER, temp);
                DUMP_UNPACKED(ATTRIBUTE_BUFFER, temp, "%s:\n", prefix);

                switch (temp.type) {
                case MALI_ATTRIBUTE_TYPE_1D_NPOT_DIVISOR_WRITE_REDUCTION:
                case MALI_ATTRIBUTE_TYPE_1D_NPOT_DIVISOR: {
                        pan_unpack(cl + (i + 1) * pan_size(ATTRIBUTE_BUFFER),
                                   ATTRIBUTE_BUFFER_CONTINUATION_NPOT, temp2);
                        pan_print(pandecode_dump_stream, ATTRIBUTE_BUFFER_CONTINUATION_NPOT,
                                  temp2, (pandecode_indent + 1) * 2);
                        i++;
                        break;
                }
                case MALI_ATTRIBUTE_TYPE_3D_LINEAR:
                case MALI_ATTRIBUTE_TYPE_3D_INTERLEAVED: {
                        pan_unpack(cl + (i + 1) * pan_size(ATTRIBUTE_BUFFER_CONTINUATION_3D),
                                   ATTRIBUTE_BUFFER_CONTINUATION_3D, temp2);
                        pan_print(pandecode_dump_stream, ATTRIBUTE_BUFFER_CONTINUATION_3D,
                                  temp2, (pandecode_indent + 1) * 2);
                        i++;
                        break;
                }
                default:
                        break;
                }
        }
        pandecode_log("\n");
}
#endif /* PAN_ARCH <= 7 */

#if PAN_ARCH >= 6
/* Decodes a Bifrost blend constant. See the notes in bifrost_blend_rt */

static mali_ptr
pandecode_bifrost_blend(void *descs, int rt_no, mali_ptr frag_shader)
{
        pan_unpack(descs + (rt_no * pan_size(BLEND)), BLEND, b);
        DUMP_UNPACKED(BLEND, b, "Blend RT %d:\n", rt_no);
        if (b.internal.mode != MALI_BLEND_MODE_SHADER)
                return 0;

        return (frag_shader & 0xFFFFFFFF00000000ULL) | b.internal.shader.pc;
}
#elif PAN_ARCH == 5
static mali_ptr
pandecode_midgard_blend_mrt(void *descs, int rt_no)
{
        pan_unpack(descs + (rt_no * pan_size(BLEND)), BLEND, b);
        DUMP_UNPACKED(BLEND, b, "Blend RT %d:\n", rt_no);
        return b.blend_shader ? (b.shader_pc & ~0xf) : 0;
}
#endif /* PAN_ARCH >= 6 || PAN_ARCH == 5 */

#if PAN_ARCH <= 7
static unsigned
pandecode_attribute_meta(int count, mali_ptr attribute, bool varying)
{
        unsigned max = 0;

        for (int i = 0; i < count; ++i, attribute += pan_size(ATTRIBUTE)) {
                MAP_ADDR(ATTRIBUTE, attribute, cl);
                pan_unpack(cl, ATTRIBUTE, a);
                DUMP_UNPACKED(ATTRIBUTE, a, "%s:\n", varying ? "Varying" : "Attribute");
                max = MAX2(max, a.buffer_index);
        }

        pandecode_log("\n");
        return MIN2(max + 1, 256);
}

/* return bits [lo, hi) of word */
static u32
bits(u32 word, u32 lo, u32 hi)
{
        if (hi - lo >= 32)
                return word; // avoid undefined behavior with the shift

        if (lo >= 32)
                return 0;

        return (word >> lo) & ((1 << (hi - lo)) - 1);
}

static void
pandecode_invocation(const void *i)
{
        /* Decode invocation_count. See the comment before the definition of
         * invocation_count for an explanation.
         */
        pan_unpack(i, INVOCATION, invocation);

        unsigned size_x = bits(invocation.invocations, 0, invocation.size_y_shift) + 1;
        unsigned size_y = bits(invocation.invocations, invocation.size_y_shift, invocation.size_z_shift) + 1;
        unsigned size_z = bits(invocation.invocations, invocation.size_z_shift, invocation.workgroups_x_shift) + 1;

        unsigned groups_x = bits(invocation.invocations, invocation.workgroups_x_shift, invocation.workgroups_y_shift) + 1;
        unsigned groups_y = bits(invocation.invocations, invocation.workgroups_y_shift, invocation.workgroups_z_shift) + 1;
        unsigned groups_z = bits(invocation.invocations, invocation.workgroups_z_shift, 32) + 1;

        pandecode_log("Invocation (%d, %d, %d) x (%d, %d, %d)\n",
                      size_x, size_y, size_z,
                      groups_x, groups_y, groups_z);

        DUMP_UNPACKED(INVOCATION, invocation, "Invocation:\n")
}
#endif /* PAN_ARCH <= 7 */

#if PAN_ARCH < 10
static void
pandecode_primitive(const void *p)
{
        pan_unpack(p, PRIMITIVE, primitive);
        DUMP_UNPACKED(PRIMITIVE, primitive, "Primitive:\n");

#if PAN_ARCH <= 7
        /* Validate an index buffer is present if we need one. TODO: verify
         * relationship between invocation_count and index_count */

        if (primitive.indices) {
                /* Grab the size */
                unsigned size = (primitive.index_type == MALI_INDEX_TYPE_UINT32) ?
                        sizeof(uint32_t) : primitive.index_type;

                /* Ensure we got a size, and if so, validate the index buffer
                 * is large enough to hold a full set of indices of the given
                 * size */

                if (!size)
                        pandecode_msg("XXX: index size missing\n");
                else
                        pandecode_validate_buffer(primitive.indices, primitive.index_count * size);
        } else if (primitive.index_type)
                pandecode_msg("XXX: unexpected index size\n");
#endif /* PAN_ARCH <= 7 */
}

static void
pandecode_primitive_size(const void *s, bool constant)
{
        pan_unpack(s, PRIMITIVE_SIZE, ps);
        if (ps.size_array == 0x0)
                return;

        DUMP_UNPACKED(PRIMITIVE_SIZE, ps, "Primitive Size:\n")
}
#endif /* PAN_ARCH < 10 */

#if PAN_ARCH <= 7
static void
pandecode_uniform_buffers(mali_ptr pubufs, int ubufs_count)
{
        struct pandecode_mapped_memory *umem = pandecode_find_mapped_gpu_mem_containing(pubufs);
        uint64_t *PANDECODE_PTR_VAR(ubufs, umem, pubufs);

        for (int i = 0; i < ubufs_count; i++) {
                mali_ptr addr = (ubufs[i] >> 10) << 2;
                unsigned size = addr ? (((ubufs[i] & ((1 << 10) - 1)) + 1) * 16) : 0;

                pandecode_validate_buffer(addr, size);

                char *ptr = pointer_as_memory_reference(addr);
                pandecode_log("ubuf_%d[%u] = %s;\n", i, size, ptr);
                free(ptr);
        }

        pandecode_log("\n");
}

static void
pandecode_uniforms(mali_ptr uniforms, unsigned uniform_count)
{
        pandecode_validate_buffer(uniforms, uniform_count * 16);

        char *ptr = pointer_as_memory_reference(uniforms);
        pandecode_log("vec4 uniforms[%u] = %s;\n", uniform_count, ptr);
        free(ptr);
        pandecode_log("\n");
}
#endif /* PAN_ARCH <= 7 */

static const char *
shader_type_for_job(unsigned type)
{
        switch (type) {
#if PAN_ARCH <= 7
        case MALI_JOB_TYPE_VERTEX:  return "VERTEX";
#endif
        case MALI_JOB_TYPE_TILER:   return "FRAGMENT";
        case MALI_JOB_TYPE_FRAGMENT: return "FRAGMENT";
        case MALI_JOB_TYPE_COMPUTE: return "COMPUTE";
        default: return "UNKNOWN";
        }
}

static unsigned shader_id = 0;

static struct midgard_disasm_stats
pandecode_shader_disassemble(mali_ptr shader_ptr, int type, unsigned gpu_id)
{
        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing(shader_ptr);
        uint8_t *PANDECODE_PTR_VAR(code, mem, shader_ptr);

        /* Compute maximum possible size */
        size_t sz = mem->length - (shader_ptr - mem->gpu_va);

        /* Print some boilerplate to clearly denote the assembly (which doesn't
         * obey indentation rules), and actually do the disassembly! */

        pandecode_log_cont("\n\n");

        struct midgard_disasm_stats stats = { 0 };

#if PAN_ARCH >= 9
        disassemble_valhall(pandecode_dump_stream, (const uint64_t *) code, sz, true);
#elif PAN_ARCH >= 6 && PAN_ARCH <= 7
        disassemble_bifrost(pandecode_dump_stream, code, sz, false);
#else
	stats = disassemble_midgard(pandecode_dump_stream,
                                    code, sz, gpu_id, true);
#endif

        unsigned nr_threads =
                (stats.work_count <= 4) ? 4 :
                (stats.work_count <= 8) ? 2 :
                1;

        pandecode_log_cont("shader%d - MESA_SHADER_%s shader: "
                "%u inst, %u bundles, %u quadwords, "
                "%u registers, %u threads, 0 loops, 0:0 spills:fills\n\n\n",
                shader_id++,
                shader_type_for_job(type),
                stats.instruction_count, stats.bundle_count, stats.quadword_count,
                stats.work_count, nr_threads);

        return stats;
}

#if PAN_ARCH <= 7
static void
pandecode_texture_payload(mali_ptr payload,
                          enum mali_texture_dimension dim,
                          enum mali_texture_layout layout,
                          bool manual_stride,
                          uint8_t levels,
                          uint16_t nr_samples,
                          uint16_t array_size,
                          struct pandecode_mapped_memory *tmem)
{
        pandecode_log(".payload = {\n");
        pandecode_indent++;

        /* A bunch of bitmap pointers follow.
         * We work out the correct number,
         * based on the mipmap/cubemap
         * properties, but dump extra
         * possibilities to futureproof */

        int bitmap_count = levels;

        /* Miptree for each face */
        if (dim == MALI_TEXTURE_DIMENSION_CUBE)
                bitmap_count *= 6;

        /* Array of layers */
        bitmap_count *= nr_samples;

        /* Array of textures */
        bitmap_count *= array_size;

        /* Stride for each element */
        if (manual_stride)
                bitmap_count *= 2;

        mali_ptr *pointers_and_strides = pandecode_fetch_gpu_mem(tmem,
                payload, sizeof(mali_ptr) * bitmap_count);
        for (int i = 0; i < bitmap_count; ++i) {
                /* How we dump depends if this is a stride or a pointer */

                if (manual_stride && (i & 1)) {
                        /* signed 32-bit snuck in as a 64-bit pointer */
                        uint64_t stride_set = pointers_and_strides[i];
                        int32_t row_stride = stride_set;
                        int32_t surface_stride = stride_set >> 32;
                        pandecode_log("(mali_ptr) %d /* surface stride */ %d /* row stride */, \n",
                                      surface_stride, row_stride);
                } else {
                        char *a = pointer_as_memory_reference(pointers_and_strides[i]);
                        pandecode_log("%s, \n", a);
                        free(a);
                }
        }

        pandecode_indent--;
        pandecode_log("},\n");
}
#endif /* PAN_ARCH <= 7 */

#if PAN_ARCH <= 5
static void
pandecode_texture(mali_ptr u,
                struct pandecode_mapped_memory *tmem,
                unsigned tex)
{
        struct pandecode_mapped_memory *mapped_mem = pandecode_find_mapped_gpu_mem_containing(u);
        const uint8_t *cl = pandecode_fetch_gpu_mem(mapped_mem, u, pan_size(TEXTURE));

        pan_unpack(cl, TEXTURE, temp);
        DUMP_UNPACKED(TEXTURE, temp, "Texture:\n")

        pandecode_indent++;
        unsigned nr_samples = temp.dimension == MALI_TEXTURE_DIMENSION_3D ?
                              1 : temp.sample_count;
        pandecode_texture_payload(u + pan_size(TEXTURE),
                        temp.dimension, temp.texel_ordering, temp.manual_stride,
                        temp.levels, nr_samples, temp.array_size, mapped_mem);
        pandecode_indent--;
}
#else /* PAN_ARCH > 5 */
static void
pandecode_bifrost_texture(
                const void *cl,
                unsigned tex)
{
        pan_unpack(cl, TEXTURE, temp);
        DUMP_UNPACKED(TEXTURE, temp, "Texture:\n")

        pandecode_indent++;

#if PAN_ARCH >= 9
        /* TODO: count */
        for (unsigned i = 0; i < 4; ++i)
                DUMP_ADDR(PLANE, temp.surfaces + i * pan_size(PLANE), "Plane %u:\n", i);
#else /* PAN_ARCH < 9 */
        struct pandecode_mapped_memory *tmem = pandecode_find_mapped_gpu_mem_containing(temp.surfaces);
        unsigned nr_samples = temp.dimension == MALI_TEXTURE_DIMENSION_3D ?
                              1 : temp.sample_count;

        pandecode_texture_payload(temp.surfaces, temp.dimension, temp.texel_ordering,
                                  true, temp.levels, nr_samples, temp.array_size, tmem);
#endif
        pandecode_indent--;
}
#endif

#if PAN_ARCH <= 7
static void
pandecode_blend_shader_disassemble(mali_ptr shader, int job_type,
                                   unsigned gpu_id)
{
        struct midgard_disasm_stats stats =
                pandecode_shader_disassemble(shader, job_type, gpu_id);

        bool has_texture = (stats.texture_count > 0);
        bool has_sampler = (stats.sampler_count > 0);
        bool has_attribute = (stats.attribute_count > 0);
        bool has_varying = (stats.varying_count > 0);
        bool has_uniform = (stats.uniform_count > 0);
        bool has_ubo = (stats.uniform_buffer_count > 0);

        if (has_texture || has_sampler)
                pandecode_msg("XXX: blend shader accessing textures\n");

        if (has_attribute || has_varying)
                pandecode_msg("XXX: blend shader accessing interstage\n");

        if (has_uniform || has_ubo)
                pandecode_msg("XXX: blend shader accessing uniforms\n");
}

static void
pandecode_textures(mali_ptr textures, unsigned texture_count)
{
        struct pandecode_mapped_memory *mmem = pandecode_find_mapped_gpu_mem_containing(textures);

        if (!mmem)
                return;

        pandecode_log("Textures %"PRIx64":\n", textures);
        pandecode_indent++;

#if PAN_ARCH >= 6
        const void *cl =
                pandecode_fetch_gpu_mem(mmem,
                                        textures,
                                        pan_size(TEXTURE) *
                                        texture_count);

        for (unsigned tex = 0; tex < texture_count; ++tex) {
                pandecode_bifrost_texture(cl + pan_size(TEXTURE) * tex, tex);
        }
#else /* PAN_ARCH < 6 */
        mali_ptr *PANDECODE_PTR_VAR(u, mmem, textures);

        for (int tex = 0; tex < texture_count; ++tex) {
                mali_ptr *PANDECODE_PTR_VAR(u, mmem, textures + tex * sizeof(mali_ptr));
                char *a = pointer_as_memory_reference(*u);
                pandecode_log("%s,\n", a);
                free(a);
        }

        /* Now, finally, descend down into the texture descriptor */
        for (unsigned tex = 0; tex < texture_count; ++tex) {
                mali_ptr *PANDECODE_PTR_VAR(u, mmem, textures + tex * sizeof(mali_ptr));
                struct pandecode_mapped_memory *tmem = pandecode_find_mapped_gpu_mem_containing(*u);
                if (tmem)
                        pandecode_texture(*u, tmem, tex);
        }
#endif
        pandecode_indent--;
        pandecode_log("\n");
}

static void
pandecode_samplers(mali_ptr samplers, unsigned sampler_count)
{
        pandecode_log("Samplers %"PRIx64":\n", samplers);
        pandecode_indent++;

        for (int i = 0; i < sampler_count; ++i)
                DUMP_ADDR(SAMPLER, samplers + (pan_size(SAMPLER) * i), "Sampler %d:\n", i);

        pandecode_indent--;
        pandecode_log("\n");
}

static void
pandecode_dcd(const struct MALI_DRAW *p,
              enum mali_job_type job_type,
              char *suffix, unsigned gpu_id)
{
        struct pandecode_mapped_memory *attr_mem;

#if PAN_ARCH >= 5
        struct pandecode_fbd fbd_info = {
                /* Default for Bifrost */
                .rt_count = 1
        };
#endif

#if PAN_ARCH >= 6
        pandecode_local_storage(p->thread_storage & ~1);
#elif PAN_ARCH == 5
        if (job_type != MALI_JOB_TYPE_TILER) {
                pandecode_local_storage(p->thread_storage & ~1);
	} else {
                assert(p->fbd & MALI_FBD_TAG_IS_MFBD);
                fbd_info = pandecode_mfbd_bfr((u64) ((uintptr_t) p->fbd) & ~MALI_FBD_TAG_MASK,
                                              false, gpu_id);
        }
#else
        pandecode_sfbd((u64) (uintptr_t) p->fbd, false, gpu_id);
#endif

        int varying_count = 0, attribute_count = 0, uniform_count = 0, uniform_buffer_count = 0;
        int texture_count = 0, sampler_count = 0;

        if (p->state) {
                struct pandecode_mapped_memory *smem = pandecode_find_mapped_gpu_mem_containing(p->state);
                uint32_t *cl = pandecode_fetch_gpu_mem(smem, p->state, pan_size(RENDERER_STATE));

                pan_unpack(cl, RENDERER_STATE, state);

                if (state.shader.shader & ~0xF)
                        pandecode_shader_disassemble(state.shader.shader & ~0xF, job_type, gpu_id);

#if PAN_ARCH >= 6
                bool idvs = (job_type == MALI_JOB_TYPE_INDEXED_VERTEX);

                if (idvs && state.secondary_shader)
                        pandecode_shader_disassemble(state.secondary_shader, job_type, gpu_id);
#endif
                DUMP_UNPACKED(RENDERER_STATE, state, "State:\n");
                pandecode_indent++;

                /* Save for dumps */
                attribute_count = state.shader.attribute_count;
                varying_count = state.shader.varying_count;
                texture_count = state.shader.texture_count;
                sampler_count = state.shader.sampler_count;
                uniform_buffer_count = state.properties.uniform_buffer_count;

#if PAN_ARCH >= 6
                uniform_count = state.preload.uniform_count;
#else
                uniform_count = state.properties.uniform_count;
#endif

#if PAN_ARCH == 4
                mali_ptr shader = state.blend_shader & ~0xF;
                if (state.multisample_misc.blend_shader && shader)
                        pandecode_blend_shader_disassemble(shader, job_type, gpu_id);
#endif
                pandecode_indent--;
                pandecode_log("\n");

                /* MRT blend fields are used whenever MFBD is used, with
                 * per-RT descriptors */

#if PAN_ARCH >= 5
                if ((job_type == MALI_JOB_TYPE_TILER || job_type == MALI_JOB_TYPE_FRAGMENT) &&
                    (PAN_ARCH >= 6 || p->thread_storage & MALI_FBD_TAG_IS_MFBD)) {
                        void* blend_base = ((void *) cl) + pan_size(RENDERER_STATE);

                        for (unsigned i = 0; i < fbd_info.rt_count; i++) {
                                mali_ptr shader = 0;

#if PAN_ARCH >= 6
                                shader = pandecode_bifrost_blend(blend_base, i,
                                                                 state.shader.shader);
#else
                                shader = pandecode_midgard_blend_mrt(blend_base, i);
#endif
                                if (shader & ~0xF)
                                        pandecode_blend_shader_disassemble(shader, job_type,
                                                                           gpu_id);
                        }
                }
#endif /* PAN_ARCH >= 5 */
        } else
                pandecode_msg("XXX: missing shader descriptor\n");

        if (p->viewport) {
                DUMP_ADDR(VIEWPORT, p->viewport, "Viewport:\n");
                pandecode_log("\n");
        }

        unsigned max_attr_index = 0;

        if (p->attributes)
                max_attr_index = pandecode_attribute_meta(attribute_count, p->attributes, false);

        if (p->attribute_buffers) {
                attr_mem = pandecode_find_mapped_gpu_mem_containing(p->attribute_buffers);
                pandecode_attributes(attr_mem, p->attribute_buffers, suffix, max_attr_index, false, job_type);
        }

        if (p->varyings) {
                varying_count = pandecode_attribute_meta(varying_count, p->varyings, true);
        }

        if (p->varying_buffers) {
                attr_mem = pandecode_find_mapped_gpu_mem_containing(p->varying_buffers);
                pandecode_attributes(attr_mem, p->varying_buffers, suffix, varying_count, true, job_type);
        }

        if (p->uniform_buffers) {
                if (uniform_buffer_count)
                        pandecode_uniform_buffers(p->uniform_buffers, uniform_buffer_count);
                else
                        pandecode_msg("warn: UBOs specified but not referenced\n");
        } else if (uniform_buffer_count)
                pandecode_msg("XXX: UBOs referenced but not specified\n");

        /* We don't want to actually dump uniforms, but we do need to validate
         * that the counts we were given are sane */

        if (p->push_uniforms) {
                if (uniform_count)
                        pandecode_uniforms(p->push_uniforms, uniform_count);
                else
                        pandecode_msg("warn: Uniforms specified but not referenced\n");
        } else if (uniform_count)
                pandecode_msg("XXX: Uniforms referenced but not specified\n");

        if (p->textures)
                pandecode_textures(p->textures, texture_count);

        if (p->samplers)
                pandecode_samplers(p->samplers, sampler_count);
}

static void
pandecode_vertex_compute_geometry_job(const struct MALI_JOB_HEADER *h,
                                      const struct pandecode_mapped_memory *mem,
                                      mali_ptr job, unsigned gpu_id)
{
        struct mali_compute_job_packed *PANDECODE_PTR_VAR(p, mem, job);
        pan_section_unpack(p, COMPUTE_JOB, DRAW, draw);
        pandecode_dcd(&draw, h->type, "", gpu_id);

        pandecode_log("Vertex Job Payload:\n");
        pandecode_indent++;
        pandecode_invocation(pan_section_ptr(p, COMPUTE_JOB, INVOCATION));
        DUMP_SECTION(COMPUTE_JOB, PARAMETERS, p, "Vertex Job Parameters:\n");
        DUMP_UNPACKED(DRAW, draw, "Draw:\n");
        pandecode_indent--;
        pandecode_log("\n");
}
#endif /* PAN_ARCH <= 7 */

#if PAN_ARCH >= 6
static void
pandecode_bifrost_tiler_heap(mali_ptr gpu_va)
{
        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing(gpu_va);
        pan_unpack(PANDECODE_PTR(mem, gpu_va, void), TILER_HEAP, h);
        DUMP_UNPACKED(TILER_HEAP, h, "Bifrost Tiler Heap:\n");
}

static void
pandecode_bifrost_tiler(mali_ptr gpu_va)
{
        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing(gpu_va);
        pan_unpack(PANDECODE_PTR(mem, gpu_va, void), TILER_CONTEXT, t);

        if (t.heap)
                pandecode_bifrost_tiler_heap(t.heap);

        DUMP_UNPACKED(TILER_CONTEXT, t, "Bifrost Tiler:\n");
}

#endif /* PAN_ARCH >= 6 */

#if PAN_ARCH < 10
#if PAN_ARCH >= 6
#if PAN_ARCH <= 7
static void
pandecode_indexed_vertex_job(const struct MALI_JOB_HEADER *h,
                             const struct pandecode_mapped_memory *mem,
                             mali_ptr job, unsigned gpu_id)
{
        struct mali_indexed_vertex_job_packed *PANDECODE_PTR_VAR(p, mem, job);

        pandecode_log("Vertex:\n");
        pan_section_unpack(p, INDEXED_VERTEX_JOB, VERTEX_DRAW, vert_draw);
        pandecode_dcd(&vert_draw, h->type, "", gpu_id);
        DUMP_UNPACKED(DRAW, vert_draw, "Vertex Draw:\n");

        pandecode_log("Fragment:\n");
        pan_section_unpack(p, INDEXED_VERTEX_JOB, FRAGMENT_DRAW, frag_draw);
        pandecode_dcd(&frag_draw, MALI_JOB_TYPE_FRAGMENT, "", gpu_id);
        DUMP_UNPACKED(DRAW, frag_draw, "Fragment Draw:\n");

        pan_section_unpack(p, INDEXED_VERTEX_JOB, TILER, tiler_ptr);
        pandecode_log("Tiler Job Payload:\n");
        pandecode_indent++;
        pandecode_bifrost_tiler(tiler_ptr.address);
        pandecode_indent--;

        pandecode_invocation(pan_section_ptr(p, INDEXED_VERTEX_JOB, INVOCATION));
        pandecode_primitive(pan_section_ptr(p, INDEXED_VERTEX_JOB, PRIMITIVE));

        /* TODO: gl_PointSize on Bifrost */
        pandecode_primitive_size(pan_section_ptr(p, INDEXED_VERTEX_JOB, PRIMITIVE_SIZE), true);

        pan_section_unpack(p, INDEXED_VERTEX_JOB, PADDING, padding);
}
#endif /* PAN_ARCH <= 7 */
#endif /* PAN_ARCH >= 6 */

static void
pandecode_tiler_job(const struct MALI_JOB_HEADER *h,
                    const struct pandecode_mapped_memory *mem,
                    mali_ptr job, unsigned gpu_id)
{
        struct mali_tiler_job_packed *PANDECODE_PTR_VAR(p, mem, job);
        pan_section_unpack(p, TILER_JOB, DRAW, draw);
        pandecode_dcd(&draw, h->type, "", gpu_id);
        pandecode_log("Tiler Job Payload:\n");
        pandecode_indent++;

#if PAN_ARCH <= 7
        pandecode_invocation(pan_section_ptr(p, TILER_JOB, INVOCATION));
#endif

        pandecode_primitive(pan_section_ptr(p, TILER_JOB, PRIMITIVE));
        DUMP_UNPACKED(DRAW, draw, "Draw:\n");

#if PAN_ARCH >= 6
        pan_section_unpack(p, TILER_JOB, TILER, tiler_ptr);
        pandecode_bifrost_tiler(tiler_ptr.address);

        /* TODO: gl_PointSize on Bifrost */
        pandecode_primitive_size(pan_section_ptr(p, TILER_JOB, PRIMITIVE_SIZE), true);

#if PAN_ARCH >= 9
        DUMP_SECTION(TILER_JOB, INSTANCE_COUNT, p, "Instance count:\n");
        DUMP_SECTION(TILER_JOB, VERTEX_COUNT, p, "Vertex count:\n");
        DUMP_SECTION(TILER_JOB, SCISSOR, p, "Scissor:\n");
        DUMP_SECTION(TILER_JOB, INDICES, p, "Indices:\n");
#else
        pan_section_unpack(p, TILER_JOB, PADDING, padding);
#endif

#else /* PAN_ARCH < 6 */
        pan_section_unpack(p, TILER_JOB, PRIMITIVE, primitive);
        pandecode_primitive_size(pan_section_ptr(p, TILER_JOB, PRIMITIVE_SIZE),
                                 primitive.point_size_array_format == MALI_POINT_SIZE_ARRAY_FORMAT_NONE);
#endif
        pandecode_indent--;
        pandecode_log("\n");
}
#endif /* PAN_ARCH < 10 */

static void
pandecode_fragment_job(const struct pandecode_mapped_memory *mem,
                       mali_ptr job, uint32_t *cs_buf, uint32_t *cs_buf_unk,
                       unsigned gpu_id)
{
#if PAN_ARCH < 10
        struct mali_fragment_job_packed *PANDECODE_PTR_VAR(p, mem, job);
#endif

        pan_section_unpack_cs_v10(p, cs_buf, cs_buf_unk, FRAGMENT_JOB, PAYLOAD, s);

#if PAN_ARCH == 4
        pandecode_sfbd(s.framebuffer, true, gpu_id);
#else
        assert(s.framebuffer & MALI_FBD_TAG_IS_MFBD);

        struct pandecode_fbd info;

        info = pandecode_mfbd_bfr(s.framebuffer & ~MALI_FBD_TAG_MASK,
                                  true, gpu_id);
#endif

#if PAN_ARCH >= 5
        unsigned expected_tag = 0;

        /* Compute the tag for the tagged pointer. This contains the type of
         * FBD (MFBD/SFBD), and in the case of an MFBD, information about which
         * additional structures follow the MFBD header (an extra payload or
         * not, as well as a count of render targets) */

        expected_tag = MALI_FBD_TAG_IS_MFBD;
        if (info.has_extra)
                expected_tag |= MALI_FBD_TAG_HAS_ZS_RT;

        expected_tag |= MALI_FBD_TAG_IS_MFBD | (MALI_POSITIVE(info.rt_count) << 2);
#endif /* PAN_ARCH >= 5 */

        DUMP_UNPACKED(FRAGMENT_JOB_PAYLOAD, s, "Fragment Job Payload:\n");

#if PAN_ARCH >= 5
        /* The FBD is a tagged pointer */

        unsigned tag = (s.framebuffer & MALI_FBD_TAG_MASK);

        if (tag != expected_tag)
                pandecode_msg("XXX: expected FBD tag %X but got %X\n", expected_tag, tag);
#endif

        pandecode_log("\n");
}

#if PAN_ARCH < 10
static void
pandecode_write_value_job(const struct pandecode_mapped_memory *mem,
                          mali_ptr job)
{
        struct mali_write_value_job_packed *PANDECODE_PTR_VAR(p, mem, job);
        pan_section_unpack(p, WRITE_VALUE_JOB, PAYLOAD, u);
        DUMP_SECTION(WRITE_VALUE_JOB, PAYLOAD, p, "Write Value Payload:\n");
        pandecode_log("\n");
}

static void
pandecode_cache_flush_job(const struct pandecode_mapped_memory *mem,
                          mali_ptr job)
{
        struct mali_cache_flush_job_packed *PANDECODE_PTR_VAR(p, mem, job);
        pan_section_unpack(p, CACHE_FLUSH_JOB, PAYLOAD, u);
        DUMP_SECTION(CACHE_FLUSH_JOB, PAYLOAD, p, "Cache Flush Payload:\n");
        pandecode_log("\n");
}
#endif /* PAN_ARCH < 10 */

#if PAN_ARCH >= 9
static void
dump_fau(mali_ptr addr, unsigned count, const char *name)
{
        struct pandecode_mapped_memory *mem =
                pandecode_find_mapped_gpu_mem_containing(addr);
        const uint32_t *PANDECODE_PTR_VAR(raw, mem, addr);

        pandecode_validate_buffer(addr, count * 8);

        fprintf(pandecode_dump_stream, "%s:\n", name);
        for (unsigned i = 0; i < count; ++i) {
                fprintf(pandecode_dump_stream, "  %08X %08X\n",
                                raw[2*i], raw[2*i + 1]);
        }
        fprintf(pandecode_dump_stream, "\n");
}

static mali_ptr
pandecode_shader(mali_ptr addr, const char *label, unsigned gpu_id)
{
        MAP_ADDR(SHADER_PROGRAM, addr, cl);
        pan_unpack(cl, SHADER_PROGRAM, desc);

        assert(desc.type == 8);

        DUMP_UNPACKED(SHADER_PROGRAM, desc, "%s Shader:\n", label);
        pandecode_shader_disassemble(desc.binary, 0, gpu_id);
        return desc.binary;
}

static void
pandecode_resources(mali_ptr addr, unsigned size)
{
        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing(addr);
        const uint8_t *cl = pandecode_fetch_gpu_mem(mem, addr, size);
        assert((size % 0x20) == 0);

        for (unsigned i = 0; i < size; i += 0x20) {
                unsigned type = (cl[i] & 0xF);

                switch (type) {
                case MALI_DESCRIPTOR_TYPE_SAMPLER:
                        DUMP_CL(SAMPLER, cl + i, "Sampler:\n");
                        break;
                case MALI_DESCRIPTOR_TYPE_TEXTURE:
                        pandecode_bifrost_texture(cl + i, i);
                        break;
                case MALI_DESCRIPTOR_TYPE_ATTRIBUTE:
                        DUMP_CL(ATTRIBUTE, cl + i, "Attribute:\n");
                        break;
                case MALI_DESCRIPTOR_TYPE_BUFFER:
                        DUMP_CL(BUFFER, cl + i, "Buffer:\n");
                        break;
                default:
                        fprintf(pandecode_dump_stream, "Unknown descriptor type %X\n", type);
                        break;
                }
        }
}

static void
pandecode_resource_tables(mali_ptr addr, const char *label)
{
        unsigned count = addr & 0x3F;
        addr = addr & ~0x3F;

        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing(addr);
        const uint8_t *cl = pandecode_fetch_gpu_mem(mem, addr, MALI_RESOURCE_LENGTH * count);

        for (unsigned i = 0; i < count; ++i) {
                pan_unpack(cl + i * MALI_RESOURCE_LENGTH, RESOURCE, entry);
                DUMP_UNPACKED(RESOURCE, entry, "Entry %u:\n", i);

                pandecode_indent += 2;
                if (entry.address)
                        pandecode_resources(entry.address, entry.size);
                pandecode_indent -= 2;
        }
}

static void
pandecode_depth_stencil(mali_ptr addr)
{
        MAP_ADDR(DEPTH_STENCIL, addr, cl);
        pan_unpack(cl, DEPTH_STENCIL, desc);
        DUMP_UNPACKED(DEPTH_STENCIL, desc, "Depth/stencil");
}

static void
pandecode_shader_environment(const struct MALI_SHADER_ENVIRONMENT *p,
                             unsigned gpu_id)
{
        if (p->shader)
                pandecode_shader(p->shader, "Shader", gpu_id);

        if (p->resources)
                pandecode_resource_tables(p->resources, "Resources");

        if (p->thread_storage)
                pandecode_local_storage(p->thread_storage);

        if (p->fau)
                dump_fau(p->fau, p->fau_count, "FAU");
}

static void
pandecode_dcd(const struct MALI_DRAW *p,
              enum mali_job_type job_type,
              char *suffix, unsigned gpu_id)
{
        mali_ptr frag_shader = 0;

        pandecode_depth_stencil(p->depth_stencil);

        for (unsigned i = 0; i < p->blend_count; ++i) {
                struct pandecode_mapped_memory *blend_mem =
                        pandecode_find_mapped_gpu_mem_containing(p->blend);

                struct mali_blend_packed *PANDECODE_PTR_VAR(blend_descs, blend_mem, p->blend);

                mali_ptr blend_shader = pandecode_bifrost_blend(blend_descs, i, frag_shader);
                if (blend_shader) {
                        fprintf(pandecode_dump_stream, "Blend shader %u", i);
                        pandecode_shader_disassemble(blend_shader, 0, gpu_id);
                }
        }

        pandecode_shader_environment(&p->shader, gpu_id);
        DUMP_UNPACKED(DRAW, *p, "Draw:\n");
}

static void
pandecode_malloc_vertex_job(const struct pandecode_mapped_memory *mem,
                            mali_ptr job, uint32_t *cs_buf, uint32_t *cs_buf_unk,
                            unsigned gpu_id)
{
#if PAN_ARCH < 10
        struct mali_malloc_vertex_job_packed *PANDECODE_PTR_VAR(p, mem, job);
#endif

        DUMP_SECTION_CS_V10(MALLOC_VERTEX_JOB, PRIMITIVE, p, cs_buf, cs_buf_unk, "Primitive:\n");
        DUMP_SECTION_CS_V10(MALLOC_VERTEX_JOB, INSTANCE_COUNT, p, cs_buf, cs_buf_unk, "Instance count:\n");
#if PAN_ARCH < 10
        DUMP_SECTION(MALLOC_VERTEX_JOB, ALLOCATION, p, "Allocation:\n");
#endif
        DUMP_SECTION_CS_V10(MALLOC_VERTEX_JOB, TILER, p, cs_buf, cs_buf_unk, "Tiler:\n");
        DUMP_SECTION_CS_V10(MALLOC_VERTEX_JOB, SCISSOR, p, cs_buf, cs_buf_unk, "Scissor:\n");
        DUMP_SECTION_CS_V10(MALLOC_VERTEX_JOB, PRIMITIVE_SIZE, p, cs_buf, cs_buf_unk, "Primitive Size:\n");
#if PAN_ARCH < 10
        DUMP_SECTION_CS_V10(MALLOC_VERTEX_JOB, INDICES, p, cs_buf, cs_buf_unk, "Indices:\n"); // todo v10
#endif

        pan_section_unpack_cs_v10(p, cs_buf, cs_buf_unk, MALLOC_VERTEX_JOB, DRAW, dcd);

        pan_section_unpack_cs_v10(p, cs_buf, cs_buf_unk, MALLOC_VERTEX_JOB, TILER, tiler_ptr);
        pandecode_log("Tiler Job Payload:\n");
        pandecode_indent++;
        if (tiler_ptr.address)
                pandecode_bifrost_tiler(tiler_ptr.address);
        else
                pandecode_log("<omitted>\n");
        pandecode_indent--;

        pandecode_dcd(&dcd, 0, NULL, gpu_id);

        pan_section_unpack_cs_v10(p, cs_buf, cs_buf_unk, MALLOC_VERTEX_JOB, POSITION, position);
        pan_section_unpack_cs_v10(p, cs_buf, cs_buf_unk, MALLOC_VERTEX_JOB, VARYING, varying);
        pandecode_shader_environment(&position, gpu_id);
        pandecode_shader_environment(&varying, gpu_id);
}

static void
pandecode_compute_job(const struct pandecode_mapped_memory *mem,
                      mali_ptr job, uint32_t *cs_buf, uint32_t *cs_buf_unk,
                      unsigned gpu_id)
{
#if PAN_ARCH < 10
	struct mali_compute_job_packed *PANDECODE_PTR_VAR(p, mem, job);
#endif
	pan_section_unpack_cs_v10(p, cs_buf, cs_buf_unk, COMPUTE_JOB, PAYLOAD, payload);

	pandecode_shader(payload.compute.shader, "Shader", gpu_id);
	if (payload.compute.thread_storage)
		pandecode_local_storage(payload.compute.thread_storage);
	if (payload.compute.fau)
		dump_fau(payload.compute.fau, payload.compute.fau_count, "FAU");
	if (payload.compute.resources)
		pandecode_resource_tables(payload.compute.resources, "Resources");

	DUMP_UNPACKED(COMPUTE_PAYLOAD, payload, "Compute:\n");
}
#endif /* PAN_ARCH >= 9 */

#if PAN_ARCH < 10
/* Entrypoint to start tracing. jc_gpu_va is the GPU address for the first job
 * in the chain; later jobs are found by walking the chain. Bifrost is, well,
 * if it's bifrost or not. GPU ID is the more finegrained ID (at some point, we
 * might wish to combine this with the bifrost parameter) because some details
 * are model-specific even within a particular architecture. */

void
GENX(pandecode_jc)(mali_ptr jc_gpu_va, unsigned gpu_id)
{
        pandecode_dump_file_open();

        mali_ptr next_job = 0;

        do {
                struct pandecode_mapped_memory *mem =
                        pandecode_find_mapped_gpu_mem_containing(jc_gpu_va);

                pan_unpack(PANDECODE_PTR(mem, jc_gpu_va, struct mali_job_header_packed),
                           JOB_HEADER, h);
                next_job = h.next;

                DUMP_UNPACKED(JOB_HEADER, h, "Job Header (%" PRIx64 "):\n", jc_gpu_va);
                pandecode_log("\n");

                switch (h.type) {
                case MALI_JOB_TYPE_WRITE_VALUE:
                        pandecode_write_value_job(mem, jc_gpu_va);
                        break;

                case MALI_JOB_TYPE_CACHE_FLUSH:
                        pandecode_cache_flush_job(mem, jc_gpu_va);
                        break;

                case MALI_JOB_TYPE_TILER:
                        pandecode_tiler_job(&h, mem, jc_gpu_va, gpu_id);
                        break;

#if PAN_ARCH <= 7
                case MALI_JOB_TYPE_VERTEX:
                case MALI_JOB_TYPE_COMPUTE:
                        pandecode_vertex_compute_geometry_job(&h, mem, jc_gpu_va, gpu_id);
                        break;

#if PAN_ARCH >= 6
                case MALI_JOB_TYPE_INDEXED_VERTEX:
                        pandecode_indexed_vertex_job(&h, mem, jc_gpu_va, gpu_id);
                        break;
#endif
#else /* PAN_ARCH > 7 */
		case MALI_JOB_TYPE_COMPUTE:
			pandecode_compute_job(mem, jc_gpu_va, NULL, NULL, gpu_id);
			break;

		case MALI_JOB_TYPE_MALLOC_VERTEX:
			pandecode_malloc_vertex_job(mem, jc_gpu_va, NULL, NULL, gpu_id);
			break;
#endif

                case MALI_JOB_TYPE_FRAGMENT:
                        pandecode_fragment_job(mem, jc_gpu_va, NULL, NULL, gpu_id);
                        break;

                default:
                        break;
                }
        } while ((jc_gpu_va = next_job));

        fflush(pandecode_dump_stream);
        pandecode_map_read_write();
}

void
GENX(pandecode_abort_on_fault)(mali_ptr jc_gpu_va)
{
        mali_ptr next_job = 0;

        do {
                struct pandecode_mapped_memory *mem =
                        pandecode_find_mapped_gpu_mem_containing(jc_gpu_va);

                pan_unpack(PANDECODE_PTR(mem, jc_gpu_va, struct mali_job_header_packed),
                           JOB_HEADER, h);
                next_job = h.next;

                /* Ensure the job is marked COMPLETE */
                if (h.exception_status != 0x1) {
                        fprintf(stderr, "Incomplete job or timeout\n");
                        fflush(NULL);
                        abort();
                }
        } while ((jc_gpu_va = next_job));

        pandecode_map_read_write();
}
#endif /* PAN_ARCH < 10 */

#if PAN_ARCH >= 10
static void
pandecode_cs_dump_state(uint32_t *state)
{
        uint64_t *st_64 = (uint64_t *)state;
        for (unsigned i = 0; i < 256 / 4; ++i) {
                uint64_t v1 = st_64[i * 2];
                uint64_t v2 = st_64[i * 2 + 1];

                if (!v1 && !v2)
                        continue;

                pandecode_log("0x%2x: 0x%16"PRIx64" 0x%16"PRIx64"\n",
                              i * 4, v1, v2);
        }
}

static void
pandecode_cs_buffer(uint64_t *commands, unsigned size,
                    uint32_t *buffer, uint32_t *buffer_unk,
                    unsigned gpu_id);

static void
pandecode_cs_command(uint64_t command,
                     uint32_t *buffer, uint32_t *buffer_unk,
                     unsigned gpu_id)
{
        uint8_t op = command >> 56;
        uint8_t addr = (command >> 48) & 0xff;
        uint64_t value = command & 0xffffffffffffULL;

        uint32_t h = value >> 32;
        uint32_t l = value;

        uint8_t arg1 = h & 0xff;
        uint8_t arg2 = h >> 8;

	if (command)
                pandecode_log("%016"PRIx64" ", command);

	switch (op) {
        case 0:
                if (addr || value)
                        pandecode_log("nop %02x, #0x%"PRIx64"\n", addr, value);
                break;
        case 1:
                buffer_unk[addr] = buffer[addr] = l;
                buffer_unk[addr + 1] = buffer[addr + 1] = h;
                pandecode_log("mov x%02x, #0x%"PRIx64"\n", addr, value);
                break;
        case 2:
                buffer_unk[addr] = buffer[addr] = l;
                pandecode_log("mov w%02x, #0x%"PRIx64"\n", addr, value);
                break;
        case 3:
                if (l & 0xffff || h || addr)
                        pandecode_log("state (unk %02x), (unk %04x), "
                                      "%i, (unk %04x)\n", addr, h, l >> 16, l);
                else
                        pandecode_log("state %i\n", l >> 16);
                break;
        case 4: {
                uint32_t masked = l & 0xffff0000;
                unsigned task_increment = l & 0x3fff;
                unsigned task_axis = (l >> 14) & 3;
                if (h != 0xff00 || addr || masked)
                        pandecode_log("compute (unk %02x), (unk %04x), "
                                      "(unk %x), inc %i, axis %i\n\n", addr, h, masked, task_increment, task_axis);
                else
                        pandecode_log("compute inc %i, axis %i\n\n", task_increment, task_axis);

                pandecode_indent++;

                pandecode_compute_job(NULL, 0, buffer, buffer_unk, gpu_id);

                /* The gallium driver emits this even for compute jobs, clear
                 * it from unknown state */
                pan_unpack_cs(buffer, buffer_unk, SCISSOR, unused_scissor);

                pandecode_cs_dump_state(buffer_unk);
                pandecode_log("\n");
                pandecode_indent--;

                break;
        }
        case 6: {
                uint32_t masked = l & 0xfffff8f0;
                uint8_t mode = l & 0xf;
                uint8_t index = (l >> 8) & 7;
                if (addr || masked)
                        pandecode_log("idvs (unk %02x), w%02x, w%02x, (unk %x), "
                                      "mode %i index %i\n\n",
                                      addr, arg1, arg2, masked, mode, index);
                else
                        pandecode_log("idvs w%02x, w%02x, mode %i index %i\n\n",
                                      arg1, arg2, mode, index);

                pandecode_indent++;

                pandecode_malloc_vertex_job(NULL, 0, buffer, buffer_unk, gpu_id);

                pandecode_cs_dump_state(buffer_unk);
                pandecode_log("\n");
                pandecode_indent--;

                break;
        }
        case 7: {
                if (addr || value)
                        pandecode_log("fragment (unk %02x), (unk %"PRIx64")\n\n",
                                      addr, value);
                else
                        pandecode_log("fragment\n\n");

                pandecode_indent++;

                pandecode_fragment_job(NULL, 0, buffer, buffer_unk, gpu_id);

                pandecode_cs_dump_state(buffer_unk);
                pandecode_log("\n");
                pandecode_indent--;

                break;
        }
        case 17: {
                if (arg1)
                        pandecode_log("add x%02x, (unk %x), x%02x, #0x%x\n",
                                      addr, arg1, arg2, l);
                else if ((int32_t) l < 0)
                        pandecode_log("add x%02x, x%02x, %i\n",
                                      addr, arg2, (int32_t) l);
                else if (l)
                        pandecode_log("add x%02x, x%02x, #0x%x\n",
                                      addr, arg2, l);
                else
                        pandecode_log("mov x%02x, x%02x\n", addr, arg2);

                break;
        }
        case 21: {
                if (arg1 || (l >> 16) != 3)
                        pandecode_log("str (unk %02x), x%02x, (unk %x), [x%02x, %x]\n",
                                      arg1, addr, l >> 16, arg2, l & 0xffff);
                else
                        pandecode_log("str x%02x, [x%02x, %x]\n",
                                      addr, arg2, l & 0xffff);
                break;
        }
        case 23: {
                if (value >> 8 || addr)
                        pandecode_log("select (unk %02x), (unk %"PRIx64"), "
                                      "%i\n", addr, value >> 8, l & 0xff);
                else
                        pandecode_log("select %i\n", l & 0xff);
                break;
        }
        case 32: {
                unsigned length = buffer[arg1];
                uint64_t target = (((uint64_t)buffer[arg2 + 1]) << 32) | buffer[arg2];

                assert(!(length & 7));
                unsigned instrs = length / 8;

                if (addr || l)
                        pandecode_log("job (unk %02x), w%02x (%i instructions), x%02x (0x%"PRIx64"), (unk %x)\n",
                                      addr, arg1, instrs, arg2, target, l);
                else
                        pandecode_log("job w%02x (%i instructions), x%02x (0x%"PRIx64")\n",
                                      arg1, instrs, arg2, target);

                uint64_t *t = pandecode_fetch_gpu_mem(NULL, target, length);
                pandecode_indent++;
                pandecode_cs_buffer(t, length, buffer, buffer_unk, gpu_id);
                pandecode_indent--;
                break;
        }
        case 34: {
                const char *name;
                switch (l) {
                case 1: name = "other"; break;
                case 2: name = "fragment"; break;
                case 3: name = "compute"; break;
                case 13: name = "vertex"; break;
                default: name = "unk";
                }
                pandecode_log("iter %s\n", name);
                break;
        }
        case 37: {
                pandecode_log("strev(unk) (unk %02x), w%02x, [x%02x, unk %x]\n",
                              addr, arg1, arg2, l);
                break;
        }
        case 38: {
                pandecode_log("strev (unk %02x), w%02x, [x%02x, unk %x]\n",
                              addr, arg1, arg2, l);
                break;
        }
        default:
                pandecode_log("UNK %02x %02x, #0x%"PRIx64"\n", addr, op, value);
                break;
        }
}

static void
pandecode_cs_buffer(uint64_t *commands, unsigned size,
                    uint32_t *buffer, uint32_t *buffer_unk,
                    unsigned gpu_id)
{
        uint64_t *end = (uint64_t *)((uint8_t *) commands + size);

        for (uint64_t c = *commands; commands < end; c = *(++commands)) {
                pandecode_cs_command(c, buffer, buffer_unk, gpu_id);
        }
}

void
GENX(pandecode_cs)(mali_ptr cs_gpu_va, unsigned size, unsigned gpu_id)
{
        pandecode_dump_file_open();

        uint32_t buffer[256] = {0};
        uint32_t buffer_unk[256] = {0};

        uint64_t *commands = pandecode_fetch_gpu_mem(NULL, cs_gpu_va, size);

        pandecode_log("\n");

        pandecode_cs_buffer(commands, size, buffer, buffer_unk, gpu_id);

        pandecode_map_read_write();
        fflush(pandecode_dump_stream);
}
#endif

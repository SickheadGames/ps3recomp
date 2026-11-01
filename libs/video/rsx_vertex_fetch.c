/*
 * ps3recomp - RSX guest vertex fetch (see rsx_vertex_fetch.h)
 */
#include "rsx_vertex_fetch.h"
#include "rsx_vertex_formats.h"

#include "ps3emu/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float rsx_rd_bef(const u8* p)
{
    u32 w; memcpy(&w, p, 4);
    w = ((w >> 24) & 0xFFu) | ((w >> 8) & 0xFF00u) |
        ((w << 8) & 0xFF0000u) | ((w << 24) & 0xFF000000u);
    float f; memcpy(&f, &w, 4); return f;
}

float rsx_rd_half_be(const u8* p)
{
    u16 h = (u16)((p[0] << 8) | p[1]);
    u32 sgn = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu, f;
    if (exp == 0)        f = sgn << 31;
    else if (exp == 31)  f = (sgn << 31) | 0x7F800000u | (man << 13);
    else                 f = (sgn << 31) | ((exp - 15u + 127u) << 23) | (man << 13);
    float o; memcpy(&o, &f, 4); return o;
}

/* Convert one element at p by its NV4097 type into out (n components; the
 * rest keep what out holds). Shared by both fetch paths so they cannot
 * drift apart. */
static void rsx_convert(const u8* p, u32 type, u32 n, int idx, u32 size, u32 stride, float out[4])
{
    switch (type) {
    case 2: /* CELL_GCM_VERTEX_F: float32 BE */
        for (u32 k = 0; k < n; k++) out[k] = rsx_rd_bef(p + k * 4);
        break;
    case 3: /* SF: half float BE */
        for (u32 k = 0; k < n; k++) out[k] = rsx_rd_half_be(p + k * 2);
        break;
    case 4: /* UB: u8 normalized [0,1] */
        for (u32 k = 0; k < n; k++) out[k] = p[k] / 255.0f;
        break;
    case 1: /* S1: s16 normalized [-1,1] */
        for (u32 k = 0; k < n; k++) {
            s16 s = (s16)((p[k*2] << 8) | p[k*2+1]);
            out[k] = (float)s / 32767.0f;
        }
        break;
    case 5: /* S32K: s16 integer */
        for (u32 k = 0; k < n; k++)
            out[k] = (float)(s16)((p[k*2] << 8) | p[k*2+1]);
        break;
    case 6: /* CMP: one big-endian word, x and y signed 11 bits, z signed 10,
             * normalized. It fell to the float default, so a mesh with
             * compressed normals was lit from garbage. Same decode as
             * rsx_vertex_compact.c and rsx_live_draw.c. */
        {
            const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
            s32 x = (s32)(w & 0x7FF);          if (x & 0x400) x -= 0x800;
            s32 y = (s32)((w >> 11) & 0x7FF); if (y & 0x400) y -= 0x800;
            s32 z = (s32)((w >> 22) & 0x3FF); if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
        }
        break;
    case 7: /* UB256: u8 unnormalized */
        for (u32 k = 0; k < n; k++) out[k] = (float)p[k];
        break;
    default:
        { static int t = -1; if (t < 0) t = getenv("VTX_TYPEDBG") ? 1 : 0;
          if (t) { static u32 seen = 0;
              if (!(seen & (1u << (type & 31)))) { seen |= 1u << (type & 31);
                  ps3_log("[VTXTYPE] UNHANDLED type=%u attr=%d size=%u stride=%u\n",
                          type, idx, size, stride); } } }
        for (u32 k = 0; k < n; k++) out[k] = rsx_rd_bef(p + k * 4);
        break;
    }
}

void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{
    /* A disabled attribute array feeds the CONSTANT vertex attribute register
     * (NV4097_SET_VERTEX_DATA4F_M), not zero -- same rule as glColor4f with
     * the colour array off. Defaulting these to black multiplied Rubber
     * Ducky's duck texture away in the fragment program. Components the array
     * does not supply (size < 4) keep the register's value too. */
    out[0] = st->vertex_data4f[idx][0];
    out[1] = st->vertex_data4f[idx][1];
    out[2] = st->vertex_data4f[idx][2];
    out[3] = st->vertex_data4f[idx][3];

    const rsx_vertex_attrib* a = &st->vertex_attribs[idx];
    if (!a->enabled || a->stride == 0 || !vm_base) return;

    /* Vertex frequency divisor (instancing). An ordinary array
     * (rsx_vertex_freq_plain: freq 0, or 1 with DIVIDE) is per-vertex.
     * Otherwise the element index is either vertex/freq (DIVIDE: per-instance
     * data advances once per freq verts) or vertex%freq (MODULO: a mesh
     * repeats every freq verts). NV4097_SET_FREQUENCY_DIVIDER_OPERATION's
     * per-attribute bit selects MODULO. DeferredShading's cube rings need
     * this: the shared cube mesh is MODULO, the per-instance transform in
     * attrib9 is DIVIDE -- without it attrib9 advanced every vertex and the
     * instanced cubes drew with garbage transforms (only the baseplate,
     * which isn't instanced, survived). */
    /* An ordinary array also adds NV4097_SET_VERTEX_DATA_BASE_INDEX (same
     * rule as rsx_live_draw.c, rsx_vertex_element_index). */
    u32 ei = (vi + st->vertex_data_base_index) & 0x000FFFFFu;
    if (!rsx_vertex_freq_plain(a->frequency,
                               (st->frequency_divider_op >> idx) & 1u)) {
        static int nofreq = -1;                 /* VP_NOFREQ: instancing kill-switch */
        if (nofreq < 0) nofreq = getenv("VP_NOFREQ") ? 1 : 0;
        if (!nofreq)
            ei = (st->frequency_divider_op & (1u << idx))
                     ? (vi % a->frequency)      /* MODULO: repeat mesh */
                     : (vi / a->frequency);     /* DIVIDE: per-instance */
    }

    /* Bit 31 of the vertex-array OFFSET selects the context DMA: 0 = LOCAL
     * (VRAM), 1 = MAIN (IO-mapped system memory). Resolve LOCAL as local --
     * routing it through cellGcmResolveOffset lets the IO table win for any
     * offset whose page the IO region also covers. One title's vertex arrays
     * sit at offsets like 0x4480, shadowed by its 1MB IO window at 0x11100000:
     * the array was uploaded to VRAM 0xC0004480 but resolved to main
     * 0x11104480, which is empty -- so every INDEXED mesh fetched zeros and
     * collapsed to the origin, while non-indexed geometry whose arrays lie
     * outside the shadowed pages drew fine. */
    /* NV4097_SET_VERTEX_DATA_BASE_OFFSET is added to the array offset and the
     * sum taken to 28 bits (rsx_dsp_resolve_vertex_offset). */
    u32 off = ((st->vertex_data_base_offset + (a->offset & 0x7FFFFFFFu)) & 0x0FFFFFFFu)
              + ei * a->stride;
    const u8* p = vm_base + ((a->offset & 0x80000000u)
        ? cellGcmResolveLocated(0, off)   /* MAIN:  IO offset table          */
        : cellGcmResolveLocated(1, off)); /* LOCAL: VRAM, never the IO table */

    u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
    rsx_convert(p, a->type, n, idx, a->size, a->stride, out);
}

void rsx_fetch_plan_init(const rsx_state* st, int idx, rsx_fetch_plan* p)
{
    memset(p, 0, sizeof *p);
    p->idx = (u8)idx;
    for (int k = 0; k < 4; k++) p->c[k] = st->vertex_data4f[idx][k];
    const rsx_vertex_attrib* a = &st->vertex_attribs[idx];
    if (!a->enabled || a->stride == 0 || !vm_base) return;   /* mode 0: the register */
    p->stride = a->stride;
    p->type = (u8)a->type;
    { u32 n = a->size ? a->size : 4; if (n > 4) n = 4; p->n = (u8)n; }
    p->base_index = st->vertex_data_base_index;
    p->mode = 1;
    if (!rsx_vertex_freq_plain(a->frequency, (st->frequency_divider_op >> idx) & 1u)) {
        static int nofreq = -1;                 /* VP_NOFREQ, as in rsx_fetch_attrib */
        if (nofreq < 0) nofreq = getenv("VP_NOFREQ") ? 1 : 0;
        if (!nofreq) {
            p->mode = (st->frequency_divider_op & (1u << idx)) ? 3 : 2;
            p->freq = a->frequency;
        }
    }
    const u32 off0 = (st->vertex_data_base_offset + (a->offset & 0x7FFFFFFFu)) & 0x0FFFFFFFu;
    if (a->offset & 0x80000000u) p->off0 = off0;                         /* MAIN */
    else                         p->base = vm_base + cellGcmResolveLocated(1, off0);   /* LOCAL */
}

void rsx_fetch_planned(const rsx_fetch_plan* p, u32 vi, float out[4])
{
    out[0] = p->c[0]; out[1] = p->c[1]; out[2] = p->c[2]; out[3] = p->c[3];
    if (!p->mode) return;
    const u32 ei = p->mode == 1 ? ((vi + p->base_index) & 0x000FFFFFu)
                 : p->mode == 3 ? vi % p->freq : vi / p->freq;
    const u8* q = p->base ? p->base + (u64)ei * p->stride
                          : vm_base + cellGcmResolveLocated(0, p->off0 + ei * p->stride);
    rsx_convert(q, p->type, p->n, p->idx, p->n, p->stride, out);
}

/* ---------------------------------------------------------------------------
 * rsx_fetch_planned_many: the per-type loops.
 *
 * Each loop reads an element, converts all its components at once and keeps
 * the constant register in the components the array does not supply -- the
 * same arithmetic as rsx_convert, lane by lane: a byte swap for float32, an
 * exact int->float conversion and the same IEEE division for the normalized
 * types, and the same bit assembly for half floats (exponent 0 is a signed
 * zero, not a denormal, so not F16C). Anything the loops do not cover takes
 * the per-vertex path, which is the definition.
 * -------------------------------------------------------------------------*/
#if !defined(__ARM_NEON) && \
    (defined(__SSE4_1__) || (defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)))
#define RVF_SSE 1
#include <immintrin.h>
#else
#define RVF_SSE 0
#endif

#if RVF_SSE
/* Lanes 0..n-1 all-ones: where the array supplies the component. */
static __m128 rvf_keep(u32 n)
{
    return _mm_castsi128_ps(_mm_cmpgt_epi32(_mm_set1_epi32((int)n), _mm_setr_epi32(0, 1, 2, 3)));
}
/* The element's bytes into the low end of a vector. One 16-byte load: the bytes past the element
 * only reach lanes the caller replaces with the constant register. Only within 16 bytes of the
 * end of guest memory does it copy just the element instead (a variable-length memcpy per
 * element was a library call each, a quarter of this function's time). */
extern uint32_t ppu_vm_size;
static inline __m128i rvf_load(const u8* q, u32 bytes)
{
    if ((u64)(q - vm_base) + 16u <= (u64)ppu_vm_size) return _mm_loadu_si128((const __m128i*)q);
    u8 t[16] = {0}; memcpy(t, q, bytes);
    return _mm_loadu_si128((const __m128i*)t);
}
/* Half floats (low 16 bits of each lane) as rsx_rd_half_be builds them. */
static inline __m128i rvf_half(__m128i h)
{
    const __m128i sgn = _mm_slli_epi32(_mm_and_si128(h, _mm_set1_epi32(0x8000)), 16);
    const __m128i exp = _mm_and_si128(_mm_srli_epi32(h, 10), _mm_set1_epi32(0x1F));
    const __m128i man = _mm_slli_epi32(_mm_and_si128(h, _mm_set1_epi32(0x3FF)), 13);
    const __m128i nrm = _mm_or_si128(_mm_or_si128(sgn, man), _mm_slli_epi32(_mm_add_epi32(exp, _mm_set1_epi32(112)), 23));
    const __m128i inf = _mm_or_si128(_mm_or_si128(sgn, man), _mm_set1_epi32(0x7F800000));
    __m128i r = _mm_blendv_epi8(nrm, inf, _mm_cmpeq_epi32(exp, _mm_set1_epi32(31)));
    return _mm_blendv_epi8(r, sgn, _mm_cmpeq_epi32(exp, _mm_setzero_si128()));
}
#endif

void rsx_fetch_planned_many(const rsx_fetch_plan* p, const u32* vis, u32 vi0, u32 nverts,
                            u8* out, u32 out_stride)
{
#define RVF_VI(i) (vis ? vis[i] : vi0 + (i))
    if (p->mode == 0) {   /* the constant register for every vertex */
        for (u32 i = 0; i < nverts; i++) memcpy(out + (u64)i * out_stride, p->c, 16);
        return;
    }
#if RVF_SSE
    const u32 n = p->n;
    if (p->mode == 1 && p->base && n >= 1 && n <= 4) {
        const __m128 c = _mm_loadu_ps(p->c), keep = rvf_keep(n);
        const u8* const base = p->base;
        const u32 bi = p->base_index, stride = p->stride;
#define RVF_LOOP(ELEM_BYTES, CONVERT)                                                        \
        for (u32 i = 0; i < nverts; i++) {                                                   \
            const u8* q = base + (u64)((RVF_VI(i) + bi) & 0x000FFFFFu) * stride;            \
            const __m128i raw = rvf_load(q, (ELEM_BYTES));                                   \
            const __m128 v = (CONVERT);                                                      \
            _mm_storeu_ps((float*)(out + (u64)i * out_stride), _mm_blendv_ps(c, v, keep));   \
        }                                                                                    \
        return;
        switch (p->type) {
        case 2: {   /* float32: a byte swap per word */
            const __m128i bs = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
            RVF_LOOP(n * 4, _mm_castsi128_ps(_mm_shuffle_epi8(raw, bs)))
        }
        case 4:     /* u8 normalized */
            RVF_LOOP(n, _mm_div_ps(_mm_cvtepi32_ps(_mm_cvtepu8_epi32(raw)), _mm_set1_ps(255.0f)))
        case 7:     /* u8 unnormalized */
            RVF_LOOP(n, _mm_cvtepi32_ps(_mm_cvtepu8_epi32(raw)))
        case 1: {   /* s16 normalized */
            const __m128i bs = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
            RVF_LOOP(n * 2, _mm_div_ps(_mm_cvtepi32_ps(_mm_cvtepi16_epi32(_mm_shuffle_epi8(raw, bs))),
                                       _mm_set1_ps(32767.0f)))
        }
        case 5: {   /* s16 integer */
            const __m128i bs = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
            RVF_LOOP(n * 2, _mm_cvtepi32_ps(_mm_cvtepi16_epi32(_mm_shuffle_epi8(raw, bs))))
        }
        case 3: {   /* half float */
            const __m128i bs = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
            RVF_LOOP(n * 2, _mm_castsi128_ps(rvf_half(_mm_cvtepu16_epi32(_mm_shuffle_epi8(raw, bs)))))
        }
        default:
            break;   /* CMP and anything else: the per-vertex path */
        }
#undef RVF_LOOP
    }
#endif
    for (u32 i = 0; i < nverts; i++)
        rsx_fetch_planned(p, RVF_VI(i), (float*)(out + (u64)i * out_stride));
#undef RVF_VI
}

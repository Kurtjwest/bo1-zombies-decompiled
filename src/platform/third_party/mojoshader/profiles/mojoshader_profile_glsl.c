/**
 * MojoShader; generate shader programs from bytecode of compiled
 *  Direct3D shaders.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#define __MOJOSHADER_INTERNAL__ 1
#include "mojoshader_profile.h"

// BO1 Linux port: GLSL for the Direct3D 9 layer's OpenGL renderer (src/platform/opengl/gl_rhi.cpp), valid as GLSL 4.50
// and, with GL_ARB_shading_language_420pack and explicit locations, as 3.30 and 1.40 (its OpenGL 3.x path). This is
// not MojoShader's own GLSL profile: it is the BO1 port's Metal profile (mojoshader_profile_metal.c, with its fixes
// for the game's shaders: predication, subroutines, relative addressing, texldl/texldp/texldb, the D3D9 rules for
// pow, log, nrm, texkill, cmp/cnd, ...) emitting GLSL, so both renderers translate every instruction the same way.
// The output has the same hooks, filled in with preprocessor definitions per pipeline variant: texture types and
// sampling modes (depth/shadow textures), alpha test, the extra vertex outputs a pixel shader reads, centroid
// interpolation and D3D9 pixel-center vPos.
//
// The renderer compiles the output after a header of its own (gl_rhi.cpp) that starts it with the version and
// extension lines and maps the Metal-style names the instruction emitters use to GLSL ones (float4 -> vec4, int4 -> ivec4,
// bool4 -> bvec4, FLT_MAX). Interface (layout matches the Metal profile's, see d3d9_shaders.cpp):
//   - vertex inputs: layout(location = n) in vec4 v<n> (the input register number; integer vertex formats are
//     converted to float by the vertex fetch)
//   - varyings: named after their usage and index (bo1v_texcoord0, bo1v_color1, ...), matched by name
//   - uniforms: one std140 block per stage (binding 0 vertex, 1 pixel): vec4 float registers, ivec4 int registers,
//     then the bool registers as bytes in uvec4s (bo1_vs_float4, bo1_ps_int4, ...: no instance name, which GLSL 1.40
//     does not have); binding 2: the vertex shaders' per-draw values (bo1_vsfix)
//   - samplers: texture unit = sampler register (pixel shaders), 16 + register (vertex shaders)
//   - pixel outputs: layout(location = n) out vec4 for oC<n>; oDepth is gl_FragDepth

#pragma GCC visibility push(hidden)

// !!! FIXME: A lot of this is cut-and-paste from the GLSL version.
#if SUPPORT_PROFILE_GLSL

#define EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(op) \
    void emit_GLSL_##op(Context *ctx) { \
        fail(ctx, #op " unimplemented in glsl profile"); \
    }

static inline const char *get_GLSL_register_string(Context *ctx,
                        const RegisterType regtype, const int regnum,
                        char *regnum_str, const size_t regnum_size)
{
    // turns out these are identical at the moment.
    return get_D3D_register_string(ctx,regtype,regnum,regnum_str,regnum_size);
} // get_GLSL_register_string

const char *get_GLSL_uniform_type(Context *ctx, const RegisterType rtype)
{
    switch (rtype)
    {
        case REG_TYPE_CONST: return "float4";
        case REG_TYPE_CONSTINT: return "int4";
        case REG_TYPE_CONSTBOOL: return "bool";
        default: fail(ctx, "BUG: used a uniform we don't know how to define.");
    } // switch

    return NULL;
} // get_GLSL_uniform_type

const char *get_GLSL_varname_in_buf(Context *ctx, RegisterType rt,
                                     int regnum, char *buf,
                                     const size_t len)
{
    char regnum_str[16];
    const char *regtype_str = get_GLSL_register_string(ctx, rt, regnum,
                                              regnum_str, sizeof (regnum_str));

    // Registers are locals of main(), so most names need no vs_ or ps_ prefix. Samplers are uniforms, which the
    //  two stages of a program share by name: theirs have one (vs_s0 and ps_s0 are different units).
    if (rt == REG_TYPE_SAMPLER)
        snprintf(buf, len, "%s_%s%s", shader_is_vertex(ctx) ? "vs" : "ps", regtype_str, regnum_str);
    else
        snprintf(buf, len, "%s%s", regtype_str, regnum_str);
    return buf;
} // get_GLSL_varname_in_buf


const char *get_GLSL_varname(Context *ctx, RegisterType rt, int regnum)
{
    char buf[512];
    get_GLSL_varname_in_buf(ctx, rt, regnum, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_GLSL_varname


static inline const char *get_GLSL_const_array_varname_in_buf(Context *ctx,
                                                const int base, const int size,
                                                char *buf, const size_t buflen)
{
    snprintf(buf, buflen, "const_array_%d_%d", base, size);
    return buf;
} // get_GLSL_const_array_varname_in_buf

const char *get_GLSL_const_array_varname(Context *ctx, int base, int size)
{
    char buf[512];
    get_GLSL_const_array_varname_in_buf(ctx, base, size, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_GLSL_const_array_varname


static inline const char *get_GLSL_input_array_varname(Context *ctx,
                                                char *buf, const size_t buflen)
{
    snprintf(buf, buflen, "%s", "vertex_input_array");
    return buf;
} // get_GLSL_input_array_varname


const char *get_GLSL_uniform_array_varname(Context *ctx,
                                            const RegisterType regtype,
                                            char *buf, const size_t len)
{
    const char *type = get_GLSL_uniform_type(ctx, regtype);
    // BO1: members of a block without an instance name (GLSL 1.40 has none), named for the stage so that the two
    // stages' blocks do not share member names
    snprintf(buf, len, "bo1_%s_%s", shader_is_vertex(ctx) ? "vs" : "ps", type);
    return buf;
} // get_GLSL_uniform_array_varname

const char *get_GLSL_destarg_varname(Context *ctx, char *buf, size_t len)
{
    const DestArgInfo *arg = &ctx->dest_arg;
    return get_GLSL_varname_in_buf(ctx, arg->regtype, arg->regnum, buf, len);
} // get_GLSL_destarg_varname

const char *get_GLSL_srcarg_varname(Context *ctx, const size_t idx,
                                     char *buf, size_t len)
{
    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        *buf = '\0';
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];
    return get_GLSL_varname_in_buf(ctx, arg->regtype, arg->regnum, buf, len);
} // get_GLSL_srcarg_varname


const char *make_GLSL_destarg_assign(Context *, char *, const size_t,
                                      const char *, ...) ISPRINTF(4,5);
char *make_GLSL_swizzle_string(char *swiz_str, const size_t strsize,
                                const int swizzle, const int writemask);

const char *make_GLSL_destarg_assign(Context *ctx, char *buf,
                                      const size_t buflen,
                                      const char *fmt, ...)
{
    int need_parens = 0;
    const DestArgInfo *arg = &ctx->dest_arg;

    if (arg->writemask == 0)
    {
        *buf = '\0';
        return buf;  // no writemask? It's a no-op.
    } // if

    char clampbuf[32] = { '\0' };
    const char *clampleft = "";
    const char *clampright = "";
    if (arg->result_mod & MOD_SATURATE)
    {
        ctx->metal_need_header_common = 1;
        const int vecsize = vecsize_from_writemask(arg->writemask);
        clampleft = "clamp(";
        if (vecsize == 1)
            clampright = ", 0.0, 1.0)";
        else
        {
            snprintf(clampbuf, sizeof (clampbuf),
                     ", float%d(0.0), float%d(1.0))", vecsize, vecsize);
            clampright = clampbuf;
        } // else
    } // if

    // MSDN says MOD_PP is a hint and many implementations ignore it. So do we.

    // CENTROID only allowed in DCL opcodes, which shouldn't come through here.
    assert((arg->result_mod & MOD_CENTROID) == 0);

    // BO1: predicated instructions write only the components whose predicate component is true (see the end)
    char predicate[512] = { '\0' };
    int predicate_not = 0;
    if (ctx->predicated)
    {
        const SourceArgInfo *parg = &ctx->predicate_arg;
        char pregnum[16];
        const char *pregtype = get_GLSL_register_string(ctx, parg->regtype, parg->regnum, pregnum, sizeof (pregnum));
        char pswiz[8];
        make_GLSL_swizzle_string(pswiz, sizeof (pswiz), parg->swizzle, arg->writemask);
        snprintf(predicate, sizeof (predicate), "%s%s%s", pregtype, pregnum, pswiz);
        predicate_not = (parg->src_mod == SRCMOD_NOT);
    } // if

    char operation[4096];
    va_list ap;
    va_start(ap, fmt);
    const int len = vsnprintf(operation, sizeof (operation), fmt, ap);
    va_end(ap);
    if (len >= sizeof (operation))
    {
        fail(ctx, "operation string too large");  // I'm lazy.  :P
        *buf = '\0';
        return buf;
    } // if

    const char *result_shift_str = "";
    switch (arg->result_shift)
    {
        case 0x1: result_shift_str = " * 2.0"; break;
        case 0x2: result_shift_str = " * 4.0"; break;
        case 0x3: result_shift_str = " * 8.0"; break;
        case 0xD: result_shift_str = " / 8.0"; break;
        case 0xE: result_shift_str = " / 4.0"; break;
        case 0xF: result_shift_str = " / 2.0"; break;
    } // switch
    need_parens |= (result_shift_str[0] != '\0');

    char regnum_str[16];
    const char *regtype_str = get_GLSL_register_string(ctx, arg->regtype,
                                                       arg->regnum, regnum_str,
                                                       sizeof (regnum_str));
    char writemask_str[6];
    size_t i = 0;
    const int scalar = isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum);
    if (!scalar && !writemask_xyzw(arg->writemask))
    {
        writemask_str[i++] = '.';
        if (arg->writemask0) writemask_str[i++] = 'x';
        if (arg->writemask1) writemask_str[i++] = 'y';
        if (arg->writemask2) writemask_str[i++] = 'z';
        if (arg->writemask3) writemask_str[i++] = 'w';
    } // if
    writemask_str[i] = '\0';
    assert(i < sizeof (writemask_str));

    const char *leftparen = (need_parens) ? "(" : "";
    const char *rightparen = (need_parens) ? ")" : "";

    if (ctx->predicated)
    {
        // BO1: (p0.swz) op dst.mask, ... -> per-component select on the predicate
        if (vecsize_from_writemask(arg->writemask) == 1)
        {
            const int written = snprintf(buf, buflen, "if (%s%s) { %s%s%s = %s%s%s%s%s%s; }",
                     predicate_not ? "!" : "", predicate,
                     regtype_str, regnum_str, writemask_str,
                     clampleft, leftparen, operation, rightparen, result_shift_str,
                     clampright);
            if ((written < 0) || ((size_t) written >= buflen))
                fail(ctx, "BUG: GLSL expression too long for its buffer");
        } // if
        else
        {
            char newval[4096];
            snprintf(newval, sizeof (newval), "%s%s%s%s%s%s", clampleft, leftparen, operation, rightparen,
                     result_shift_str, clampright);
            char oldval[512];
            snprintf(oldval, sizeof (oldval), "%s%s%s", regtype_str, regnum_str, writemask_str);
            const int written = snprintf(buf, buflen, "%s = mix(%s, %s, %s);", oldval,
                     predicate_not ? newval : oldval, predicate_not ? oldval : newval, predicate);
            if ((written < 0) || ((size_t) written >= buflen))
                fail(ctx, "BUG: GLSL expression too long for its buffer");
        } // else
        return buf;
    } // if

    const int written = snprintf(buf, buflen, "%s%s%s = %s%s%s%s%s%s;",
             regtype_str, regnum_str, writemask_str,
             clampleft, leftparen, operation, rightparen, result_shift_str,
             clampright);
    // BO1: a truncated line is broken GLSL; say so instead of emitting it (relative-addressed constants are long:
    // "bo1_vs_float4[(ARRAYBASE_0 + 120) + aL]", and the buffers above used to be 64/128 bytes)
    if ((written < 0) || ((size_t) written >= buflen))
        fail(ctx, "BUG: GLSL expression too long for its buffer");
    return buf;
} // make_GLSL_destarg_assign


char *make_GLSL_swizzle_string(char *swiz_str, const size_t strsize,
                                const int swizzle, const int writemask)
{
    size_t i = 0;
    if ( (!no_swizzle(swizzle)) || (!writemask_xyzw(writemask)) )
    {
        const int writemask0 = (writemask >> 0) & 0x1;
        const int writemask1 = (writemask >> 1) & 0x1;
        const int writemask2 = (writemask >> 2) & 0x1;
        const int writemask3 = (writemask >> 3) & 0x1;

        const int swizzle_x = (swizzle >> 0) & 0x3;
        const int swizzle_y = (swizzle >> 2) & 0x3;
        const int swizzle_z = (swizzle >> 4) & 0x3;
        const int swizzle_w = (swizzle >> 6) & 0x3;

        swiz_str[i++] = '.';
        if (writemask0) swiz_str[i++] = swizzle_channels[swizzle_x];
        if (writemask1) swiz_str[i++] = swizzle_channels[swizzle_y];
        if (writemask2) swiz_str[i++] = swizzle_channels[swizzle_z];
        if (writemask3) swiz_str[i++] = swizzle_channels[swizzle_w];
    } // if
    assert(i < strsize);
    swiz_str[i] = '\0';
    return swiz_str;
} // make_GLSL_swizzle_string


const char *make_GLSL_srcarg_string(Context *ctx, const size_t idx,
                                     const int writemask, char *buf,
                                     const size_t buflen)
{
    *buf = '\0';

    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];

    const char *premod_str = "";
    const char *postmod_str = "";
    switch (arg->src_mod)
    {
        case SRCMOD_NEGATE:
            premod_str = "-";
            break;

        case SRCMOD_BIASNEGATE:
            premod_str = "-(";
            postmod_str = " - 0.5)";
            break;

        case SRCMOD_BIAS:
            premod_str = "(";
            postmod_str = " - 0.5)";
            break;

        case SRCMOD_SIGNNEGATE:
            premod_str = "-((";
            postmod_str = " - 0.5) * 2.0)";
            break;

        case SRCMOD_SIGN:
            premod_str = "((";
            postmod_str = " - 0.5) * 2.0)";
            break;

        case SRCMOD_COMPLEMENT:
            premod_str = "(1.0 - ";
            postmod_str = ")";
            break;

        case SRCMOD_X2NEGATE:
            premod_str = "-(";
            postmod_str = " * 2.0)";
            break;

        case SRCMOD_X2:
            premod_str = "(";
            postmod_str = " * 2.0)";
            break;

        case SRCMOD_DZ:
            fail(ctx, "SRCMOD_DZ unsupported"); return buf; // !!! FIXME
            postmod_str = "_dz";
            break;

        case SRCMOD_DW:
            fail(ctx, "SRCMOD_DW unsupported"); return buf; // !!! FIXME
            postmod_str = "_dw";
            break;

        case SRCMOD_ABSNEGATE:
            ctx->metal_need_header_math = 1;
            premod_str = "-abs(";
            postmod_str = ")";
            break;

        case SRCMOD_ABS:
            ctx->metal_need_header_math = 1;
            premod_str = "abs(";
            postmod_str = ")";
            break;

        case SRCMOD_NOT:
            premod_str = "!";
            break;

        case SRCMOD_NONE:
        case SRCMOD_TOTAL:
             break;  // stop compiler whining.
    } // switch

    const char *regtype_str = NULL;

    if (!arg->relative)
    {
        regtype_str = get_GLSL_varname_in_buf(ctx, arg->regtype, arg->regnum,
                                              (char *) alloca(64), 64);
    } // if

    const char *rel_lbracket = "";
    char rel_offset[32] = { '\0' };
    const char *rel_rbracket = "";
    char rel_swizzle[4] = { '\0' };
    const char *rel_regtype_str = "";
    if (arg->relative)
    {
        if (arg->regtype == REG_TYPE_INPUT)
            regtype_str=get_GLSL_input_array_varname(ctx,(char*)alloca(64),64);
        else
        {
            assert(arg->regtype == REG_TYPE_CONST);
            const int arrayidx = arg->relative_array->index;
            const int offset = arg->regnum - arrayidx;
            assert(offset >= 0);
            if (arg->relative_array->constant)
            {
                const int arraysize = arg->relative_array->count;
                regtype_str = get_GLSL_const_array_varname_in_buf(ctx,
                                arrayidx, arraysize, (char *) alloca(64), 64);
                if (offset != 0)
                    snprintf(rel_offset, sizeof (rel_offset), "%d + ", offset);
            } // if
            else
            {
                regtype_str = get_GLSL_uniform_array_varname(ctx, arg->regtype,
                                                      (char *) alloca(64), 64);
                if (offset == 0)
                {
                    snprintf(rel_offset, sizeof (rel_offset),
                             "ARRAYBASE_%d + ", arrayidx);
                } // if
                else
                {
                    snprintf(rel_offset, sizeof (rel_offset),
                             "(ARRAYBASE_%d + %d) + ", arrayidx, offset);
                } // else
            } // else
        } // else

        rel_lbracket = "[";

        rel_regtype_str = get_GLSL_varname_in_buf(ctx, arg->relative_regtype,
                                                  arg->relative_regnum,
                                                  (char *) alloca(64), 64);
        // BO1: the loop counter aL is a scalar int (emit_GLSL_LOOP); only a0 takes a component
        if (arg->relative_regtype != REG_TYPE_LOOP)
        {
            rel_swizzle[0] = '.';
            rel_swizzle[1] = swizzle_channels[arg->relative_component];
            rel_swizzle[2] = '\0';
        } // if
        rel_rbracket = "]";
    } // if

    char swiz_str[6] = { '\0' };
    if (!isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum))
    {
        make_GLSL_swizzle_string(swiz_str, sizeof (swiz_str),
                                 arg->swizzle, writemask);
    } // if

    if (regtype_str == NULL)
    {
        fail(ctx, "Unknown source register type.");
        return buf;
    } // if

    const int written = snprintf(buf, buflen, "%s%s%s%s%s%s%s%s%s",
             premod_str, regtype_str, rel_lbracket, rel_offset,
             rel_regtype_str, rel_swizzle, rel_rbracket, swiz_str,
             postmod_str);
    if ((written < 0) || ((size_t) written >= buflen))  // BO1: see make_GLSL_destarg_assign
        fail(ctx, "BUG: GLSL source argument too long for its buffer");
    return buf;
} // make_GLSL_srcarg_string

// generate some convenience functions.
#define MAKE_GLSL_SRCARG_STRING_(mask, bitmask) \
    static inline const char *make_GLSL_srcarg_string_##mask(Context *ctx, \
                                                const size_t idx, char *buf, \
                                                const size_t buflen) { \
        return make_GLSL_srcarg_string(ctx, idx, bitmask, buf, buflen); \
    }
MAKE_GLSL_SRCARG_STRING_(x, (1 << 0))
MAKE_GLSL_SRCARG_STRING_(y, (1 << 1))
MAKE_GLSL_SRCARG_STRING_(z, (1 << 2))
MAKE_GLSL_SRCARG_STRING_(w, (1 << 3))
MAKE_GLSL_SRCARG_STRING_(scalar, (1 << 0))
MAKE_GLSL_SRCARG_STRING_(full, 0xF)
MAKE_GLSL_SRCARG_STRING_(masked, ctx->dest_arg.writemask)
MAKE_GLSL_SRCARG_STRING_(vec3, 0x7)
MAKE_GLSL_SRCARG_STRING_(vec2, 0x3)
#undef MAKE_GLSL_SRCARG_STRING_

// special cases for comparison opcodes...

const char *get_GLSL_comparison_string_scalar(Context *ctx)
{
    const char *comps[] = { "", ">", "==", ">=", "<", "!=", "<=" };
    if (ctx->instruction_controls >= STATICARRAYLEN(comps))
    {
        fail(ctx, "unknown comparison control");
        return "";
    } // if

    return comps[ctx->instruction_controls];
} // get_GLSL_comparison_string_scalar

const char *get_GLSL_comparison_string_vector(Context *ctx)
{
    // BO1: GLSL's relational operators take scalars only; vectors compare with these functions
    const char *comps[] = { "", "greaterThan", "equal", "greaterThanEqual", "lessThan", "notEqual", "lessThanEqual" };
    if (ctx->instruction_controls >= STATICARRAYLEN(comps))
    {
        fail(ctx, "unknown comparison control");
        return "";
    } // if

    return comps[ctx->instruction_controls];
} // get_GLSL_comparison_string_vector


void emit_GLSL_start(Context *ctx, const char *profilestr)
{
    if (!shader_is_vertex(ctx) && !shader_is_pixel(ctx))
    {
        failf(ctx, "Shader type %u unsupported in this profile.",
              (uint) ctx->shader_type);
        return;
    } // if

    if (!ctx->mainfn)
    {
        if (shader_is_vertex(ctx))
            ctx->mainfn = StrDup(ctx, "VertexShader");
        else if (shader_is_pixel(ctx))
            ctx->mainfn = StrDup(ctx, "FragmentShader");
    } // if

    set_output(ctx, &ctx->mainline);
    ctx->indent++;
} // emit_GLSL_start

void emit_GLSL_RET(Context *ctx);
void emit_GLSL_end(Context *ctx)
{
    // !!! FIXME: maybe handle this at a higher level?
    // ps_1_* writes color to r0 instead oC0. We move it to the right place.
    // We don't have to worry about a RET opcode messing this up, since
        //  RET isn't available before ps_2_0.
    if (shader_is_pixel(ctx) && !shader_version_atleast(ctx, 2, 0))
    {
        set_used_register(ctx, REG_TYPE_COLOROUT, 0, 1);
        output_line(ctx, "oC0 = r0;");
    } // if

    // !!! FIXME: maybe handle this at a higher level?
    // force a RET opcode if we're at the end of the stream without one.
    if (ctx->previous_opcode != OPCODE_RET)
        emit_GLSL_RET(ctx);
} // emit_GLSL_end

void emit_GLSL_phase(Context *ctx)
{
    // no-op in GLSL.
} // emit_GLSL_phase

// BO1: copies text to out with every "/*BO1_CALL_n*/" marker replaced by subroutine n's body, itself expanded.
static int glsl_expand_calls(Context *ctx, Buffer *out, const char *text, char **bodies, const int depth)
{
    static const char marker[] = "/*BO1_CALL_";
    const char *p = text;
    for (;;)
    {
        const char *m = strstr(p, marker);
        if (m == NULL)
            return buffer_append(out, p, strlen(p));
        if (!buffer_append(out, p, (size_t) (m - p)))
            return 0;
        const int label = atoi(m + sizeof (marker) - 1);
        const char *end = strstr(m, "*/");
        if ((end == NULL) || (label < 0) || (label >= 16) || (bodies[label] == NULL))
        {
            failf(ctx, "CALL to undefined label l%d", label);
            return 0;
        } // if
        if (depth >= 8)
        {
            fail(ctx, "subroutine calls nested too deeply (recursion?)");
            return 0;
        } // if
        if (!glsl_expand_calls(ctx, out, bodies[label], bodies, depth + 1))
            return 0;
        p = end + 2;
    } // for
} // glsl_expand_calls

static void glsl_inline_subroutines(Context *ctx)
{
    char *bodies[STATICARRAYLEN(ctx->metal_label_bodies)];
    int have = 0;
    size_t i;
    for (i = 0; i < STATICARRAYLEN(bodies); i++)
    {
        bodies[i] = NULL;
        if (ctx->metal_label_bodies[i] != NULL)
        {
            bodies[i] = buffer_flatten(ctx->metal_label_bodies[i]);
            if (bodies[i] == NULL)
                ctx->out_of_memory = ctx->isfail = 1;
            have = 1;
        } // if
    } // for
    if (have && !ctx->isfail && (ctx->mainline != NULL))
    {
        char *text = buffer_flatten(ctx->mainline);  // leaves mainline empty
        if (text == NULL)
            ctx->out_of_memory = ctx->isfail = 1;
        else
        {
            glsl_expand_calls(ctx, ctx->mainline, text, bodies, 0);
            Free(ctx, text);
        } // else
    } // if
    for (i = 0; i < STATICARRAYLEN(bodies); i++)
    {
        if (bodies[i] != NULL)
            Free(ctx, bodies[i]);
    } // for
} // glsl_inline_subroutines

void emit_GLSL_finalize(Context *ctx)
{
    glsl_inline_subroutines(ctx);  // BO1

    // If we had a relative addressing of REG_TYPE_INPUT, we need to build
    //  an array for it at the start of main(). GLSL doesn't let you specify
    //  arrays of attributes.
    //float4 blah_array[BIGGEST_ARRAY];
    if (ctx->have_relative_input_registers) // !!! FIXME
        fail(ctx, "Relative addressing of input registers not supported.");

    const int vs = shader_is_vertex(ctx);

    push_output(ctx, &ctx->preflight);
    // (the renderer tells the stages apart by this, and compiles the source after its own header)
    output_line(ctx, "#define BO1_STAGE_%s 1", vs ? "VS" : "PS");
    // BO1: sampling helpers. Pixel shaders - kind: 0 plain, 1 projected, 2 biased, 3 explicit LOD in .w; mode:
    //  0 color texture, 1 depth texture with comparison (D3D9 hardware shadow map: sampler2DShadow). Vertex shaders
    //  only have explicit-LOD lookups (texldl), and their helpers use nothing else.
    output_line(ctx, "#ifndef BO1_HELPERS");
    output_line(ctx, "#define BO1_HELPERS 1");
    if (vs)
    {
        output_line(ctx, "float4 bo1_vtex2d(sampler2D s, float4 c, int mode) { return textureLod(s, c.xy, c.w); }");
        output_line(ctx, "float4 bo1_vtex2d(sampler2DShadow s, float4 c, int mode) {");
        output_line(ctx, "    return float4(textureLod(s, float3(c.xy, clamp(c.z, 0.0, 1.0)), 0.0));");
        output_line(ctx, "}");
        output_line(ctx, "float4 bo1_vtexcube(samplerCube s, float4 c, int mode) { return textureLod(s, c.xyz, c.w); }");
        output_line(ctx, "float4 bo1_vtex3d(sampler3D s, float4 c, int mode) { return textureLod(s, c.xyz, c.w); }");
    } // if
    else
    {
        output_line(ctx, "float4 bo1_tex2d(sampler2D s, float4 c, int kind, int mode) {");
        output_line(ctx, "    if (kind == 1) return texture(s, c.xy / c.w);");
        output_line(ctx, "    if (kind == 2) return texture(s, c.xy, c.w);");
        output_line(ctx, "    if (kind == 3) return textureLod(s, c.xy, c.w);");
        output_line(ctx, "    return texture(s, c.xy);");
        output_line(ctx, "}");
        // BO1: the reference is clamped to [0, 1] as Direct3D 9's fixed-point (D24) compare does; the maps are
        //  32-bit float here, and a receiver beyond the far plane must still compare as lit against a cleared map
        output_line(ctx, "float4 bo1_tex2d(sampler2DShadow s, float4 c, int kind, int mode) {");
        output_line(ctx, "    if (kind == 1) return float4(texture(s, float3(c.xy / c.w, clamp(c.z / c.w, 0.0, 1.0))));");
        output_line(ctx, "    if (kind == 3) return float4(textureLod(s, float3(c.xy, clamp(c.z, 0.0, 1.0)), 0.0));");
        output_line(ctx, "    return float4(texture(s, float3(c.xy, clamp(c.z, 0.0, 1.0))));");
        output_line(ctx, "}");
        output_line(ctx, "float4 bo1_tex2dgrad(sampler2D s, float4 c, float4 dx, float4 dy, int mode) {");
        output_line(ctx, "    return textureGrad(s, c.xy, dx.xy, dy.xy);");
        output_line(ctx, "}");
        output_line(ctx, "float4 bo1_tex2dgrad(sampler2DShadow s, float4 c, float4 dx, float4 dy, int mode) {");
        output_line(ctx, "    return float4(textureGrad(s, float3(c.xy, clamp(c.z, 0.0, 1.0)), dx.xy, dy.xy));");
        output_line(ctx, "}");
        output_line(ctx, "float4 bo1_texcube(samplerCube s, float4 c, int kind, int mode) {");
        output_line(ctx, "    if (kind == 1) return texture(s, c.xyz / c.w);");
        output_line(ctx, "    if (kind == 2) return texture(s, c.xyz, c.w);");
        output_line(ctx, "    if (kind == 3) return textureLod(s, c.xyz, c.w);");
        output_line(ctx, "    return texture(s, c.xyz);");
        output_line(ctx, "}");
        output_line(ctx, "float4 bo1_texcubegrad(samplerCube s, float4 c, float4 dx, float4 dy, int mode) {");
        output_line(ctx, "    return textureGrad(s, c.xyz, dx.xyz, dy.xyz);");
        output_line(ctx, "}");
        output_line(ctx, "float4 bo1_tex3d(sampler3D s, float4 c, int kind, int mode) {");
        output_line(ctx, "    if (kind == 1) return texture(s, c.xyz / c.w);");
        output_line(ctx, "    if (kind == 2) return texture(s, c.xyz, c.w);");
        output_line(ctx, "    if (kind == 3) return textureLod(s, c.xyz, c.w);");
        output_line(ctx, "    return texture(s, c.xyz);");
        output_line(ctx, "}");
        output_line(ctx, "float4 bo1_tex3dgrad(sampler3D s, float4 c, float4 dx, float4 dy, int mode) {");
        output_line(ctx, "    return textureGrad(s, c.xyz, dx.xyz, dy.xyz);");
        output_line(ctx, "}");
    } // else
    output_line(ctx, "#endif");
    output_line(ctx, "#ifndef BO1_ALPHA_TEST");
    output_line(ctx, "#define BO1_ALPHA_TEST(c)");
    output_line(ctx, "#endif");
    output_line(ctx, "#ifndef BO1_VS_EXTRA_OUTPUTS");
    output_line(ctx, "#define BO1_VS_EXTRA_OUTPUTS");
    output_line(ctx, "#define BO1_VS_EXTRA_INIT");
    output_line(ctx, "#endif");
    output_blank_line(ctx);
    pop_output(ctx);

    // The uniforms: one block per stage, packed as the Metal profile packs its struct (d3d9_shaders.cpp fills it):
    //  float registers, int registers, then one byte per bool register (four to a uint, four uints to a uvec4).
    push_output(ctx, &ctx->globals);
    const int uniform_count = ctx->uniform_float4_count + ctx->uniform_int4_count + ctx->uniform_bool_count;
    if (uniform_count > 0)
    {
        output_line(ctx, "layout(std140, binding = %d) uniform bo1_%s_uniforms", vs ? 0 : 1, vs ? "vs" : "ps");
        output_line(ctx, "{");
        ctx->indent++;
        if (ctx->uniform_float4_count > 0)
            output_line(ctx, "float4 bo1_%s_float4[%d];", vs ? "vs" : "ps", ctx->uniform_float4_count);
        if (ctx->uniform_int4_count > 0)
            output_line(ctx, "int4 bo1_%s_int4[%d];", vs ? "vs" : "ps", ctx->uniform_int4_count);
        if (ctx->uniform_bool_count > 0)
            output_line(ctx, "uvec4 bo1_%s_bool4[%d];", vs ? "vs" : "ps", (ctx->uniform_bool_count + 15) / 16);
        ctx->indent--;
        output_line(ctx, "};"); // (no instance name: GLSL 1.40 has none)
        if (ctx->uniform_bool_count > 0)
        {
            output_line(ctx, "bool bo1_bool(const int i) { return ((bo1_%s_bool4[i >> 4][(i >> 2) & 3] >> uint((i & 3) * 8)) & 0xFFu) != 0u; }", vs ? "vs" : "ps");
        } // if
    } // if
    // BO1: per-draw values of the Direct3D 9 layer for vertex shaders: .x is D3DRS_DEPTHBIAS in clip-space units,
    //  added to the position's depth below (as in the Metal renderer, whose constant depth bias scales with the depth
    //  of a 32-bit float buffer while Direct3D 9's 24-bit one does not)
    if (vs)
    {
        output_line(ctx, "layout(std140, binding = 2) uniform bo1_vsfix_block { float4 bo1_vsfix; };");
        // BO1: invariant, so that every vertex shader transforming the same vertex the same way produces the same
        //  depth (the depth prepass, then LESSEQUAL/EQUAL passes with other shaders, as Direct3D 9 guarantees)
        output_line(ctx, "invariant gl_Position;");
        // BO1: outputs the pipeline's pixel shader reads but this vertex shader does not write (the variant defines
        //  them; GL links a pixel shader input only to a vertex shader output)
        output_line(ctx, "BO1_VS_EXTRA_OUTPUTS");
    } // if
    output_blank_line(ctx);
    pop_output(ctx);

    // Fill in the shader's mainline function signature.
    push_output(ctx, &ctx->mainline_intro);
    output_line(ctx, "void main()");
    output_line(ctx, "{");
    if (vs)
    {
        ctx->indent++;
        output_line(ctx, "BO1_VS_EXTRA_INIT");  // BO1: see BO1_VS_EXTRA_OUTPUTS
        ctx->indent--;
    } // if
    pop_output(ctx);

    push_output(ctx, &ctx->mainline);
    ctx->indent++;
    // BO1: D3DRS_ALPHATESTENABLE (no-op unless the pipeline variant defines it)
    if (shader_is_pixel(ctx) && (reglist_find(&ctx->used_registers, REG_TYPE_COLOROUT, 0) != NULL))
        output_line(ctx, "BO1_ALPHA_TEST(bo1_oC0);");
    // BO1: D3DRS_DEPTHBIAS (bo1_vsfix above)
    if (vs)
    {
        output_line(ctx, "#ifdef BO1_VS_POS");
        output_line(ctx, "BO1_VS_POS.z += bo1_vsfix.x * BO1_VS_POS.w;");
        output_line(ctx, "#endif");
    } // if
    ctx->indent--;
    pop_output(ctx);

    // throw some blank lines around to make source more readable.
    if (ctx->inputs)
    {
        push_output(ctx, &ctx->inputs);
        output_blank_line(ctx);
        pop_output(ctx);
    } // if
    if (ctx->outputs)
    {
        push_output(ctx, &ctx->outputs);
        output_blank_line(ctx);
        pop_output(ctx);
    } // if
} // emit_GLSL_finalize

// BO1: the user() name shared by a vertex output and the pixel shader input it feeds, for every usage
static const char *bo1_varying_name(const MOJOSHADER_usage usage, const int index, char *buf, const size_t len)
{
    static const char *names[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
                                   "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth",
                                   "sample" };
    if (usage == MOJOSHADER_USAGE_FOG)
        snprintf(buf, len, "fog");
    else if (((int) usage >= 0) && ((size_t) usage < STATICARRAYLEN(names)))
        snprintf(buf, len, "%s%d", names[usage], index);
    else
        snprintf(buf, len, "varying%d_%d", (int) usage, index);
    return buf;
} // bo1_varying_name

void emit_GLSL_global(Context *ctx, RegisterType regtype, int regnum)
{
    char varname[512];
    get_GLSL_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    // Registers are locals of main(), set up at top of mainline.
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;

    switch (regtype)
    {
        case REG_TYPE_ADDRESS:
            if (shader_is_vertex(ctx))
                output_line(ctx, "int4 %s;", varname);
            else if (shader_is_pixel(ctx))  // actually REG_TYPE_TEXTURE.
            {
                // We have to map texture registers to temps for ps_1_1, since
                //  they work like temps, initialize with tex coords, and the
                //  ps_1_1 TEX opcode expects to overwrite it.
                if (!shader_version_atleast(ctx, 1, 4))
                    output_line(ctx, "float4 %s = bo1v_texcoord%d;", varname, regnum);
            } // else if
            break;
        case REG_TYPE_PREDICATE:
            output_line(ctx, "bool4 %s;", varname);
            break;
        case REG_TYPE_TEMP:
            output_line(ctx, "float4 %s;", varname);
            break;
        case REG_TYPE_LOOP:
            break; // no-op. We declare these in for loops at the moment.
        case REG_TYPE_LABEL:
            break; // no-op. If we see it here, it means we optimized it out.
        default:
            fail(ctx, "BUG: we used a register we don't know how to define.");
            break;
    } // switch

    pop_output(ctx);
} // emit_GLSL_global

void emit_GLSL_array(Context *ctx, VariableList *var)
{
    // All uniforms (except constant arrays, which are literally constant
    //  data embedded in the shader) are now packed into a single array,
    //  so we can batch the uniform transfers. So this doesn't actually
    //  define an array here; the one, big array is emitted during
    //  finalization instead.
    // However, we need to #define the offset into the one, big array here,
    //  and let dereferences use that #define.
    const int base = var->index;
    const int glslbase = ctx->uniform_float4_count;
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const int ARRAYBASE_%d = %d;", base, glslbase);
    pop_output(ctx);
    var->emit_position = glslbase;
} // emit_GLSL_array

void emit_GLSL_const_array(Context *ctx, const ConstantsList *clist,
                            int base, int size)
{
    char varname[512];
    get_GLSL_const_array_varname_in_buf(ctx,base,size,varname,sizeof(varname));

    const char *cstr = NULL;
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const float4 %s[%d] = float4[%d](", varname, size, size);
    ctx->indent++;

    int i;
    for (i = 0; i < size; i++)
    {
        while (clist->constant.type != MOJOSHADER_UNIFORM_FLOAT)
            clist = clist->next;
        assert(clist->constant.index == (base + i));

        char val0[32];
        char val1[32];
        char val2[32];
        char val3[32];
        floatstr(ctx, val0, sizeof (val0), clist->constant.value.f[0], 1);
        floatstr(ctx, val1, sizeof (val1), clist->constant.value.f[1], 1);
        floatstr(ctx, val2, sizeof (val2), clist->constant.value.f[2], 1);
        floatstr(ctx, val3, sizeof (val3), clist->constant.value.f[3], 1);

        output_line(ctx, "float4(%s, %s, %s, %s)%s", val0, val1, val2, val3,
                        (i < (size-1)) ? "," : "");

        clist = clist->next;
    } // for

    ctx->indent--;
    output_line(ctx, ");");
    pop_output(ctx);
} // emit_GLSL_const_array

void emit_GLSL_uniform(Context *ctx, RegisterType regtype, int regnum,
                              const VariableList *var)
{
    // Now that we're pushing all the uniforms as one struct, pack these
    //  down, so if we only use register c439, it'll actually map to
    //  bo1_vs_float4[0]. As we push one big struct, this will
    //  prevent uploading unused data.

    char varname[512];
    char name[512];
    int index = 0;

    get_GLSL_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;

    if (var == NULL)
    {
        get_GLSL_uniform_array_varname(ctx, regtype, name, sizeof (name));

        if (regtype == REG_TYPE_CONST)
            index = ctx->uniform_float4_count;
        else if (regtype == REG_TYPE_CONSTINT)
            index = ctx->uniform_int4_count;
        else if (regtype == REG_TYPE_CONSTBOOL)
            index = ctx->uniform_bool_count;
        else  // get_GLSL_uniform_array_varname() would have called fail().
            assert(!(ctx->isfail));

        // BO1: bool registers are bytes of the block's uvec4s (emit_GLSL_finalize)
        if (regtype == REG_TYPE_CONSTBOOL)
            output_line(ctx, "#define %s bo1_bool(%d)", varname, index);
        else
            output_line(ctx, "#define %s %s[%d]", varname, name, index);
        push_output(ctx, &ctx->mainline);
        ctx->indent++;
        output_line(ctx, "#undef %s", varname);  // !!! FIXME: gross.
        pop_output(ctx);
    } // if

    else
    {
        const int arraybase = var->index;
        if (var->constant)
        {
            get_GLSL_const_array_varname_in_buf(ctx, arraybase, var->count,
                                                name, sizeof (name));
            index = (regnum - arraybase);
        } // if
        else
        {
            assert(var->emit_position != -1);
            get_GLSL_uniform_array_varname(ctx, regtype, name, sizeof (name));
            index = (regnum - arraybase) + var->emit_position;
        } // else

        // !!! FIXME: might trigger unused var warnings in Clang.
        //output_line(ctx, "constant %s &%s = %s[%d];", utype, varname, name, index);
        // BO1: no trailing ';' (the macro is used inside expressions)
        output_line(ctx, "#define %s %s[%d]", varname, name, index);
        push_output(ctx, &ctx->mainline);
        ctx->indent++;
        output_line(ctx, "#undef %s", varname);  // !!! FIXME: gross.
        pop_output(ctx);
    } // else

    pop_output(ctx);
} // emit_GLSL_uniform

void emit_GLSL_sampler(Context *ctx,int stage,TextureType ttype,int tb)
{
    char var[512];
    const char *type = NULL;
    switch (ttype)
    {
        case TEXTURE_TYPE_2D: type = "sampler2D"; break;
        case TEXTURE_TYPE_CUBE: type = "samplerCube"; break;
        case TEXTURE_TYPE_VOLUME: type = "sampler3D"; break;
        default: assert(!"unexpected texture type"); return;
    } // switch

    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, stage, var, sizeof (var));

    // BO1: 2D textures may be depth textures (shadow maps); the pipeline variant redefines these.
    push_output(ctx, &ctx->preflight);
    if (ttype == TEXTURE_TYPE_2D)
    {
        output_line(ctx, "#ifndef BO1_TEX2D_TYPE_%d", stage);
        output_line(ctx, "#define BO1_TEX2D_TYPE_%d sampler2D", stage);
        output_line(ctx, "#define BO1_TEX2D_MODE_%d 0", stage);
        output_line(ctx, "#endif");
    } // if
    pop_output(ctx);

    // texture unit: the sampler register for pixel shaders, 16 + the register for vertex shaders (D3D9's
    //  D3DVERTEXTEXTURESAMPLER0-3)
    const int unit = shader_is_vertex(ctx) ? 16 + stage : stage;
    push_output(ctx, &ctx->globals);
    if (ttype == TEXTURE_TYPE_2D)
        output_line(ctx, "layout(binding = %d) uniform BO1_TEX2D_TYPE_%d %s;", unit, stage, var);
    else
        output_line(ctx, "layout(binding = %d) uniform %s %s;", unit, type, var);
    pop_output(ctx);

    if (tb)  // This sampler used a ps_1_1 TEXBEM opcode?
    {
        push_output(ctx, &ctx->mainline_top);
        ctx->indent++;
        char name[512];
        const int index = ctx->uniform_float4_count;
        ctx->uniform_float4_count += 2;
        get_GLSL_uniform_array_varname(ctx, REG_TYPE_CONST, name, sizeof (name));
        output_line(ctx, "float4 %s_texbem = %s[%d];", var, name, index);
        output_line(ctx, "float4 %s_texbeml = %s[%d];", var, name, index+1);
        pop_output(ctx);
    } // if
} // emit_GLSL_sampler

void emit_GLSL_attribute(Context *ctx, RegisterType regtype, int regnum,
                          MOJOSHADER_usage usage, int index, int wmask,
                          int flags)
{
    // !!! FIXME: this function doesn't deal with write masks at all yet!
    char var[512];
    char vname[64];

    get_GLSL_varname_in_buf(ctx, regtype, regnum, var, sizeof (var));

    //assert((flags & MOD_PP) == 0);  // !!! FIXME: is PP allowed?

    if (shader_is_vertex(ctx))
    {
        // a scalar register (vs_1/vs_2 oFog, oPts) writes one component of its varying
        const int scalar = isscalar(ctx, ctx->shader_type, regtype, regnum);

        // pre-vs3 output registers.
        // these don't ever happen in DCL opcodes, I think. Map to vs_3_*
        //  output registers.
        if (!shader_version_atleast(ctx, 3, 0))
        {
            if (regtype == REG_TYPE_RASTOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                index = regnum;
                switch ((const RastOutType) regnum)
                {
                    case RASTOUT_TYPE_POSITION:
                        usage = MOJOSHADER_USAGE_POSITION;
                        index = 0;
                        break;
                    case RASTOUT_TYPE_FOG:
                        usage = MOJOSHADER_USAGE_FOG;
                        index = 0;
                        break;
                    case RASTOUT_TYPE_POINT_SIZE:
                        usage = MOJOSHADER_USAGE_POINTSIZE;
                        index = 0;
                        break;
                } // switch
            } // if

            else if (regtype == REG_TYPE_ATTROUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_COLOR;
                index = regnum;
            } // else if

            else if (regtype == REG_TYPE_TEXCRDOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_TEXCOORD;
                index = regnum;
            } // else if
        } // if

        if (regtype == REG_TYPE_INPUT)
        {
            // BO1: the input register number is the attribute location (the pipeline's vertex layout describes
            //  each one); integer vertex formats are converted to float by the vertex fetch
            push_output(ctx, &ctx->inputs);
            output_line(ctx, "layout(location = %d) in float4 %s;", regnum, var);
            pop_output(ctx);
        } // if

        else if (regtype == REG_TYPE_OUTPUT)
        {
            const char *target = NULL;
            char member[96];
            if ((usage == MOJOSHADER_USAGE_POSITION) && (index == 0))
            {
                target = "gl_Position";  // (invariant: emit_GLSL_finalize)
                push_output(ctx, &ctx->preflight);
                output_line(ctx, "#define BO1_VS_POS gl_Position");
                pop_output(ctx);
            } // if
            else if (usage == MOJOSHADER_USAGE_POINTSIZE)
            {
                target = "gl_PointSize";
            } // else if
            else
            {
                // BO1: every usage is a varying named after it (bo1_varying_name), as the pixel shader's input is
                bo1_varying_name(usage, index, vname, sizeof (vname));
                push_output(ctx, &ctx->outputs);
                output_line(ctx, "out float4 bo1v_%s;", vname);
                pop_output(ctx);
                snprintf(member, sizeof (member), scalar ? "bo1v_%s.x" : "bo1v_%s", vname);
                target = member;
            } // else

            push_output(ctx, &ctx->mainline_top);
            ctx->indent++;
            output_line(ctx, "#define %s %s", var, target);
            pop_output(ctx);
            push_output(ctx, &ctx->mainline);
            ctx->indent++;
            output_line(ctx, "#undef %s", var);  // !!! FIXME: gross.
            pop_output(ctx);
        } // else if

        else
        {
            fail(ctx, "unknown vertex shader attribute register");
        } // else
    } // if

    else if (shader_is_pixel(ctx))
    {
        // samplers DCLs get handled in emit_GLSL_sampler().

        // BO1: centroid interpolation instead of failing
        const char *interp = (flags & MOD_CENTROID) ? "centroid " : "";

        if ((regtype == REG_TYPE_COLOROUT) || (regtype == REG_TYPE_DEPTHOUT))
        {
            char target[64];
            if (regtype == REG_TYPE_COLOROUT)
            {
                push_output(ctx, &ctx->outputs);
                output_line(ctx, "layout(location = %d) out float4 bo1_oC%d;", regnum, regnum);
                pop_output(ctx);
                snprintf(target, sizeof (target), "bo1_oC%d", regnum);
            } // if
            else
            {
                snprintf(target, sizeof (target), "gl_FragDepth");
            } // else

            push_output(ctx, &ctx->mainline_top);
            ctx->indent++;
            output_line(ctx, "#define %s %s", var, target);
            pop_output(ctx);
            push_output(ctx, &ctx->mainline);
            ctx->indent++;
            output_line(ctx, "#undef %s", var);  // !!! FIXME: gross.
            pop_output(ctx);
        } // if

        // !!! FIXME: can you actualy have a texture register with COLOR usage?
        else if ((regtype == REG_TYPE_TEXTURE) ||
                 (regtype == REG_TYPE_INPUT) ||
                 (regtype == REG_TYPE_MISCTYPE))
        {
            int skipreference = 0;
            char target[128];

            if (regtype == REG_TYPE_MISCTYPE)
            {
                const MiscTypeType mt = (MiscTypeType) regnum;
                // BO1: D3D9 puts pixel centers on integer coordinates, GL on half-integers
                if (mt == MISCTYPE_TYPE_POSITION)
                    snprintf(target, sizeof (target), "(gl_FragCoord - float4(0.5, 0.5, 0.0, 0.0))");
                // BO1: vFace is a float in D3D9 (positive on front faces)
                else if (mt == MISCTYPE_TYPE_FACE)
                    snprintf(target, sizeof (target), "(gl_FrontFacing ? 1.0 : -1.0)");
                else
                {
                    fail(ctx, "BUG: unhandled misc register");
                    return;
                } // else
            } // if

            else
            {
                if (usage == MOJOSHADER_USAGE_TEXCOORD)
                {
                    // ps_1_1 does a different hack for this attribute.
                    //  Refer to emit_GLSL_global()'s REG_TYPE_ADDRESS code.
                    if (!shader_version_atleast(ctx, 1, 4))
                        skipreference = 1;
                } // if

                // BO1: every usage (named as the vertex outputs are, bo1_varying_name)
                bo1_varying_name(usage, index, vname, sizeof (vname));
                push_output(ctx, &ctx->inputs);
                output_line(ctx, "%sin float4 bo1v_%s;", interp, vname);
                pop_output(ctx);
                snprintf(target, sizeof (target), "bo1v_%s", vname);
            } // else

            if (!skipreference)
            {
                push_output(ctx, &ctx->mainline_top);
                ctx->indent++;
                output_line(ctx, "#define %s %s", var, target);
                pop_output(ctx);
                push_output(ctx, &ctx->mainline);
                ctx->indent++;
                output_line(ctx, "#undef %s", var);  // !!! FIXME: gross.
                pop_output(ctx);
            } // if
        } // else if

        else
        {
            fail(ctx, "unknown pixel shader attribute register");
        } // else
    } // else if

    else
    {
        fail(ctx, "Unknown shader type");  // state machine should catch this.
    } // else
} // emit_GLSL_attribute

void emit_GLSL_NOP(Context *ctx)
{
    // no-op is a no-op.  :)
} // emit_GLSL_NOP

void emit_GLSL_MOV(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_MOV

void emit_GLSL_ADD(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[4096];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s + %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_ADD

void emit_GLSL_SUB(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[4096];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s - %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_SUB

void emit_GLSL_MAD(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char src2[512]; make_GLSL_srcarg_string_masked(ctx, 2, src2, sizeof (src2));
    char code[4096];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "(%s * %s) + %s", src0, src1, src2);
    output_line(ctx, "%s", code);
} // emit_GLSL_MAD

void emit_GLSL_MUL(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[4096];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s * %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_MUL

void emit_GLSL_RCP(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char cast[16] = { '\0' };
    if (vecsize != 1)
        snprintf(cast, sizeof (cast), "float%d", vecsize);
    char src0[512]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_math = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s((%s == 0.0) ? FLT_MAX : 1.0 / %s)", cast, src0, src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_RCP

void emit_GLSL_RSQ(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char cast[16] = { '\0' };
    if (vecsize != 1)
        snprintf(cast, sizeof (cast), "float%d", vecsize);
    char src0[512]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_math = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s((%s == 0.0) ? FLT_MAX : inversesqrt(abs(%s)))", cast, src0, src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_RSQ

void emit_GLSL_dotprod(Context *ctx, const char *src0, const char *src1,
                              const char *extra)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char castleft[16] = { '\0' };
    const char *castright = "";
    if (vecsize != 1)
    {
        snprintf(castleft, sizeof (castleft), "float%d(", vecsize);
        castright = ")";
    } // if

    char code[4096];
    ctx->metal_need_header_geometric = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%sdot(%s, %s)%s%s",
                             castleft, src0, src1, extra, castright);
    output_line(ctx, "%s", code);
} // emit_GLSL_dotprod

void emit_GLSL_DP3(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_vec3(ctx, 1, src1, sizeof (src1));
    emit_GLSL_dotprod(ctx, src0, src1, "");
} // emit_GLSL_DP3

void emit_GLSL_DP4(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_full(ctx, 1, src1, sizeof (src1));
    emit_GLSL_dotprod(ctx, src0, src1, "");
} // emit_GLSL_DP4

void emit_GLSL_MIN(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[4096];
    ctx->metal_need_header_math = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "min(%s, %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_MIN

void emit_GLSL_MAX(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[4096];
    ctx->metal_need_header_math = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "max(%s, %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_MAX

void emit_GLSL_SLT(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[4096];

    // float(bool) or vec(bvec) results in 0.0 or 1.0, like SLT wants.
    if (vecsize == 1)
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "float(%s < %s)", src0, src1);
    else
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                  "float%d(lessThan(%s, %s))", vecsize, src0, src1);
    } // else
    output_line(ctx, "%s", code);
} // emit_GLSL_SLT

void emit_GLSL_SGE(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[4096];

    // float(bool) or vec(bvec) results in 0.0 or 1.0, like SGE wants.
    if (vecsize == 1)
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "float(%s >= %s)", src0, src1);
    } // if
    else
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                  "float%d(greaterThanEqual(%s, %s))", vecsize, src0, src1);
    } // else
    output_line(ctx, "%s", code);
} // emit_GLSL_SGE

void emit_GLSL_EXP(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_math = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "exp2(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_EXP

void emit_GLSL_LOG(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_math = 1;
    // BO1: Direct3D 9's log is of the absolute value (as rsq is)
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "log2(abs(%s))", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_LOG

void emit_GLSL_LIT_helper(Context *ctx)
{
    const char *maxp = "127.9961"; // value from the dx9 reference.

    if (ctx->glsl_generated_lit_helper)
        return;

    ctx->glsl_generated_lit_helper = 1;
    ctx->metal_need_header_common = 1;
    ctx->metal_need_header_math = 1;

    push_output(ctx, &ctx->helpers);
    output_line(ctx, "float4 LIT(const float4 src)");
    output_line(ctx, "{"); ctx->indent++;
    output_line(ctx,   "const float power = clamp(src.w, -%s, %s);",maxp,maxp);
    output_line(ctx,   "float4 retval = float4(1.0, 0.0, 0.0, 1.0);");
    output_line(ctx,   "if (src.x > 0.0) {"); ctx->indent++;
    output_line(ctx,     "retval.y = src.x;");
    output_line(ctx,     "if (src.y > 0.0) {"); ctx->indent++;
    output_line(ctx,       "retval.z = pow(src.y, power);"); ctx->indent--;
    output_line(ctx,     "}"); ctx->indent--;
    output_line(ctx,   "}");
    output_line(ctx,   "return retval;"); ctx->indent--;
    output_line(ctx, "}");
    output_blank_line(ctx);
    pop_output(ctx);
} // emit_GLSL_LIT_helper

// BO1: the destination write mask as a swizzle (".yz"; "" for .xyzw), for instructions whose result is a float4
//  whatever the mask (lit, dst, crs): the assignment's two sides must have the same width
static const char *bo1_dest_mask_swizzle(Context *ctx, char *buf, const size_t buflen)
{
    const DestArgInfo *arg = &ctx->dest_arg;
    size_t i = 0;
    if (isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum))
        buf[i++] = '.', buf[i++] = 'x';
    else if (!writemask_xyzw(arg->writemask))
    {
        buf[i++] = '.';
        if (arg->writemask0) buf[i++] = 'x';
        if (arg->writemask1) buf[i++] = 'y';
        if (arg->writemask2) buf[i++] = 'z';
        if (arg->writemask3) buf[i++] = 'w';
    } // else if
    assert(i < buflen);
    buf[i] = '\0';
    return buf;
} // bo1_dest_mask_swizzle

void emit_GLSL_LIT(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char mask[8]; bo1_dest_mask_swizzle(ctx, mask, sizeof (mask));
    char code[4096];
    emit_GLSL_LIT_helper(ctx);
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "LIT(%s)%s", src0, mask);
    output_line(ctx, "%s", code);
} // emit_GLSL_LIT

void emit_GLSL_DST(Context *ctx)
{
    char src0_y[512]; make_GLSL_srcarg_string_y(ctx, 0, src0_y, sizeof (src0_y));
    char src1_y[512]; make_GLSL_srcarg_string_y(ctx, 1, src1_y, sizeof (src1_y));
    char src0_z[512]; make_GLSL_srcarg_string_z(ctx, 0, src0_z, sizeof (src0_z));
    char src1_w[512]; make_GLSL_srcarg_string_w(ctx, 1, src1_w, sizeof (src1_w));
    char mask[8]; bo1_dest_mask_swizzle(ctx, mask, sizeof (mask));

    char code[4096];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                             "float4(1.0, %s * %s, %s, %s)%s",
                             src0_y, src1_y, src0_z, src1_w, mask);
    output_line(ctx, "%s", code);
} // emit_GLSL_DST

void emit_GLSL_LRP(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char src2[512]; make_GLSL_srcarg_string_masked(ctx, 2, src2, sizeof (src2));
    char code[4096];
    ctx->metal_need_header_common = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "mix(%s, %s, %s)",
                             src2, src1, src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_LRP

void emit_GLSL_FRC(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_math = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "fract(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_FRC

void emit_GLSL_M4X4(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char row0[512]; make_GLSL_srcarg_string_full(ctx, 1, row0, sizeof (row0));
    char row1[512]; make_GLSL_srcarg_string_full(ctx, 2, row1, sizeof (row1));
    char row2[512]; make_GLSL_srcarg_string_full(ctx, 3, row2, sizeof (row2));
    char row3[512]; make_GLSL_srcarg_string_full(ctx, 4, row3, sizeof (row3));
    char code[4096];
    ctx->metal_need_header_geometric = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                    "float4(dot(%s, %s), dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                    src0, row0, src0, row1, src0, row2, src0, row3);
    output_line(ctx, "%s", code);
} // emit_GLSL_M4X4

void emit_GLSL_M4X3(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char row0[512]; make_GLSL_srcarg_string_full(ctx, 1, row0, sizeof (row0));
    char row1[512]; make_GLSL_srcarg_string_full(ctx, 2, row1, sizeof (row1));
    char row2[512]; make_GLSL_srcarg_string_full(ctx, 3, row2, sizeof (row2));
    char code[4096];
    ctx->metal_need_header_geometric = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                "float3(dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1, src0, row2);
    output_line(ctx, "%s", code);
} // emit_GLSL_M4X3

void emit_GLSL_M3X4(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[512]; make_GLSL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[512]; make_GLSL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));
    char row2[512]; make_GLSL_srcarg_string_vec3(ctx, 3, row2, sizeof (row2));
    char row3[512]; make_GLSL_srcarg_string_vec3(ctx, 4, row3, sizeof (row3));
    char code[4096];
    ctx->metal_need_header_geometric = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                "float4(dot(%s, %s), dot(%s, %s), "
                                     "dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1,
                                src0, row2, src0, row3);
    output_line(ctx, "%s", code);
} // emit_GLSL_M3X4

void emit_GLSL_M3X3(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[512]; make_GLSL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[512]; make_GLSL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));
    char row2[512]; make_GLSL_srcarg_string_vec3(ctx, 3, row2, sizeof (row2));
    char code[4096];
    ctx->metal_need_header_geometric = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                "float3(dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1, src0, row2);
    output_line(ctx, "%s", code);
} // emit_GLSL_M3X3

void emit_GLSL_M3X2(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[512]; make_GLSL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[512]; make_GLSL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));
    char code[4096];
    ctx->metal_need_header_geometric = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                "float2(dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1);
    output_line(ctx, "%s", code);
} // emit_GLSL_M3X2

// BO1: subroutines (CALL/CALLNZ/LABEL) are inlined at their call sites. A separate Metal function could not reach
//  the registers, inputs, outputs and uniforms, which are locals of the shader's main function; D3D9 has no
//  recursion and a call depth of at most 4, so the expansion (emit_GLSL_finalize) terminates. A call inside a
//  LOOP sees that loop's aL, as D3D9 requires.
static void glsl_emit_call_marker(Context *ctx, const int label)
{
    output_line(ctx, "{");
    output_line(ctx, "/*BO1_CALL_%d*/", label);
    output_line(ctx, "}");
} // glsl_emit_call_marker

void emit_GLSL_CALL(Context *ctx)
{
    glsl_emit_call_marker(ctx, ctx->source_args[0].regnum);
} // emit_GLSL_CALL

void emit_GLSL_CALLNZ(Context *ctx)
{
    // BO1: the condition is one component (b#, or p0 with a replicate swizzle): the masked form used the previous
    // instruction's write mask and gave "if (p0.yy)", which is not a condition in Metal
    char src1[512]; make_GLSL_srcarg_string_scalar(ctx, 1, src1, sizeof (src1));
    output_line(ctx, "if (%s) {", src1);
    ctx->indent++;
    glsl_emit_call_marker(ctx, ctx->source_args[0].regnum);
    ctx->indent--;
    output_line(ctx, "}");
} // emit_GLSL_CALLNZ

void emit_GLSL_LOOP(Context *ctx)
{
    // !!! FIXME: swizzle?
    char var[512]; get_GLSL_srcarg_varname(ctx, 1, var, sizeof (var));
    assert(ctx->source_args[0].regnum == 0);  // in case they add aL1 someday.
    output_line(ctx, "{");
    ctx->indent++;
    // BO1: D3D9 runs the loop i.x times with aL = i.y, i.y + i.z, ... (any step, including negative ones)
    output_line(ctx, "int4 aLparams = %s;", var);
    output_line(ctx, "int aL = aLparams.y;");
    output_line(ctx, "for (int aLiter = 0; aLiter < aLparams.x; aLiter++, aL += aLparams.z) {");
    ctx->indent++;
} // emit_GLSL_LOOP

void emit_GLSL_RET(Context *ctx)
{
    // thankfully, the MSDN specs say a RET _has_ to end a function...no
    //  early returns. So if you hit one, you know you can safely close
    //  a high-level function.
    if (ctx->metal_main_returned)
    {
        // BO1: the end of a subroutine body (kept for inlining, see glsl_emit_call_marker)
        ctx->indent = 0;
        set_output(ctx, &ctx->subroutines);
        return;
    } // if
    ctx->metal_main_returned = 1;
    push_output(ctx, &ctx->postflight);
    output_line(ctx, "}");
    output_blank_line(ctx);
    pop_output(ctx);  // BO1: balanced (parse_token asserts an empty output stack between instructions)
    ctx->indent = 0;
    set_output(ctx, &ctx->subroutines);  // !!! FIXME: is this for LABEL? Maybe set it there so we don't allocate unnecessarily.
} // emit_GLSL_RET

void emit_GLSL_ENDLOOP(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
    ctx->indent--;
    output_line(ctx, "}");
} // emit_GLSL_ENDLOOP

void emit_GLSL_LABEL(Context *ctx)
{
    const int label = ctx->source_args[0].regnum;
    RegisterList *reg = reglist_find(&ctx->used_registers, REG_TYPE_LABEL, label);

    // MSDN specs say CALL* has to come before the LABEL, so we know if we
    //  can ditch the entire function here as unused.
    // BO1: the body of a called one is collected on its own and inlined at the call sites (emit_GLSL_finalize).
    if ((reg == NULL) || (label < 0) || (label >= (int) STATICARRAYLEN(ctx->metal_label_bodies)))
        set_output(ctx, &ctx->ignore);  // Func not used. Parse, but don't output.
    else if (ctx->metal_label_bodies[label] != NULL)
    {
        failf(ctx, "label l%d defined twice", label);
        set_output(ctx, &ctx->ignore);
    } // else if
    else
        set_output(ctx, &ctx->metal_label_bodies[label]);
    ctx->indent = 1;
} // emit_GLSL_LABEL

void emit_GLSL_DCL(Context *ctx)
{
    // no-op. We do this in our emit_attribute() and emit_uniform().
} // emit_GLSL_DCL

void emit_GLSL_POW(Context *ctx)
{
    // BO1: pow is a scalar operation (replicate swizzles) whose result is 1 for a zero exponent, also for a zero
    // base (Metal's fast pow gives NaN for 0^0; Direct3D 9 hardware and Wine give 1)
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char cast[16] = { '\0' };
    if (vecsize != 1)
        snprintf(cast, sizeof (cast), "float%d", vecsize);
    char src0[512]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_scalar(ctx, 1, src1, sizeof (src1));
    char code[4096];
    ctx->metal_need_header_math = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                             "%s((%s == 0.0) ? 1.0 : pow(abs(%s), %s))", cast, src1, src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_POW

void emit_GLSL_CRS(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_vec3(ctx, 1, src1, sizeof (src1));
    char mask[8]; bo1_dest_mask_swizzle(ctx, mask, sizeof (mask));
    char code[4096];
    ctx->metal_need_header_geometric = 1;
    // BO1: crs writes any of .x/.y/.z (.w is left alone: Direct3D 9 doesn't allow it in the mask)
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                             "float4(cross(%s, %s), 0.0)%s", src0, src1, mask);
    output_line(ctx, "%s", code);
} // emit_GLSL_CRS

void emit_GLSL_SGN(Context *ctx)
{
    // (we don't need the temporary registers specified for the D3D opcode.)
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_common = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "sign(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_SGN

void emit_GLSL_ABS(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_math = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "abs(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_ABS

void emit_GLSL_NRM(Context *ctx)
{
    // BO1: Direct3D 9 divides every written component by the length of src.xyz (w does not count, even with an
    // .xyzw mask), and a zero vector stays zero instead of becoming NaN (as on the hardware; Wine does the same)
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src3[512]; make_GLSL_srcarg_string_vec3(ctx, 0, src3, sizeof (src3));
    char code[4096];
    ctx->metal_need_header_geometric = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "((dot(%s, %s) == 0.0) ? %s : (%s * inversesqrt(dot(%s, %s))))",
                              src3, src3, src0, src0, src3, src3);
    output_line(ctx, "%s", code);
} // emit_GLSL_NRM

void emit_GLSL_SINCOS(Context *ctx)
{
    // we don't care about the temp registers that <= sm2 demands; ignore them.
    //  sm2 also talks about what components are left untouched vs. undefined,
    //  but we just leave those all untouched with Metal write masks (which
    //  would fulfill the "undefined" requirement, too).
    const int mask = ctx->dest_arg.writemask;
    char src0[512]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char code[4096] = { '\0' };

    ctx->metal_need_header_math = 1;
    if (writemask_x(mask))
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "cos(%s)", src0);
    else if (writemask_y(mask))
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "sin(%s)", src0);
    else if (writemask_xy(mask))
    {
        // !!! FIXME: can use sincos(), but need to assign cos to a temp, since it needs a reference.
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "float2(cos(%s), sin(%s))", src0, src0);
    } // else if

    output_line(ctx, "%s", code);
} // emit_GLSL_SINCOS

void emit_GLSL_REP(Context *ctx)
{
    // !!! FIXME:
    // msdn docs say legal loop values are 0 to 255. We can check DEFI values
    //  at parse time, but if they are pulling a value from a uniform, do
    //  we clamp here?
    // !!! FIXME: swizzle is legal here, right?
    char src0[512]; make_GLSL_srcarg_string_x(ctx, 0, src0, sizeof (src0));
    const uint rep = (uint) ctx->reps;
    output_line(ctx, "for (int rep%u = 0; rep%u < %s; rep%u++) {",
                rep, rep, src0, rep);
    ctx->indent++;
} // emit_GLSL_REP

void emit_GLSL_ENDREP(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
} // emit_GLSL_ENDREP

void emit_GLSL_IF(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    output_line(ctx, "if (%s) {", src0);
    ctx->indent++;
} // emit_GLSL_IF

void emit_GLSL_IFC(Context *ctx)
{
    const char *comp = get_GLSL_comparison_string_scalar(ctx);
    char src0[512]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_scalar(ctx, 1, src1, sizeof (src1));
    output_line(ctx, "if (%s %s %s) {", src0, comp, src1);
    ctx->indent++;
} // emit_GLSL_IFC

void emit_GLSL_ELSE(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "} else {");
    ctx->indent++;
} // emit_GLSL_ELSE

void emit_GLSL_ENDIF(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
} // emit_GLSL_ENDIF

void emit_GLSL_BREAK(Context *ctx)
{
    output_line(ctx, "break;");
} // emit_GLSL_BREAK

void emit_GLSL_BREAKC(Context *ctx)
{
    const char *comp = get_GLSL_comparison_string_scalar(ctx);
    char src0[512]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_scalar(ctx, 1, src1, sizeof (src1));
    output_line(ctx, "if (%s %s %s) { break; }", src0, comp, src1);
} // emit_GLSL_BREAKC

void emit_GLSL_MOVA(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];

    ctx->metal_need_header_math = 1;
    ctx->metal_need_header_common = 1;

    if (vecsize == 1)
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "int(floor(abs(%s) + 0.5) * sign(%s))",
                                 src0, src0);
    } // if

    else
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                            "int%d(floor(abs(%s) + float%d(0.5)) * sign(%s))",
                            vecsize, src0, vecsize, src0);
    } // else

    output_line(ctx, "%s", code);
} // emit_GLSL_MOVA

void emit_GLSL_DEFB(Context *ctx)
{
    char varname[512]; get_GLSL_destarg_varname(ctx, varname, sizeof (varname));
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const bool %s = %s;",
                varname, ctx->dwords[0] ? "true" : "false");
    pop_output(ctx);
} // emit_GLSL_DEFB

void emit_GLSL_DEFI(Context *ctx)
{
    char varname[512]; get_GLSL_destarg_varname(ctx, varname, sizeof (varname));
    const int32 *x = (const int32 *) ctx->dwords;
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const int4 %s = int4(%d, %d, %d, %d);",
                varname, (int) x[0], (int) x[1], (int) x[2], (int) x[3]);
    pop_output(ctx);
} // emit_GLSL_DEFI

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXCRD)

void emit_GLSL_TEXKILL(Context *ctx)
{
    char dst[512]; get_GLSL_destarg_varname(ctx, dst, sizeof (dst));
    // BO1: Shader Model 2 and later test all four components (as Direct3D 9 hardware does, see Wine's texkill);
    // only ps_1_x looks at xyz
    const char *test = shader_version_atleast(ctx, 2, 0) ? "any(lessThan(%s, float4(0.0)))" : "any(lessThan(%s.xyz, float3(0.0)))";
    char cond[512];
    snprintf(cond, sizeof (cond), test, dst);
    ctx->metal_need_header_relational = 1;
    ctx->metal_need_header_graphics = 1;
    if (ctx->predicated)
    {
        // BO1: predicated texkill (the predicate is a replicate swizzle)
        const SourceArgInfo *parg = &ctx->predicate_arg;
        char pregnum[16];
        const char *pregtype = get_GLSL_register_string(ctx, parg->regtype, parg->regnum, pregnum, sizeof (pregnum));
        output_line(ctx, "if (%s%s%s.%c && %s) discard;",
                    (parg->src_mod == SRCMOD_NOT) ? "!" : "", pregtype, pregnum,
                    swizzle_channels[parg->swizzle & 0x3], cond);
        return;
    } // if
    output_line(ctx, "if (%s) discard;", cond);
} // emit_GLSL_TEXKILL

static void glsl_texld(Context *ctx, const int texldd, const int texldl)
{
    ctx->metal_need_header_texture = 1;
    if (!shader_version_atleast(ctx, 1, 4))
    {
        DestArgInfo *info = &ctx->dest_arg;
        char dst[512];
        char sampler[512];
        char code[4096] = {0};

        assert(!texldd);

        RegisterList *sreg;
        sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER, info->regnum);
        const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);

        char swizzle[4] = { 'x', 'y', 'z', '\0' };
        if (ttype == TEXTURE_TYPE_2D)
            swizzle[2] = '\0';  // "xy" instead of "xyz".

        // !!! FIXME: this code counts on the register not having swizzles, etc.
        get_GLSL_destarg_varname(ctx, dst, sizeof (dst));
        get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                                 sampler, sizeof (sampler));

        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                  "texture(%s, %s.%s)",
                                  sampler, dst, swizzle);
        output_line(ctx, "%s", code);
    } // if

    else if (!shader_version_atleast(ctx, 2, 0))
    {
        // ps_1_4 is different, too!
        fail(ctx, "TEXLD == Shader Model 1.4 unimplemented.");  // !!! FIXME
        return;
    } // else if

    else
    {
        // BO1: every SM2+ lookup goes through the bo1_tex* helpers, which take the full source register so that
        //  projection, bias, explicit LOD and depth comparison can be resolved there.
        const SourceArgInfo *samp_arg = &ctx->source_args[1];
        RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                          samp_arg->regnum);
        char src0[128] = { '\0' };
        char src1[512]; get_GLSL_srcarg_varname(ctx, 1, src1, sizeof (src1)); // !!! FIXME: SRC_MOD?
        char src2[128] = { '\0' };
        char src3[128] = { '\0' };

        if (sreg == NULL)
        {
            fail(ctx, "TEXLD using undeclared sampler");
            return;
        } // if

        const char *fn = NULL;
        const int vs = shader_is_vertex(ctx);
        switch ((const TextureType) sreg->index)
        {
            case TEXTURE_TYPE_2D: fn = vs ? "bo1_vtex2d" : "bo1_tex2d"; break;
            case TEXTURE_TYPE_CUBE: fn = vs ? "bo1_vtexcube" : "bo1_texcube"; break;
            case TEXTURE_TYPE_VOLUME: fn = vs ? "bo1_vtex3d" : "bo1_tex3d"; break;
            default:
                fail(ctx, "unknown texture type");
                return;
        } // switch

        char mode[32];
        if (sreg->index == TEXTURE_TYPE_2D)
            snprintf(mode, sizeof (mode), "BO1_TEX2D_MODE_%d", samp_arg->regnum);
        else
            snprintf(mode, sizeof (mode), "0");

        make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));

        int kind = 0;
        if (texldl)
            kind = 3;
        else if (ctx->instruction_controls == CONTROL_TEXLDP)
        {
            // BO1: texCUBEproj compiles to texldp on a cube map (bo1_texcube divides by w too: a negative w turns
            //  the direction around)
            kind = 1;
        } // else if
        else if (ctx->instruction_controls == CONTROL_TEXLDB)
            kind = 2;
        else if (shader_is_vertex(ctx))
            kind = 3;  // vertex functions have no implicit LOD

        assert(!isscalar(ctx, ctx->shader_type, samp_arg->regtype, samp_arg->regnum));
        char swiz_str[6] = { '\0' };
        make_GLSL_swizzle_string(swiz_str, sizeof (swiz_str),
                                 samp_arg->swizzle, ctx->dest_arg.writemask);

        char code[4096];
        if (vs)
        {
            // vertex shaders: texldl only (vs_3_0)
            make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                     "%s(%s, %s, %s)%s",
                                     fn, src1, src0, mode, swiz_str);
        } // if
        else if (texldd)
        {
            make_GLSL_srcarg_string_full(ctx, 2, src2, sizeof (src2));
            make_GLSL_srcarg_string_full(ctx, 3, src3, sizeof (src3));
            make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                     "%sgrad(%s, %s, %s, %s, %s)%s",
                                     fn, src1, src0, src2, src3, mode, swiz_str);
        } // if
        else
        {
            make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                     "%s(%s, %s, %d, %s)%s",
                                     fn, src1, src0, kind, mode, swiz_str);
        } // else

        output_line(ctx, "%s", code);
    } // else
} // metal_texld

void emit_GLSL_TEXLD(Context *ctx)
{
    glsl_texld(ctx, 0, 0);
} // emit_GLSL_TEXLD
    

void emit_GLSL_TEXBEM(Context *ctx)
{
    DestArgInfo *info = &ctx->dest_arg;
    char dst[512]; get_GLSL_destarg_varname(ctx, dst, sizeof (dst));
    char src[512]; get_GLSL_srcarg_varname(ctx, 0, src, sizeof (src));
    char sampler[512];
    char code[4096];

    ctx->metal_need_header_texture = 1;

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture(%s, float2(%s.x + (%s_texbem.x * %s.x) + (%s_texbem.z * %s.y),"
        " %s.y + (%s_texbem.y * %s.x) + (%s_texbem.w * %s.y)))",
        sampler,
        dst, sampler, src, sampler, src,
        dst, sampler, src, sampler, src);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXBEM


void emit_GLSL_TEXBEML(Context *ctx)
{
    // !!! FIXME: this code counts on the register not having swizzles, etc.
    DestArgInfo *info = &ctx->dest_arg;
    char dst[512]; get_GLSL_destarg_varname(ctx, dst, sizeof (dst));
    char src[512]; get_GLSL_srcarg_varname(ctx, 0, src, sizeof (src));
    char sampler[512];
    char code[4096];

    ctx->metal_need_header_texture = 1;

    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "(texture(%s, float2(%s.x + (%s_texbem.x * %s.x) + (%s_texbem.z * %s.y),"
        " %s.y + (%s_texbem.y * %s.x) + (%s_texbem.w * %s.y)))) *"
        " ((%s.z * %s_texbeml.x) + %s_texbem.y)",
        sampler,
        dst, sampler, src, sampler, src,
        dst, sampler, src, sampler, src,
        src, sampler, sampler);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXBEML

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2AR) // !!! FIXME
EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2GB) // !!! FIXME


void emit_GLSL_TEXM3X2PAD(Context *ctx)
{
    // no-op ... work happens in emit_GLSL_TEXM3X2TEX().
} // emit_GLSL_TEXM3X2PAD

void emit_GLSL_TEXM3X2TEX(Context *ctx)
{
    if (ctx->texm3x2pad_src0 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[512];
    char src0[512];
    char src1[512];
    char src2[512];
    char sampler[512];
    char code[4096];

    ctx->metal_need_header_texture = 1;
    ctx->metal_need_header_geometric = 1;

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x2pad_src0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x2pad_dst0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src2, sizeof (src2));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture(%s, float2(dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz)))",
        sampler, src0, src1, src2, dst);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X2TEX

void emit_GLSL_TEXM3X3PAD(Context *ctx)
{
    // no-op ... work happens in emit_GLSL_TEXM3X3*().
} // emit_GLSL_TEXM3X3PAD

void emit_GLSL_TEXM3X3TEX(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[512];
    char src0[512];
    char src1[512];
    char src2[512];
    char src3[512];
    char src4[512];
    char sampler[512];
    char code[4096];

    ctx->metal_need_header_texture = 1;
    ctx->metal_need_header_geometric = 1;

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    (void) ttype;

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture(%s,"
            " float3(dot(%s.xyz, %s.xyz),"
            " dot(%s.xyz, %s.xyz),"
            " dot(%s.xyz, %s.xyz)))",
        sampler, src0, src1, src2, src3, dst, src4);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X3TEX

void emit_GLSL_TEXM3X3SPEC_helper(Context *ctx)
{
    if (ctx->glsl_generated_texm3x3spec_helper)
        return;

    ctx->glsl_generated_texm3x3spec_helper = 1;

    push_output(ctx, &ctx->helpers);
    output_line(ctx, "float3 TEXM3X3SPEC_reflection(const float3 normal, const float3 eyeray)");
    output_line(ctx, "{"); ctx->indent++;
    output_line(ctx,   "return (2.0 * ((normal * eyeray) / (normal * normal)) * normal) - eyeray;"); ctx->indent--;
    output_line(ctx, "}");
    output_blank_line(ctx);
    pop_output(ctx);
} // emit_GLSL_TEXM3X3SPEC_helper

void emit_GLSL_TEXM3X3SPEC(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[512];
    char src0[512];
    char src1[512];
    char src2[512];
    char src3[512];
    char src4[512];
    char src5[512];
    char sampler[512];
    char code[4096];

    ctx->metal_need_header_texture = 1;
    ctx->metal_need_header_geometric = 1;

    emit_GLSL_TEXM3X3SPEC_helper(ctx);

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[1].regnum,
                            src5, sizeof (src5));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    (void) ttype;

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture(%s, "
            "TEXM3X3SPEC_reflection("
                "float3("
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz)"
                "),"
                "%s.xyz,"
            ")"
        ")",
        sampler, src0, src1, src2, src3, dst, src4, src5);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X3SPEC

void emit_GLSL_TEXM3X3VSPEC(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[512];
    char src0[512];
    char src1[512];
    char src2[512];
    char src3[512];
    char src4[512];
    char sampler[512];
    char code[4096];

    ctx->metal_need_header_texture = 1;
    ctx->metal_need_header_geometric = 1;

    emit_GLSL_TEXM3X3SPEC_helper(ctx);

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    (void) ttype;

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture(%s, "
            "TEXM3X3SPEC_reflection("
                "float3("
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz)"
                "), "
                "float3(%s.w, %s.w, %s.w)"
            ")"
        ")",
        sampler, src0, src1, src2, src3, dst, src4, src0, src2, dst);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X3VSPEC

void emit_GLSL_EXPP(Context *ctx)
{
    // !!! FIXME: msdn's asm docs don't list this opcode, I'll have to check the driver documentation.
    emit_GLSL_EXP(ctx);  // I guess this is just partial precision EXP?
} // emit_GLSL_EXPP

void emit_GLSL_LOGP(Context *ctx)
{
    // LOGP is just low-precision LOG, but we'll take the higher precision.
    emit_GLSL_LOG(ctx);
} // emit_GLSL_LOGP

// common code between CMP and CND.
// BO1: one mix() for the whole write mask. The original emitted a statement per group of components, and when
//  the destination was also a source (cmp r0.xy, r0.yx, ...) the later statements read the components already
//  written by the earlier ones.
void emit_GLSL_comparison_operations(Context *ctx, const char *cmp, const char *cmpfn, const char *cmpvalue)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char src2[512]; make_GLSL_srcarg_string_masked(ctx, 2, src2, sizeof (src2));
    char code[4096];
    ctx->metal_need_header_relational = 1;
    // the condition as wide as the mask, also when src0 is a scalar register (vFace)
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    const SourceArgInfo *arg0 = &ctx->source_args[0];
    if (vecsize > 1 && !isscalar(ctx, ctx->shader_type, ctx->dest_arg.regtype, ctx->dest_arg.regnum))
    {
        // (GLSL compares vectors with functions; a scalar src0 (vFace) is compared once for every component)
        if (isscalar(ctx, ctx->shader_type, arg0->regtype, arg0->regnum))
            make_GLSL_destarg_assign(ctx, code, sizeof (code), "mix(%s, %s, bool%d(%s %s %s))", src2, src1, vecsize, src0, cmp, cmpvalue);
        else
            make_GLSL_destarg_assign(ctx, code, sizeof (code), "mix(%s, %s, %s(%s, float%d(%s)))", src2, src1, cmpfn, src0, vecsize, cmpvalue);
    } // if
    else
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "mix(%s, %s, (%s %s %s))", src2, src1, src0, cmp, cmpvalue);
    output_line(ctx, "%s", code);
} // emit_GLSL_comparison_operations

void emit_GLSL_CND(Context *ctx)
{
    emit_GLSL_comparison_operations(ctx, ">", "greaterThan", "0.5");
} // emit_GLSL_CND

void emit_GLSL_DEF(Context *ctx)
{
    const float *val = (const float *) ctx->dwords; // !!! FIXME: could be int?
    char varname[512]; get_GLSL_destarg_varname(ctx, varname, sizeof (varname));
    char val0[32]; floatstr(ctx, val0, sizeof (val0), val[0], 1);
    char val1[32]; floatstr(ctx, val1, sizeof (val1), val[1], 1);
    char val2[32]; floatstr(ctx, val2, sizeof (val2), val[2], 1);
    char val3[32]; floatstr(ctx, val3, sizeof (val3), val[3], 1);

    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const float4 %s = float4(%s, %s, %s, %s);",
                varname, val0, val1, val2, val3);
    pop_output(ctx);
} // emit_GLSL_DEF

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2RGB) // !!! FIXME
EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3TEX) // !!! FIXME
EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X2DEPTH) // !!! FIXME
EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3) // !!! FIXME

void emit_GLSL_TEXM3X3(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    char dst[512];
    char src0[512];
    char src1[512];
    char src2[512];
    char src3[512];
    char src4[512];
    char code[4096];

    ctx->metal_need_header_geometric = 1;

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "float4(dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz), 1.0)",
        src0, src1, src2, src3, dst, src4);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X3

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXDEPTH) // !!! FIXME

void emit_GLSL_CMP(Context *ctx)
{
    emit_GLSL_comparison_operations(ctx, ">=", "greaterThanEqual", "0.0");
} // emit_GLSL_CMP

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(BEM) // !!! FIXME

void emit_GLSL_DP2ADD(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_vec2(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_vec2(ctx, 1, src1, sizeof (src1));
    char src2[512]; make_GLSL_srcarg_string_scalar(ctx, 2, src2, sizeof (src2));
    char extra[512]; snprintf(extra, sizeof (extra), " + %s", src2);
    emit_GLSL_dotprod(ctx, src0, src1, extra);
} // emit_GLSL_DP2ADD

void emit_GLSL_DSX(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_graphics = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "dFdx(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_DSX

void emit_GLSL_DSY(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[4096];
    ctx->metal_need_header_graphics = 1;
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "dFdy(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_DSY

void emit_GLSL_TEXLDD(Context *ctx)
{
    glsl_texld(ctx, 1, 0);
} // emit_GLSL_TEXLDD

void emit_GLSL_SETP(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[512]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[512]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[4096];

    // destination is always predicate register (which is type bvec4).
    const char *comp = (vecsize == 1) ?
            get_GLSL_comparison_string_scalar(ctx) :
            get_GLSL_comparison_string_vector(ctx);

    if (vecsize == 1)
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "(%s %s %s)", src0, comp, src1);
    else
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s(%s, %s)", comp, src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_SETP

void emit_GLSL_TEXLDL(Context *ctx)
{
    // BO1: explicit LOD from src0.w (textureLod works in pixel and vertex shaders)
    glsl_texld(ctx, 0, 1);
} // emit_GLSL_TEXLDL

void emit_GLSL_BREAKP(Context *ctx)
{
    char src0[512]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    output_line(ctx, "if (%s) { break; }", src0);
} // emit_GLSL_BREAKP

void emit_GLSL_RESERVED(Context *ctx)
{
    // do nothing; fails in the state machine.
} // emit_GLSL_RESERVED

#endif  // SUPPORT_PROFILE_GLSL

#pragma GCC visibility pop

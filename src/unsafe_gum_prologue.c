#define _GNU_SOURCE
#include "unsafe_gum_prologue.h"

#include <stdint.h>
#include <string.h>

PeakUnsafeGumProloguePolicy
peak_unsafe_gum_prologue_policy_from_env(const char* value,
                                         gboolean* valid_out)
{
    if (valid_out != NULL) {
        *valid_out = TRUE;
    }

    if (value == NULL || value[0] == '\0' ||
        g_ascii_strcasecmp(value, "default") == 0 ||
        g_ascii_strcasecmp(value, "audited") == 0 ||
        g_ascii_strcasecmp(value, "narrow") == 0) {
        return PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;
    }

    if (g_ascii_strcasecmp(value, "conservative") == 0 ||
        g_ascii_strcasecmp(value, "broad") == 0) {
        return PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE;
    }

    if (valid_out != NULL) {
        *valid_out = FALSE;
    }
    return PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;
}

const char*
peak_unsafe_gum_prologue_policy_name(PeakUnsafeGumProloguePolicy policy)
{
    switch (policy) {
    case PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT:
        return "default";
    case PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE:
        return "conservative";
    default:
        return "unknown";
    }
}

#if defined(__x86_64__) || defined(__amd64__)
static gboolean
peak_x86_match_endbr(const guint8* code, gsize available, gsize* size_out)
{
    if (available >= 4 &&
        code[0] == 0xf3 && code[1] == 0x0f && code[2] == 0x1e &&
        (code[3] == 0xfa || code[3] == 0xfb)) {
        *size_out = 4;
        return TRUE;
    }

    return FALSE;
}

static gboolean
peak_x86_match_zero_rdx(const guint8* code, gsize* size_out)
{
    if ((code[0] == 0x30 || code[0] == 0x32 ||
         code[0] == 0x28 || code[0] == 0x2a ||
         code[0] == 0x31 || code[0] == 0x33 ||
         code[0] == 0x29 || code[0] == 0x2b) &&
        code[1] == 0xd2) {
        *size_out = 2;
        return TRUE;
    }

    if (code[0] == 0x48 &&
        (code[1] == 0x31 || code[1] == 0x33 ||
         code[1] == 0x29 || code[1] == 0x2b) &&
        code[2] == 0xd2) {
        *size_out = 3;
        return TRUE;
    }

    if (code[0] == 0xb2 && code[1] == 0x00) {
        *size_out = 2;
        return TRUE;
    }

    if (code[0] == 0xba &&
        code[1] == 0x00 && code[2] == 0x00 &&
        code[3] == 0x00 && code[4] == 0x00) {
        *size_out = 5;
        return TRUE;
    }

    return FALSE;
}

static gboolean
peak_x86_match_zero_eax(const guint8* code)
{
    return (code[0] == 0x31 || code[0] == 0x33) && code[1] == 0xc0;
}

static gboolean
peak_x86_match_first_indexed_load_after_zero(const guint8* code)
{
    return code[0] == 0x8b && code[1] == 0x0c &&
           (code[2] == 0x07 || code[2] == 0x38);
}

static gboolean
peak_x86_match_rdx_counter_update_after_load(const guint8* code)
{
    if (code[0] == 0xfe && code[1] == 0xc2) {
        return TRUE;
    }

    if (code[0] == 0xff && code[1] == 0xc2) {
        return TRUE;
    }

    if (code[0] == 0x48 && code[1] == 0xff && code[2] == 0xc2) {
        return TRUE;
    }

    if (code[0] == 0x80 && (code[1] == 0xc2 || code[1] == 0xea) &&
        code[2] != 0x00) {
        return TRUE;
    }

    if (code[0] == 0x83 && (code[1] == 0xc2 || code[1] == 0xea) &&
        code[2] != 0x00) {
        return TRUE;
    }

    if (code[0] == 0x48 && code[1] == 0x83 &&
        (code[2] == 0xc2 || code[2] == 0xea) &&
        code[3] != 0x00) {
        return TRUE;
    }

    return FALSE;
}

static gboolean
peak_x86_has_high_movabs_at_entry(const guint8* code)
{
    gsize endbr_size = 0;

    if (peak_x86_match_endbr(code, 4, &endbr_size)) {
        code += endbr_size;
    }

    if (!(code[0] >= 0x40 && code[0] <= 0x4f &&
          (code[0] & 0x08u) != 0 &&
          (code[0] & 0x01u) != 0 &&
          code[1] >= 0xb8 && code[1] <= 0xbf)) {
        return FALSE;
    }

    for (gsize i = 0; i < 8; i++) {
        guint8 byte = code[2 + i];
        if (byte == 0xc2 || byte == 0xc3 ||
            byte == 0xca || byte == 0xcb) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_x86_immediate_contains_return_opcode(const guint8* immediate, gsize size)
{
    for (gsize i = 0; i < size; i++) {
        if (immediate[i] == 0xc2 || immediate[i] == 0xc3 ||
            immediate[i] == 0xca || immediate[i] == 0xcb) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_x86_modrm_tail_size(const guint8* code,
                         gsize available,
                         gsize* size_out)
{
    guint8 modrm;
    guint8 mod;
    guint8 rm;
    gsize size = 1;

    if (available == 0) {
        return FALSE;
    }

    modrm = code[0];
    mod = modrm >> 6;
    rm = modrm & 0x7u;

    if (mod != 3 && rm == 4) {
        guint8 sib;
        guint8 base;

        if (available < size + 1) {
            return FALSE;
        }
        sib = code[size++];
        base = sib & 0x7u;
        if (mod == 0 && base == 5) {
            size += 4;
        }
    } else if (mod == 0 && rm == 5) {
        size += 4;
    }

    if (mod == 1) {
        size += 1;
    } else if (mod == 2) {
        size += 4;
    }

    if (size > available) {
        return FALSE;
    }

    *size_out = size;
    return TRUE;
}

static gboolean
peak_x86_decode_instruction_size(const guint8* code,
                                 gsize available,
                                 gsize* size_out,
                                 gboolean* is_return_out)
{
    gsize offset = 0;
    gboolean rex_w = FALSE;
    gboolean operand16 = FALSE;
    guint8 op;
    gsize modrm_tail = 0;

    *is_return_out = FALSE;

    while (offset < available) {
        guint8 prefix = code[offset];

        if (prefix == 0x66) {
            operand16 = TRUE;
            offset++;
        } else if (prefix == 0xf0 || prefix == 0xf2 || prefix == 0xf3 ||
                   prefix == 0x2e || prefix == 0x36 || prefix == 0x3e ||
                   prefix == 0x26 || prefix == 0x64 || prefix == 0x65 ||
                   prefix == 0x67) {
            offset++;
        } else if (prefix >= 0x40 && prefix <= 0x4f) {
            rex_w = (prefix & 0x08u) != 0;
            offset++;
        } else {
            break;
        }
    }

    if (offset >= available) {
        return FALSE;
    }

    op = code[offset++];
    if (op == 0xc3 || op == 0xcb) {
        *size_out = offset;
        *is_return_out = TRUE;
        return TRUE;
    }
    if (op == 0xc2 || op == 0xca) {
        if (available < offset + 2) {
            return FALSE;
        }
        *size_out = offset + 2;
        *is_return_out = TRUE;
        return TRUE;
    }

    if (op >= 0xb8 && op <= 0xbf) {
        gsize imm_size = rex_w ? 8 : (operand16 ? 2 : 4);
        if (available < offset + imm_size) {
            return FALSE;
        }
        *size_out = offset + imm_size;
        return TRUE;
    }

    if (op == 0x68 || op == 0xe8 || op == 0xe9) {
        if (available < offset + 4) {
            return FALSE;
        }
        *size_out = offset + 4;
        return TRUE;
    }

    if ((op & 0xfeu) == 0x04 || (op & 0xfeu) == 0x0cu ||
        (op & 0xfeu) == 0x14 || (op & 0xfeu) == 0x1cu ||
        (op & 0xfeu) == 0x24 || (op & 0xfeu) == 0x2cu ||
        (op & 0xfeu) == 0x34 || (op & 0xfeu) == 0x3cu) {
        gsize imm_size = (op & 0x01u) != 0 ? (operand16 ? 2 : 4) : 1;
        if (available < offset + imm_size) {
            return FALSE;
        }
        *size_out = offset + imm_size;
        return TRUE;
    }

    if (op == 0x6a || op == 0xeb || (op >= 0x70 && op <= 0x7f)) {
        if (available < offset + 1) {
            return FALSE;
        }
        *size_out = offset + 1;
        return TRUE;
    }

    if ((op >= 0x50 && op <= 0x5f) || op == 0x90 || op == 0x9c ||
        op == 0x9d || op == 0xcc) {
        *size_out = offset;
        return TRUE;
    }

    if (op == 0x80 || op == 0x82 || op == 0x83 ||
        op == 0xc0 || op == 0xc1 || op == 0xc6) {
        if (!peak_x86_modrm_tail_size(&code[offset],
                                      available - offset,
                                      &modrm_tail)) {
            return FALSE;
        }
        if (available < offset + modrm_tail + 1) {
            return FALSE;
        }
        *size_out = offset + modrm_tail + 1;
        return TRUE;
    }

    if (op == 0x81 || op == 0xc7) {
        gsize imm_size = operand16 ? 2 : 4;
        if (!peak_x86_modrm_tail_size(&code[offset],
                                      available - offset,
                                      &modrm_tail)) {
            return FALSE;
        }
        if (available < offset + modrm_tail + imm_size) {
            return FALSE;
        }
        *size_out = offset + modrm_tail + imm_size;
        return TRUE;
    }

    if ((op <= 0x3b && (op & 0x07u) <= 0x03u) ||
        op == 0x84 || op == 0x85 ||
        (op >= 0x88 && op <= 0x8f) ||
        op == 0xfe || op == 0xff) {
        if (!peak_x86_modrm_tail_size(&code[offset],
                                      available - offset,
                                      &modrm_tail)) {
            return FALSE;
        }
        *size_out = offset + modrm_tail;
        return TRUE;
    }

    if (op == 0x0f) {
        if (available <= offset) {
            return FALSE;
        }
        op = code[offset++];
        if (op == 0x05 || op == 0x34) {
            *size_out = offset;
            return TRUE;
        }
        if (op >= 0x80 && op <= 0x8f) {
            if (available < offset + 4) {
                return FALSE;
            }
            *size_out = offset + 4;
            return TRUE;
        }
        if ((op >= 0x90 && op <= 0x9f) ||
            (op >= 0xb6 && op <= 0xbf)) {
            if (!peak_x86_modrm_tail_size(&code[offset],
                                          available - offset,
                                          &modrm_tail)) {
                return FALSE;
            }
            *size_out = offset + modrm_tail;
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_x86_has_return_before_entry_patch(const guint8* code,
                                       gboolean* undecodable_out)
{
    const gsize min_patch_size = 5;
    gsize offset = 0;
    gsize endbr_size = 0;

    if (undecodable_out != NULL) {
        *undecodable_out = FALSE;
    }

    if (code == NULL) {
        return FALSE;
    }

    if (peak_x86_match_endbr(code, 4, &endbr_size)) {
        offset = endbr_size;
    }

    while (offset < min_patch_size) {
        gsize insn_size = 0;
        gboolean is_return = FALSE;

        if (!peak_x86_decode_instruction_size(&code[offset],
                                              min_patch_size - offset,
                                              &insn_size,
                                              &is_return) ||
            insn_size == 0) {
            if (undecodable_out != NULL) {
                *undecodable_out = TRUE;
            }
            return FALSE;
        }
        if (is_return) {
            return TRUE;
        }
        offset += insn_size;
    }

    return FALSE;
}

static gboolean
peak_x86_has_return_opcode_mov_immediate_at_entry(const guint8* code)
{
    gsize endbr_size = 0;
    gsize offset = 0;
    gboolean operand16 = FALSE;
    gboolean rex_w = FALSE;
    guint8 op;

    if (peak_x86_match_endbr(code, 4, &endbr_size)) {
        code += endbr_size;
    }

    while (offset < 8) {
        guint8 prefix = code[offset];
        if (prefix == 0x66) {
            operand16 = TRUE;
            offset++;
        } else if (prefix == 0xf0 || prefix == 0xf2 || prefix == 0xf3 ||
                   prefix == 0x2e || prefix == 0x36 || prefix == 0x3e ||
                   prefix == 0x26 || prefix == 0x64 || prefix == 0x65 ||
                   prefix == 0x67) {
            offset++;
        } else if (prefix >= 0x40 && prefix <= 0x4f) {
            rex_w = (prefix & 0x08u) != 0;
            offset++;
        } else {
            break;
        }
    }

    op = code[offset++];
    if (op >= 0xb8 && op <= 0xbf) {
        gsize imm_size = rex_w ? 8 : (operand16 ? 2 : 4);
        return peak_x86_immediate_contains_return_opcode(&code[offset],
                                                         imm_size);
    }

    return FALSE;
}

static gboolean
peak_x86_has_rdx_prefix_prologue(gpointer address)
{
    const guint8* code = (const guint8*)address;
    gsize endbr_size = 0;
    gsize zero_size = 0;
    const gsize zero_eax_size = 2;
    const gsize first_load_size = 3;

    if (code == NULL) {
        return FALSE;
    }

    if (peak_x86_match_endbr(code, 4, &endbr_size)) {
        code += endbr_size;
    }

    if (!peak_x86_match_zero_rdx(code, &zero_size)) {
        return FALSE;
    }

    /*
     * Frida Gum 16.5.9's x86 invoke trampoline may use RDX after executing
     * relocated prologue instructions but before returning to the original
     * function body. Tiny leaf loops such as MILC's f2d_4mat initialize a
     * DL/EDX/RDX value, zero EAX, and execute the first indexed load in
     * relocated bytes. GCC/local canaries prove live counter variants corrupt
     * application data when Gum clobbers RDX at jump-back, and Frontera Intel
     * canaries show even the short same-prefix variants can crash when Gum
     * attaches. Guard only this audited prefix family by default; unrelated
     * small copy prologues remain attachable.
     */
    if (!peak_x86_match_zero_eax(&code[zero_size]) ||
        !peak_x86_match_first_indexed_load_after_zero(
            &code[zero_size + zero_eax_size])) {
        return FALSE;
    }

    if (!peak_x86_match_rdx_counter_update_after_load(
            &code[zero_size + zero_eax_size + first_load_size])) {
        return FALSE;
    }

    return TRUE;
}
#endif

#if defined(__aarch64__)
static void
peak_arm64_note_ip(guint32 reg, gboolean* seen_x16, gboolean* seen_x17)
{
    if (reg == 16) {
        *seen_x16 = TRUE;
    } else if (reg == 17) {
        *seen_x17 = TRUE;
    }
}

static void
peak_arm64_note_prefix_operands(guint32 insn,
                                gboolean* seen_x16,
                                gboolean* seen_x17)
{
    guint32 rd = insn & 0x1fu;
    guint32 rn = (insn >> 5) & 0x1fu;
    guint32 rm = (insn >> 16) & 0x1fu;
    guint32 rt = rd;

    if ((insn & 0x7f800000u) == 0x52800000u ||
        (insn & 0x7f800000u) == 0x12800000u ||
        (insn & 0x7f800000u) == 0x72800000u) {
        peak_arm64_note_ip(rd, seen_x16, seen_x17);
        return;
    }

    if ((insn & 0x1f000000u) == 0x11000000u ||
        (insn & 0x1f000000u) == 0x51000000u) {
        peak_arm64_note_ip(rd, seen_x16, seen_x17);
        peak_arm64_note_ip(rn, seen_x16, seen_x17);
        return;
    }

    if ((insn & 0x1f000000u) == 0x0a000000u) {
        peak_arm64_note_ip(rd, seen_x16, seen_x17);
        peak_arm64_note_ip(rn, seen_x16, seen_x17);
        peak_arm64_note_ip(rm, seen_x16, seen_x17);
        return;
    }

    if ((insn & 0x1f000000u) == 0x10000000u) {
        peak_arm64_note_ip(rd, seen_x16, seen_x17);
        return;
    }

    if ((insn & 0x7e000000u) == 0x34000000u) {
        peak_arm64_note_ip(rt, seen_x16, seen_x17);
        return;
    }

    if ((insn & 0x3b000000u) == 0x39000000u ||
        (insn & 0x3b200c00u) == 0x38000400u ||
        (insn & 0x3b200c00u) == 0x38000c00u ||
        (insn & 0x3b200c00u) == 0x38200800u) {
        peak_arm64_note_ip(rt, seen_x16, seen_x17);
        peak_arm64_note_ip(rn, seen_x16, seen_x17);
        return;
    }

    if ((insn & 0xfffffc1fu) == 0xd65f0000u) {
        peak_arm64_note_ip(rn, seen_x16, seen_x17);
    }
}

static gboolean
peak_arm64_conservative_both_ip_registers_in_prefix(gpointer address)
{
    const guint8* code = (const guint8*)address;
    gboolean seen_x16 = FALSE;
    gboolean seen_x17 = FALSE;

    if (code == NULL) {
        return FALSE;
    }

    for (gsize i = 0; i < 4; i++) {
        guint32 insn;

        memcpy(&insn, code + (i * 4), sizeof(insn));
        peak_arm64_note_prefix_operands(insn, &seen_x16, &seen_x17);
    }

    return seen_x16 && seen_x17;
}
#endif

static gboolean
peak_has_default_unsafe_prologue(gpointer address, const char** reason_out)
{
#if defined(__x86_64__) || defined(__amd64__)
    if (peak_x86_has_rdx_prefix_prologue(address)) {
        if (reason_out != NULL) {
            *reason_out = "x86-rdx-prefix";
        }
        return TRUE;
    }
#else
    (void)address;
#endif

    return FALSE;
}

static gboolean
peak_has_conservative_unsafe_prologue(gpointer address,
                                      const char** reason_out)
{
#if defined(__x86_64__) || defined(__amd64__)
    const guint8* code = (const guint8*)address;

    if (code == NULL) {
        return FALSE;
    }

    if (peak_x86_has_high_movabs_at_entry(code)) {
        if (reason_out != NULL) {
            *reason_out = "x86-high-movabs-entry";
        }
        return TRUE;
    }

    if (peak_x86_has_return_opcode_mov_immediate_at_entry(code)) {
        if (reason_out != NULL) {
            *reason_out = "x86-return-immediate-entry";
        }
        return TRUE;
    }
#elif defined(__aarch64__)
    if (peak_arm64_conservative_both_ip_registers_in_prefix(address)) {
        if (reason_out != NULL) {
            *reason_out = "arm64-both-ip-prefix-conservative";
        }
        return TRUE;
    }
#else
    (void)address;
#endif

    return FALSE;
}

gboolean
peak_unsafe_gum_prologue_check(gpointer address,
                               PeakUnsafeGumProloguePolicy policy,
                               const char** reason_out)
{
    if (reason_out != NULL) {
        *reason_out = NULL;
    }

    if (address == NULL) {
        return FALSE;
    }

    if (peak_has_default_unsafe_prologue(address, reason_out)) {
        return TRUE;
    }

    if (policy == PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE &&
        peak_has_conservative_unsafe_prologue(address, reason_out)) {
        return TRUE;
    }

    return FALSE;
}

gboolean
peak_gum_prologue_too_short_for_attach(gpointer address,
                                       const char** reason_out)
{
    if (reason_out != NULL) {
        *reason_out = NULL;
    }

    if (address == NULL) {
        return FALSE;
    }

#if defined(__x86_64__) || defined(__amd64__)
    if (peak_x86_has_return_before_entry_patch((const guint8*)address,
                                               NULL)) {
        if (reason_out != NULL) {
            *reason_out = "x86-return-before-entry-patch";
        }
        return TRUE;
    }
#else
    (void)address;
#endif

    return FALSE;
}

gboolean
peak_unsafe_gum_support_prologue_check(gpointer address,
                                       const char** reason_out)
{
    if (reason_out != NULL) {
        *reason_out = NULL;
    }

    (void)address;
    return FALSE;
}

/* Local entry-patch sizing for the pinned Arm64 relocator. CHECKED requests
 * inspect at most the requested 16-byte prefix. This is not a function CFG or
 * a proof about incoming edges or data reads outside that visible prefix. */
#include <stdint.h>

typedef struct {
  uintptr_t target;
  uintptr_t literal_start;
  guint literal_size;
  guint8 used_ip;
  gboolean uses_requested;
  gboolean direct;
} PeakArm64PrefixInstruction;

static guint8
peak_arm64_prefix_ip (guint reg)
{
  if (reg == ARM64_REG_X16 || reg == ARM64_REG_W16) return 1;
  if (reg == ARM64_REG_X17 || reg == ARM64_REG_W17) return 2;
  return 0;
}

/* Wn writes also overwrite Xn, including the separately enumerated FP/LR.
 * Other register classes retain exact matching; no suffix liveness is inferred. */
static arm64_reg
peak_arm64_prefix_gpr_alias (arm64_reg reg)
{
  if (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W28)
    return ARM64_REG_X0 + (reg - ARM64_REG_W0);
  if (reg == ARM64_REG_W29) return ARM64_REG_X29;
  if (reg == ARM64_REG_W30) return ARM64_REG_X30;
  return reg;
}

gboolean
gum_arm64_relocator_can_relocate (gpointer address, guint min_bytes,
    GumRelocationScenario scenario, GumRelocationPolicy policy,
    guint * maximum, arm64_reg * available_scratch_reg)
{
  PeakArm64PrefixInstruction instructions[4] = { { 0 } };
  uintptr_t entry = (uintptr_t) address;
  arm64_reg requested = available_scratch_reg != NULL ?
      *available_scratch_reg : ARM64_REG_INVALID;
  arm64_reg chosen = ARM64_REG_INVALID;
  csh capstone = 0;
  cs_insn * insn = NULL;
  guint8 * prefix = NULL;
  gsize available = 0;
  guint request, decoded = 0, limit = 0, candidate = 0, i;

  /* PEAK's ONLINE CHECKED path never delegates to the pinned suffix CFG.
   * Keep upstream OFFLINE and other explicit-policy API contracts unchanged. */
  if (scenario != GUM_SCENARIO_ONLINE || policy != GUM_RELOCATION_CHECKED)
    return peak_arm64_legacy_can_relocate (address, min_bytes, scenario, policy,
        maximum, available_scratch_reg);
  if (maximum != NULL) *maximum = 0;
  if (available_scratch_reg != NULL)
    *available_scratch_reg = ARM64_REG_INVALID;
  if ((entry & 3) != 0 || entry > UINTPTR_MAX - 16) return FALSE;
  request = min_bytes >= 16 ? 16 : MAX (4, (min_bytes + 3) & ~3u);
  /* A readable 8- or 4-byte prefix at a mapping end still participates in the
   * ordinary max16 fallback. No read starts beyond the requested prefix. */
  for (i = request; i >= 4; i = i > 8 ? 8 : i > 4 ? 4 : 0) {
    prefix = gum_memory_read (address, i, &available);
    if (prefix != NULL && available >= 4) break;
    g_free (prefix);
    prefix = NULL;
  }
  if (prefix == NULL) goto done;
  available = MIN (available, request) & ~((gsize) 3);
  cs_arch_register_arm64 ();
  if (cs_open (CS_ARCH_ARM64, GUM_DEFAULT_CS_ENDIAN, &capstone) != CS_ERR_OK ||
      cs_option (capstone, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) goto done;
  insn = cs_malloc (capstone);
  if (insn == NULL) goto done;
  while (decoded * 4 < available) {
    guint offset = decoded * 4;
    const guint8 * code = prefix + offset;
    size_t size = 4;
    uint64_t pc = entry + offset;
    PeakArm64PrefixInstruction * current = &instructions[decoded];
    cs_arm64 * detail;
    cs_regs reads, writes;
    uint8_t read_count = 0, write_count = 0;
    gint operand = -1;
    gboolean cutoff = FALSE, terminal = FALSE, call = FALSE;
    guint32 word;
    if (!cs_disasm_iter (capstone, &code, &size, &pc, insn) ||
        insn->size != 4 || insn->detail == NULL ||
        cs_regs_access (capstone, insn, reads, &read_count,
            writes, &write_count) != CS_ERR_OK) break;
    detail = &insn->detail->arm64;
    for (i = 0; i != read_count; i++) {
      current->used_ip |= peak_arm64_prefix_ip (reads[i]);
      current->uses_requested |= requested != ARM64_REG_INVALID &&
          peak_arm64_prefix_gpr_alias (reads[i]) == peak_arm64_prefix_gpr_alias (requested);
    }
    for (i = 0; i != write_count; i++) {
      current->used_ip |= peak_arm64_prefix_ip (writes[i]);
      current->uses_requested |= requested != ARM64_REG_INVALID &&
          peak_arm64_prefix_gpr_alias (writes[i]) == peak_arm64_prefix_gpr_alias (requested);
    }
    switch (insn->id) {
      case ARM64_INS_B:
        operand = 0; cutoff = TRUE; break;
      case ARM64_INS_CBZ: case ARM64_INS_CBNZ:
        operand = 1; cutoff = TRUE; break;
      case ARM64_INS_TBZ: case ARM64_INS_TBNZ:
        operand = 2; cutoff = TRUE; break;
      case ARM64_INS_BL:
        operand = 0; call = TRUE; break;
      case ARM64_INS_BLR: case ARM64_INS_BLRAA: case ARM64_INS_BLRAAZ:
      case ARM64_INS_BLRAB: case ARM64_INS_BLRABZ:
        call = TRUE; break;
      case ARM64_INS_RET: case ARM64_INS_RETAA: case ARM64_INS_RETAB:
        terminal = TRUE; break;
      case ARM64_INS_SVC:
        terminal = scenario == GUM_SCENARIO_ONLINE; break;
      case ARM64_INS_BR: case ARM64_INS_BRAA: case ARM64_INS_BRAAZ:
      case ARM64_INS_BRAB: case ARM64_INS_BRABZ:
      case ARM64_INS_BC: case ARM64_INS_BRK: case ARM64_INS_HLT:
      case ARM64_INS_ERET: case ARM64_INS_ERETAA: case ARM64_INS_ERETAB:
      case ARM64_INS_DRPS:
        cutoff = TRUE; break;
      default:
        cutoff = cs_insn_group (capstone, insn, CS_GRP_JUMP) ||
            cs_insn_group (capstone, insn, CS_GRP_IRET) ||
            cs_insn_group (capstone, insn, CS_GRP_RET) ||
            cs_insn_group (capstone, insn, CS_GRP_INT);
        break;
    }
    if (operand >= 0) {
      if (detail->op_count <= operand ||
          detail->operands[operand].type != ARM64_OP_IMM) break;
      current->direct = TRUE;
      current->target = (uintptr_t) detail->operands[operand].imm;
    }
    word = (guint32) insn->bytes[0] | ((guint32) insn->bytes[1] << 8) |
        ((guint32) insn->bytes[2] << 16) | ((guint32) insn->bytes[3] << 24);
    if ((insn->id == ARM64_INS_LDR || insn->id == ARM64_INS_LDRSW) &&
        (word & 0x3b000000u) == 0x18000000u) {
      guint opc = word >> 30;
      gboolean vector = (word & (1u << 26)) != 0;
      gint64 displacement = (word >> 5) & 0x7ffff;
      if (opc > 2) break;
      if (displacement & 0x40000) displacement -= 0x80000;
      displacement *= 4;
      current->literal_size = vector ? 4u << opc : opc == 1 ? 8 : 4;
      if ((displacement >= 0 && entry + offset > UINTPTR_MAX - displacement) ||
          (displacement < 0 && entry + offset < (guint64) -displacement)) break;
      current->literal_start = displacement >= 0 ? entry + offset + displacement :
          entry + offset - -displacement;
      if (current->literal_start > UINTPTR_MAX - current->literal_size) break;
    }
    decoded++;
    if (cutoff) {
      /* The first excluded branch was already visible while sizing. Its known
       * target may still force a smaller range (e.g. +8 B -> entry+4). */
      limit = offset;
      break;
    }
    limit = offset + 4;
    if (terminal || (call && scenario == GUM_SCENARIO_ONLINE)) break;
  }
  for (i = 16; i >= 4; i /= 2) {
    guint j;
    guint8 used = 0;
    gboolean safe = i <= limit;
    gboolean uses_requested = FALSE;
    if (!safe) continue;
    for (j = 0; j != decoded; j++) {
      PeakArm64PrefixInstruction * current = &instructions[j];
      if ((current->direct && current->target > entry &&
           current->target - entry < i) ||
          (current->literal_size != 0 && current->literal_start < entry + i &&
           entry < current->literal_start + current->literal_size)) safe = FALSE;
      if (j * 4 < i) {
        used |= current->used_ip;
        uses_requested |= current->uses_requested;
      }
    }
    if (!safe) continue;
    if (requested != ARM64_REG_INVALID) {
      if (!uses_requested) chosen = requested;
    } else if (!(used & 1)) chosen = ARM64_REG_X16;
    else if (!(used & 2)) chosen = ARM64_REG_X17;
    if (chosen != ARM64_REG_INVALID) { candidate = i; break; }
  }
done:
  g_free (prefix);
  if (insn != NULL) cs_free (insn, 1);
  if (capstone != 0) cs_close (&capstone);
  if (maximum != NULL) *maximum = candidate;
  if (available_scratch_reg != NULL) *available_scratch_reg = chosen;
  return candidate >= min_bytes;
}

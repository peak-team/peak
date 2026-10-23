/* Checked ONLINE relocation for the pinned Gum 17.15.3 source member.
 * Keep the upstream object ABI and rewriters; prove a usable patch range and
 * both kinds of scratch-register exit before handing it to the backend. */
#include <stdint.h>

#define PEAK_ARM64_RANGE_BUDGET 4096

typedef struct {
  uintptr_t pc;
  uintptr_t target;
  uintptr_t literal_start;
  guint literal_size;
  guint next[2];
  guint next_count;
  guint8 reads;
  guint8 writes;
  gboolean direct;
  gboolean fall_through;
  gboolean call;
  gboolean decoded;
  gboolean complete;
} PeakArm64RangeNode;

typedef struct {
  PeakArm64RangeNode * nodes;
  guint count;
  GHashTable * indices;
  csh capstone;
  cs_insn * instruction;
  guint8 * cache;
  uintptr_t cache_base;
  gsize cache_size;
} PeakArm64RangeScan;

static guint8
peak_arm64_range_register (guint reg)
{
  if (reg == ARM64_REG_X16 || reg == ARM64_REG_W16) return 1;
  if (reg == ARM64_REG_X17 || reg == ARM64_REG_W17) return 2;
  return 0;
}

static gboolean
peak_arm64_range_add (PeakArm64RangeScan * scan, uintptr_t pc, guint * index)
{
  gpointer found;
  if ((pc & 3) != 0) return FALSE;
  found = g_hash_table_lookup (scan->indices, (gpointer) pc);
  if (found != NULL) {
    *index = GPOINTER_TO_UINT (found) - 1;
    return TRUE;
  }
  if (scan->count == PEAK_ARM64_RANGE_BUDGET) return FALSE;
  *index = scan->count++;
  scan->nodes[*index].pc = pc;
  g_hash_table_insert (scan->indices, (gpointer) pc,
      GUINT_TO_POINTER (*index + 1));
  return TRUE;
}

static gboolean
peak_arm64_range_decode (PeakArm64RangeScan * scan, guint index)
{
  PeakArm64RangeNode * node = &scan->nodes[index];
  const guint8 * code;
  size_t size = 4;
  uint64_t pc = node->pc;
  cs_insn * insn = scan->instruction;
  cs_arm64 * detail;
  cs_regs reads, writes;
  uint8_t read_count = 0, write_count = 0;
  guint32 word;
  gint operand = -1;
  guint i;

  if (scan->cache == NULL || node->pc < scan->cache_base ||
      node->pc - scan->cache_base + 4 > scan->cache_size) {
    g_free (scan->cache);
    scan->cache_base = node->pc & ~((uintptr_t) 1023);
    scan->cache = gum_interceptor_peak_read_relocation_code (
        (gpointer) scan->cache_base, 1024,
        &scan->cache_size);
    if (scan->cache == NULL ||
        node->pc - scan->cache_base + 4 > scan->cache_size) {
      /* An unsupported hook elsewhere in the cache window must not turn an
       * unrelated instruction into an opaque path. Ask for just this word. */
      g_free (scan->cache);
      scan->cache_base = node->pc;
      scan->cache = gum_interceptor_peak_read_relocation_code (
          (gpointer) node->pc, 4, &scan->cache_size);
      if (scan->cache == NULL || scan->cache_size != 4) return FALSE;
    }
  }
  code = scan->cache + (node->pc - scan->cache_base);
  if (!cs_disasm_iter (scan->capstone, &code, &size, &pc, insn) ||
      insn->size != 4 || insn->detail == NULL ||
      cs_regs_access (scan->capstone, insn, reads, &read_count,
          writes, &write_count) != CS_ERR_OK) return FALSE;
  for (i = 0; i != read_count; i++)
    node->reads |= peak_arm64_range_register (reads[i]);
  for (i = 0; i != write_count; i++)
    node->writes |= peak_arm64_range_register (writes[i]);
  /* Capstone in this devkit marks the destination of SBFM/UBFM aliases
   * (e.g. SXTW) as read+write. The ISA writes the whole destination; BFM,
   * which really merges the old destination, deliberately stays untouched. */
  word = (guint32) insn->bytes[0] | ((guint32) insn->bytes[1] << 8) |
      ((guint32) insn->bytes[2] << 16) | ((guint32) insn->bytes[3] << 24);
  if (((word & 0x7f800000u) == 0x13000000u ||
       (word & 0x7f800000u) == 0x53000000u) &&
      ((word >> 5) & 31) != (word & 31)) {
    guint destination = word & 31;
    if (destination == 16) node->reads &= ~1u;
    if (destination == 17) node->reads &= ~2u;
  }
  node->decoded = TRUE;
  /* Literal loads keep their original data address in the upstream rewriter.
   * Check the complete read interval, including a vector load starting before
   * the entry. ADR/ADRP merely form an address and impose no read interval. */
  if ((insn->id == ARM64_INS_LDR || insn->id == ARM64_INS_LDRSW) &&
      (word & 0x3b000000u) == 0x18000000u) {
    guint opc = word >> 30;
    gboolean vector = (word & (1u << 26)) != 0;
    gint64 offset = (word >> 5) & 0x7ffff;
    if (offset & 0x40000) offset -= 0x80000;
    offset *= 4;
    if (vector) {
      if (opc > 2) return FALSE;
      node->literal_size = 4u << opc;
    } else {
      if (opc > 2) return FALSE;
      node->literal_size = opc == 1 ? 8 : 4;
    }
    if ((offset >= 0 && node->pc > UINTPTR_MAX - (guint64) offset) ||
        (offset < 0 && node->pc < (guint64) -offset)) return FALSE;
    node->literal_start = offset >= 0 ? node->pc + offset : node->pc - -offset;
    if (node->literal_start > UINTPTR_MAX - node->literal_size) return FALSE;
  }
  detail = &insn->detail->arm64;
  node->fall_through = TRUE;
  switch (insn->id) {
    case ARM64_INS_B:
      operand = 0;
      node->fall_through = detail->cc != ARM64_CC_INVALID &&
          detail->cc != ARM64_CC_AL && detail->cc != ARM64_CC_NV;
      break;
    case ARM64_INS_CBZ: case ARM64_INS_CBNZ:
      operand = 1;
      break;
    case ARM64_INS_TBZ: case ARM64_INS_TBNZ:
      operand = 2;
      break;
    case ARM64_INS_RET: case ARM64_INS_RETAA: case ARM64_INS_RETAB:
      node->fall_through = FALSE;
      break;
    case ARM64_INS_BL:
      operand = 0;
      node->call = TRUE;
      break;
    case ARM64_INS_BLR:
    case ARM64_INS_BLRAA: case ARM64_INS_BLRAAZ:
    case ARM64_INS_BLRAB: case ARM64_INS_BLRABZ:
      node->call = TRUE;
      break;
    case ARM64_INS_SVC:
      /* Linux resumes at the next instruction without the function-call
       * IP0/IP1 clobber contract. The prefix ONLINE stop remains unchanged. */
      break;
    case ARM64_INS_BR: case ARM64_INS_BRAA: case ARM64_INS_BRAAZ:
    case ARM64_INS_BRAB: case ARM64_INS_BRABZ:
    case ARM64_INS_ERET: case ARM64_INS_ERETAA: case ARM64_INS_ERETAB:
    case ARM64_INS_DRPS: case ARM64_INS_BC:
      return FALSE;
    default:
      if (cs_insn_group (scan->capstone, insn, CS_GRP_JUMP) ||
          cs_insn_group (scan->capstone, insn, CS_GRP_IRET) ||
          cs_insn_group (scan->capstone, insn, CS_GRP_RET) ||
          cs_insn_group (scan->capstone, insn, CS_GRP_INT)) return FALSE;
      break;
  }
  if (operand >= 0) {
    if (detail->op_count <= operand ||
        detail->operands[operand].type != ARM64_OP_IMM) return FALSE;
    node->direct = TRUE;
    node->target = (uintptr_t) detail->operands[operand].imm;
    if (!gum_interceptor_peak_relocation_target_available ((gpointer) node->target))
      return FALSE;
    /* A known BL destination can enter overwritten bytes, although normal
     * call/return analysis does not recursively walk the callee body. */
    if (!node->call) {
      if (!peak_arm64_range_add (scan, node->target,
          &node->next[node->next_count])) return FALSE;
      node->next_count++;
    }
  }
  if (node->fall_through) {
    if (node->pc > UINTPTR_MAX - 4 || !peak_arm64_range_add (scan,
        node->pc + 4, &node->next[node->next_count])) return FALSE;
    node->next_count++;
  }
  node->complete = TRUE;
  return TRUE;
}

/* Every path must overwrite the old register value before reading it. Calls
 * kill IP0/IP1 under the same normal ABI call/return assumption as Gum. */
static gboolean
peak_arm64_range_dead_at (PeakArm64RangeScan * scan, uintptr_t pc, guint8 reg)
{
  guint pending[PEAK_ARM64_RANGE_BUDGET];
  guint8 visited[PEAK_ARM64_RANGE_BUDGET] = { 0 };
  gpointer found = g_hash_table_lookup (scan->indices, (gpointer) pc);
  guint head = 0, tail = 0;
  if (found == NULL) return FALSE;
  pending[tail++] = GPOINTER_TO_UINT (found) - 1;
  visited[pending[0]] = 1;
  while (head != tail) {
    PeakArm64RangeNode * node = &scan->nodes[pending[head++]];
    guint i;
    if (!node->decoded || (node->reads & reg) != 0) return FALSE;
    if ((node->writes & reg) != 0 || node->call) continue;
    if (!node->complete) return FALSE;
    for (i = 0; i != node->next_count; i++) {
      guint next = node->next[i];
      if (visited[next]) continue;
      visited[next] = 1;
      pending[tail++] = next;
    }
  }
  return TRUE;
}

static gboolean
peak_arm64_range_candidate (PeakArm64RangeScan * scan, uintptr_t entry,
                            guint span, arm64_reg requested,
                            arm64_reg * chosen)
{
  guint i;
  guint8 used = 0;
  PeakArm64RangeNode * last = NULL;
  if (!gum_interceptor_peak_relocation_range_available ((gpointer) entry, span))
    return FALSE;
  for (i = 0; i != scan->count; i++) {
    PeakArm64RangeNode * node = &scan->nodes[i];
    if (node->literal_size != 0 && node->literal_start < entry + span &&
        entry < node->literal_start + node->literal_size) return FALSE;
    if (node->direct && node->target > entry &&
        node->target - entry < span) return FALSE;
    if (node->pc >= entry && node->pc - entry < span) {
      if (!node->complete) return FALSE;
      used |= node->reads | node->writes;
      if (node->pc - entry == span - 4) last = node;
      /* These upstream rewriters use X16 independently of backend scratch. */
      if (node->direct && !node->call && !peak_arm64_range_dead_at (scan, node->target, 1))
        return FALSE;
    }
  }
  for (i = 0; i != 2; i++) {
    arm64_reg reg = i == 0 ? ARM64_REG_X16 : ARM64_REG_X17;
    guint8 bit = 1 << i;
    if (requested != ARM64_REG_INVALID &&
        peak_arm64_range_register (requested) != bit) continue;
    if ((used & bit) != 0) continue;
    if (last != NULL && last->fall_through &&
        !peak_arm64_range_dead_at (scan, entry + span, bit)) continue;
    *chosen = reg;
    return TRUE;
  }
  return FALSE;
}

gboolean
gum_arm64_relocator_can_relocate (gpointer address, guint min_bytes,
    GumRelocationScenario scenario, GumRelocationPolicy policy,
    guint * maximum, arm64_reg * available_scratch_reg)
{
  GumArm64Writer writer;
  GumArm64Relocator relocator;
  PeakArm64RangeScan scan = { 0 };
  guint8 writer_bytes[48];
  guint8 * prefix = NULL;
  gsize prefix_size = 0;
  guint n = 0, requested_bytes, index, i, candidate = 0;
  uintptr_t entry = (uintptr_t) address;
  arm64_reg requested = available_scratch_reg != NULL ?
      *available_scratch_reg : ARM64_REG_INVALID;
  arm64_reg chosen = ARM64_REG_INVALID;
  gboolean initialized = FALSE;

  if (scenario != GUM_SCENARIO_ONLINE || policy != GUM_RELOCATION_CHECKED)
    return peak_arm64_legacy_can_relocate (address, min_bytes, scenario, policy,
        maximum, available_scratch_reg);
  if (maximum != NULL) *maximum = 0;
  if (available_scratch_reg != NULL)
    *available_scratch_reg = ARM64_REG_INVALID;
  if ((entry & 3) != 0 || min_bytes > 400 || entry > UINTPTR_MAX - 400)
    return FALSE;
  requested_bytes = MAX (4, (min_bytes + 3) & ~3u);
  prefix = gum_memory_read (address, requested_bytes, &prefix_size);
  if (prefix == NULL || prefix_size != requested_bytes) goto done;
  gum_arm64_writer_init (&writer, writer_bytes);
  gum_arm64_relocator_init (&relocator, prefix, &writer);
  relocator.input_pc = entry;
  initialized = TRUE;
  do {
    const cs_insn * insn;
    guint bytes = gum_arm64_relocator_read_one (&relocator, &insn);
    if (bytes == 0) break;
    n = bytes;
    if (insn->id == ARM64_INS_BL || insn->id == ARM64_INS_BLR ||
        insn->id == ARM64_INS_SVC) break;
  } while (n < min_bytes);
  if (n < 4) goto done;
  scan.nodes = g_new0 (PeakArm64RangeNode, PEAK_ARM64_RANGE_BUDGET);
  scan.indices = g_hash_table_new (g_direct_hash, g_direct_equal);
  if (scan.nodes == NULL || scan.indices == NULL ||
      cs_open (CS_ARCH_ARM64, GUM_DEFAULT_CS_ENDIAN, &scan.capstone) != CS_ERR_OK)
    goto done;
  if (cs_option (scan.capstone, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    goto done;
  scan.instruction = cs_malloc (scan.capstone);
  if (scan.instruction == NULL || !peak_arm64_range_add (&scan, entry, &index))
    goto done;
  /* Starting at the entry includes prefix branches even when read_one ended
   * at EOI. Every queued path must finish: partial scans never grant a range. */
  for (index = 0; index != scan.count; index++)
    if (!peak_arm64_range_decode (&scan, index)) goto done;
  /* Even a 4-byte patch requires a complete walk: a later literal load may
   * read the entry bytes although it cannot branch into their interior. */
  for (i = 16; i >= 4; i /= 2) {
    if (i <= n && peak_arm64_range_candidate (&scan, entry, i,
        requested, &chosen)) {
      candidate = i;
      break;
    }
  }
done:
  if (initialized) {
    gum_arm64_relocator_clear (&relocator);
    gum_arm64_writer_clear (&writer);
  }
  g_free (prefix);
  g_free (scan.cache);
  g_free (scan.nodes);
  if (scan.indices != NULL) g_hash_table_unref (scan.indices);
  if (scan.instruction != NULL) cs_free (scan.instruction, 1);
  if (scan.capstone != 0) cs_close (&scan.capstone);
  if (maximum != NULL) *maximum = candidate;
  if (available_scratch_reg != NULL) *available_scratch_reg = chosen;
  return candidate >= min_bytes;
}

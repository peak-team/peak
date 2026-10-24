/* Helpers for the small, hash-pinned can_relocate source insertion. Original
 * addresses stay in Gum's worklists; readable copies are only decoder input. */
typedef struct {
  const guint8 * start;
  guint8 * bytes;
  gsize size;
} PeakArm64CodeRead;

static guint
peak_arm64_readable_prefix (gpointer address, guint minimum)
{
  gsize size = 0;
  guint requested = MAX (minimum, 4);
  guint8 * bytes;
  if (requested > G_MAXUINT - 3) return 0;
  requested = (requested + 3) & ~3u;
  bytes = gum_memory_read (address, requested, &size);
  if (bytes == NULL) return 0;
  g_free (bytes);
  return size & ~((gsize) 3);
}

static void
peak_arm64_begin_code_read (PeakArm64CodeRead * read, const guint8 * address,
                            gsize remaining)
{
  g_free (read->bytes);
  read->start = address;
  read->size = 0;
  read->bytes = remaining >= 4 ? gum_memory_read (address, remaining,
      &read->size) : NULL;
}

static gboolean
peak_arm64_read_instruction (PeakArm64CodeRead * read, csh capstone,
                             const guint8 ** code, size_t * remaining,
                             uint64_t * address, cs_insn * insn)
{
  const guint8 * cursor;
  size_t available;
  gsize offset;
  if (GPOINTER_TO_SIZE (*code) < GPOINTER_TO_SIZE (read->start)) return FALSE;
  offset = GPOINTER_TO_SIZE (*code) - GPOINTER_TO_SIZE (read->start);
  if (*remaining < 4 || read->bytes == NULL || offset > read->size ||
      read->size - offset < 4) return FALSE;
  cursor = read->bytes + offset;
  available = 4;
  if (!cs_disasm_iter (capstone, &cursor, &available, address, insn) ||
      insn->size != 4) return FALSE;
  *code += 4;
  *remaining -= 4;
  return TRUE;
}

static gpointer
peak_arm64_prefix_branch_target (const cs_insn * insn)
{
  const cs_arm64 * detail = &insn->detail->arm64;
  guint operand;
  switch (insn->id) {
    case ARM64_INS_B: operand = 0; break;
    case ARM64_INS_CBZ: case ARM64_INS_CBNZ: operand = 1; break;
    case ARM64_INS_TBZ: case ARM64_INS_TBNZ: operand = 2; break;
    default: return NULL;
  }
  if (detail->op_count <= operand ||
      detail->operands[operand].type != ARM64_OP_IMM) return NULL;
  return GSIZE_TO_POINTER (detail->operands[operand].imm);
}

static gboolean
peak_arm64_prefix_has_roots (const GumArm64Relocator * relocator, guint n)
{
  guint i;
  for (i = 0; i != MIN (n, 16) / 4; i++)
    if (peak_arm64_prefix_branch_target (relocator->input_insns[i]) != NULL)
      return TRUE;
  return FALSE;
}

static void
peak_arm64_queue_prefix_roots (const GumArm64Relocator * relocator, guint n,
                               GHashTable * checked, GHashTable * queued)
{
  guint i;
  for (i = 0; i != MIN (n, 16) / 4; i++) {
    gpointer target = peak_arm64_prefix_branch_target (relocator->input_insns[i]);
    if (target != NULL && !g_hash_table_contains (checked, target))
      g_hash_table_add (queued, target);
  }
}

/* Check only the complete instruction prefix actually displaced by Gum.
 * The caller must guarantee that unrelocated code does not enter its interior
 * or access overwritten bytes as data. This cannot prove suffix properties. */
typedef struct {
  guintptr pc, target, data_target;
  guint size, data_size;
  gboolean direct, conditional, call, rip_data, unsupported;
} PeakX86PrefixInsn;

/* Capstone reports pointer-width operands for these state-image accesses,
 * not their actual (possibly feature-dependent) memory extent. */
static gboolean
peak_x86_range_state_image (guint id)
{
  switch (id)
  {
    case X86_INS_FLDENV:
    case X86_INS_FRSTOR:
    case X86_INS_FNSAVE:
    case X86_INS_FNSTENV:
    case X86_INS_FXRSTOR:
    case X86_INS_FXRSTOR64:
    case X86_INS_FXSAVE:
    case X86_INS_FXSAVE64:
    case X86_INS_XRSTOR:
    case X86_INS_XRSTOR64:
    case X86_INS_XRSTORS:
    case X86_INS_XRSTORS64:
    case X86_INS_XSAVE:
    case X86_INS_XSAVE64:
    case X86_INS_XSAVEC:
    case X86_INS_XSAVEC64:
    case X86_INS_XSAVEOPT:
    case X86_INS_XSAVEOPT64:
    case X86_INS_XSAVES:
    case X86_INS_XSAVES64:
      return TRUE;
    default: return FALSE;
  }
}

gboolean
gum_x86_relocator_can_relocate (gpointer address, guint min_bytes,
                               GumRelocationScenario scenario, guint * maximum)
{
  guintptr entry = GPOINTER_TO_SIZE (address), limit;
  guint span = 0, count = 0, i, safe = 0;
  gsize available = 0, remaining;
  guint8 * copy;
  const guint8 * code;
  guint64 pc = entry;
  csh capstone = 0;
  cs_insn * insn = NULL;
  PeakX86PrefixInsn * nodes;
  gboolean changed, decoded = TRUE;

  if (scenario != GUM_SCENARIO_ONLINE)
    return peak_x86_legacy_can_relocate (address, min_bytes, scenario, maximum);
  if (maximum != NULL) *maximum = 0;
  /* A last instruction can extend the requested prefix by at most 14 bytes.
   * A partial read is sufficient when it contains every required instruction. */
  if (min_bytes == 0 || min_bytes > G_MAXUINT - 14 ||
      entry > G_MAXSIZE - (min_bytes + 14)) return FALSE;
  copy = gum_memory_read (address, min_bytes + 14, &available);
  if (copy == NULL) return FALSE;
  nodes = g_new0 (PeakX86PrefixInsn, min_bytes);
  code = copy;
  remaining = available;
  cs_arch_register_x86 ();
  if (cs_open (CS_ARCH_X86, sizeof (gpointer) == 8 ? CS_MODE_64 : CS_MODE_32,
      &capstone) != CS_ERR_OK) goto beach;
  if (cs_option (capstone, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) goto beach;
  insn = cs_malloc (capstone);
  if (insn == NULL) goto beach;

  while (span < min_bytes)
  {
    PeakX86PrefixInsn * node = &nodes[count];
    cs_x86 * x;
    gboolean jmp, terminal;
    if (!cs_disasm_iter (capstone, &code, &remaining, &pc, insn))
    { decoded = FALSE; break; }
    x = &insn->detail->x86;
    node->pc = insn->address;
    node->size = insn->size;
    node->call = insn->id == X86_INS_CALL;
    jmp = insn->id == X86_INS_JMP;
    node->conditional = gum_x86_reader_insn_is_jcc (insn) ||
        insn->id == X86_INS_JECXZ || insn->id == X86_INS_JRCXZ;
    node->unsupported = insn->id == X86_INS_JCXZ ||
        insn->id == X86_INS_LOOP || insn->id == X86_INS_LOOPE ||
        insn->id == X86_INS_LOOPNE ||
        (cs_insn_group (capstone, insn, CS_GRP_BRANCH_RELATIVE) &&
         !node->conditional && !node->call && !jmp);
    if (node->call || jmp || node->conditional)
    {
      node->direct = x->op_count != 0 && x->operands[0].type == X86_OP_IMM;
      if (node->direct) node->target = (guintptr) x->operands[0].imm;
    }
    for (i = 0; i < x->op_count; i++)
    {
      cs_x86_op * op = &x->operands[i];
      if (insn->id == X86_INS_NOP || op->type != X86_OP_MEM ||
          op->mem.base != X86_REG_RIP) continue;
      node->data_target = node->pc + node->size + op->mem.disp;
      node->data_size = insn->id == X86_INS_LEA ? 1 :
          (peak_x86_range_state_image (insn->id) ? 0 : op->size);
      node->rip_data = insn->id != X86_INS_LEA || node->data_target != entry;
      if (node->data_target > G_MAXSIZE - node->data_size)
        node->unsupported = TRUE;
    }
    count++;
    span += node->size;
    terminal = jmp || insn->id == X86_INS_RET || insn->id == X86_INS_RETF ||
        insn->id == X86_INS_RETFQ;
    /* Preserve the pinned ONLINE rule: include this instruction, then stop. */
    if (terminal || node->call || insn->id == X86_INS_SYSCALL ||
        insn->id == X86_INS_SYSENTER || insn->id == X86_INS_INT) break;
  }

  limit = entry + span;
  do
  {
    guintptr rounded = entry;
    changed = FALSE;
    for (i = 0; i < count; i++)
      if (nodes[i].pc + nodes[i].size <= limit)
        rounded = nodes[i].pc + nodes[i].size;
    if (rounded != limit) { limit = rounded; changed = TRUE; }
    for (i = 0; i < count; i++)
    {
      PeakX86PrefixInsn * node = &nodes[i];
      guintptr next = limit;
      if (node->pc >= limit) break;
      if (node->unsupported) next = node->pc;
      /* Zero denotes unknown extent. A forward memory access starting at or
       * after the patch end cannot overlap it, regardless of its width. */
      if (node->rip_data && node->data_target < limit &&
          (node->data_size == 0 || node->data_target + node->data_size > entry))
        next = MIN (next, MAX (entry, node->data_target));
      if (node->conditional)
      {
        guint j;
        gboolean boundary = FALSE;
        for (j = 0; j < count; j++)
          if (nodes[j].pc == node->target && nodes[j].pc < limit)
            boundary = TRUE;
        /* Internal Jcc targets require labels on instruction boundaries.
         * External targets use the pinned writer's ordinary branch rewrite. */
        if (!node->direct ||
            (node->target >= entry && node->target < limit && !boundary))
          next = MIN (next, node->pc);
      }
      else if (node->direct && node->target > entry && node->target < limit)
        /* JMP/CALL writers retain their original destination addresses. */
        next = MIN (next, node->target);
      if (next < limit) { limit = next; changed = TRUE; }
    }
  }
  while (changed);
  safe = limit - entry;
beach:
  if (insn != NULL) cs_free (insn, 1);
  if (capstone != 0) cs_close (&capstone);
  g_free (nodes);
  g_free (copy);
  if (maximum != NULL) *maximum = safe;
  return decoded && safe >= min_bytes;
}

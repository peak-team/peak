/* Supplement pinned Gum's linear prefix classifier with a completed reachable
 * instruction graph. This does not prove absence of arbitrary external entries:
 * calls return conventionally; RET and indirect tail JMP terminate a path.
 * An indirect tail instruction may not be overwritten except the canonical
 * six-byte FF25 entry dispatch thunk; its future slot values must be valid
 * function entries under this same ABI contract. */
#define PEAK_X86_RANGE_BUDGET 4096

typedef struct {
  guintptr pc;
  guint size;
  guint id;
  guintptr target;
  gboolean direct;
  gboolean conditional;
  gboolean call;
  gboolean unsupported_relative;
  gboolean rip_data;
  guintptr data_target;
  guint data_size;
  gboolean unknown;
  gboolean indirect_tail;
} PeakX86RangeNode;

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

static gboolean
peak_x86_range_enqueue (guintptr pc, PeakX86RangeNode * nodes, guint * count,
                       GHashTable * starts)
{
  if (g_hash_table_contains (starts, GSIZE_TO_POINTER (pc)))
    return TRUE;
  if (*count == PEAK_X86_RANGE_BUDGET)
    return FALSE;
  nodes[*count].pc = pc;
  g_hash_table_insert (starts, GSIZE_TO_POINTER (pc),
      GUINT_TO_POINTER (*count + 1));
  (*count)++;
  return TRUE;
}

gboolean
gum_x86_relocator_can_relocate (gpointer address, guint min_bytes,
                               GumRelocationScenario scenario, guint * maximum)
{
  guint legacy_span = 0, count = 0, cursor, i, safe = 0;
  gboolean changed;
  guintptr entry = GPOINTER_TO_SIZE (address), limit;
  gboolean legacy, complete = FALSE;
  PeakX86RangeNode * nodes;
  GHashTable * starts, * bytes;
  csh capstone = 0;
  cs_insn * insn = NULL;

  if (scenario != GUM_SCENARIO_ONLINE)
    return peak_x86_legacy_can_relocate (address, min_bytes, scenario, maximum);
  if (maximum != NULL) *maximum = 0;
  if (min_bytes == 0 || min_bytes > 64 || entry > G_MAXSIZE - (min_bytes + 15))
    return FALSE;

  /* Legacy decoding may read across a mapping boundary. Validate the candidate
   * bytes first, then retain its instruction / online-call stopping rules. */
  {
    gsize available = 0;
    guint8 * prefix = gum_memory_read (address, min_bytes + 15, &available);
    g_free (prefix);
    if (available < min_bytes + 15) return FALSE;
  }
  legacy = peak_x86_legacy_can_relocate (address, min_bytes, scenario,
      &legacy_span);
  if (legacy_span == 0) return FALSE;
  limit = entry + legacy_span;
  nodes = g_new0 (PeakX86RangeNode, PEAK_X86_RANGE_BUDGET);
  starts = g_hash_table_new (g_direct_hash, g_direct_equal);
  bytes = g_hash_table_new (g_direct_hash, g_direct_equal);
  cs_arch_register_x86 ();
  if (cs_open (CS_ARCH_X86, sizeof (gpointer) == 8 ? CS_MODE_64 : CS_MODE_32,
      &capstone) != CS_ERR_OK) goto beach;
  if (cs_option (capstone, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) goto beach;
  insn = cs_malloc (capstone);
  if (insn == NULL || !peak_x86_range_enqueue (entry, nodes, &count, starts))
    goto beach;

  for (cursor = 0; cursor < count; cursor++)
  {
    PeakX86RangeNode * node = &nodes[cursor];
    gsize available = 0, remaining;
    guint8 * copy;
    /* Registered replacement entries are ABI tail-call boundaries. The exact
     * metadata predicate never turns an arbitrary unreadable address safe. */
    if (gum_interceptor_peak_relocation_is_opaque_entry (
        GSIZE_TO_POINTER (node->pc))) continue;
    copy = gum_interceptor_peak_read_relocation_code (
        GSIZE_TO_POINTER (node->pc), 15, &available);
    const guint8 * code = copy;
    guint64 pc = node->pc;
    gboolean terminal = FALSE;
    if (copy == NULL)
    {
      guint window;
      for (window = 14; window != 0 && copy == NULL; window--)
        copy = gum_interceptor_peak_read_relocation_code (
            GSIZE_TO_POINTER (node->pc), window, &available);
      code = copy;
    }
    remaining = available;
    if (copy == NULL || !cs_disasm_iter (capstone, &code, &remaining, &pc, insn))
    {
      g_free (copy);
      node->unknown = TRUE;
      continue;
    }
    g_free (copy);
    node->size = insn->size;
    node->id = insn->id;
    if (node->pc > G_MAXSIZE - node->size) goto beach;
    for (i = 0; i < node->size; i++)
    {
      gpointer key = GSIZE_TO_POINTER (node->pc + i);
      if (g_hash_table_contains (bytes, key)) goto beach; /* overlapping decode */
      g_hash_table_insert (bytes, key, GUINT_TO_POINTER (cursor + 1));
    }
    for (i = 0; i < insn->detail->x86.op_count; i++)
    {
      cs_x86_op * op = &insn->detail->x86.operands[i];
      if (op->type == X86_OP_MEM && op->mem.base == X86_REG_RIP)
      {
        node->rip_data = TRUE;
        if (insn->id != X86_INS_LEA &&
            (op->size == 0 || peak_x86_range_state_image (insn->id)))
          node->unknown = TRUE;
        node->data_target = node->pc + node->size + op->mem.disp;
        /* LEA exposes an address, not an operand-sized memory access. */
        node->data_size = insn->id == X86_INS_LEA ? 1 : MAX (op->size, 1);
        /* Returning the exact public function entry preserves pointer identity
         * across attach. It materializes an address without reading modified entry bytes. */
        if (insn->id == X86_INS_LEA && node->data_target == entry)
          node->rip_data = FALSE;
        if (node->data_target > G_MAXSIZE - node->data_size) goto beach;
      }
    }
    node->unsupported_relative = insn->id == X86_INS_JCXZ ||
        insn->id == X86_INS_LOOP || insn->id == X86_INS_LOOPE ||
        insn->id == X86_INS_LOOPNE;
    node->call = insn->id == X86_INS_CALL;
    node->conditional = gum_x86_reader_insn_is_jcc (insn) ||
        insn->id == X86_INS_JECXZ || insn->id == X86_INS_JRCXZ ||
        insn->id == X86_INS_JCXZ || insn->id == X86_INS_LOOP ||
        insn->id == X86_INS_LOOPE || insn->id == X86_INS_LOOPNE;
    if (node->conditional || insn->id == X86_INS_JMP || node->call)
    {
      cs_x86 * x = &insn->detail->x86;
      node->direct = x->op_count != 0 && x->operands[0].type == X86_OP_IMM;
      if (node->direct)
      {
        node->target = (guintptr) x->operands[0].imm;
        if (!gum_interceptor_peak_relocation_branch_available (
            GSIZE_TO_POINTER (node->pc), node->size,
            GSIZE_TO_POINTER (node->target),
            (gum_x86_reader_insn_is_jcc (insn) ||
             insn->id == X86_INS_JECXZ || insn->id == X86_INS_JRCXZ) &&
            !node->unsupported_relative))
          node->unknown = TRUE;
      }
      if (!node->call)
      {
        if (!node->direct)
        {
          node->indirect_tail = TRUE;
          terminal = TRUE;
          /* Canonical function-entry dispatch thunk. The mutable slot is read
           * only to reject a currently invalid destination; future targets
           * must remain valid ABI entries, not arbitrary internal blocks. */
          if (node->pc == entry && legacy_span == 6 && node->size == 6 &&
              insn->bytes[0] == 0xff && insn->bytes[1] == 0x25 &&
              x->op_count == 1 && x->operands[0].type == X86_OP_MEM &&
              x->operands[0].size == 8 &&
              x->operands[0].mem.base == X86_REG_RIP &&
              x->operands[0].mem.index == X86_REG_INVALID &&
              x->operands[0].mem.segment == X86_REG_INVALID)
          {
            guint64 target;
            gsize n = 0;
            guint8 * value;
            if ((node->data_target < entry + 6 &&
                 node->data_target + 8 > entry) ||
                !gum_interceptor_peak_relocation_range_available (
                    GSIZE_TO_POINTER (node->data_target), 8)) goto beach;
            value = gum_memory_read (GSIZE_TO_POINTER (node->data_target), 8, &n);
            if (value == NULL || n != 8) { g_free (value); goto beach; }
            memcpy (&target, value, 8);
            g_free (value);
            if ((target > entry && target < entry + 6) ||
                !gum_interceptor_peak_relocation_branch_available (
                    GSIZE_TO_POINTER (entry), 6, GSIZE_TO_POINTER (target), FALSE))
              goto beach;
            node->indirect_tail = FALSE;
          }
        }
        else
        {
          if (!peak_x86_range_enqueue (node->target, nodes, &count, starts)) goto beach;
          terminal = !node->conditional;
        }
      }
      /* Calls are opaque conventional returning edges, not tail branches.
       * Their fallthrough remains reachable, including call-next idioms. */
    }
    else if (insn->id == X86_INS_RET || insn->id == X86_INS_RETF ||
             insn->id == X86_INS_RETFQ || insn->id == X86_INS_UD2)
      /* RET and UD2 end normal control flow; arbitrary signal-handler PC
       * changes are outside this caller-CFG contract. UD2 cannot be displaced. */
      terminal = TRUE;
#if defined(__linux__) && defined(__x86_64__)
    else if (insn->id == X86_INS_SYSCALL)
    {
      /* Linux AMD64 SYSCALL returns to its following instruction under the
       * normal kernel ABI. Keep legacy ONLINE prefix stopping unchanged. */
    }
#endif
    else if (cs_insn_group (capstone, insn, CS_GRP_JUMP) ||
             cs_insn_group (capstone, insn, CS_GRP_CALL) ||
             cs_insn_group (capstone, insn, CS_GRP_BRANCH_RELATIVE) ||
             cs_insn_group (capstone, insn, CS_GRP_RET) ||
             cs_insn_group (capstone, insn, CS_GRP_IRET) ||
             cs_insn_group (capstone, insn, CS_GRP_INT) ||
             insn->id == X86_INS_SYSCALL || insn->id == X86_INS_SYSENTER ||
             insn->id == X86_INS_SYSEXIT || insn->id == X86_INS_SYSRET ||
             insn->id == X86_INS_HLT || insn->id == X86_INS_UD2)
      { node->unknown = TRUE; terminal = TRUE; }
    if (!terminal && !peak_x86_range_enqueue (node->pc + node->size,
        nodes, &count, starts)) goto beach;
  }

  for (i = 0; i < count; i++)
  {
    PeakX86RangeNode * node = &nodes[i];
    if (node->unknown) goto beach;
    if (node->direct && node->call)
    {
      guint owner = GPOINTER_TO_UINT (g_hash_table_lookup (bytes,
          GSIZE_TO_POINTER (node->target)));
      if (owner != 0 && nodes[owner - 1].pc != node->target) goto beach;
    }
  }

  /* Evaluate after graph completion: instruction-boundary overlap has now been
   * excluded even for alternate blocks discovered after a branch. */
  do
  {
    guintptr rounded = entry;
    changed = FALSE;
    for (i = 0; i < count; i++)
    {
      PeakX86RangeNode * node = &nodes[i];
      if (node->pc >= entry && node->pc < limit &&
          node->pc + node->size <= limit &&
          gum_interceptor_peak_relocation_range_available (address,
              node->pc + node->size - entry))
        rounded = MAX (rounded, node->pc + node->size);
    }
    if (rounded < limit) { limit = rounded; changed = TRUE; }
    for (i = 0; i < count; i++)
    {
      PeakX86RangeNode * node = &nodes[i];
      if ((node->unsupported_relative || node->id == X86_INS_UD2 || node->indirect_tail) &&
          node->pc >= entry && node->pc < limit)
      {
        limit = node->pc;
        changed = TRUE;
      }
      if (node->rip_data && node->data_target < limit &&
          node->data_target + node->data_size > entry)
      {
        guintptr next_limit = MAX (entry, node->data_target);
        if (next_limit < limit) { limit = next_limit; changed = TRUE; }
      }
      if (!node->direct || node->target <= entry ||
          node->target >= limit)
        continue;
      if (node->call || !node->conditional || node->pc < entry || node->pc >= limit)
      {
        limit = node->target;
        changed = TRUE;
      }
      /* Internal Jcc edges are rewritten to relocated instruction labels.
       * JMP/CALL writers retain original target addresses. */
    }
  }
  while (changed);
  for (i = 0; i < count; i++)
  {
    PeakX86RangeNode * node = &nodes[i];
    if (node->pc >= entry && node->pc < limit &&
        node->pc + node->size <= limit &&
        node->pc + node->size - entry > safe &&
        gum_interceptor_peak_relocation_range_available (address,
            node->pc + node->size - entry))
      safe = node->pc + node->size - entry;
  }
  complete = TRUE;
beach:
  if (insn != NULL) cs_free (insn, 1);
  if (capstone != 0) cs_close (&capstone);
  g_hash_table_unref (bytes);
  g_hash_table_unref (starts);
  g_free (nodes);
  if (!complete) return FALSE;
  if (maximum != NULL) *maximum = safe;
  return legacy && legacy_span <= safe;
}

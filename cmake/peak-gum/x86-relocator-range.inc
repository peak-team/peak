/* Supplement pinned Gum's linear prefix classifier with a completed reachable
 * instruction graph. This does not prove absence of arbitrary external entries:
 * calls are assumed to return conventionally and RET terminates a path. */
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
  gboolean fallthrough;
  gboolean reachable;
  guint predecessors;
  guint predecessor;
  gint folded;
  guint writes;
  guint move_register;
  guint move_kind;
  guint64 immediate;
  gboolean memory_rip;
  guint memory_register;
  gint64 displacement;
  guint test_register;
  gboolean register_metadata;
  gboolean overlapping_call_entry;
} PeakX86RangeNode;

/* Full-width values only; every partial GPR alias write kills its value. */
static guint
peak_x86_range_register (x86_reg reg)
{
  switch (reg)
  {
    case X86_REG_RAX: case X86_REG_EAX: case X86_REG_AX: case X86_REG_AL: case X86_REG_AH: return 1u << 0;
    case X86_REG_RCX: case X86_REG_ECX: case X86_REG_CX: case X86_REG_CL: case X86_REG_CH: return 1u << 1;
    case X86_REG_RDX: case X86_REG_EDX: case X86_REG_DX: case X86_REG_DL: case X86_REG_DH: return 1u << 2;
    case X86_REG_RBX: case X86_REG_EBX: case X86_REG_BX: case X86_REG_BL: case X86_REG_BH: return 1u << 3;
    case X86_REG_RSP: case X86_REG_ESP: case X86_REG_SP: case X86_REG_SPL: return 1u << 4;
    case X86_REG_RBP: case X86_REG_EBP: case X86_REG_BP: case X86_REG_BPL: return 1u << 5;
    case X86_REG_RSI: case X86_REG_ESI: case X86_REG_SI: case X86_REG_SIL: return 1u << 6;
    case X86_REG_RDI: case X86_REG_EDI: case X86_REG_DI: case X86_REG_DIL: return 1u << 7;
    case X86_REG_R8: case X86_REG_R8D: case X86_REG_R8W: case X86_REG_R8B: return 1u << 8;
    case X86_REG_R9: case X86_REG_R9D: case X86_REG_R9W: case X86_REG_R9B: return 1u << 9;
    case X86_REG_R10: case X86_REG_R10D: case X86_REG_R10W: case X86_REG_R10B: return 1u << 10;
    case X86_REG_R11: case X86_REG_R11D: case X86_REG_R11W: case X86_REG_R11B: return 1u << 11;
    case X86_REG_R12: case X86_REG_R12D: case X86_REG_R12W: case X86_REG_R12B: return 1u << 12;
    case X86_REG_R13: case X86_REG_R13D: case X86_REG_R13W: case X86_REG_R13B: return 1u << 13;
    case X86_REG_R14: case X86_REG_R14D: case X86_REG_R14W: case X86_REG_R14B: return 1u << 14;
    case X86_REG_R15: case X86_REG_R15D: case X86_REG_R15W: case X86_REG_R15B: return 1u << 15;
    default: return 0;
  }
}

static guint
peak_x86_range_full_register (x86_reg reg)
{
  switch (reg)
  {
    case X86_REG_RAX: return 1u << 0;
    case X86_REG_RCX: return 1u << 1;
    case X86_REG_RDX: return 1u << 2;
    case X86_REG_RBX: return 1u << 3;
    case X86_REG_RSP: return 1u << 4;
    case X86_REG_RBP: return 1u << 5;
    case X86_REG_RSI: return 1u << 6;
    case X86_REG_RDI: return 1u << 7;
    case X86_REG_R8: return 1u << 8;
    case X86_REG_R9: return 1u << 9;
    case X86_REG_R10: return 1u << 10;
    case X86_REG_R11: return 1u << 11;
    case X86_REG_R12: return 1u << 12;
    case X86_REG_R13: return 1u << 13;
    case X86_REG_R14: return 1u << 14;
    case X86_REG_R15: return 1u << 15;
    default: return 0;
  }
}

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

#if defined(__linux__)
# include <errno.h>
# include <fcntl.h>
# include <sys/stat.h>
# include <sys/sysmacros.h>
# include <unistd.h>
#endif
#define PEAK_X86_MAPS_MAX (1024u * 1024u)
#define PEAK_X86_MAP_PATH_MAX 4096u

typedef struct {
  guintptr start, end;
  guint64 offset, inode;
  guint device_major, device_minor;
  gchar permissions[4];
  gchar * path;
  gboolean checked, image;
} PeakX86RangeMapping;
typedef struct { GArray * entries; } PeakX86RangeMaps;

static void
peak_x86_range_maps_free (PeakX86RangeMaps * maps)
{
  guint i;
  if (maps == NULL) return;
  for (i = 0; i < maps->entries->len; i++)
    g_free (g_array_index (maps->entries, PeakX86RangeMapping, i).path);
  g_array_free (maps->entries, TRUE);
  g_free (maps);
}

static gboolean
peak_x86_range_maps_number (const gchar ** cursor, guint base, guint64 * value)
{
  const gchar * p = *cursor; guint64 n = 0; guint digits = 0;
  for (;; p++)
  {
    gint digit = g_ascii_xdigit_value (*p);
    if (digit < 0 || (guint) digit >= base) break;
    if (n > (G_MAXUINT64 - digit) / base) return FALSE;
    n = n * base + digit; digits++;
  }
  if (digits == 0) return FALSE;
  *cursor = p; *value = n; return TRUE;
}

static void
peak_x86_range_maps_spaces (const gchar ** cursor)
{
  while (**cursor == ' ' || **cursor == '\t') (*cursor)++;
}

static gboolean
peak_x86_range_maps_line (const gchar * line, PeakX86RangeMapping * mapping)
{
  const gchar * p = line; guint64 start, end, major_value, minor_value;
  guint i, length = 0; gchar path[PEAK_X86_MAP_PATH_MAX];
  if (!peak_x86_range_maps_number (&p, 16, &start) || *p++ != '-' ||
      !peak_x86_range_maps_number (&p, 16, &end) || *p++ != ' ' ||
      start >= end || end > G_MAXSIZE) return FALSE;
  mapping->start = start; mapping->end = end;
  peak_x86_range_maps_spaces (&p);
  if (strlen (p) < 5 || (p[0] != 'r' && p[0] != '-') ||
      (p[1] != 'w' && p[1] != '-') || (p[2] != 'x' && p[2] != '-') ||
      (p[3] != 'p' && p[3] != 's') || p[4] != ' ') return FALSE;
  memcpy (mapping->permissions, p, 4); p += 5;
  peak_x86_range_maps_spaces (&p);
  if (!peak_x86_range_maps_number (&p, 16, &mapping->offset) || *p++ != ' ')
    return FALSE;
  peak_x86_range_maps_spaces (&p);
  if (!peak_x86_range_maps_number (&p, 16, &major_value) || *p++ != ':' ||
      !peak_x86_range_maps_number (&p, 16, &minor_value) || *p++ != ' ' ||
      major_value > G_MAXUINT || minor_value > G_MAXUINT) return FALSE;
  mapping->device_major = major_value; mapping->device_minor = minor_value;
  peak_x86_range_maps_spaces (&p);
  if (!peak_x86_range_maps_number (&p, 10, &mapping->inode) ||
      (*p != '\0' && *p != ' ')) return FALSE;
  peak_x86_range_maps_spaces (&p);
  while (*p != '\0')
  {
    guint byte = (guchar) *p++;
    if (byte == '\\')
    {
      byte = 0;
      for (i = 0; i != 3; i++)
      {
        if (p[i] < '0' || p[i] > '7') return FALSE;
        byte = byte * 8 + p[i] - '0';
      }
      p += 3;
      if (byte == 0 || byte > 255) return FALSE;
    }
    if (length == sizeof (path) - 1) return FALSE;
    path[length++] = byte;
  }
  path[length] = '\0';
  mapping->path = g_strdup (path);
  return TRUE;
}

static PeakX86RangeMaps *
peak_x86_range_maps_snapshot (void)
{
#if defined(__linux__)
  gint fd; gchar * buffer; gsize used = 0; gboolean valid = TRUE;
  PeakX86RangeMaps * maps; gchar * line, * end;
  fd = open ("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd == -1) return NULL;
  buffer = g_malloc (PEAK_X86_MAPS_MAX + 2);
  for (;;)
  {
    ssize_t n = read (fd, buffer + used, PEAK_X86_MAPS_MAX + 1 - used);
    if (n == -1 && errno == EINTR) continue;
    if (n == -1) { valid = FALSE; break; }
    if (n == 0) break;
    used += n;
    if (used > PEAK_X86_MAPS_MAX) { valid = FALSE; break; }
  }
  if (close (fd) != 0) valid = FALSE;
  if (!valid || used == 0 || buffer[used - 1] != '\n' ||
      memchr (buffer, '\0', used) != NULL) { g_free (buffer); return NULL; }
  buffer[used] = '\0';
  maps = g_new0 (PeakX86RangeMaps, 1);
  maps->entries = g_array_new (FALSE, FALSE, sizeof (PeakX86RangeMapping));
  line = buffer;
  while ((end = strchr (line, '\n')) != NULL)
  {
    PeakX86RangeMapping mapping = { 0 };
    *end = '\0';
    if (*line == '\0' || !peak_x86_range_maps_line (line, &mapping) ||
        (maps->entries->len != 0 && mapping.start <
         g_array_index (maps->entries, PeakX86RangeMapping,
             maps->entries->len - 1).end))
    {
      g_free (mapping.path); peak_x86_range_maps_free (maps);
      g_free (buffer); return NULL;
    }
    g_array_append_val (maps->entries, mapping);
    line = end + 1;
  }
  g_free (buffer);
  return maps;
#else
  return NULL;
#endif
}

static gboolean
peak_x86_range_same_image (const PeakX86RangeMapping * a,
                            const PeakX86RangeMapping * b)
{
  return a->inode == b->inode && a->device_major == b->device_major &&
      a->device_minor == b->device_minor;
}

static gboolean
peak_x86_range_mapping_image (PeakX86RangeMaps * maps,
                              PeakX86RangeMapping * candidate)
{
#if defined(__linux__)
  struct stat metadata; guint i; gboolean executable = FALSE, header = FALSE;
  if (candidate->checked) return candidate->image;
  candidate->checked = TRUE;
  if (candidate->inode == 0 || candidate->path[0] != '/' ||
      stat (candidate->path, &metadata) != 0 || !S_ISREG (metadata.st_mode) ||
      (guint64) metadata.st_ino != candidate->inode ||
      major (metadata.st_dev) != candidate->device_major ||
      minor (metadata.st_dev) != candidate->device_minor) return FALSE;
  for (i = 0; i < maps->entries->len; i++)
  {
    PeakX86RangeMapping * m = &g_array_index (maps->entries, PeakX86RangeMapping, i);
    if (!peak_x86_range_same_image (candidate, m) || m->permissions[3] != 'p')
      continue;
    if (m->permissions[2] == 'x' && m->permissions[1] != 'w') executable = TRUE;
    if (m->offset == 0 && m->permissions[0] == 'r' &&
        m->permissions[1] != 'w' && m->end - m->start >= 64)
    {
      gsize n = 0; guint8 * bytes = gum_memory_read (GSIZE_TO_POINTER (m->start),64,&n);
      if (bytes != NULL && n == 64 && memcmp (bytes,"\177ELF",4) == 0 &&
          bytes[4] == 2 && bytes[5] == 1 && bytes[6] == 1 &&
          (bytes[16] == 2 || bytes[16] == 3) && bytes[17] == 0 &&
          bytes[18] == 62 && bytes[19] == 0) header = TRUE;
      g_free (bytes);
    }
  }
  candidate->image = executable && header;
  return candidate->image;
#else
  (void) maps; (void) candidate; return FALSE;
#endif
}

static gboolean
peak_x86_range_constant_source (PeakX86RangeMaps * maps, guintptr address)
{
  guint i;
  if (maps == NULL || address > G_MAXSIZE - 8) return FALSE;
  for (i = 0; i < maps->entries->len; i++)
  {
    PeakX86RangeMapping * m = &g_array_index (maps->entries, PeakX86RangeMapping, i);
    if (address >= m->start && address + 8 <= m->end)
      return memcmp (m->permissions,"r--p",4) == 0 &&
          peak_x86_range_mapping_image (maps,m);
  }
  return FALSE;
}

static gboolean
peak_x86_range_read_constant (PeakX86RangeMaps * maps, guintptr address,
                              guint64 * value)
{
  guint i; gsize length = 0; guint8 * copy;
  if (!peak_x86_range_constant_source (maps, address)) return FALSE;
  for (i = 0; i < sizeof (*value); i++)
  {
    GumPageProtection prot;
    if (!gum_memory_query_protection (GSIZE_TO_POINTER (address + i), &prot) ||
        (prot & GUM_PAGE_READ) == 0 ||
        (prot & (GUM_PAGE_WRITE | GUM_PAGE_EXECUTE)) != 0) return FALSE;
  }
  copy = gum_memory_read (GSIZE_TO_POINTER (address), sizeof (*value), &length);
  if (copy == NULL || length != sizeof (*value)) { g_free (copy); return FALSE; }
  memcpy (value, copy, sizeof (*value));
  g_free (copy);
  return TRUE;
}

static gboolean
peak_x86_range_add_displacement (guint64 base, gint64 displacement,
                                guintptr * address)
{
  guint64 magnitude;
  if (displacement >= 0)
  {
    if (base > G_MAXSIZE - (guint64) displacement) return FALSE;
    *address = base + displacement;
  }
  else
  {
    magnitude = (guint64) (-(displacement + 1)) + 1;
    if (base < magnitude) return FALSE;
    *address = base - magnitude;
  }
  return TRUE;
}

static void
peak_x86_range_predecessor (PeakX86RangeNode * nodes, GHashTable * starts,
                            guintptr pc, guint predecessor)
{
  guint encoded = GPOINTER_TO_UINT (g_hash_table_lookup (starts,
      GSIZE_TO_POINTER (pc)));
  if (encoded != 0)
  {
    PeakX86RangeNode * node = &nodes[encoded - 1];
    node->predecessors++;
    node->predecessor = predecessor;
  }
}

static gint
peak_x86_range_fold (PeakX86RangeNode * nodes, guint branch,
                      PeakX86RangeMaps * maps)
{
  guint chain[16], count = 0, index, reg, known = 0;
  guint64 values[16] = { 0 };
  PeakX86RangeNode * condition = &nodes[branch], * test;
  if (maps == NULL) return -1;
  if ((condition->id != X86_INS_JE && condition->id != X86_INS_JNE) ||
      condition->predecessors != 1 || condition->overlapping_call_entry) return -1;
  index = condition->predecessor;
  test = &nodes[index];
  if (test->pc + test->size != condition->pc || test->test_register == 0 ||
      test->overlapping_call_entry)
    return -1;
  reg = test->test_register;
  for (;;)
  {
    PeakX86RangeNode * node = &nodes[index];
    chain[count++] = index;
    if (count == G_N_ELEMENTS (chain) || node->predecessors != 1) break;
    index = node->predecessor;
    if (nodes[index].pc + nodes[index].size != node->pc ||
        nodes[index].conditional ||
        (nodes[index].direct && !nodes[index].call)) break;
  }
  while (count != 0)
  {
    PeakX86RangeNode * node = &nodes[chain[--count]];
    if (!node->register_metadata || node->overlapping_call_entry) return -1;
    guint destination = node->move_register;
    guint64 value = 0;
    gboolean constant = FALSE;
    if (destination != 0)
    {
      if (node->move_kind == 1) { value = node->immediate; constant = TRUE; }
      else if (node->move_kind == 2)
      {
        guintptr address;
        guint64 base = node->memory_rip ? node->pc + node->size :
            (node->memory_register != 0 ?
             values[g_bit_nth_lsf (node->memory_register, -1)] : 0);
        if ((node->memory_rip ||
             (node->memory_register != 0 && (known & node->memory_register))) &&
            peak_x86_range_add_displacement (base, node->displacement, &address))
          constant = peak_x86_range_read_constant (maps, address, &value);
      }
    }
    known &= ~node->writes;
    if (node->call || node->id == X86_INS_SYSCALL) known = 0;
    if (constant)
    {
      values[g_bit_nth_lsf (destination, -1)] = value;
      known |= destination;
    }
  }
  if ((known & reg) == 0) return -1;
  return (values[g_bit_nth_lsf (reg, -1)] == 0) == (condition->id == X86_INS_JE);
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
  nodes[*count].folded = -1;
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
  PeakX86RangeMaps * maps = NULL;

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
    guint8 * copy = gum_interceptor_peak_read_relocation_code (
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
        if (node->data_target > G_MAXSIZE - node->data_size) goto beach;
      }
    }
    node->unsupported_relative = insn->id == X86_INS_JCXZ ||
        insn->id == X86_INS_LOOP || insn->id == X86_INS_LOOPE ||
        insn->id == X86_INS_LOOPNE;
    {
      cs_regs reads, writes; guint8 n_reads = 0, n_writes = 0;
      cs_x86 * x = &insn->detail->x86;
      if (cs_regs_access (capstone, insn, reads, &n_reads, writes, &n_writes)
          != CS_ERR_OK) node->writes = 0xffff;
      else
      {
        node->register_metadata = TRUE;
        for (i = 0; i < n_writes; i++)
          node->writes |= peak_x86_range_register ((x86_reg) writes[i]);
      }
      if (insn->id == X86_INS_TEST && x->op_count == 2 &&
          x->operands[0].type == X86_OP_REG &&
          x->operands[1].type == X86_OP_REG &&
          x->operands[0].reg == x->operands[1].reg &&
          peak_x86_range_full_register (x->operands[0].reg) != 0 &&
          x->operands[0].size == 8 && x->operands[1].size == 8)
        node->test_register = peak_x86_range_register (x->operands[0].reg);
      if (insn->id == X86_INS_MOV && x->op_count == 2 &&
          x->operands[0].type == X86_OP_REG && x->operands[0].size == 8 &&
          peak_x86_range_full_register (x->operands[0].reg) != 0)
      {
        cs_x86_op * source = &x->operands[1];
        node->move_register = peak_x86_range_register (x->operands[0].reg);
        if (source->type == X86_OP_IMM)
        { node->move_kind = 1; node->immediate = source->imm; }
        else if (source->type == X86_OP_MEM && source->size == 8 &&
                 source->mem.index == X86_REG_INVALID &&
                 source->mem.segment == X86_REG_INVALID &&
                 (source->mem.base == X86_REG_RIP ||
                  peak_x86_range_full_register (source->mem.base) != 0))
        {
          node->move_kind = 2;
          node->memory_rip = source->mem.base == X86_REG_RIP;
          node->memory_register = peak_x86_range_register (source->mem.base);
          node->displacement = source->mem.disp;
        }
      }
    }
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
        if (!node->direct) { node->unknown = TRUE; terminal = TRUE; }
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
    node->fallthrough = !terminal;
    if (!terminal && !peak_x86_range_enqueue (node->pc + node->size,
        nodes, &count, starts)) goto beach;
  }

  /* Build predecessor counts from the entire graph before proving any branch.
   * In particular, a later edge into TEST invalidates a lexical-only proof. */
  nodes[0].predecessors = 1; /* virtual entry: never infer state before entry */
  nodes[0].predecessor = 0;
  for (i = 0; i < count; i++)
  {
    PeakX86RangeNode * node = &nodes[i];
    /* A conventional callee is opaque, but a direct call into a node already
     * present in this graph is an additional unknown-state entry to its proof
     * chain. Do not let its lexical fallthrough masquerade as domination. */
    if (node->direct)
    {
      peak_x86_range_predecessor (nodes, starts, node->target, i);
      if (node->call)
      {
        guint owner = GPOINTER_TO_UINT (g_hash_table_lookup (bytes,
            GSIZE_TO_POINTER (node->target)));
        if (owner != 0 && nodes[owner - 1].pc != node->target)
          nodes[owner - 1].overlapping_call_entry = TRUE;
      }
    }
    if (node->fallthrough)
      peak_x86_range_predecessor (nodes, starts, node->pc + node->size, i);
  }
  /* This proof assumes loaded ELF image code/data remain stable for this
   * analysis and attach transaction. Permission-only RO anonymous/shared data
   * are not immutable: independently snapshot and verify file/ELF provenance. */
  maps = peak_x86_range_maps_snapshot ();
  for (i = 0; i < count; i++)
    nodes[i].folded = peak_x86_range_fold (nodes, i, maps);
  /* No node is rejected merely because it lies on a proven-unselected edge.
   * Unknown nodes remain fatal whenever they are still reachable. */
  {
    guint queue[PEAK_X86_RANGE_BUDGET], head = 0, tail = 1;
    queue[0] = 0; nodes[0].reachable = TRUE;
    while (head < tail)
    {
      PeakX86RangeNode * node = &nodes[queue[head++]];
      guintptr successors[2]; guint n = 0, k;
      if (node->unknown) goto beach;
      if (node->direct && !node->call && node->folded != 0)
        successors[n++] = node->target;
      if (node->fallthrough && node->folded != 1)
        successors[n++] = node->pc + node->size;
      for (k = 0; k < n; k++)
      {
        guint encoded = GPOINTER_TO_UINT (g_hash_table_lookup (starts,
            GSIZE_TO_POINTER (successors[k])));
        if (encoded == 0) goto beach;
        if (!nodes[encoded - 1].reachable)
        { nodes[encoded - 1].reachable = TRUE; queue[tail++] = encoded - 1; }
      }
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
      if ((node->unsupported_relative || node->id == X86_INS_UD2 || node->unknown) &&
          node->pc >= entry && node->pc < limit)
      {
        limit = node->pc;
        changed = TRUE;
      }
      if (node->reachable && node->rip_data && node->data_target < limit &&
          node->data_target + node->data_size > entry)
      {
        guintptr next_limit = MAX (entry, node->data_target);
        if (next_limit < limit) { limit = next_limit; changed = TRUE; }
      }
      if (!node->reachable || !node->direct || node->target <= entry ||
          node->target >= limit || node->folded == 0)
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
  peak_x86_range_maps_free (maps);
  if (insn != NULL) cs_free (insn, 1);
  if (capstone != 0) cs_close (&capstone);
  g_hash_table_unref (bytes);
  g_hash_table_unref (starts);
  g_free (nodes);
  if (!complete) return FALSE;
  if (maximum != NULL) *maximum = safe;
  return legacy && legacy_span <= safe;
}

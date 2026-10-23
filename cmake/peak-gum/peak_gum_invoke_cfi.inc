/* DWARF row snapshots for relocated invoke instructions. No private unwind
 * context. */
#if defined(__x86_64__)
typedef struct {
  gpointer text, data, func;
} PeakDwarfBases;
extern const void *_Unwind_Find_FDE(const void *, PeakDwarfBases *);
typedef struct {
  guint kind, reg;
  gint64 offset;
  const guint8 *expr;
  gsize length;
} PeakCfiRule;
typedef struct {
  PeakCfiRule regs[128], cfa;
  guint64 args;
} PeakCfiRow;
typedef struct {
  const guint8 *p, *end;
  gboolean ok;
} PeakCfiCursor;
typedef struct PeakInvokeCfi PeakInvokeCfi;
struct PeakInvokeCfi {
  PeakInvokeCfi *next;
  gpointer owner;
  guintptr start, end, source, original_start;
  PeakDwarfBases bases;
  gpointer personality, lsda;
  gboolean signal_frame, mapped;
  gsize frame_size, lsda_offset;
  guint8 *frame, *fde;
};
static GMutex peak_invoke_lock;
static PeakInvokeCfi *peak_invoke_frames;
static _Thread_local PeakGumFunctionContext17 *peak_invoke_building;
static _Thread_local PeakInvokeCfi *peak_invoke_pending;
static _Thread_local gboolean peak_invoke_failed;
/* The FDE comes from this process's libgcc registry. Check its mapped
 * protections without process_vm_readv or ptrace; all bytes are then consumed
 * by the bounded cursor through ordinary in-process loads. */
static gboolean peak_cfi_readable(gconstpointer address, gsize n) {
  guintptr p = (guintptr)address, end;
  gsize page = gum_query_page_size();
  if (!page || __builtin_add_overflow(p, n, &end))
    return FALSE;
  while (p < end) {
    GumPageProtection prot;
    if (!gum_memory_query_protection((void *)p, &prot) ||
        !(prot & GUM_PAGE_READ))
      return FALSE;
    gsize step = page - p % page;
    if (step > end - p)
      step = end - p;
    p += step;
  }
  return TRUE;
}
static guint64 peak_cfi_read(PeakCfiCursor *c, guint n) {
  guint64 v = 0;
  if (!c->ok || n > 8 || (gsize)(c->end - c->p) < n) {
    c->ok = FALSE;
    return 0;
  }
  memcpy(&v, c->p, n);
  c->p += n;
  return v;
}
static guint64 peak_cfi_uleb(PeakCfiCursor *c) {
  guint64 n = 0;
  for (guint i = 0; i < 10 && c->ok; i++) {
    guint8 b = peak_cfi_read(c, 1);
    if (i == 9 && (b & 0xfe)) {
      c->ok = FALSE;
      break;
    }
    n |= (guint64)(b & 127) << (7 * i);
    if (!(b & 128))
      return n;
  }
  c->ok = FALSE;
  return 0;
}
static gint64 peak_cfi_sleb(PeakCfiCursor *c) {
  guint64 n = 0;
  for (guint i = 0; i < 10 && c->ok; i++) {
    guint8 b = peak_cfi_read(c, 1);
    if (i == 9 && b != 0 && b != 127) {
      c->ok = FALSE;
      break;
    }
    n |= (guint64)(b & 127) << (7 * i);
    if (!(b & 128)) {
      if (i < 9 && (b & 64))
        n |= ~(guint64)0 << (7 * (i + 1));
      return (gint64)n;
    }
  }
  c->ok = FALSE;
  return 0;
}
static gboolean peak_cfi_skip(PeakCfiCursor *c, guint64 n) {
  if (!c->ok || n > (guint64)(c->end - c->p)) {
    c->ok = FALSE;
    return FALSE;
  }
  c->p += n;
  return TRUE;
}
static gint64 peak_cfi_scale(PeakCfiCursor *c, gint64 a, gint64 b) {
  gint64 n;
  if (__builtin_mul_overflow(a, b, &n)) {
    c->ok = FALSE;
    return 0;
  }
  return n;
}
static guint64 peak_cfi_unsigned(PeakCfiCursor *c) {
  guint64 n = peak_cfi_uleb(c);
  if (n > G_MAXINT64)
    c->ok = FALSE;
  return n;
}
static guintptr peak_cfi_pointer(PeakCfiCursor *c, guint8 enc,
                                 PeakDwarfBases *bases) {
  if (enc == 255)
    return 0;
  guint app = enc & 0x70;
  if (app == 0x50) {
    gsize n = (-(guintptr)c->p) & 7;
    if (!peak_cfi_skip(c, n))
      return 0;
  } else if (app != 0 && app != 0x10 && app != 0x20 && app != 0x30 &&
             app != 0x40) {
    c->ok = FALSE;
    return 0;
  }
  guintptr base = app == 0x10   ? (guintptr)c->p
                  : app == 0x20 ? (guintptr)bases->text
                  : app == 0x30 ? (guintptr)bases->data
                  : app == 0x40 ? (guintptr)bases->func
                                : 0,
           v = 0;
  switch (enc & 15) {
  case 0:
    v = peak_cfi_read(c, 8);
    break;
  case 1:
    v = peak_cfi_uleb(c);
    break;
  case 2:
    v = peak_cfi_read(c, 2);
    break;
  case 3:
    v = peak_cfi_read(c, 4);
    break;
  case 4:
    v = peak_cfi_read(c, 8);
    break;
  case 9:
    v = peak_cfi_sleb(c);
    break;
  case 10:
    v = (gint16)peak_cfi_read(c, 2);
    break;
  case 11:
    v = (gint32)peak_cfi_read(c, 4);
    break;
  case 12:
    v = (gint64)peak_cfi_read(c, 8);
    break;
  default:
    c->ok = FALSE;
    return 0;
  }
  if (v)
    v += base;
  if ((enc & 128) && v) {
    if (!peak_cfi_readable((void *)v, 8)) {
      c->ok = FALSE;
      return 0;
    }
    memcpy(&v, (void *)v, 8);
  }
  return v;
}
static gboolean peak_cfi_record(const guint8 *p, PeakCfiCursor *c,
                                guint *offset_size) {
  if (!peak_cfi_readable(p, 4))
    return FALSE;
  guint32 small;
  memcpy(&small, p, 4);
  guint64 n = small;
  gsize header = 4;
  *offset_size = 4;
  if (small == 0xffffffff) {
    if (!peak_cfi_readable(p, 12))
      return FALSE;
    memcpy(&n, p + 4, 8);
    header = 12;
    *offset_size = 8;
  }
  gsize bytes;
  if (n < *offset_size || n > G_MAXSIZE ||
      __builtin_add_overflow(header, (gsize)n, &bytes) ||
      (guintptr)p > UINTPTR_MAX - bytes || !peak_cfi_readable(p, bytes))
    return FALSE;
  *c = (PeakCfiCursor){p + header, p + bytes, TRUE};
  return TRUE;
}
typedef struct PeakCfiSaved PeakCfiSaved;
struct PeakCfiSaved {
  PeakCfiSaved *next;
  PeakCfiRow row;
};
static gboolean peak_cfi_exec(PeakCfiCursor c, PeakCfiRow *row,
                              const PeakCfiRow *initial, guintptr pc,
                              guintptr limit, guint64 code, gint64 data,
                              guint8 enc, PeakDwarfBases *bases) {
  PeakCfiSaved *saved = NULL;
  while (c.ok && c.p < c.end && pc <= limit) {
    guint8 op = peak_cfi_read(&c, 1);
    guint64 reg, n;
    gint64 off;
    guint64 advance = 0;
    if ((op & 0xc0) == 0x40) {
      advance = op & 63;
      goto advance;
    }
    if ((op & 0xc0) == 0x80) {
      reg = op & 63;
      row->regs[reg] = (PeakCfiRule){
          .kind = 1, .offset = peak_cfi_scale(&c, peak_cfi_unsigned(&c), data)};
      continue;
    }
    if ((op & 0xc0) == 0xc0) {
      row->regs[op & 63] = initial->regs[op & 63];
      continue;
    }
    switch (op) {
    case 0:
      break;
    case 1:
      pc = peak_cfi_pointer(&c, enc, bases);
      break;
    case 2:
      advance = peak_cfi_read(&c, 1);
      goto advance;
    case 3:
      advance = peak_cfi_read(&c, 2);
      goto advance;
    case 4:
      advance = peak_cfi_read(&c, 4);
      goto advance;
    case 5:
    case 0x11:
    case 0x14:
    case 0x15:
      reg = peak_cfi_uleb(&c);
      off = (op == 0x11 || op == 0x15) ? peak_cfi_sleb(&c)
                                       : (gint64)peak_cfi_unsigned(&c);
      if (reg >= 128) {
        c.ok = FALSE;
        break;
      }
      row->regs[reg] = (PeakCfiRule){.kind = (op == 0x14 || op == 0x15) ? 5 : 1,
                                     .offset = peak_cfi_scale(&c, off, data)};
      break;
    case 6:
      reg = peak_cfi_uleb(&c);
      if (reg >= 128) {
        c.ok = FALSE;
        break;
      }
      row->regs[reg] = initial->regs[reg];
      break;
    case 7:
    case 8:
      reg = peak_cfi_uleb(&c);
      if (reg >= 128) {
        c.ok = FALSE;
        break;
      }
      row->regs[reg] = (PeakCfiRule){.kind = op == 7 ? 2 : 0};
      break;
    case 9:
      reg = peak_cfi_uleb(&c);
      n = peak_cfi_uleb(&c);
      if (reg >= 128 || n >= 128) {
        c.ok = FALSE;
        break;
      }
      row->regs[reg] = (PeakCfiRule){.kind = 3, .reg = n};
      break;
    case 10: {
      PeakCfiSaved *s = g_try_malloc(sizeof(*s));
      if (!s) {
        c.ok = FALSE;
        break;
      }
      s->row = *row;
      s->next = saved;
      saved = s;
      break;
    }
    case 11:
      if (!saved) {
        c.ok = FALSE;
        break;
      }
      {
        PeakCfiSaved *s = saved;
        saved = s->next;
        *row = s->row;
        g_free(s);
      }
      break;
    case 12:
    case 0x12:
      reg = peak_cfi_uleb(&c);
      off = op == 12 ? (gint64)peak_cfi_unsigned(&c)
                     : peak_cfi_scale(&c, peak_cfi_sleb(&c), data);
      if (reg >= 128) {
        c.ok = FALSE;
        break;
      }
      row->cfa = (PeakCfiRule){.kind = 1, .reg = reg, .offset = off};
      break;
    case 13:
      reg = peak_cfi_uleb(&c);
      if (reg >= 128) {
        c.ok = FALSE;
        break;
      }
      row->cfa.reg = reg;
      break;
    case 14:
    case 0x13:
      row->cfa.offset = op == 14 ? (gint64)peak_cfi_unsigned(&c)
                                 : peak_cfi_scale(&c, peak_cfi_sleb(&c), data);
      break;
    case 15:
      n = peak_cfi_uleb(&c);
      row->cfa = (PeakCfiRule){.kind = 4, .expr = c.p, .length = n};
      peak_cfi_skip(&c, n);
      break;
    case 16:
    case 0x16:
      reg = peak_cfi_uleb(&c);
      n = peak_cfi_uleb(&c);
      if (reg >= 128) {
        c.ok = FALSE;
        break;
      }
      row->regs[reg] =
          (PeakCfiRule){.kind = op == 16 ? 4 : 6, .expr = c.p, .length = n};
      peak_cfi_skip(&c, n);
      break;
    case 0x2e:
      row->args = peak_cfi_uleb(&c);
      break;
    case 0x2f:
      reg = peak_cfi_uleb(&c);
      off = peak_cfi_unsigned(&c);
      if (reg >= 128) {
        c.ok = FALSE;
        break;
      }
      row->regs[reg] =
          (PeakCfiRule){.kind = 1, .offset = peak_cfi_scale(&c, -off, data)};
      break;
    default:
      c.ok = FALSE;
      break;
    }
    continue;
  advance: {
    guintptr delta;
    if (__builtin_mul_overflow(advance, code, &delta) ||
        __builtin_add_overflow(pc, delta, &pc))
      c.ok = FALSE;
  }
  }
  while (saved) {
    PeakCfiSaved *s = saved;
    saved = s->next;
    g_free(s);
  }
  return c.ok;
}
static gboolean peak_cfi_snapshot(guintptr source, PeakInvokeCfi *m,
                                  PeakCfiRow *row) {
  const guint8 *fde = _Unwind_Find_FDE((void *)(source - 1), &m->bases);
  if (!fde)
    return FALSE;
  guint os;
  PeakCfiCursor f;
  if (!peak_cfi_record(fde, &f, &os))
    return FALSE;
  guintptr field = (guintptr)f.p;
  guint64 cieoff = peak_cfi_read(&f, os);
  if (!f.ok || cieoff > field)
    return FALSE;
  guint cos;
  PeakCfiCursor c;
  if (!peak_cfi_record((void *)(field - cieoff), &c, &cos))
    return FALSE;
  if (peak_cfi_read(&c, cos) != 0)
    return FALSE;
  guint8 version = peak_cfi_read(&c, 1);
  if (version != 1 && version != 3 && version != 4)
    return FALSE;
  const guint8 *aug = c.p, *nul = memchr(c.p, 0, c.end - c.p);
  if (!nul)
    return FALSE;
  c.p = nul + 1;
  if (version == 4) {
    if (peak_cfi_read(&c, 1) != 8 || peak_cfi_read(&c, 1) != 0)
      return FALSE;
  }
  guint64 code = peak_cfi_uleb(&c);
  gint64 data = peak_cfi_sleb(&c);
  guint64 ret = version == 1 ? peak_cfi_read(&c, 1) : peak_cfi_uleb(&c);
  if (!c.ok || ret != 16 || !code || !data)
    return FALSE;
  guint8 enc = 0, lsdaenc = 255;
  gboolean z = *aug == 'z';
  PeakCfiCursor a = c;
  if (z) {
    guint64 n = peak_cfi_uleb(&c);
    a = c;
    if (!peak_cfi_skip(&c, n))
      return FALSE;
    a.end = c.p;
    aug++;
  }
  for (; aug < nul; aug++)
    switch (*aug) {
    case 'R':
      enc = peak_cfi_read(&a, 1);
      break;
    case 'L':
      lsdaenc = peak_cfi_read(&a, 1);
      break;
    case 'P': {
      guint8 e = peak_cfi_read(&a, 1);
      m->personality = (void *)peak_cfi_pointer(&a, e, &m->bases);
      break;
    }
    case 'S':
      m->signal_frame = TRUE;
      break;
    default:
      return FALSE;
    }
  if (!a.ok)
    return FALSE;
  if (!z)
    c = a;
  PeakCfiRow initial = {0};
  memset(row, 0, sizeof(*row));
  for (guint i = 0; i < 128; i++)
    initial.regs[i].kind = row->regs[i].kind = 7;
  if (!peak_cfi_exec(c, row, &initial, 0, ~(guintptr)0, code, data, enc,
                     &m->bases))
    return FALSE;
  initial = *row;
  guintptr location = peak_cfi_pointer(&f, enc, &m->bases);
  PeakDwarfBases empty = {0};
  peak_cfi_pointer(&f, enc & 15, &empty);
  if (z) {
    guint64 n = peak_cfi_uleb(&f);
    a = f;
    if (!peak_cfi_skip(&f, n))
      return FALSE;
    a.end = f.p;
    if (lsdaenc != 255)
      m->lsda = (void *)peak_cfi_pointer(&a, lsdaenc, &m->bases);
    if (!a.ok)
      return FALSE;
  } else if (lsdaenc != 255)
    return FALSE;
  return f.ok && peak_cfi_exec(f, row, &initial, location, source - 1, code,
                               data, enc, &m->bases);
}
static void peak_cfi_put_uleb(guint8 **p, guint64 n) {
  do {
    guint8 b = n & 127;
    n >>= 7;
    *(*p)++ = b | (n ? 128 : 0);
  } while (n);
}
static void peak_cfi_put_sleb(guint8 **p, gint64 n) {
  gboolean more;
  do {
    guint8 b = n & 127;
    n >>= 7;
    more = !((n == 0 && !(b & 64)) || (n == -1 && (b & 64)));
    *(*p)++ = b | (more ? 128 : 0);
  } while (more);
}
static PeakInvokeCfi *peak_invoke_lookup(guintptr pc) {
  for (PeakInvokeCfi *m = peak_invoke_frames; m; m = m->next)
    if (pc > m->start && pc <= m->end)
      return m;
  return NULL;
}
static guintptr peak_invoke_normalize_address(guintptr pc) {
  g_mutex_lock(&peak_invoke_lock);
  PeakInvokeCfi *m = peak_invoke_lookup(pc);
  if (m)
    pc = m->source;
  g_mutex_unlock(&peak_invoke_lock);
  return pc;
}
typedef struct PeakUnwindScope PeakUnwindScope;
struct PeakUnwindScope {
  PeakUnwindScope *previous;
  struct _Unwind_Context *context;
  guintptr source, region;
  gpointer lsda;
};
static _Thread_local PeakUnwindScope *peak_unwind_scope;
static gpointer peak_unwind_region_original, peak_unwind_ipinfo_original,
    peak_unwind_lsda_original;
static GumInterceptor *peak_unwind_api_interceptor;
static GMutex peak_unwind_api_lock;
typedef void *(*PeakLsdaFn)(struct _Unwind_Context *);
static PeakInvokeCfi *peak_cfi_envelope(gpointer raw);
typedef _Unwind_Ptr (*PeakRegionFn)(struct _Unwind_Context *);
typedef _Unwind_Ptr (*PeakIpInfoFn)(struct _Unwind_Context *, int *);
static _Unwind_Ptr peak_unwind_ipinfo(struct _Unwind_Context *ctx,
                                      int *before) {
  PeakIpInfoFn original =
      GUM_POINTER_TO_FUNCPTR(PeakIpInfoFn, peak_unwind_ipinfo_original);
  guintptr ip = original(ctx, before);
  g_mutex_lock(&peak_invoke_lock);
  PeakInvokeCfi *m = peak_invoke_lookup(ip + (*before ? 1 : 0));
  if (m)
    ip = *before ? m->original_start : m->source;
  g_mutex_unlock(&peak_invoke_lock);
  return ip;
}
G_GNUC_INTERNAL _Unwind_Ptr
peak_gum_unwind_get_ip(struct _Unwind_Context *ctx) {
  guintptr ip = _Unwind_GetIP(ctx);
  g_mutex_lock(&peak_invoke_lock);
  PeakInvokeCfi *m = peak_invoke_lookup(ip);
  if (m)
    ip = m->source;
  g_mutex_unlock(&peak_invoke_lock);
  return ip;
}
static _Unwind_Ptr peak_unwind_region(struct _Unwind_Context *ctx) {
  for (PeakUnwindScope *s = peak_unwind_scope; s; s = s->previous)
    if (s->context == ctx)
      return s->region;
  PeakIpInfoFn get_ip =
      GUM_POINTER_TO_FUNCPTR(PeakIpInfoFn, peak_unwind_ipinfo_original);
  int before;
  guintptr ip = get_ip(ctx, &before);
  g_mutex_lock(&peak_invoke_lock);
  PeakInvokeCfi *m = peak_invoke_lookup(ip + (before ? 1 : 0));
  guintptr region = m ? (guintptr)m->bases.func : 0;
  g_mutex_unlock(&peak_invoke_lock);
  if (region)
    return region;
  PeakRegionFn original =
      GUM_POINTER_TO_FUNCPTR(PeakRegionFn, peak_unwind_region_original);
  return original(ctx);
}
static void *peak_unwind_lsda(struct _Unwind_Context *ctx) {
  for (PeakUnwindScope *s = peak_unwind_scope; s; s = s->previous)
    if (s->context == ctx)
      return s->lsda;
  PeakLsdaFn original =
      GUM_POINTER_TO_FUNCPTR(PeakLsdaFn, peak_unwind_lsda_original);
  gpointer raw = original(ctx);
  g_mutex_lock(&peak_invoke_lock);
  PeakInvokeCfi *m = peak_cfi_envelope(raw);
  gpointer answer = m ? m->lsda : raw;
  g_mutex_unlock(&peak_invoke_lock);
  return answer;
}
static gboolean peak_invoke_api_ensure(GumInterceptor *interceptor) {
  gboolean ok = TRUE;
  g_mutex_lock(&peak_unwind_api_lock);
  if (!peak_unwind_api_interceptor) {
    GumReplaceReturn r = gum_interceptor_replace(
        interceptor, (void *)_Unwind_GetRegionStart, (void *)peak_unwind_region,
        &peak_unwind_region_original, NULL);
    if (r != GUM_REPLACE_OK)
      ok = FALSE;
    if (ok) {
      r = gum_interceptor_replace(interceptor, (void *)_Unwind_GetIPInfo,
                                  (void *)peak_unwind_ipinfo,
                                  &peak_unwind_ipinfo_original, NULL);
      if (r != GUM_REPLACE_OK) {
        gum_interceptor_revert(interceptor, (void *)_Unwind_GetRegionStart);
        ok = FALSE;
      }
    }
    if (ok) {
      r = gum_interceptor_replace(
          interceptor, (void *)_Unwind_GetLanguageSpecificData,
          (void *)peak_unwind_lsda, &peak_unwind_lsda_original, NULL);
      if (r != GUM_REPLACE_OK) {
        gum_interceptor_revert(interceptor, (void *)_Unwind_GetIPInfo);
        gum_interceptor_revert(interceptor, (void *)_Unwind_GetRegionStart);
        ok = FALSE;
      }
    }
    if (ok)
      peak_unwind_api_interceptor = g_object_ref(interceptor);
  }
  g_mutex_unlock(&peak_unwind_api_lock);
  return ok;
}
static void peak_invoke_api_shutdown(void) {
  g_mutex_lock(&peak_unwind_api_lock);
  if (peak_unwind_api_interceptor) {
    gum_interceptor_revert(peak_unwind_api_interceptor,
                           (void *)_Unwind_GetLanguageSpecificData);
    gum_interceptor_revert(peak_unwind_api_interceptor,
                           (void *)_Unwind_GetIPInfo);
    gum_interceptor_revert(peak_unwind_api_interceptor,
                           (void *)_Unwind_GetRegionStart);
    g_clear_object(&peak_unwind_api_interceptor);
  }
  g_mutex_unlock(&peak_unwind_api_lock);
}
static _Unwind_Reason_Code
peak_invoke_personality(int v, _Unwind_Action actions, guint64 cls,
                        struct _Unwind_Exception *e,
                        struct _Unwind_Context *ctx) {
  PeakIpInfoFn get_ip =
      GUM_POINTER_TO_FUNCPTR(PeakIpInfoFn, peak_unwind_ipinfo_original);
  PeakLsdaFn get_lsda =
      GUM_POINTER_TO_FUNCPTR(PeakLsdaFn, peak_unwind_lsda_original);
  int before;
  guintptr ip = get_ip(ctx, &before);
  g_mutex_lock(&peak_invoke_lock);
  PeakInvokeCfi *m = peak_cfi_envelope(get_lsda(ctx));
  gpointer fn = m ? m->personality : NULL;
  PeakUnwindScope scope = {peak_unwind_scope, ctx,
                           m ? (before ? m->original_start : m->source) : 0,
                           m ? (guintptr)m->bases.func : 0, m ? m->lsda : NULL};
  g_mutex_unlock(&peak_invoke_lock);
  if (!fn)
    return _URC_FATAL_PHASE1_ERROR;
  peak_unwind_scope = &scope;
  _Unwind_SetIP(ctx, scope.source);
  _Unwind_Personality_Fn call =
      GUM_POINTER_TO_FUNCPTR(_Unwind_Personality_Fn, fn);
  _Unwind_Reason_Code result = call(v, actions, cls, e, ctx);
  if (result == _URC_INSTALL_CONTEXT) {
    guintptr landing = get_ip(ctx, &before);
    g_mutex_lock(&peak_invoke_lock);
    for (PeakInvokeCfi *p = peak_invoke_frames; p; p = p->next)
      if (p->owner == m->owner && p->original_start == landing) {
        landing = p->start;
        break;
      }
    g_mutex_unlock(&peak_invoke_lock);
    _Unwind_SetIP(ctx, landing);
  } else
    _Unwind_SetIP(ctx, ip);
  peak_unwind_scope = scope.previous;
  return result;
}
static gboolean peak_invoke_emit(PeakInvokeCfi *m, PeakCfiRow *row) {
  gsize bytes = 256 + 128 * 32;
  if (row->cfa.kind != 1 && row->cfa.kind != 4)
    return FALSE;
  if (__builtin_add_overflow(bytes, row->cfa.length, &bytes))
    return FALSE;
  for (guint i = 0; i < 128; i++)
    if (__builtin_add_overflow(bytes, row->regs[i].length, &bytes))
      return FALSE;
  guint8 *p = m->frame = g_try_malloc0(bytes), *cie = p;
  if (!p)
    return FALSE;
  peak_uw_u32(&p, 0);
  peak_uw_u32(&p, 0);
  peak_uw_b(&p, 1);
  const char *aug = m->personality ? (m->signal_frame ? "zPLRS" : "zPLR")
                                   : (m->signal_frame ? "zLRS" : "zLR");
  memcpy(p, aug, strlen(aug) + 1);
  p += strlen(aug) + 1;
  /* A unit data alignment preserves every signed byte offset exactly. */
  peak_uw_b(&p, 1);
  peak_uw_b(&p, 1);
  peak_uw_b(&p, 16);
  peak_uw_b(&p, m->personality ? 11 : 2);
  if (m->personality) {
    peak_uw_b(&p, 0);
    peak_uw_ptr(&p, (guintptr)(m->mapped ? (gpointer)peak_invoke_personality
                                         : m->personality));
  }
  peak_uw_b(&p, 0);
  peak_uw_b(&p, 0);
  /* These constant snapshot rules belong in the CIE. The virtual PC may
   * precede the original function base, so a PC-bounded FDE program would
   * otherwise be skipped by libgcc before applying its first instruction. */
  if (row->cfa.kind == 4) {
    peak_uw_b(&p, 15);
    peak_cfi_put_uleb(&p, row->cfa.length);
    memcpy(p, row->cfa.expr, row->cfa.length);
    p += row->cfa.length;
  } else {
    peak_uw_b(&p, 0x12);
    peak_cfi_put_uleb(&p, row->cfa.reg);
    peak_cfi_put_sleb(&p, row->cfa.offset);
  }
  for (guint i = 0; i < 128; i++) {
    PeakCfiRule *r = &row->regs[i];
    if (r->kind == 7)
      continue;
    guint8 op = r->kind == 0   ? 8
                : r->kind == 1 ? 0x11
                : r->kind == 2 ? 7
                : r->kind == 3 ? 9
                : r->kind == 4 ? 16
                : r->kind == 5 ? 0x15
                               : 0x16;
    peak_uw_b(&p, op);
    peak_cfi_put_uleb(&p, i);
    if (r->kind == 1 || r->kind == 5)
      peak_cfi_put_sleb(&p, r->offset);
    else if (r->kind == 3)
      peak_cfi_put_uleb(&p, r->reg);
    else if (r->kind == 4 || r->kind == 6) {
      peak_cfi_put_uleb(&p, r->length);
      memcpy(p, r->expr, r->length);
      p += r->length;
    }
  }
  if (row->args) {
    peak_uw_b(&p, 0x2e);
    peak_cfi_put_uleb(&p, row->args);
  }
  peak_uw_finish(cie, &p);
  m->fde = p;
  peak_uw_u32(&p, 0);
  peak_uw_u32(&p, p - m->frame);
  peak_uw_ptr(&p, (guintptr)m->bases.func);
  peak_uw_ptr(&p, 1);
  peak_uw_b(&p, 8);
  m->lsda_offset = p - m->frame;
  peak_uw_ptr(&p, (guintptr)m->lsda);
  peak_uw_finish(m->fde, &p);
  peak_uw_u32(&p, 0);
  m->frame_size = p - m->frame;
  g_assert(m->frame_size <= bytes);
  return TRUE;
}
typedef struct PeakUwFdeCache PeakUwFdeCache;
struct PeakUwFdeCache {
  PeakUwFdeCache *next;
  guintptr lookup;
  PeakInvokeCfi m;
};
/* Only compare registered metadata identities, never dereference an arbitrary
 * LSDA. */
static PeakInvokeCfi *peak_cfi_envelope(gpointer raw) {
  for (PeakInvokeCfi *m = peak_invoke_frames; m; m = m->next)
    if (m == raw && m->mapped && m->personality)
      return m;
  if (peak_uw_thread)
    for (PeakUwFdeCache *c = peak_uw_thread->cfi_cache; c; c = c->next)
      if (&c->m == raw && c->m.mapped && c->m.personality)
        return &c->m;
  return NULL;
}
static void peak_cfi_bind(PeakInvokeCfi *m) {
  if (m->mapped && m->personality) {
    guintptr envelope = (guintptr)m;
    memcpy(m->frame + m->lsda_offset, &envelope, 8);
  }
}
static gboolean peak_cfi_bytes_equal(PeakInvokeCfi *a, PeakInvokeCfi *b) {
  return a->frame_size == b->frame_size && a->lsda_offset == b->lsda_offset &&
         memcmp(a->frame, b->frame, a->lsda_offset) == 0 &&
         memcmp(a->frame + a->lsda_offset + 8, b->frame + b->lsda_offset + 8,
                a->frame_size - a->lsda_offset - 8) == 0;
}
static void peak_uw_cache_clear(PeakUwThread *owner) {
  PeakUwFdeCache *c = owner->cfi_cache;
  owner->cfi_cache = NULL;
  while (c) {
    PeakUwFdeCache *next = c->next;
    g_free(c->m.frame);
    g_free(c);
    c = next;
  }
}
static void peak_cfi_branch(guint8 *field, guint8 *target) {
  gint16 delta = target - (field + 2);
  memcpy(field, &delta, 2);
}
static guint8 *peak_cfi_map_ra(PeakCfiRule *r, gsize *length) {
  if (r->kind == 2 || r->kind == 7)
    return NULL;
  gboolean location = r->kind == 1 || r->kind == 4;
  gsize bytes;
  if (__builtin_add_overflow(r->length, (gsize)256, &bytes))
    return NULL;
  guint8 *code = g_try_malloc(bytes), *p = code;
  if (!p)
    return NULL;
  peak_uw_b(&p, 0x12); /* Retain a base entry below the computed RA. */
  /* libgcc supplies the actual frame CFA as the expression's initial stack
   * value. No synthetic frame, private context access, TLS opcode or helper. */
  if (r->kind == 1 || r->kind == 5) {
    peak_uw_b(&p, 0x11);
    peak_cfi_put_sleb(&p, r->offset);
    peak_uw_b(&p, 0x22);
  } else if (r->kind == 3 || r->kind == 0) {
    peak_uw_b(&p, 0x90);
    peak_cfi_put_uleb(&p, r->kind == 3 ? r->reg : 16);
  } else if (r->kind == 4 || r->kind == 6) {
    memcpy(p, r->expr, r->length);
    p += r->length;
  } else {
    g_free(code);
    return NULL;
  }
  if (location) {
    peak_uw_b(&p, 0x12);
    peak_uw_b(&p, 6);
  }
  peak_uw_b(&p, 3);
  peak_uw_ptr(&p, (guintptr)&peak_uw_thread->active);
  peak_uw_b(&p, 6);
  guint8 *loop = p;
  peak_uw_b(&p, 0x12);
  peak_uw_b(&p, 0x30);
  peak_uw_b(&p, 0x29);
  peak_uw_b(&p, 0x28);
  guint8 *empty = p;
  p += 2;
  peak_uw_b(&p, 0x12);
  peak_uw_b(&p, 0x23);
  peak_cfi_put_uleb(&p, offsetof(PeakUwFrame, pc));
  peak_uw_b(&p, 6);
  peak_uw_b(&p, 0x23);
  peak_uw_b(&p, 1);
  peak_uw_b(&p, 0x15);
  peak_uw_b(&p, 2);
  peak_uw_b(&p, 0x29);
  peak_uw_b(&p, 0x28);
  guint8 *match = p;
  p += 2;
  peak_uw_b(&p, 0x23);
  peak_cfi_put_uleb(&p, offsetof(PeakUwFrame, next));
  peak_uw_b(&p, 6);
  peak_uw_b(&p, 0x2f);
  guint8 *again = p;
  p += 2;
  peak_cfi_branch(again, loop);
  peak_cfi_branch(match, p);
  peak_uw_b(&p, 0x23);
  peak_cfi_put_uleb(&p, offsetof(PeakUwFrame, caller));
  if (!location)
    peak_uw_b(&p, 6);
  peak_uw_b(&p, 0x16);
  peak_uw_b(&p, 0x13);
  if (location) {
    peak_uw_b(&p, 0x16);
    peak_uw_b(&p, 0x13);
    peak_uw_b(&p, 0x12);
    peak_uw_b(&p, 6);
  }
  /* A tailcall's caller may be an older bridge at the same physical slot.
   * Restart until reaching the native RA. Active callers precede their
   * child in admission order, so this bridge graph is acyclic. */
  peak_uw_b(&p, 3);
  peak_uw_ptr(&p, (guintptr)&peak_uw_thread->active);
  peak_uw_b(&p, 6);
  peak_uw_b(&p, 0x2f);
  guint8 *restart = p;
  p += 2;
  peak_cfi_branch(restart, loop);
  peak_cfi_branch(empty, p);
  peak_uw_b(&p, 0x13);
  if (location)
    peak_uw_b(&p, 0x13);
  *length = p - code;
  g_assert(*length <= bytes);
  return code;
}
G_GNUC_INTERNAL const void *peak_gum_unwind_find_fde(const void *pc,
                                                     PeakDwarfBases *bases) {
  g_mutex_lock(&peak_invoke_lock);
  PeakInvokeCfi *source_meta = peak_invoke_lookup((guintptr)pc + 1);
  PeakInvokeCfi m = {0};
  if (source_meta)
    m = *source_meta;
  g_mutex_unlock(&peak_invoke_lock);
  if (!peak_uw_thread || !peak_uw_thread->active) {
    if (source_meta) {
      *bases = m.bases;
      bases->func = (void *)m.start;
      return m.fde;
    }
    return _Unwind_Find_FDE(pc, bases);
  }
  PeakCfiRow row;
  guintptr source = source_meta ? m.source : (guintptr)pc + 1;
  m.source = source;
  if (!peak_cfi_snapshot(source, &m, &row))
    return _Unwind_Find_FDE(pc, bases);
  gsize expr_len;
  guint8 *expr = peak_cfi_map_ra(&row.regs[16], &expr_len);
  if (expr)
    row.regs[16] = (PeakCfiRule){
        .kind = (row.regs[16].kind == 1 || row.regs[16].kind == 4) ? 4 : 6,
        .expr = expr,
        .length = expr_len};
  m.frame = m.fde = NULL;
  if (!peak_invoke_emit(&m, &row)) {
    g_free(expr);
    return _Unwind_Find_FDE(pc, bases);
  }
  g_free(expr);
  for (PeakUwFdeCache *c = peak_uw_thread->cfi_cache; c; c = c->next)
    if (c->lookup == (guintptr)pc && c->m.personality == m.personality &&
        c->m.bases.text == m.bases.text && c->m.bases.data == m.bases.data &&
        c->m.bases.func == m.bases.func && c->m.lsda == m.lsda &&
        c->m.mapped == m.mapped && c->m.signal_frame == m.signal_frame &&
        c->m.source == m.source && c->m.start == m.start && c->m.end == m.end &&
        c->m.original_start == m.original_start &&
        peak_cfi_bytes_equal(&c->m, &m)) {
      g_free(m.frame);
      *bases = c->m.bases;
      if (c->m.mapped)
        bases->func = (void *)c->m.start;
      return c->m.fde;
    }
  PeakUwFdeCache *c = g_try_malloc(sizeof(*c));
  if (!c) {
    g_free(m.frame);
    return _Unwind_Find_FDE(pc, bases);
  }
  c->lookup = (guintptr)pc;
  c->m = m;
  peak_cfi_bind(&c->m);
  c->next = peak_uw_thread->cfi_cache;
  peak_uw_thread->cfi_cache = c;
  *bases = m.bases;
  if (m.mapped)
    bases->func = (void *)m.start;
  return m.fde;
}
extern gboolean _gum_interceptor_backend_create_trampoline_peak_original(
    GumInterceptorBackend *, PeakGumFunctionContext17 *, gboolean);
extern void _gum_interceptor_backend_destroy_trampoline_peak_original(
    GumInterceptorBackend *, PeakGumFunctionContext17 *);
extern cs_insn *gum_x86_relocator_peek_next_write_insn(PeakGumX86Relocator17 *);
extern gboolean gum_x86_relocator_write_one(PeakGumX86Relocator17 *);
static void peak_invoke_free(PeakInvokeCfi *m) {
  g_free(m->frame);
  g_free(m);
}
G_GNUC_INTERNAL void peak_gum_unwind_relocate_all(PeakGumX86Relocator17 *rl) {
  cs_insn *insn;
  while ((insn = gum_x86_relocator_peek_next_write_insn(rl)) != NULL) {
    guintptr start = rl->output->pc, original_start = insn->address,
             source = insn->address + insn->size;
    if (!gum_x86_relocator_write_one(rl)) {
      if (peak_invoke_building)
        peak_invoke_failed = TRUE;
      break;
    }
    if (peak_invoke_building && !peak_invoke_failed) {
      PeakDwarfBases bases;
      /* Generated code and hand-written assembly may intentionally have no
       * registered FDE. There is no source unwind row to preserve in that
       * case, so retain Gum's established attach behavior. A present FDE
       * still has to parse and reproduce completely before the trampoline is
       * accepted. */
      if (_Unwind_Find_FDE((void *)(source - 1), &bases) == NULL)
        continue;
      PeakInvokeCfi *m = g_try_malloc0(sizeof(*m));
      PeakCfiRow row;
      if (!m) {
        peak_invoke_failed = TRUE;
        continue;
      }
      m->original_start = original_start;
      m->mapped = TRUE;
      m->owner = peak_invoke_building;
      m->start = start;
      m->end = rl->output->pc;
      m->source = source;
      if (peak_cfi_snapshot(source, m, &row) && peak_invoke_emit(m, &row)) {
        peak_cfi_bind(m);
        m->next = peak_invoke_pending;
        peak_invoke_pending = m;
      } else {
        peak_invoke_free(m);
        peak_invoke_failed = TRUE;
      }
    }
  }
}
G_GNUC_INTERNAL gboolean _gum_interceptor_backend_create_trampoline(
    GumInterceptorBackend *b, PeakGumFunctionContext17 *ctx, gboolean force) {
  PeakGumFunctionContext17 *old = peak_invoke_building;
  PeakInvokeCfi *old_pending = peak_invoke_pending;
  gboolean old_failed = peak_invoke_failed;
  peak_invoke_building = ctx;
  peak_invoke_pending = NULL;
  peak_invoke_failed = FALSE;
  gboolean result =
      _gum_interceptor_backend_create_trampoline_peak_original(b, ctx, force);
  if (result && peak_invoke_failed) {
    _gum_interceptor_backend_destroy_trampoline_peak_original(b, ctx);
    result = FALSE;
  }
  g_mutex_lock(&peak_invoke_lock);
  while (peak_invoke_pending) {
    PeakInvokeCfi *m = peak_invoke_pending;
    peak_invoke_pending = m->next;
    if (result) {
      m->next = peak_invoke_frames;
      peak_invoke_frames = m;
    } else
      peak_invoke_free(m);
  }
  g_mutex_unlock(&peak_invoke_lock);
  peak_invoke_building = old;
  peak_invoke_pending = old_pending;
  peak_invoke_failed = old_failed;
  return result;
}
G_GNUC_INTERNAL void
_gum_interceptor_backend_destroy_trampoline(GumInterceptorBackend *b,
                                            PeakGumFunctionContext17 *ctx) {
  g_mutex_lock(&peak_invoke_lock);
  for (PeakInvokeCfi **p = &peak_invoke_frames; *p;) {
    PeakInvokeCfi *m = *p;
    if (m->owner == ctx) {
      *p = m->next;
      peak_invoke_free(m);
    } else
      p = &m->next;
  }
  g_mutex_unlock(&peak_invoke_lock);
  _gum_interceptor_backend_destroy_trampoline_peak_original(b, ctx);
}
#else
static gboolean peak_invoke_api_ensure(GumInterceptor *interceptor) {
  (void)interceptor;
  return TRUE;
}
static void peak_invoke_api_shutdown(void) {}
static guintptr peak_invoke_normalize_address(guintptr pc) { return pc; }
#endif

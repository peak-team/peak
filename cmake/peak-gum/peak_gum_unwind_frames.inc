/* Linux x86-64 return-frame metadata, owned through actual thread termination.
 */
#if defined(__x86_64__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
extern void __register_frame(void *);
extern void *__deregister_frame(void *);
extern gboolean _frida_g_thread_garbage_collect(void);
extern GPrivate peak_gum_interceptor_context_private;
extern void peak_gum_release_interceptor_thread_context_original(gpointer);
extern gboolean pthread_listener_current_thread_token(gsize *, guint64 *)
    __attribute__((weak));
extern void peak_general_listener_controller_wake(void) __attribute__((weak));
extern void peak_general_listener_note_dropped_current_thread(void)
    __attribute__((weak));
#define PEAK_UW_SLOTS 32

typedef struct PeakUwFrame PeakUwFrame;
typedef struct PeakUwBlock PeakUwBlock;
typedef struct PeakUwThread PeakUwThread;
struct PeakUwFrame {
  gpointer destination, caller;
  gpointer *slot;
  PeakGumFunctionContext17 *context;
  guint8 *pc;
  gboolean observed, generic;
  guint64 reaped_epoch;
  gpointer reaped_context, reaped_slot;
  guint64 walk_live_epoch;
  PeakUwFrame *next, *free_next;
  guint8 eh[160];
};
struct PeakUwBlock {
  PeakUwBlock *next;
  gsize bytes;
  PeakUwFrame frames[PEAK_UW_SLOTS];
};
struct PeakUwThread {
  PeakUwThread *next;
  PeakUwBlock *blocks;
  PeakUwFrame *free, *active;
  guint live;
  gpointer cfi_cache;
  PeakGumInterceptorThreadContext17 *gum_context;
  guint64 cookie, reap_epoch;
  gsize pthread_slot;
  guint64 pthread_generation;
  pthread_t thread;
  pid_t tid;
  gint retired, pthread_token_valid;
};
static _Thread_local PeakUwThread *peak_uw_thread;
static _Thread_local guint peak_uw_internal;
static pthread_key_t peak_uw_key;
static pthread_once_t peak_uw_once = PTHREAD_ONCE_INIT;
static gboolean peak_uw_key_ready;
static GMutex peak_uw_owners_lock;
static PeakUwThread *peak_uw_owners;
static guint64 peak_uw_next_cookie;
static gint peak_uw_task_fd = -1;
static void peak_uw_gum_context_destroy(gpointer data) {
  int saved_errno = errno;

  /* Clear every non-owning PEAK pointer before Gum removes and frees the
   * context. A later allocator reuse can therefore never make an old owner
   * inspect a new thread's invocation stack at the same address. */
  g_mutex_lock(&peak_uw_owners_lock);
  for (PeakUwThread *o = peak_uw_owners; o; o = o->next)
    if (o->gum_context == data)
      o->gum_context = NULL;
  g_mutex_unlock(&peak_uw_owners_lock);

  peak_gum_release_interceptor_thread_context_original(data);
  errno = saved_errno;
}
gboolean gum_interceptor_peak_unwind_lifecycle_init(void) {
  /* This is called before gum_init_embedded() and before PEAK installs any
   * hook. GPrivate.p becoming non-NULL means a cleanup record may already
   * have captured the original notify and is too late to wrap. */
  if (peak_gum_interceptor_context_private.p != NULL)
    return FALSE;
  if (peak_gum_interceptor_context_private.notify ==
      peak_uw_gum_context_destroy)
    return TRUE;
  if (peak_gum_interceptor_context_private.notify !=
      peak_gum_release_interceptor_thread_context_original)
    return FALSE;
  peak_gum_interceptor_context_private.notify = peak_uw_gum_context_destroy;
  return TRUE;
}
static void peak_uw_thread_destroy(void *data) {
  PeakUwThread *owner = data;
  /* Keep both owner and TLS pointer through every later application key
   * destructor, including re-entry after POSIX's final destructor pass. */
  g_atomic_int_set(&owner->retired, 1);
  if (peak_general_listener_controller_wake)
    peak_general_listener_controller_wake();
}
static void peak_uw_key_init(void) {
  peak_uw_key_ready =
      pthread_key_create(&peak_uw_key, peak_uw_thread_destroy) == 0;
  if (peak_uw_key_ready)
    peak_uw_task_fd =
        open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}
static void peak_uw_b(guint8 **p, guint8 v) { *(*p)++ = v; }
static void peak_uw_u32(guint8 **p, guint32 v) {
  memcpy(*p, &v, 4);
  *p += 4;
}
static void peak_uw_ptr(guint8 **p, guintptr v) {
  memcpy(*p, &v, 8);
  *p += 8;
}
static void peak_uw_finish(guint8 *start, guint8 **p) {
  while (((guintptr)*p & 3) != 0)
    peak_uw_b(p, 0);
  guint32 n = *p - start - 4;
  memcpy(start, &n, 4);
}
static void peak_uw_expr(guint8 **p, guint8 op, guint8 reg, gpointer addr,
                         gboolean plus_word) {
  peak_uw_b(p, op);
  if (op != 0x0f)
    peak_uw_b(p, reg);
  peak_uw_b(p, plus_word ? 12 : 10);
  peak_uw_b(p, 3);
  peak_uw_ptr(p, (guintptr)addr);
  peak_uw_b(p, 6);
  if (plus_word) {
    peak_uw_b(p, 0x23);
    peak_uw_b(p, 8);
  }
}
static void peak_uw_frame_cfi(PeakUwFrame *f) {
  guint8 *p = f->eh;
  peak_uw_u32(&p, 0);
  peak_uw_u32(&p, 0);
  peak_uw_b(&p, 1);
  peak_uw_b(&p, 'z');
  peak_uw_b(&p, 'R');
  peak_uw_b(&p, 0);
  peak_uw_b(&p, 1);
  peak_uw_b(&p, 0x78);
  peak_uw_b(&p, 16);
  peak_uw_b(&p, 1);
  peak_uw_b(&p, 0);
  peak_uw_b(&p, 0x0c);
  peak_uw_b(&p, 7);
  peak_uw_b(&p, 0);
  peak_uw_finish(f->eh, &p);
  guint8 *fde = p;
  peak_uw_u32(&p, 0);
  peak_uw_u32(&p, (guint32)(p - f->eh));
  peak_uw_ptr(&p, (guintptr)f->pc);
  peak_uw_ptr(&p, 16);
  peak_uw_b(&p, 0);
  peak_uw_expr(&p, 0x0f, 0, &f->slot, FALSE);
  peak_uw_expr(&p, 0x16, 16, &f->caller, FALSE);
  peak_uw_expr(&p, 0x16, 7, &f->slot, TRUE);
  peak_uw_finish(fde, &p);
  peak_uw_u32(&p, 0);
}
static void peak_uw_cache_clear(PeakUwThread *owner);
/* Called with peak_uw_owners_lock held after thread-death proof. */
static void peak_uw_settle_dead_gum_context(PeakUwThread *owner) {
  PeakGumInterceptorThreadContext17 *context = owner->gum_context;

  if (context == NULL)
    return;
  for (PeakUwThread *other = peak_uw_owners; other; other = other->next)
    if (other != owner && other->gum_context == context) {
      owner->gum_context = NULL;
      return;
    }

  gum_spinlock_acquire(&peak_gum_interceptor_thread_context_lock);
  if (peak_gum_interceptor_thread_contexts != NULL &&
      g_hash_table_contains(peak_gum_interceptor_thread_contexts, context)) {
    PeakGumInvocationStack17 *stack = context->stack;

    for (PeakUwFrame *f = owner->active; f != NULL; f = f->next)
      if (f->generic) {
        for (guint i = stack->len; i != 0; i--) {
          PeakGumInvocationStackEntry17 *entry =
              &g_array_index(stack, PeakGumInvocationStackEntry17, i - 1);
          if (entry->function_ctx == f->context &&
              entry->stack_address == f->slot) {
            g_atomic_int_dec_and_test(
                &entry->function_ctx->trampoline_usage_counter);
            g_array_remove_index(stack, i - 1);
          }
        }
      }
  }
  gum_spinlock_release(&peak_gum_interceptor_thread_context_lock);
  /* Gum's pending GPrivate notification remains the sole owner of context
   * lifetime. Freeing it here would leave that raw notification pointer able
   * to remove a later context allocated at the same address. The exact dead
   * entries above are gone, so Gum's eventual destructor neither repeats
   * their lease decrements nor retains the PEAK trampolines. */
  owner->gum_context = NULL;
}
static void peak_uw_owner_free(PeakUwThread *owner) {
  peak_uw_cache_clear(owner);
  for (PeakUwFrame *f = owner->active; f; f = f->next)
    g_atomic_int_dec_and_test(&f->context->trampoline_usage_counter);
  PeakUwBlock *b = owner->blocks;
  while (b) {
    PeakUwBlock *next = b->next;
    for (guint i = 0; i < PEAK_UW_SLOTS; i++)
      __deregister_frame(b->frames[i].eh);
    munmap(b, b->bytes);
    b = next;
  }
  g_free(owner);
}
guint64 gum_interceptor_peak_unwind_thread_cookie(guintptr native_thread) {
  guint64 cookie = 0;
  g_mutex_lock(&peak_uw_owners_lock);
  for (PeakUwThread *o = peak_uw_owners; o; o = o->next)
    if ((guintptr)o->thread == native_thread) {
      cookie = o->cookie;
      break;
    }
  g_mutex_unlock(&peak_uw_owners_lock);
  return cookie;
}
void gum_interceptor_peak_unwind_thread_joined(guint64 cookie,
                                               guintptr native_thread,
                                               gboolean token_valid,
                                               guint64 slot,
                                               guint64 generation) {
  PeakUwThread *dead = NULL;
  /* Frida defers GPrivate destruction until its garbage collector observes
   * the native thread's lifetime beacon. Prompt that collector now, but do
   * not treat its return as a completion barrier: it drops its queue lock
   * before running private-value destructors. Invocation-stack ownership and
   * synchronization remain entirely inside Frida; PEAK releases only its
   * separate atomic frame lease below. */
  (void)_frida_g_thread_garbage_collect();
  g_mutex_lock(&peak_uw_owners_lock);
  for (PeakUwThread **p = &peak_uw_owners; *p;) {
    PeakUwThread *o = *p;
    gboolean match = o->cookie == cookie && cookie != 0;
    if (!match && cookie == 0 &&
        (guintptr)o->thread == native_thread && token_valid &&
        o->pthread_token_valid &&
        o->pthread_slot == (gsize)slot &&
        o->pthread_generation == generation)
      match = TRUE;
    if (!match && cookie == 0 && (guintptr)o->thread == native_thread) {
      struct stat st;
      char name[32];
      g_snprintf(name, sizeof(name), "%ld", (long)o->tid);
      /* A pool owner may be created during POSIX's final destructor pass,
       * after this key's destructor has already been considered. Successful
       * pthread_join plus absence of the saved kernel TID proves that owner
       * dead even when its retired callback never ran. A concurrently reused
       * pthread_t has a different, still-present TID and is retained. */
      match = peak_uw_task_fd >= 0 &&
              fstatat(peak_uw_task_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 &&
              errno == ENOENT;
    }
    if (match) {
      peak_uw_settle_dead_gum_context(o);
      *p = o->next;
      o->next = dead;
      dead = o;
    } else
      p = &o->next;
  }
  g_mutex_unlock(&peak_uw_owners_lock);
  while (dead) {
    PeakUwThread *o = dead;
    dead = o->next;
    peak_uw_owner_free(o);
  }
}
gboolean gum_interceptor_peak_unwind_reap_threads(void) {
  if (peak_uw_task_fd < 0)
    return FALSE;
  /* Prompt cleanup of any eligible Frida GPrivate context. This is progress,
   * not a barrier; PEAK never reads or mutates the associated Gum stack. */
  (void)_frida_g_thread_garbage_collect();
  PeakUwThread *dead = NULL;
  g_mutex_lock(&peak_uw_owners_lock);
  for (PeakUwThread **p = &peak_uw_owners; *p;) {
    PeakUwThread *o = *p;
    struct stat st;
    char name[32];
    g_snprintf(name, sizeof(name), "%ld", (long)o->tid);
    /* TID absence is the death proof. retired is only an eager notification
     * and can be missing when the owner was first created in the final POSIX
     * destructor pass. */
    if (fstatat(peak_uw_task_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 &&
        errno == ENOENT) {
      peak_uw_settle_dead_gum_context(o);
      *p = o->next;
      o->next = dead;
      dead = o;
    } else
      p = &o->next;
  }
  g_mutex_unlock(&peak_uw_owners_lock);
  gboolean work = dead != NULL;
  while (dead) {
    PeakUwThread *o = dead;
    dead = o->next;
    peak_uw_owner_free(o);
  }
  return work;
}
void gum_interceptor_peak_unwind_diagnostics(guint64 *owners, guint64 *blocks,
                                             guint64 *active) {
  *owners = *blocks = *active = 0;
  g_mutex_lock(&peak_uw_owners_lock);
  for (PeakUwThread *o = peak_uw_owners; o; o = o->next) {
    (*owners)++;
    *active += __atomic_load_n(&o->live, __ATOMIC_RELAXED);
    for (PeakUwBlock *b = o->blocks; b; b = b->next)
      (*blocks)++;
  }
  g_mutex_unlock(&peak_uw_owners_lock);
}
static gboolean peak_uw_owner_ensure(void) {
  if (peak_uw_thread)
    return TRUE;
  pthread_once(&peak_uw_once, peak_uw_key_init);
  if (!peak_uw_key_ready)
    return FALSE;
  (void)gum_interceptor_peak_unwind_reap_threads();
  PeakUwThread *o = g_try_malloc0(sizeof(*o));
  if (!o)
    return FALSE;
  o->thread = pthread_self();
  o->tid = syscall(SYS_gettid);
  if (pthread_listener_current_thread_token != NULL)
    o->pthread_token_valid = pthread_listener_current_thread_token(
        &o->pthread_slot, &o->pthread_generation);
  if (pthread_setspecific(peak_uw_key, o) != 0) {
    g_free(o);
    return FALSE;
  }
  g_mutex_lock(&peak_uw_owners_lock);
  o->cookie = ++peak_uw_next_cookie;
  o->next = peak_uw_owners;
  peak_uw_owners = o;
  g_mutex_unlock(&peak_uw_owners_lock);
  peak_uw_thread = o;
  return TRUE;
}
static void peak_uw_bind_gum_context(void) {
  PeakGumInterceptorThreadContext17 *context;

  if (peak_uw_thread == NULL)
    return;
  /* Gum's getter may run its deferred GPrivate collector. Do not hold the
   * owners lock across it: our wrapped notify also takes that lock before
   * freeing an old context. The wrapper clears every stale pointer before
   * allocator reuse, and the returned current-thread context remains live. */
  context = peak_gum_get_interceptor_thread_context();
  g_mutex_lock(&peak_uw_owners_lock);
  if (peak_uw_thread->gum_context == NULL)
    peak_uw_thread->gum_context = context;
  g_mutex_unlock(&peak_uw_owners_lock);
}
static void peak_uw_release(PeakUwFrame **link);
typedef struct {
  PeakUwThread *owner;
  guint64 epoch;
  gboolean terminal;
  PeakGumInvocationStack17 *gum_stack;
  guint8 *gum_live;
} PeakUwWalk;
static gpointer peak_uw_canonical_address(PeakUwThread *owner,
                                          gpointer caller) {
  if (!owner)
    return caller;
  for (guint n = 0; n < owner->live; n++) {
    PeakUwFrame *match = NULL;
    for (PeakUwFrame *f = owner->active; f; f = f->next)
      if (caller == f->pc + 1) {
        match = f;
        break;
      }
    if (!match)
      break;
    caller = match->caller;
  }
  return caller;
}
static gpointer peak_uw_canonical_caller(PeakUwThread *owner,
                                         PeakUwFrame *frame) {
  return peak_uw_canonical_address(owner, frame->caller);
}
static _Unwind_Reason_Code peak_uw_walk_frame(struct _Unwind_Context *context,
                                              void *data) {
  PeakUwWalk *walk = data;
  int before = 0;
  guintptr ip = _Unwind_GetIPInfo(context, &before);
  if (ip == 0) {
    walk->terminal = TRUE;
    return _URC_NO_REASON;
  }
  guintptr cfa = _Unwind_GetCFA(context);
  for (PeakUwFrame *f = walk->owner ? walk->owner->active : NULL; f;
       f = f->next) {
    guintptr caller = peak_invoke_normalize_address(
        (guintptr)peak_uw_canonical_caller(walk->owner, f));
    if (cfa == (guintptr)f->slot + sizeof(gpointer) && ip == caller && !before)
      f->walk_live_epoch = walk->epoch;
  }
  if (walk->gum_stack)
    for (guint i = 0; i < walk->gum_stack->len; i++) {
      PeakGumInvocationStackEntry17 *entry =
          &g_array_index(walk->gum_stack, PeakGumInvocationStackEntry17, i);
      guintptr caller =
          peak_invoke_normalize_address((guintptr)peak_uw_canonical_address(
              walk->owner, entry->caller_ret_addr));
      if (cfa == (guintptr)entry->stack_address + sizeof(gpointer) &&
          ip == caller && !before)
        walk->gum_live[i] = 1;
    }
  return _URC_NO_REASON;
}
gboolean gum_interceptor_peak_unwind_reconcile_current_thread(void) {
  PeakUwThread *owner = peak_uw_thread;
  PeakGumInvocationStack17 *gum_stack = peak_gum_invocation_stack();
  guint gum_count = gum_stack ? gum_stack->len : 0;
  if ((!owner || !owner->active) && gum_count == 0)
    return TRUE;
  guint8 *gum_live = gum_count ? g_try_malloc0(gum_count) : NULL;
  if (gum_count && gum_live == NULL)
    return FALSE;
  guint64 epoch = owner ? ++owner->reap_epoch : 0;
  PeakUwWalk walk = {owner, epoch, FALSE, gum_stack, gum_live};
  peak_uw_internal++;
  _Unwind_Reason_Code result = _Unwind_Backtrace(peak_uw_walk_frame, &walk);
  peak_uw_internal--;
  if (result != _URC_END_OF_STACK || !walk.terminal) {
    g_free(gum_live);
    return FALSE;
  }
  if (owner)
    for (PeakUwFrame **p = &owner->active; *p;) {
      PeakUwFrame *f = *p;
      if (f->walk_live_epoch == epoch) {
        p = &f->next;
        continue;
      }
      f->reaped_epoch = epoch;
      f->reaped_context = f->context;
      f->reaped_slot = f->slot;
      peak_uw_release(p);
    }
  for (guint i = gum_count; i > 0; i--)
    if (!gum_live[i - 1]) {
      PeakGumInvocationStackEntry17 *entry =
          &g_array_index(gum_stack, PeakGumInvocationStackEntry17, i - 1);
      g_atomic_int_dec_and_test(&entry->function_ctx->trampoline_usage_counter);
      g_array_remove_index(gum_stack, i - 1);
    }
  g_free(gum_live);
  return TRUE;
}
static gboolean peak_uw_grow(void) {
#if defined(PEAK_UW_TEST_FAIL_GROW)
  return FALSE;
#endif
  gsize page = (gsize)sysconf(_SC_PAGESIZE);
  if (!page || (page & (page - 1)))
    return FALSE;
  gsize data = (sizeof(PeakUwBlock) + page - 1) & ~(page - 1),
        bytes = data + page;
  PeakUwBlock *b = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (b == MAP_FAILED)
    return FALSE;
  b->bytes = bytes;
  guint8 *code = (guint8 *)b + data;
  for (guint i = 0; i < PEAK_UW_SLOTS; i++) {
    PeakUwFrame *f = &b->frames[i];
    f->pc = code + i * 16;
    f->pc[0] = 0x90;
    f->pc[1] = 0xff;
    f->pc[2] = 0x25;
    gint32 disp = (guint8 *)&f->destination - (f->pc + 7);
    memcpy(f->pc + 3, &disp, 4);
    peak_uw_frame_cfi(f);
  }
  if (mprotect(code, page, PROT_READ | PROT_EXEC) != 0) {
    munmap(b, bytes);
    return FALSE;
  }
  /* Cancellation is disabled by prepare until every FDE has an owner. */
  for (guint i = 0; i < PEAK_UW_SLOTS; i++)
    __register_frame(b->frames[i].eh);
  g_mutex_lock(&peak_uw_owners_lock);
  b->next = peak_uw_thread->blocks;
  peak_uw_thread->blocks = b;
  g_mutex_unlock(&peak_uw_owners_lock);
  for (guint i = 0; i < PEAK_UW_SLOTS; i++) {
    PeakUwFrame *f = &b->frames[i];
    f->free_next = peak_uw_thread->free;
    peak_uw_thread->free = f;
  }
  return TRUE;
}
static void peak_uw_release(PeakUwFrame **link) {
  PeakUwFrame *f = *link;
  *link = f->next;
  g_atomic_int_dec_and_test(&f->context->trampoline_usage_counter);
  f->slot = NULL;
  f->context = NULL;
  f->free_next = peak_uw_thread->free;
  peak_uw_thread->free = f;
  if (__atomic_sub_fetch(&peak_uw_thread->live, 1, __ATOMIC_RELAXED) == 0)
    peak_uw_cache_clear(peak_uw_thread);
}
static void peak_uw_reap(gpointer *slot) {
  if (!peak_uw_thread)
    return;
  ++peak_uw_thread->reap_epoch;
  while (peak_uw_thread->active) {
    PeakUwFrame *f = peak_uw_thread->active;
    if ((guint8 *)f->slot > (guint8 *)slot ||
        (f->slot == slot && *slot == f->pc + 1))
      break;
    f->reaped_epoch = peak_uw_thread->reap_epoch;
    f->reaped_context = f->context;
    f->reaped_slot = f->slot;
    peak_uw_release(&peak_uw_thread->active);
  }
}
gboolean gum_interceptor_peak_unwind_frame_is_live(gpointer identity,
                                                   gpointer slot) {
  if (!peak_uw_thread)
    return TRUE; /* No provenance: retain Gum's equality rule. */
  for (PeakUwFrame *f = peak_uw_thread->active; f; f = f->next)
    if (f->slot == slot &&
        (f->context == identity || f->context->function_address == identity))
      return TRUE;
  for (PeakUwBlock *b = peak_uw_thread->blocks; b; b = b->next)
    for (guint i = 0; i < PEAK_UW_SLOTS; i++) {
      PeakUwFrame *f = &b->frames[i];
      if (f->reaped_epoch == peak_uw_thread->reap_epoch &&
          f->reaped_context == identity && f->reaped_slot == slot)
        return FALSE;
    }
  return TRUE;
}
static PeakUwFrame *peak_uw_prepare(void) {
  if (!peak_uw_thread || !peak_uw_thread->free) {
    int saved_errno = errno, old_cancel;
    peak_uw_internal++;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);
    gboolean ok =
        peak_uw_owner_ensure() && (peak_uw_thread->free || peak_uw_grow());
    peak_uw_internal--;
    pthread_setcancelstate(old_cancel, NULL);
    errno = saved_errno;
    if (!ok)
      return NULL;
  }
  PeakUwFrame *f = peak_uw_thread->free;
  peak_uw_thread->free = f->free_next;
  return f;
}
static void peak_uw_cancel(PeakUwFrame *f) {
  if (f) {
    f->free_next = peak_uw_thread->free;
    peak_uw_thread->free = f;
  }
}
static void peak_uw_enter(PeakUwFrame *f, PeakGumFunctionContext17 *context,
                          gpointer *slot, gpointer original, gboolean observed,
                          gboolean generic) {
  if (!f)
    return;
  f->context = context;
  f->slot = slot;
  f->caller = original;
  f->observed = observed;
  f->generic = generic;
  f->destination = observed ? *slot : context->on_leave_trampoline;
  g_atomic_int_inc(&context->trampoline_usage_counter);
  f->next = peak_uw_thread->active;
  peak_uw_thread->active = f;
  __atomic_add_fetch(&peak_uw_thread->live, 1, __ATOMIC_RELAXED);
  __atomic_signal_fence(__ATOMIC_RELEASE);
  *slot = f->pc + 1;
}
static gboolean peak_uw_leave(gpointer context, gpointer stack,
                              gpointer *caller) {
  if (!peak_uw_thread)
    return FALSE;
  for (PeakUwFrame **p = &peak_uw_thread->active; *p; p = &(*p)->next)
    if ((*p)->context == context && (*p)->slot == stack) {
      gboolean plain = !(*p)->observed;
      *caller = (*p)->caller;
      peak_uw_release(p);
      return plain;
    }
  return FALSE;
}
#else
gboolean gum_interceptor_peak_unwind_lifecycle_init(void) { return TRUE; }
gboolean gum_interceptor_peak_unwind_frame_is_live(gpointer identity,
                                                   gpointer slot) {
  (void)identity;
  (void)slot;
  return TRUE;
}
gboolean gum_interceptor_peak_unwind_reconcile_current_thread(void) {
  return TRUE;
}
typedef void PeakUwFrame;
static guint peak_uw_internal;
static PeakUwFrame *peak_uw_prepare(void) { return NULL; }
static void peak_uw_bind_gum_context(void) {}
static void peak_uw_cancel(PeakUwFrame *f) { (void)f; }
static void peak_uw_reap(gpointer *slot) { (void)slot; }
static void peak_uw_enter(PeakUwFrame *f, gpointer ctx, gpointer *slot,
                          gpointer original, gboolean observed,
                          gboolean generic) {
  (void)f;
  (void)ctx;
  (void)slot;
  (void)original;
  (void)observed;
  (void)generic;
}
static gboolean peak_uw_leave(gpointer ctx, gpointer stack, gpointer *caller) {
  (void)ctx;
  (void)stack;
  (void)caller;
  return FALSE;
}
guint64 gum_interceptor_peak_unwind_thread_cookie(guintptr thread) {
  (void)thread;
  return 0;
}
void gum_interceptor_peak_unwind_thread_joined(guint64 cookie,
                                               guintptr thread,
                                               gboolean token_valid,
                                               guint64 slot,
                                               guint64 generation) {
  (void)cookie;
  (void)thread;
  (void)token_valid;
  (void)slot;
  (void)generation;
}
gboolean gum_interceptor_peak_unwind_reap_threads(void) { return FALSE; }
void gum_interceptor_peak_unwind_diagnostics(guint64 *o, guint64 *b,
                                             guint64 *a) {
  *o = *b = *a = 0;
}
#endif

#define _GNU_SOURCE
#include "general_listener.h"
#include "pthread_listener.h"
#include <stdatomic.h>

#define PEAK_SIG_STOP (SIGRTMIN + 0)
#define PEAK_SIG_CONT (SIGRTMIN + 1)

GumInterceptor* interceptor;
GumInvocationListener** array_listener;
static gboolean* array_listener_detached;
static gboolean* array_listener_reattached;
extern gboolean* peak_need_detach;
extern gboolean* peak_detached;
extern PeakHeartbeatArgs* args;
extern gdouble* heartbeat_overhead;
extern gboolean** peak_target_thread_called;
extern unsigned int check_interval;
gpointer* hook_address = NULL;
static double peak_general_overhead;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;
char** peak_demangled_strings;
extern gulong peak_max_num_threads;
extern double peak_main_time;
extern float peak_detach_cost;
extern float target_profile_ratio;
extern float global_target_ratio;
extern float peak_global_reattach_factor;
extern float peak_global_detach_factor;
extern bool enable_per_target_heartbeat;
extern bool enable_global_heartbeat;
extern unsigned long long sig_cont_wait_interval;
extern unsigned int sig_stop_ack_wait_interval;
extern unsigned int heartbeat_time;
static gulong peak_detach_count = G_MAXULONG;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

volatile gboolean heartbeat_running = true;
static void peak_general_listener_iface_init(gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_EXTENDED(PeakGeneralListener,
                       peak_general_listener,
                       G_TYPE_OBJECT,
                       0,
                       G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                                             peak_general_listener_iface_init))
typedef struct _PeakGeneralThreadState {
    gulong level;
    gulong capacity;
    gdouble* child_time;
    pthread_t* tid_keys;
    size_t* mapped_ids;
    int* pause_session_ids;
    int* pause_status;
    size_t self_mapped_id;
    gboolean self_mapped_known;
} PeakGeneralThreadState;

typedef struct _PeakInvocationData {
    gdouble start_time;
    gboolean initialized;
} PeakInvocationData;

static __thread PeakGeneralThreadState thread_data;

static sem_t pthread_pause_sem;
pthread_once_t pthread_pause_once_ctrl = PTHREAD_ONCE_INIT;
static __thread int last_cont_id = -1;
static _Atomic int global_session_counter = 0;

void pthread_pause_handler(int signal, siginfo_t* info, void* context)
{
    (void)context;
    int session_id = 0;
    if (info != NULL) {
        session_id = info->si_value.sival_int;
    }

    if (signal == PEAK_SIG_STOP) {
        sem_post(&pthread_pause_sem);

        if (last_cont_id >= session_id) {
            return;
        }

        sigset_t block_set, original_mask, wait_set;

        sigemptyset(&block_set);
        sigaddset(&block_set, PEAK_SIG_CONT);
        pthread_sigmask(SIG_BLOCK, &block_set, &original_mask);

        if (last_cont_id >= session_id) {
            pthread_sigmask(SIG_SETMASK, &original_mask, NULL);
            return;
        }

        sigemptyset(&wait_set);
        sigaddset(&wait_set, PEAK_SIG_CONT);

        for (;;) {
            siginfo_t cont_info;
            struct timespec timeout;
            int ret;

            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += sig_cont_wait_interval / 1000000000;
            timeout.tv_nsec += sig_cont_wait_interval % 1000000000;

            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec += 1;
                timeout.tv_nsec -= 1000000000;
            }

            ret = sigtimedwait(&wait_set, &cont_info, &timeout);
            if (ret == -1) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if (ret == PEAK_SIG_CONT) {
                int cont_id = cont_info.si_value.sival_int;

                if (cont_id > last_cont_id) {
                    last_cont_id = cont_id;
                }

                if (cont_id >= session_id) {
                    break;
                }
            }
        }

        pthread_sigmask(SIG_SETMASK, &original_mask, NULL);
    } else if (signal == PEAK_SIG_CONT) {
        if (session_id > last_cont_id) {
            last_cont_id = session_id;
        }
    } else {
        return;
    }
}

void pthread_pause_once(void)
{
    sem_init(&pthread_pause_sem, 0, 0);

    // Prepare sigset
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);   

    //Register signal handlers
    //We now use sigaction() instead of signal(), because it supports SA_RESTART
    const struct sigaction pause_sa = {
        .sa_sigaction = pthread_pause_handler,
        .sa_mask = sigset,
        .sa_flags = SA_RESTART | SA_SIGINFO,
        .sa_restorer = NULL
    };
    sigaction(PEAK_SIG_STOP, &pause_sa, NULL);
    sigaction(PEAK_SIG_CONT, &pause_sa, NULL);  
}

#define pthread_pause_init() (pthread_once(&pthread_pause_once_ctrl, &pthread_pause_once))

void pthread_pause_enable()
{
    pthread_pause_init();
    //Prepare sigset
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);
    //UnBlock signals
    pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
}

void pthread_pause_disable()
{
    //This is important for when you want to do some signal unsafe stuff
    //Eg.: locking mutex, calling printf() which has internal mutex, etc...
    //After unlocking mutex, you can enable pause again.
    pthread_pause_init();

    //Block signals
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);
}

static int pthread_pause_mapped(pthread_t thread, size_t mapped_id, int* session_id_out)
{
    union sigval sv;
    int session_id = atomic_fetch_add(&global_session_counter, 1);

    if (session_id_out != NULL) {
        *session_id_out = -1;
    }

    if (mapped_id >= peak_max_num_threads) {
        return -1;
    }

    sv.sival_int = session_id;

    while (pthread_sigqueue(thread, PEAK_SIG_STOP, sv) == -1) {
        if (errno != EAGAIN) {
            return -1;
        }
        usleep(1000);
    }

    if (session_id_out != NULL) {
        *session_id_out = session_id;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += sig_stop_ack_wait_interval / 1000000000;
    ts.tv_nsec += sig_stop_ack_wait_interval % 1000000000;

    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }

    for (;;) {
        if (sem_timedwait(&pthread_pause_sem, &ts) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ETIMEDOUT) {
            return 1;
        }
        return -1;
    }
}

int pthread_pause(pthread_t thread, int* session_id_out)
{
    gboolean mapped_found = FALSE;
    size_t mapped_id = pthread_listener_lookup_thread(thread, &mapped_found);

    if (!mapped_found || mapped_id >= peak_max_num_threads) {
        if (session_id_out != NULL) {
            *session_id_out = -1;
        }
        return -1;
    }

    return pthread_pause_mapped(thread, mapped_id, session_id_out);
}

int pthread_unpause(pthread_t thread, int session_id)
{
    union sigval sv;
    sv.sival_int = session_id;

    while (pthread_sigqueue(thread, PEAK_SIG_CONT, sv) == -1) {
        if (errno != EAGAIN) {
            return -1;
        }
        usleep(1000);
    }

    return 0;
}

typedef struct {
    size_t index;
    double ratio;  // hard-gate ratio
    double rate;   // for global ordering only
} OverheadEntry;

// rate descending (global detach)
static int compare_rate_de(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->rate < y->rate) return 1;
    if (x->rate > y->rate) return -1;

    // tie-break: larger ratio first
    if (x->ratio < y->ratio) return 1;
    if (x->ratio > y->ratio) return -1;
    return 0;
}

// rate ascending (global reattach)
static int compare_rate_inc(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->rate < y->rate) return -1;
    if (x->rate > y->rate) return 1;

    // tie-break: smaller ratio first
    if (x->ratio < y->ratio) return -1;
    if (x->ratio > y->ratio) return 1;
    return 0;
}

static inline double clipd(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

void* peak_heartbeat_monitor(void* arg) {
    unsigned int heartbeat_time = args->heartbeat_time;   // base sleep (us)
    unsigned int check_interval = args->check_interval;

    const unsigned int HB_MIN_US   = 10000;     // hard min sleep
    const unsigned int HB_MAX_US   = 500000;    // hard max sleep
    const double       HB_K_ERR    = 3.0;       // sensitivity to overshoot
    const double       HB_K_RATE   = 0.8;       // sensitivity to growth rate
    const double       HB_EMA_A    = 0.3;       // EMA alpha in (0,1]
    unsigned int heartbeat_counter = 0;

    gum_interceptor_ignore_current_thread(interceptor);
    pthread_t my_tid = pthread_self();

    OverheadEntry* entries = g_new0(OverheadEntry, peak_hook_address_count);

    double* ratio_snapshot = g_new0(double, peak_hook_address_count);
    double* rate_snapshot  = g_new0(double, peak_hook_address_count);
    double* prev_ratio     = g_new0(double, peak_hook_address_count);
    double* prev_time      = g_new0(double, peak_hook_address_count);
    pthread_t* hb_tid_keys = g_new0(pthread_t, peak_max_num_threads);
    size_t* hb_mapped_ids = g_new0(size_t, peak_max_num_threads);
    int* hb_pause_session_ids = g_new0(int, peak_max_num_threads);
    int* hb_pause_status = g_new0(int, peak_max_num_threads);

    double now0 = peak_second();
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        ratio_snapshot[i] = 0.0;
        rate_snapshot[i]  = 0.0;
        prev_ratio[i]     = 0.0;
        prev_time[i]      = now0;
    }

    // ------------------------------
    // Global dynamics state
    // ------------------------------
    double prev_global_overhead = 0.0;
    double prev_global_time     = now0;
    double ema_global_rate      = 0.0;

    while (heartbeat_running) {
        heartbeat_counter++;
        double now = peak_second();

        double total_execution_time = now - peak_main_time;
        if (total_execution_time <= 0.0) total_execution_time = 1e-12;

        double global_overhead = 0.0;

        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (!(hook_address[i] && array_listener[i])) {
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i]  = 0.0;
                prev_ratio[i]     = 0.0;
                prev_time[i]      = now;
                continue;
            }

            PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(array_listener[i]);

            gulong total_num_calls = 0;
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                total_num_calls += pg_listener->num_calls[j];
            }

            double ratio =
                (total_num_calls * peak_general_overhead + heartbeat_overhead[i]) / total_execution_time;

            ratio_snapshot[i] = ratio;
            // g_printerr ("ratio %ld: %ld\n", i, total_num_calls);

            double dt = now - prev_time[i];
            if (dt <= 1e-12) dt = 1e-12;
            rate_snapshot[i] = (ratio - prev_ratio[i]) / dt;

            prev_ratio[i] = ratio;
            prev_time[i]  = now;

            if (!peak_detached[i]) {
                global_overhead += ratio;
            }
        }

        // g_printerr ("global_overhead %.3e\n", global_overhead);

        // ------------------------------------------------------------
        // 1) Per-target DETACH
        // ------------------------------------------------------------
        if (enable_per_target_heartbeat) {
            pthread_mutex_lock(&lock);
            for (size_t i = 0; i < peak_hook_address_count; i++) {
                if (!(hook_address[i] && array_listener[i])) continue;
                if (peak_detached[i]) continue;

                if (ratio_snapshot[i] > target_profile_ratio) {
                    peak_need_detach[i] = TRUE;
                }
            }
             pthread_mutex_unlock(&lock);
        }

        // ------------------------------------------------------------
        // 2) Global DETACH
        // ------------------------------------------------------------
        if (enable_global_heartbeat) {
            if (global_overhead > global_target_ratio * peak_global_detach_factor) {
                size_t n_attached = 0;
                for (size_t i = 0; i < peak_hook_address_count; i++) {
                    if (!(hook_address[i] && array_listener[i])) continue;
                    if (peak_detached[i]) continue;

                    entries[n_attached].index = i;
                    entries[n_attached].ratio = ratio_snapshot[i];
                    entries[n_attached].rate  = rate_snapshot[i];
                    n_attached++;
                }

                if (n_attached > 1) {
                    qsort(entries, n_attached, sizeof(OverheadEntry), compare_rate_de);
                }

                double reduced = global_overhead;
                pthread_mutex_lock(&lock);
                for (size_t k = 0; k < n_attached && reduced > global_target_ratio; k++) {
                    size_t idx = entries[k].index;

                    if (!(hook_address[idx] && array_listener[idx])) continue;
                    if (peak_detached[idx]) continue;

                    reduced -= entries[k].ratio;
                    peak_need_detach[idx] = TRUE;
                }
                pthread_mutex_unlock(&lock);
            }
        }

        // ------------------------------------------------------------
        // 3) Reattach
        // ------------------------------------------------------------
        if (check_interval != 0 && (heartbeat_counter % check_interval) == 0) {
            // Per-target REATTACH
            if (enable_per_target_heartbeat) {
                for (size_t i = 0; i < peak_hook_address_count; i++) {
                    if (!(hook_address[i] && array_listener[i])) continue;
                    if (!peak_detached[i]) continue;
                    if (peak_need_detach[i]) continue;

                    if (ratio_snapshot[i] <= target_profile_ratio) {
                        double start_attach_time = peak_second();
                        gboolean did_attach = FALSE;
                        size_t snapshot_cap = peak_max_num_threads;
                        size_t snapshot_count =
                            pthread_listener_snapshot_threads(hb_tid_keys, hb_mapped_ids, snapshot_cap);
                        for (size_t s = 0; s < snapshot_count; s++) {
                            hb_pause_session_ids[s] = -1;
                            hb_pause_status[s] = -1;
                        }
                        // double end_pthread = peak_second();
                        // g_printerr ("end_pthread per %.3e\n", end_pthread - start_attach_time);
                        pthread_mutex_lock(&lock);
                        // double pthread_mutex_lock_time = peak_second();
                        // g_printerr ("pthread_mutex_lock_time per %.3e\n", pthread_mutex_lock_time - end_pthread);


                        if (peak_detached[i] && !peak_need_detach[i]) {
                            //  double start_pause = peak_second();
                            // g_printerr ("start_pause per\n");

                            for (size_t s = 0; s < snapshot_count; s++) {
                               
                                pthread_t tid_key = hb_tid_keys[s];
                                size_t mapped = hb_mapped_ids[s];
                                if (mapped < peak_max_num_threads &&
                                    tid_key != my_tid &&
                                    peak_target_thread_called[i][mapped]) {
                                    hb_pause_status[s] =
                                        pthread_pause_mapped(tid_key, mapped, &hb_pause_session_ids[s]);
                                }
                            }
                            // double end_pause = peak_second();
                            // g_printerr ("end_pause per %.3e\n", end_pause - start_pause);


                            gum_interceptor_begin_transaction(interceptor);
                            gum_interceptor_attach(interceptor, hook_address[i], array_listener[i], NULL);
                            gum_interceptor_end_transaction(interceptor);

                            //  double start_unpause = peak_second();
                            // g_printerr ("start_unpause per %.3e\n", start_unpause - end_pause);

                            for (size_t s = 0; s < snapshot_count; s++) {
                                pthread_t tid_key = hb_tid_keys[s];
                                size_t mapped = hb_mapped_ids[s];
                                if (mapped < peak_max_num_threads &&
                                    tid_key != my_tid &&
                                    peak_target_thread_called[i][mapped] &&
                                    (hb_pause_status[s] == 0 || hb_pause_status[s] == 1) &&
                                    hb_pause_session_ids[s] >= 0) {
                                    pthread_unpause(tid_key, hb_pause_session_ids[s]);
                                }
                            }
                            // double end_unpause = peak_second();
                            // g_printerr ("end_unpause per %.3e\n", end_unpause - start_unpause);


                            peak_need_detach[i] = FALSE;
                            peak_detached[i] = FALSE;
                            array_listener_reattached[i] = TRUE;
                            // g_printerr ("Per-target REATTACH\n");

                            did_attach = TRUE;
                        }

                        pthread_mutex_unlock(&lock);
                        // g_printerr ("pthread_mutex_unlock per %.3e\n",  peak_second() - start_attach_time);

                        if (!did_attach) {
                            continue;
                        }

                        double end_attach_time = peak_second();
                        heartbeat_overhead[i] += end_attach_time - start_attach_time;

                        PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(array_listener[i]);
                        gulong total_num_calls = 0;
                        for (size_t j = 0; j < peak_max_num_threads; j++) {
                            total_num_calls += pg_listener->num_calls[j];
                        }

                        double exec_at_attach = end_attach_time - peak_main_time;
                        if (exec_at_attach <= 0.0) exec_at_attach = 1e-12;

                        double ratio_at_attach =
                            (total_num_calls * peak_general_overhead + heartbeat_overhead[i]) / exec_at_attach;

                        ratio_snapshot[i] = ratio_at_attach;
                        prev_time[i] = end_attach_time;
                        prev_ratio[i] = ratio_at_attach;
                        rate_snapshot[i] = 0.0;

                        global_overhead += ratio_at_attach;
                    }
                }
            }
            // Global REATTACH
            if (enable_global_heartbeat) {
                if (global_overhead <= peak_global_reattach_factor * global_target_ratio) {
                    size_t detached_cnt = 0;
                    for (size_t i = 0; i < peak_hook_address_count; i++) {
                        if (!(hook_address[i] && array_listener[i])) continue;
                        if (!peak_detached[i]) continue;
                        if (peak_need_detach[i]) continue;

                        entries[detached_cnt].index = i;
                        entries[detached_cnt].ratio = ratio_snapshot[i];
                        entries[detached_cnt].rate  = rate_snapshot[i];
                        detached_cnt++;
                    }

                    if (detached_cnt > 1) {
                        qsort(entries, detached_cnt, sizeof(OverheadEntry), compare_rate_inc);
                    }

                    for (size_t k = 0; k < detached_cnt; k++) {
                        size_t i = entries[k].index;

                        if (!peak_detached[i]) continue;

                        if (global_overhead + entries[k].ratio > global_target_ratio) {
                            break;
                        }

                        double start_attach_time = peak_second();
                        gboolean did_attach = FALSE;
                        size_t snapshot_cap = peak_max_num_threads;
                        size_t snapshot_count =
                            pthread_listener_snapshot_threads(hb_tid_keys, hb_mapped_ids, snapshot_cap);
                        for (size_t s = 0; s < snapshot_count; s++) {
                            hb_pause_session_ids[s] = -1;
                            hb_pause_status[s] = -1;
                        }
                        // double end_pthread = peak_second();
                        // g_printerr ("end_pthread Global %.3e\n", end_pthread - start_attach_time);
                        pthread_mutex_lock(&lock);
                        // double pthread_mutex_lock_time = peak_second();
                        // g_printerr ("pthread_mutex_lock_time Global %.3e\n", pthread_mutex_lock_time - end_pthread);

                        if (peak_detached[i] && !peak_need_detach[i]) {
                            // double start_pause = peak_second();
                            // g_printerr ("start_pause Global\n");

                            for (size_t s = 0; s < snapshot_count; s++) {
                                pthread_t tid_key = hb_tid_keys[s];
                                size_t mapped = hb_mapped_ids[s];
                                if (mapped < peak_max_num_threads &&
                                    tid_key != my_tid &&
                                    peak_target_thread_called[i][mapped]) {
                                    hb_pause_status[s] =
                                        pthread_pause_mapped(tid_key, mapped, &hb_pause_session_ids[s]);
                                }
                            }
                            // double end_pause = peak_second();
                            // g_printerr ("end_pause Global %.3e\n", end_pause - start_pause);


                            gum_interceptor_begin_transaction(interceptor);
                            gum_interceptor_attach(interceptor, hook_address[i], array_listener[i], NULL);
                            gum_interceptor_end_transaction(interceptor);

                            // double start_unpause = peak_second();
                            // g_printerr ("start_unpause Global %.3e\n", start_unpause - end_pause);

                            for (size_t s = 0; s < snapshot_count; s++) {
                                pthread_t tid_key = hb_tid_keys[s];
                                size_t mapped = hb_mapped_ids[s];
                                if (mapped < peak_max_num_threads &&
                                    tid_key != my_tid &&
                                    peak_target_thread_called[i][mapped] &&
                                    (hb_pause_status[s] == 0 || hb_pause_status[s] == 1) &&
                                    hb_pause_session_ids[s] >= 0) {
                                    pthread_unpause(tid_key, hb_pause_session_ids[s]);
                                }
                            }

                            // double end_unpause = peak_second();
                            // g_printerr ("end_unpause Global %.3e\n", end_unpause - start_unpause);


                            peak_need_detach[i] = FALSE;
                            peak_detached[i] = FALSE;
                            array_listener_reattached[i] = TRUE;
                            // g_printerr ("Global REATTACH\n");

                            did_attach = TRUE;
                        }

                        pthread_mutex_unlock(&lock);
                        // g_printerr ("pthread_mutex_unlock Global %.3e\n",  peak_second() - start_attach_time);

                        if (!did_attach) {
                            continue;
                        }

                        double end_attach_time = peak_second();
                        heartbeat_overhead[i] += end_attach_time - start_attach_time;

                        PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(array_listener[i]);
                        gulong total_num_calls = 0;
                        for (size_t j = 0; j < peak_max_num_threads; j++) {
                            total_num_calls += pg_listener->num_calls[j];
                        }

                        double exec_at_attach = end_attach_time - peak_main_time;
                        if (exec_at_attach <= 0.0) exec_at_attach = 1e-12;

                        double ratio_at_attach =
                            (total_num_calls * peak_general_overhead + heartbeat_overhead[i]) / exec_at_attach;

                        ratio_snapshot[i] = ratio_at_attach;
                        prev_time[i] = end_attach_time;
                        prev_ratio[i] = ratio_at_attach;
                        rate_snapshot[i] = 0.0;

                        global_overhead += ratio_at_attach;

                        if (global_overhead > peak_global_reattach_factor * global_target_ratio) {
                            break;
                        }
                    }
                }
            }
        }

        // ------------------------------------------------------------
        // Adaptive heartbeat sleep
        // ------------------------------------------------------------
        double gdt = now - prev_global_time;
        if (gdt <= 1e-12) gdt = 1e-12;

        double global_rate = (global_overhead - prev_global_overhead) / gdt;
        ema_global_rate = HB_EMA_A * global_rate + (1.0 - HB_EMA_A) * ema_global_rate;

        prev_global_overhead = global_overhead;
        prev_global_time     = now;

        // error: how much we exceed global target (normalized)
        double err = (global_target_ratio > 0.0) ? (global_overhead / global_target_ratio - 1.0) : 0.0;
        if (err < 0.0) err = 0.0;

        // // only care positive growth (shrinking shouldn't speed up)
        // double pos_rate = (ema_global_rate > 0.0) ? ema_global_rate : 0.0;

        // scale factor: faster when err/rate bigger
        double scale = 1.0 / (1.0 + HB_K_ERR * err + HB_K_RATE * ema_global_rate);

        double sleep_us = clipd((double)heartbeat_time * scale, (double)HB_MIN_US, (double)HB_MAX_US);

        usleep(sleep_us);
    }

    g_free(prev_time);
    g_free(prev_ratio);
    g_free(rate_snapshot);
    g_free(ratio_snapshot);
    g_free(hb_pause_status);
    g_free(hb_pause_session_ids);
    g_free(hb_mapped_ids);
    g_free(hb_tid_keys);
    g_free(entries);

    gum_interceptor_unignore_current_thread(interceptor);
    return NULL;
}

static void
peak_general_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    if (!listener || g_object_is_floating(listener)) {
            return;
    }
    gum_interceptor_ignore_current_thread(interceptor);
    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    pthread_t my_tid = pthread_self();
    gboolean mapped_found = FALSE;
    size_t mapped_tid = pthread_listener_lookup_thread(my_tid, &mapped_found);
    if (!mapped_found || mapped_tid >= peak_max_num_threads) {
        mapped_tid = 0;
    }
    thread_data.self_mapped_id = mapped_tid;
    thread_data.self_mapped_known = mapped_found && mapped_tid < peak_max_num_threads;
    if (peak_detach_cost == 0 && heartbeat_time == 0) {
        // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
        // g_print ("hook_id %lu max %lu tid %lu ncall %p \n", hook_id, peak_max_num_threads, mapped_tid, self->num_calls);
        size_t index = mapped_tid;
        if (thread_data.child_time == NULL) {
            thread_data.level = 0;
            thread_data.capacity = 16;
            thread_data.child_time = g_new(gdouble, 16);
        }
        thread_data.child_time[thread_data.level] = 0.0;
        thread_data.level++;
        if (thread_data.level == thread_data.capacity) {
            thread_data.capacity *= 2;
            thread_data.child_time = g_renew(double, thread_data.child_time, thread_data.capacity);
        }
        self->num_calls[index]++;
        // g_printerr ("hook_id %lu time %f count %lu\n", hook_id, *current_time, self->num_calls[mapped_tid]);
    } else {
        // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
        // g_print ("hook_id %lu max %lu tid %lu ncall %p \n", hook_id, peak_max_num_threads, mapped_tid, self->num_calls);
        size_t index = mapped_tid;
        if (thread_data.child_time == NULL) {
            thread_data.level = 0;
            thread_data.capacity = 16;
            pthread_pause_disable();
            thread_data.child_time = g_new(gdouble, 16);
            pthread_pause_enable();
        }
        if (thread_data.tid_keys == NULL) {
            pthread_pause_disable();
            thread_data.tid_keys   = g_new0(pthread_t, peak_max_num_threads);
            thread_data.mapped_ids = g_new0(size_t,    peak_max_num_threads);
            thread_data.pause_session_ids = g_new0(int, peak_max_num_threads);
            thread_data.pause_status = g_new0(int, peak_max_num_threads);
            pthread_pause_enable();
        }
        thread_data.child_time[thread_data.level] = 0.0;
        thread_data.level++;
        if (thread_data.level == thread_data.capacity) {
            thread_data.capacity *= 2;
            pthread_pause_disable();
            thread_data.child_time = g_renew(double, thread_data.child_time, thread_data.capacity);
            pthread_pause_enable();
        }
        self->num_calls[index]++;
        size_t hook_id = self->hook_id;
        peak_target_thread_called[hook_id][index] = true;
        size_t snapshot_cap = peak_max_num_threads;
        pthread_t* tid_keys = thread_data.tid_keys;
        size_t* mapped_ids  = thread_data.mapped_ids;
        int* pause_session_ids = thread_data.pause_session_ids;
        int* pause_status = thread_data.pause_status;
        size_t snapshot_count =
            pthread_listener_snapshot_threads(tid_keys, mapped_ids, snapshot_cap);
        for (size_t s = 0; s < snapshot_count; s++) {
            pause_session_ids[s] = -1;
            pause_status[s] = -1;
        }

        pthread_mutex_lock(&lock);
        if (self->num_calls[index] >= peak_detach_count) peak_need_detach[hook_id] = true;
        if (!peak_detached[hook_id] && peak_need_detach[hook_id]) {
            array_listener_detached[hook_id] = TRUE;
            for (size_t s = 0; s < snapshot_count; s++) {
                pthread_t peak_tid_key = tid_keys[s];
                size_t cur_mapped_tid = mapped_ids[s];
                if (cur_mapped_tid < peak_max_num_threads &&
                    peak_tid_key != my_tid &&
                    peak_target_thread_called[hook_id][cur_mapped_tid]) {
                    pause_status[s] =
                        pthread_pause_mapped(peak_tid_key, cur_mapped_tid, &pause_session_ids[s]);
                }
            }
            gum_interceptor_begin_transaction(interceptor);
            gum_interceptor_detach(interceptor, listener);
            gum_interceptor_end_transaction(interceptor);
            for (size_t s = 0; s < snapshot_count; s++) {
                pthread_t peak_tid_key = tid_keys[s];
                size_t cur_mapped_tid = mapped_ids[s];
                if (cur_mapped_tid < peak_max_num_threads &&
                    peak_tid_key != my_tid &&
                    peak_target_thread_called[hook_id][cur_mapped_tid] &&
                    (pause_status[s] == 0 || pause_status[s] == 1) &&
                    pause_session_ids[s] >= 0) {
                    pthread_unpause(peak_tid_key, pause_session_ids[s]);
                }
            }
            peak_detached[hook_id] = true;
            peak_need_detach[hook_id] = false;
            // g_printerr("detach\n");
        }
        pthread_mutex_unlock(&lock);
        // gum_interceptor_revert(interceptor, hook_address[hook_id]);
        // g_printerr ("revert hook_id %lu %p\n", hook_id, hook_address[hook_id]);

        if (check_interval != 0) pthread_pause_enable();
        else pthread_pause_disable();
    }
    PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
    priv->start_time = peak_second();
    priv->initialized = TRUE;
    gum_interceptor_unignore_current_thread(interceptor);
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    gum_interceptor_ignore_current_thread(interceptor);
    if (peak_detach_cost == 0) {
        if (!listener || g_object_is_floating(listener)) {
            thread_data.level--;
            if (thread_data.level == 0) {
                void* tmp_ptr = thread_data.child_time;
                thread_data.child_time = NULL;
                g_free(tmp_ptr);
            }
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
        if (!priv->initialized) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
        // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
        // size_t hook_id = state->hook_id;
        gboolean mapped_found = FALSE;
        size_t mapped_tid = pthread_listener_lookup_thread(pthread_self(), &mapped_found);
        if (!mapped_found || mapped_tid >= peak_max_num_threads) {
            mapped_tid = 0;
        }
        end_time = end_time - priv->start_time;
        size_t index = mapped_tid;
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] || self->num_calls[index] == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        thread_data.level--;
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] += end_time - thread_data.child_time[thread_data.level];
        thread_data.child_time[thread_data.level] = 0.0;
        priv->initialized = FALSE;
        if (thread_data.level == 0) {
            void* tmp_ptr = thread_data.child_time;
            thread_data.child_time = NULL;
            g_free(tmp_ptr);
        }
    } else {
        pthread_pause_enable();
        // g_printerr("pthread_pause_enable\n");
        if (!listener || g_object_is_floating(listener)) {
            thread_data.level--;
            if (thread_data.level == 0) {
                void* tmp_ptr = thread_data.child_time;
                thread_data.child_time = NULL;
                pthread_pause_disable();
                g_free(tmp_ptr);
                pthread_pause_enable();
            }
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
        if (!priv->initialized) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
        // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
        // size_t hook_id = state->hook_id;
        gboolean mapped_found = FALSE;
        size_t mapped_tid = pthread_listener_lookup_thread(pthread_self(), &mapped_found);
        if (!mapped_found || mapped_tid >= peak_max_num_threads) {
            mapped_tid = 0;
        }
        end_time = end_time - priv->start_time;
        size_t index = mapped_tid;
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] || self->num_calls[index] == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        thread_data.level--;
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] += end_time - thread_data.child_time[thread_data.level];
        thread_data.child_time[thread_data.level] = 0.0;
        priv->initialized = FALSE;
        if (thread_data.level == 0) {
            void* tmp_ptr = thread_data.child_time;
            thread_data.child_time = NULL;
            pthread_pause_disable();
            g_free(tmp_ptr);
            pthread_pause_enable();
        }
    }
    gum_interceptor_unignore_current_thread(interceptor);
}

static void
peak_general_listener_class_init(PeakGeneralListenerClass* klass)
{
    (void)PEAKGENERAL_IS_LISTENER;
    (void)glib_autoptr_cleanup_PeakGeneralListener;
}

static void
peak_general_listener_iface_init(gpointer g_iface,
                                 gpointer iface_data)
{
    GumInvocationListenerInterface* iface = g_iface;

    iface->on_enter = peak_general_listener_on_enter;
    iface->on_leave = peak_general_listener_on_leave;
}

static void
peak_general_listener_init(PeakGeneralListener* self)
{
    size_t total_count = peak_max_num_threads;
    self->num_calls = g_new0(gulong, total_count);
    self->total_time = g_new0(gdouble, total_count);
    self->exclusive_time = g_new0(gdouble, total_count);
    self->max_time = g_new0(gfloat, total_count);
    self->min_time = g_new0(gfloat, total_count);
    // g_print ("total count %lu self->num_calls %lu\n", total_count, self->num_calls[0]);
}

static void
peak_general_listener_free(PeakGeneralListener* self)
{
    g_free(self->num_calls);
    g_free(self->total_time);
    g_free(self->exclusive_time);
    g_free(self->max_time);
    g_free(self->min_time);
}

__attribute__((noinline)) static void peak_general_overhead_dummy_func()
{
    struct timespec ts = { 0, 1 }; // Sleep for 1 nanosecond
    nanosleep(&ts, NULL);
}

static void
peak_general_overhead_bootstrapping()
{
    GumInvocationListener* listener_bootstrapping = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
    PEAKGENERAL_LISTENER(listener_bootstrapping)->hook_id = 0;
    gum_interceptor_begin_transaction(interceptor);
    gum_interceptor_attach(interceptor,
                           &peak_general_overhead_dummy_func,
                           listener_bootstrapping,
                           NULL);
    gum_interceptor_end_transaction(interceptor);

    guint n_tests = 2000;
    double* time = g_new(double, n_tests * 2);
    for (guint i = 0; i < n_tests; i++) {
        time[n_tests + i] = peak_second();
        peak_general_overhead_dummy_func();
        time[n_tests + i] = peak_second() - time[n_tests + i];
    }

    // g_printerr("%10lu times  %10.3f s total  %10.3e s max  %10.3e s min \n",
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->num_calls[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->total_time[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->max_time[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->min_time[0]);
    gum_interceptor_detach(interceptor, listener_bootstrapping);
    peak_general_listener_free(PEAKGENERAL_LISTENER(listener_bootstrapping));
    g_object_unref(listener_bootstrapping);

    for (guint i = 0; i < n_tests; i++) {
        time[i] = peak_second();
        peak_general_overhead_dummy_func();
        time[i] = peak_second() - time[i];
    }

    // g_printerr("orig %.6e time %.6e\n", orig_time, time);
    peak_general_overhead = (median_double(&time[n_tests], n_tests) - median_double(&time[0], n_tests));
    g_free(time);
}

void peak_general_listener_attach()
{
    pthread_pause_enable();
    interceptor = gum_interceptor_obtain();
    array_listener = (GumInvocationListener**)g_new0(gpointer, peak_hook_address_count);
    array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_reattached = g_new0(gboolean, peak_hook_address_count);

    hook_address = g_new0(gpointer, peak_hook_address_count);
    peak_demangled_strings = g_new0(char*, peak_hook_address_count);
    // g_printerr ("peak_hook_address_count %lu peak_max_num_threads %lu\n",  peak_hook_address_count, peak_max_num_threads);
    gum_interceptor_begin_transaction(interceptor);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        // replace certain function we are capturing already.
        if (strcmp(peak_hook_strings[i], "MPI_Finalize") == 0) {
            hook_address[i] = gum_find_function("peak_pmpi_finalize");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "close") == 0) {
            hook_address[i] = gum_find_function("peak_close");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "exit") == 0) {
            hook_address[i] = gum_find_function("peak_exit");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "main") == 0) {
            hook_address[i] = NULL;
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelEx") == 0) {
            // C++ API template versions, also use cudaLaunchKernelExC internal
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelExC") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernelEx") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_kernel_ex");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "dlopen") == 0) {
            hook_address[i] = gum_find_function("peak_dlopen");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else {
            gpointer ptr = gum_find_function(peak_hook_strings[i]);
            if (ptr) {
                hook_address[i] = ptr;
                char* demangled = cxa_demangle(peak_hook_strings[i]);
                peak_demangled_strings[i] = g_strdup(demangled);
                free(demangled);
            } else {
                if (cxa_demangle_status(peak_hook_strings[i]) != 0) {
                    // peak_hook_strings is just function name
                    gchar* truncate_hook = g_strdup_printf("*%s*", peak_hook_strings[i]);
                    GArray* addresses = gum_find_functions_matching(truncate_hook);
                    for (gsize j = 0; j < addresses->len; j++) {
                        gpointer addr = g_array_index(addresses, gpointer, j);
                        gchar* mangled = gum_symbol_name_from_address(addr);
                        gboolean function_match = FALSE;
                        if (!mangled) continue;
                    
                        char* demangled = cxa_demangle(mangled);
                        g_free(mangled);
                        if (!demangled) continue;
        
                        gchar* function_name = extract_function_name(demangled);
                        if (strcmp(peak_hook_strings[i], function_name) == 0) {
                            hook_address[i] = addr;
                            peak_demangled_strings[i] = g_strdup(demangled);
                            function_match = TRUE;
                        }
                        free(demangled);
                        free(function_name);
    
                        if (function_match) {
                            break;
                        }
                    }
                    g_array_free(addresses, TRUE);
                    g_free(truncate_hook);
                }
            }
        }
        if (hook_address[i]) {
            // g_printerr ("%s address = %p\n", peak_hook_strings[i], hook_address[i]);
            array_listener[i] = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
            PEAKGENERAL_LISTENER(array_listener[i])->hook_id = i;
            gum_interceptor_attach(interceptor,
                                   hook_address[i],
                                   array_listener[i],
                                   NULL);
        }
    }
    gum_interceptor_end_transaction(interceptor);
    if (peak_hook_address_count) {
        peak_general_overhead_bootstrapping();
        if (peak_detach_cost > 0)
            peak_detach_count = (peak_detach_cost > peak_general_overhead) ? peak_detach_cost / peak_general_overhead : 1;
        peak_need_detach[0] = false;
    }
}

static FILE* peak_stats_csv_open(void) {
    char base[256] = {0};
    char out_csv[512] = {0};

    const char *env_path = getenv("PEAK_STATSLOG_PATH");
    if (env_path && *env_path) {
        size_t n = strlen(env_path);
        if (n >= sizeof(base)) n = sizeof(base) - 1;
        memcpy(base, env_path, n);
        base[n] = '\0';
    } else {
        snprintf(base, sizeof(base), "./peak_statslog");
    }

    int pid = (int) getpid();
    snprintf(out_csv, 512, "%s-p%d.csv", base, pid);
    
    FILE* fp = fopen(out_csv, "w");
    if (!fp) {
        g_printerr("[peak] failed to open stats csv '%s': %s\n", out_csv, strerror(errno));
        return NULL;
    }

    fprintf(fp,
            "function,"
            "count,per_thread,per_rank,call_max_s,call_min_s,"
            "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n");
    return fp;
}

static void
peak_general_listener_export_csv_result(gulong* sum_num_calls,
    gdouble* sum_total_time,
    gdouble* max_total_time,
    gdouble* min_total_time,
    gdouble* sum_exclusive_time,
    gfloat* sum_max_time,
    gfloat* sum_min_time,
    gulong* thread_count,
    const int rank_count)
{
    gboolean have_output = FALSE;
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            have_output = TRUE;
            break;
        }
    }

    if (!have_output) {
        return;
    }

    FILE* csv = peak_stats_csv_open();

    if (csv) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                fprintf(csv, "\"%s\",%lu,%lu,%lu,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e\n",
                    peak_demangled_strings[i],
                    (unsigned long)sum_num_calls[i],
                    (unsigned long)sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                    (unsigned long)sum_num_calls[i] / rank_count,
                    (double)sum_max_time[i],
                    (double)sum_min_time[i],
                    (double)sum_total_time[i],
                    (double)sum_exclusive_time[i],
                    (double)max_total_time[i],
                    (double)min_total_time[i],
                    (double)(sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0)) * peak_general_overhead);
            }
        }
        fclose(csv);
    }
}

static void
peak_general_listener_print_result(gulong* sum_num_calls,
                                   gdouble* sum_total_time,
                                   gdouble* max_total_time,
                                   gdouble* min_total_time,
                                   gdouble* sum_exclusive_time,
                                   gfloat* sum_max_time,
                                   gfloat* sum_min_time,
                                   gulong* thread_count,
                                   const int rank_count)
{
    peak_general_listener_export_csv_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count,
                                            rank_count
    );

    guint max_function_width = 20;
    guint max_col_width = 10;
    guint row_width = max_function_width + max_col_width * 5 + 7;
    char* row_separator = malloc(row_width + 1);
    memset(row_separator, '-', row_width);
    row_separator[row_width] = '\0';

    char* argv_o;
    get_argv0(&argv_o);
    double total_overhead = 0.0;
    gboolean have_output = FALSE;
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            total_overhead += (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                              * peak_general_overhead;
            have_output = TRUE;
        }
    }
    if (have_output) {
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("%*s PEAK Library\n", (row_width - 12) / 2, "");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("Time: %f\n", peak_main_time);
        g_printerr("PEAK done with: %s\n", argv_o);
        g_printerr("Estimated overhead: %.3es per call and %.3es total\n", peak_general_overhead, total_overhead);

        g_printerr("\n%.*s function statistics (call)  %.*s\n", (row_width - 28) / 2, row_separator, (row_width - 28) / 2, row_separator);
        g_printerr(" individual call counts and time (in seconds)\n");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "count",
                   max_col_width, "per thread",
                   max_col_width, "per rank",
                   max_col_width, "max",
                   max_col_width, "min");
        g_printerr("%.*s\n", row_width, row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                char* truncated_name = truncate_string(peak_demangled_strings[i], max_function_width);
                if (!array_listener_detached[i])
                    g_printerr("|%*s|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                               max_function_width, truncated_name,
                               max_col_width, sum_num_calls[i],
                               max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                               max_col_width, sum_num_calls[i] / rank_count,
                               max_col_width, sum_max_time[i],
                               max_col_width, sum_min_time[i]);
                else {
                    if (!array_listener_reattached[i])
                        g_printerr("|%*s*|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                                max_function_width, truncated_name,
                                max_col_width, sum_num_calls[i],
                                max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                                max_col_width, sum_num_calls[i] / rank_count,
                                max_col_width, sum_max_time[i],
                                max_col_width, sum_min_time[i]);
                    else
                        g_printerr("|%*s**|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                                max_function_width, truncated_name,
                                max_col_width, sum_num_calls[i],
                                max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                                max_col_width, sum_num_calls[i] / rank_count,
                                max_col_width, sum_max_time[i],
                                max_col_width, sum_min_time[i]);
                }
                free(truncated_name);
            }
        }
        g_printerr("%.*s\n", row_width, row_separator);

        g_printerr("\n%.*s function statistics (thread)  %.*s\n", (row_width - 30) / 2, row_separator, (row_width - 30) / 2, row_separator);
        g_printerr(" thread aggregated time (in seconds)\n");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "total",
                   max_col_width, "exclusive",
                   max_col_width, "max",
                   max_col_width, "min",
                   max_col_width, "overhead");
        g_printerr("%.*s\n", row_width, row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                char* truncated_name = truncate_string(peak_demangled_strings[i], max_function_width);
                if (!array_listener_detached[i]) {
                    g_printerr("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                               max_function_width, truncated_name,
                               max_col_width, sum_total_time[i],
                               max_col_width, sum_exclusive_time[i],
                               max_col_width, max_total_time[i],
                               max_col_width, min_total_time[i],
                               max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                              * peak_general_overhead);
                } else {
                    if (!array_listener_reattached[i])
                        g_printerr("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                                    max_function_width, truncated_name,
                                    max_col_width, sum_total_time[i],
                                    max_col_width, sum_exclusive_time[i],
                                    max_col_width, max_total_time[i],
                                    max_col_width, min_total_time[i],
                                    max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                                    * peak_general_overhead);
                    else
                        g_printerr("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                                max_function_width, truncated_name,
                                max_col_width, sum_total_time[i],
                                max_col_width, sum_exclusive_time[i],
                                max_col_width, max_total_time[i],
                                max_col_width, min_total_time[i],
                                max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                                * peak_general_overhead);
                }
                free(truncated_name);
            }
        }
        g_printerr("%.*s\n", row_width, row_separator);
    }
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        g_free(peak_demangled_strings[i]);
    }
    g_free(peak_demangled_strings);
    free(argv_o);
    free(row_separator);
}

#ifdef HAVE_MPI
static void
peak_general_listener_reduce_result(gulong* sum_num_calls,
                                    gdouble* sum_total_time,
                                    gdouble* max_total_time,
                                    gdouble* min_total_time,
                                    gdouble* sum_exclusive_time,
                                    gfloat* sum_max_time,
                                    gfloat* sum_min_time,
                                    gulong* thread_count)
{
    int rank, size;
    int init_flag;
    MPI_Initialized(&init_flag);
    if (!init_flag)
        MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    gulong* mpi_sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* mpi_sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* mpi_sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* mpi_sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* mpi_thread_count = g_new0(gulong, peak_hook_address_count);
    gboolean* mpi_array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    MPI_Reduce(sum_num_calls, mpi_sum_num_calls, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_total_time, mpi_sum_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(max_total_time, mpi_max_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(min_total_time, mpi_min_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_exclusive_time, mpi_sum_exclusive_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_max_time, mpi_sum_max_time, peak_hook_address_count, MPI_FLOAT, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_min_time, mpi_sum_min_time, peak_hook_address_count, MPI_FLOAT, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(thread_count, mpi_thread_count, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(array_listener_detached, mpi_array_listener_detached, peak_hook_address_count, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);
    g_free(array_listener_detached);
    array_listener_detached = mpi_array_listener_detached;
    if (rank == 0) {
        peak_general_listener_print_result(mpi_sum_num_calls,
                                           mpi_sum_total_time,
                                           mpi_max_total_time,
                                           mpi_min_total_time,
                                           mpi_sum_exclusive_time,
                                           mpi_sum_max_time,
                                           mpi_sum_min_time,
                                           mpi_thread_count, size);
    }
    g_free(mpi_sum_num_calls);
    g_free(mpi_sum_total_time);
    g_free(mpi_max_total_time);
    g_free(mpi_min_total_time);
    g_free(mpi_sum_exclusive_time);
    g_free(mpi_sum_max_time);
    g_free(mpi_sum_min_time);
    g_free(mpi_thread_count);
}
#endif

void peak_general_listener_print(int is_MPI)
{
    gulong* sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* thread_count = g_new0(gulong, peak_hook_address_count);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(array_listener[i]);
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                size_t index = j;
                sum_num_calls[i] += pg_listener->num_calls[index];
                sum_total_time[i] += pg_listener->total_time[index];
                sum_exclusive_time[i] += pg_listener->exclusive_time[index];
                if (pg_listener->num_calls[index] != 0) {
                    thread_count[i]++;
                    if (pg_listener->total_time[index] > max_total_time[i])
                        max_total_time[i] = pg_listener->total_time[index];
                    if (pg_listener->total_time[index] < min_total_time[i] || thread_count[i] == 1)
                        min_total_time[i] = pg_listener->total_time[index];
                    if (pg_listener->max_time[index] > sum_max_time[i])
                        sum_max_time[i] = pg_listener->max_time[index];
                    if (pg_listener->min_time[index] < sum_min_time[i] || thread_count[i] == 1)
                        sum_min_time[i] = pg_listener->min_time[index];
                }
            }
            if (thread_count[i] == 0)
                thread_count[i] = 1;
        }
    }
#ifdef HAVE_MPI
    if (is_MPI) {
        peak_general_listener_reduce_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count);
    } else {
        peak_general_listener_print_result(sum_num_calls,
                                           sum_total_time,
                                           max_total_time,
                                           min_total_time,
                                           sum_exclusive_time,
                                           sum_max_time,
                                           sum_min_time,
                                           thread_count, 1);
    }
#else
    peak_general_listener_print_result(sum_num_calls,
                                       sum_total_time,
                                       max_total_time,
                                       min_total_time,
                                       sum_exclusive_time,
                                       sum_max_time,
                                       sum_min_time,
                                       thread_count, 1);
#endif
    g_free(sum_num_calls);
    g_free(sum_total_time);
    g_free(max_total_time);
    g_free(min_total_time);
    g_free(sum_exclusive_time);
    g_free(sum_max_time);
    g_free(sum_min_time);
    g_free(thread_count);
}

void peak_general_listener_dettach()
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            if (!peak_detached[i])
                gum_interceptor_detach(interceptor, array_listener[i]);
            peak_general_listener_free(PEAKGENERAL_LISTENER(array_listener[i]));
            g_object_unref(array_listener[i]);
        }
    }
    g_object_unref(interceptor);
    g_free(hook_address);
    g_free(array_listener_detached);
    g_free(array_listener_reattached);
    g_free(array_listener);
}

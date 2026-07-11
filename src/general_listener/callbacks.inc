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
    gulong stack_level;
    gboolean initialized;
} PeakInvocationData;

static __thread PeakGeneralThreadState thread_data;

pthread_once_t pthread_pause_once_ctrl = PTHREAD_ONCE_INIT;
pthread_mutex_t heartbeat_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  heartbeat_cond  = PTHREAD_COND_INITIALIZER;
static __thread int last_cont_id = -1;
static _Atomic int global_session_counter = 0;
static int pthread_pause_ack_pipe[2] = { -1, -1 };

static gboolean peak_general_controller_flush_teardown(void);
static gboolean peak_general_listener_pop_invocation(PeakInvocationData* priv,
                                                     gdouble* child_duration_out);

static int pthread_pause_deadline_ms(const struct timespec* deadline)
{
    struct timespec now;
    long long remaining_ns;

    clock_gettime(CLOCK_REALTIME, &now);
    remaining_ns = ((long long)deadline->tv_sec - (long long)now.tv_sec) * 1000000000LL +
                   ((long long)deadline->tv_nsec - (long long)now.tv_nsec);
    if (remaining_ns <= 0) {
        return 0;
    }

    remaining_ns = (remaining_ns + 999999LL) / 1000000LL;
    return remaining_ns > G_MAXINT ? G_MAXINT : (int)remaining_ns;
}

static int pthread_pause_wait_for_ack(int session_id, const struct timespec* deadline)
{
    if (pthread_pause_ack_pipe[0] < 0) {
        return -1;
    }

    for (;;) {
        int ack_session_id = -1;
        ssize_t nread = read(pthread_pause_ack_pipe[0],
                             &ack_session_id,
                             sizeof(ack_session_id));

        if (nread == sizeof(ack_session_id)) {
            if (ack_session_id == session_id) {
                return 0;
            }
            continue;
        }
        if (nread == -1 && errno == EINTR) {
            continue;
        }
        if (nread == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            int timeout_ms = pthread_pause_deadline_ms(deadline);
            int poll_ret;

            if (timeout_ms <= 0) {
                return 1;
            }

            pfd.fd = pthread_pause_ack_pipe[0];
            pfd.events = POLLIN;
            pfd.revents = 0;
            poll_ret = poll(&pfd, 1, timeout_ms);
            if (poll_ret > 0) {
                continue;
            }
            if (poll_ret == 0) {
                return 1;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        return -1;
    }
}

void pthread_pause_handler(int signal, siginfo_t* info, void* context)
{
    (void)context;
    int session_id = 0;
    if (info != NULL) {
        session_id = info->si_value.sival_int;
    }

    if (signal == PEAK_SIG_STOP) {
        if (pthread_pause_ack_pipe[1] >= 0) {
            ssize_t nwritten = write(pthread_pause_ack_pipe[1],
                                     &session_id,
                                     sizeof(session_id));
            (void)nwritten;
        }

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
    if (pipe2(pthread_pause_ack_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        pthread_pause_ack_pipe[0] = -1;
        pthread_pause_ack_pipe[1] = -1;
    }

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

    return pthread_pause_wait_for_ack(session_id, &ts);
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

static gboolean peak_general_hook_is_published_unlocked(size_t hook_id)
{
    return peak_hook_states != NULL &&
           hook_address != NULL &&
           array_listener != NULL &&
           hook_id < peak_hook_address_count &&
           hook_address[hook_id] != NULL &&
           array_listener[hook_id] != NULL;
}

static void peak_general_listener_publish_legacy_flags_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count ||
        peak_need_detach == NULL ||
        peak_detached == NULL) {
        return;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_DETACH_REQUESTED:
        case PEAK_HOOK_DETACHING:
            peak_need_detach[hook_id] = TRUE;
            peak_detached[hook_id] = FALSE;
            break;
        case PEAK_HOOK_DETACHED:
        case PEAK_HOOK_REATTACH_REQUESTED:
        case PEAK_HOOK_REATTACHING:
        case PEAK_HOOK_SHUTDOWN:
            peak_need_detach[hook_id] = FALSE;
            peak_detached[hook_id] = TRUE;
            break;
        case PEAK_HOOK_UNRESOLVED:
        case PEAK_HOOK_ATTACHED:
        default:
            peak_need_detach[hook_id] = FALSE;
            peak_detached[hook_id] = FALSE;
            break;
    }
}

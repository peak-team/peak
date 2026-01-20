#include "malloc_otf2.h"

typedef struct {
    uint32_t tid;
    OTF2_LocationRef loc_ref;
} TidLocationEntry;

typedef struct {
    TidLocationEntry* data;
    size_t            size;
    size_t            capacity;
} TidLocationMap;

static void tid_map_init(TidLocationMap* m) {
    m->data = NULL;
    m->size = 0;
    m->capacity = 0;
}

static void tid_map_free(TidLocationMap* m) {
    free(m->data);
    m->data = NULL;
    m->size = 0;
    m->capacity = 0;
}

static int tid_map_reserve(TidLocationMap* m, size_t new_cap) {
    if (new_cap <= m->capacity) return 0;
    TidLocationEntry* p =
        (TidLocationEntry*)realloc(m->data, new_cap * sizeof(TidLocationEntry));
    if (!p) return -1;
    m->data = p;
    m->capacity = new_cap;
    return 0;
}

static OTF2_LocationRef tid_map_get_or_add(TidLocationMap* m, uint32_t tid) {
    // linear scan; number of threads is usually small
    for (size_t i = 0; i < m->size; ++i) {
        if (m->data[i].tid == tid) return m->data[i].loc_ref;
    }
    if (tid_map_reserve(m, m->size + 8) != 0) {
        return (OTF2_LocationRef)UINT64_MAX;
    }
    OTF2_LocationRef loc_ref = (OTF2_LocationRef)m->size;
    m->data[m->size].tid     = tid;
    m->data[m->size].loc_ref = loc_ref;
    m->size++;
    return loc_ref;
}

static size_t tid_map_find_idx_for_tid(uint32_t tid, const TidLocationMap* m) {
    for (size_t i = 0; i < m->size; ++i) {
        if (m->data[i].tid == tid) return i;
    }
    return SIZE_MAX;
}

static OTF2_FlushType
peak_pre_flush(void* userData,
               OTF2_FileType fileType,
               OTF2_LocationRef location,
               void* callerData,
               bool final) {
    (void)userData;
    (void)fileType;
    (void)location;
    (void)callerData;
    (void)final;
    return OTF2_FLUSH;
}

static OTF2_TimeStamp
peak_post_flush(void* userData,
                OTF2_FileType fileType,
                OTF2_LocationRef location) {
    (void)userData;
    (void)fileType;
    (void)location;
    return 0;
}

static OTF2_FlushCallbacks peak_flush_callbacks = {
    .otf2_pre_flush  = peak_pre_flush,
    .otf2_post_flush = peak_post_flush,
};

static void peak_otf2_log_err(const char* where, OTF2_ErrorCode ec) {
    const char* name = OTF2_Error_GetName(ec);
    const char* desc = OTF2_Error_GetDescription(ec);
    fprintf(stderr,
            "[peak][memlog][otf2] %s failed: code=%d name=%s desc=%s\n",
            where,
            (int)ec,
            name ? name : "unknown",
            desc ? desc : "");
}

/* base: pointer to first PeakMemEvent in mmap region (after header_bytes)
 * events: count from atomic_load_explicit(&g_memlog.index, ...)
 */
void peak_memlog_export_otf2(char* filename, const PeakMemEvent* base, size_t events) {
    if (!base || events == 0) return;

    const char* env_otf_dir = getenv("PEAK_MEMLOG_OTF2_DIR");
    char out_dir[PATH_MAX];

    if (env_otf_dir && env_otf_dir[0] != '\0') {
        strncpy(out_dir, env_otf_dir, sizeof(out_dir));
        out_dir[sizeof(out_dir) - 1] = '\0';
    } else {
        strncpy(out_dir, ".", sizeof(out_dir));
        out_dir[sizeof(out_dir) - 1] = '\0';
    }

    /* scan timestamps + tids */
    uint64_t min_ts = UINT64_MAX, max_ts = 0;
    TidLocationMap tid_map;
    tid_map_init(&tid_map);

    for (size_t i = 0; i < events; ++i) {
        const PeakMemEvent* e = base + i;
        uint64_t t = e->ts_ns;
        if (t < min_ts) min_ts = t;
        if (t > max_ts) max_ts = t;
        if (tid_map_get_or_add(&tid_map, e->tid) == (OTF2_LocationRef)UINT64_MAX) {
            fprintf(stderr, "[peak][memlog][otf2] tid_map alloc failed\n");
            tid_map_free(&tid_map);
            return;
        }
    }
    if (min_ts == UINT64_MAX) {
        min_ts = 0;
        max_ts = 0;
    }

    OTF2_Archive* archive = OTF2_Archive_Open(
        out_dir,
        filename,
        OTF2_FILEMODE_WRITE,
        4 * 1024 * 1024,       /* event chunk size */
        4 * 1024 * 1024,       /* def chunk size   */
        OTF2_SUBSTRATE_POSIX,
        OTF2_COMPRESSION_NONE
    );
    if (!archive) {
        fprintf(stderr,
                "[peak][memlog][otf2] failed to open archive at %s/%s\n",
                out_dir, filename);
        tid_map_free(&tid_map);
        return;
    }

    OTF2_Archive_SetFlushCallbacks(archive, &peak_flush_callbacks, NULL);
    OTF2_Archive_SetSerialCollectiveCallbacks(archive);

    OTF2_GlobalDefWriter* def = OTF2_Archive_GetGlobalDefWriter(archive);
    if (!def) {
        fprintf(stderr, "[peak][memlog][otf2] failed to get def writer\n");
        OTF2_Archive_Close(archive);
        tid_map_free(&tid_map);
        return;
    }

    /* String IDs */
    const OTF2_StringRef STR_EMPTY        = 0;
    const OTF2_StringRef STR_TRACE_NAME   = 1;
    const OTF2_StringRef STR_METRIC_NAME  = 2;
    const OTF2_StringRef STR_DELTA_NAME   = 3;
    const OTF2_StringRef STR_CURRENT_NAME = 4;
    const OTF2_StringRef STR_PROCESS_NAME = 5;
    const OTF2_StringRef STR_UNIT_BYTES   = 6;
    const OTF2_StringRef STR_THREAD_BASE  = 10; // thread names: base + i

    OTF2_ErrorCode ec;
    ec = OTF2_GlobalDefWriter_WriteString(def, STR_EMPTY, "");
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteString(EMPTY)", ec);

    ec = OTF2_GlobalDefWriter_WriteString(def, STR_TRACE_NAME, "PEAK_MEMORY_TRACE");
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteString(TRACE_NAME)", ec);

    ec = OTF2_GlobalDefWriter_WriteString(def, STR_METRIC_NAME, "PEAK_MEMORY");
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteString(METRIC_NAME)", ec);

    ec = OTF2_GlobalDefWriter_WriteString(def, STR_DELTA_NAME, "delta_bytes");
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteString(DELTA_NAME)", ec);

    ec = OTF2_GlobalDefWriter_WriteString(def, STR_CURRENT_NAME, "current_bytes");
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteString(CURRENT_NAME)", ec);

    ec = OTF2_GlobalDefWriter_WriteString(def, STR_PROCESS_NAME, "Process");
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteString(PROCESS_NAME)", ec);

    ec = OTF2_GlobalDefWriter_WriteString(def, STR_UNIT_BYTES, "bytes");
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteString(UNIT_BYTES)", ec);

    /* Clock properties */
    uint64_t timer_resolution = 1000000000ULL; // 1 tick = 1 ns
    uint64_t trace_length     = (max_ts > min_ts) ? (max_ts - min_ts) : 0;

    ec = OTF2_GlobalDefWriter_WriteClockProperties(
        def,
        timer_resolution,
        min_ts,      /* globalOffset (ticks) */
        trace_length,
        min_ts       /* realtimeTimestamp (just reuse min_ts here) */
    );
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteClockProperties", ec);

    /* Metric members & class */
    const OTF2_MetricMemberRef METRIC_MEMBER_DELTA   = 1;
    const OTF2_MetricMemberRef METRIC_MEMBER_CURRENT = 2;
    const OTF2_MetricRef       METRIC_CLASS_PEAKMEM  = 1;

    ec = OTF2_GlobalDefWriter_WriteMetricMember(
        def,
        METRIC_MEMBER_DELTA,          /* self          */
        STR_DELTA_NAME,               /* name          */
        STR_EMPTY,                    /* description   */
        OTF2_METRIC_TYPE_OTHER,       /* metricType    */
        OTF2_METRIC_ABSOLUTE_POINT,   /* metricMode    */
        OTF2_TYPE_INT64,              /* valueType     */
        OTF2_BASE_DECIMAL,            /* base          */
        0,                            /* exponent      */
        STR_UNIT_BYTES                /* unit ("bytes")*/
    );
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteMetricMember(DELTA)", ec);

    ec = OTF2_GlobalDefWriter_WriteMetricMember(
        def,
        METRIC_MEMBER_CURRENT,        /* self          */
        STR_CURRENT_NAME,             /* name          */
        STR_EMPTY,                    /* description   */
        OTF2_METRIC_TYPE_OTHER,       /* metricType    */
        OTF2_METRIC_ABSOLUTE_POINT,   /* metricMode    */
        OTF2_TYPE_UINT64,             /* valueType     */
        OTF2_BASE_DECIMAL,            /* base          */
        0,                            /* exponent      */
        STR_UNIT_BYTES                /* unit ("bytes")*/
    );
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteMetricMember(CURRENT)", ec);

    OTF2_MetricMemberRef members[2] = {
        METRIC_MEMBER_DELTA,
        METRIC_MEMBER_CURRENT
    };

    ec = OTF2_GlobalDefWriter_WriteMetricClass(
        def,
        METRIC_CLASS_PEAKMEM,             /* self             */
        2,                                /* numberOfMetrics  */
        members,                          /* metricMembers    */
        OTF2_METRIC_SYNCHRONOUS,          /* occurrence       */
        OTF2_RECORDER_KIND_CPU            /* recorderKind     */
    );
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteMetricClass(PEAKMEM)", ec);

    /* System tree + location group */
    OTF2_SystemTreeNodeRef stn_ref = 0;
    ec = OTF2_GlobalDefWriter_WriteSystemTreeNode(
        def,
        stn_ref,
        STR_PROCESS_NAME,
        STR_EMPTY,
        OTF2_UNDEFINED_SYSTEM_TREE_NODE
    );
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteSystemTreeNode", ec);

    OTF2_LocationGroupRef lg_ref = 0;
    ec = OTF2_GlobalDefWriter_WriteLocationGroup(
        def,
        lg_ref,
        STR_PROCESS_NAME,
        OTF2_LOCATION_GROUP_TYPE_PROCESS,
        stn_ref,
        OTF2_UNDEFINED_LOCATION_GROUP   /* creatingLocationGroup */
    );
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteLocationGroup", ec);

    /* Per-thread locations */
    for (size_t i = 0; i < tid_map.size; ++i) {
        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "Thread %u", tid_map.data[i].tid);

        OTF2_StringRef name_ref = STR_THREAD_BASE + (OTF2_StringRef)(i+1);
        ec = OTF2_GlobalDefWriter_WriteString(def, name_ref, name_buf);
        if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteString(THREAD_NAME)", ec);

        ec = OTF2_GlobalDefWriter_WriteLocation(
            def,
            tid_map.data[i].loc_ref,
            name_ref,
            OTF2_LOCATION_TYPE_CPU_THREAD,
            0,
            lg_ref
        );
        if (ec != OTF2_SUCCESS) peak_otf2_log_err("WriteLocation", ec);
    }

    ec = OTF2_Archive_CloseGlobalDefWriter(archive, def);
    if (ec != OTF2_SUCCESS) peak_otf2_log_err("CloseGlobalDefWriter", ec);

    /* Event writers per location */
    OTF2_EvtWriter** writers =
        (OTF2_EvtWriter**)calloc(tid_map.size, sizeof(OTF2_EvtWriter*));
    if (!writers) {
        fprintf(stderr, "[peak][memlog][otf2] failed to alloc evt writers\n");
        OTF2_Archive_Close(archive);
        tid_map_free(&tid_map);
        return;
    }

    for (size_t i = 0; i < tid_map.size; ++i) {
        writers[i] =
            OTF2_Archive_GetEvtWriter(archive, tid_map.data[i].loc_ref);
        if (!writers[i]) {
            fprintf(stderr,
                    "[peak][memlog][otf2] failed to get evt writer for loc=%" PRIu64 "\n",
                    (uint64_t)tid_map.data[i].loc_ref);
        }
    }

    /* Write metric events */
    static const OTF2_Type peak_metric_types[2] = {
        OTF2_TYPE_INT64,   /* delta  */
        OTF2_TYPE_UINT64   /* current */
    };

    for (size_t i = 0; i < events; ++i) {
        const PeakMemEvent* e = base + i;

        size_t idx = tid_map_find_idx_for_tid(e->tid, &tid_map);
        if (idx == SIZE_MAX) continue;
        OTF2_EvtWriter* w = writers[idx];
        if (!w) continue;

        OTF2_TimeStamp ts_real = (OTF2_TimeStamp)(e->ts_ns);

        OTF2_MetricValue vals[2];
        memset(vals, 0, sizeof(vals));
        vals[0].signed_int   = (int64_t)e->delta;
        vals[1].unsigned_int = (uint64_t)e->current;

        ec = OTF2_EvtWriter_Metric(
            w,
            NULL,
            ts_real,
            METRIC_CLASS_PEAKMEM,
            2,
            peak_metric_types,
            vals
        );
        if (ec != OTF2_SUCCESS) {
            peak_otf2_log_err("EvtWriter_Metric", ec);
        }
    }

    /* Close writers & archive */
    for (size_t i = 0; i < tid_map.size; ++i) {
        if (writers[i]) {
            OTF2_ErrorCode ec2 = OTF2_Archive_CloseEvtWriter(archive, writers[i]);
            if (ec2 != OTF2_SUCCESS) peak_otf2_log_err("CloseEvtWriter", ec2);
        }
    }
    free(writers);

    OTF2_ErrorCode ec2 = OTF2_Archive_Close(archive);
    if (ec2 != OTF2_SUCCESS) peak_otf2_log_err("Archive_Close", ec2);

    tid_map_free(&tid_map);

    fprintf(stderr,
            "[peak] memlog OTF2 written: %s/%s (events=%zu)\n",
            out_dir, filename, events);
}
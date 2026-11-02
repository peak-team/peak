#include "internal/mpi_progress_contract.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char intel_name[] = "intel-2021.11-ofi-opx-host-async-v1";
static const char ompi_name[] =
    "ompi-5.0.5-ucx-1.17-rc-proto2-host-observed-v1";
static const char *setting(const char *name) {
  const char *value = getenv(name);
  return value ? value : "";
}
static void copy(char *out, size_t size, const char *value) {
  size_t length = 0;
  while (length + 1 < size && value[length])
    ++length;
  memcpy(out, value, length);
  out[length] = 0;
}
static int version_token(const char *version, const char *token, int comma) {
  const char *match = version ? strstr(version, token) : NULL;
  if (!match)
    return 0;
  unsigned char next = (unsigned char)match[strlen(token)];
  return !next || isspace(next) || (comma && next == ',');
}
static int exact_set(const char *value, const char *const *wanted,
                     size_t count) {
  unsigned found = 0;
  if (count > sizeof(found) * 8)
    return 0;
  while (*value) {
    while (isspace((unsigned char)*value))
      ++value;
    const char *end = strchr(value, ',');
    if (!end)
      end = value + strlen(value);
    const char *trim = end;
    while (trim > value && isspace((unsigned char)trim[-1]))
      --trim;
    int matched = 0;
    for (size_t i = 0; i < count; ++i)
      if (strlen(wanted[i]) == (size_t)(trim - value) &&
          !strncmp(value, wanted[i], (size_t)(trim - value))) {
        found |= 1u << i;
        matched = 1;
        break;
      }
    if (!matched)
      return 0;
    if (!*end)
      break;
    value = end + 1;
    if (!*value)
      return 0;
  }
  return found == ((1u << count) - 1);
}
static const char *ucx_setting(const char *custom, const char *generic,
                               const char *fallback) {
  const char *value = getenv(custom);
  if (value)
    return value;
  value = getenv(generic);
  return value ? value : fallback;
}
PeakMpiProgressContract peak_mpi_progress_contract_select(const char *version) {
  return peak_mpi_progress_contract_select_verified(version, -1, -1);
}
PeakMpiProgressContract
peak_mpi_progress_contract_select_verified(const char *version, int async,
                                           int provided) {
  PeakMpiProgressVerification verification = {
      .effective_async_progress = async, .provided_thread_level = provided};
  return peak_mpi_progress_contract_select_public(version, &verification);
}
PeakMpiProgressContract
peak_mpi_progress_contract_select_public(const char *version,
                                         const PeakMpiProgressVerification *v) {
  PeakMpiProgressContract c = {
      .profile = PEAK_MPI_PROGRESS_UNKNOWN,
      .name = "unknown",
      .reason = "no reviewed public progress contract for this configuration",
      .effective_async_progress = -1,
      .provided_thread_level = -1};
  copy(c.requested_profile, sizeof(c.requested_profile),
       setting("PEAK_MPI_PROGRESS_PROFILE"));
  copy(c.fabrics, sizeof(c.fabrics), setting("I_MPI_FABRICS"));
  copy(c.ofi_provider, sizeof(c.ofi_provider), setting("I_MPI_OFI_PROVIDER"));
  copy(c.fi_provider, sizeof(c.fi_provider), setting("FI_PROVIDER"));
  copy(c.async_setting, sizeof(c.async_setting),
       setting("I_MPI_ASYNC_PROGRESS"));
  copy(c.ucx_tls, sizeof(c.ucx_tls), ucx_setting("MPI_UCX_TLS", "UCX_TLS", ""));
  copy(c.ucx_proto, sizeof(c.ucx_proto),
       ucx_setting("MPI_UCX_PROTO_ENABLE", "UCX_PROTO_ENABLE", ""));
  copy(c.ucx_tm, sizeof(c.ucx_tm), setting("UCX_RC_MLX5_TM_ENABLE"));
  if (v) {
    c.effective_async_progress = v->effective_async_progress;
    c.async_verified = v->effective_async_progress >= 0;
    c.provided_thread_level = v->provided_thread_level;
    c.pml_verified = v->pml_verified;
    c.coll_verified = v->coll_verified;
    c.fixed_rules_verified = v->fixed_rules_verified;
    copy(c.pml, sizeof(c.pml), v->pml);
    copy(c.coll, sizeof(c.coll), v->coll);
  }
  if (!strcmp(c.requested_profile, intel_name)) {
    int version_ok = version_token(version, "Intel(R) MPI Library 2021.11", 0);
    int config = (!strcmp(c.fabrics, "shm:ofi") || !strcmp(c.fabrics, "ofi")) &&
                 !strcmp(c.ofi_provider, "opx") &&
                 (!getenv("FI_PROVIDER") || !strcmp(c.fi_provider, "opx"));
    if (version_ok && config && !strcmp(c.async_setting, "1") &&
        c.effective_async_progress == 1 && c.provided_thread_level >= 0) {
      c.profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
      c.autonomous_p2p = c.autonomous_blocking_collective =
          c.autonomous_selected_nbc = 1;
      c.name = intel_name;
      c.reason = "Intel 2021-11 public asynchronous progress support matrix";
    } else
      c.reason =
          !version_ok ? "named Intel profile MPI version mismatch"
          : !config
              ? "named Intel profile requires explicit OFI/OPX configuration"
          : strcmp(c.async_setting, "1")
              ? "named Intel profile requires I_MPI_ASYNC_PROGRESS=1"
          : c.effective_async_progress != 1
              ? "public MPI_T asynchronous progress verification missing or "
                "disabled"
              : "native provided thread level could not be recorded";
  } else if (!strcmp(c.requested_profile, ompi_name)) {
    const char *tls[] = {"rc", "self"}, *pml[] = {"ucx"},
               *coll[] = {"basic", "tuned", "self", "libnbc"};
    int version_ok = version_token(version, "Open MPI v5.0.5", 1);
    int config =
        exact_set(ucx_setting("MPI_UCX_TLS", "UCX_TLS", ""), tls, 2) &&
        !strcmp(ucx_setting("MPI_UCX_PROTO_ENABLE", "UCX_PROTO_ENABLE", ""),
                "y") &&
        !strcmp(setting("UCX_RC_MLX5_TM_ENABLE"), "n");
    int public_ok = v && v->pml_verified && v->coll_verified &&
                    v->fixed_rules_verified && exact_set(v->pml, pml, 1) &&
                    exact_set(v->coll, coll, 4);
    if (version_ok && config && public_ok && c.provided_thread_level >= 0) {
      c.profile = PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED;
      c.observed_p2p_completion = c.observed_buffer_detach =
          c.observed_collective_completion = 1;
      c.name = ompi_name;
      c.reason = "reviewed Open MPI5.0.5 UCX1.17 RC proto2 host public "
                 "completion profile";
    } else
      c.reason =
          !version_ok ? "named Open MPI profile MPI version mismatch"
          : !config
              ? "named Open MPI profile effective UCX configuration mismatch"
          : !public_ok ? "public MPI_T PML/collective fixed-rules verification "
                         "missing or mismatched"
                       : "native provided thread level could not be recorded";
  }
  return c;
}
int peak_mpi_progress_contract_supports(const PeakMpiProgressContract *c,
                                        PeakMpiCausalOperation op,
                                        unsigned flags) {
  if (!c || c->profile == PEAK_MPI_PROGRESS_UNKNOWN)
    return 0;
  int observed = c->profile ==
                 PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED;
  if (observed && (flags & PEAK_MPI_CAUSAL_VARIABLE_COUNT))
    return 0;
  switch (op) {
  case PEAK_MPI_CAUSAL_SSEND:
    return !observed && c->autonomous_p2p;
  case PEAK_MPI_CAUSAL_SEND:
  case PEAK_MPI_CAUSAL_BSEND:
  case PEAK_MPI_CAUSAL_RSEND:
  case PEAK_MPI_CAUSAL_RECV:
  case PEAK_MPI_CAUSAL_SENDRECV:
  case PEAK_MPI_CAUSAL_PROBE:
  case PEAK_MPI_CAUSAL_MPROBE:
  case PEAK_MPI_CAUSAL_MRECV:
    return observed ? c->observed_p2p_completion : c->autonomous_p2p;
  case PEAK_MPI_CAUSAL_BARRIER:
  case PEAK_MPI_CAUSAL_BCAST:
  case PEAK_MPI_CAUSAL_ALLREDUCE:
  case PEAK_MPI_CAUSAL_REDUCE:
  case PEAK_MPI_CAUSAL_ALLGATHER:
  case PEAK_MPI_CAUSAL_ALLTOALL:
  case PEAK_MPI_CAUSAL_REDUCE_SCATTER:
  case PEAK_MPI_CAUSAL_GATHER:
  case PEAK_MPI_CAUSAL_SCATTER:
  case PEAK_MPI_CAUSAL_SCAN:
  case PEAK_MPI_CAUSAL_EXSCAN:
    if (observed)
      return c->observed_collective_completion;
    if (!(flags & PEAK_MPI_CAUSAL_NONBLOCKING))
      return c->autonomous_blocking_collective;
    return c->autonomous_selected_nbc &&
           (op == PEAK_MPI_CAUSAL_BCAST || op == PEAK_MPI_CAUSAL_REDUCE ||
            op == PEAK_MPI_CAUSAL_ALLREDUCE);
  case PEAK_MPI_CAUSAL_COMM_DUP:
  case PEAK_MPI_CAUSAL_COMM_SPLIT:
    return 1;
  case PEAK_MPI_CAUSAL_CANCEL:
    return !observed;
  case PEAK_MPI_CAUSAL_COMM_IDUP:
    return observed;
  case PEAK_MPI_CAUSAL_START:
  case PEAK_MPI_CAUSAL_WAIT:
  case PEAK_MPI_CAUSAL_TEST:
  case PEAK_MPI_CAUSAL_BUFFER_DETACH:
  case PEAK_MPI_CAUSAL_REQUEST_FREE:
  case PEAK_MPI_CAUSAL_FINALIZE:
  case PEAK_MPI_CAUSAL_COMM_FREE:
    return 1;
  }
  return 0;
}
int peak_mpi_progress_contract_collective_allowed(
    const PeakMpiProgressContract *c, PeakMpiCausalOperation op, unsigned flags,
    int size) {
  if (!peak_mpi_progress_contract_supports(c, op, flags))
    return 0;
  if (c->profile ==
          PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED &&
      op == PEAK_MPI_CAUSAL_BARRIER && !(flags & PEAK_MPI_CAUSAL_NONBLOCKING)) {
    int power = size > 0 && !(size & (size - 1));
    if (size == 3 || (size >= 9 && size <= 63 && !power))
      return 0;
  }
  return 1;
}
unsigned peak_mpi_progress_contract_result_flags(
    const PeakMpiProgressContract *c, PeakMpiCausalOperation op, unsigned flags,
    int observed_completion, PeakMpiObserverProgressScope *scope) {
  *scope = PEAK_MPI_OBSERVER_SCOPE_UNKNOWN;
  if (!peak_mpi_progress_contract_supports(c, op, flags))
    return 0;
  if (c->profile == PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC) {
    *scope = PEAK_MPI_OBSERVER_SCOPE_AUTONOMOUS;
    return PEAK_MPI_OBSERVER_PROGRESS_SEALED;
  }
  *scope = PEAK_MPI_OBSERVER_SCOPE_INTERVAL;
  if (op == PEAK_MPI_CAUSAL_BUFFER_DETACH)
    return PEAK_MPI_OBSERVER_PROGRESS_SEALED;
  if (op == PEAK_MPI_CAUSAL_BSEND || (flags & PEAK_MPI_CAUSAL_BUFFERED) ||
      op == PEAK_MPI_CAUSAL_START)
    return 0;
  return !(flags & PEAK_MPI_CAUSAL_NONBLOCKING) || observed_completion ||
                 (flags & PEAK_MPI_CAUSAL_INACTIVE_REQUEST)
             ? PEAK_MPI_OBSERVER_PROGRESS_SEALED
             : 0;
}

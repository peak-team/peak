#include "general_listener.h"

#include <math.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int
close_enough(double actual, double expected)
{
    return fabs(actual - expected) <= 1e-12;
}

static PeakMpiReportTestTuple
fixture_tuple(int rank)
{
    PeakMpiReportTestTuple tuple = {0};

    tuple.accounting_valid = TRUE;
    tuple.local_ranks = 3U;
    tuple.stop_window_count = 100U + (uint64_t)rank;
    tuple.failed_stop_window_count = 10U + (uint64_t)rank;
    if (rank == 0) {
        tuple.elapsed_seconds = 10.0;
        tuple.profile_seconds = 1.0;
        tuple.control_seconds = 1.0;
        tuple.management_seconds = 0.5;
    } else if (rank == 1) {
        tuple.elapsed_seconds = 20.0;
        tuple.profile_seconds = 6.0;
        tuple.control_seconds = 2.0;
        tuple.management_seconds = 4.0;
    } else {
        tuple.elapsed_seconds = 30.0 + (double)rank;
    }
    tuple.control_risk_seconds =
        tuple.control_seconds * (double)tuple.local_ranks;
    tuple.profile_control_risk_seconds =
        tuple.profile_seconds + tuple.control_risk_seconds;
    tuple.profile_ratio = tuple.profile_seconds / tuple.elapsed_seconds;
    tuple.control_ratio = tuple.control_seconds / tuple.elapsed_seconds;
    tuple.profile_control_risk_ratio =
        tuple.profile_control_risk_seconds / tuple.elapsed_seconds;
    tuple.control_risk_ratio =
        tuple.control_risk_seconds / tuple.elapsed_seconds;
    tuple.management_ratio =
        tuple.management_seconds / tuple.elapsed_seconds;
    tuple.ratio =
        (tuple.profile_seconds + tuple.control_seconds) /
        tuple.elapsed_seconds;
    return tuple;
}

static int
tuple_matches(const PeakMpiReportTestTuple* actual,
              const PeakMpiReportTestTuple* expected)
{
    return actual->accounting_valid == expected->accounting_valid &&
           actual->local_ranks == expected->local_ranks &&
           actual->stop_window_count == expected->stop_window_count &&
           actual->failed_stop_window_count ==
               expected->failed_stop_window_count &&
           close_enough(actual->elapsed_seconds, expected->elapsed_seconds) &&
           close_enough(actual->profile_seconds, expected->profile_seconds) &&
           close_enough(actual->control_seconds, expected->control_seconds) &&
           close_enough(actual->management_seconds,
                        expected->management_seconds) &&
           close_enough(actual->control_risk_seconds,
                        expected->control_risk_seconds) &&
           close_enough(actual->profile_control_risk_seconds,
                        expected->profile_control_risk_seconds) &&
           close_enough(actual->profile_ratio, expected->profile_ratio) &&
           close_enough(actual->control_ratio, expected->control_ratio) &&
           close_enough(actual->profile_control_risk_ratio,
                        expected->profile_control_risk_ratio) &&
           close_enough(actual->control_risk_ratio,
                        expected->control_risk_ratio) &&
           close_enough(actual->management_ratio,
                        expected->management_ratio) &&
           close_enough(actual->ratio, expected->ratio);
}

static int
ratios_recompute(const PeakMpiReportTestTuple maximum[6])
{
    return close_enough(
               maximum[0].ratio,
               (maximum[0].profile_seconds + maximum[0].control_seconds) /
                   maximum[0].elapsed_seconds) &&
           close_enough(maximum[1].profile_ratio,
                        maximum[1].profile_seconds /
                            maximum[1].elapsed_seconds) &&
           close_enough(maximum[2].control_ratio,
                        maximum[2].control_seconds /
                            maximum[2].elapsed_seconds) &&
           close_enough(maximum[3].management_ratio,
                        maximum[3].management_seconds /
                            maximum[3].elapsed_seconds) &&
           close_enough(maximum[4].control_risk_seconds,
                        maximum[4].control_seconds *
                            (double)maximum[4].local_ranks) &&
           close_enough(maximum[4].profile_control_risk_seconds,
                        maximum[4].profile_seconds +
                            maximum[4].control_risk_seconds) &&
           close_enough(maximum[4].profile_control_risk_ratio,
                        maximum[4].profile_control_risk_seconds /
                            maximum[4].elapsed_seconds) &&
           close_enough(maximum[5].control_risk_seconds,
                        maximum[5].control_seconds *
                            (double)maximum[5].local_ranks) &&
           close_enough(maximum[5].control_risk_ratio,
                        maximum[5].control_risk_seconds /
                            maximum[5].elapsed_seconds);
}

int
main(int argc, char** argv)
{
    static const int expected_owners[6] = {1, 1, 0, 1, 1, 0};
    PeakMpiReportTestTuple maximum[6] = {{0}};
    int owners[6] = {0};
    int rank;
    int size;
    int failures = 0;
    int global_failures = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size < 2) {
        if (rank == 0) {
            fprintf(stderr, "test_mpi_report_reduction requires two ranks\n");
        }
        MPI_Finalize();
        return EXIT_FAILURE;
    }

    PeakMpiReportTestTuple local = fixture_tuple(rank);
    if (!peak_general_listener_test_reduce_report_tuples(&local,
                                                         maximum,
                                                         owners)) {
        failures++;
    }
    for (size_t i = 0; i < 6; i++) {
        PeakMpiReportTestTuple expected = fixture_tuple(expected_owners[i]);
        if (owners[i] != expected_owners[i] ||
            !tuple_matches(&maximum[i], &expected)) {
            failures++;
        }
    }
    if (!ratios_recompute(maximum)) {
        failures++;
    }
    if (peak_general_listener_test_mpi_uint64_type_size() !=
        (int)sizeof(uint64_t)) {
        failures++;
    }

    MPI_Reduce(&failures,
               &global_failures,
               1,
               MPI_INT,
               MPI_SUM,
               0,
               MPI_COMM_WORLD);
    if (rank == 0 && global_failures == 0) {
        peak_general_listener_test_print_report_tuples(maximum, owners);
        puts("mpi_report_reduction_ok");
    }
    MPI_Finalize();
    return global_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

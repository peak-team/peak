#include "mpi_utils.h"
#include "process_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
expect_filtered(const char* command)
{
    if (!check_command(command)) {
        fprintf(stderr, "expected command to be filtered: %s\n", command);
        return 0;
    }
    return 1;
}

static int
expect_profiled(const char* command)
{
    if (check_command(command)) {
        fprintf(stderr, "expected command to remain profiled: %s\n", command);
        return 0;
    }
    return 1;
}

static int
expect_module_helper(int argc, char* const argv[])
{
    if (!check_module_helper_command(argc, argv)) {
        fprintf(stderr, "expected module helper to be filtered: %s\n", argv[0]);
        return 0;
    }
    return 1;
}

static int
expect_not_module_helper(int argc, char* const argv[])
{
    if (check_module_helper_command(argc, argv)) {
        fprintf(stderr, "expected interpreter app to remain profiled: %s\n", argv[0]);
        return 0;
    }
    return 1;
}

static int
expect_interpreter(const char* command)
{
    if (!check_interpreter_command(command)) {
        fprintf(stderr, "expected interpreter command: %s\n", command);
        return 0;
    }
    return 1;
}

static int
expect_not_interpreter(const char* command)
{
    if (check_interpreter_command(command)) {
        fprintf(stderr, "expected non-interpreter command: %s\n", command);
        return 0;
    }
    return 1;
}

static int
expect_profile_decision(int argc,
                        char* const argv[],
                        int expected,
                        const char* label)
{
    int actual = peak_should_profile_command(argc, argv);
    if (actual != expected) {
        fprintf(stderr,
                "unexpected profile decision for %s: got %d expected %d\n",
                label,
                actual,
                expected);
        return 0;
    }
    return 1;
}

static void
clear_mpi_env(void)
{
    static const char* names[] = {
        "PMI_RANK",
        "PMIX_RANK",
        "MV2_COMM_WORLD_RANK",
        "OMPI_COMM_WORLD_RANK",
        "I_MPI_RANK",
        "PMI_SIZE",
        "PMIX_SIZE",
        "MV2_COMM_WORLD_SIZE",
        "OMPI_COMM_WORLD_SIZE",
        "I_MPI_SIZE",
        "SLURM_PROCID",
        "SLURM_NTASKS",
        NULL
    };

    for (const char** name = names; *name != NULL; name++) {
        unsetenv(*name);
    }
}

static int
expect_mpi_detected_by(const char* name)
{
    clear_mpi_env();
    setenv(name, "1", 1);
    if (!check_MPI()) {
        fprintf(stderr, "expected MPI rank env to be detected: %s\n", name);
        return 0;
    }
    return 1;
}

static int
expect_mpi_not_detected_by(const char* name)
{
    clear_mpi_env();
    setenv(name, "1", 1);
    if (check_MPI()) {
        fprintf(stderr, "expected non-rank MPI env to be ignored: %s\n", name);
        return 0;
    }
    return 1;
}

static int
expect_requested_work_override(int enabled)
{
    peak_set_process_requests_work(enabled);
    int actual = peak_process_requests_work();
    if (actual != enabled) {
        fprintf(stderr,
                "unexpected requested-work override: got %d expected %d\n",
                actual,
                enabled);
        return 0;
    }
    return 1;
}

int
main(void)
{
    int ok = 1;
    char* lmod_lua[] = {
        "/usr/bin/lua",
        "/opt/apps/lmod/lmod/libexec/lmod",
        "list",
        NULL
    };
    char* lmod_python[] = {
        "/usr/bin/python3.11",
        "/opt/apps/lmod/lmod/init/env_modules_python.py",
        "restore",
        NULL
    };
    char* modules_tcl[] = {
        "/usr/bin/tclsh",
        "/usr/share/Modules/libexec/modulecmd.tcl",
        "bash",
        "list",
        NULL
    };
    char* app_python[] = {
        "/usr/bin/python3",
        "train.py",
        NULL
    };
    char* app_lua[] = {
        "/usr/bin/lua5.4",
        "simulate.lua",
        NULL
    };
    char* timeout_wrapper[] = {
        "timeout",
        "50m",
        "ibrun",
        "./su3_rhmd_hisq.skx",
        NULL
    };
    char* timeout_path_wrapper[] = {
        "/usr/bin/timeout",
        "50m",
        "ibrun",
        "./su3_rhmd_hisq.skx",
        NULL
    };
    char* milc_app[] = {
        "./su3_rhmd_hisq.skx",
        NULL
    };
    char* python_app[] = {
        "/usr/bin/python3",
        "train.py",
        NULL
    };
    char* node_app[] = {
        "node",
        "server.js",
        NULL
    };

    ok &= expect_filtered("/opt/apps/lmod/lmod/libexec/modulecmd");
    ok &= expect_filtered("/usr/bin/env");
    ok &= expect_filtered("timeout");
    ok &= expect_filtered("/usr/bin/time");
    ok &= expect_filtered("tee");
    ok &= expect_filtered("ln");
    ok &= expect_filtered("/usr/bin/ln");
    ok &= expect_filtered("/bin/stty");
    ok &= expect_filtered("tty");
    ok &= expect_filtered("tput");
    ok &= expect_filtered("basename");
    ok &= expect_filtered("readlink");
    ok &= expect_filtered("xargs");
    ok &= expect_filtered("test");
    ok &= expect_filtered("[");
    ok &= expect_filtered("ibrun");
    ok &= expect_profiled("/usr/bin/lua");
    ok &= expect_profiled("lua5.4");
    ok &= expect_profiled("/usr/bin/python3");
    ok &= expect_profiled("/usr/bin/python3.11");
    ok &= expect_profiled("/usr/bin/perl");
    ok &= expect_profiled("./su3_rhmd_hisq.skx");
    ok &= expect_profiled("/scratch/app/my-science-code");
    ok &= expect_interpreter("/usr/bin/lua");
    ok &= expect_interpreter("lua5.4");
    ok &= expect_interpreter("luajit");
    ok &= expect_interpreter("/usr/bin/python3");
    ok &= expect_interpreter("/usr/bin/python3.11");
    ok &= expect_interpreter("/usr/bin/perl5.34");
    ok &= expect_interpreter("tclsh8.6");
    if (check_interpreter_command(NULL)) {
        fprintf(stderr, "expected NULL interpreter command to be ignored\n");
        ok = 0;
    }
    ok &= expect_not_interpreter("./su3_rhmd_hisq.skx");
    ok &= expect_module_helper(3, lmod_lua);
    ok &= expect_module_helper(3, lmod_python);
    ok &= expect_module_helper(4, modules_tcl);
    ok &= expect_not_module_helper(2, app_python);
    ok &= expect_not_module_helper(2, app_lua);

    unsetenv("PEAK_PROFILE_INTERPRETERS");
    unsetenv("PEAK_JIT_ENABLE");
    setenv("PEAK_TARGET_FILE", "/tmp/peak-target-list", 1);
    ok &= expect_profile_decision(4,
                                  timeout_wrapper,
                                  0,
                                  "timeout wrapper with PEAK_TARGET_FILE");
    ok &= expect_profile_decision(4,
                                  timeout_path_wrapper,
                                  0,
                                  "/usr/bin/timeout wrapper");
    ok &= expect_profile_decision(1, milc_app, 1, "MILC app");
    ok &= expect_profile_decision(2, python_app, 0, "python default skip");
    ok &= expect_profile_decision(2, node_app, 0, "node default skip");

    setenv("PEAK_PROFILE_INTERPRETERS", "1", 1);
    ok &= expect_profile_decision(2, python_app, 1, "python opt-in");
    unsetenv("PEAK_PROFILE_INTERPRETERS");

    setenv("PEAK_JIT_ENABLE", "1", 1);
    ok &= expect_profile_decision(2, node_app, 1, "node jit opt-in");
    unsetenv("PEAK_JIT_ENABLE");
    unsetenv("PEAK_TARGET_FILE");

    ok &= expect_requested_work_override(0);
    ok &= expect_requested_work_override(1);

    clear_mpi_env();
    if (check_MPI()) {
        fprintf(stderr, "expected no MPI rank env to stay non-MPI\n");
        ok = 0;
    }
    ok &= expect_mpi_detected_by("PMI_RANK");
    ok &= expect_mpi_detected_by("PMIX_RANK");
    ok &= expect_mpi_detected_by("MV2_COMM_WORLD_RANK");
    ok &= expect_mpi_detected_by("OMPI_COMM_WORLD_RANK");
    ok &= expect_mpi_detected_by("I_MPI_RANK");
    ok &= expect_mpi_not_detected_by("PMI_SIZE");
    ok &= expect_mpi_not_detected_by("PMIX_SIZE");
    ok &= expect_mpi_not_detected_by("MV2_COMM_WORLD_SIZE");
    ok &= expect_mpi_not_detected_by("OMPI_COMM_WORLD_SIZE");
    ok &= expect_mpi_not_detected_by("I_MPI_SIZE");
    ok &= expect_mpi_not_detected_by("SLURM_PROCID");
    ok &= expect_mpi_not_detected_by("SLURM_NTASKS");
    clear_mpi_env();

    if (!ok) {
        return 1;
    }

    puts("command_filter_ok");
    return 0;
}

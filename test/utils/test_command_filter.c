#include "utils.h"

#include <stdio.h>
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

    ok &= expect_filtered("/opt/apps/lmod/lmod/libexec/modulecmd");
    ok &= expect_filtered("/usr/bin/env");
    ok &= expect_filtered("timeout");
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
    ok &= expect_not_interpreter("./su3_rhmd_hisq.skx");
    ok &= expect_module_helper(3, lmod_lua);
    ok &= expect_module_helper(3, lmod_python);
    ok &= expect_module_helper(4, modules_tcl);
    ok &= expect_not_module_helper(2, app_python);
    ok &= expect_not_module_helper(2, app_lua);

    if (!ok) {
        return 1;
    }

    puts("command_filter_ok");
    return 0;
}

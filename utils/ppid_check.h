#ifndef PPID_CHECK_H
#define PPID_CHECK_H

#include <stdio.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

int check_parent_process(char *lock_file, int *need_to_clean);
void remove_ppid_file(char *lock_file);

#endif
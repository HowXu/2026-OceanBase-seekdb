/* Test startup policy through the real constructor, independent of host quota. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "seekdb_omp_config.h"

FILE *__real_fopen(const char *, const char *);
int __real_sched_getaffinity(pid_t, size_t, cpu_set_t *);

FILE *__wrap_fopen(const char *path, const char *mode)
{
  if (strcmp(path, "/sys/fs/cgroup/cpu.max") == 0) {
    const char *text = getenv("TEST_CPU_MAX");
    if (!text) return NULL;
    return fmemopen((void *)text, strlen(text), "r");
  }
  return __real_fopen(path, mode);
}

int __wrap_sched_getaffinity(pid_t pid, size_t bytes, cpu_set_t *set)
{
  const char *count = getenv("TEST_AFFINITY_CPUS");
  if (!count) return -1;
  int cpus = atoi(count);
  assert(bytes == sizeof(*set) && cpus >= 0 && cpus <= CPU_SETSIZE);
  CPU_ZERO(set);
  for (int i = 0; i < cpus; ++i) CPU_SET(i, set);
  (void)pid;
  return 0;
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  const char *value = getenv("OMP_NUM_THREADS");
  if (strcmp(argv[1], "unset") == 0) assert(value == NULL);
  else assert(value != NULL && strcmp(value, argv[1]) == 0);
  printf("PASS startup OpenMP policy: %s\n", argv[1]);
  return 0;
}

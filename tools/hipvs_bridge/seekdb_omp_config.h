#pragma once
/* Keep hipVS's CPU preparation within the container's CPU allocation. */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int seekdb_cuvs_cpu_quota(void)
{
  char value[32], *end;
  unsigned long long period, quota;
  FILE *file = fopen("/sys/fs/cgroup/cpu.max", "r");
  if (!file) return 0;
  int fields = fscanf(file, "%31s %llu", value, &period);
  fclose(file);
  if (fields != 2 || !period || strcmp(value, "max") == 0) return 0;
  quota = strtoull(value, &end, 10);
  if (*end || !quota) return 0;
  unsigned long long threads = quota / period + (quota % period != 0);
  return threads > INT_MAX ? INT_MAX : (int)threads;
}

/* The bridge is linked into seekdb, so this runs before application OpenMP
 * regions initialize their defaults. hipVS creates its own host threads;
 * changing the calling thread's omp_set_num_threads would not cover them. */
__attribute__((constructor)) static void seekdb_cuvs_configure_openmp(void)
{
  if (getenv("OMP_NUM_THREADS") != NULL) return;
  cpu_set_t affinity;
  if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) return;
  int available = CPU_COUNT(&affinity);
  int quota = seekdb_cuvs_cpu_quota();
  if (quota <= 0 || quota >= available) return;
  char value[32];
  snprintf(value, sizeof(value), "%d", quota);
  /* Do not overwrite a setting provided by another initializer. */
  setenv("OMP_NUM_THREADS", value, 0);
}
#endif

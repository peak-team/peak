#define _POSIX_C_SOURCE 200809L
#include "internal/mpi_observer.h"
#include <assert.h>
static void peak_mpi_observer_test_before_publish(PeakMpiObserver *);
#define PEAK_MPI_OBSERVER_TEST_BEFORE_PUBLISH 1
#include "../../src/mpi_observer.c"
#include <pthread.h>
#include <stdio.h>
#include <time.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static unsigned notices;
static int pending, armed, drain_before_publish;
static PeakMpiObserver *observer;
static void notify(void) {
  pthread_mutex_lock(&mutex);
  notices++;
  pending = 1;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);
}
static void peak_mpi_observer_test_before_publish(PeakMpiObserver *o) {
  if (drain_before_publish) {
    PeakMpiObserverRecord r;
    assert(peak_mpi_observer_consume(o, 0, &r));
    drain_before_publish = 0;
  }
}
static void *waiter(void *unused) {
  (void)unused;
  pthread_mutex_lock(&mutex);
  armed = 1;
  pthread_cond_broadcast(&cond);
  while (!pending && !peak_mpi_observer_has_pending(observer))
    pthread_cond_wait(&cond, &mutex);
  assert(pending || peak_mpi_observer_has_pending(observer));
  pthread_mutex_unlock(&mutex);
  return NULL;
}
static unsigned quantum_notices;
static void quantum_notify(void) { ++quantum_notices; }
static void test_quantum(void) {
  const struct {
    unsigned capacity, seed, occupancy, quantum;
  } cases[] = {{2, 1, 1, 1}, {4, 3, 1, 1}, {8, 3, 1, 2},
               {256, 67, 63, 64}, {256, 230, 223, 64}};
  for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
    PeakMpiObserverConfig config = {1, cases[c].capacity, 1, 1};
    PeakMpiObserver *o = peak_mpi_observer_create(&config);
    PeakMpiObserverProducer p;
    PeakMpiObserverRecord r = {.kind = PEAK_MPI_OBSERVER_EVENT_START, .op_uid = 1};
    assert(o && !peak_mpi_observer_claim(o, 0, 1, 1, &p));
    peak_mpi_observer_activate(o);
    peak_mpi_observer_set_notify(o, quantum_notify);
    for (unsigned i = 0; i < cases[c].seed; ++i)
      assert(!peak_mpi_observer_emit(o, &p, &r));
    for (unsigned i = 0; i < cases[c].seed - cases[c].occupancy; ++i)
      assert(peak_mpi_observer_consume(o, 0, &r));
    unsigned before = quantum_notices;
    for (unsigned i = 0; i < 128; ++i) {
      assert(!peak_mpi_observer_emit(o, &p, &r));
      assert(peak_mpi_observer_consume(o, 0, &r));
    }
    assert(quantum_notices - before == 128 / cases[c].quantum);
    peak_mpi_observer_destroy(o);
  }
  puts("small rings/unaligned publication/high occupancy notification bounds PASS");
}

int main(void) {
  test_quantum();
  PeakMpiObserverConfig c = {1, 256, 1, 1};
  observer = peak_mpi_observer_create(&c);
  assert(observer);
  peak_mpi_observer_set_notify(observer, notify);
  peak_mpi_observer_activate(observer);
  PeakMpiObserverProducer p;
  assert(!peak_mpi_observer_claim(observer, 0, 1, 1, &p));
  PeakMpiObserverRecord r = {.kind = PEAK_MPI_OBSERVER_EVENT_START,
                             .op_uid = 1};
  assert(!peak_mpi_observer_emit(observer, &p, &r));
  assert(notices == 1 && peak_mpi_observer_has_pending(observer));
  assert(!peak_mpi_observer_emit(observer, &p, &r));
  assert(notices == 1);
  assert(peak_mpi_observer_consume(observer, 0, &r));
  assert(peak_mpi_observer_consume(observer, 0, &r));
  assert(!peak_mpi_observer_has_pending(observer));
  for (unsigned i = 0; i < 1000; i++) {
    pthread_mutex_lock(&mutex);
    pending = 0;
    armed = 0;
    pthread_mutex_unlock(&mutex);
    pthread_t t;
    assert(!pthread_create(&t, NULL, waiter, NULL));
    pthread_mutex_lock(&mutex);
    while (!armed)
      pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex);
    assert(!peak_mpi_observer_emit(observer, &p, &r));
    assert(!pthread_join(t, NULL));
    assert(peak_mpi_observer_consume(observer, 0, &r));
  }
  assert(notices == 1001);
  assert(!peak_mpi_observer_emit(observer, &p, &r));
  assert(notices == 1002);
  drain_before_publish = 1;
  assert(!peak_mpi_observer_emit(observer, &p, &r));
  assert(notices == 1003);
  assert(peak_mpi_observer_consume(observer, 0, &r));
  assert(!peak_mpi_observer_has_pending(observer));
  peak_mpi_observer_destroy(observer);
  puts("actual SPSC release-before-notify/empty-transition and 1000 protected "
       "sleep/publication races PASS");
}

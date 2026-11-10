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
int main(void) {
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

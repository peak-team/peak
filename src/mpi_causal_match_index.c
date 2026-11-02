#include "internal/mpi_causal_match_index.h"

#include <stdlib.h>

#define NO_INDEX SIZE_MAX
#define HASH_TOMBSTONE SIZE_MAX
#define MAX_PROBES 64u

typedef struct {
  size_t bucket, previous, next;
} MatchMember;
typedef struct {
  PeakMpiCausalMatchKey key;
  size_t head, tail, hash_slot, free_next;
} MatchBucket;
struct PeakMpiCausalMatchIndex {
  size_t max_operations, hash_capacity;
  size_t free_bucket[PEAK_MPI_CAUSAL_MATCH_ROLE_COUNT];
  MatchMember *members;
  MatchBucket *buckets;
  size_t *hash_slots;
};

static int valid_role(PeakMpiCausalMatchRole role) {
  return (unsigned)role < PEAK_MPI_CAUSAL_MATCH_ROLE_COUNT;
}
static int same_key(const PeakMpiCausalMatchKey *a,
                    const PeakMpiCausalMatchKey *b) {
  return a->context == b->context && a->ordinal == b->ordinal &&
         a->source == b->source && a->destination == b->destination &&
         a->tag == b->tag && a->operation == b->operation;
}
static uint64_t mix(uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}
static size_t key_hash(const PeakMpiCausalMatchKey *key) {
  uint64_t x = mix(key->context);
  x = mix(x ^ key->ordinal);
  x = mix(x ^ ((uint64_t)(uint32_t)key->source << 32) ^
          (uint32_t)key->destination);
  x = mix(x ^ ((uint64_t)(uint32_t)key->tag << 32) ^ (uint32_t)key->operation);
  return (size_t)x;
}
/* Empty/tombstone output is meaningful only to insertion. Roles have separate
 * hash and bucket namespaces; no operation can link into another role. */
static size_t find_bucket(const PeakMpiCausalMatchIndex *index,
                          PeakMpiCausalMatchRole role,
                          const PeakMpiCausalMatchKey *key, size_t *insertion) {
  size_t offset = (size_t)role * index->hash_capacity;
  size_t h = key_hash(key);
  if (insertion)
    *insertion = NO_INDEX;
  size_t probes =
      index->hash_capacity < MAX_PROBES ? index->hash_capacity : MAX_PROBES;
  for (size_t probe = 0; probe < probes; probe++) {
    size_t at = offset + ((h + probe) & (index->hash_capacity - 1));
    size_t value = index->hash_slots[at];
    if (!value) {
      if (insertion && *insertion == NO_INDEX)
        *insertion = at;
      return NO_INDEX;
    }
    if (value == HASH_TOMBSTONE) {
      if (insertion && *insertion == NO_INDEX)
        *insertion = at;
    } else if (same_key(&index->buckets[value - 1].key, key)) {
      return value - 1;
    }
  }
  return NO_INDEX;
}

PeakMpiCausalMatchIndex *peak_mpi_causal_match_index_create(size_t count) {
  if (!count || count > SIZE_MAX / (3 * sizeof(MatchMember)) ||
      count > SIZE_MAX / (3 * sizeof(MatchBucket)) || count > SIZE_MAX / 4)
    return NULL;
  size_t hash_capacity = 2;
  while (hash_capacity < count * 2)
    hash_capacity *= 2;
  if (hash_capacity > SIZE_MAX / (3 * sizeof(size_t)))
    return NULL;
  PeakMpiCausalMatchIndex *index = calloc(1, sizeof(*index));
  if (!index)
    return NULL;
  index->max_operations = count;
  index->hash_capacity = hash_capacity;
  index->members = calloc(count * 3, sizeof(*index->members));
  index->buckets = calloc(count * 3, sizeof(*index->buckets));
  index->hash_slots = calloc(hash_capacity * 3, sizeof(*index->hash_slots));
  if (!index->members || !index->buckets || !index->hash_slots) {
    peak_mpi_causal_match_index_destroy(index);
    return NULL;
  }
  for (unsigned role = 0; role < PEAK_MPI_CAUSAL_MATCH_ROLE_COUNT; role++) {
    size_t offset = (size_t)role * count;
    index->free_bucket[role] = offset;
    for (size_t slot = 0; slot < count; slot++) {
      MatchMember *member = &index->members[offset + slot];
      member->bucket = member->previous = member->next = NO_INDEX;
      index->buckets[offset + slot].free_next =
          slot + 1 == count ? NO_INDEX : offset + slot + 1;
    }
  }
  return index;
}
void peak_mpi_causal_match_index_destroy(PeakMpiCausalMatchIndex *index) {
  if (!index)
    return;
  free(index->members);
  free(index->buckets);
  free(index->hash_slots);
  free(index);
}
int peak_mpi_causal_match_index_insert(PeakMpiCausalMatchIndex *index,
                                       PeakMpiCausalMatchRole role, size_t slot,
                                       const PeakMpiCausalMatchKey *key) {
  if (!index || !key || !valid_role(role) || slot >= index->max_operations)
    return 0;
  size_t member_offset = (size_t)role * index->max_operations;
  MatchMember *member = &index->members[member_offset + slot];
  if (member->bucket != NO_INDEX)
    return same_key(&index->buckets[member->bucket].key, key);
  size_t insertion = NO_INDEX;
  size_t bucket_index = find_bucket(index, role, key, &insertion);
  if (bucket_index == NO_INDEX) {
    if (insertion == NO_INDEX || index->free_bucket[role] == NO_INDEX)
      return 0;
    bucket_index = index->free_bucket[role];
    MatchBucket *bucket = &index->buckets[bucket_index];
    index->free_bucket[role] = bucket->free_next;
    bucket->key = *key;
    bucket->head = bucket->tail = NO_INDEX;
    bucket->hash_slot = insertion;
    index->hash_slots[insertion] = bucket_index + 1;
  }
  MatchBucket *bucket = &index->buckets[bucket_index];
  member->bucket = bucket_index;
  member->previous = bucket->tail;
  member->next = NO_INDEX;
  if (bucket->tail == NO_INDEX)
    bucket->head = slot;
  else
    index->members[member_offset + bucket->tail].next = slot;
  bucket->tail = slot;
  return 1;
}
void peak_mpi_causal_match_index_remove_role(PeakMpiCausalMatchIndex *index,
                                             PeakMpiCausalMatchRole role,
                                             size_t slot) {
  if (!index || !valid_role(role) || slot >= index->max_operations)
    return;
  size_t offset = (size_t)role * index->max_operations;
  MatchMember *member = &index->members[offset + slot];
  if (member->bucket == NO_INDEX)
    return;
  size_t bucket_index = member->bucket;
  MatchBucket *bucket = &index->buckets[bucket_index];
  if (member->previous == NO_INDEX)
    bucket->head = member->next;
  else
    index->members[offset + member->previous].next = member->next;
  if (member->next == NO_INDEX)
    bucket->tail = member->previous;
  else
    index->members[offset + member->next].previous = member->previous;
  member->bucket = member->previous = member->next = NO_INDEX;
  if (bucket->head == NO_INDEX) {
    index->hash_slots[bucket->hash_slot] = HASH_TOMBSTONE;
    bucket->free_next = index->free_bucket[role];
    index->free_bucket[role] = bucket_index;
  }
}
void peak_mpi_causal_match_index_remove_all(PeakMpiCausalMatchIndex *index,
                                            size_t slot) {
  for (unsigned role = 0; role < PEAK_MPI_CAUSAL_MATCH_ROLE_COUNT; role++)
    peak_mpi_causal_match_index_remove_role(index, (PeakMpiCausalMatchRole)role,
                                            slot);
}
int peak_mpi_causal_match_index_contains(const PeakMpiCausalMatchIndex *index,
                                         PeakMpiCausalMatchRole role,
                                         size_t slot) {
  return index && valid_role(role) && slot < index->max_operations &&
         index->members[(size_t)role * index->max_operations + slot].bucket !=
             NO_INDEX;
}
size_t peak_mpi_causal_match_index_first(const PeakMpiCausalMatchIndex *index,
                                         PeakMpiCausalMatchRole role,
                                         const PeakMpiCausalMatchKey *key) {
  if (!index || !key || !valid_role(role))
    return NO_INDEX;
  size_t bucket = find_bucket(index, role, key, NULL);
  return bucket == NO_INDEX ? NO_INDEX : index->buckets[bucket].head;
}
size_t peak_mpi_causal_match_index_next(const PeakMpiCausalMatchIndex *index,
                                        PeakMpiCausalMatchRole role,
                                        size_t slot) {
  return peak_mpi_causal_match_index_contains(index, role, slot)
             ? index->members[(size_t)role * index->max_operations + slot].next
             : NO_INDEX;
}

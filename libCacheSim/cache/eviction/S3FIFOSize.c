//
//  size-aware S3-FIFO
//  admit large object with probability
//
//  S3FIFOSize.c
//  libCacheSim
//
//  Created by Juncheng on 12/4/22.
//  Copyright © 2018 Juncheng. All rights reserved.
//

#include "../../dataStructure/hashtable/hashtable.h"
#include "../../include/libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

// PROB_ADMISSION1 does not work well
// #define PROB_ADMISSION1
#define PROB_ADMISSION2
// #define USE_FILTER

typedef struct {
  cache_t *fifo;
  cache_t *fifo_ghost;
  cache_t *main_cache;

  int64_t n_obj_admit_to_fifo;
  int64_t n_obj_admit_to_main;
  int64_t n_obj_move_to_main;
  int64_t n_byte_admit_to_fifo;
  int64_t n_byte_admit_to_main;
  int64_t n_byte_move_to_main;

  int move_to_main_threshold;
  double fifo_size_ratio;
  double ghost_size_ratio;
  char main_cache_type[32];

  bool has_evicted;
  request_t *req_local;
} S3FIFOSize_params_t;

static const char *DEFAULT_CACHE_PARAMS = "fifo-size-ratio=0.20,ghost-size-ratio=0.80,move-to-main-threshold=1";

// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************
cache_t *S3FIFOSize_init(const common_cache_params_t ccache_params, const char *cache_specific_params);
static void S3FIFOSize_free(cache_t *cache);
static bool S3FIFOSize_get(cache_t *cache, const request_t *req);

static cache_obj_t *S3FIFOSize_find(cache_t *cache, const request_t *req, const bool update_cache);
static cache_obj_t *S3FIFOSize_insert(cache_t *cache, const request_t *req);
static cache_obj_t *S3FIFOSize_to_evict(cache_t *cache, const request_t *req);
static void S3FIFOSize_evict(cache_t *cache, const request_t *req);
static bool S3FIFOSize_remove(cache_t *cache, const obj_id_t obj_id);
static inline int64_t S3FIFOSize_get_occupied_byte(const cache_t *cache);
static inline int64_t S3FIFOSize_get_n_obj(const cache_t *cache);
static inline bool S3FIFOSize_can_insert(cache_t *cache, const request_t *req);
static void S3FIFOSize_parse_params(cache_t *cache, const char *cache_specific_params);

static void S3FIFOSize_evict_fifo(cache_t *cache, const request_t *req);
static void S3FIFOSize_evict_main(cache_t *cache, const request_t *req);

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ***********************************************************************

cache_t *S3FIFOSize_init(const common_cache_params_t ccache_params, const char *cache_specific_params) {
  cache_t *cache = cache_struct_init("S3FIFOSize", ccache_params, cache_specific_params);
  cache->cache_init = S3FIFOSize_init;
  cache->cache_free = S3FIFOSize_free;
  cache->get = S3FIFOSize_get;
  cache->find = S3FIFOSize_find;
  cache->insert = S3FIFOSize_insert;
  cache->evict = S3FIFOSize_evict;
  cache->remove = S3FIFOSize_remove;
  cache->to_evict = S3FIFOSize_to_evict;
  cache->get_n_obj = S3FIFOSize_get_n_obj;
  cache->get_occupied_byte = S3FIFOSize_get_occupied_byte;
  cache->can_insert = S3FIFOSize_can_insert;

  cache->obj_md_size = 0;

  cache->eviction_params = malloc(sizeof(S3FIFOSize_params_t));
  memset(cache->eviction_params, 0, sizeof(S3FIFOSize_params_t));
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  params->req_local = new_request();

  S3FIFOSize_parse_params(cache, DEFAULT_CACHE_PARAMS);
  if (cache_specific_params != NULL) {
    S3FIFOSize_parse_params(cache, cache_specific_params);
  }

  int64_t fifo_cache_size = (int64_t)ccache_params.cache_size * params->fifo_size_ratio;
  int64_t main_cache_size = ccache_params.cache_size - fifo_cache_size;
  int64_t fifo_ghost_cache_size = (int64_t)(ccache_params.cache_size * params->ghost_size_ratio);

  common_cache_params_t ccache_params_local = ccache_params;
  ccache_params_local.cache_size = fifo_cache_size;
  params->fifo = FIFO_init(ccache_params_local, NULL);
  params->has_evicted = false;

  if (fifo_ghost_cache_size > 0) {
    ccache_params_local.cache_size = fifo_ghost_cache_size;
    params->fifo_ghost = FIFO_init(ccache_params_local, NULL);
    snprintf(params->fifo_ghost->cache_name, CACHE_NAME_ARRAY_LEN, "FIFO-ghost");
  } else {
    params->fifo_ghost = NULL;
  }

  ccache_params_local.cache_size = main_cache_size;
  params->main_cache = FIFO_init(ccache_params_local, NULL);

#if defined(TRACK_EVICTION_V_AGE)
  if (params->fifo_ghost != NULL) {
    params->fifo_ghost->track_eviction_age = false;
  }
  params->fifo->track_eviction_age = false;
  params->main_cache->track_eviction_age = false;
#endif

  snprintf(cache->cache_name, CACHE_NAME_ARRAY_LEN, "S3FIFOSize-%.4lf-%d", params->fifo_size_ratio,
           params->move_to_main_threshold);

  return cache;
}

/**
 * free resources used by this cache
 *
 * @param cache
 */
static void S3FIFOSize_free(cache_t *cache) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  free_request(params->req_local);
  params->fifo->cache_free(params->fifo);
  if (params->fifo_ghost != NULL) {
    params->fifo_ghost->cache_free(params->fifo_ghost);
  }
  params->main_cache->cache_free(params->main_cache);
  free(cache->eviction_params);
  cache_struct_free(cache);
}

/**
 * @brief this function is the user facing API
 * it performs the following logic
 *
 * ```
 * if obj in cache:
 *    update_metadata
 *    return true
 * else:
 *    if cache does not have enough space:
 *        evict until it has space to insert
 *    insert the object
 *    return false
 * ```
 *
 * @param cache
 * @param req
 * @return true if cache hit, false if cache miss
 */
static bool S3FIFOSize_get(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  DEBUG_ASSERT(params->fifo->get_occupied_byte(params->fifo) +
                   params->main_cache->get_occupied_byte(params->main_cache) <=
               cache->cache_size);

  cache->n_req += 1;
  // bool cache_hit = cache_get_base(cache, req);

  cache_obj_t *obj = cache->find(cache, req, true);
  bool cache_hit = (obj != NULL);

  if (!cache_hit) {
    if (!cache->can_insert(cache, req)) {
      // params->fifo_ghost->insert(params->fifo_ghost, req);
    } else {
      while (cache->get_occupied_byte(cache) + req->obj_size + cache->obj_md_size > cache->cache_size) {
        cache->evict(cache, req);
      }
      cache->insert(cache, req);
    }
  }
  return cache_hit;
}

// ***********************************************************************
// ****                                                               ****
// ****       developer facing APIs (used by cache developer)         ****
// ****                                                               ****
// ***********************************************************************
/**
 * @brief find an object in the cache
 *
 * @param cache
 * @param req
 * @param update_cache whether to update the cache,
 *  if true, the object is promoted
 *  and if the object is expired, it is removed from the cache
 * @return the object or NULL if not found
 */
static cache_obj_t *S3FIFOSize_find(cache_t *cache, const request_t *req, const bool update_cache) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;

  // if update cache is false, we only check the fifo and main caches
  if (!update_cache) {
    cache_obj_t *obj = params->fifo->find(params->fifo, req, false);
    if (obj != NULL) {
      return obj;
    }
    obj = params->main_cache->find(params->main_cache, req, false);
    if (obj != NULL) {
      return obj;
    }
    return NULL;
  }

  /* update cache is true from now */
  cache_obj_t *obj = params->fifo->find(params->fifo, req, true);
  if (obj != NULL) {
#ifdef USE_FILTER
    if (params->n_byte_admit_to_fifo - obj->S3FIFO.insertion_time > params->fifo->cache_size / 2) {
      obj->S3FIFO.freq += 1;
      // printf("incr %ld %ld %ld\n", params->n_byte_admit_to_fifo/10000000, obj->S3FIFO.insertion_time/10000000,
      // params->fifo->get_occupied_byte(params->fifo)/10000000);
      // } else {
      //   printf("no incr %ld %ld %ld\n", params->n_byte_admit_to_fifo/10000000, obj->S3FIFO.insertion_time/10000000,
      //   params->fifo->get_occupied_byte(params->fifo)/10000000);
    }
#else
    obj->S3FIFO.freq += 1;
#endif
    return obj;
  }

  obj = params->main_cache->find(params->main_cache, req, true);
  if (obj != NULL) {
    obj->S3FIFO.freq += 1;
  }

  return obj;
}

/**
 * @brief insert an object into the cache,
 * update the hash table and cache metadata
 * this function assumes the cache has enough space
 * eviction should be
 * performed before calling this function
 *
 * @param cache
 * @param req
 * @return the inserted object
 */
static cache_obj_t *S3FIFOSize_insert(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  cache_obj_t *obj = NULL;

  cache_obj_t *ghost_obj = params->fifo_ghost->find(params->fifo_ghost, req, false);

  if (ghost_obj != NULL) {
    double mean_obj_size_in_small =
        params->fifo->get_n_obj(params->fifo) == 0
            ? 100000000.0
            : params->fifo->get_occupied_byte(params->fifo) / params->fifo->get_n_obj(params->fifo);
    double mean_obj_size_in_main =
        params->main_cache->get_occupied_byte(params->main_cache) / params->main_cache->get_n_obj(params->main_cache);
    double ratio = (double)req->obj_size / mean_obj_size_in_small;

    if ((ghost_obj->S3FIFO.freq + 1) / ratio >= params->move_to_main_threshold + 1) {
      // insert to main
      params->n_obj_admit_to_main += 1;
      params->n_byte_admit_to_main += req->obj_size;
      obj = params->main_cache->insert(params->main_cache, req);
    } else {
      // insert to small FIFO
      params->n_obj_admit_to_fifo += 1;
      params->n_byte_admit_to_fifo += req->obj_size;
      obj = params->fifo->insert(params->fifo, req);
      obj->S3FIFO.insertion_time = params->n_byte_admit_to_fifo;
    }
    params->fifo_ghost->remove(params->fifo_ghost, req->obj_id);
    // obj->S3FIFO.freq == ghost_obj->S3FIFO.freq + 1;
    obj->S3FIFO.freq == 0;
  } else {
    /* insert into the fifo */
    // #ifdef PROB_ADMISSION1
    //     double r = (double) (params->fifo->cache_size) / (req->obj_size);
    //     if (next_rand_double() < exp(-r)) {
    //       params->fifo_ghost->insert(params->fifo_ghost, req);
    //       return NULL;
    //     }
    // #elif defined(PROB_ADMISSION2)
    //     double r = (double)(req->obj_size) / params->fifo->cache_size;
    //     if (next_rand_double() < r) {
    //       printf("skip insert to fifo obj size %ld\n", req->obj_size);
    //       params->fifo_ghost->insert(params->fifo_ghost, req);
    //       return NULL;
    //     }
    // #else
    //     if (req->obj_size >= params->fifo->cache_size) {
    //       params->fifo_ghost->insert(params->fifo_ghost, req);
    //       return NULL;
    //     }
    // #endif

    if (!params->has_evicted && params->fifo->get_occupied_byte(params->fifo) >= params->fifo->cache_size) {
      params->n_obj_admit_to_main += 1;
      params->n_byte_admit_to_main += req->obj_size;
      obj = params->main_cache->insert(params->main_cache, req);
    } else {
      params->n_obj_admit_to_fifo += 1;
      params->n_byte_admit_to_fifo += req->obj_size;
      obj = params->fifo->insert(params->fifo, req);
      obj->S3FIFO.insertion_time = params->n_byte_admit_to_fifo;
    }

    obj->S3FIFO.freq == 0;
  }

#if defined(TRACK_EVICTION_V_AGE)
  obj->create_time = CURR_TIME(cache, req);
#endif

#if defined(TRACK_DEMOTION)
  obj->create_time = cache->n_req;
#endif

  return obj;
}

/**
 * @brief find the object to be evicted
 * this function does not actually evict the object or update metadata
 * not all eviction algorithms support this function
 * because the eviction logic cannot be decoupled from finding eviction
 * candidate, so use assert(false) if you cannot support this function
 *
 * @param cache the cache
 * @return the object to be evicted
 */
static cache_obj_t *S3FIFOSize_to_evict(cache_t *cache, const request_t *req) {
  assert(false);
  return NULL;
}

static void S3FIFOSize_evict_fifo(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  cache_t *fifo = params->fifo;
  cache_t *ghost = params->fifo_ghost;
  cache_t *main = params->main_cache;
  static __thread int cond1_cnt = 0, cond2_cnt = 0;
  static __thread int64_t total_cnt = 0;

  bool has_evicted = false;
  while (!has_evicted && fifo->get_occupied_byte(fifo) > 0) {
    // evict from FIFO
    cache_obj_t *obj_to_evict = fifo->to_evict(fifo, req);
    DEBUG_ASSERT(obj_to_evict != NULL);
    // need to copy the object before it is evicted
    copy_cache_obj_to_request(params->req_local, obj_to_evict);

    int64_t obj_size = obj_to_evict->obj_size;
    int64_t mean_obj_size_in_fifo = fifo->get_occupied_byte(fifo) / fifo->get_n_obj(fifo);
    int64_t mean_obj_size_in_main = main->get_occupied_byte(main) / main->get_n_obj(main);
    int64_t mean_obj_size = cache->cache_size / cache->get_n_obj(cache);
    double ratio = (double)obj_size / mean_obj_size;
    // printf("obj_size %ld mean_obj_size_in_fifo %ld mean_obj_size_in_main %ld mean_obj_size %ld ratio %.2lf\n",
    // obj_size, mean_obj_size_in_fifo, mean_obj_size_in_main, mean_obj_size, ratio);
    bool cond1 = obj_to_evict->S3FIFO.freq >= params->move_to_main_threshold;
    bool cond2 = obj_to_evict->S3FIFO.freq / ratio >= params->move_to_main_threshold;
    if (cond1) {
      cond1_cnt += 1;
    }
    if (cond2) {
      cond2_cnt += 1;
    }
    total_cnt += 1;
    // if (rand() % 100000 == 0) {
    //   printf("%lu cond1 %d cond2 %d / %ld\n", cache->cache_size, cond1_cnt, cond2_cnt, total_cnt);
    // }

    if ((obj_to_evict->S3FIFO.freq + 1) / ratio >= params->move_to_main_threshold + 1) {
#if defined(TRACK_DEMOTION)
      printf("%ld keep %ld %ld\n", cache->n_req, obj_to_evict->create_time, obj_to_evict->misc.next_access_vtime);
#endif
      // freq is updated in cache_find_base
      params->n_obj_move_to_main += 1;
      params->n_byte_move_to_main += obj_to_evict->obj_size;

      cache_obj_t *new_obj = main->insert(main, params->req_local);
      // printf("---- reinsert fifo %ld\n", new_obj->obj_id);
      new_obj->misc.freq = obj_to_evict->misc.freq;
#if defined(TRACK_EVICTION_V_AGE)
      new_obj->create_time = obj_to_evict->create_time;
    } else {
      record_eviction_age(cache, obj_to_evict, CURR_TIME(cache, req) - obj_to_evict->create_time);
#else
    } else {
#endif

#if defined(TRACK_DEMOTION)
      printf("%ld demote %ld %ld\n", cache->n_req, obj_to_evict->create_time, obj_to_evict->misc.next_access_vtime);
#endif

      // insert to ghost
      if (ghost != NULL) {
        ghost->get(ghost, params->req_local);
        cache_obj_t *ghost_obj = ghost->find(ghost, params->req_local, false);
        ghost_obj->S3FIFO.freq = obj_to_evict->S3FIFO.freq;
      }
      has_evicted = true;
    }

    // remove from fifo, but do not update stat
    bool removed = fifo->remove(fifo, params->req_local->obj_id);
    assert(removed);
  }
}

static void S3FIFOSize_evict_main(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  cache_t *fifo = params->fifo;
  cache_t *ghost = params->fifo_ghost;
  cache_t *main = params->main_cache;

  // evict from main cache
  bool has_evicted = false;
  while (!has_evicted && main->get_occupied_byte(main) > 0) {
    cache_obj_t *obj_to_evict = main->to_evict(main, req);
    DEBUG_ASSERT(obj_to_evict != NULL);
    int freq = obj_to_evict->S3FIFO.freq;
#if defined(TRACK_EVICTION_V_AGE)
    int64_t create_time = obj_to_evict->create_time;
#endif
    copy_cache_obj_to_request(params->req_local, obj_to_evict);
    double mean_obj_size = cache->get_occupied_byte(cache) / cache->get_n_obj(cache);
    double ratio = (double)obj_to_evict->obj_size / mean_obj_size;
    if ((double)freq / ratio >= 1) {
      // we need to evict first because the object to insert has the same obj_id
      // main->evict(main, req);
      main->remove(main, obj_to_evict->obj_id);
      obj_to_evict = NULL;

      // printf("---- reinsert main %ld\n", params->req_local->obj_id);
      cache_obj_t *new_obj = main->insert(main, params->req_local);
      // clock with 2-bit counter
      new_obj->S3FIFO.freq = MIN(freq, 3) - 1;
      new_obj->misc.freq = freq;

#if defined(TRACK_EVICTION_V_AGE)
      new_obj->create_time = create_time;
#endif
    } else {
#if defined(TRACK_EVICTION_V_AGE)
      record_eviction_age(cache, obj_to_evict, CURR_TIME(cache, req) - obj_to_evict->create_time);
#endif

      // printf("---- evict main %ld\n", obj_to_evict->obj_id);
      // main->evict(main, req);
      bool removed = main->remove(main, obj_to_evict->obj_id);
      if (!removed) {
        ERROR("cannot remove obj %ld\n", obj_to_evict->obj_id);
      }

      has_evicted = true;
    }
  }
}

/**
 * @brief evict an object from the cache
 * it needs to call cache_evict_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param req not used
 * @param evicted_obj if not NULL, return the evicted object to caller
 */
static void S3FIFOSize_evict(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  params->has_evicted = true;

  cache_t *fifo = params->fifo;
  cache_t *ghost = params->fifo_ghost;
  cache_t *main = params->main_cache;

  if (main->get_occupied_byte(main) > main->cache_size || fifo->get_occupied_byte(fifo) == 0) {
    return S3FIFOSize_evict_main(cache, req);
  }
  return S3FIFOSize_evict_fifo(cache, req);
}

/**
 * @brief remove an object from the cache
 * this is different from cache_evict because it is used to for user trigger
 * remove, and eviction is used by the cache to make space for new objects
 *
 * it needs to call cache_remove_obj_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param obj_id
 * @return true if the object is removed, false if the object is not in the
 * cache
 */
static bool S3FIFOSize_remove(cache_t *cache, const obj_id_t obj_id) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  bool removed = false;
  removed = removed || params->fifo->remove(params->fifo, obj_id);
  removed = removed || (params->fifo_ghost && params->fifo_ghost->remove(params->fifo_ghost, obj_id));
  removed = removed || params->main_cache->remove(params->main_cache, obj_id);

  return removed;
}

static inline int64_t S3FIFOSize_get_occupied_byte(const cache_t *cache) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  return params->fifo->get_occupied_byte(params->fifo) + params->main_cache->get_occupied_byte(params->main_cache);
}

static inline int64_t S3FIFOSize_get_n_obj(const cache_t *cache) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  return params->fifo->get_n_obj(params->fifo) + params->main_cache->get_n_obj(params->main_cache);
}

static inline bool can_insert_to_small(const cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;

#ifdef PROB_ADMISSION1
  double r = (double)(params->fifo->cache_size) / (req->obj_size);
  if (next_rand_double() < exp(-r)) {
    return false;
  }
#elif defined(PROB_ADMISSION2)
  double r = (double)(req->obj_size) / params->fifo->cache_size;
  if (next_rand_double() < r) {
    return false;
  }
#else
  if (req->obj_size >= params->fifo->cache_size) {
    return false;
  }
#endif
  return true;
}

static inline bool S3FIFOSize_can_insert(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;

  bool will_insert_to_small = false;

  cache_obj_t *ghost_obj = params->fifo_ghost->find(params->fifo_ghost, req, false);

  if (ghost_obj != NULL) {
    double mean_obj_size_in_small =
        params->fifo->get_occupied_byte(params->fifo) / params->fifo->get_n_obj(params->fifo);
    double ratio = (double)req->obj_size / mean_obj_size_in_small;

    if ((ghost_obj->S3FIFO.freq + 1) / ratio >= params->move_to_main_threshold + 1) {
      if (req->obj_size >= params->main_cache->cache_size) {
        return false;
      }
    } else {
      if (!can_insert_to_small(cache, req)) {
        return false;
      }
    }
  } else {
    if (!can_insert_to_small(cache, req)) {
      return false;
    }
  }

  return cache_can_insert_default(cache, req);
}

// ***********************************************************************
// ****                                                               ****
// ****                parameter set up functions                     ****
// ****                                                               ****
// ***********************************************************************
static const char *S3FIFOSize_current_params(S3FIFOSize_params_t *params) {
  static __thread char params_str[128];
  snprintf(params_str, 128, "fifo-size-ratio=%.4lf,ghost-size-ratio=%.4lf,move-to-main-threshold=%d\n",
           params->fifo_size_ratio, params->ghost_size_ratio, params->move_to_main_threshold);
  return params_str;
}

static void S3FIFOSize_parse_params(cache_t *cache, const char *cache_specific_params) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)(cache->eviction_params);

  char *params_str = strdup(cache_specific_params);
  char *old_params_str = params_str;
  char *end;

  while (params_str != NULL && params_str[0] != '\0') {
    /* different parameters are separated by comma,
     * key and value are separated by = */
    char *key = strsep((char **)&params_str, "=");
    char *value = strsep((char **)&params_str, ",");

    // skip the white space
    while (params_str != NULL && *params_str == ' ') {
      params_str++;
    }

    if (strcasecmp(key, "fifo-size-ratio") == 0) {
      params->fifo_size_ratio = strtod(value, NULL);
    } else if (strcasecmp(key, "ghost-size-ratio") == 0) {
      params->ghost_size_ratio = strtod(value, NULL);
    } else if (strcasecmp(key, "move-to-main-threshold") == 0) {
      params->move_to_main_threshold = atoi(value);
    } else if (strcasecmp(key, "print") == 0) {
      printf("parameters: %s\n", S3FIFOSize_current_params(params));
      exit(0);
    } else {
      ERROR("%s does not have parameter %s\n", cache->cache_name, key);
      exit(1);
    }
  }

  free(old_params_str);
}

#ifdef __cplusplus
}
#endif
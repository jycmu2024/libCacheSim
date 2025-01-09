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

#define PROB_ADMISSION
// #define USE_FILTER

typedef struct {
  cache_t *small;
  cache_t *ghost;
  cache_t *main;

  int64_t n_obj_admit_to_fifo;
  int64_t n_obj_admit_to_main;
  int64_t n_obj_move_to_main;
  int64_t n_byte_admit_to_fifo;
  int64_t n_byte_admit_to_main;
  int64_t n_byte_move_to_main;

  int move_to_main_threshold;
  double small_size_ratio;
  double ghost_size_ratio;

  bool has_evicted;
  request_t *req_local;
} S3FIFOSize_params_t;

static const char *DEFAULT_CACHE_PARAMS = "small-size-ratio=0.10,ghost-size-ratio=0.90,move-to-main-threshold=1";

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

  int64_t fifo_cache_size = (int64_t)ccache_params.cache_size * params->small_size_ratio;
  int64_t main_size = ccache_params.cache_size - fifo_cache_size;
  int64_t ghost_cache_size = (int64_t)(ccache_params.cache_size * params->ghost_size_ratio);

  common_cache_params_t ccache_params_local = ccache_params;
  ccache_params_local.cache_size = fifo_cache_size;
  params->small = FIFO_init(ccache_params_local, NULL);
  // params->small = GDSF_init(ccache_params_local, NULL);
  params->has_evicted = false;

  if (ghost_cache_size > 0) {
    ccache_params_local.cache_size = ghost_cache_size;
    params->ghost = FIFO_init(ccache_params_local, NULL);
    snprintf(params->ghost->cache_name, CACHE_NAME_ARRAY_LEN, "FIFO-ghost");
  } else {
    params->ghost = NULL;
  }

  ccache_params_local.cache_size = main_size;
  params->main = FIFO_init(ccache_params_local, NULL);

  snprintf(cache->cache_name, CACHE_NAME_ARRAY_LEN, "S3FIFOSize-%.4lf-%d", params->small_size_ratio,
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
  params->small->cache_free(params->small);
  if (params->ghost != NULL) {
    params->ghost->cache_free(params->ghost);
  }
  params->main->cache_free(params->main);
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
  DEBUG_ASSERT(params->small->get_occupied_byte(params->small) + params->main->get_occupied_byte(params->main) <=
               cache->cache_size);

  cache->n_req += 1;

  cache_obj_t *obj = cache->find(cache, req, true);
  bool cache_hit = (obj != NULL);

  if (!cache_hit) {
    if (!cache->can_insert(cache, req)) {
      obj = params->ghost->find(params->ghost, req, false);
      if (obj == NULL) {
        obj = params->ghost->insert(params->ghost, req);
        obj->S3FIFO.freq = 1;
      } else {
        obj->S3FIFO.freq += 1;
      }

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
    cache_obj_t *obj = params->small->find(params->small, req, false);
    if (obj != NULL) {
      return obj;
    }
    obj = params->main->find(params->main, req, false);
    if (obj != NULL) {
      return obj;
    }
    return NULL;
  }

  /* update cache is true from now */
  cache_obj_t *obj = params->small->find(params->small, req, true);
  if (obj != NULL) {
#ifdef USE_FILTER
    if (params->n_byte_admit_to_fifo - obj->S3FIFO.insertion_time > params->small->cache_size / 2) {
      obj->S3FIFO.freq += 1;
      // } else {
      //   params->small->get_occupied_byte(params->small)/10000000);
    }
#else
    obj->S3FIFO.freq += 1;
#endif
    return obj;
  }

  obj = params->main->find(params->main, req, true);
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

  cache_t *small_q = params->small;
  cache_t *main_q = params->main;
  cache_t *ghost_q = params->ghost;

  double small_q_n_obj = MAX(small_q->get_n_obj(small_q), 1e-8);
  double small_q_byte = small_q->get_occupied_byte(small_q);
  double main_q_n_obj = MAX(main_q->get_n_obj(main_q), 1e-8);
  double main_q_byte = main_q->get_occupied_byte(main_q);
  double cache_n_obj = MAX(cache->get_n_obj(cache), 1e-8);
  double cache_byte = cache->get_occupied_byte(cache);

  double mean_obj_size_in_small = small_q_byte / small_q_n_obj;
  double mean_obj_size_in_main = main_q_byte / main_q_n_obj;
  double mean_obj_size = cache_byte / cache_n_obj;

  cache_obj_t *ghost_obj = ghost_q->find(ghost_q, req, false);
  assert(ghost_obj == NULL || ghost_obj->S3FIFO.freq > 0);

  if (ghost_obj != NULL) {
    // we need to compare with the small queue, because the object has not had chance to accumulate enough hits in the
    // main queue
    double ratio = (double)req->obj_size / mean_obj_size_in_small;
    // TODO: Why >= not larger than???????????????????
    if ((ghost_obj->S3FIFO.freq) / ratio >= params->move_to_main_threshold) {
      // insert to main
      params->n_obj_admit_to_main += 1;
      params->n_byte_admit_to_main += req->obj_size;
      obj = main_q->insert(main_q, req);
      obj->S3FIFO.freq = 1;
    } else {
      // insert to small FIFO
      params->n_obj_admit_to_fifo += 1;
      params->n_byte_admit_to_fifo += req->obj_size;
      obj = small_q->insert(small_q, req);
      obj->S3FIFO.insertion_time = params->n_byte_admit_to_fifo;
      // only keep the frequency when inserting into the small queue
      // the ghost frequency has not been updated
      obj->S3FIFO.freq = ghost_obj->S3FIFO.freq + 1;
    }
    ghost_q->remove(ghost_q, req->obj_id);
  } else {
    if (!params->has_evicted && small_q_byte >= small_q->cache_size) {
      params->n_obj_admit_to_main += 1;
      params->n_byte_admit_to_main += req->obj_size;
      obj = main_q->insert(main_q, req);
    } else {
      params->n_obj_admit_to_fifo += 1;
      params->n_byte_admit_to_fifo += req->obj_size;
      obj = small_q->insert(small_q, req);
      obj->S3FIFO.insertion_time = params->n_byte_admit_to_fifo;
    }

    obj->S3FIFO.freq = 1;
  }

  // obj->S3FIFO.freq = 0;

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

// evict from FIFO
static void S3FIFOSize_evict_fifo(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  cache_t *small_q = params->small;
  cache_t *main_q = params->main;
  cache_t *ghost_q = params->ghost;

  cache_obj_t *obj_to_evict = small_q->to_evict(small_q, req);
  DEBUG_ASSERT(obj_to_evict != NULL);
  // need to copy the object before it is evicted
  copy_cache_obj_to_request(params->req_local, obj_to_evict);

  double cache_n_obj = MAX(cache->get_n_obj(cache), 1e-8);
  double cache_byte = cache->get_occupied_byte(cache);

  double mean_obj_size = cache_byte / cache_n_obj;

  int64_t obj_size = obj_to_evict->obj_size;
  double ratio = (double)obj_size / mean_obj_size;

  if ((obj_to_evict->S3FIFO.freq) / ratio >= params->move_to_main_threshold) {
    params->n_obj_move_to_main += 1;
    params->n_byte_move_to_main += obj_to_evict->obj_size;

    cache_obj_t *new_obj = main_q->insert(main_q, params->req_local);
    new_obj->S3FIFO.freq = 1;
  } else {
    // insert to ghost
    if (ghost_q != NULL) {
      ghost_q->get(ghost_q, params->req_local);
      cache_obj_t *ghost_obj = ghost_q->find(ghost_q, params->req_local, false);
      ghost_obj->S3FIFO.freq = obj_to_evict->S3FIFO.freq;
    }
  }

  small_q->evict(small_q, params->req_local);
}

// evict from main cache
static void S3FIFOSize_evict_main(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  cache_t *small_q = params->small;
  cache_t *main_q = params->main;
  cache_t *ghost_q = params->ghost;

  double cache_n_obj = MAX(cache->get_n_obj(cache), 1e-8);
  double cache_byte = cache->get_occupied_byte(cache);

  cache_obj_t *obj_to_evict = main_q->to_evict(main_q, req);
  int freq = obj_to_evict->S3FIFO.freq;
  copy_cache_obj_to_request(params->req_local, obj_to_evict);
  double mean_obj_size = cache_byte / cache_n_obj;

  double ratio = (double)obj_to_evict->obj_size / mean_obj_size;
  if ((double)(freq) / ratio >= params->move_to_main_threshold) {
    // we need to evict first because the object to insert has the same obj_id
    main_q->remove(main_q, obj_to_evict->obj_id);
    obj_to_evict = NULL;

    cache_obj_t *new_obj = main_q->insert(main_q, params->req_local);
    // clock with 2-bit counter, 4 is better than 3
    new_obj->S3FIFO.freq = MIN(freq, 4) - 1;
  } else {
    bool removed = main_q->remove(main_q, obj_to_evict->obj_id);
    if (!removed) {
      ERROR("cannot remove obj %ld\n", obj_to_evict->obj_id);
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

  cache_t *small_q = params->small;
  cache_t *main_q = params->main;
  cache_t *ghost_q = params->ghost;

  while (cache->get_occupied_byte(cache) + req->obj_size + cache->obj_md_size > cache->cache_size) {
    // printf("%ld %ld = %ld + %ld\n", cache->n_req, cache->get_occupied_byte(cache), small_q->get_occupied_byte(small_q),
    //        main_q->get_occupied_byte(main_q));
    if (main_q->get_occupied_byte(main_q) > main_q->cache_size || small_q->get_occupied_byte(small_q) == 0) {
      S3FIFOSize_evict_main(cache, req);
    } else {
      S3FIFOSize_evict_fifo(cache, req);
    }
  }
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
  removed = removed || params->small->remove(params->small, obj_id);
  removed = removed || (params->ghost && params->ghost->remove(params->ghost, obj_id));
  removed = removed || params->main->remove(params->main, obj_id);

  return removed;
}

static inline int64_t S3FIFOSize_get_occupied_byte(const cache_t *cache) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  return params->small->get_occupied_byte(params->small) + params->main->get_occupied_byte(params->main);
}

static inline int64_t S3FIFOSize_get_n_obj(const cache_t *cache) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;
  return params->small->get_n_obj(params->small) + params->main->get_n_obj(params->main);
}

static bool can_insert_to_small(const cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;

#ifdef PROB_ADMISSION
  double r = (double)(req->obj_size) / params->small->cache_size;
  if (next_rand_double() < r) {
    return false;
  }
#else
  if (req->obj_size >= params->small->cache_size) {
    return false;
  }
#endif
  return true;
}

static bool S3FIFOSize_can_insert(cache_t *cache, const request_t *req) {
  S3FIFOSize_params_t *params = (S3FIFOSize_params_t *)cache->eviction_params;

  cache_t *small_q = params->small;
  cache_t *main_q = params->main;
  cache_t *ghost_q = params->ghost;

  double small_q_n_obj = MAX(small_q->get_n_obj(small_q), 1e-8);
  double small_q_byte = small_q->get_occupied_byte(small_q);
  double main_q_n_obj = MAX(main_q->get_n_obj(main_q), 1e-8);
  double main_q_byte = main_q->get_occupied_byte(main_q);
  double cache_n_obj = MAX(cache->get_n_obj(cache), 1e-8);
  double cache_byte = cache->get_occupied_byte(cache);

  double mean_obj_size_in_small = small_q_byte / small_q_n_obj;
  double mean_obj_size_in_main = main_q_byte / main_q_n_obj;
  double mean_obj_size = cache_byte / cache_n_obj;
  double ratio = (double)req->obj_size / mean_obj_size_in_small;

  cache_obj_t *ghost_obj = ghost_q->find(ghost_q, req, false);

  if (ghost_obj != NULL) {
    if ((ghost_obj->S3FIFO.freq) / ratio >= params->move_to_main_threshold) {
      if (req->obj_size >= params->main->cache_size) {
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
           params->small_size_ratio, params->ghost_size_ratio, params->move_to_main_threshold);
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

    if (strcasecmp(key, "fifo-size-ratio") == 0 || strcasecmp(key, "small-size-ratio") == 0) {
      params->small_size_ratio = strtod(value, NULL);
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
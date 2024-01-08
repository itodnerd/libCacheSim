/* WATT caching */

#include "../../dataStructure/hashtable/hashtable.h"
#include "../../include/libCacheSim/evictionAlgo.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct WATT_params {
  int n_sample;
} WATT_params_t;

// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************

static void WATT_parse_params(cache_t *cache,
                                    const char *cache_specific_params);
static void WATT_free(cache_t *cache);
static bool WATT_get(cache_t *cache, const request_t *req);
static cache_obj_t *WATT_find(cache_t *cache, const request_t *req,
                                    const bool update_cache);
static cache_obj_t *WATT_insert(cache_t *cache, const request_t *req);
static cache_obj_t *WATT_to_evict(cache_t *cache, const request_t *req);
static void WATT_evict(cache_t *cache, const request_t *req);
static bool WATT_remove(cache_t *cache, const obj_id_t obj_id);

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ***********************************************************************
/**
 * @brief initialize a cache
 *
 * @param ccache_params some common cache parameters
 * @param cache_specific_params cache specific parameters, see parse_params
 * function or use -e "print" with the cachesim binary
 */
cache_t *WATT_init(const common_cache_params_t ccache_params,
                         const char *cache_specific_params) {
  // reduce hash table size to make sampling faster
  common_cache_params_t ccache_params_local = ccache_params;
  ccache_params_local.hashpower = MAX(12, ccache_params_local.hashpower - 8);

  cache_t *cache = cache_struct_init("WATT", ccache_params_local, cache_specific_params);
  cache->cache_init = WATT_init;
  cache->cache_free = WATT_free;
  cache->get = WATT_get;
  cache->find = WATT_find;
  cache->insert = WATT_insert;
  cache->evict = WATT_evict;
  cache->remove = WATT_remove;
  cache->to_evict = WATT_to_evict;

  WATT_params_t *params = my_malloc(WATT_params_t);
  params->n_sample = 64;
  cache->eviction_params = params;

  if (cache_specific_params != NULL) {
    WATT_parse_params(cache, cache_specific_params);
  }

  if (ccache_params.consider_obj_metadata) {
    // freq + age
    cache->obj_md_size = 8 + 8;
  } else {
    cache->obj_md_size = 0;
  }

  return cache;
}

/**
 * free resources used by this cache
 *
 * @param cache
 */
static void WATT_free(cache_t *cache) {
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
static bool WATT_get(cache_t *cache, const request_t *req) {
  bool ret = cache_get_base(cache, req);

  return ret;
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
static cache_obj_t *WATT_find(cache_t *cache, const request_t *req,
                                    const bool update_cache) {
  cache_obj_t *cache_obj = cache_find_base(cache, req, update_cache);

  if (update_cache && cache_obj) {
    int64_t curr_time = cache->n_req;

    int64_t next_pos = (cache_obj->WATT.last_pos +1) %8;
    cache_obj->WATT.accesses[next_pos] = curr_time;
    cache_obj->WATT.last_pos = next_pos;
  }

  return cache_obj;
}

/**
 * @brief insert an object into the cache,
 * update the hash table and cache metadata
 * this function assumes the cache has enough space
 * and eviction is not part of this function
 *
 * @param cache
 * @param req
 * @return the inserted object
 */
static cache_obj_t *WATT_insert(cache_t *cache, const request_t *req) {
  cache_obj_t *cached_obj = cache_insert_base(cache, req);
  cached_obj->WATT.last_pos = 0;
  int64_t curr_time = cache->n_req;
  cached_obj->WATT.accesses[0] = curr_time;
  cached_obj->WATT.accesses[1] = curr_time-3000000;
  cached_obj->WATT.accesses[2] = curr_time-3000000;
  cached_obj->WATT.accesses[3] = curr_time-3000000;
  cached_obj->WATT.accesses[4] = curr_time-3000000;
  cached_obj->WATT.accesses[5] = curr_time-3000000;
  cached_obj->WATT.accesses[6] = curr_time-3000000;
  cached_obj->WATT.accesses[7] = curr_time-3000000;

  return cached_obj;
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
static cache_obj_t *WATT_to_evict(cache_t *cache, const request_t *req) {
  WATT_params_t *params = cache->eviction_params;
  int64_t curr_time = cache->n_req;

  cache_obj_t *best_candidate = NULL, *sampled_obj;
  double worst_candidate_score = 1.0e16, sampled_obj_score;
  for (int i = 0; i < params->n_sample; i++) {
    sampled_obj = hashtable_rand_obj(cache->hashtable);
    double sampled_obj_score = 0.2/ (curr_time - sampled_obj->WATT.accesses[sampled_obj->WATT.last_pos]);
    for (int i =1; i<8; i++){
      double new_value = 1.0*(i+1) / (curr_time - sampled_obj->WATT.accesses[(sampled_obj->WATT.last_pos +8 -i) %8]);
      if (new_value > sampled_obj_score){
        sampled_obj_score = new_value;
      }
    }
    if (worst_candidate_score > sampled_obj_score) {
      best_candidate = sampled_obj;
      worst_candidate_score = sampled_obj_score;
    }
  }

  cache->to_evict_candidate = best_candidate;
  cache->to_evict_candidate_gen_vtime = cache->n_req;

  return best_candidate;
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
static void WATT_evict(cache_t *cache,
                             __attribute__((unused)) const request_t *req) {
  cache_obj_t *obj_to_evict = NULL;
  if (cache->to_evict_candidate_gen_vtime == cache->n_req) {
    obj_to_evict = cache->to_evict_candidate;
  } else {
    obj_to_evict = WATT_to_evict(cache, req);
  }
  cache->to_evict_candidate_gen_vtime = -1;

  if (obj_to_evict == NULL) {
    DEBUG_ASSERT(cache->n_obj == 0);
    WARN("no object can be evicted\n");
  }

  cache_evict_base(cache, obj_to_evict, true);
}

static void WATT_remove_obj(cache_t *cache, cache_obj_t *obj) {
  cache_remove_obj_base(cache, obj, true);
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
static bool WATT_remove(cache_t *cache, const obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) {
    return false;
  }

  WATT_remove_obj(cache, obj);

  return true;
}

// ***********************************************************************
// ****                                                               ****
// ****                parameter set up functions                     ****
// ****                                                               ****
// ***********************************************************************
static const char *WATT_current_params(WATT_params_t *params) {
  static __thread char params_str[128];
  snprintf(params_str, 128, "n-sample=%d\n", params->n_sample);
  return params_str;
}

static void WATT_parse_params(cache_t *cache,
                                    const char *cache_specific_params) {
  char *end;
  WATT_params_t *params = (WATT_params_t *)cache->eviction_params;
  char *params_str = strdup(cache_specific_params);
  char *old_params_str = params_str;

  while (params_str != NULL && params_str[0] != '\0') {
    /* different parameters are separated by comma,
     * key and value are separated by = */
    char *key = strsep((char **)&params_str, "=");
    char *value = strsep((char **)&params_str, ",");

    // skip the white space
    while (params_str != NULL && *params_str == ' ') {
      params_str++;
    }

    if (strcasecmp(key, "n-sample") == 0) {
      params->n_sample = (int)strtol(value, &end, 0);
      if (strlen(end) > 2) {
        ERROR("param parsing error, find string \"%s\" after number\n", end);
      }
    } else if (strcasecmp(key, "print") == 0) {
      printf("parameters: %s\n", WATT_current_params(params));
      exit(0);
    } else {
      ERROR("%s does not have parameter %s, support %s\n", cache->cache_name,
            key, WATT_current_params(params));
      exit(1);
    }
  }

  free(old_params_str);
}

#ifdef __cplusplus
}
#endif

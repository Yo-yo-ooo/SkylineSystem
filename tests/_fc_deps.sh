#!/bin/bash
cd /mnt/c/ZSY/SkylineSystem/kernel/src/fs
awk 'NR>=1280' fc.cpp > /tmp/fc_tail.cpp
for n in g_fc_cpus g_num_active_cpus g_osc_free_lists g_osc_pool_locks g_osc_pool_sizes \
         fc_oscillate_free fc_oscillate_alloc fc_update_averages_internal file_cache_should_evict \
         file_cache_should_cache fc_bad_ptr fc_lru_remove fc_lru_push_back fc_lru_push_front \
         fc_lru_move_to_back fc_entry_free fc_pick_and_unlink_victim fc_kmalloc_with_fallback \
         fc_kcalloc_with_fallback fc_try_evict_for_space fc_crc32_partial fc_collect_stats_cb \
         fc_track_free fc_track_insert fc_update_oscillate g_osc_pool_locks art_delete art_insert art_search; do
  c=$(grep -ac "$n" /tmp/fc_tail.cpp)
  echo "$n $c"
done

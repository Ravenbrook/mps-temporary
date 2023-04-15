/* 
TEST_HEADER
 id = $Id$
 summary = random test of exact references in AWL 
 language = c
 link = testlib.o awlfmt.o
END_HEADER
*/

#include "testlib.h"
#include "mpscawl.h"
#include "mpscamc.h"
#include "awlfmt.h"
#include "mpsavm.h"


#define genCOUNT (3)

static mps_gen_param_s testChain[genCOUNT] = {
  { 6000, 0.90 }, { 8000, 0.65 }, { 16000, 0.50 } };


static void test(void *stack_pointer)
{
 mps_arena_t arena;
 mps_pool_t poolamc, poolawl;
 mps_thr_t thread;
 mps_root_t root;

 mps_fmt_t format;
 mps_chain_t chain;
 mps_ap_t apamc, apawl;

 mycell *a[100];

 int i;
 int j;
 int k,z;

 alloccomments = 1;
 formatcomments = 1;

 cdie(mps_arena_create(&arena, mps_arena_class_vm(), mmqaArenaSIZE),
      "create arena");

 die(mps_thread_reg(&thread, arena), "register thread");
 cdie(mps_root_create_thread(&root, arena, thread, stack_pointer), "thread root");
 cdie(make_format(&format, arena), "create format");
 cdie(mps_chain_create(&chain, arena, genCOUNT, testChain), "chain_create");

 die(mmqa_pool_create_chain(&poolamc, arena, mps_class_amc(), format, chain),
     "create pool");

 cdie(
  mps_pool_create(&poolawl, arena, mps_class_awl(), format, getassociated),
  "create pool");

 cdie(
  mps_ap_create(&apawl, poolawl, mps_rank_exact()),
  "create ap");

 cdie(
  mps_ap_create(&apamc, poolamc, mps_rank_exact()),
  "create ap");

 for(i=0; i<100; i++) {
  a[i] = allocone(apawl, 6, 1);
 }

 for(i=0; i<10000; i++) {
  j = ranint(100);
  k = 5 + ranint(50);
  comment("New object %i, %i", j, k);
  a[j] = allocone(apawl, k, 1);
  k = ranint(100);
  z = ranint(2);
  comment("setting %i (%p) %i", k, a[k], z);
  setref(a[k], z, a[j]);
  (void)allocdumb(apamc, 0x400*64, 0);
 }

 mps_arena_park(arena);
 mps_ap_destroy(apawl);
 mps_ap_destroy(apamc);
 mps_pool_destroy(poolamc);
 mps_pool_destroy(poolawl);
 mps_chain_destroy(chain);
 mps_fmt_destroy(format);
 mps_root_destroy(root);
 mps_thread_dereg(thread);
 mps_arena_destroy(arena);
 comment("Destroyed arena.");
}


int main(void)
{
 run_test(test);
 pass();
 return 0;
}

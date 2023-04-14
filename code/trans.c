/* trans.c: RAVENBROOK MEMORY POOL SYSTEM TRANSFORMS
 *
 * $Id$
 * Copyright 2011-2018 Ravenbrook Limited.  See end of file for license.
 *
 * This code is specific to Configura.
 *
 * A transform is a special kind of garbage collection that replaces references
 * to a set of objects.  The transform is piggybacked onto a garbage
 * collection by overriding the fix method for a trace.  The mapping used to
 * replace the references is built up in a hash table by the client.
 *
 *
 * Rationale
 *
 * This design was arrived at after some pain.  The MPS isn't really designed
 * for this kind of thing, and the pools generally assume that they're doing
 * a garbage collection when they're asked to condemn, scan, fix, and reclaim
 * stuff.  This makes it very hard to apply the transform without also doing
 * a garbage collection.  Changing this would require a significant reworking
 * of the MPS to generalise its ideas, and would bloat the pool classes.
 *
 *
 * Assumptions:
 *
 *   - Single-threaded mutator.  In fact this code might work if other mutator
 *     threads are running, since the shield ought to operate correctly.
 *     However, in this case there can only be one trace running so that the
 *     correct fix method is chosen by ScanStateInit.
 */

#include "trans.h"
#include "table.h"


#define TransformSig         ((Sig)0x51926A45) /* SIGnature TRANSform */

typedef struct mps_transform_s {
  Sig sig;                      /* <design/sig/> */
  Arena arena;                  /* owning arena */
  Table oldToNew;               /* map to apply to refs */
  Epoch epoch;                  /* epoch in which transform was created */
  Bool aborted;                 /* no longer transforming, just GCing */
} TransformStruct;


Bool TransformCheck(Transform transform)
{
  CHECKS(Transform, transform);
  CHECKU(Arena, transform->arena);
  /* .check.boot: avoid bootstrap problem in transformTableAlloc where
     transformTableFree checks the transform while the table is being
     destroyed */
  if (transform->oldToNew != NULL)
    CHECKD(Table, transform->oldToNew);
  CHECKL(BoolCheck(transform->aborted));
  CHECKL(transform->epoch <= ArenaEpoch(transform->arena));
  return TRUE;
}


/* Allocator functions for the Table oldToNew */

static void *transformTableAlloc(void *closure, size_t size)
{
  Transform transform = (Transform)closure;
  Res res;
  void *p;

  AVERT(Transform, transform);

  res = ControlAlloc(&p, transform->arena, size);
  if (res != ResOK)
    return NULL;

  return p;
}

static void transformTableFree(void *closure, void *p, size_t size)
{
  Transform transform = (Transform)closure;
  AVERT(Transform, transform);
  ControlFree(transform->arena, p, size);
}


Res TransformCreate(Transform *transformReturn, Arena arena)
{
  Transform transform;
  Res res;
  void *p;

  AVER(transformReturn != NULL);
  AVERT(Arena, arena);

  res = ControlAlloc(&p, arena, sizeof(TransformStruct));
  if (res != ResOK)
    goto failAlloc;
  transform = (Transform)p;
  
  transform->oldToNew = NULL;
  transform->arena = arena;
  transform->epoch = ArenaEpoch(arena);
  transform->aborted = FALSE;

  transform->sig = TransformSig;

  AVERT(Transform, transform);

  res = TableCreate(&transform->oldToNew,
                    0, /* don't grow table until TransformAddOldNew */
                    transformTableAlloc,
                    transformTableFree,
                    transform,
                    0, 1); /* these can't be old references */
  if (res != ResOK)
    goto failTable;

  *transformReturn = transform;
  return ResOK;

failTable:
  ControlFree(arena, transform, sizeof(TransformStruct));
failAlloc:
  return res;
}


void TransformDestroy(Transform transform)
{
  Arena arena;
  Table oldToNew;

  AVERT(Transform, transform);

  /* TODO: Log some transform statistics. */

  /* Workaround bootstrap problem, see .check.boot */
  oldToNew = transform->oldToNew;
  transform->oldToNew = NULL;
  TableDestroy(oldToNew);

  arena = TransformArena(transform);
  transform->sig = SigInvalid;
  ControlFree(arena, transform, sizeof(TransformStruct));
}


/* TransformArena -- return transform's arena
 * 
 * Must be thread-safe as it is called outside the arena lock. See
 * <design/thread-safety/#sol.check>
 */

Arena TransformArena(Transform transform)
{
  Arena arena;
  AVER(TESTT(Transform, transform));
  arena = transform->arena;
  AVER(TESTT(Arena, arena));
  return arena;
}


Res TransformAddOldNew(Transform transform,
                       Ref old_list[],
                       Ref new_list[],
                       Count count)
{
  Res res;
  Index i;
  Count added = 0;

  AVERT(Transform, transform);
  AVER(old_list != NULL);
  AVER(new_list != NULL);
  /* count: cannot check */
  
  res = TableGrow(transform->oldToNew, count);
  if (res != ResOK)
    return res;

  for(i = 0; i < count; ++i) {
    /* NOTE: If the mutator isn't adding references while the arena is parked,
       we might need to access the client-provided lists, using ArenaRead. */
    if(old_list[i] == NULL)
      continue;  /* permitted, but no transform to do */
    if(old_list[i] == new_list[i])
      continue;  /* ignore identity-transforms */

    /* Old refs must be in managed memory. */
    {
      Seg seg;
      AVER(SegOfAddr(&seg, transform->arena, old_list[i]));
    }

    res = TableDefine(transform->oldToNew, (Word)old_list[i], new_list[i]);
    AVER(res != ResFAIL); /* It's a static error to add the same old twice. */
    if (res != ResOK)
      return res;
    
    ++added;
  }

  AVERT(Transform, transform);
  
  return ResOK;
}


/* TransformApply -- transform references on the heap */

static Res transformFix(Seg seg, ScanState ss, Ref *refIO)
{
  Ref ref;
  Transform transform;
  Res res;

  AVERT_CRITICAL(Seg, seg);
  AVERT_CRITICAL(ScanState, ss);
  AVER_CRITICAL(refIO != NULL);

  transform = ss->fixClosure;
  AVERT_CRITICAL(Transform, transform);

  if (!transform->aborted) {
    void *refNew;

    ref = *refIO;

    if (TableLookup(&refNew, transform->oldToNew, (Word)ref)) {
      if (ss->rank == RankAMBIG) {
        /* We rely on the fact that all ambiguous references are fixed before
           any others, so no references will have transformed by the time we
           abort.
           NOTE: Configura CVM prints a message and exits if a transform
           fails.  See Configura's
           //depot/project/cet/kernel/internal/version3.2/cvm/c2/updateHeap2.m#4
           line 133.*/
        transform->aborted = TRUE;
      } else {
        /* NOTE: We could fix refNew in the table before copying it,
           since any summaries etc. collected in the scan state will still
           apply when it's copied.  That could save a few snap-outs. */
        *refIO = refNew;
      }
    }
  }

  /* Now progress to a normal GC fix. */
  /* TODO: Make a clean interface to this kind of dynamic binding. */
  ss->fix = ss->arena->emergency ? SegFixEmergency : SegFix;
  TRACE_SCAN_BEGIN(ss) {
    res = TRACE_FIX12(ss, refIO);
  } TRACE_SCAN_END(ss);
  ss->fix = transformFix;

  return res;
}


static void transformCondemn(void *closure, Word old, void *value)
{
  Seg seg;
  GenDesc gen;
  Bool b;
  Trace trace = closure;

  AVERT(Trace, trace);
  UNUSED(value);

  /* Find segment containing old address. */
  b = SegOfAddr(&seg, trace->arena, (Ref)old);
  AVER(b); /* old refs must be in managed memory, else client param error */

  /* Condemn generation containing seg if not already condemned. */
  gen = PoolSegPoolGen(SegPool(seg), seg)->gen;
  AVERT(GenDesc, gen);
  if (RingIsSingle(&gen->trace[trace->ti].traceRing))
    GenDescStartTrace(gen, trace);
}


Res TransformApply(Bool *appliedReturn, Transform transform)
{
  Res res;
  Arena arena;
  Globals globals;
  Trace trace;
  double mortality;

  AVER(appliedReturn != NULL);
  AVERT(Transform, transform);

  arena = TransformArena(transform);

  /* If there have been any flips since the transform was created, the old
     and new pointers will be invalid, since they are not scanned as roots.
     NOTE: Configura CVM parks the arena before adding references.  See
     //depot/project/cet/kernel/internal/version3.2/cvm/c2/updateHeap2.m#4
     line 114. */
  if (transform->epoch != ArenaEpoch(arena))
    return ResPARAM;

  globals = ArenaGlobals(arena);
  AVERT(Globals, globals);

  ArenaPark(globals);
  
  res = TraceCreate(&trace, arena, TraceStartWhyEXTENSION);
  AVER(res == ResOK); /* parking should make a trace available */
  if (res != ResOK)
    return res;

  /* Condemn the generations containing the transform's old objects,
     so that all references to them are scanned. */
  TraceCondemnStart(trace);
  TableMap(transform->oldToNew, transformCondemn, trace);
  res = TraceCondemnEnd(&mortality, trace);
  if (res != ResOK) {
    /* Nothing to transform. */
    TraceDestroyInit(trace);
    goto done;
  }

  trace->fix = transformFix;
  trace->fixClosure = transform;

  res = TraceStart(trace, 1.0, 0.0);
  AVER(res == ResOK); /* transformFix can't fail */
  
  /* If transformFix during traceFlip found ambiguous references and
     aborted the transform then the rest of the trace is just a normal GC. 
     Note that aborting a trace part-way through is pretty much impossible
     without corrupting the mutator graph.  We could safely
         if (transform->aborted) {
           trace->fix = PoolFix;
           trace->fixClosure = NULL;
         }
   */
  
  /* Force the trace to complete now. */
  ArenaPark(globals);

done:
  if (transform->aborted) {
    *appliedReturn = FALSE;
  } else {
    *appliedReturn = TRUE;
    /* I'm not sure why the interface is defined this way. RB 2012-08-03 */
    TransformDestroy(transform);
  }
  
  return ResOK;
}


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2011-2018 Ravenbrook Limited <http://www.ravenbrook.com/>.
 * All rights reserved.  This is an open source license.  Contact
 * Ravenbrook for commercial licensing options.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. Redistributions in any form must be accompanied by information on how
 * to obtain complete source code for this software and any accompanying
 * software that uses this software.  The source code must either be
 * included in the distribution or be available for no more than the cost
 * of distribution plus a nominal fee, and must be freely redistributable
 * under reasonable conditions.  For an executable file, complete source
 * code means the source code for all modules it contains. It does not
 * include source code for modules or files that typically accompany the
 * major components of the operating system on which the executable file
 * runs.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* impl.c.poolamc: AUTOMATIC MOSTLY-COPYING MEMORY POOL CLASS
 *
 * $HopeName: !poolamc.c(trunk.17) $
 * Copyright (C) 1998.  Harlequin Group plc.  All rights reserved.
 *
 * .sources: design.mps.poolamc.
 */

#include "amc.h"
#include "mpscamc.h"
#include "mpm.h"

SRCID(poolamc, "$HopeName: !poolamc.c(trunk.17) $");


/* Binary i/f used by ASG (drj 1998-06-11) */
unsigned long AMCTopGen = 2;

/* PType enumeration -- distinguishes AMCGen and AMCNailBoard */
enum {AMCPTypeGen = 1, AMCPTypeNailBoard};

/* forward declarations */

extern PoolClass PoolClassAMCZ(void);


/* AMCGenStruct -- pool AMC generation descriptor */

#define AMCGenSig       ((Sig)0x519A3C69)

typedef struct AMCGenStruct *AMCGen;
typedef struct AMCGenStruct {
  Sig sig;                      /* impl.h.misc.sig */
  Serial serial;                /* generation number */
  int type;                     /* AMCPTypeGen for a gen */
  AMC amc;                      /* owning AMC pool */
  RingStruct amcRing;           /* link in list of gens in pool */
  ActionStruct actionStruct;    /* action to condemn generation */
  Buffer forward;               /* forwarding buffer */
  Count segs;                   /* number of segs in gen */
  Size size;                    /* total size of segs in gen */
  double collected;             /* time of last collection */
} AMCGenStruct;


#define AMCBufferGen(buffer) ((AMCGen)((buffer)->p))
#define AMCBufferSetGen(buffer, gen) ((buffer)->p = (void*)(gen))


/* .ramp.generation: The ramp gen has serial AMCTopGen+1. */
#define AMCRampGen (AMCTopGen+1)

enum { outsideRamp, beginRamp, ramping, finishRamp, collectingRamp };


/* AMCNailBoard -- the nail board */

typedef struct AMCNailBoardStruct *AMCNailBoard;
typedef struct AMCNailBoardStruct {
  Sig sig;
  int type;         /* AMCPTypeNailBoard for a nail board */
  AMCGen gen;       /* generation of this segment */
  Count nails;      /* number of ambigFixes, not necessarily distinct */
  Count distinctNails; /* number of distinct ambigFixes */
  Bool newMarks;    /* set to TRUE if a new mark bit is added */
  Shift markShift;  /* shift to convert offset into bit index for mark */
  BT mark;          /* mark table used to record ambiguous fixes */
} AMCNailBoardStruct;

#define AMCNailBoardSig ((Sig)0x519A3C4B) /* SIGnature AMC NailBoard */


/* AMCSegHasNailBoard -- test whether the segment has a nail board
 *
 * See design.mps.poolamc.fix.nail.distinguish.
 */

static Bool AMCSegHasNailBoard(Seg seg)
{
  int type;

  type = *(int *)SegP(seg);
  AVER(type == AMCPTypeNailBoard || type == AMCPTypeGen);
  return type == AMCPTypeNailBoard;
}


/* AMCSegNailBoard -- get the nail board for this segment */

static AMCNailBoard AMCSegNailBoard(Seg seg)
{
  void *p;

  p = SegP(seg);
  AVER(AMCSegHasNailBoard(seg));
  return PARENT(AMCNailBoardStruct, type, p);
}


/* AMCSegGen -- get the generation structure for this segment */

static AMCGen AMCSegGen(Seg seg)
{
  void *p;

  p = SegP(seg);
  if(AMCSegHasNailBoard(seg)) {
    AMCNailBoard nailBoard = AMCSegNailBoard(seg);
    return nailBoard->gen;
  } else {
    return PARENT(AMCGenStruct, type, p);
  }
}


/* AMCStruct -- pool AMC descriptor
 *
 * See design.mps.poolamc.struct.
 */

#define AMCSig          ((Sig)0x519A3C99) /* SIGnature AMC */

typedef struct AMCStruct {      /* design.mps.poolamc.struct */
  PoolStruct poolStruct;        /* generic pool structure */
  RankSet rankSet;              /* rankSet for entire pool */
  RingStruct genRing;           /* ring of generations */
  AMCGen nursery;               /* the default mutator generation */
  AMCGen rampGen;               /* the ramp generation */
  AMCGen afterRampGen;          /* the generation after rampGen */
  unsigned rampCount;           /* see .ramp.hack */
  int rampMode;                 /* see .ramp.hack */
  Sig sig;                      /* impl.h.misc.sig */
} AMCStruct;


static Bool AMCCheck(AMC amc);


/* PoolPoolAMC -- convert generic Pool to AMC */

#define PoolPoolAMC(pool) \
  PARENT(AMCStruct, poolStruct, (pool))


/* ActionAMCGen -- convert an action to a gen */

#define ActionAMCGen(action) \
  PARENT(AMCGenStruct, actionStruct, action)


/* AMCPool -- convert AMC to generic Pool */

static Pool AMCPool(AMC amc)
{
  AVERT(AMC, amc);
  return &amc->poolStruct;
}


/* AMCGenCheck -- check consistency of a generation structure */

static Bool AMCGenCheck(AMCGen gen)
{
  Arena arena;
  CHECKS(AMCGen, gen);
  CHECKU(AMC, gen->amc);
  CHECKL(gen->type == AMCPTypeGen);
  CHECKD(Action, &gen->actionStruct);
  CHECKD(Buffer, gen->forward);
  CHECKL(RingCheck(&gen->amcRing));
  CHECKL(gen->serial <= AMCTopGen + 1); /* see .ramp.generation */
  CHECKL((gen->size == 0) == (gen->segs == 0));
  arena = gen->amc->poolStruct.arena;
  CHECKL(gen->size >= gen->segs * ArenaAlign(arena));
  return TRUE;
}


/* AMCNailBoardCheck -- check the nail board */

static Bool AMCNailBoardCheck(AMCNailBoard board)
{
  CHECKS(AMCNailBoard, board);
  CHECKL(board->type == AMCPTypeNailBoard);
  CHECKD(AMCGen, board->gen);
  /* nails is >= number of set bits in mark, but we can't check this. */
  /* We know that shift corresponds to pool->align */
  CHECKL(BoolCheck(board->newMarks));
  CHECKL(board->distinctNails <= board->nails);
  CHECKL(1uL << board->markShift ==
         AMCPool(board->gen->amc)->alignment);
  /* weak check for BTs @@@@ */
  CHECKL(board->mark != NULL);
  return TRUE;
}


/* AMCGenCreate -- create a generation */

static Res AMCGenCreate(AMCGen *genReturn, AMC amc, Serial genNum)
{
  Arena arena;
  Buffer buffer;
  Pool pool;
  AMCGen gen;
  Res res;
  void *p;

  AVERT(AMC, amc);

  pool = &amc->poolStruct;
  arena = pool->arena;

  res = ArenaAlloc(&p, arena, sizeof(AMCGenStruct));
  if(res != ResOK)
    goto failArenaAlloc;
  gen = (AMCGen)p;

  res = BufferCreate(&buffer, pool);
  if(res != ResOK)
    goto failBufferCreate;
  buffer->p = NULL; /* no gen yet -- see design.mps.poolamc.forward.gen */
  buffer->i = TRUE; /* it's a forwarding buffer */

  RingInit(&gen->amcRing);
  ActionInit(&gen->actionStruct, pool);
  gen->type = AMCPTypeGen;
  gen->amc = amc;
  gen->segs = 0;
  gen->size = 0;
  gen->forward = buffer;
  gen->collected = ArenaMutatorAllocSize(arena);

  gen->sig = AMCGenSig;
  gen->serial = genNum;

  AVERT(AMCGen, gen);

  RingAppend(&amc->genRing, &gen->amcRing);
  if(genNum == AMCRampGenFollows + 1)
    amc->afterRampGen = gen;
  if(genNum == AMCRampGen)
    amc->rampGen = gen;

  EVENT_PP(AMCGenCreate, amc, gen);

  *genReturn = gen;
  return ResOK;

failBufferCreate:
  ArenaFree(arena, p, sizeof(AMCGenStruct));
failArenaAlloc:
  return res;
}


/* AMCGenDestroy -- destroy a generation */

static void AMCGenDestroy(AMCGen gen)
{
  Arena arena;

  AVERT(AMCGen, gen);
  AVER(gen->segs == 0);
  AVER(gen->size == 0);

  EVENT_P(AMCGenDestroy, gen);
  arena = gen->amc->poolStruct.arena;
  RingRemove(&gen->amcRing);
  gen->sig = SigInvalid;
  ActionFinish(&gen->actionStruct);
  BufferDestroy(gen->forward);
  RingFinish(&gen->amcRing);
  ArenaFree(arena, gen, sizeof(AMCGenStruct));
}


/* AMCSegCreateNailBoard -- create nail board for segment */

static Res AMCSegCreateNailBoard(Seg seg, Pool pool)
{
  AMCNailBoard board;
  Arena arena;
  Count bits;
  Res res;
  void *p;

  AVER(!AMCSegHasNailBoard(seg));

  arena = PoolArena(pool);

  res = ArenaAlloc(&p, arena, sizeof(AMCNailBoardStruct));
  if(res != ResOK)
    goto failAllocNailBoard;
  board = p;
  board->type = AMCPTypeNailBoard;
  board->gen = AMCSegGen(seg);
  board->nails = (Count)0;
  board->distinctNails = (Count)0;
  board->newMarks = FALSE;
  board->markShift = SizeLog2((Size)pool->alignment);
  bits = SegSize(seg) >> board->markShift;
  res = ArenaAlloc(&p, arena, BTSize(bits));
  if(res != ResOK)
    goto failMarkTable;
  board->mark = p;
  BTResRange(board->mark, 0, bits);
  board->sig = AMCNailBoardSig;
  AVERT(AMCNailBoard, board);
  SegSetP(seg, &board->type); /* design.mps.poolamc.fix.nail.distinguish */
  return ResOK;

failMarkTable:
  ArenaFree(arena, board, sizeof(AMCNailBoardStruct));
failAllocNailBoard:
  return res;
}


/* AMCSegDestroyNailBoard -- destroy the nail board of a segment */

static void AMCSegDestroyNailBoard(Seg seg, Pool pool)
{
  AMCNailBoard board;
  AMCGen gen;
  Arena arena;
  Count bits;

  gen = AMCSegGen(seg);
  board = AMCSegNailBoard(seg);
  AVERT(AMCNailBoard, board);

  arena = PoolArena(pool);
  AVERT(Arena, arena);

  bits = SegSize(seg) >> board->markShift;
  ArenaFree(arena, board->mark, BTSize(bits));
  board->sig = SigInvalid;
  ArenaFree(arena, board, sizeof(AMCNailBoardStruct));
  SegSetP(seg, &gen->type); /* design.mps.poolamc.fix.nail.distinguish */
}


/* AMCNailGetMark -- get the mark bit for ref from the nail board */

static Bool AMCNailGetMark(Seg seg, Ref ref)
{
  AMCNailBoard board;
  Index i;

  board = AMCSegNailBoard(seg);
  AVERT(AMCNailBoard, board);

  i = AddrOffset(SegBase(seg), ref) >> board->markShift;
  return BTGet(board->mark, i);
}


/* AMCNailGetAndSetMark
 *
 * Set the mark bit for ref in the nail board.
 * Returns the old value. */

static Bool AMCNailGetAndSetMark(Seg seg, Ref ref)
{
  AMCNailBoard board;
  Index i;

  board = AMCSegNailBoard(seg);
  AVERT(AMCNailBoard, board);

  ++board->nails;
  i = AddrOffset(SegBase(seg), ref) >> board->markShift;
  if(!BTGet(board->mark, i)) {
    BTSet(board->mark, i);
    board->newMarks = TRUE;
    ++board->distinctNails;
    return FALSE;
  }
  return TRUE;
}


/* AMCNailMarkRange -- nail a range in the board
 *
 * We may assume that the range is unmarked.
 */

static void AMCNailMarkRange(Seg seg, Addr base, Addr limit)
{
  AMCNailBoard board;
  Index ibase, ilimit;

  AVER(SegBase(seg) <= base && base < SegLimit(seg));
  AVER(SegBase(seg) <= limit && limit <= SegLimit(seg));
  AVER(base < limit);

  board = AMCSegNailBoard(seg);
  AVERT(AMCNailBoard, board);
  ibase = AddrOffset(SegBase(seg), base) >> board->markShift;
  ilimit = AddrOffset(SegBase(seg), limit) >> board->markShift;
  AVER(BTIsResRange(board->mark, ibase, ilimit));

  BTSetRange(board->mark, ibase, ilimit);
  board->nails += ilimit - ibase;
  board->distinctNails += ilimit - ibase;
}


/* AMCNailRangeIsMarked -- check that a range in the board is marked */

static Bool AMCNailRangeIsMarked(Seg seg, Addr base, Addr limit)
{
  AMCNailBoard board;
  Index ibase, ilimit;

  AVER(SegBase(seg) <= base && base < SegLimit(seg));
  AVER(SegBase(seg) <= limit && limit <= SegLimit(seg));
  AVER(base < limit);

  board = AMCSegNailBoard(seg);
  AVERT(AMCNailBoard, board);
  ibase = AddrOffset(SegBase(seg), base) >> board->markShift;
  ilimit = AddrOffset(SegBase(seg), limit) >> board->markShift;
  return BTIsSetRange(board->mark, ibase, ilimit);
}


/* AMCInitComm -- initialize AMC/Z pool
 *
 * See design.mps.poolamc.init.
 * Shared by AMCInit and AMCZinit.
 */

static Res AMCInitComm(Pool pool, RankSet rankSet, va_list arg)
{
  AMC amc;
  AMCGen gen;
  Res res;

  AVER(pool != NULL);

  amc = PoolPoolAMC(pool);

  pool->format = va_arg(arg, Format);
  AVERT(Format, pool->format);
  pool->alignment = pool->format->alignment;
  amc->rankSet = rankSet;

  RingInit(&amc->genRing);
  /* amc gets checked before the nursery gets created, but the */
  /* nursery gets created later in this function. */
  amc->nursery = NULL;
  /* The other generations get created when only needed. */
  amc->rampGen = NULL; amc->afterRampGen = NULL;

  amc->rampCount = 0; amc->rampMode = outsideRamp;

  amc->sig = AMCSig;
  AVERT(AMC, amc);

  res = AMCGenCreate(&gen, amc, (Serial)0);
  if(res != ResOK)
    return res;
  amc->nursery = gen;

  AVERT(AMC, amc);
  EVENT_PP(AMCInit, pool, amc);
  return ResOK;
}

static Res AMCInit(Pool pool, va_list arg)
{
  return AMCInitComm(pool, RankSetSingle(RankEXACT), arg);
}

static Res AMCZInit(Pool pool, va_list arg)
{
  return AMCInitComm(pool, RankSetEMPTY, arg);
}


/* AMCFinish -- finish AMC pool
 *
 * See design.mps.poolamc.finish.
 */

static void AMCFinish(Pool pool)
{
  AMC amc;
  Ring ring;
  Ring node, nextNode;

  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);

  EVENT_P(AMCFinish, amc);

  /* @@@@ Make sure that segments aren't buffered by forwarding buffers. */
  /* This is a hack which allows the pool to be destroyed */
  /* while it is collecting.  Note that there aren't any mutator */
  /* buffers by this time. */
  ring = &amc->genRing;
  RING_FOR(node, ring, nextNode) {
    AMCGen gen = RING_ELT(AMCGen, amcRing, node);
    BufferDetach(gen->forward, pool);
  }

  ring = PoolSegRing(pool);
  RING_FOR(node, ring, nextNode) {
    Seg seg = SegOfPoolRing(node);
    Size size;
    AMCGen gen = AMCSegGen(seg);

    --gen->segs;
    size = SegSize(seg);
    gen->size -= size;

    SegFree(seg);
  }

  ring = &amc->genRing;
  RING_FOR(node, ring, nextNode) {
    AMCGen gen = RING_ELT(AMCGen, amcRing, node);

    AMCGenDestroy(gen);
  }

  amc->sig = SigInvalid;
}


/* AMCBufferInit -- initialize a new mutator buffer */

static Res AMCBufferInit(Pool pool, Buffer buffer, va_list args)
{
  AMC amc;

  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  UNUSED(args);

  buffer->rankSet = amc->rankSet;

  /* Set up the buffer to be a mutator buffer allocating in */
  /* the nursery. */
  buffer->p = amc->nursery;
  buffer->i = FALSE;                    /* mutator buffer */
  EVENT_PP(AMCBufferInit, amc, buffer);
  return ResOK;
}


/* AMCBufferFill -- refill an allocation buffer
 *
 * See design.mps.poolamc.fill.
 */

static Res AMCBufferFill(Seg *segReturn,
                         Addr *baseReturn, Addr *limitReturn,
                         Pool pool, Buffer buffer, Size size,
                         Bool withReservoirPermit)
{
  Seg seg;
  AMC amc;
  Res res;
  Addr base;
  Arena arena;
  Size alignedSize;
  AMCGen gen;
  SegPrefStruct segPrefStruct;

  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  AVER(segReturn != NULL);
  AVER(baseReturn != NULL);
  AVER(limitReturn != NULL);
  AVERT(Buffer, buffer);
  AVER(BufferIsReset(buffer));
  AVER(size >  0);
  AVER(BoolCheck(withReservoirPermit));

  gen = (AMCGen)buffer->p;
  AVERT(AMCGen, gen);

  /* Create and attach segment.  The location of this segment is */
  /* expressed as a generation number.  We rely on the arena to */
  /* organize locations appropriately.  */
  arena = PoolArena(pool);
  alignedSize = SizeAlignUp(size, ArenaAlign(arena));
  segPrefStruct = *SegPrefDefault();
  SegPrefExpress(&segPrefStruct, SegPrefCollected, NULL);
  SegPrefExpress(&segPrefStruct, SegPrefGen, &gen->serial);
  res = SegAlloc(&seg, &segPrefStruct, alignedSize, pool, 
                 withReservoirPermit);
  if(res != ResOK)
    return res;

  /* design.mps.seg.field.rankSet.start */
  if(BufferRankSet(buffer) == RankSetEMPTY) {
    SegSetRankAndSummary(seg, BufferRankSet(buffer), RefSetEMPTY);
  } else {
    SegSetRankAndSummary(seg, BufferRankSet(buffer), RefSetUNIV);
  }

  /* Put the segment in the generation indicated by the buffer. */
  SegSetP(seg, &gen->type); /* design.mps.poolamc.fix.nail.distinguish */
  ++gen->segs;
  gen->size += alignedSize;
  /* If the generation was empty, restart the collection clock. */
  if(gen->segs == 1) gen->collected = ArenaMutatorAllocSize(arena);

  /* Give the buffer the entire segment to allocate in. */
  *segReturn = seg;
  base = SegBase(seg);
  *baseReturn = base;
  *limitReturn = AddrAdd(base, alignedSize);
  EVENT_PPWAW(AMCBufferFill, amc, buffer, size, base, alignedSize);
  return ResOK;
}


/* AMCBufferEmpty -- detach a buffer from a segment
 *
 * See design.mps.poolamc.flush.
 */

static void AMCBufferEmpty(Pool pool, Buffer buffer)
{
  AMC amc;
  Seg seg;
  Word size;
  Arena arena;

  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  AVERT(Buffer, buffer);
  AVER(!BufferIsReset(buffer));
  AVER(BufferIsReady(buffer));

  seg = BufferSeg(buffer);
  arena = BufferArena(buffer);

  /* design.mps.poolamc.flush.pad */
  size = AddrOffset(BufferGetInit(buffer), SegLimit(seg));
  if(size > 0) {
    ShieldExpose(arena, seg);
    (*pool->format->pad)(BufferGetInit(buffer), size);
    ShieldCover(arena, seg);
  }
  EVENT_PPW(AMCBufferEmpty, amc, buffer, size);
}


/* AMCBenefit -- calculate benefit of collecting some generation */

static double AMCBenefit(Pool pool, Action action)
{
  AMCGen gen;           /* generation which owns action */
  AMC amc;
  Arena arena;
  double f;             /* frequency of collection, in Mb of alloc */
  Bool inRampMode;

  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  AVERT(Action, action);
  gen = ActionAMCGen(action);
  AVERT(AMCGen, gen);

  inRampMode = amc->rampMode != outsideRamp;

  switch(gen->serial) {
  case 0: f = inRampMode ? AMCGen0RampmodeFrequency : AMCGen0Frequency;
    break;
  case 1: f = inRampMode ? AMCGen1RampmodeFrequency : AMCGen1Frequency;
    break;
  case 2: f = inRampMode ? AMCGen2RampmodeFrequency : AMCGen2Frequency;
    break;
  default:
    if(gen->serial == AMCGenFinal) {
      return 0; /* Don't ever collect the final generation. */
    } else if(gen->serial == AMCRampGen) {
      if(amc->rampMode == finishRamp)
        return 1e99; /* do it now */
      else
        if(gen->size != 0)
          f = AMCRampGenFrequency;
        else
          return 0; /* Don't collect an empty ramp gen. */
    } else {
      f = inRampMode
            ? (AMCGen2RampmodeFrequency
               + AMCGen2plusRampmodeFrequencyMultiplier * gen->serial)
            : (AMCGen2Frequency
               + AMCGen2plusFrequencyMultiplier * gen->serial);
    }
    break;
  }

  arena = PoolArena(pool);

  return (ArenaMutatorAllocSize(arena) - gen->collected) - f * 1024*1024L;
}


/* AMCRampBegin -- note an entry into a ramp pattern */

static void AMCRampBegin(Pool pool, Buffer buf)
{
  AMC amc;

  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  AVERT(Buffer, buf);

  AVER(amc->rampCount < UINT_MAX);
  ++amc->rampCount;
  if(amc->rampCount == 1 && amc->rampMode != finishRamp)
    amc->rampMode = beginRamp;
}


/* AMCRampEnd -- note an exit from a ramp pattern */

static void AMCRampEnd(Pool pool, Buffer buf)
{
  AMC amc;

  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  AVERT(Buffer, buf);

  AVER(amc->rampCount > 0);
  --amc->rampCount;
  if(amc->rampCount == 0)
    if(amc->rampGen != NULL) /* if we have old objects, clean up */
      amc->rampMode = finishRamp;
    else
      amc->rampMode = outsideRamp;
}


/* AMCWhiten -- condemn the segment for the trace
 *
 * If the segment has a mutator buffer on it, we nail the buffer,
 * because we can't scan or reclaim uncommitted buffers.
 */

static Res AMCWhiten(Pool pool, Trace trace, Seg seg)
{
  AMCGen gen, newGen;
  AMC amc;
  Buffer buffer;
  Res res;

  AVERT(Pool, pool);
  AVERT(Trace, trace);
  AVERT(Seg, seg);

  buffer = SegBuffer(seg);
  if(buffer != NULL) {
    AVERT(Buffer, buffer);

    if(buffer->i) {                 /* forwarding buffer */
      AVER(BufferIsReady(buffer));
      BufferDetach(buffer, pool);
    } else {                        /* mutator buffer */
      if(BufferScanLimit(buffer) == SegBase(seg)) {
        /* There's nothing but the buffer, don't condemn. */
        return ResOK;
      } else /* if(BufferScanLimit(buffer) == BufferLimit(buffer)) { */
        /* The buffer is full, so it won't be used by the mutator. */
        /* @@@@ We should detach it, but can't for technical reasons. */
        /* BufferDetach(buffer, pool); */
      /* } else */ {
        /* There is an active buffer, make sure it's nailed. */
        if(!AMCSegHasNailBoard(seg)) {
          if(SegNailed(seg) == TraceSetEMPTY) {
            res = AMCSegCreateNailBoard(seg, pool);
            if(res != ResOK)
              return ResOK; /* can't create nail board, don't condemn */
            if(BufferScanLimit(buffer) != BufferLimit(buffer))
              AMCNailMarkRange(seg, BufferScanLimit(buffer),
                               BufferLimit(buffer));
            ++trace->nailCount;
            SegSetNailed(seg, TraceSetSingle(trace->ti));
          } else {
            /* Segment is nailed already, cannot create a nail board */
            /* (see .nail.new), just give up condemning. */
            return ResOK;
          }
        } else {
          /* We have a nail board, the buffer must be nailed already. */
          AVER((BufferScanLimit(buffer) == BufferLimit(buffer))
               || AMCNailRangeIsMarked(seg, BufferScanLimit(buffer),
                                       BufferLimit(buffer)));
          /* Nail it for this trace as well. */
          SegSetNailed(seg, TraceSetAdd(SegNailed(seg), trace->ti));
        }
        /* We didn't condemn the buffer, subtract it from the count. */
        /* @@@@ We could subtract all the nailed grains. */
        trace->condemned -= AddrOffset(BufferScanLimit(buffer),
                                       BufferLimit(buffer));
      }
    }
  }

  SegSetWhite(seg, TraceSetAdd(SegWhite(seg), trace->ti));
  trace->condemned += SegSize(seg);

  /* ensure we are forwarding into the right generation */

  gen = AMCSegGen(seg);
  AVERT(AMCGen, gen);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  /* see design.mps.poolamc.gen.ramp */
  /* This switching needs to be more complex for multiple traces. */
  AVER(TraceSetIsSingle(PoolArena(pool)->busyTraces));
  if(amc->rampMode == beginRamp && gen->serial == AMCRampGenFollows) {
    if(amc->rampGen == NULL) {
      res = AMCGenCreate(&newGen, amc, AMCRampGen);
      if(res != ResOK)
        return res; /* @@@@ should we clean up? */
    }
    BufferDetach(gen->forward, pool);
    AMCBufferSetGen(gen->forward, amc->rampGen);
    BufferDetach(amc->rampGen->forward, pool);
    AMCBufferSetGen(amc->rampGen->forward, amc->rampGen);
    amc->rampMode = ramping;
  } else
    if(amc->rampMode == finishRamp && gen->serial == AMCRampGenFollows) {
      if(amc->afterRampGen == NULL) {
        res = AMCGenCreate(&newGen, amc, AMCRampGenFollows + 1);
        if(res != ResOK)
          return res;
      }
      BufferDetach(gen->forward, pool);
      AMCBufferSetGen(gen->forward, amc->afterRampGen);
      AVER(amc->rampGen != NULL);
      BufferDetach(amc->rampGen->forward, pool);
      AMCBufferSetGen(amc->rampGen->forward, amc->afterRampGen);
      amc->rampMode = collectingRamp;
    }

  /* see design.mps.poolamc.forward.gen */
  if(AMCBufferGen(gen->forward) == NULL) {
    if(gen->serial == AMCTopGen) {
      /* top generation forwards into itself */
      AMCBufferSetGen(gen->forward, gen);
    } else {
      /* Because we switch when condemning AMCRampGenFollows, the gen */
      /* that AMCRampGen is set to forward into must already exist */
      /* when we come to condemn it. */
      AVER(gen->serial != AMCRampGen);
      res = AMCGenCreate(&newGen, amc, gen->serial + 1);
      if(res != ResOK)
        return res;
      AMCBufferSetGen(gen->forward, newGen);
    }
  }

  return ResOK;
}


/* AMCAct -- start collection described by the action */

static Res AMCAct(Pool pool, Action action)
{
  Trace trace;
  AMC amc;
  Res res;
  Arena arena;
  Ring node, nextNode;
  AMCGen gen;
  RefSet condemnedSet;
  Serial genNum;

  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  AVERT(Action, action);
  gen = ActionAMCGen(action);
  AVERT(AMCGen, gen);
  AVER(gen->amc == amc);

  arena = PoolArena(pool);
  genNum = gen->serial;

  res = TraceCreate(&trace, arena);
  if(res != ResOK)
    goto failCreate;

  res = PoolTraceBegin(pool, trace);
  if(res != ResOK)
    goto failBegin;

  /* Identify the condemned set in this pool, and find its zone set */
  /* @@@@ Could accumulate actual refset for generation. */
  condemnedSet = RefSetEMPTY;
  RING_FOR(node, PoolSegRing(pool), nextNode) {
    Seg seg = SegOfPoolRing(node);
    Serial segGenNum = AMCSegGen(seg)->serial;

    /* Condemn the given generation and all previous ones; note that */
    /* despite the numbering of the ramp gen (.ramp.generation), we */
    /* consider it to be between AMCRampGenFollows and the next gen. */
    if(genNum == AMCRampGen
       ? (segGenNum <= AMCRampGenFollows || segGenNum == AMCRampGen)
       : (segGenNum <= genNum
          || (segGenNum == AMCRampGen && genNum > AMCRampGenFollows)))
      condemnedSet = RefSetUnion(condemnedSet, RefSetOfSeg(arena, seg));
  }

  if(condemnedSet != RefSetEMPTY) {
    res = TraceCondemnRefSet(trace, condemnedSet);
    if(res != ResOK)
      goto failCondemn;
  }

  res = TraceStart(trace, TraceMortalityEstimate,
                   AMCGen0Frequency * TraceGen0IncrementalityMultiple
                   * 1024*1024uL);
  if(res != ResOK)
    goto failStart;

  /* Make sure the generation collection time gets updated even */
  /* if the collection is empty. */
  gen->collected = ArenaMutatorAllocSize(arena);
  return ResOK;

failStart:
  NOTREACHED;
failCondemn:
  NOTREACHED; /* @@@@ Would leave white sets inconsistent. */
failBegin:
  TraceDestroy(trace);
failCreate:
  return res;
}


/* AMCScanNailedOnce -- make one scanning pass over a nailed segment
 *
 * *totalReturn set to TRUE iff all objects in segment scanned.
 * *moreReturn set to FALSE only if there are no more objects
 * on the segment that need scanning (which is normally the case).
 * It is set to TRUE if scanning had to be abandoned early on, and
 * also if during emergency fixing any new marks got added to the
 * nail board.
 */
static Res AMCScanNailedOnce(Bool *totalReturn, Bool *moreReturn,
                             ScanState ss, Pool pool,
                             Seg seg, AMC amc)
{
  Addr p, limit;
  Format format;
  Res res;
  Bool total = TRUE;
  Size bytesScanned = 0;

  /* arguments checked by AMCScan */

  /* Actually ownly unused when telemetry is off.  Needs */
  /* fixing when EVENT_* is fixed. @@@@ */
  UNUSED(amc);

  EVENT_PPP(AMCScanBegin, amc, seg, ss); /* @@@@ use own event */

  format = pool->format;
  AMCSegNailBoard(seg)->newMarks = FALSE;

  p = SegBase(seg);
  while(SegBuffer(seg) != NULL) {
    limit = BufferScanLimit(SegBuffer(seg));
    if(p >= limit) {
      AVER(p == limit);
      goto returnGood;
    }
    while(p < limit) {
      Addr q;
      q = (*format->skip)(p);
      if(AMCNailGetMark(seg, p)) {
        res = (*format->scan)(ss, p, q);
        if(res != ResOK) {
          *totalReturn = FALSE;
          *moreReturn = TRUE;
          return res;
        }
        bytesScanned += AddrOffset(p, q);
      } else {
        total = FALSE;
      }
      p = q;
    }
    AVER(p == limit);
  }

  /* Should have a ScanMarkedRange or something like that @@@@ */
  /* to abstract common code. */

  limit = SegLimit(seg);
  while(p < limit) {
    Addr q;
    q = (*format->skip)(p);
    if(AMCNailGetMark(seg, p)) {
      res = (*format->scan)(ss, p, q);
      if(res != ResOK) {
        *totalReturn = FALSE;
        *moreReturn = TRUE;
        return res;
      }
      bytesScanned += AddrOffset(p, q);
    } else {
      total = FALSE;
    }
    p = q;
  }
  AVER(p == limit);

returnGood:
  EVENT_PPP(AMCScanEnd, amc, seg, ss); /* @@@@ use own event */

  AVER(bytesScanned <= SegSize(seg));
  ss->scannedSize += bytesScanned;
  *totalReturn = total;
  *moreReturn = AMCSegNailBoard(seg)->newMarks;
  return ResOK;
}


/* AMCScanNailed -- scan a nailed segment */

static Res AMCScanNailed(Bool *totalReturn,
                         ScanState ss, Pool pool,
                         Seg seg, AMC amc)
{
  Bool total;
  Bool moreScanning;

  /* arguments checked by AMCScan */

  do {
    Res res;
    res = AMCScanNailedOnce(&total, &moreScanning, ss, pool, seg, amc);
    if(res != ResOK) {
      *totalReturn = FALSE;
      return res;
    }
  } while(moreScanning);

  *totalReturn = total;
  return ResOK;
}


/* AMCScan -- scan a single seg, turning it black
 *
 * See design.mps.poolamc.scan.
 */

static Res AMCScan(Bool *totalReturn, ScanState ss, Pool pool, Seg seg)
{
  Addr base, limit;
  Arena arena;
  Format format;
  AMC amc;
  Res res;

  AVER(totalReturn != NULL);
  AVERT(ScanState, ss);
  AVERT(Seg, seg);
  AVERT(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);


  format = pool->format;
  arena = pool->arena;

  if(AMCSegHasNailBoard(seg)) {
    return AMCScanNailed(totalReturn, ss, pool, seg, amc);
  }

  EVENT_PPP(AMCScanBegin, amc, seg, ss);

  base = SegBase(seg);
  while(SegBuffer(seg) != NULL) {  /* design.mps.poolamc.scan.loop */
    limit = BufferScanLimit(SegBuffer(seg));
    if(base >= limit) {
      AVER(base == limit);
      *totalReturn = TRUE;
      return ResOK;
    }
    res = (*format->scan)(ss, base, limit);
    if(res != ResOK) {
      *totalReturn = FALSE;
      return res;
    }
    ss->scannedSize += AddrOffset(base, limit);
    base = limit;
  }

  /* design.mps.poolamc.scan.finish */
  limit = SegLimit(seg);
  AVER(SegBase(seg) <= base && base <= SegLimit(seg));
  if(base < limit) {
    res = (*format->scan)(ss, base, limit);
    if(res != ResOK) {
      *totalReturn = FALSE;
      return res;
    }
  }

  ss->scannedSize += AddrOffset(base, limit);
  EVENT_PPP(AMCScanEnd, amc, seg, ss);

  *totalReturn = TRUE;
  return ResOK;
}


/* AMCFixInPlace -- fix an reference without moving the object
 *
 * Usually this function is used for ambiguous references,
 * but during emergency tracing may be used for references of
 * any rank.
 *
 * If the segment has a nail board then we use that to record the fix.
 * Otherwise we simply grey and nail the entire segment.
 */
static void AMCFixInPlace(Pool pool, Seg seg, ScanState ss, Ref *refIO)
{
  Addr ref;

  /* arguments AVERed by AMCFix */
  UNUSED(pool);

  ref = (Addr)*refIO;
  AVER(SegBase(seg) <= ref);
  AVER(ref < SegLimit(seg));

  EVENT_0(AMCFixInPlace);
  if(AMCSegHasNailBoard(seg)) {
    Bool wasMarked = AMCNailGetAndSetMark(seg, ref);
    /* If there are no new marks (i.e., no new traces for which we */
    /* are marking, and no new mark bits set) then we can return */
    /* immediately, without changing colour. */
    if(TraceSetSub(ss->traces, SegNailed(seg)) && wasMarked) {
      return;
    }
  } else if(TraceSetSub(ss->traces, SegNailed(seg))) {
    return;
  }
  SegSetNailed(seg, TraceSetUnion(SegNailed(seg), ss->traces));
  if(SegRankSet(seg) != RankSetEMPTY) {
    SegSetGrey(seg, TraceSetUnion(SegGrey(seg), ss->traces));
  }
}


/* AMCFixEmergency -- fix a reference, without allocating
 *
 * See design.mps.poolamc.emergency.fix.
 */

static Res AMCFixEmergency(Pool pool, ScanState ss, Seg seg, Ref *refIO)
{
  Arena arena;
  AMC amc;
  Addr newRef;

  AVERT(Pool, pool);
  AVERT(ScanState, ss);
  AVERT(Seg, seg);
  AVER(refIO != NULL);

  arena = PoolArena(pool);
  AVERT(Arena, arena);
  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);

  ss->wasMarked = TRUE;

  if(ss->rank == RankAMBIG) {
    goto fixInPlace;
  }

  ShieldExpose(arena, seg);
  newRef = (*pool->format->isMoved)(*refIO);
  ShieldCover(arena, seg);
  if(newRef != (Addr)0) {
    /* Object has been forwarded already, so snap-out pointer. */
    /* Useful weak pointer semantics not implemented. @@@@ */
    *refIO = newRef;
    return ResOK;
  }

fixInPlace: /* see design.mps.poolamc.nailboard.emergency */
  AMCFixInPlace(pool, seg, ss, refIO);
  return ResOK;
}


/* AMCFix -- fix a reference to the pool
 *
 * See design.mps.poolamc.fix.
 */

static Res AMCFix(Pool pool, ScanState ss, Seg seg, Ref *refIO)
{
  Arena arena;
  AMC amc;
  Res res;
  Format format;        /* cache of pool->format */
  Ref ref;              /* reference to be fixed */
  Ref newRef;           /* new location, if moved */
  Size length;          /* length of object to be relocated */
  Buffer buffer;        /* buffer to allocate new copy into */
  AMCGen gen;           /* generation of old copy of object */
  TraceSet grey;        /* greyness of object being relocated */
  TraceSet toGrey;      /* greyness of object's destination */
  RefSet summary;       /* summary of object being relocated */
  RefSet toSummary;     /* summary of object's destination */
  Seg toSeg;            /* segment to which object is being relocated */

  /* design.mps.trace.fix.noaver */
  AVERT_CRITICAL(Pool, pool);
  AVERT_CRITICAL(ScanState, ss);
  AVERT_CRITICAL(Seg, seg);
  AVER_CRITICAL(refIO != NULL);
  EVENT_0(AMCFix);

  /* For the moment, assume that the object was already marked. */
  /* (See design.mps.fix.protocol.was-marked.) */
  ss->wasMarked = TRUE;

  /* If the reference is ambiguous, set up the datastructures for */
  /* managing a nailed segment.  This involves marking the segment */
  /* as nailed, and setting up a per-word mark table */
  if(ss->rank == RankAMBIG) {
    /* .nail.new: Check to see whether we need a NailBoard for */
    /* this seg.  We use "SegNailed(seg) == TraceSetEMPTY" */
    /* rather than "!AMCSegHasNailBoard(seg)" because this avoids */
    /* setting up a new nail board when the segment was nailed, but had */
    /* no nail board.  This must be avoided because otherwise */
    /* assumptions in AMCFixEmergency will be wrong (essentially */
    /* we will lose some pointer fixes because we introduced a */
    /* nail board). */
    if(SegNailed(seg) == TraceSetEMPTY) {
      res = AMCSegCreateNailBoard(seg, pool);
      if(res != ResOK)
        return res;
      ++ss->nailCount;
      SegSetNailed(seg, TraceSetUnion(SegNailed(seg), ss->traces));
    }
    AMCFixInPlace(pool, seg, ss, refIO);
    return ResOK;
  }

  amc = PoolPoolAMC(pool);
  AVERT_CRITICAL(AMC, amc);
  format = pool->format;
  ref = *refIO;

  arena = pool->arena;

  if(SegNailed(seg) != TraceSetEMPTY) {
    /* If segment is nailed then may have grey and white */
    /* objects on same segment, hence segment may be protected */
    /* hence we need to expose it to examine the broken heart. */
    /* @@@@ This assumes an particular style of barrier. */
    ShieldExpose(arena, seg);
  } else {
    AVER_CRITICAL((SegPM(seg) & AccessREAD) == AccessSetEMPTY);
  }
  /* .fix.ismoved: test for a broken heart */
  newRef = (*format->isMoved)(ref);

  if(newRef == (Addr)0) {
    /* If object is nailed already then we mustn't copy it: */
    if(SegNailed(seg) != TraceSetEMPTY
       && (!AMCSegHasNailBoard(seg) || AMCNailGetMark(seg, ref))) {
      /* Segment only needs greying if there are new traces for which */
      /* we are nailing. */
      if(!TraceSetSub(ss->traces, SegNailed(seg))) {
        if(SegRankSet(seg) != RankSetEMPTY) {
          SegSetGrey(seg, TraceSetUnion(SegGrey(seg), ss->traces));
        }
        SegSetNailed(seg, TraceSetUnion(SegNailed(seg), ss->traces));
      }
      res = ResOK;
      goto returnRes;
    } else if(ss->rank == RankWEAK) {
      /* object is not preserved (neither moved, nor nailed) */
      /* hence, reference should be splatted */
      goto updateReference;
    }
    /* object is not preserved yet (neither moved, nor nailed) */
    /* so should be preserved by forwarding */
    ++ss->forwardCount;
    EVENT_A(AMCFixForward, newRef);
    /* design.mps.fix.protocol.was-marked */
    ss->wasMarked = FALSE;

    /* Get the forwarding buffer from the object's generation. */
    gen = AMCSegGen(seg);
    buffer = gen->forward;
    AVER_CRITICAL(buffer != NULL);

    length = AddrOffset(ref, (*format->skip)(ref));

    do {
      res = BUFFER_RESERVE(&newRef, buffer, length,
                           /* withReservoirPermit */ FALSE);
      if(res != ResOK)
        goto returnRes;

      toSeg = BufferSeg(buffer);
      ShieldExpose(arena, toSeg);

      /* Since we're moving an object from one segment to another, */
      /* union the greyness and the summaries together. */
      grey = TraceSetUnion(ss->traces, SegGrey(seg));
      toGrey = SegGrey(toSeg);
      if(TraceSetDiff(grey, toGrey) != TraceSetEMPTY &&
         SegRankSet(seg) != RankSetEMPTY) {
        SegSetGrey(toSeg, TraceSetUnion(toGrey, grey));
      }
      summary = SegSummary(seg);
      toSummary = SegSummary(toSeg);
      if(RefSetDiff(summary, toSummary) != RefSetEMPTY) {
        SegSetSummary(toSeg, RefSetUnion(toSummary, summary));
      }

      /* design.mps.trace.fix.copy */
      (void)AddrCopy(newRef, ref, length);

      ShieldCover(arena, toSeg);
    } while(!BUFFER_COMMIT(buffer, newRef, length));
    ss->copiedSize += length;

    /* @@@@ Must expose the old segment because it might be */
    /* write protected.  However, in the read barrier phase */
    /* nothing white is accessible, so this could be optimized */
    /* away. */
    ShieldExpose(arena, seg);
    (*format->move)(ref, newRef);       /* install broken heart */
    ShieldCover(arena, seg);
  } else {
    /* reference to broken heart (which should be snapped out -- */
    /* consider adding to (non-existant) snap-out cache here) */
    ++ss->snapCount;
  }

  /* .fix.update: update the reference to whatever the above code */
  /* decided it should be */
updateReference:
  *refIO = newRef;
  res = ResOK;

returnRes:
  if(SegNailed(seg) != TraceSetEMPTY) {
    ShieldCover(arena, seg);
  }
  return res;
}


/* AMCReclaimNailed -- reclaim what you can from a nailed segment */

static void AMCReclaimNailed(Pool pool, Trace trace, Seg seg)
{
  Addr p, limit;
  Arena arena;
  Format format;
  Size bytesReclaimed = 0;
  AMC amc;

  /* All arguments AVERed by AMCReclaim */

  amc = PoolPoolAMC(pool);
  AVERT(AMC, amc);
  format = pool->format;

  arena = PoolArena(pool);
  AVERT(Arena, arena);

  if(!AMCSegHasNailBoard(seg)) {
    /* We didn't keep a mark table, so preserve everything. */
    goto adjustColour;
  }

  /* see design.mps.poolamc.nailboard.limitations for improvements */
  ShieldExpose(arena, seg);
  p = SegBase(seg);
  if(SegBuffer(seg) != NULL)
    limit = BufferScanLimit(SegBuffer(seg));
  else
    limit = SegLimit(seg);
  while(p < limit) {
    Addr q;
    q = (*format->skip)(p);
    if(!AMCNailGetMark(seg, p)) {
      (*format->pad)(p, AddrOffset(p, q));
      bytesReclaimed += AddrOffset(p, q);
    }
    AVER(p < q);
    p = q;
  }
  AVER(p == limit);
  ShieldCover(arena, seg);

adjustColour:
  SegSetNailed(seg, TraceSetDel(SegNailed(seg), trace->ti));
  SegSetWhite(seg, TraceSetDel(SegWhite(seg), trace->ti));
  if(SegNailed(seg) == TraceSetEMPTY && AMCSegHasNailBoard(seg)) {
    AMCSegDestroyNailBoard(seg, pool);
  }

  AVER(bytesReclaimed <= SegSize(seg));
  trace->reclaimSize += bytesReclaimed;
}


/* AMCReclaim -- recycle a segment if it is still white
 *
 * See design.mps.poolamc.reclaim.
 */

static void AMCReclaim(Pool pool, Trace trace, Seg seg)
{
  AMC amc;
  AMCGen gen;
  Size size;

  AVERT_CRITICAL(Pool, pool);
  amc = PoolPoolAMC(pool);
  AVERT_CRITICAL(AMC, amc);
  AVERT_CRITICAL(Trace, trace);
  AVERT_CRITICAL(Seg, seg);

  gen = AMCSegGen(seg);
  AVERT_CRITICAL(AMCGen, gen);

  EVENT_PPP(AMCReclaim, gen, trace, seg);

  /* Should be (at most) once only @@@@ */
  if(gen->collected != ArenaMutatorAllocSize(PoolArena(pool))) {
    gen->collected = ArenaMutatorAllocSize(PoolArena(pool));
  }

  /* This switching needs to be more complex for multiple traces. */
  AVER_CRITICAL(TraceSetIsSingle(PoolArena(pool)->busyTraces));
  if(amc->rampMode == collectingRamp)
     if(amc->rampCount > 0)
       /* Entered ramp mode before previous one was cleaned up */
       amc->rampMode = beginRamp;
     else
       amc->rampMode = outsideRamp;

  if(SegNailed(seg) != TraceSetEMPTY) {
    AMCReclaimNailed(pool, trace, seg);
    return;
  }

  --gen->segs;
  size = SegSize(seg);
  gen->size -= size;

  trace->reclaimSize += size;

  SegFree(seg);
}


/* AMCSegDescribe -- describe the contents of a segment
 *
 * See design.mps.poolamc.seg-describe.
 */

static Res AMCSegDescribe(AMC amc, Seg seg, mps_lib_FILE *stream)
{
  Res res;
  Addr i, p, base, limit, init;
  Align step;
  Size row;

  step = amc->poolStruct.alignment;
  row = step * 64;

  base = SegBase(seg);
  p = base;
  limit = SegLimit(seg);
  if(SegBuffer(seg) != NULL)
    init = BufferGetInit(SegBuffer(seg));
  else
    init = limit;

  res = WriteF(stream,
               "AMC seg $P [$A,$A){\n",
               (WriteFP)seg, (WriteFA)base, (WriteFA)limit,
               "  Map\n",
               NULL);
  if(res != ResOK)
    return res;

  for(i = base; i < limit; i = AddrAdd(i, row)) {
    Addr j;
    char c;

    res = WriteF(stream, "    $A  ", i, NULL);
    if(res != ResOK)
      return res;

    /* @@@@ This needs to describe nailboards as well */
    for(j = i; j < AddrAdd(i, row); j = AddrAdd(j, step)) {
      if(j >= limit)
        c = ' ';
      else if(j >= init)
        c = '.';
      else if(j == p) {
        c = '*';
        p = (*amc->poolStruct.format->skip)(p);
      } else
        c = '=';
      res = WriteF(stream, "$C", c, NULL);
      if(res != ResOK)
        return res;
    }

    res = WriteF(stream, "\n", NULL);
    if(res != ResOK)
      return res;
  }

  res = WriteF(stream, "} AMC Seg $P\n", (WriteFP)seg, NULL);
  if(res != ResOK)
    return res;

  return ResOK;
}


/* AMCWalk -- Apply function to (black) objects in segment */

static void AMCWalk(Pool pool, Seg seg,
                    FormattedObjectsStepMethod f,
                    void *p, unsigned long s)
{
  AVERT(Pool, pool);
  AVERT(Seg, seg);
  AVER(FUNCHECK(f));
  /* p and s are arbitrary closures so can't be checked */

  /* Avoid applying the function to grey or white objects. */
  /* White objects might not be alive, and grey objects */
  /* may have pointers to old-space. */

  /* NB, segments containing a mix of colours (i.e., nailed segs) */
  /* are not handled properly:  No objects are walked @@@@ */
  if(SegWhite(seg) == TraceSetEMPTY &&
     SegGrey(seg) == TraceSetEMPTY &&
     SegNailed(seg) == TraceSetEMPTY) {
    Addr object = SegBase(seg);
    Addr nextObject;
    Addr limit;
    AMC amc;
    Format format;

    amc = PoolPoolAMC(pool);
    AVERT(AMC, amc);
    format = pool->format;

    /* If the segment is buffered, only walk as far as the end */
    /* of the initialized objects.  cf. AMCScan */
    if(SegBuffer(seg) != NULL)
      limit = BufferScanLimit(SegBuffer(seg));
    else
      limit = SegLimit(seg);

    while(object < limit) {
      /* Check not a broken heart. */
      AVER((*format->isMoved)(object) == NULL);
      (*f)(object, pool->format, pool, p, s);
      nextObject = (*pool->format->skip)(object);
      AVER(nextObject > object);
      object = nextObject;
    }
    AVER(object == limit);
  }
}


/* AMCWalkAll -- Apply a function to all (black) objects in a pool */

static void AMCWalkAll(Pool pool,
                       FormattedObjectsStepMethod f,
                       void *p, unsigned long s)
{
  Arena arena;
  Ring ring, next, node;

  AVERT(Pool, pool);
  AVER(FUNCHECK(f));
  /* p and s are arbitrary closures, hence can't be checked */
  AVER(pool->class == PoolClassAMC() ||
       pool->class == PoolClassAMCZ());

  arena = PoolArena(pool);

  ring = PoolSegRing(pool);
  node = RingNext(ring);
  RING_FOR(node, ring, next) {
    Seg seg = SegOfPoolRing(node);

    ShieldExpose(arena, seg);
    AMCWalk(pool, seg, f, p, s);
    ShieldCover(arena, seg);
  }
}


/* AMCDescribe -- describe the contents of the AMC pool
 *
 * See design.mps.poolamc.describe.
 */

static Res AMCDescribe(Pool pool, mps_lib_FILE *stream)
{
  Res res;
  AMC amc;
  Ring ring, node, nextNode;
  char *rampmode;

  if(!CHECKT(Pool, pool)) return ResFAIL;
  amc = PoolPoolAMC(pool);
  if(!CHECKT(AMC, amc)) return ResFAIL;

  res = WriteF(stream,
               (amc->rankSet == RankSetEMPTY) ? "AMCZ" : "AMC",
               " $P {\n", (WriteFP)amc, "  pool $P ($U)  ",
               (WriteFP)AMCPool(amc), (WriteFU)AMCPool(amc)->serial,
               NULL);
  if(res != ResOK)
    return res;

  /* @@@@ should add something about generations */

  switch(amc->rampMode) {
  case outsideRamp: rampmode = "outside ramp"; break;
  case beginRamp: rampmode = "begin ramp"; break;
  case ramping: rampmode = "ramping"; break;
  case finishRamp: rampmode = "finish ramp"; break;
  case collectingRamp: rampmode = "collecting ramp"; break;
  default: rampmode = "unknown ramp mode"; break;
  }
  res = WriteF(stream,
               "  ", rampmode, " ($U)", (WriteFU)amc->rampCount,
               NULL);
  if(res != ResOK)
    return res;

  ring = PoolSegRing(pool);
  RING_FOR(node, ring, nextNode) {
    Seg seg = SegOfPoolRing(node);
    AMCSegDescribe(amc, seg, stream);
  }

  res = WriteF(stream, "} AMC $P\n", (WriteFP)amc, NULL);
  if(res != ResOK)
    return res;

  return ResOK;
}


/* PoolClassAMCStruct -- the class descriptor */

static PoolClassStruct PoolClassAMCStruct = {
  PoolClassSig,
  "AMC",                                /* name */
  sizeof(AMCStruct),                    /* size */
  offsetof(AMCStruct, poolStruct),      /* offset */
  NULL,                                 /* super */
  AttrFMT | AttrSCAN | AttrBUF | AttrBUF_RESERVE |
    AttrGC | AttrMOVINGGC | AttrINCR_RB,
  AMCInit,                              /* init */
  AMCFinish,                            /* finish */
  PoolNoAlloc,                          /* alloc */
  PoolNoFree,                           /* free */
  AMCBufferInit,                        /* bufferInit */
  AMCBufferFill,                        /* bufferFill */
  AMCBufferEmpty,                       /* bufferEmpty */
  PoolTrivBufferFinish,                 /* bufferFinish */
  PoolTrivTraceBegin,                   /* traceBegin */
  PoolSegAccess,                        /* access */
  AMCWhiten,                            /* whiten */
  PoolTrivGrey,                         /* grey */
  PoolTrivBlacken,                      /* blacken */
  AMCScan,                              /* scan */
  AMCFix,                               /* fix */
  AMCFixEmergency,                      /* emergency fix */
  AMCReclaim,                           /* reclaim */
  AMCBenefit,                           /* benefit */
  AMCAct,                               /* act */
  AMCRampBegin,
  AMCRampEnd,
  AMCWalk,                              /* walk */
  AMCDescribe,                          /* describe */
  PoolNoDebugMixin,
  PoolClassSig                          /* impl.h.mpm.class.end-sig */
};


/* PoolClassAMCZStruct -- the class descriptor */

static PoolClassStruct PoolClassAMCZStruct = {
  PoolClassSig,
  "AMCZ",                               /* name */
  sizeof(AMCStruct),                    /* size */
  offsetof(AMCStruct, poolStruct),      /* offset */
  NULL,                                 /* super */
  AttrFMT | AttrBUF | AttrBUF_RESERVE |
    AttrGC | AttrMOVINGGC,
  AMCZInit,                             /* init */
  AMCFinish,                            /* finish */
  PoolNoAlloc,                          /* alloc */
  PoolNoFree,                           /* free */
  AMCBufferInit,                        /* bufferInit */
  AMCBufferFill,                        /* bufferFill */
  AMCBufferEmpty,                       /* bufferEmpty */
  PoolTrivBufferFinish,                 /* bufferFinish */
  PoolTrivTraceBegin,                   /* traceBegin */
  PoolSegAccess,                        /* access */
  AMCWhiten,                            /* whiten */
  PoolNoGrey,                           /* grey */
  PoolTrivBlacken,                      /* blacken */
  PoolNoScan,                           /* scan */
  AMCFix,                               /* fix */
  AMCFixEmergency,                      /* emergency fix */
  AMCReclaim,                           /* reclaim */
  AMCBenefit,                           /* benefit */
  AMCAct,                               /* act */
  AMCRampBegin,
  AMCRampEnd,
  AMCWalk,                              /* walk */
  AMCDescribe,                          /* describe */
  PoolNoDebugMixin,
  PoolClassSig                          /* impl.h.mpm.class.end-sig */
};


/* PoolClassAMC -- return the pool class descriptor */
/* Surely this function isn't used externally?  And is only used */
/* internally for dubious reasons?  @@@@ We should get rid of it */

PoolClass PoolClassAMC(void)
{
  return &PoolClassAMCStruct;
}

PoolClass PoolClassAMCZ(void)
{
  return &PoolClassAMCZStruct;
}


/* mps_class_amc -- return the pool class descriptor to the client */

mps_class_t mps_class_amc(void)
{
  return (mps_class_t)(&PoolClassAMCStruct);
}

/* mps_class_amcz -- return the pool class descriptor to the client */

mps_class_t mps_class_amcz(void)
{
  return (mps_class_t)(&PoolClassAMCZStruct);
}


/* mps_amc_apply -- apply function to all objects in pool
 *
 * The iterator that is passed by the client is stored in
 * a closure structure which is passed to a local iterator
 * in order to ensure that any type conversion necessary
 * between Addr and mps_addr_t happen.  They are almost
 * certainly the same on all platforms, but this is the
 * correct way to do it.
 */

typedef struct mps_amc_apply_closure_s {
  void (*f)(mps_addr_t object, void *p, size_t s);
  void *p;
  size_t s;
} mps_amc_apply_closure_s;

static void mps_amc_apply_iter(Addr addr, Format format, Pool pool,
                               void *p, unsigned long s)
{
  mps_amc_apply_closure_s *closure = p;
  /* Can't check addr */
  AVERT(Format, format);
  AVERT(Pool, pool);
  /* We could check that s is the sizeof *p, but it would be slow */
  UNUSED(format);
  UNUSED(pool);
  UNUSED(s);
  (*closure->f)(addr, closure->p, closure->s);
}

void mps_amc_apply(mps_pool_t mps_pool,
                   void (*f)(mps_addr_t object, void *p, size_t s),
                   void *p, size_t s)
{
  Pool pool = (Pool)mps_pool;
  mps_amc_apply_closure_s closure_s;
  Arena arena;

  AVER(CHECKT(Pool, pool));
  arena = PoolArena(pool);
  ArenaEnter(arena);
  AVERT(Pool, pool);

  closure_s.f = f; closure_s.p = p; closure_s.s = s;
  AMCWalkAll(pool, mps_amc_apply_iter, &closure_s, sizeof(closure_s));

  ArenaLeave(arena);
}


/* AMCCheck -- check consistency of the AMC pool
 *
 * See design.mps.poolamc.check.
 */

static Bool AMCCheck(AMC amc)
{
  CHECKS(AMC, amc);
  CHECKD(Pool, &amc->poolStruct);
  CHECKL(amc->poolStruct.class == &PoolClassAMCStruct ||
         amc->poolStruct.class == &PoolClassAMCZStruct);
  CHECKL(RankSetCheck(amc->rankSet));
  CHECKL(RingCheck(&amc->genRing));
  if(amc->nursery != NULL)
    CHECKD(AMCGen, amc->nursery);
  if(amc->rampGen != NULL)
    CHECKD(AMCGen, amc->rampGen);
  if(amc->afterRampGen != NULL)
    CHECKD(AMCGen, amc->afterRampGen);
  /* nothing to check for rampCount */
  CHECKL(amc->rampMode >= outsideRamp && amc->rampMode <= collectingRamp);

  CHECKL((unsigned long)(Serial)AMCTopGen == AMCTopGen);
  CHECKL(AMCTopGen >= 2); /* AMCBenefit assumes three gens */
  CHECKL(AMCGenFinal <= AMCTopGen);
  CHECKL(AMCTopGen + 1 > 0); /* we can represent the ramp gen */

  return TRUE;
}

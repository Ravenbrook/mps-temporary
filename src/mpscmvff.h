/* impl.h.mpscmvff: MEMORY POOL SYSTEM CLASS "MVFF"
 *
 * $HopeName: !mpscmvff.h(trunk.2) $
 * Copyright (C) 1998 Harlequin Group plc, all rights reserved.
 */

#ifndef mpscmvff_h
#define mpscmvff_h

#include "mps.h"

extern size_t mps_mvff_free_size(mps_pool_t mps_pool);
extern size_t mps_mvff_size(mps_pool_t mps_pool);
extern mps_class_t mps_class_mvff(void);

#endif /* mpscmvff_h */

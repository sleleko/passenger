/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2010 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_HOOKS_H_
#define _PASSENGER_HOOKS_H_

#include <apr_pools.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup Hooks Apache hooks
 * @ingroup Core
 */

extern void passenger_register_hooks(apr_pool_t *p);

#ifdef __cplusplus
}
#endif

#endif /* _PASSENGER_HOOKS_H_ */

/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_OLEAUT32_CONNPT_H
#define RAVEN_OLEAUT32_CONNPT_H

#include "oa.h"

/* A point lives as long as its container: it forwards its references there. */
HRESULT oa_connection_point_create(IUnknown *owner, REFIID iid, IConnectionPoint **out);
/* Each live sink, one call at a time; a sink that vanishes meanwhile is skipped. */
typedef void (*oa_sink_call)(IUnknown *sink, void *context);
void oa_connection_point_each(IConnectionPoint *point, oa_sink_call call, void *context);
void oa_connection_point_destroy(IConnectionPoint *point);

#endif

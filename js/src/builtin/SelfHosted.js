/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Build the message table using a macro-generating macro.
#defmeta MSG_DEF(name, id, _, _, _) \
  #define name id
#include ../js.msg
#undef MSG_DEF

// Common utility macros.
#define TO_UINT32(x) (x >>> 0)

#include Utilities.js
#include Array.js
#include ParallelArray.js
#include Intl.js
#include IntlData.js

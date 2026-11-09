#pragma once
/* An embedding application may provide a compile-time CPU profiling hook.
 * The default has no runtime cost or dependency. */
#ifndef RN_PROFILE_SCOPE
#define RN_PROFILE_SCOPE()
#endif

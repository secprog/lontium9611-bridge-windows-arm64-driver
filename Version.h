/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#ifdef LT9611_BUILD_VERSION_GENERATED
#include "lt9611_build_version.h"
#else
#define LT9611_VERSION_MAJOR 1
#define LT9611_VERSION_MINOR 0
#define LT9611_VERSION_BUILD 0
#define LT9611_VERSION_REVISION 0
#endif

#define LT9611_VERSION_TEXT_IMPL(a, b, c, d) #a "." #b "." #c "." #d
#define LT9611_VERSION_TEXT(a, b, c, d) LT9611_VERSION_TEXT_IMPL(a, b, c, d)
#define LT9611_VERSION_STRING LT9611_VERSION_TEXT(LT9611_VERSION_MAJOR, LT9611_VERSION_MINOR, LT9611_VERSION_BUILD, LT9611_VERSION_REVISION)
#define LT9611_VERSION_NUMBER LT9611_VERSION_MAJOR, LT9611_VERSION_MINOR, LT9611_VERSION_BUILD, LT9611_VERSION_REVISION

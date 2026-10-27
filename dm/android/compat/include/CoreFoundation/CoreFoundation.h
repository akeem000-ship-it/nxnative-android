// Android stand-in for <CoreFoundation/CoreFoundation.h>: the pointer-keyed mutable dictionary the NVN layer keeps side tables in (NULL callbacks: keys and values are
// raw pointers, compared by address and neither retained nor released). Nothing else of CoreFoundation is provided
#pragma once
#include "../darwin_compat.h"
typedef long CFIndex;
typedef const void *CFTypeRef, *CFAllocatorRef;
typedef struct dm_cfdict *CFMutableDictionaryRef;
typedef const struct dm_cfdict *CFDictionaryRef;
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *keycb, const void *valcb);   // the callbacks must be NULL
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *key);
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *key, const void *value);
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *key);
CFIndex CFDictionaryGetCount(CFDictionaryRef d);
void CFRelease(CFTypeRef d);   // only for dictionaries made here

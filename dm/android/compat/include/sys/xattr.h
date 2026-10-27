// Android stand-in for Darwin's <sys/xattr.h>: Darwin's calls take a position and an options word (XATTR_NOFOLLOW → the l* call). Names go to the user. namespace,
// the only one an app may write on Linux (Darwin names have no namespace)
#pragma once
#include_next <sys/xattr.h>
#include <stdint.h>
#define XATTR_NOFOLLOW 0x0001
ssize_t dm_getxattr(const char *p, const char *n, void *v, size_t s, uint32_t pos, int opt);
int dm_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opt);
int dm_removexattr(const char *p, const char *n, int opt);
#define getxattr(p, n, v, s, pos, opt) dm_getxattr(p, n, v, s, pos, opt)
#define setxattr(p, n, v, s, pos, opt) dm_setxattr(p, n, v, s, pos, opt)
#define removexattr(p, n, opt) dm_removexattr(p, n, opt)

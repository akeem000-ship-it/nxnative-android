// Android stand-in for the Apple header <sys/clonefile.h> (and renamex_np from Darwin's <stdio.h>), for the save system (dm/host/hle_fs.c):
//   clonefile copies the tree (ext4 / f2fs have no copy-on-write clones: a real copy, files and directories, xattrs included; saves are small);
//   renamex_np(RENAME_SWAP) is renameat2(RENAME_EXCHANGE), atomic on ext4 and f2fs like APFS's swap
#pragma once
#include "../../darwin_compat.h"
#include <stdio.h>
#define CLONE_NOFOLLOW 0x0001
#define CLONE_NOOWNERCOPY 0x0002
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x00000002
#define RENAME_EXCL 0x00000004
#endif
int clonefile(const char *src, const char *dst, uint32_t flags);   // fails with EEXIST when dst exists, as on Darwin
int renamex_np(const char *from, const char *to, unsigned flags);

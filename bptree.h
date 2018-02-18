#ifndef __BPTREE_H__
#define __BPTREE_H__

typedef void * bpt_handle;
typedef unsigned char bpt_level;
typedef unsigned long long bpt_pageno_t;
typedef unsigned char boolean_t;

extern bpt_handle bpt_open(const char *name, unsigned int page_bits);
extern void bpt_close(bpt_handle h);
extern int bpt_insertkey(bpt_handle h, unsigned char *key,
			 unsigned int len, bpt_level level,
			 bpt_pageno_t page_no);
extern int bpt_deletekey(bpt_handle h, unsigned char *key,
			 unsigned int len, bpt_level level);
extern unsigned int bpt_firstkey(bpt_handle h, unsigned char *key,
				 unsigned int len);
extern unsigned int bpt_nextkey(bpt_handle h, unsigned int slot);

#endif	/* __BPTREE_H__ */

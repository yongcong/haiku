/*
 * Copyright 2007, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PAGE_CACHE_LOCKER_H
#define PAGE_CACHE_LOCKER_H


#include <null.h>

struct vm_page;


class PageCacheLocker {
public:
	PageCacheLocker(vm_page* page);
	~PageCacheLocker();

	bool IsLocked() { return fPage != NULL; }

	bool Lock(vm_page* page);
	void Unlock();

private:
	bool _IgnorePage(vm_page* page);

	vm_page*	fPage;
};

#endif	// PAGE_CACHE_LOCKER_H

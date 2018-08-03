//
//  pager-imp.h
//  sqlite
//
//  Created by wangjufan on 2018/8/3.
//  Copyright © 2018 wangjufan. All rights reserved.
//

#ifndef pager_imp_h
#define pager_imp_h

#include <stdio.h>

static int pagerStress(void *p, PgHdr *pPg);
static int pager_write_pagelist(Pager *pPager, PgHdr *pList);

#endif /* pager_imp_h */

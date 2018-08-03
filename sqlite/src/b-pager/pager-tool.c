//
//  pager-tool.c
//  sqlite
//
//  Created by wangjufan on 2018/8/3.
//  Copyright © 2018 wangjufan. All rights reserved.
//

#include "pager-tool.h"

/*
 ** Return a 32-bit hash of the page data for pPage.
 */
static u32 pager_datahash(int nByte, unsigned char *pData){
    u32 hash = 0;
    int i;
    for(i=0; i<nByte; i++){
        hash = (hash*1039) + pData[i];
    }
    return hash;
}
static u32 pager_pagehash(PgHdr *pPage){
    return pager_datahash(pPage->pPager->pageSize, (unsigned char *)pPage->pData);
}
static void pager_set_pagehash(PgHdr *pPage){
    pPage->pageHash = pager_pagehash(pPage);
}



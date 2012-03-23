/*
** Copyright (c) 2007 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)

** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This file contains code used to create a RSS feed for the CGI interface.
*/
#include "config.h"
#include <time.h>
#include "rss.h"
#include <assert.h>

/*
** WEBPAGE: timeline.rss
*/
void page_timeline_rss(void){
  Stmt q;
  int nLine=0;
  char *zPubDate, *zProjectName, *zProjectDescr, *zFreeProjectName=0;
  Blob bSQL;
  const char *zType = PD("y","all"); /* Type of events.  All if NULL */
  int nLimit = atoi(PD("n","20"));
  const char zSQL1[] =
    @ SELECT
    @   blob.rid,
    @   uuid,
    @   event.mtime,
    @   coalesce(ecomment,comment),
    @   coalesce(euser,user),
    @   (SELECT count(*) FROM plink WHERE pid=blob.rid AND isprim),
    @   (SELECT count(*) FROM plink WHERE cid=blob.rid)
    @ FROM event, blob
    @ WHERE blob.rid=event.objid
  ;

  login_check_credentials();
  if( !g.perm.Read && !g.perm.RdTkt && !g.perm.RdWiki ){ 
    return;
  }

  blob_zero(&bSQL);
  blob_append( &bSQL, zSQL1, -1 );

  if( zType[0]!='a' ){
    if( zType[0]=='c' && !g.perm.Read ) zType = "x";
    if( zType[0]=='w' && !g.perm.RdWiki ) zType = "x";
    if( zType[0]=='t' && !g.perm.RdTkt ) zType = "x";
    blob_appendf(&bSQL, " AND event.type=%Q", zType);
  }else{
    if( !g.perm.Read ){
      if( g.perm.RdTkt && g.perm.RdWiki ){
        blob_append(&bSQL, " AND event.type!='ci'", -1);
      }else if( g.perm.RdTkt ){
        blob_append(&bSQL, " AND event.type=='t'", -1);
      }else{
        blob_append(&bSQL, " AND event.type=='w'", -1);
      }
    }else if( !g.perm.RdWiki ){
      if( g.perm.RdTkt ){
        blob_append(&bSQL, " AND event.type!='w'", -1);
      }else{
        blob_append(&bSQL, " AND event.type=='ci'", -1);
      }
    }else if( !g.perm.RdTkt ){
      assert( !g.perm.RdTkt &&& g.perm.Read && g.perm.RdWiki );
      blob_append(&bSQL, " AND event.type!='t'", -1);
    }
  }

  blob_append( &bSQL, " ORDER BY event.mtime DESC", -1 );

  cgi_set_content_type("application/rss+xml");

  zProjectName = db_get("project-name", 0);
  if( zProjectName==0 ){
    zFreeProjectName = zProjectName = mprintf("Fossil source repository for: %s",
      g.zBaseURL);
  }
  zProjectDescr = db_get("project-description", 0);
  if( zProjectDescr==0 ){
    zProjectDescr = zProjectName;
  }

  zPubDate = cgi_rfc822_datestamp(time(NULL));

  @ <?xml version="1.0"?>
  @ <rss xmlns:dc="http://purl.org/dc/elements/1.1/" version="2.0">
  @   <channel>
  @     <title>%h(zProjectName)</title>
  @     <link>%s(g.zBaseURL)</link>
  @     <description>%h(zProjectDescr)</description>
  @     <pubDate>%s(zPubDate)</pubDate>
  @     <generator>Fossil version %s(MANIFEST_VERSION) %s(MANIFEST_DATE)</generator>
  free(zPubDate);
  db_prepare(&q, blob_str(&bSQL));
  blob_reset( &bSQL );
  while( db_step(&q)==SQLITE_ROW && nLine<=nLimit ){
    const char *zId = db_column_text(&q, 1);
    const char *zCom = db_column_text(&q, 3);
    const char *zAuthor = db_column_text(&q, 4);
    char *zPrefix = "";
    char *zDate;
    int nChild = db_column_int(&q, 5);
    int nParent = db_column_int(&q, 6);
    time_t ts;

    ts = (time_t)((db_column_double(&q,2) - 2440587.5)*86400.0);
    zDate = cgi_rfc822_datestamp(ts);

    if( nParent>1 && nChild>1 ){
      zPrefix = "*MERGE/FORK* ";
    }else if( nParent>1 ){
      zPrefix = "*MERGE* ";
    }else if( nChild>1 ){
      zPrefix = "*FORK* ";
    }

    @     <item>
    @       <title>%s(zPrefix)%h(zCom)</title>
    @       <link>%s(g.zBaseURL)/info/%s(zId)</link>
    @       <description>%s(zPrefix)%h(zCom)</description>
    @       <pubDate>%s(zDate)</pubDate>
    @       <dc:creator>%h(zAuthor)</dc:creator>
    @       <guid>%s(g.zBaseURL)/info/%s(zId)</guid>
    @     </item>
    free(zDate);
    nLine++;
  }

  db_finalize(&q);
  @   </channel>
  @ </rss>

  if( zFreeProjectName != 0 ){
    free( zFreeProjectName );
  }
}

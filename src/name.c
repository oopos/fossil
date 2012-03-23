/*
** Copyright (c) 2006 D. Richard Hipp
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
** This file contains code used to convert user-supplied object names into
** canonical UUIDs.
**
** A user-supplied object name is any unique prefix of a valid UUID but
** not necessarily in canonical form.  
*/
#include "config.h"
#include "name.h"
#include <assert.h>

/*
** Return TRUE if the string begins with something that looks roughly
** like an ISO date/time string.  The SQLite date/time functions will
** have the final say-so about whether or not the date/time string is
** well-formed.
*/
static int is_date(const char *z){
  if( !fossil_isdigit(z[0]) ) return 0;
  if( !fossil_isdigit(z[1]) ) return 0;
  if( !fossil_isdigit(z[2]) ) return 0;
  if( !fossil_isdigit(z[3]) ) return 0;
  if( z[4]!='-') return 0;
  if( !fossil_isdigit(z[5]) ) return 0;
  if( !fossil_isdigit(z[6]) ) return 0;
  if( z[7]!='-') return 0;
  if( !fossil_isdigit(z[8]) ) return 0;
  if( !fossil_isdigit(z[9]) ) return 0;
  return 1;
}

/*
** Convert a symbolic name into a RID.  Acceptable forms:
**
**   *  SHA1 hash
**   *  SHA1 hash prefix of at least 4 characters
**   *  Symbolic Name
**   *  "tag:" + symbolic name
**   *  Date or date-time 
**   *  "date:" + Date or date-time
**   *  symbolic-name ":" date-time
**   *  "tip"
**
** The following additional forms are available in local checkouts:
**
**   *  "current"
**   *  "prev" or "previous"
**   *  "next"
**
** Return the RID of the matching artifact.  Or return 0 if the name does not
** match any known object.  Or return -1 if the name is ambiguious.
**
** The zType parameter specifies the type of artifact: ci, t, w, e, g. 
** If zType is NULL or "" or "*" then any type of artifact will serve.
** zType is "ci" in most use cases since we are usually searching for
** a check-in.
*/
int symbolic_name_to_rid(const char *zTag, const char *zType){
  int vid;
  int rid = 0;
  int nTag;
  int i;

  if( zType==0 || zType[0]==0 ) zType = "*";
  if( zTag==0 || zTag[0]==0 ) return 0;

  /* special keyword: "tip" */
  if( fossil_strcmp(zTag, "tip")==0 && (zType[0]=='*' || zType[0]=='c') ){
    rid = db_int(0,
      "SELECT objid"
      "  FROM event"
      " WHERE type='ci'"
      " ORDER BY event.mtime DESC"
    );
    if( rid ) return rid;
  }

  /* special keywords: "prev", "previous", "current", and "next" */
  if( g.localOpen && (vid=db_lget_int("checkout",0))!=0 ){
    if( fossil_strcmp(zTag, "current")==0 ){
      rid = vid;
    }else if( fossil_strcmp(zTag, "prev")==0 
              || fossil_strcmp(zTag, "previous")==0 ){
      rid = db_int(0, "SELECT pid FROM plink WHERE cid=%d AND isprim", vid);
    }else if( fossil_strcmp(zTag, "next")==0 ){
      rid = db_int(0, "SELECT cid FROM plink WHERE pid=%d"
                      "  ORDER BY isprim DESC, mtime DESC", vid);
    }
    if( rid ) return rid;
  }

  /* Date and times */
  if( memcmp(zTag, "date:", 5)==0 ){
    rid = db_int(0, 
      "SELECT objid FROM event"
      " WHERE mtime<=julianday(%Q) AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      &zTag[5], zType);
    return rid;
  }
  if( is_date(zTag) ){
    rid = db_int(0, 
      "SELECT objid FROM event"
      " WHERE mtime<=julianday(%Q) AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      zTag, zType);
    if( rid) return rid;
  }

  /* Deprecated date & time formats:   "local:" + date-time and
  ** "utc:" + date-time */
  if( memcmp(zTag, "local:", 6)==0 ){
    rid = db_int(0, 
      "SELECT objid FROM event"
      " WHERE mtime<=julianday(%Q) AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      &zTag[6], zType);
    return rid;
  }
  if( memcmp(zTag, "utc:", 4)==0 ){
    rid = db_int(0, 
      "SELECT objid FROM event"
      " WHERE mtime<=julianday('%qz') AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      &zTag[4], zType);
    return rid;
  }

  /* "tag:" + symbolic-name */
  if( memcmp(zTag, "tag:", 4)==0 ){
    rid = db_int(0,
       "SELECT event.objid"
       "  FROM tag, tagxref, event"
       " WHERE tag.tagname='sym-%q' "
       "   AND tagxref.tagid=tag.tagid AND tagxref.tagtype>0 "
       "   AND event.objid=tagxref.rid "
       "   AND event.type GLOB '%q'"
       " ORDER BY event.mtime DESC /*sort*/",
       &zTag[4], zType
    );
    return rid;
  }

  /* symbolic-name ":" date-time */
  nTag = strlen(zTag);
  for(i=0; i<nTag-10 && zTag[i]!=':'; i++){}
  if( zTag[i]==':' && is_date(&zTag[i+1]) ){
    char *zDate = mprintf("%s", &zTag[i+1]);
    char *zTagBase = mprintf("%.*s", i, zTag);
    int nDate = strlen(zDate);
    if( sqlite3_strnicmp(&zDate[nDate-3],"utc",3)==0 ){
      zDate[nDate-3] = 'z';
      zDate[nDate-2] = 0;
    }
    rid = db_int(0,
      "SELECT event.objid"
      "  FROM tag, tagxref, event"
      " WHERE tag.tagname='sym-%q' "
      "   AND tagxref.tagid=tag.tagid AND tagxref.tagtype>0 "
      "   AND event.objid=tagxref.rid "
      "   AND event.mtime<=julianday(%Q)"
      "   AND event.type GLOB '%q'"
      " ORDER BY event.mtime DESC /*sort*/ ",
      zTagBase, zDate, zType
    );
    return rid;
  }

  /* SHA1 hash or prefix */
  if( nTag>=4 && nTag<=UUID_SIZE && validate16(zTag, nTag) ){
    Stmt q;
    char zUuid[UUID_SIZE+1];
    memcpy(zUuid, zTag, nTag+1);
    canonical16(zUuid, nTag);
    rid = 0;
    if( zType[0]=='*' ){
      db_prepare(&q, "SELECT rid FROM blob WHERE uuid GLOB '%s*'", zUuid);
    }else{
      db_prepare(&q,
        "SELECT blob.rid"
        "  FROM blob, event"
        " WHERE blob.uuid GLOB '%s*'"
        "   AND event.objid=blob.rid"
        "   AND event.type GLOB '%q'",
        zUuid, zType
      );
    }
    if( db_step(&q)==SQLITE_ROW ){
      rid = db_column_int(&q, 0);
      if( db_step(&q)==SQLITE_ROW ) rid = -1;
    }
    db_finalize(&q);
    if( rid ) return rid;
  }

  /* Symbolic name */
  rid = db_int(0,
    "SELECT event.objid"
    "  FROM tag, tagxref, event"
    " WHERE tag.tagname='sym-%q' "
    "   AND tagxref.tagid=tag.tagid AND tagxref.tagtype>0 "
    "   AND event.objid=tagxref.rid "
    "   AND event.type GLOB '%q'"
    " ORDER BY event.mtime DESC /*sort*/ ",
    zTag, zType
  );
  if( rid>0 ) return rid;

  /* Undocumented:  numeric tags get translated directly into the RID */
  for(i=0; fossil_isdigit(zTag[i]); i++){}
  if( zTag[i]==0 ){
    rid = db_int(0, 
      "SELECT event.objid"
      "  FROM event"
      " WHERE event.objid=%s"
      "   AND event.type GLOB '%q'", zTag, zType);
  }
  return rid;
}


/*
** This routine takes a user-entered UUID which might be in mixed
** case and might only be a prefix of the full UUID and converts it
** into the full-length UUID in canonical form.
**
** If the input is not a UUID or a UUID prefix, then try to resolve
** the name as a tag.  If multiple tags match, pick the latest.
** If the input name matches "tag:*" then always resolve as a tag.
**
** If the input is not a tag, then try to match it as an ISO-8601 date
** string YYYY-MM-DD HH:MM:SS and pick the nearest check-in to that date.
** If the input is of the form "date:*" or "localtime:*" or "utc:*" then
** always resolve the name as a date.
**
** Return 0 on success.  Return 1 if the name cannot be resolved.
** Return 2 name is ambiguous.
*/
int name_to_uuid(Blob *pName, int iErrPriority, const char *zType){
  char *zName = blob_str(pName);
  int rid = symbolic_name_to_rid(zName, zType);
  if( rid<0 ){
    fossil_error(iErrPriority, "ambiguous name: %s", zName);
    return 2;
  }else if( rid==0 ){
    fossil_error(iErrPriority, "not found: %s", zName);
    return 1;
  }else{
    blob_reset(pName);
    db_blob(pName, "SELECT uuid FROM blob WHERE rid=%d", rid);
    return 0;
  }
}

/*
** COMMAND:  test-name-to-id
**
** Convert a name to a full artifact ID.
*/
void test_name_to_id(void){
  int i;
  Blob name;
  db_must_be_within_tree();
  for(i=2; i<g.argc; i++){
    blob_init(&name, g.argv[i], -1);
    fossil_print("%s -> ", g.argv[i]);
    if( name_to_uuid(&name, 1, "*") ){
      fossil_print("ERROR: %s\n", g.zErrMsg);
      fossil_error_reset();
    }else{
      fossil_print("%s\n", blob_buffer(&name));
    }
    blob_reset(&name);
  }
}

/*
** Convert a name to a rid.  If the name can be any of the various forms
** accepted:
**
**   * SHA1 hash or prefix thereof
**   * symbolic name
**   * date
**   * label:date
**   * prev, previous
**   * next
**   * tip
**
** This routine is used by command-line routines to resolve command-line inputs
** into a rid.
*/
int name_to_typed_rid(const char *zName, const char *zType){
  int rid;

  if( zName==0 || zName[0]==0 ) return 0;
  rid = symbolic_name_to_rid(zName, zType);
  if( rid<0 ){
    fossil_error(1, "ambiguous name: %s", zName);
    return 0;
  }else if( rid==0 ){
    fossil_error(1, "not found: %s", zName);
    return 0;
  }else{
    return rid;
  }
}
int name_to_rid(const char *zName){
  return name_to_typed_rid(zName, "*");
}

/*
** WEBPAGE: ambiguous
** URL: /ambiguous?name=UUID&src=WEBPAGE
** 
** The UUID given by the name paramager is ambiguous.  Display a page
** that shows all possible choices and let the user select between them.
*/
void ambiguous_page(void){
  Stmt q;
  const char *zName = P("name");  
  const char *zSrc = P("src");
  char *z;
  
  if( zName==0 || zName[0]==0 || zSrc==0 || zSrc[0]==0 ){
    fossil_redirect_home();
  }
  style_header("Ambiguous Artifact ID");
  @ <p>The artifact id <b>%h(zName)</b> is ambiguous and might
  @ mean any of the following:
  @ <ol>
  z = mprintf("%s", zName);
  canonical16(z, strlen(z));
  db_prepare(&q, "SELECT uuid, rid FROM blob WHERE uuid GLOB '%q*'", z);
  while( db_step(&q)==SQLITE_ROW ){
    const char *zUuid = db_column_text(&q, 0);
    int rid = db_column_int(&q, 1);
    @ <li><p><a href="%s(g.zTop)/%T(zSrc)/%S(zUuid)">
    @ %S(zUuid)</a> -
    object_description(rid, 0, 0);
    @ </p></li>
  }
  @ </ol>
  style_footer();
}

/*
** Convert the name in CGI parameter zParamName into a rid and return that
** rid.  If the CGI parameter is missing or is not a valid artifact tag,
** return 0.  If the CGI parameter is ambiguous, redirect to a page that
** shows all possibilities and do not return.
*/
int name_to_rid_www(const char *zParamName){
  int rid;
  const char *zName = P(zParamName);
#ifdef FOSSIL_ENABLE_JSON
  if(!zName && fossil_has_json()){
    zName = json_find_option_cstr(zParamName,NULL,NULL);
  }
#endif
  if( zName==0 || zName[0]==0 ) return 0;
  rid = symbolic_name_to_rid(zName, "*");
  if( rid<0 ){
    cgi_redirectf("%s/ambiguous/%T?src=%t", g.zTop, zName, g.zPath);
    rid = 0;
  }
  return rid;
}

/*
** COMMAND: whatis*
** Usage: %fossil whatis NAME
**
** Resolve the symbol NAME into its canonical 40-character SHA1-hash
** artifact name and provide a description of what role that artifact
** plays.
*/
void whatis_cmd(void){
  int rid;
  const char *zName;
  int fExtra;
  db_find_and_open_repository(0,0);
  fExtra = find_option("verbose","v",0)!=0;
  if( g.argc!=3 ) usage("whatis NAME");
  zName = g.argv[2];
  rid = symbolic_name_to_rid(zName, 0);
  if( rid<0 ){
    fossil_print("Ambiguous artifact name prefix: %s\n", zName);
  }else if( rid==0 ){
    fossil_print("Unknown artifact: %s\n", zName);
  }else{
    Stmt q;
    db_prepare(&q,
       "SELECT uuid, size, datetime(mtime, 'localtime'), ipaddr,"
       "       (SELECT group_concat(substr(tagname,5), ', ') FROM tag, tagxref"
       "         WHERE tagname GLOB 'sym-*' AND tag.tagid=tagxref.tagid"
       "           AND tagxref.rid=blob.rid AND tagxref.tagtype>0)"
       "  FROM blob, rcvfrom"
       " WHERE rid=%d"
       "   AND rcvfrom.rcvid=blob.rcvid",
       rid);
    if( db_step(&q)==SQLITE_ROW ){
      const char *zTagList = db_column_text(&q, 4);
      if( fExtra ){
        fossil_print("artifact: %s (%d)\n", db_column_text(&q,0), rid);
        fossil_print("size:     %d bytes\n", db_column_int(&q,1));
        fossil_print("received: %s from %s\n",
           db_column_text(&q, 2),
           db_column_text(&q, 3));
      }else{
        fossil_print("artifact: %s\n", db_column_text(&q,0));
        fossil_print("size:     %d bytes\n", db_column_int(&q,1));
      }
      if( zTagList && zTagList[0] ){
        fossil_print("tags:     %s\n", zTagList);
      }
    }
    db_finalize(&q);
    db_prepare(&q,
       "SELECT type, datetime(mtime,'localtime'),"
       "       coalesce(euser,user), coalesce(ecomment,comment)"
       "  FROM event WHERE objid=%d", rid);
    if( db_step(&q)==SQLITE_ROW ){
      const char *zType;
      switch( db_column_text(&q,0)[0] ){
        case 'c':  zType = "Check-in";       break;
        case 'w':  zType = "Wiki-edit";      break;
        case 'e':  zType = "Event";          break;
        case 't':  zType = "Ticket-change";  break;
        case 'g':  zType = "Tag-change";     break;
        default:   zType = "Unknown";        break;
      }
      fossil_print("type:     %s by %s on %s\n", zType, db_column_text(&q,2),
                   db_column_text(&q, 1));
      fossil_print("comment:  ");
      comment_print(db_column_text(&q,3), 10, 78);
    }
    db_finalize(&q);
    db_prepare(&q,
      "SELECT filename.name, blob.uuid, datetime(event.mtime,'localtime'),"
      "       coalesce(euser,user), coalesce(ecomment,comment)"
      "  FROM mlink, filename, blob, event"
      " WHERE mlink.fid=%d"
      "   AND filename.fnid=mlink.fnid"
      "   AND event.objid=mlink.mid"
      "   AND blob.rid=mlink.mid"
      " ORDER BY event.mtime DESC /*sort*/",
      rid);
    while( db_step(&q)==SQLITE_ROW ){
      fossil_print("file:     %s\n", db_column_text(&q,0));
      fossil_print("          part of [%.10s] by %s on %s\n",
        db_column_text(&q, 1),
        db_column_text(&q, 3),
        db_column_text(&q, 2));
      fossil_print("          ");
      comment_print(db_column_text(&q,4), 10, 78);
    }
    db_finalize(&q);
  }
}

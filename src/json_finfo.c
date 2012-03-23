#ifdef FOSSIL_ENABLE_JSON
/*
** Copyright (c) 2011 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*/
#include "VERSION.h"
#include "config.h"
#include "json_finfo.h"

#if INTERFACE
#include "json_detail.h"
#endif

/*
** Implements the /json/finfo page/command.
**
*/
cson_value * json_page_finfo(){
  cson_object * pay = NULL;
  cson_array * checkins = NULL;
  char const * zFilename = NULL;
  Blob sql = empty_blob;
  Stmt q = empty_Stmt;
  char const * zAfter = NULL;
  char const * zBefore = NULL;
  int limit = -1;
  int currentRow = 0;
  if(!g.perm.Read){
    json_set_err(FSL_JSON_E_DENIED,"Requires 'o' privileges.");
    return NULL;
  }
  json_warn( FSL_JSON_W_UNKNOWN, "Achtung: the output of the finfo command is up for change.");
  zFilename = json_find_option_cstr2("name",NULL,NULL,
                                 g.json.dispatchDepth+1);

  if(!zFilename){
    json_set_err(FSL_JSON_E_MISSING_ARGS, "Missing 'name' parameter.");
    return NULL;
  }
  zBefore = json_find_option_cstr("before",NULL,"b");
  zAfter = json_find_option_cstr("after",NULL,"a");
  limit = json_find_option_int("limit",NULL,"n", -1);

  blob_appendf(&sql, 
        "SELECT b.uuid,"
        "   ci.uuid,"
        "   (SELECT uuid FROM blob WHERE rid=mlink.fid),"  /* Current file uuid */
        "   cast(strftime('%%s',event.mtime) AS INTEGER),"
        "   coalesce(event.euser, event.user),"
        "   coalesce(event.ecomment, event.comment),"
#if 1
        " (SELECT uuid FROM blob WHERE rid=mlink.pid),"  /* Parent file uuid */
#endif
        "   event.bgcolor,"
        " 1"
        "  FROM mlink, blob b, event, blob ci, filename"
        " WHERE filename.name=%Q %s"
        "   AND mlink.fnid=filename.fnid"
        "   AND b.rid=mlink.fid"
        "   AND event.objid=mlink.mid"
        "   AND event.objid=ci.rid",
        zFilename, filename_collation()
               );
  if( zAfter ){
    blob_appendf(&sql, " AND event.mtime>=julianday('%q')", zAfter);
  }else if( zBefore ){
    blob_appendf(&sql, " AND event.mtime<=julianday('%q')", zBefore);
  }
  blob_appendf(&sql," ORDER BY event.mtime DESC /*sort*/");
  db_prepare(&q, "%s", blob_str(&sql)/*extra %s to avoid double-expanding
                                       SQL escapes*/);
  blob_reset(&sql);

  pay = cson_new_object();
  cson_object_set(pay, "name", json_new_string(zFilename));
  if( limit > 0 ){
    cson_object_set(pay, "limit", json_new_int(limit));
  }
  checkins = cson_new_array();
  cson_object_set(pay, "checkins", cson_array_value(checkins));
  while( db_step(&q)==SQLITE_ROW ){
    cson_object * row = cson_new_object();
    cson_array_append( checkins, cson_object_value(row) );
    cson_object_set(row, "checkin", json_new_string( db_column_text(&q,1) ));
    cson_object_set(row, "artifact", json_new_string( db_column_text(&q,2) ));
    /*cson_object_set(row, "parentArtifact", json_new_string( db_column_text(&q,6) ));*/
    cson_object_set(row, "mtime", json_new_int( db_column_int(&q,3) ));
    cson_object_set(row, "user", json_new_string( db_column_text(&q,4) ));
    cson_object_set(row, "comment", json_new_string( db_column_text(&q,5) ));
    /*cson_object_set(row, "bgColor", json_new_string( db_column_text(&q,7) ));*/
    if( (0 < limit) && (++currentRow >= limit) ){
      break;
    }
  }
  db_finalize(&q);

  return cson_object_value(pay);
}



#endif /* FOSSIL_ENABLE_JSON */

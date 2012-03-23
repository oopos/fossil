/*
** Copyright (c) 2009 D. Richard Hipp
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
** This file contains code to implement the "finfo" command.
*/
#include "config.h"
#include "finfo.h"

/*
** COMMAND: finfo
** 
** Usage: %fossil finfo ?OPTIONS? FILENAME
**
** Print the complete change history for a single file going backwards
** in time.  The default is -l.
**
** For the -l|--log option: If "-b|--brief" is specified one line per revision
** is printed, otherwise the full comment is printed.  The "--limit N"
** and "--offset P" options limits the output to the first N changes
** after skipping P changes.
**
** In the -s form prints the status as <status> <revision>.  This is
** a quick status and does not check for up-to-date-ness of the file.
**
** In the -p form, there's an optional flag "-r|--revision REVISION".
** The specified version (or the latest checked out version) is printed
** to stdout.
**
** Options:
**   --brief|-b           display a brief (one line / revision) summary
**   --limit N            display the first N changes
**   --log|-l             select log mode (the default)
**   --offset P           skip P changes
**   -p                   select print mode
**   --revision|-r R      print the given revision (or ckout, if none is given)
**                        to stdout (only in print mode)
**   -s                   select status mode (print a status indicator for FILE)
**   --case-sensitive B   Enable or disable case-sensitive filenames.  B is a
**                        boolean: "yes", "no", "true", "false", etc.
**
** See also: artifact, descendants, info, leaves
*/
void finfo_cmd(void){
  capture_case_sensitive_option();
  db_must_be_within_tree();
  if (find_option("status","s",0)) {
    Stmt q;
    Blob line;
    Blob fname;
    int vid;

    if( g.argc!=3 ) usage("-s|--status FILENAME");
    vid = db_lget_int("checkout", 0);
    if( vid==0 ){
      fossil_panic("no checkout to finfo files in");
    }
    vfile_check_signature(vid, 1, 0);
    file_tree_name(g.argv[2], &fname, 1);
    db_prepare(&q,
        "SELECT pathname, deleted, rid, chnged, coalesce(origname!=pathname,0)"
        "  FROM vfile WHERE vfile.pathname=%B %s",
        &fname, filename_collation());
    blob_zero(&line);
    if ( db_step(&q)==SQLITE_ROW ) {
      Blob uuid;
      int isDeleted = db_column_int(&q, 1);
      int isNew = db_column_int(&q,2) == 0;
      int chnged = db_column_int(&q,3);
      int renamed = db_column_int(&q,4);

      blob_zero(&uuid);
      db_blob(&uuid,
           "SELECT uuid FROM blob, mlink, vfile WHERE "
           "blob.rid = mlink.mid AND mlink.fid = vfile.rid AND "
           "vfile.pathname=%B %s",
           &fname, filename_collation()
      );
      if( isNew ){
        blob_appendf(&line, "new");
      }else if( isDeleted ){
        blob_appendf(&line, "deleted");
      }else if( renamed ){
        blob_appendf(&line, "renamed");
      }else if( chnged ){
        blob_appendf(&line, "edited");
      }else{
        blob_appendf(&line, "unchanged");
      }
      blob_appendf(&line, " ");
      blob_appendf(&line, " %10.10s", blob_str(&uuid));
      blob_reset(&uuid);
    }else{
      blob_appendf(&line, "unknown 0000000000");
    }
    db_finalize(&q);
    fossil_print("%s\n", blob_str(&line));
    blob_reset(&fname);
    blob_reset(&line);
  }else if( find_option("print","p",0) ){
    Blob record;
    Blob fname;
    const char *zRevision = find_option("revision", "r", 1);

    file_tree_name(g.argv[2], &fname, 1);
    if( zRevision ){
      historical_version_of_file(zRevision, blob_str(&fname), &record, 0, 0, 0);
    }else{
      int rid = db_int(0, "SELECT rid FROM vfile WHERE pathname=%B %s",
                       &fname, filename_collation());
      if( rid==0 ){
        fossil_fatal("no history for file: %b", &fname);
      }
      content_get(rid, &record);
    }
    blob_write_to_file(&record, "-");
    blob_reset(&record);
    blob_reset(&fname);
  }else{
    Blob line;
    Stmt q;
    Blob fname;
    int rid;
    const char *zFilename;
    const char *zLimit;
    const char *zOffset;
    int iLimit, iOffset, iBrief;

    if( find_option("log","l",0) ){
      /* this is the default, no-op */
    }
    zLimit = find_option("limit",0,1);
    iLimit = zLimit ? atoi(zLimit) : -1;
    zOffset = find_option("offset",0,1);
    iOffset = zOffset ? atoi(zOffset) : 0;
    iBrief = (find_option("brief","b",0) == 0);
    if( g.argc!=3 ){
      usage("?-l|--log? ?-b|--brief? FILENAME");
    }
    file_tree_name(g.argv[2], &fname, 1);
    rid = db_int(0, "SELECT rid FROM vfile WHERE pathname=%B %s",
                 &fname, filename_collation());
    if( rid==0 ){
      fossil_fatal("no history for file: %b", &fname);
    }
    zFilename = blob_str(&fname);
    db_prepare(&q,
        "SELECT b.uuid, ci.uuid, date(event.mtime,'localtime'),"
        "       coalesce(event.ecomment, event.comment),"
        "       coalesce(event.euser, event.user)"
        "  FROM mlink, blob b, event, blob ci, filename"
        " WHERE filename.name=%Q %s"
        "   AND mlink.fnid=filename.fnid"
        "   AND b.rid=mlink.fid"
        "   AND event.objid=mlink.mid"
        "   AND event.objid=ci.rid"
        " ORDER BY event.mtime DESC LIMIT %d OFFSET %d",
        zFilename, filename_collation(), iLimit, iOffset
    );
    blob_zero(&line);
    if( iBrief ){
      fossil_print("History of %s\n", blob_str(&fname));
    }
    while( db_step(&q)==SQLITE_ROW ){
      const char *zFileUuid = db_column_text(&q, 0);
      const char *zCiUuid = db_column_text(&q,1);
      const char *zDate = db_column_text(&q, 2);
      const char *zCom = db_column_text(&q, 3);
      const char *zUser = db_column_text(&q, 4);
      char *zOut;
      if( iBrief ){
        fossil_print("%s ", zDate);
        zOut = sqlite3_mprintf("[%.10s] %s (user: %s, artifact: [%.10s])",
                               zCiUuid, zCom, zUser, zFileUuid);
        comment_print(zOut, 11, 79);
        sqlite3_free(zOut);
      }else{
        blob_reset(&line);
        blob_appendf(&line, "%.10s ", zCiUuid);
        blob_appendf(&line, "%.10s ", zDate);
        blob_appendf(&line, "%8.8s ", zUser);
        blob_appendf(&line,"%-40.40s\n", zCom );
        comment_print(blob_str(&line), 0, 79);
      }
    }
    db_finalize(&q);
    blob_reset(&fname);
  }
}


/*
** WEBPAGE: finfo
** URL: /finfo?name=FILENAME
**
** Show the change history for a single file. 
**
** Additional query parameters:
**
**    a=DATE     Only show changes after DATE
**    b=DATE     Only show changes before DATE
**    n=NUM      Show the first NUM changes only
**    brbg       Background color by branch name
**    ubg        Background color by user name
*/
void finfo_page(void){
  Stmt q;
  const char *zFilename;
  char zPrevDate[20];
  const char *zA;
  const char *zB;
  int n;
  Blob title;
  Blob sql;
  GraphContext *pGraph;
  int brBg = P("brbg")!=0;
  int uBg = P("ubg")!=0;

  login_check_credentials();
  if( !g.perm.Read ){ login_needed(); return; }
  style_header("File History");
  login_anonymous_available();

  zPrevDate[0] = 0;
  zFilename = PD("name","");
  blob_zero(&sql);
  blob_appendf(&sql, 
    "SELECT"
    " datetime(event.mtime,'localtime'),"            /* Date of change */
    " coalesce(event.ecomment, event.comment),"      /* Check-in comment */
    " coalesce(event.euser, event.user),"            /* User who made chng */
    " mlink.pid,"                                    /* Parent rid */
    " mlink.fid,"                                    /* File rid */
    " (SELECT uuid FROM blob WHERE rid=mlink.pid),"  /* Parent file uuid */
    " (SELECT uuid FROM blob WHERE rid=mlink.fid),"  /* Current file uuid */
    " (SELECT uuid FROM blob WHERE rid=mlink.mid),"  /* Check-in uuid */
    " event.bgcolor,"                                /* Background color */
    " (SELECT value FROM tagxref WHERE tagid=%d AND tagtype>0"
                                " AND tagxref.rid=mlink.mid)" /* Tags */
    "  FROM mlink, event"
    " WHERE mlink.fnid IN (SELECT fnid FROM filename WHERE name=%Q %s)"
    "   AND event.objid=mlink.mid",
    TAG_BRANCH,
    zFilename, filename_collation()
  );
  if( (zA = P("a"))!=0 ){
    blob_appendf(&sql, " AND event.mtime>=julianday('%q')", zA);
  }
  if( (zB = P("b"))!=0 ){
    blob_appendf(&sql, " AND event.mtime<=julianday('%q')", zB);
  }
  blob_appendf(&sql," ORDER BY event.mtime DESC /*sort*/");
  if( (n = atoi(PD("n","0")))>0 ){
    blob_appendf(&sql, " LIMIT %d", n);
  }
  db_prepare(&q, blob_str(&sql));
  blob_reset(&sql);
  blob_zero(&title);
  blob_appendf(&title, "History of ");
  hyperlinked_path(zFilename, &title, 0);
  @ <h2>%b(&title)</h2>
  blob_reset(&title);
  pGraph = graph_init();
  @ <div id="canvas" style="position:relative;width:1px;height:1px;"></div>
  @ <table id="timelineTable" class="timelineTable">
  while( db_step(&q)==SQLITE_ROW ){
    const char *zDate = db_column_text(&q, 0);
    const char *zCom = db_column_text(&q, 1);
    const char *zUser = db_column_text(&q, 2);
    int fpid = db_column_int(&q, 3);
    int frid = db_column_int(&q, 4);
    const char *zPUuid = db_column_text(&q, 5);
    const char *zUuid = db_column_text(&q, 6);
    const char *zCkin = db_column_text(&q,7);
    const char *zBgClr = db_column_text(&q, 8);
    const char *zBr = db_column_text(&q, 9);
    int gidx;
    char zTime[10];
    char zShort[20];
    char zShortCkin[20];
    if( zBr==0 ) zBr = "trunk";
    if( uBg ){
      zBgClr = hash_color(zUser);
    }else if( brBg || zBgClr==0 || zBgClr[0]==0 ){
      zBgClr = strcmp(zBr,"trunk")==0 ? "" : hash_color(zBr);
    }
    gidx = graph_add_row(pGraph, frid, fpid>0 ? 1 : 0, &fpid, zBr, zBgClr, 0);
    if( memcmp(zDate, zPrevDate, 10) ){
      sqlite3_snprintf(sizeof(zPrevDate), zPrevDate, "%.10s", zDate);
      @ <tr><td>
      @   <div class="divider">%s(zPrevDate)</div>
      @ </td></tr>
    }
    memcpy(zTime, &zDate[11], 5);
    zTime[5] = 0;
    @ <tr><td class="timelineTime">
    @ <a href="%s(g.zTop)/timeline?c=%t(zDate)">%s(zTime)</a></td>
    @ <td class="timelineGraph"><div id="m%d(gidx)"></div></td>
    if( zBgClr && zBgClr[0] ){
      @ <td class="timelineTableCell" style="background-color: %h(zBgClr);">
    }else{
      @ <td class="timelineTableCell">
    }
    sqlite3_snprintf(sizeof(zShort), zShort, "%.10s", zUuid);
    sqlite3_snprintf(sizeof(zShortCkin), zShortCkin, "%.10s", zCkin);
    if( zUuid ){
      if( g.perm.History ){
        @ <a href="%s(g.zTop)/artifact/%s(zUuid)">[%S(zUuid)]</a>
      }else{
        @ [%S(zUuid)]
      }
      @ part of check-in
    }else{
      @ <b>Deleted</b> by check-in
    }
    hyperlink_to_uuid(zShortCkin);
    @ %h(zCom) (user: 
    hyperlink_to_user(zUser, zDate, "");
    @ branch: %h(zBr))
    if( g.perm.History && zUuid ){
      const char *z = zFilename;
      if( fpid ){
        @ <a href="%s(g.zTop)/fdiff?v1=%s(zPUuid)&amp;v2=%s(zUuid)">[diff]</a>
      }
      @ <a href="%s(g.zTop)/annotate?checkin=%S(zCkin)&amp;filename=%h(z)">
      @ [annotate]</a>
    }
    @ </td></tr>
  }
  db_finalize(&q);
  if( pGraph ){
    graph_finish(pGraph, 0);
    if( pGraph->nErr ){
      graph_free(pGraph);
      pGraph = 0;
    }else{
      @ <tr><td></td><td>
      @ <div id="grbtm" style="width:%d(pGraph->mxRail*20+30)px;"></div>
      @     </td></tr>
    }
  }
  @ </table>
  timeline_output_graph_javascript(pGraph, 0);
  style_footer();
}

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
** This file contains code used to manage tags
*/
#include "config.h"
#include "tag.h"
#include <assert.h>

/*
** Propagate the tag given by tagid to the children of pid.
**
** This routine assumes that tagid is a tag that should be
** propagated and that the tag is already present in pid.
**
** If tagtype is 2 then the tag is being propagated from an
** ancestor node.  If tagtype is 0 it means a propagating tag is
** being blocked.
*/
static void tag_propagate(
  int pid,             /* Propagate the tag to children of this node */
  int tagid,           /* Tag to propagate */
  int tagType,         /* 2 for a propagating tag.  0 for an antitag */
  int origId,          /* Artifact of tag, when tagType==2 */
  const char *zValue,  /* Value of the tag.  Might be NULL */
  double mtime         /* Timestamp on the tag */
){
  PQueue queue;        /* Queue of check-ins to be tagged */
  Stmt s;              /* Query the children of :pid to which to propagate */
  Stmt ins;            /* INSERT INTO tagxref */
  Stmt eventupdate;    /* UPDATE event */

  assert( tagType==0 || tagType==2 );
  pqueue_init(&queue);
  pqueue_insert(&queue, pid, 0.0, 0);

  /* Query for children of :pid to which to propagate the tag.
  ** Three returns:  (1) rid of the child.  (2) timestamp of child.
  ** (3) True to propagate or false to block.
  */
  db_prepare(&s, 
     "SELECT cid, plink.mtime,"
     "       coalesce(srcid=0 AND tagxref.mtime<:mtime, %d) AS doit"
     "  FROM plink LEFT JOIN tagxref ON cid=rid AND tagid=%d"
     " WHERE pid=:pid AND isprim",
     tagType==2, tagid
  );
  db_bind_double(&s, ":mtime", mtime);

  if( tagType==2 ){
    /* Set the propagated tag marker on checkin :rid */
    db_prepare(&ins,
       "REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid)"
       "VALUES(%d,2,0,%d,%Q,:mtime,:rid)",
       tagid, origId, zValue
    );
    db_bind_double(&ins, ":mtime", mtime);
  }else{
    /* Remove all references to the tag from checkin :rid */
    zValue = 0;
    db_prepare(&ins,
       "DELETE FROM tagxref WHERE tagid=%d AND rid=:rid", tagid
    );
  }
  if( tagid==TAG_BGCOLOR ){
    db_prepare(&eventupdate,
      "UPDATE event SET bgcolor=%Q WHERE objid=:rid", zValue
    );
  }
  while( (pid = pqueue_extract(&queue, 0))!=0 ){
    db_bind_int(&s, ":pid", pid);
    while( db_step(&s)==SQLITE_ROW ){
      int doit = db_column_int(&s, 2);
      if( doit ){
        int cid = db_column_int(&s, 0);
        double mtime = db_column_double(&s, 1);
        pqueue_insert(&queue, cid, mtime, 0);
        db_bind_int(&ins, ":rid", cid);
        db_step(&ins);
        db_reset(&ins);
        if( tagid==TAG_BGCOLOR ){
          db_bind_int(&eventupdate, ":rid", cid);
          db_step(&eventupdate);
          db_reset(&eventupdate);
        }
        if( tagid==TAG_BRANCH ){
          leaf_eventually_check(cid);
        }
      }
    }
    db_reset(&s);
  }
  pqueue_clear(&queue);
  db_finalize(&ins);
  db_finalize(&s);
  if( tagid==TAG_BGCOLOR ){
    db_finalize(&eventupdate);
  }
}

/*
** Propagate all propagatable tags in pid to the children of pid.
*/
void tag_propagate_all(int pid){
  Stmt q;
  db_prepare(&q,
     "SELECT tagid, tagtype, mtime, value, origid FROM tagxref"
     " WHERE rid=%d",
     pid
  );
  while( db_step(&q)==SQLITE_ROW ){
    int tagid = db_column_int(&q, 0);
    int tagtype = db_column_int(&q, 1);
    double mtime = db_column_double(&q, 2);
    const char *zValue = db_column_text(&q, 3);
    int origid = db_column_int(&q, 4);
    if( tagtype==1 ) tagtype = 0;
    tag_propagate(pid, tagid, tagtype, origid, zValue, mtime);
  }
  db_finalize(&q);
}

/*
** Get a tagid for the given TAG.  Create a new tag if necessary
** if createFlag is 1.
*/
int tag_findid(const char *zTag, int createFlag){
  int id;
  id = db_int(0, "SELECT tagid FROM tag WHERE tagname=%Q", zTag);
  if( id==0 && createFlag ){
    db_multi_exec("INSERT INTO tag(tagname) VALUES(%Q)", zTag);
    id = db_last_insert_rowid();
  }
  return id;
}

/*
** Insert a tag into the database.
*/
int tag_insert(
  const char *zTag,        /* Name of the tag (w/o the "+" or "-" prefix */
  int tagtype,             /* 0:cancel  1:singleton  2:propagated */
  const char *zValue,      /* Value if the tag is really a property */
  int srcId,               /* Artifact that contains this tag */
  double mtime,            /* Timestamp.  Use default if <=0.0 */
  int rid                  /* Artifact to which the tag is to attached */
){
  Stmt s;
  const char *zCol;
  int tagid = tag_findid(zTag, 1);
  int rc;

  if( mtime<=0.0 ){
    mtime = db_double(0.0, "SELECT julianday('now')");
  }
  db_prepare(&s,
    "SELECT 1 FROM tagxref"
    " WHERE tagid=%d"
    "   AND rid=%d"
    "   AND mtime>=:mtime",
    tagid, rid
  );
  db_bind_double(&s, ":mtime", mtime);
  rc = db_step(&s);
  db_finalize(&s);
  if( rc==SQLITE_ROW ){
    /* Another entry that is more recent already exists.  Do nothing */
    return tagid;
  }
  db_prepare(&s, 
    "REPLACE INTO tagxref(tagid,tagtype,srcId,origid,value,mtime,rid)"
    " VALUES(%d,%d,%d,%d,%Q,:mtime,%d)",
    tagid, tagtype, srcId, rid, zValue, rid
  );
  db_bind_double(&s, ":mtime", mtime);
  db_step(&s);
  db_finalize(&s);
  if( tagid==TAG_BRANCH ) leaf_eventually_check(rid);
  if( tagtype==0 ){
    zValue = 0;
  }
  zCol = 0;
  switch( tagid ){
    case TAG_BGCOLOR: {
      zCol = "bgcolor";
      break;
    }
    case TAG_COMMENT: {
      zCol = "ecomment";
      break;
    }
    case TAG_USER: {
      zCol = "euser";
      break;
    }
    case TAG_PRIVATE: {
      db_multi_exec(
        "INSERT OR IGNORE INTO private(rid) VALUES(%d);",
        rid
      );
    }
  }
  if( zCol ){
    db_multi_exec("UPDATE event SET %s=%Q WHERE objid=%d", zCol, zValue, rid);
    if( tagid==TAG_COMMENT ){
      char *zCopy = mprintf("%s", zValue);
      wiki_extract_links(zCopy, rid, 0, mtime, 1, WIKI_INLINE);
      free(zCopy);
    }
  }
  if( tagid==TAG_DATE ){
    db_multi_exec("UPDATE event "
                  "   SET mtime=julianday(%Q),"
                  "       omtime=coalesce(omtime,mtime)"
                  " WHERE objid=%d",
                  zValue, rid);
  }
  if( tagtype==1 ) tagtype = 0;
  tag_propagate(rid, tagid, tagtype, rid, zValue, mtime);
  return tagid;
}


/*
** COMMAND: test-tag
** %fossil test-tag (+|*|-)TAGNAME ARTIFACT-ID ?VALUE?
**
** Add a tag or anti-tag to the rebuildable tables of the local repository.
** No tag artifact is created so the new tag is erased the next
** time the repository is rebuilt.  This routine is for testing
** use only.
*/
void testtag_cmd(void){
  const char *zTag;
  const char *zValue;
  int rid;
  int tagtype;
  db_must_be_within_tree();
  if( g.argc!=4 && g.argc!=5 ){
    usage("TAGNAME ARTIFACT-ID ?VALUE?");
  }
  zTag = g.argv[2];
  switch( zTag[0] ){
    case '+':  tagtype = 1;  break;
    case '*':  tagtype = 2;  break;
    case '-':  tagtype = 0;  break;
    default:   
      fossil_fatal("tag should begin with '+', '*', or '-'");
      return;
  }
  rid = name_to_rid(g.argv[3]);
  if( rid==0 ){
    fossil_fatal("no such object: %s", g.argv[3]);
  }
  g.markPrivate = content_is_private(rid);
  zValue = g.argc==5 ? g.argv[4] : 0;
  db_begin_transaction();
  tag_insert(zTag, tagtype, zValue, -1, 0.0, rid);
  db_end_transaction(0); 
}

/*
** Add a control record to the repository that either creates
** or cancels a tag.
*/
void tag_add_artifact(
  const char *zPrefix,        /* Prefix to prepend to tag name */
  const char *zTagname,       /* The tag to add or cancel */
  const char *zObjName,       /* Name of object attached to */
  const char *zValue,         /* Value for the tag.  Might be NULL */
  int tagtype,                /* 0:cancel 1:singleton 2:propagated */
  const char *zDateOvrd,      /* Override date string */
  const char *zUserOvrd       /* Override user name */
){
  int rid;
  int nrid;
  char *zDate;
  Blob uuid;
  Blob ctrl;
  Blob cksum;
  static const char zTagtype[] = { '-', '+', '*' };

  assert( tagtype>=0 && tagtype<=2 );
  user_select();
  blob_zero(&uuid);
  blob_append(&uuid, zObjName, -1);
  if( name_to_uuid(&uuid, 9, "*") ){
    fossil_fatal("%s", g.zErrMsg);
    return;
  }
  rid = name_to_rid(blob_str(&uuid));
  g.markPrivate = content_is_private(rid);
  blob_zero(&ctrl);

#if 0
  if( validate16(zTagname, strlen(zTagname)) ){
    fossil_fatal(
       "invalid tag name \"%s\" - might be confused with"
       " a hexadecimal artifact ID",
       zTagname
    );
  }
#endif
  zDate = date_in_standard_format(zDateOvrd ? zDateOvrd : "now");
  blob_appendf(&ctrl, "D %s\n", zDate);
  blob_appendf(&ctrl, "T %c%s%F %b",
               zTagtype[tagtype], zPrefix, zTagname, &uuid);
  if( tagtype>0 && zValue && zValue[0] ){
    blob_appendf(&ctrl, " %F\n", zValue);
  }else{
    blob_appendf(&ctrl, "\n");
  }
  blob_appendf(&ctrl, "U %F\n", zUserOvrd ? zUserOvrd : g.zLogin);
  md5sum_blob(&ctrl, &cksum);
  blob_appendf(&ctrl, "Z %b\n", &cksum);
  nrid = content_put(&ctrl);
  manifest_crosslink(nrid, &ctrl);
  assert( blob_is_reset(&ctrl) );
}

/*
** COMMAND: tag
** Usage: %fossil tag SUBCOMMAND ...
**
** Run various subcommands to control tags and properties
**
**     %fossil tag add ?--raw? ?--propagate? TAGNAME CHECK-IN ?VALUE?
**
**         Add a new tag or property to CHECK-IN. The tag will
**         be usable instead of a CHECK-IN in commands such as
**         update and merge.  If the --propagate flag is present,
**         the tag value propages to all descendants of CHECK-IN
**
**     %fossil tag cancel ?--raw? TAGNAME CHECK-IN
**
**         Remove the tag TAGNAME from CHECK-IN, and also remove
**         the propagation of the tag to any descendants.
**
**     %fossil tag find ?--raw? ?--type TYPE? TAGNAME
**
**         List all objects that use TAGNAME.  TYPE can be "ci" for
**         checkins or "e" for events.
**
**     %fossil tag list ?--raw? ?CHECK-IN?
**
**         List all tags, or if CHECK-IN is supplied, list
**         all tags and their values for CHECK-IN.
**
** The option --raw allows the manipulation of all types of tags
** used for various internal purposes in fossil. It also shows
** "cancel" tags for the "find" and "list" subcommands. You should
** not use this option to make changes unless you are sure what
** you are doing.
**
** If you need to use a tagname that might be confused with
** a hexadecimal baseline or artifact ID, you can explicitly
** disambiguate it by prefixing it with "tag:". For instance:
**
**   fossil update decaf
**
** will be taken as an artifact or baseline ID and fossil will
** probably complain that no such revision was found. However
**
**   fossil update tag:decaf
**
** will assume that "decaf" is a tag/branch name.
**
** only allow --date-override and --user-override in 
**   %fossil tag add --date-override 'YYYY-MMM-DD HH:MM:SS' \\
**                   --user-override user 
** in order to import history from other scm systems
*/
void tag_cmd(void){
  int n;
  int fRaw = find_option("raw","",0)!=0;
  int fPropagate = find_option("propagate","",0)!=0;
  const char *zPrefix = fRaw ? "" : "sym-";

  db_find_and_open_repository(0, 0);
  if( g.argc<3 ){
    goto tag_cmd_usage;
  }
  n = strlen(g.argv[2]);
  if( n==0 ){
    goto tag_cmd_usage;
  }

  if( strncmp(g.argv[2],"add",n)==0 ){
    char *zValue;
    const char *zDateOvrd = find_option("date-override",0,1);
    const char *zUserOvrd = find_option("user-override",0,1);
    if( g.argc!=5 && g.argc!=6 ){
      usage("add ?--raw? ?--propagate? TAGNAME CHECK-IN ?VALUE?");
    }
    zValue = g.argc==6 ? g.argv[5] : 0;
    db_begin_transaction();
    tag_add_artifact(zPrefix, g.argv[3], g.argv[4], zValue,
                     1+fPropagate,zDateOvrd,zUserOvrd);
    db_end_transaction(0);
  }else

  if( strncmp(g.argv[2],"branch",n)==0 ){
    fossil_fatal("the \"fossil tag branch\" command is discontinued\n"
                 "Use the \"fossil branch new\" command instead.");
  }else

  if( strncmp(g.argv[2],"cancel",n)==0 ){
    if( g.argc!=5 ){
      usage("cancel ?--raw? TAGNAME CHECK-IN");
    }
    db_begin_transaction();
    tag_add_artifact(zPrefix, g.argv[3], g.argv[4], 0, 0, 0, 0);
    db_end_transaction(0);
  }else

  if( strncmp(g.argv[2],"find",n)==0 ){
    Stmt q;
    const char *zType = find_option("type","t",1);
    if( zType==0 || zType[0]==0 ) zType = "*";
    if( g.argc!=4 ){
      usage("find ?--raw? TAGNAME");
    }
    if( fRaw ){
      db_prepare(&q,
        "SELECT blob.uuid FROM tagxref, blob"
        " WHERE tagid=(SELECT tagid FROM tag WHERE tagname=%Q)"
        "   AND tagxref.tagtype>0"
        "   AND blob.rid=tagxref.rid",
        g.argv[3]
      );
      while( db_step(&q)==SQLITE_ROW ){
        fossil_print("%s\n", db_column_text(&q, 0));
      }
      db_finalize(&q);
    }else{
      int tagid = db_int(0, "SELECT tagid FROM tag WHERE tagname='sym-%q'",
                         g.argv[3]);
      if( tagid>0 ){
        db_prepare(&q,
          "%s"
          "  AND event.type GLOB '%q'"
          "  AND blob.rid IN ("
                    " SELECT rid FROM tagxref"
                    "  WHERE tagtype>0 AND tagid=%d"
                    ")"
          " ORDER BY event.mtime DESC",
          timeline_query_for_tty(), zType, tagid
        );
        print_timeline(&q, 2000, 0);
        db_finalize(&q);
      }
    }
  }else

  if( strncmp(g.argv[2],"list",n)==0 ){
    Stmt q;
    if( g.argc==3 ){
      db_prepare(&q, 
        "SELECT tagname FROM tag"
        " WHERE EXISTS(SELECT 1 FROM tagxref"
        "               WHERE tagid=tag.tagid"
        "                 AND tagtype>0)"
        " ORDER BY tagname"
      );
      while( db_step(&q)==SQLITE_ROW ){
        const char *zName = db_column_text(&q, 0);
        if( fRaw ){
          fossil_print("%s\n", zName);
        }else if( strncmp(zName, "sym-", 4)==0 ){
          fossil_print("%s\n", &zName[4]);
        }
      }
      db_finalize(&q);
    }else if( g.argc==4 ){
      int rid = name_to_rid(g.argv[3]);
      db_prepare(&q,
        "SELECT tagname, value FROM tagxref, tag"
        " WHERE tagxref.rid=%d AND tagxref.tagid=tag.tagid"
        "   AND tagtype>%d"
        " ORDER BY tagname",
        rid,
        fRaw ? -1 : 0
      );
      while( db_step(&q)==SQLITE_ROW ){
        const char *zName = db_column_text(&q, 0);
        const char *zValue = db_column_text(&q, 1);
        if( fRaw==0 ){
          if( strncmp(zName, "sym-", 4)!=0 ) continue;
          zName += 4;
        }
        if( zValue && zValue[0] ){
          fossil_print("%s=%s\n", zName, zValue);
        }else{
          fossil_print("%s\n", zName);
        }
      }
      db_finalize(&q);
    }else{
      usage("tag list ?CHECK-IN?");
    }
  }else
  {
    goto tag_cmd_usage;
  }

  /* Cleanup */
  return;

tag_cmd_usage:
  usage("add|cancel|find|list ...");
}

/*
** WEBPAGE: /taglist
*/
void taglist_page(void){
  Stmt q;

  login_check_credentials();
  if( !g.perm.Read ){
    login_needed();
  }
  login_anonymous_available();
  style_header("Tags");
  style_submenu_element("Timeline", "Timeline", "tagtimeline");
  @ <h2>Non-propagating tags:</h2>
  db_prepare(&q,
    "SELECT substr(tagname,5)"
    "  FROM tag"
    " WHERE EXISTS(SELECT 1 FROM tagxref"
    "               WHERE tagid=tag.tagid"
    "                 AND tagtype=1)"
    " AND tagname GLOB 'sym-*'"
    " ORDER BY tagname"
  );
  @ <ul>
  while( db_step(&q)==SQLITE_ROW ){
    const char *zName = db_column_text(&q, 0);
    if( g.perm.History ){
      @ <li><a class="tagLink" href="%s(g.zTop)/timeline?t=%T(zName)">
      @ %h(zName)</a></li>
    }else{
      @ <li><span class="tagDsp">%h(zName)</span></li>
    }
  }
  @ </ul>
  db_finalize(&q);
  style_footer();
}

/*
** WEBPAGE: /tagtimeline
*/
void tagtimeline_page(void){
  Stmt q;

  login_check_credentials();
  if( !g.perm.Read ){ login_needed(); return; }

  style_header("Tagged Check-ins");
  style_submenu_element("List", "List", "taglist");
  login_anonymous_available();
  @ <h2>Check-ins with non-propagating tags:</h2>
  db_prepare(&q,
    "%s AND blob.rid IN (SELECT rid FROM tagxref"
    "                     WHERE tagtype=1 AND srcid>0"
    "                       AND tagid IN (SELECT tagid FROM tag "
    "                                      WHERE tagname GLOB 'sym-*'))"
    " ORDER BY event.mtime DESC",
    timeline_query_for_www()
  );
  www_print_timeline(&q, 0, 0, 0, 0);
  db_finalize(&q);
  @ <br />
  @ <script  type="text/JavaScript">
  @ function xin(id){
  @ }
  @ function xout(id){
  @ }
  @ </script>
  style_footer();
}

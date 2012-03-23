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
** This file contains code used to find decendants of a version
** or leaves of a version tree.
*/
#include "config.h"
#include "descendants.h"
#include <assert.h>


/*
** Create a temporary table named "leaves" if it does not
** already exist.  Load this table with the RID of all
** check-ins that are leaves which are decended from
** check-in iBase.
**
** A "leaf" is a check-in that has no children in the same branch.
** There is a separate permanent table LEAF that contains all leaves
** in the tree.  This routine is used to compute a subset of that
** table consisting of leaves that are descended from a single checkin.
**
** The closeMode flag determines behavior associated with the "closed"
** tag:
**
**    closeMode==0       Show all leaves regardless of the "closed" tag.
**
**    closeMode==1       Show only leaves without the "closed" tag.
**
**    closeMode==2       Show only leaves with the "closed" tag.
**
** The default behavior is to ignore closed leaves (closeMode==0).  To
** Show all leaves, use closeMode==1.  To show only closed leaves, use
** closeMode==2.
*/
void compute_leaves(int iBase, int closeMode){

  /* Create the LEAVES table if it does not already exist.  Make sure
  ** it is empty.
  */
  db_multi_exec(
    "CREATE TEMP TABLE IF NOT EXISTS leaves("
    "  rid INTEGER PRIMARY KEY"
    ");"
    "DELETE FROM leaves;"
  );

  if( iBase>0 ){
    Bag seen;     /* Descendants seen */
    Bag pending;  /* Unpropagated descendants */
    Stmt q1;      /* Query to find children of a check-in */
    Stmt isBr;    /* Query to check to see if a check-in starts a new branch */
    Stmt ins;     /* INSERT statement for a new record */

    /* Initialize the bags. */
    bag_init(&seen);
    bag_init(&pending);
    bag_insert(&pending, iBase);

    /* This query returns all non-branch-merge children of check-in :rid.
    **
    ** If a a child is a merge of a fork within the same branch, it is 
    ** returned.  Only merge children in different branches are excluded.
    */
    db_prepare(&q1,
      "SELECT cid FROM plink"
      " WHERE pid=:rid"
      "   AND (isprim"
      "        OR coalesce((SELECT value FROM tagxref"
                        "   WHERE tagid=%d AND rid=plink.pid), 'trunk')"
                 "=coalesce((SELECT value FROM tagxref"
                        "   WHERE tagid=%d AND rid=plink.cid), 'trunk'))",
      TAG_BRANCH, TAG_BRANCH
    );
  
    /* This query returns a single row if check-in :rid is the first
    ** check-in of a new branch.
    */
    db_prepare(&isBr, 
       "SELECT 1 FROM tagxref"
       " WHERE rid=:rid AND tagid=%d AND tagtype=2"
       "   AND srcid>0",
       TAG_BRANCH
    );
  
    /* This statement inserts check-in :rid into the LEAVES table.
    */
    db_prepare(&ins, "INSERT OR IGNORE INTO leaves VALUES(:rid)");
  
    while( bag_count(&pending) ){
      int rid = bag_first(&pending);
      int cnt = 0;
      bag_remove(&pending, rid);
      db_bind_int(&q1, ":rid", rid);
      while( db_step(&q1)==SQLITE_ROW ){
        int cid = db_column_int(&q1, 0);
        if( bag_insert(&seen, cid) ){
          bag_insert(&pending, cid);
        }
        db_bind_int(&isBr, ":rid", cid);
        if( db_step(&isBr)==SQLITE_DONE ){
          cnt++;
        }
        db_reset(&isBr);
      }
      db_reset(&q1);
      if( cnt==0 && !is_a_leaf(rid) ){
        cnt++;
      }
      if( cnt==0 ){
        db_bind_int(&ins, ":rid", rid);
        db_step(&ins);
        db_reset(&ins);
      }
    }
    db_finalize(&ins);
    db_finalize(&isBr);
    db_finalize(&q1);
    bag_clear(&pending);
    bag_clear(&seen);
  }
  if( closeMode==1 ){
    db_multi_exec(
      "DELETE FROM leaves WHERE rid IN"
      "  (SELECT leaves.rid FROM leaves, tagxref"
      "    WHERE tagxref.rid=leaves.rid "
      "      AND tagxref.tagid=%d"
      "      AND tagxref.tagtype>0)",
      TAG_CLOSED
    );
  }else if( closeMode==2 ){
    db_multi_exec(
      "DELETE FROM leaves WHERE rid NOT IN"
      "  (SELECT leaves.rid FROM leaves, tagxref"
      "    WHERE tagxref.rid=leaves.rid "
      "      AND tagxref.tagid=%d"
      "      AND tagxref.tagtype>0)",
      TAG_CLOSED
    );
  }
}

/*
** Load the record ID rid and up to N-1 closest ancestors into
** the "ok" table.
*/
void compute_ancestors(int rid, int N){
  Bag seen;
  PQueue queue;
  Stmt ins;
  Stmt q;
  bag_init(&seen);
  pqueue_init(&queue);
  bag_insert(&seen, rid);
  pqueue_insert(&queue, rid, 0.0, 0);
  db_prepare(&ins, "INSERT OR IGNORE INTO ok VALUES(:rid)");
  db_prepare(&q,
    "SELECT a.pid, b.mtime FROM plink a LEFT JOIN plink b ON b.cid=a.pid"
    " WHERE a.cid=:rid"
  );
  while( (N--)>0 && (rid = pqueue_extract(&queue, 0))!=0 ){
    db_bind_int(&ins, ":rid", rid);
    db_step(&ins);
    db_reset(&ins);
    db_bind_int(&q, ":rid", rid);
    while( db_step(&q)==SQLITE_ROW ){
      int pid = db_column_int(&q, 0);
      double mtime = db_column_double(&q, 1);
      if( bag_insert(&seen, pid) ){
        pqueue_insert(&queue, pid, -mtime, 0);
      }
    }
    db_reset(&q);
  }
  bag_clear(&seen);
  pqueue_clear(&queue);
  db_finalize(&ins);
  db_finalize(&q);
}

/*
** Compute up to N direct ancestors (merge ancestors do not count)
** for the check-in rid and put them in a table named "ancestor".
** Label each generation with consecutive integers going backwards
** in time such that rid has the smallest generation number and the oldest
** direct ancestor as the largest generation number.
*/
void compute_direct_ancestors(int rid, int N){
  Stmt ins;
  Stmt q;
  int gen = 0;
  db_multi_exec(
    "CREATE TEMP TABLE IF NOT EXISTS ancestor(rid INTEGER, generation INTEGER PRIMARY KEY);"
    "DELETE FROM ancestor;"
    "INSERT INTO ancestor VALUES(%d, 0);", rid
  );
  db_prepare(&ins, "INSERT INTO ancestor VALUES(:rid, :gen)");
  db_prepare(&q, 
    "SELECT pid FROM plink"
    " WHERE cid=:rid AND isprim"
  );
  while( (N--)>0 ){
    db_bind_int(&q, ":rid", rid);
    if( db_step(&q)!=SQLITE_ROW ) break;
    rid = db_column_int(&q, 0);
    db_reset(&q);
    gen++;
    db_bind_int(&ins, ":rid", rid);
    db_bind_int(&ins, ":gen", gen);
    db_step(&ins);
    db_reset(&ins);
  }
  db_finalize(&ins);
  db_finalize(&q);
}

/*
** Load the record ID rid and up to N-1 closest descendants into
** the "ok" table.
*/
void compute_descendants(int rid, int N){
  Bag seen;
  PQueue queue;
  Stmt ins;
  Stmt q;

  bag_init(&seen);
  pqueue_init(&queue);
  bag_insert(&seen, rid);
  pqueue_insert(&queue, rid, 0.0, 0);
  db_prepare(&ins, "INSERT OR IGNORE INTO ok VALUES(:rid)");
  db_prepare(&q, "SELECT cid, mtime FROM plink WHERE pid=:rid");
  while( (N--)>0 && (rid = pqueue_extract(&queue, 0))!=0 ){
    db_bind_int(&ins, ":rid", rid);
    db_step(&ins);
    db_reset(&ins);
    db_bind_int(&q, ":rid", rid);
    while( db_step(&q)==SQLITE_ROW ){
      int pid = db_column_int(&q, 0);
      double mtime = db_column_double(&q, 1);
      if( bag_insert(&seen, pid) ){
        pqueue_insert(&queue, pid, mtime, 0);
      }
    }
    db_reset(&q);
  }
  bag_clear(&seen);
  pqueue_clear(&queue);
  db_finalize(&ins);
  db_finalize(&q);
}

/*
** COMMAND: descendants*
**
** Usage: %fossil descendants ?BASELINE-ID? ?OPTIONS?
**
** Find all leaf descendants of the baseline specified or if the argument
** is omitted, of the baseline currently checked out.
**
** Options:
**    -R|--repository FILE       Extract info from repository FILE
**
** See also: finfo, info, leaves
*/
void descendants_cmd(void){
  Stmt q;
  int base;

  db_find_and_open_repository(0,0);
  if( g.argc==2 ){
    base = db_lget_int("checkout", 0);
  }else{
    base = name_to_typed_rid(g.argv[2], "ci");
  }
  if( base==0 ) return;
  compute_leaves(base, 0);
  db_prepare(&q,
    "%s"
    "   AND event.objid IN (SELECT rid FROM leaves)"
    " ORDER BY event.mtime DESC",
    timeline_query_for_tty()
  );
  print_timeline(&q, 20, 0);
  db_finalize(&q);
}

/*
** COMMAND: leaves*
**
** Usage: %fossil leaves ?OPTIONS?
**
** Find leaves of all branches.  By default show only open leaves.
** The --all flag causes all leaves (closed and open) to be shown.
** The --closed flag shows only closed leaves.
**
** The --recompute flag causes the content of the "leaf" table in the
** repository database to be recomputed.
**
** Options:
**   --all        show ALL leaves
**   --closed     show only closed leaves
**   --recompute  recompute the "leaf" table in the repository DB
**
** See also: descendants, finfo, info, branch
*/
void leaves_cmd(void){
  Stmt q;
  Blob sql;
  int showAll = find_option("all", 0, 0)!=0;
  int showClosed = find_option("closed", 0, 0)!=0;
  int recomputeFlag = find_option("recompute",0,0)!=0;

  db_find_and_open_repository(0,0);
  if( recomputeFlag ) leaf_rebuild();
  blob_zero(&sql);
  blob_append(&sql, timeline_query_for_tty(), -1);
  blob_appendf(&sql, " AND blob.rid IN leaf");
  if( showClosed ){
    blob_appendf(&sql," AND %z", leaf_is_closed_sql("blob.rid"));
  }else if( !showAll ){
    blob_appendf(&sql," AND NOT %z", leaf_is_closed_sql("blob.rid"));
  }
  db_prepare(&q, "%s ORDER BY event.mtime DESC", blob_str(&sql));
  blob_reset(&sql);
  print_timeline(&q, 2000, 0);
  db_finalize(&q);
}

/*
** WEBPAGE:  leaves
**
** Find leaves of all branches.
*/
void leaves_page(void){
  Blob sql;
  Stmt q;
  int showAll = P("all")!=0;
  int showClosed = P("closed")!=0;

  login_check_credentials();
  if( !g.perm.Read ){ login_needed(); return; }

  if( !showAll ){
    style_submenu_element("All", "All", "leaves?all");
  }
  if( !showClosed ){
    style_submenu_element("Closed", "Closed", "leaves?closed");
  }
  if( showClosed || showAll ){
    style_submenu_element("Open", "Open", "leaves");
  }
  style_header("Leaves");
  login_anonymous_available();
  style_sidebox_begin("Nomenclature:", "33%");
  @ <ol>
  @ <li> A <div class="sideboxDescribed">leaf</div>
  @ is a check-in with no descendants in the same branch.</li>
  @ <li> An <div class="sideboxDescribed">open leaf</div>
  @ is a leaf that does not have a "closed" tag
  @ and is thus assumed to still be in use.</li>
  @ <li> A <div class="sideboxDescribed">closed leaf</div>
  @ has a "closed" tag and is thus assumed to
  @ be historical and no longer in active use.</li>
  @ </ol>
  style_sidebox_end();

  if( showAll ){
    @ <h1>All leaves, both open and closed:</h1>
  }else if( showClosed ){
    @ <h1>Closed leaves:</h1>
  }else{
    @ <h1>Open leaves:</h1>
  }
  blob_zero(&sql);
  blob_append(&sql, timeline_query_for_www(), -1);
  blob_appendf(&sql, " AND blob.rid IN leaf");
  if( showClosed ){
    blob_appendf(&sql," AND %z", leaf_is_closed_sql("blob.rid"));
  }else if( !showAll ){
    blob_appendf(&sql," AND NOT %z", leaf_is_closed_sql("blob.rid"));
  }
  db_prepare(&q, "%s ORDER BY event.mtime DESC", blob_str(&sql));
  blob_reset(&sql);
  www_print_timeline(&q, TIMELINE_LEAFONLY, 0, 0, 0);
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

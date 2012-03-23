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
** This file contains code to implement the file transfer protocol.
*/
#include "config.h"
#include "xfer.h"

/*
** This structure holds information about the current state of either
** a client or a server that is participating in xfer.
*/
typedef struct Xfer Xfer;
struct Xfer {
  Blob *pIn;          /* Input text from the other side */
  Blob *pOut;         /* Compose our reply here */
  Blob line;          /* The current line of input */
  Blob aToken[6];     /* Tokenized version of line */
  Blob err;           /* Error message text */
  int nToken;         /* Number of tokens in line */
  int nIGotSent;      /* Number of "igot" cards sent */
  int nGimmeSent;     /* Number of gimme cards sent */
  int nFileSent;      /* Number of files sent */
  int nDeltaSent;     /* Number of deltas sent */
  int nFileRcvd;      /* Number of files received */
  int nDeltaRcvd;     /* Number of deltas received */
  int nDanglingFile;  /* Number of dangling deltas received */
  int mxSend;         /* Stop sending "file" with pOut reaches this size */
  u8 syncPrivate;     /* True to enable syncing private content */
  u8 nextIsPrivate;   /* If true, next "file" received is a private */
};


/*
** The input blob contains a UUID.  Convert it into a record ID.
** Create a phantom record if no prior record exists and
** phantomize is true.
**
** Compare to uuid_to_rid().  This routine takes a blob argument
** and does less error checking.
*/
static int rid_from_uuid(Blob *pUuid, int phantomize, int isPrivate){
  static Stmt q;
  int rid;

  db_static_prepare(&q, "SELECT rid FROM blob WHERE uuid=:uuid");
  db_bind_str(&q, ":uuid", pUuid);
  if( db_step(&q)==SQLITE_ROW ){
    rid = db_column_int(&q, 0);
  }else{
    rid = 0;
  }
  db_reset(&q);
  if( rid==0 && phantomize ){
    rid = content_new(blob_str(pUuid), isPrivate);
  }
  return rid;
}

/*
** Remember that the other side of the connection already has a copy
** of the file rid.
*/
static void remote_has(int rid){
  if( rid ){
    static Stmt q;
    db_static_prepare(&q, "INSERT OR IGNORE INTO onremote VALUES(:r)");
    db_bind_int(&q, ":r", rid);
    db_step(&q);
    db_reset(&q);
  }
}

/*
** The aToken[0..nToken-1] blob array is a parse of a "file" line 
** message.  This routine finishes parsing that message and does
** a record insert of the file.
**
** The file line is in one of the following two forms:
**
**      file UUID SIZE \n CONTENT
**      file UUID DELTASRC SIZE \n CONTENT
**
** The content is SIZE bytes immediately following the newline.
** If DELTASRC exists, then the CONTENT is a delta against the
** content of DELTASRC.
**
** If any error occurs, write a message into pErr which has already
** be initialized to an empty string.
**
** Any artifact successfully received by this routine is considered to
** be public and is therefore removed from the "private" table.
*/
static void xfer_accept_file(Xfer *pXfer, int cloneFlag){
  int n;
  int rid;
  int srcid = 0;
  Blob content, hash;
  int isPriv;
  
  isPriv = pXfer->nextIsPrivate;
  pXfer->nextIsPrivate = 0;
  if( pXfer->nToken<3 
   || pXfer->nToken>4
   || !blob_is_uuid(&pXfer->aToken[1])
   || !blob_is_int(&pXfer->aToken[pXfer->nToken-1], &n)
   || n<0
   || (pXfer->nToken==4 && !blob_is_uuid(&pXfer->aToken[2]))
  ){
    blob_appendf(&pXfer->err, "malformed file line");
    return;
  }
  blob_zero(&content);
  blob_zero(&hash);
  blob_extract(pXfer->pIn, n, &content);
  if( !cloneFlag && uuid_is_shunned(blob_str(&pXfer->aToken[1])) ){
    /* Ignore files that have been shunned */
    blob_reset(&content);
    return;
  }
  if( isPriv && !g.perm.Private ){
    /* Do not accept private files if not authorized */
    blob_reset(&content);
    return;
  }
  if( cloneFlag ){
    if( pXfer->nToken==4 ){
      srcid = rid_from_uuid(&pXfer->aToken[2], 1, isPriv);
      pXfer->nDeltaRcvd++;
    }else{
      srcid = 0;
      pXfer->nFileRcvd++;
    }
    rid = content_put_ex(&content, blob_str(&pXfer->aToken[1]), srcid,
                         0, isPriv);
    remote_has(rid);
    blob_reset(&content);
    return;
  }
  if( pXfer->nToken==4 ){
    Blob src, next;
    srcid = rid_from_uuid(&pXfer->aToken[2], 1, isPriv);
    if( content_get(srcid, &src)==0 ){
      rid = content_put_ex(&content, blob_str(&pXfer->aToken[1]), srcid,
                           0, isPriv);
      pXfer->nDanglingFile++;
      db_multi_exec("DELETE FROM phantom WHERE rid=%d", rid);
      if( !isPriv ) content_make_public(rid);
      blob_reset(&src);
      blob_reset(&content);
      return;
    }
    pXfer->nDeltaRcvd++;
    blob_delta_apply(&src, &content, &next);
    blob_reset(&src);
    blob_reset(&content);
    content = next;
  }else{
    pXfer->nFileRcvd++;
  }
  sha1sum_blob(&content, &hash);
  if( !blob_eq_str(&pXfer->aToken[1], blob_str(&hash), -1) ){
    blob_appendf(&pXfer->err, "content does not match sha1 hash");
  }
  rid = content_put_ex(&content, blob_str(&hash), 0, 0, isPriv);
  blob_reset(&hash);
  if( rid==0 ){
    blob_appendf(&pXfer->err, "%s", g.zErrMsg);
    blob_reset(&content);
  }else{
    if( !isPriv ) content_make_public(rid);
    manifest_crosslink(rid, &content);
  }
  assert( blob_is_reset(&content) );
  remote_has(rid);
}

/*
** The aToken[0..nToken-1] blob array is a parse of a "cfile" line 
** message.  This routine finishes parsing that message and does
** a record insert of the file.  The difference between "file" and
** "cfile" is that with "cfile" the content is already compressed.
**
** The file line is in one of the following two forms:
**
**      cfile UUID USIZE CSIZE \n CONTENT
**      cfile UUID DELTASRC USIZE CSIZE \n CONTENT
**
** The content is CSIZE bytes immediately following the newline.
** If DELTASRC exists, then the CONTENT is a delta against the
** content of DELTASRC.
**
** The original size of the UUID artifact is USIZE.
**
** If any error occurs, write a message into pErr which has already
** be initialized to an empty string.
**
** Any artifact successfully received by this routine is considered to
** be public and is therefore removed from the "private" table.
*/
static void xfer_accept_compressed_file(Xfer *pXfer){
  int szC;   /* CSIZE */
  int szU;   /* USIZE */
  int rid;
  int srcid = 0;
  Blob content;
  int isPriv;
  
  isPriv = pXfer->nextIsPrivate;
  pXfer->nextIsPrivate = 0;
  if( pXfer->nToken<4 
   || pXfer->nToken>5
   || !blob_is_uuid(&pXfer->aToken[1])
   || !blob_is_int(&pXfer->aToken[pXfer->nToken-2], &szU)
   || !blob_is_int(&pXfer->aToken[pXfer->nToken-1], &szC)
   || szC<0 || szU<0
   || (pXfer->nToken==5 && !blob_is_uuid(&pXfer->aToken[2]))
  ){
    blob_appendf(&pXfer->err, "malformed cfile line");
    return;
  }
  if( isPriv && !g.perm.Private ){
    /* Do not accept private files if not authorized */
    return;
  }
  blob_zero(&content);
  blob_extract(pXfer->pIn, szC, &content);
  if( uuid_is_shunned(blob_str(&pXfer->aToken[1])) ){
    /* Ignore files that have been shunned */
    blob_reset(&content);
    return;
  }
  if( pXfer->nToken==5 ){
    srcid = rid_from_uuid(&pXfer->aToken[2], 1, isPriv);
    pXfer->nDeltaRcvd++;
  }else{
    srcid = 0;
    pXfer->nFileRcvd++;
  }
  rid = content_put_ex(&content, blob_str(&pXfer->aToken[1]), srcid,
                       szC, isPriv);
  remote_has(rid);
  blob_reset(&content);
}

/*
** Try to send a file as a delta against its parent.
** If successful, return the number of bytes in the delta.
** If we cannot generate an appropriate delta, then send
** nothing and return zero.
**
** Never send a delta against a private artifact.
*/
static int send_delta_parent(
  Xfer *pXfer,            /* The transfer context */
  int rid,                /* record id of the file to send */
  int isPrivate,          /* True if rid is a private artifact */
  Blob *pContent,         /* The content of the file to send */
  Blob *pUuid             /* The UUID of the file to send */
){
  static const char *azQuery[] = {
    "SELECT pid FROM plink x"
    " WHERE cid=%d"
    "   AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=pid)"
    "   AND NOT EXISTS(SELECT 1 FROM plink y"
                      " WHERE y.pid=x.cid AND y.cid=x.pid)",

    "SELECT pid FROM mlink x"
    " WHERE fid=%d"
    "   AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=pid)"
    "   AND NOT EXISTS(SELECT 1 FROM mlink y"
                     "  WHERE y.pid=x.fid AND y.fid=x.pid)"
  };
  int i;
  Blob src, delta;
  int size = 0;
  int srcId = 0;

  for(i=0; srcId==0 && i<count(azQuery); i++){
    srcId = db_int(0, azQuery[i], rid);
  }
  if( srcId>0
   && (pXfer->syncPrivate || !content_is_private(srcId))
   && content_get(srcId, &src)
  ){
    char *zUuid = db_text(0, "SELECT uuid FROM blob WHERE rid=%d", srcId);
    blob_delta_create(&src, pContent, &delta);
    size = blob_size(&delta);
    if( size>=blob_size(pContent)-50 ){
      size = 0;
    }else if( uuid_is_shunned(zUuid) ){
      size = 0;
    }else{
      if( isPrivate ) blob_append(pXfer->pOut, "private\n", -1);
      blob_appendf(pXfer->pOut, "file %b %s %d\n", pUuid, zUuid, size);
      blob_append(pXfer->pOut, blob_buffer(&delta), size);
    }
    blob_reset(&delta);
    free(zUuid);
    blob_reset(&src);
  }
  return size;
}

/*
** Try to send a file as a native delta.  
** If successful, return the number of bytes in the delta.
** If we cannot generate an appropriate delta, then send
** nothing and return zero.
**
** Never send a delta against a private artifact.
*/
static int send_delta_native(
  Xfer *pXfer,            /* The transfer context */
  int rid,                /* record id of the file to send */
  int isPrivate,          /* True if rid is a private artifact */
  Blob *pUuid             /* The UUID of the file to send */
){
  Blob src, delta;
  int size = 0;
  int srcId;

  srcId = db_int(0, "SELECT srcid FROM delta WHERE rid=%d", rid);
  if( srcId>0
   && (pXfer->syncPrivate || !content_is_private(srcId))
  ){
    blob_zero(&src);
    db_blob(&src, "SELECT uuid FROM blob WHERE rid=%d", srcId);
    if( uuid_is_shunned(blob_str(&src)) ){
      blob_reset(&src);
      return 0;
    }
    blob_zero(&delta);
    db_blob(&delta, "SELECT content FROM blob WHERE rid=%d", rid);
    blob_uncompress(&delta, &delta);
    if( isPrivate ) blob_append(pXfer->pOut, "private\n", -1);
    blob_appendf(pXfer->pOut, "file %b %b %d\n",
                pUuid, &src, blob_size(&delta));
    blob_append(pXfer->pOut, blob_buffer(&delta), blob_size(&delta));
    size = blob_size(&delta);
    blob_reset(&delta);
    blob_reset(&src);
  }else{
    size = 0;
  }
  return size;
}

/*
** Send the file identified by rid.
**
** The pUuid can be NULL in which case the correct UUID is computed
** from the rid.
**
** Try to send the file as a native delta if nativeDelta is true, or
** as a parent delta if nativeDelta is false.
**
** It should never be the case that rid is a private artifact.  But
** as a precaution, this routine does check on rid and if it is private
** this routine becomes a no-op.
*/
static void send_file(Xfer *pXfer, int rid, Blob *pUuid, int nativeDelta){
  Blob content, uuid;
  int size = 0;
  int isPriv = content_is_private(rid);

  if( pXfer->syncPrivate==0 && isPriv ) return;
  if( db_exists("SELECT 1 FROM onremote WHERE rid=%d", rid) ){
     return;
  }
  blob_zero(&uuid);
  db_blob(&uuid, "SELECT uuid FROM blob WHERE rid=%d AND size>=0", rid);
  if( blob_size(&uuid)==0 ){
    return;
  }
  if( pUuid ){
    if( blob_compare(pUuid, &uuid)!=0 ){
      blob_reset(&uuid);
      return;
    }
  }else{
    pUuid = &uuid;
  }
  if( uuid_is_shunned(blob_str(pUuid)) ){
    blob_reset(&uuid);
    return;
  }
  if( pXfer->mxSend<=blob_size(pXfer->pOut) ){
    const char *zFormat = isPriv ? "igot %b 1\n" : "igot %b\n";
    blob_appendf(pXfer->pOut, zFormat, pUuid);
    pXfer->nIGotSent++;
    blob_reset(&uuid);
    return;
  }
  if( nativeDelta ){
    size = send_delta_native(pXfer, rid, isPriv, pUuid);
    if( size ){
      pXfer->nDeltaSent++;
    }
  }
  if( size==0 ){
    content_get(rid, &content);

    if( !nativeDelta && blob_size(&content)>100 ){
      size = send_delta_parent(pXfer, rid, isPriv, &content, pUuid);
    }
    if( size==0 ){
      int size = blob_size(&content);
      if( isPriv ) blob_append(pXfer->pOut, "private\n", -1);
      blob_appendf(pXfer->pOut, "file %b %d\n", pUuid, size);
      blob_append(pXfer->pOut, blob_buffer(&content), size);
      pXfer->nFileSent++;
    }else{
      pXfer->nDeltaSent++;
    }
    blob_reset(&content);
  }
  remote_has(rid);
  blob_reset(&uuid);
#if 0
  if( blob_buffer(pXfer->pOut)[blob_size(pXfer->pOut)-1]!='\n' ){
    blob_appendf(pXfer->pOut, "\n", 1);
  }
#endif
}

/*
** Send the file identified by rid as a compressed artifact.  Basically,
** send the content exactly as it appears in the BLOB table using 
** a "cfile" card.
*/
static void send_compressed_file(Xfer *pXfer, int rid){
  const char *zContent;
  const char *zUuid;
  const char *zDelta;
  int szU;
  int szC;
  int rc;
  int isPrivate;
  int srcIsPrivate;
  static Stmt q1;
  Blob fullContent;

  isPrivate = content_is_private(rid);
  if( isPrivate && pXfer->syncPrivate==0 ) return;
  db_static_prepare(&q1,
    "SELECT uuid, size, content, delta.srcid IN private,"
         "  (SELECT uuid FROM blob WHERE rid=delta.srcid)"
    " FROM blob LEFT JOIN delta ON (blob.rid=delta.rid)"
    " WHERE blob.rid=:rid"
    "   AND blob.size>=0"
    "   AND NOT EXISTS(SELECT 1 FROM shun WHERE shun.uuid=blob.uuid)"
  );
  db_bind_int(&q1, ":rid", rid);
  rc = db_step(&q1);
  if( rc==SQLITE_ROW ){
    zUuid = db_column_text(&q1, 0);
    szU = db_column_int(&q1, 1);
    szC = db_column_bytes(&q1, 2);
    zContent = db_column_raw(&q1, 2);
    srcIsPrivate = db_column_int(&q1, 3);
    zDelta = db_column_text(&q1, 4);
    if( isPrivate ) blob_append(pXfer->pOut, "private\n", -1);
    blob_appendf(pXfer->pOut, "cfile %s ", zUuid);
    if( !isPrivate && srcIsPrivate ){
      content_get(rid, &fullContent);
      szU = blob_size(&fullContent);
      blob_compress(&fullContent, &fullContent);
      szC = blob_size(&fullContent);
      zContent = blob_buffer(&fullContent);
      zDelta = 0;
    }
    if( zDelta ){
      blob_appendf(pXfer->pOut, "%s ", zDelta);
      pXfer->nDeltaSent++;
    }else{
      pXfer->nFileSent++;
    }
    blob_appendf(pXfer->pOut, "%d %d\n", szU, szC);
    blob_append(pXfer->pOut, zContent, szC);
    if( blob_buffer(pXfer->pOut)[blob_size(pXfer->pOut)-1]!='\n' ){
      blob_appendf(pXfer->pOut, "\n", 1);
    }
    if( !isPrivate && srcIsPrivate ){
      blob_reset(&fullContent);
    }
  }
  db_reset(&q1);
}

/*
** Send a gimme message for every phantom.
**
** Except: do not request shunned artifacts.  And do not request
** private artifacts if we are not doing a private transfer.
*/
static void request_phantoms(Xfer *pXfer, int maxReq){
  Stmt q;
  db_prepare(&q, 
    "SELECT uuid FROM phantom JOIN blob USING(rid)"
    " WHERE NOT EXISTS(SELECT 1 FROM shun WHERE uuid=blob.uuid) %s",
    (pXfer->syncPrivate ? "" :
         "   AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)")
  );
  while( db_step(&q)==SQLITE_ROW && maxReq-- > 0 ){
    const char *zUuid = db_column_text(&q, 0);
    blob_appendf(pXfer->pOut, "gimme %s\n", zUuid);
    pXfer->nGimmeSent++;
  }
  db_finalize(&q);
}

/*
** Compute an SHA1 hash on the tail of pMsg.  Verify that it matches the
** the hash given in pHash.  Return non-zero for an error and 0 on success.
*/
static int check_tail_hash(Blob *pHash, Blob *pMsg){
  Blob tail;
  Blob h2;
  int rc;
  blob_tail(pMsg, &tail);
  sha1sum_blob(&tail, &h2);
  rc = blob_compare(pHash, &h2);
  blob_reset(&h2);
  blob_reset(&tail);
  return rc;
}

/*
** Check the signature on an application/x-fossil payload received by
** the HTTP server.  The signature is a line of the following form:
**
**        login LOGIN NONCE SIGNATURE
**
** The NONCE is the SHA1 hash of the remainder of the input.  
** SIGNATURE is the SHA1 checksum of the NONCE concatenated 
** with the users password.
**
** The parameters to this routine are ephermeral blobs holding the
** LOGIN, NONCE and SIGNATURE.
**
** This routine attempts to locate the user and verify the signature.
** If everything checks out, the USER.CAP column for the USER table
** is consulted to set privileges in the global g variable.
**
** If anything fails to check out, no changes are made to privileges.
**
** Signature generation on the client side is handled by the 
** http_exchange() routine.
**
** Return non-zero for a login failure and zero for success.
*/
int check_login(Blob *pLogin, Blob *pNonce, Blob *pSig){
  Stmt q;
  int rc = -1;
  char *zLogin = blob_terminate(pLogin);
  defossilize(zLogin);

  if( fossil_strcmp(zLogin, "nobody")==0 || fossil_strcmp(zLogin,"anonymous")==0 ){
    return 0;   /* Anybody is allowed to sync as "nobody" or "anonymous" */
  }
  if( fossil_strcmp(P("REMOTE_USER"), zLogin)==0 ){
    return 0;   /* Accept Basic Authorization */
  }
  db_prepare(&q,
     "SELECT pw, cap, uid FROM user"
     " WHERE login=%Q"
     "   AND login NOT IN ('anonymous','nobody','developer','reader')"
     "   AND length(pw)>0",
     zLogin
  );
  if( db_step(&q)==SQLITE_ROW ){
    int szPw;
    Blob pw, combined, hash;
    blob_zero(&pw);
    db_ephemeral_blob(&q, 0, &pw);
    szPw = blob_size(&pw);
    blob_zero(&combined);
    blob_copy(&combined, pNonce);
    blob_append(&combined, blob_buffer(&pw), szPw);
    sha1sum_blob(&combined, &hash);
    assert( blob_size(&hash)==40 );
    rc = blob_constant_time_cmp(&hash, pSig);
    blob_reset(&hash);
    blob_reset(&combined);
    if( rc!=0 && szPw!=40 ){
      /* If this server stores cleartext passwords and the password did not
      ** match, then perhaps the client is sending SHA1 passwords.  Try
      ** again with the SHA1 password.
      */
      const char *zPw = db_column_text(&q, 0);
      char *zSecret = sha1_shared_secret(zPw, blob_str(pLogin), 0);
      blob_zero(&combined);
      blob_copy(&combined, pNonce);
      blob_append(&combined, zSecret, -1);
      free(zSecret);
      sha1sum_blob(&combined, &hash);
      rc = blob_constant_time_cmp(&hash, pSig);
      blob_reset(&hash);
      blob_reset(&combined);
    }
    if( rc==0 ){
      const char *zCap;
      zCap = db_column_text(&q, 1);
      login_set_capabilities(zCap, 0);
      g.userUid = db_column_int(&q, 2);
      g.zLogin = mprintf("%b", pLogin);
      g.zNonce = mprintf("%b", pNonce);
    }
  }
  db_finalize(&q);
  return rc;
}

/*
** Send the content of all files in the unsent table.
**
** This is really just an optimization.  If you clear the
** unsent table, all the right files will still get transferred.
** It just might require an extra round trip or two.
*/
static void send_unsent(Xfer *pXfer){
  Stmt q;
  db_prepare(&q, "SELECT rid FROM unsent EXCEPT SELECT rid FROM private");
  while( db_step(&q)==SQLITE_ROW ){
    int rid = db_column_int(&q, 0);
    send_file(pXfer, rid, 0, 0);
  }
  db_finalize(&q);
  db_multi_exec("DELETE FROM unsent");
}

/*
** Check to see if the number of unclustered entries is greater than
** 100 and if it is, form a new cluster.  Unclustered phantoms do not
** count toward the 100 total.  And phantoms are never added to a new
** cluster.
*/
void create_cluster(void){
  Blob cluster, cksum;
  Blob deleteWhere;
  Stmt q;
  int nUncl;
  int nRow = 0;
  int rid;

#if 0
  /* We should not ever get any private artifacts in the unclustered table.
  ** But if we do (because of a bug) now is a good time to delete them. */
  db_multi_exec(
    "DELETE FROM unclustered WHERE rid IN (SELECT rid FROM private)"
  );
#endif

  nUncl = db_int(0, "SELECT count(*) FROM unclustered /*scan*/"
                    " WHERE NOT EXISTS(SELECT 1 FROM phantom"
                                      " WHERE rid=unclustered.rid)");
  if( nUncl>=100 ){
    blob_zero(&cluster);
    blob_zero(&deleteWhere);
    db_prepare(&q, "SELECT uuid FROM unclustered, blob"
                   " WHERE NOT EXISTS(SELECT 1 FROM phantom"
                   "                   WHERE rid=unclustered.rid)"
                   "   AND unclustered.rid=blob.rid"
                   "   AND NOT EXISTS(SELECT 1 FROM shun WHERE uuid=blob.uuid)"
                   " ORDER BY 1");
    while( db_step(&q)==SQLITE_ROW ){
      blob_appendf(&cluster, "M %s\n", db_column_text(&q, 0));
      nRow++;
      if( nRow>=800 && nUncl>nRow+100 ){
        md5sum_blob(&cluster, &cksum);
        blob_appendf(&cluster, "Z %b\n", &cksum);
        blob_reset(&cksum);
        rid = content_put(&cluster);
        blob_reset(&cluster);
        nUncl -= nRow;
        nRow = 0;
        blob_appendf(&deleteWhere, ",%d", rid);
      }
    }
    db_finalize(&q);
    db_multi_exec(
      "DELETE FROM unclustered WHERE rid NOT IN (0 %s)", 
      blob_str(&deleteWhere)
    );
    blob_reset(&deleteWhere);
    if( nRow>0 ){
      md5sum_blob(&cluster, &cksum);
      blob_appendf(&cluster, "Z %b\n", &cksum);
      blob_reset(&cksum);
      content_put(&cluster);
      blob_reset(&cluster);
    }
  }
}

/*
** Send igot messages for every private artifact
*/
static int send_private(Xfer *pXfer){
  int cnt = 0;
  Stmt q;
  if( pXfer->syncPrivate ){
    db_prepare(&q, "SELECT uuid FROM private JOIN blob USING(rid)");
    while( db_step(&q)==SQLITE_ROW ){
      blob_appendf(pXfer->pOut, "igot %s 1\n", db_column_text(&q,0));
      cnt++;
    }
    db_finalize(&q);
  }
  return cnt;
}

/*
** Send an igot message for every entry in unclustered table.
** Return the number of cards sent.
*/
static int send_unclustered(Xfer *pXfer){
  Stmt q;
  int cnt = 0;
  db_prepare(&q, 
    "SELECT uuid FROM unclustered JOIN blob USING(rid)"
    " WHERE NOT EXISTS(SELECT 1 FROM shun WHERE uuid=blob.uuid)"
    "   AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=blob.rid)"
    "   AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)"
  );
  while( db_step(&q)==SQLITE_ROW ){
    blob_appendf(pXfer->pOut, "igot %s\n", db_column_text(&q, 0));
    cnt++;
  }
  db_finalize(&q);
  return cnt;
}

/*
** Send an igot message for every artifact.
*/
static void send_all(Xfer *pXfer){
  Stmt q;
  db_prepare(&q, 
    "SELECT uuid FROM blob "
    " WHERE NOT EXISTS(SELECT 1 FROM shun WHERE uuid=blob.uuid)"
    "   AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)"
    "   AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=blob.rid)"
  );
  while( db_step(&q)==SQLITE_ROW ){
    blob_appendf(pXfer->pOut, "igot %s\n", db_column_text(&q, 0));
  }
  db_finalize(&q);
}

/*
** Send a single old-style config card for configuration item zName.
**
** This routine and the functionality it implements is scheduled for
** removal on 2012-05-01.
*/
static void send_legacy_config_card(Xfer *pXfer, const char *zName){
  if( zName[0]!='@' ){
    Blob val;
    blob_zero(&val);
    db_blob(&val, "SELECT value FROM config WHERE name=%Q", zName);
    if( blob_size(&val)>0 ){
      blob_appendf(pXfer->pOut, "config %s %d\n", zName, blob_size(&val));
      blob_append(pXfer->pOut, blob_buffer(&val), blob_size(&val));
      blob_reset(&val);
      blob_append(pXfer->pOut, "\n", 1);
    }
  }else{
    Blob content;
    blob_zero(&content);
    configure_render_special_name(zName, &content);
    blob_appendf(pXfer->pOut, "config %s %d\n%s\n", zName,
                 blob_size(&content), blob_str(&content));
    blob_reset(&content);
  }
}

/*
** Called when there is an attempt to transfer private content to and
** from a server without authorization.
*/
static void server_private_xfer_not_authorized(void){
  @ error not\sauthorized\sto\ssync\sprivate\scontent
}

/*
** Run the specified TH1 script, if any, and returns the return code or TH_OK
** when there is no script.
*/
static int run_script(const char *zScript){
  if( !zScript ){
    return TH_OK; /* No script, return success. */
  }
  Th_FossilInit(); /* Make sure TH1 is ready. */
  return Th_Eval(g.interp, 0, zScript, -1);
}

/*
** Run the pre-transfer TH1 script, if any, and returns the return code.
*/
static int run_common_script(void){
  return run_script(db_get("xfer-common-script", 0));
}

/*
** Run the post-push TH1 script, if any, and returns the return code.
*/
static int run_push_script(void){
  return run_script(db_get("xfer-push-script", 0));
}

/*
** If this variable is set, disable login checks.  Used for debugging
** only.
*/
static int disableLogin = 0;

/*
** The CGI/HTTP preprocessor always redirects requests with a content-type
** of application/x-fossil or application/x-fossil-debug to this page,
** regardless of what path was specified in the HTTP header.  This allows
** clone clients to specify a URL that omits default pathnames, such
** as "http://fossil-scm.org/" instead of "http://fossil-scm.org/index.cgi".
**
** WEBPAGE: xfer
**
** This is the transfer handler on the server side.  The transfer
** message has been uncompressed and placed in the g.cgiIn blob.
** Process this message and form an appropriate reply.
*/
void page_xfer(void){
  int isPull = 0;
  int isPush = 0;
  int nErr = 0;
  Xfer xfer;
  int deltaFlag = 0;
  int isClone = 0;
  int nGimme = 0;
  int size;
  int recvConfig = 0;
  char *zNow;

  if( fossil_strcmp(PD("REQUEST_METHOD","POST"),"POST") ){
     fossil_redirect_home();
  }
  g.zLogin = "anonymous";
  login_set_anon_nobody_capabilities();
  login_check_credentials();
  memset(&xfer, 0, sizeof(xfer));
  blobarray_zero(xfer.aToken, count(xfer.aToken));
  cgi_set_content_type(g.zContentType);
  cgi_reset_content();
  if( db_schema_is_outofdate() ){
    @ error database\sschema\sis\sout-of-date\son\sthe\sserver.
    return;
  }
  blob_zero(&xfer.err);
  xfer.pIn = &g.cgiIn;
  xfer.pOut = cgi_output_blob();
  xfer.mxSend = db_get_int("max-download", 5000000);
  g.xferPanic = 1;

  db_begin_transaction();
  db_multi_exec(
     "CREATE TEMP TABLE onremote(rid INTEGER PRIMARY KEY);"
  );
  manifest_crosslink_begin();
  if( run_common_script()==TH_ERROR ){
    cgi_reset_content();
    @ error common\sscript\sfailed:\s%F(Th_GetResult(g.interp, 0))
    nErr++;
  }
  while( blob_line(xfer.pIn, &xfer.line) ){
    if( blob_buffer(&xfer.line)[0]=='#' ) continue;
    if( blob_size(&xfer.line)==0 ) continue;
    xfer.nToken = blob_tokenize(&xfer.line, xfer.aToken, count(xfer.aToken));

    /*   file UUID SIZE \n CONTENT
    **   file UUID DELTASRC SIZE \n CONTENT
    **
    ** Accept a file from the client.
    */
    if( blob_eq(&xfer.aToken[0], "file") ){
      if( !isPush ){
        cgi_reset_content();
        @ error not\sauthorized\sto\swrite
        nErr++;
        break;
      }
      xfer_accept_file(&xfer, 0);
      if( blob_size(&xfer.err) ){
        cgi_reset_content();
        @ error %T(blob_str(&xfer.err))
        nErr++;
        break;
      }
    }else

    /*   cfile UUID USIZE CSIZE \n CONTENT
    **   cfile UUID DELTASRC USIZE CSIZE \n CONTENT
    **
    ** Accept a file from the client.
    */
    if( blob_eq(&xfer.aToken[0], "cfile") ){
      if( !isPush ){
        cgi_reset_content();
        @ error not\sauthorized\sto\swrite
        nErr++;
        break;
      }
      xfer_accept_compressed_file(&xfer);
      if( blob_size(&xfer.err) ){
        cgi_reset_content();
        @ error %T(blob_str(&xfer.err))
        nErr++;
        break;
      }
    }else

    /*   gimme UUID
    **
    ** Client is requesting a file.  Send it.
    */
    if( blob_eq(&xfer.aToken[0], "gimme")
     && xfer.nToken==2
     && blob_is_uuid(&xfer.aToken[1])
    ){
      nGimme++;
      if( isPull ){
        int rid = rid_from_uuid(&xfer.aToken[1], 0, 0);
        if( rid ){
          send_file(&xfer, rid, &xfer.aToken[1], deltaFlag);
        }
      }
    }else

    /*   igot UUID ?ISPRIVATE?
    **
    ** Client announces that it has a particular file.  If the ISPRIVATE
    ** argument exists and is non-zero, then the file is a private file.
    */
    if( xfer.nToken>=2
     && blob_eq(&xfer.aToken[0], "igot")
     && blob_is_uuid(&xfer.aToken[1])
    ){
      if( isPush ){
        if( xfer.nToken==2 || blob_eq(&xfer.aToken[2],"1")==0 ){
          rid_from_uuid(&xfer.aToken[1], 1, 0);
        }else if( g.perm.Private ){
          rid_from_uuid(&xfer.aToken[1], 1, 1);
        }else{
          server_private_xfer_not_authorized();
        }
      }
    }else
  
    
    /*    pull  SERVERCODE  PROJECTCODE
    **    push  SERVERCODE  PROJECTCODE
    **
    ** The client wants either send or receive.  The server should
    ** verify that the project code matches.
    */
    if( xfer.nToken==3
     && (blob_eq(&xfer.aToken[0], "pull") || blob_eq(&xfer.aToken[0], "push"))
     && blob_is_uuid(&xfer.aToken[1])
     && blob_is_uuid(&xfer.aToken[2])
    ){
      const char *zPCode;
      zPCode = db_get("project-code", 0);
      if( zPCode==0 ){
        fossil_panic("missing project code");
      }
      if( !blob_eq_str(&xfer.aToken[2], zPCode, -1) ){
        cgi_reset_content();
        @ error wrong\sproject
        nErr++;
        break;
      }
      login_check_credentials();
      if( blob_eq(&xfer.aToken[0], "pull") ){
        if( !g.perm.Read ){
          cgi_reset_content();
          @ error not\sauthorized\sto\sread
          nErr++;
          break;
        }
        isPull = 1;
      }else{
        if( !g.perm.Write ){
          if( !isPull ){
            cgi_reset_content();
            @ error not\sauthorized\sto\swrite
            nErr++;
          }else{
            @ message pull\sonly\s-\snot\sauthorized\sto\spush
          }
        }else{
          isPush = 1;
        }
      }
    }else

    /*    clone   ?PROTOCOL-VERSION?  ?SEQUENCE-NUMBER?
    **
    ** The client knows nothing.  Tell all.
    */
    if( blob_eq(&xfer.aToken[0], "clone") ){
      int iVers;
      login_check_credentials();
      if( !g.perm.Clone ){
        cgi_reset_content();
        @ push %s(db_get("server-code", "x")) %s(db_get("project-code", "x"))
        @ error not\sauthorized\sto\sclone
        nErr++;
        break;
      }
      if( xfer.nToken==3
       && blob_is_int(&xfer.aToken[1], &iVers)
       && iVers>=2
      ){
        int seqno, max;
        if( iVers>=3 ){
          cgi_set_content_type("application/x-fossil-uncompressed");
        }
        blob_is_int(&xfer.aToken[2], &seqno);
        max = db_int(0, "SELECT max(rid) FROM blob");
        while( xfer.mxSend>blob_size(xfer.pOut) && seqno<=max ){
          if( iVers>=3 ){
            send_compressed_file(&xfer, seqno);
          }else{
            send_file(&xfer, seqno, 0, 1);
          }
          seqno++;
        }
        if( seqno>=max ) seqno = 0;
        @ clone_seqno %d(seqno)
      }else{
        isClone = 1;
        isPull = 1;
        deltaFlag = 1;
      }
      @ push %s(db_get("server-code", "x")) %s(db_get("project-code", "x"))
    }else

    /*    login  USER  NONCE  SIGNATURE
    **
    ** Check for a valid login.  This has to happen before anything else.
    ** The client can send multiple logins.  Permissions are cumulative.
    */
    if( blob_eq(&xfer.aToken[0], "login")
     && xfer.nToken==4
    ){
      if( disableLogin ){
        g.perm.Read = g.perm.Write = g.perm.Private = g.perm.Admin = 1;
      }else{
        if( check_tail_hash(&xfer.aToken[2], xfer.pIn)
         || check_login(&xfer.aToken[1], &xfer.aToken[2], &xfer.aToken[3])
        ){
          cgi_reset_content();
          @ error login\sfailed
          nErr++;
          break;
        }        
      }
    }else
    
    /*    reqconfig  NAME
    **
    ** Request a configuration value
    */
    if( blob_eq(&xfer.aToken[0], "reqconfig")
     && xfer.nToken==2
    ){
      if( g.perm.Read ){
        char *zName = blob_str(&xfer.aToken[1]);
        if( zName[0]=='/' ){
          /* New style configuration transfer */
          int groupMask = configure_name_to_mask(&zName[1], 0);
          if( !g.perm.Admin ) groupMask &= ~CONFIGSET_USER;
          if( !g.perm.RdAddr ) groupMask &= ~CONFIGSET_ADDR;
          configure_send_group(xfer.pOut, groupMask, 0);
        }else if( configure_is_exportable(zName) ){
          /* Old style configuration transfer */
          send_legacy_config_card(&xfer, zName);
        }
      }
    }else
    
    /*   config NAME SIZE \n CONTENT
    **
    ** Receive a configuration value from the client.  This is only
    ** permitted for high-privilege users.
    */
    if( blob_eq(&xfer.aToken[0],"config") && xfer.nToken==3
        && blob_is_int(&xfer.aToken[2], &size) ){
      const char *zName = blob_str(&xfer.aToken[1]);
      Blob content;
      blob_zero(&content);
      blob_extract(xfer.pIn, size, &content);
      if( !g.perm.Admin ){
        cgi_reset_content();
        @ error not\sauthorized\sto\spush\sconfiguration
        nErr++;
        break;
      }
      if( !recvConfig && zName[0]=='@' ){
        configure_prepare_to_receive(0);
        recvConfig = 1;
      }
      configure_receive(zName, &content, CONFIGSET_ALL);
      blob_reset(&content);
      blob_seek(xfer.pIn, 1, BLOB_SEEK_CUR);
    }else

      

    /*    cookie TEXT
    **
    ** A cookie contains a arbitrary-length argument that is server-defined.
    ** The argument must be encoded so as not to contain any whitespace.
    ** The server can optionally send a cookie to the client.  The client
    ** might then return the same cookie back to the server on its next
    ** communication.  The cookie might record information that helps
    ** the server optimize a push or pull.
    **
    ** The client is not required to return a cookie.  So the server
    ** must not depend on the cookie.  The cookie should be an optimization
    ** only.  The client might also send a cookie that came from a different
    ** server.  So the server must be prepared to distinguish its own cookie
    ** from cookies originating from other servers.  The client might send
    ** back several different cookies to the server.  The server should be
    ** prepared to sift through the cookies and pick the one that it wants.
    */
    if( blob_eq(&xfer.aToken[0], "cookie") && xfer.nToken==2 ){
      /* Process the cookie */
    }else


    /*    private
    **
    ** This card indicates that the next "file" or "cfile" will contain
    ** private content.
    */
    if( blob_eq(&xfer.aToken[0], "private") ){
      if( !g.perm.Private ){
        server_private_xfer_not_authorized();
      }else{
        xfer.nextIsPrivate = 1;
      }
    }else


    /*    pragma NAME VALUE...
    **
    ** The client issue pragmas to try to influence the behavior of the
    ** server.  These are requests only.  Unknown pragmas are silently
    ** ignored.
    */
    if( blob_eq(&xfer.aToken[0], "pragma") && xfer.nToken>=2 ){
      /*   pragma send-private
      **
      ** If the user has the "x" privilege (which must be set explicitly -
      ** it is not automatic with "a" or "s") then this pragma causes
      ** private information to be pulled in addition to public records.
      */
      if( blob_eq(&xfer.aToken[1], "send-private") ){
        login_check_credentials();
        if( !g.perm.Private ){
          server_private_xfer_not_authorized();
        }else{
          xfer.syncPrivate = 1;
        }
      }
    }else

    /* Unknown message
    */
    {
      cgi_reset_content();
      @ error bad\scommand:\s%F(blob_str(&xfer.line))
    }
    blobarray_reset(xfer.aToken, xfer.nToken);
  }
  if( isPush ){
    if( run_push_script()==TH_ERROR ){
      cgi_reset_content();
      @ error push\sscript\sfailed:\s%F(Th_GetResult(g.interp, 0))
      nErr++;
    }
    request_phantoms(&xfer, 500);
  }
  if( isClone && nGimme==0 ){
    /* The initial "clone" message from client to server contains no
    ** "gimme" cards. On that initial message, send the client an "igot"
    ** card for every artifact currently in the respository.  This will
    ** cause the client to create phantoms for all artifacts, which will
    ** in turn make sure that the entire repository is sent efficiently
    ** and expeditiously.
    */
    send_all(&xfer);
    if( xfer.syncPrivate ) send_private(&xfer);
  }else if( isPull ){
    create_cluster();
    send_unclustered(&xfer);
    if( xfer.syncPrivate ) send_private(&xfer);
  }
  if( recvConfig ){
    configure_finalize_receive();
  }
  manifest_crosslink_end();

  /* Send the server timestamp last, in case prior processing happened
  ** to use up a significant fraction of our time window.
  */
  zNow = db_text(0, "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%S', 'now')");
  @ # timestamp %s(zNow)
  free(zNow);

  db_end_transaction(0);
}

/*
** COMMAND: test-xfer
**
** This command is used for debugging the server.  There is a single
** argument which is the uncompressed content of an "xfer" message
** from client to server.  This command interprets that message as
** if had been received by the server.
**
** On the client side, run:
**
**      fossil push http://bogus/ --httptrace
**
** Or a similar command to provide the output.  The content of the
** message will appear on standard output.  Capture this message
** into a file named (for example) out.txt.  Then run the
** server in gdb:
**
**     gdb fossil
**     r test-xfer out.txt
*/
void cmd_test_xfer(void){
  db_find_and_open_repository(0,0);
  if( g.argc!=2 && g.argc!=3 ){
    usage("?MESSAGEFILE?");
  }
  blob_zero(&g.cgiIn);
  blob_read_from_file(&g.cgiIn, g.argc==2 ? "-" : g.argv[2]);
  disableLogin = 1;
  page_xfer();
  fossil_print("%s\n", cgi_extract_content());
}

/*
** Format strings for progress reporting.
*/
static const char zLabelFormat[] = "%-10s %10s %10s %10s %10s\n";
static const char zValueFormat[] = "\r%-10s %10d %10d %10d %10d\n";


/*
** Sync to the host identified in g.urlName and g.urlPath.  This
** routine is called by the client.
**
** Records are pushed to the server if pushFlag is true.  Records
** are pulled if pullFlag is true.  A full sync occurs if both are
** true.
*/
int client_sync(
  int pushFlag,           /* True to do a push (or a sync) */
  int pullFlag,           /* True to do a pull (or a sync) */
  int cloneFlag,          /* True if this is a clone */
  int privateFlag,        /* True to exchange private branches */
  int configRcvMask,      /* Receive these configuration items */
  int configSendMask      /* Send these configuration items */
){
  int go = 1;             /* Loop until zero */
  int nCardSent = 0;      /* Number of cards sent */
  int nCardRcvd = 0;      /* Number of cards received */
  int nCycle = 0;         /* Number of round trips to the server */
  int size;               /* Size of a config value */
  int origConfigRcvMask;  /* Original value of configRcvMask */
  int nFileRecv;          /* Number of files received */
  int mxPhantomReq = 200; /* Max number of phantoms to request per comm */
  const char *zCookie;    /* Server cookie */
  i64 nSent, nRcvd;       /* Bytes sent and received (after compression) */
  int cloneSeqno = 1;     /* Sequence number for clones */
  Blob send;              /* Text we are sending to the server */
  Blob recv;              /* Reply we got back from the server */
  Xfer xfer;              /* Transfer data */
  int pctDone;            /* Percentage done with a message */
  int lastPctDone = -1;   /* Last displayed pctDone */
  double rArrivalTime;    /* Time at which a message arrived */
  const char *zSCode = db_get("server-code", "x");
  const char *zPCode = db_get("project-code", 0);
  int nErr = 0;           /* Number of errors */

  if( db_get_boolean("dont-push", 0) ) pushFlag = 0;
  if( pushFlag + pullFlag + cloneFlag == 0 
     && configRcvMask==0 && configSendMask==0 ) return 0;

  transport_stats(0, 0, 1);
  socket_global_init();
  memset(&xfer, 0, sizeof(xfer));
  xfer.pIn = &recv;
  xfer.pOut = &send;
  xfer.mxSend = db_get_int("max-upload", 250000);
  if( privateFlag ){
    g.perm.Private = 1;
    xfer.syncPrivate = 1;
  }

  assert( pushFlag | pullFlag | cloneFlag | configRcvMask | configSendMask );
  db_begin_transaction();
  db_record_repository_filename(0);
  db_multi_exec(
    "CREATE TEMP TABLE onremote(rid INTEGER PRIMARY KEY);"
  );
  blobarray_zero(xfer.aToken, count(xfer.aToken));
  blob_zero(&send);
  blob_zero(&recv);
  blob_zero(&xfer.err);
  blob_zero(&xfer.line);
  origConfigRcvMask = 0;


  /* Send the send-private pragma if we are trying to sync private data */
  if( privateFlag ) blob_append(&send, "pragma send-private\n", -1);

  /*
  ** Always begin with a clone, pull, or push message
  */
  if( cloneFlag ){
    blob_appendf(&send, "clone 3 %d\n", cloneSeqno);
    pushFlag = 0;
    pullFlag = 0;
    nCardSent++;
    /* TBD: Request all transferable configuration values */
    content_enable_dephantomize(0);
  }else if( pullFlag ){
    blob_appendf(&send, "pull %s %s\n", zSCode, zPCode);
    nCardSent++;
  }
  if( pushFlag ){
    blob_appendf(&send, "push %s %s\n", zSCode, zPCode);
    nCardSent++;
  }
  manifest_crosslink_begin();
  transport_global_startup();
  fossil_print(zLabelFormat, "", "Bytes", "Cards", "Artifacts", "Deltas");

  while( go ){
    int newPhantom = 0;
    char *zRandomness;

    /* Send make the most recently received cookie.  Let the server
    ** figure out if this is a cookie that it cares about.
    */
    zCookie = db_get("cookie", 0);
    if( zCookie ){
      blob_appendf(&send, "cookie %s\n", zCookie);
    }
    
    /* Generate gimme cards for phantoms and leaf cards
    ** for all leaves.
    */
    if( pullFlag || (cloneFlag && cloneSeqno==1) ){
      request_phantoms(&xfer, mxPhantomReq);
    }
    if( pushFlag ){
      send_unsent(&xfer);
      nCardSent += send_unclustered(&xfer);
      if( privateFlag ) send_private(&xfer);
    }

    /* Send configuration parameter requests.  On a clone, delay sending
    ** this until the second cycle since the login card might fail on 
    ** the first cycle.
    */
    if( configRcvMask && (cloneFlag==0 || nCycle>0) ){
      const char *zName;
      zName = configure_first_name(configRcvMask);
      while( zName ){
        blob_appendf(&send, "reqconfig %s\n", zName);
        zName = configure_next_name(configRcvMask);
        nCardSent++;
      }
      if( (configRcvMask & (CONFIGSET_USER|CONFIGSET_TKT))!=0
       && (configRcvMask & CONFIGSET_OLDFORMAT)!=0
      ){
        int overwrite = (configRcvMask & CONFIGSET_OVERWRITE)!=0;
        configure_prepare_to_receive(overwrite);
      }
      origConfigRcvMask = configRcvMask;
      configRcvMask = 0;
    }

    /* Send configuration parameters being pushed */
    if( configSendMask ){
      if( configSendMask & CONFIGSET_OLDFORMAT ){
        const char *zName;
        zName = configure_first_name(configSendMask);
        while( zName ){
          send_legacy_config_card(&xfer, zName);
          zName = configure_next_name(configSendMask);
          nCardSent++;
        }
      }else{
        nCardSent += configure_send_group(xfer.pOut, configSendMask, 0);
      }
      configSendMask = 0;
    }

    /* Append randomness to the end of the message.  This makes all
    ** messages unique so that that the login-card nonce will always
    ** be unique.
    */
    zRandomness = db_text(0, "SELECT hex(randomblob(20))");
    blob_appendf(&send, "# %s\n", zRandomness);
    free(zRandomness);

    /* Exchange messages with the server */
    fossil_print(zValueFormat, "Sent:",
                 blob_size(&send), nCardSent+xfer.nGimmeSent+xfer.nIGotSent,
                 xfer.nFileSent, xfer.nDeltaSent);
    nCardSent = 0;
    nCardRcvd = 0;
    xfer.nFileSent = 0;
    xfer.nDeltaSent = 0;
    xfer.nGimmeSent = 0;
    xfer.nIGotSent = 0;
    if( !g.cgiOutput && !g.fQuiet ){
      fossil_print("waiting for server...");
    }
    fflush(stdout);
    if( http_exchange(&send, &recv, cloneFlag==0 || nCycle>0) ){
      nErr++;
      break;
    }
    lastPctDone = -1;
    blob_reset(&send);
    rArrivalTime = db_double(0.0, "SELECT julianday('now')");

    /* Send the send-private pragma if we are trying to sync private data */
    if( privateFlag ) blob_append(&send, "pragma send-private\n", -1);

    /* Begin constructing the next message (which might never be
    ** sent) by beginning with the pull or push cards
    */
    if( pullFlag ){
      blob_appendf(&send, "pull %s %s\n", zSCode, zPCode);
      nCardSent++;
    }
    if( pushFlag ){
      blob_appendf(&send, "push %s %s\n", zSCode, zPCode);
      nCardSent++;
    }
    go = 0;

    /* Process the reply that came back from the server */
    while( blob_line(&recv, &xfer.line) ){
      if( blob_buffer(&xfer.line)[0]=='#' ){
        const char *zLine = blob_buffer(&xfer.line);
        if( memcmp(zLine, "# timestamp ", 12)==0 ){
          char zTime[20];
          double rDiff;
          sqlite3_snprintf(sizeof(zTime), zTime, "%.19s", &zLine[12]);
          rDiff = db_double(9e99, "SELECT julianday('%q') - %.17g",
                            zTime, rArrivalTime);
          if( rDiff>9e98 || rDiff<-9e98 ) rDiff = 0.0;
          if( (rDiff*24.0*3600.0) > 10.0 ){
             fossil_warning("*** time skew *** server is fast by %s",
                            db_timespan_name(rDiff));
             g.clockSkewSeen = 1;
          }else if( rDiff*24.0*3600.0 < -(blob_size(&recv)/5000.0 + 20.0) ){
             fossil_warning("*** time skew *** server is slow by %s",
                            db_timespan_name(-rDiff));
             g.clockSkewSeen = 1;
          }
        }
        nCardRcvd++;
        continue;
      }
      xfer.nToken = blob_tokenize(&xfer.line, xfer.aToken, count(xfer.aToken));
      nCardRcvd++;
      if( !g.cgiOutput && !g.fQuiet && recv.nUsed>0 ){
        pctDone = (recv.iCursor*100)/recv.nUsed;
        if( pctDone!=lastPctDone ){
          fossil_print("\rprocessed: %d%%         ", pctDone);
          lastPctDone = pctDone;
          fflush(stdout);
        }
      }

      /*   file UUID SIZE \n CONTENT
      **   file UUID DELTASRC SIZE \n CONTENT
      **
      ** Receive a file transmitted from the server.
      */
      if( blob_eq(&xfer.aToken[0],"file") ){
        xfer_accept_file(&xfer, cloneFlag);
      }else

      /*   cfile UUID USIZE CSIZE \n CONTENT
      **   cfile UUID DELTASRC USIZE CSIZE \n CONTENT
      **
      ** Receive a compressed file transmitted from the server.
      */
      if( blob_eq(&xfer.aToken[0],"cfile") ){
        xfer_accept_compressed_file(&xfer);
      }else

      /*   gimme UUID
      **
      ** Server is requesting a file.  If the file is a manifest, assume
      ** that the server will also want to know all of the content files
      ** associated with the manifest and send those too.
      */
      if( blob_eq(&xfer.aToken[0], "gimme")
       && xfer.nToken==2
       && blob_is_uuid(&xfer.aToken[1])
      ){
        if( pushFlag ){
          int rid = rid_from_uuid(&xfer.aToken[1], 0, 0);
          if( rid ) send_file(&xfer, rid, &xfer.aToken[1], 0);
        }
      }else
  
      /*   igot UUID  ?PRIVATEFLAG?
      **
      ** Server announces that it has a particular file.  If this is
      ** not a file that we have and we are pulling, then create a
      ** phantom to cause this file to be requested on the next cycle.
      ** Always remember that the server has this file so that we do
      ** not transmit it by accident.
      **
      ** If the PRIVATE argument exists and is 1, then the file is 
      ** private.  Pretend it does not exists if we are not pulling
      ** private files.
      */
      if( xfer.nToken>=2
       && blob_eq(&xfer.aToken[0], "igot")
       && blob_is_uuid(&xfer.aToken[1])
      ){
        int rid;
        int isPriv = xfer.nToken>=3 && blob_eq(&xfer.aToken[2],"1");
        rid = rid_from_uuid(&xfer.aToken[1], 0, 0);
        if( rid>0 ){
          if( !isPriv ) content_make_public(rid);
        }else if( isPriv && !g.perm.Private ){
          /* ignore private files */
        }else if( pullFlag || cloneFlag ){
          rid = content_new(blob_str(&xfer.aToken[1]), isPriv);
          if( rid ) newPhantom = 1;
        }
        remote_has(rid);
      }else
    
      
      /*   push  SERVERCODE  PRODUCTCODE
      **
      ** Should only happen in response to a clone.  This message tells
      ** the client what product to use for the new database.
      */
      if( blob_eq(&xfer.aToken[0],"push")
       && xfer.nToken==3
       && cloneFlag
       && blob_is_uuid(&xfer.aToken[1])
       && blob_is_uuid(&xfer.aToken[2])
      ){
        if( blob_eq_str(&xfer.aToken[1], zSCode, -1) ){
          fossil_fatal("server loop");
        }
        if( zPCode==0 ){
          zPCode = mprintf("%b", &xfer.aToken[2]);
          db_set("project-code", zPCode, 0);
        }
        if( cloneSeqno>0 ) blob_appendf(&send, "clone 3 %d\n", cloneSeqno);
        nCardSent++;
      }else
      
      /*   config NAME SIZE \n CONTENT
      **
      ** Receive a configuration value from the server.
      **
      ** The received configuration setting is silently ignored if it was
      ** not requested by a prior "reqconfig" sent from client to server.
      */
      if( blob_eq(&xfer.aToken[0],"config") && xfer.nToken==3
          && blob_is_int(&xfer.aToken[2], &size) ){
        const char *zName = blob_str(&xfer.aToken[1]);
        Blob content;
        blob_zero(&content);
        blob_extract(xfer.pIn, size, &content);
        g.perm.Admin = g.perm.RdAddr = 1;
        configure_receive(zName, &content, origConfigRcvMask);
        nCardSent++;
        blob_reset(&content);
        blob_seek(xfer.pIn, 1, BLOB_SEEK_CUR);
      }else

      
      /*    cookie TEXT
      **
      ** The server might include a cookie in its reply.  The client
      ** should remember this cookie and send it back to the server
      ** in its next query.
      **
      ** Each cookie received overwrites the prior cookie from the
      ** same server.
      */
      if( blob_eq(&xfer.aToken[0], "cookie") && xfer.nToken==2 ){
        db_set("cookie", blob_str(&xfer.aToken[1]), 0);
      }else


      /*    private
      **
      ** This card indicates that the next "file" or "cfile" will contain
      ** private content.
      */
      if( blob_eq(&xfer.aToken[0], "private") ){
        xfer.nextIsPrivate = 1;
      }else


      /*    clone_seqno N
      **
      ** When doing a clone, the server tries to send all of its artifacts
      ** in sequence.  This card indicates the sequence number of the next
      ** blob that needs to be sent.  If N<=0 that indicates that all blobs
      ** have been sent.
      */
      if( blob_eq(&xfer.aToken[0], "clone_seqno") && xfer.nToken==2 ){
        blob_is_int(&xfer.aToken[1], &cloneSeqno);
      }else

      /*   message MESSAGE
      **
      ** Print a message.  Similar to "error" but does not stop processing.
      **
      ** If the "login failed" message is seen, clear the sync password prior
      ** to the next cycle.
      */        
      if( blob_eq(&xfer.aToken[0],"message") && xfer.nToken==2 ){
        char *zMsg = blob_terminate(&xfer.aToken[1]);
        defossilize(zMsg);
        if( zMsg ) fossil_print("\rServer says: %s\n", zMsg);
      }else

      /*    pragma NAME VALUE...
      **
      ** The server can send pragmas to try to convey meta-information to
      ** the client.  These are informational only.  Unknown pragmas are 
      ** silently ignored.
      */
      if( blob_eq(&xfer.aToken[0], "pragma") && xfer.nToken>=2 ){
      }else

      /*   error MESSAGE
      **
      ** Report an error and abandon the sync session.
      **
      ** Except, when cloning we will sometimes get an error on the
      ** first message exchange because the project-code is unknown
      ** and so the login card on the request was invalid.  The project-code
      ** is returned in the reply before the error card, so second and 
      ** subsequent messages should be OK.  Nevertheless, we need to ignore
      ** the error card on the first message of a clone.
      */        
      if( blob_eq(&xfer.aToken[0],"error") && xfer.nToken==2 ){
        if( !cloneFlag || nCycle>0 ){
          char *zMsg = blob_terminate(&xfer.aToken[1]);
          defossilize(zMsg);
          if( fossil_strcmp(zMsg, "login failed")==0 ){
            if( nCycle<2 ){
              if( !g.dontKeepUrl ) db_unset("last-sync-pw", 0);
              go = 1;
            }
          }else{
            blob_appendf(&xfer.err, "\rserver says: %s", zMsg);
          }
          fossil_warning("\rError: %s", zMsg);
          nErr++;
          break;
        }
      }else

      /* Unknown message */
      if( xfer.nToken>0 ){
        if( blob_str(&xfer.aToken[0])[0]=='<' ){
          fossil_warning(
            "server replies with HTML instead of fossil sync protocol:\n%b",
            &recv
          );
          nErr++;
          break;
        }
        blob_appendf(&xfer.err, "unknown command: [%b]", &xfer.aToken[0]);
      }

      if( blob_size(&xfer.err) ){
        fossil_warning("%b", &xfer.err);
        nErr++;
        break;
      }
      blobarray_reset(xfer.aToken, xfer.nToken);
      blob_reset(&xfer.line);
    }
    if( (configRcvMask & (CONFIGSET_USER|CONFIGSET_TKT))!=0
     && (configRcvMask & CONFIGSET_OLDFORMAT)!=0
    ){
      configure_finalize_receive();
    }
    origConfigRcvMask = 0;
    if( nCardRcvd>0 ){
      fossil_print(zValueFormat, "Received:",
                   blob_size(&recv), nCardRcvd,
                   xfer.nFileRcvd, xfer.nDeltaRcvd + xfer.nDanglingFile);
    }
    blob_reset(&recv);
    nCycle++;

    /* If we received one or more files on the previous exchange but
    ** there are still phantoms, then go another round.
    */
    nFileRecv = xfer.nFileRcvd + xfer.nDeltaRcvd + xfer.nDanglingFile;
    if( (nFileRecv>0 || newPhantom) && db_exists("SELECT 1 FROM phantom") ){
      go = 1;
      mxPhantomReq = nFileRecv*2;
      if( mxPhantomReq<200 ) mxPhantomReq = 200;
    }else if( cloneFlag && nFileRecv>0 ){
      go = 1;
    }
    nCardRcvd = 0;
    xfer.nFileRcvd = 0;
    xfer.nDeltaRcvd = 0;
    xfer.nDanglingFile = 0;

    /* If we have one or more files queued to send, then go
    ** another round 
    */
    if( xfer.nFileSent+xfer.nDeltaSent>0 ){
      go = 1;
    }

    /* If this is a clone, the go at least two rounds */
    if( cloneFlag && nCycle==1 ) go = 1;

    /* Stop the cycle if the server sends a "clone_seqno 0" card and
    ** we have gone at least two rounds.  Always go at least two rounds
    ** on a clone in order to be sure to retrieve the configuration
    ** information which is only sent on the second round.
    */
    if( cloneSeqno<=0 && nCycle>1 ) go = 0;   
  };
  transport_stats(&nSent, &nRcvd, 1);
  fossil_print("Total network traffic: %lld bytes sent, %lld bytes received\n",
               nSent, nRcvd);
  transport_close();
  transport_global_shutdown();
  db_multi_exec("DROP TABLE onremote");
  manifest_crosslink_end();
  content_enable_dephantomize(1);
  db_end_transaction(0);
  return nErr;
}

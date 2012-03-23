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
** Commands and procedures used for creating, processing, editing, and
** querying information about users.
*/
#include "config.h"
#include "user.h"


/*
** Strip leading and trailing space from a string and add the string
** onto the end of a blob.
*/
static void strip_string(Blob *pBlob, char *z){
  int i;
  blob_reset(pBlob);
  while( fossil_isspace(*z) ){ z++; }
  for(i=0; z[i]; i++){
    if( z[i]=='\r' || z[i]=='\n' ){
       while( i>0 && fossil_isspace(z[i-1]) ){ i--; }
       z[i] = 0;
       break;
    }
    if( z[i]<' ' ) z[i] = ' ';
  }
  blob_append(pBlob, z, -1);
}

#if defined(_WIN32)
#ifdef __MINGW32__
#include <conio.h>
#endif
/*
** getpass for Windows
*/
static char *getpass(const char *prompt){
  static char pwd[64];
  size_t i;

  fputs(prompt,stderr);
  fflush(stderr);
  for(i=0; i<sizeof(pwd)-1; ++i){
    pwd[i] = _getch();
    if(pwd[i]=='\r' || pwd[i]=='\n'){
      break;
    }
    /* BS or DEL */
    else if(i>0 && (pwd[i]==8 || pwd[i]==127)){
      i -= 2;
      continue;
    }
    /* CTRL-C */
    else if(pwd[i]==3) {
      i=0;
      break;
    }
    /* ESC */
    else if(pwd[i]==27){
      i=0;
      break;
    }
    else{
      fputc('*',stderr);
    }
  }
  pwd[i]='\0';
  fputs("\n", stderr);
  return pwd;
}
#endif

/*
** Do a single prompt for a passphrase.  Store the results in the blob.
*/
static void prompt_for_passphrase(const char *zPrompt, Blob *pPassphrase){
  char *z = getpass(zPrompt);
  strip_string(pPassphrase, z);
}

/*
** Prompt the user for a password.  Store the result in the pPassphrase
** blob.
**
** Behavior is controlled by the verify parameter:
**
**     0     Just ask once.
**
**     1     If the first answer is a non-empty string, ask for
**           verification.  Repeat if the two strings do not match.
**
**     2     Ask twice, repeat if the strings do not match.
*/
void prompt_for_password(
  const char *zPrompt,
  Blob *pPassphrase,
  int verify
){
  Blob secondTry;
  blob_zero(pPassphrase);
  blob_zero(&secondTry);
  while(1){
    prompt_for_passphrase(zPrompt, pPassphrase);
    if( verify==0 ) break;
    if( verify==1 && blob_size(pPassphrase)==0 ) break;
    prompt_for_passphrase("Retype new password: ", &secondTry);
    if( blob_compare(pPassphrase, &secondTry) ){
      fossil_print("Passphrases do not match.  Try again...\n");
    }else{
      break;
    }
  }
  blob_reset(&secondTry);
}

/*
** Prompt the user to enter a single line of text.
*/
void prompt_user(const char *zPrompt, Blob *pIn){
  char *z;
  char zLine[1000];
  blob_zero(pIn);
  fossil_print("%s", zPrompt);
  fflush(stdout);
  z = fgets(zLine, sizeof(zLine), stdin);
  if( z ){
    strip_string(pIn, z);
  }
}


/*
** COMMAND: user*
**
** Usage: %fossil user SUBCOMMAND ...  ?-R|--repository FILE?
**
** Run various subcommands on users of the open repository or of
** the repository identified by the -R or --repository option.
**
**    %fossil user capabilities USERNAME ?STRING?
**
**        Query or set the capabilities for user USERNAME
**
**    %fossil user default ?USERNAME?
**
**        Query or set the default user.  The default user is the
**        user for command-line interaction.
**
**    %fossil user list
**
**        List all users known to the repository
**
**    %fossil user new ?USERNAME? ?CONTACT-INFO? ?PASSWORD?
**
**        Create a new user in the repository.  Users can never be
**        deleted.  They can be denied all access but they must continue
**        to exist in the database.
**
**    %fossil user password USERNAME ?PASSWORD?
**
**        Change the web access password for a user.
*/
void user_cmd(void){
  int n;
  db_find_and_open_repository(0, 0);
  if( g.argc<3 ){
    usage("capabilities|default|list|new|password ...");
  }
  n = strlen(g.argv[2]);
  if( n>=2 && strncmp(g.argv[2],"new",n)==0 ){
    Blob passwd, login, caps, contact;
    char *zPw;
    blob_init(&caps, db_get("default-perms", "u"), -1);

    if( g.argc>=4 ){
      blob_init(&login, g.argv[3], -1);
    }else{
      prompt_user("login: ", &login);
    }
    if( db_exists("SELECT 1 FROM user WHERE login=%B", &login) ){
      fossil_fatal("user %b already exists", &login);
    }
    if( g.argc>=5 ){
      blob_init(&contact, g.argv[4], -1);
    }else{
      prompt_user("contact-info: ", &contact);
    }
    if( g.argc>=6 ){
      blob_init(&passwd, g.argv[5], -1);
    }else{
      prompt_for_password("password: ", &passwd, 1);
    }
    zPw = sha1_shared_secret(blob_str(&passwd), blob_str(&login), 0);
    db_multi_exec(
      "INSERT INTO user(login,pw,cap,info,mtime)"
      "VALUES(%B,%Q,%B,%B,now())",
      &login, zPw, &caps, &contact
    );
    free(zPw);
  }else if( n>=2 && strncmp(g.argv[2],"default",n)==0 ){
    user_select();
    if( g.argc==3 ){
      fossil_print("%s\n", g.zLogin);
    }else{
      if( !db_exists("SELECT 1 FROM user WHERE login=%Q", g.argv[3]) ){
        fossil_fatal("no such user: %s", g.argv[3]);
      }
      if( g.localOpen ){
        db_lset("default-user", g.argv[3]);
      }else{
        db_set("default-user", g.argv[3], 0);
      }
    }
  }else if( n>=2 && strncmp(g.argv[2],"list",n)==0 ){
    Stmt q;
    db_prepare(&q, "SELECT login, info FROM user ORDER BY login");
    while( db_step(&q)==SQLITE_ROW ){
      fossil_print("%-12s %s\n", db_column_text(&q, 0), db_column_text(&q, 1));
    }
    db_finalize(&q);
  }else if( n>=2 && strncmp(g.argv[2],"password",2)==0 ){
    char *zPrompt;
    int uid;
    Blob pw;
    if( g.argc!=4 && g.argc!=5 ) usage("password USERNAME ?NEW-PASSWORD?");
    uid = db_int(0, "SELECT uid FROM user WHERE login=%Q", g.argv[3]);
    if( uid==0 ){
      fossil_fatal("no such user: %s", g.argv[3]);
    }
    if( g.argc==5 ){
      blob_init(&pw, g.argv[4], -1);
    }else{
      zPrompt = mprintf("New password for %s: ", g.argv[3]);
      prompt_for_password(zPrompt, &pw, 1);
    }
    if( blob_size(&pw)==0 ){
      fossil_print("password unchanged\n");
    }else{
      char *zSecret = sha1_shared_secret(blob_str(&pw), g.argv[3], 0);
      db_multi_exec("UPDATE user SET pw=%Q, mtime=now() WHERE uid=%d",
                    zSecret, uid);
      free(zSecret);
    }
  }else if( n>=2 && strncmp(g.argv[2],"capabilities",2)==0 ){
    int uid;
    if( g.argc!=4 && g.argc!=5 ){
      usage("user capabilities USERNAME ?PERMISSIONS?");
    }
    uid = db_int(0, "SELECT uid FROM user WHERE login=%Q", g.argv[3]);
    if( uid==0 ){
      fossil_fatal("no such user: %s", g.argv[3]);
    }
    if( g.argc==5 ){
      db_multi_exec(
        "UPDATE user SET cap=%Q, mtime=now() WHERE uid=%d",
        g.argv[4], uid
      );
    }
    fossil_print("%s\n", db_text(0, "SELECT cap FROM user WHERE uid=%d", uid));
  }else{
    fossil_panic("user subcommand should be one of: "
                 "capabilities default list new password");
  }
}

/*
** Attempt to set the user to zLogin
*/
static int attempt_user(const char *zLogin){
  int uid;

  if( zLogin==0 ){
    return 0;
  }
  uid = db_int(0, "SELECT uid FROM user WHERE login=%Q", zLogin);
  if( uid ){
    g.userUid = uid;
    g.zLogin = mprintf("%s", zLogin);
    return 1;
  }
  return 0;
}

/*
** Figure out what user is at the controls.
**
**   (1)  Use the --user and -U command-line options.
**
**   (2)  If the local database is open, check in VVAR.
**
**   (3)  Check the default user in the repository
**
**   (4)  Try the USER environment variable.
**
**   (5)  Use the first user in the USER table.
**
** The user name is stored in g.zLogin.  The uid is in g.userUid.
*/
void user_select(void){
  Stmt s;

  if( g.userUid ) return;
  if( g.zLogin ){
    if( attempt_user(g.zLogin)==0 ){
      fossil_fatal("no such user: %s", g.zLogin);
    }else{
      return;
    }
  }

  if( g.localOpen && attempt_user(db_lget("default-user",0)) ) return;

  if( attempt_user(db_get("default-user", 0)) ) return;

  if( attempt_user(fossil_getenv("USER")) ) return;

  db_prepare(&s,
    "SELECT uid, login FROM user"
    " WHERE login NOT IN ('anonymous','nobody','reader','developer')"
  );
  if( db_step(&s)==SQLITE_ROW ){
    g.userUid = db_column_int(&s, 0);
    g.zLogin = mprintf("%s", db_column_text(&s, 1));
  }
  db_finalize(&s);

  if( g.userUid==0 ){
    db_prepare(&s, "SELECT uid, login FROM user");
    if( db_step(&s)==SQLITE_ROW ){
      g.userUid = db_column_int(&s, 0);
      g.zLogin = mprintf("%s", db_column_text(&s, 1));
    }
    db_finalize(&s);
  }

  if( g.userUid==0 ){
    db_multi_exec(
      "INSERT INTO user(login, pw, cap, info, mtime)"
      "VALUES('anonymous', '', 'cfghjkmnoqw', '', now())"
    );
    g.userUid = db_last_insert_rowid();
    g.zLogin = "anonymous";
  }
}


/*
** COMMAND: test-hash-passwords
**
** Usage: %fossil test-hash-passwords REPOSITORY
**
** Convert all local password storage to use a SHA1 hash of the password
** rather than cleartext.  Passwords that are already stored as the SHA1
** has are unchanged.
*/
void user_hash_passwords_cmd(void){
  if( g.argc!=3 ) usage("REPOSITORY");
  db_open_repository(g.argv[2]);
  sqlite3_create_function(g.db, "shared_secret", 2, SQLITE_UTF8, 0,
                          sha1_shared_secret_sql_function, 0, 0);
  db_multi_exec(
    "UPDATE user SET pw=shared_secret(pw,login), mtime=now()"
    " WHERE length(pw)>0 AND length(pw)!=40"
  );
}

/*
** WEBPAGE: access_log
**
**    y=N      1: success only.  2: failure only.  3: both
**    n=N      Number of entries to show
**    o=N      Skip this many entries
*/
void access_log_page(void){
  int y = atoi(PD("y","3"));
  int n = atoi(PD("n","50"));
  int skip = atoi(PD("o","0"));
  Blob sql;
  Stmt q;
  int cnt = 0;
  int rc;

  login_check_credentials();
  if( !g.perm.Admin ){ login_needed(); return; }
  create_accesslog_table();

  if( P("delall") && P("delallbtn") ){
    db_multi_exec("DELETE FROM accesslog");
    cgi_redirectf("%s/access_log?y=%d&n=%d&o=%o", g.zTop, y, n, skip);
    return;
  }
  if( P("delanon") && P("delanonbtn") ){
    db_multi_exec("DELETE FROM accesslog WHERE uname='anonymous'");
    cgi_redirectf("%s/access_log?y=%d&n=%d&o=%o", g.zTop, y, n, skip);
    return;
  }
  if( P("delfail") && P("delfailbtn") ){
    db_multi_exec("DELETE FROM accesslog WHERE NOT success");
    cgi_redirectf("%s/access_log?y=%d&n=%d&o=%o", g.zTop, y, n, skip);
    return;
  }
  if( P("delold") && P("deloldbtn") ){
    db_multi_exec("DELETE FROM accesslog WHERE rowid in"
                  "(SELECT rowid FROM accesslog ORDER BY rowid DESC"
                  " LIMIT -1 OFFSET 200)");
    cgi_redirectf("%s/access_log?y=%d&n=%d", g.zTop, y, n);
    return;
  }
  style_header("Access Log");
  blob_zero(&sql);
  blob_append(&sql, 
    "SELECT uname, ipaddr, datetime(mtime, 'localtime'), success"
    "  FROM accesslog", -1
  );
  if( y==1 ){
    blob_append(&sql, "  WHERE success", -1);
  }else if( y==2 ){
    blob_append(&sql, "  WHERE NOT success", -1);
  }
  blob_appendf(&sql,"  ORDER BY rowid DESC LIMIT %d OFFSET %d", n+1, skip);
  if( skip ){
    style_submenu_element("Newer", "Newer entries",
              "%s/access_log?o=%d&n=%d&y=%d", g.zTop, skip>=n ? skip-n : 0,
              n, y);
  }
  rc = db_prepare_ignore_error(&q, blob_str(&sql));
  @ <center><table border="1" cellpadding="5">
  @ <tr><th width="33%%">Date</th><th width="34%%">User</th>
  @ <th width="33%%">IP Address</th></tr>
  while( rc==SQLITE_OK && db_step(&q)==SQLITE_ROW ){
    const char *zName = db_column_text(&q, 0);
    const char *zIP = db_column_text(&q, 1);
    const char *zDate = db_column_text(&q, 2);
    int bSuccess = db_column_int(&q, 3);
    cnt++;
    if( cnt>n ){
      style_submenu_element("Older", "Older entries",
                  "%s/access_log?o=%d&n=%d&y=%d", g.zTop, skip+n, n, y);
      break;
    }
    if( bSuccess ){
      @ <tr>
    }else{
      @ <tr bgcolor="#ffacc0">
    }
    @ <td>%s(zDate)</td><td>%h(zName)</td><td>%h(zIP)</td></tr>
  }
  if( skip>0 || cnt>n ){
    style_submenu_element("All", "All entries",
          "%s/access_log?n=10000000", g.zTop);
  }
  @ </table></center>
  db_finalize(&q);
  @ <hr>
  @ <form method="post" action="%s(g.zTop)/access_log">
  @ <input type="checkbox" name="delold">
  @ Delete all but the most recent 200 entries</input>
  @ <input type="submit" name="deloldbtn" value="Delete"></input>
  @ </form>
  @ <form method="post" action="%s(g.zTop)/access_log">
  @ <input type="checkbox" name="delanon">
  @ Delete all entries for user "anonymous"</input>
  @ <input type="submit" name="delanonbtn" value="Delete"></input>
  @ </form>
  @ <form method="post" action="%s(g.zTop)/access_log">
  @ <input type="checkbox" name="delfail">
  @ Delete all failed login attempts</input>
  @ <input type="submit" name="delfailbtn" value="Delete"></input>
  @ </form>
  @ <form method="post" action="%s(g.zTop)/access_log">
  @ <input type="checkbox" name="delall">
  @ Delete all entries</input>
  @ <input type="submit" name="delallbtn" value="Delete"></input>
  @ </form>
  style_footer();
}

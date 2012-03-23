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
** This file contains code for generating the login and logout screens.
**
** Notes:
**
** There are four special-case user-ids:  "anonymous", "nobody",
** "developer" and "reader".
**
** The capabilities of the nobody user are available to anyone,
** regardless of whether or not they are logged in.  The capabilities
** of anonymous are only available after logging in, but the login
** screen displays the password for the anonymous login, so this
** should not prevent a human user from doing so.  The capabilities
** of developer and reader are inherited by any user that has the
** "v" and "u" capabilities, respectively.
**
** The nobody user has capabilities that you want spiders to have.
** The anonymous user has capabilities that you want people without
** logins to have.
**
** Of course, a sophisticated spider could easily circumvent the
** anonymous login requirement and walk the website.  But that is
** not really the point.  The anonymous login keeps search-engine
** crawlers and site download tools like wget from walking change
** logs and downloading diffs of very version of the archive that
** has ever existed, and things like that.
*/
#include "config.h"
#include "login.h"
#if defined(_WIN32)  
#  include <windows.h>           /* for Sleep */
#  if defined(__MINGW32__) || defined(_MSC_VER)
#    define sleep Sleep            /* windows does not have sleep, but Sleep */
#  endif
#endif
#include <time.h>

/*
** Return the login-group name.  Or return 0 if this repository is
** not a member of a login-group.
*/
const char *login_group_name(void){
  static const char *zGroup = 0;
  static int once = 1;
  if( once ){
    zGroup = db_get("login-group-name", 0);
    once = 0;
  }
  return zGroup;
}

/*
** Return a path appropriate for setting a cookie.
**
** The path is g.zTop for single-repo cookies.  It is "/" for
** cookies of a login-group.
*/
static const char *login_cookie_path(void){
  if( login_group_name()==0 ){
    return g.zTop;
  }else{
    return "/";
  }
}

/*
** Return the name of the login cookie.
**
** The login cookie name is always of the form:  fossil-XXXXXXXXXXXXXXXX
** where the Xs are the first 16 characters of the login-group-code or
** of the project-code if we are not a member of any login-group.
*/
char *login_cookie_name(void){
  static char *zCookieName = 0;
  if( zCookieName==0 ){
    zCookieName = db_text(0,
       "SELECT 'fossil-' || substr(value,1,16)"
       "  FROM config"
       " WHERE name IN ('project-code','login-group-code')"
       " ORDER BY name /*sort*/"
    );
  }
  return zCookieName;
}

/*
** Redirect to the page specified by the "g" query parameter.
** Or if there is no "g" query parameter, redirect to the homepage.
*/
static void redirect_to_g(void){
  const char *zGoto = P("g");
  if( zGoto ){
    cgi_redirect(zGoto);
  }else{
    fossil_redirect_home();
  }
}

/*
** The IP address of the client is stored as part of login cookies.
** But some clients are behind firewalls that shift the IP address 
** with each HTTP request.  To allow such (broken) clients to log in, 
** extract just a prefix of the IP address.  
*/
static char *ipPrefix(const char *zIP){
  int i, j;
  static int ip_prefix_terms = -1;
  if( ip_prefix_terms<0 ){
    ip_prefix_terms = db_get_int("ip-prefix-terms",2);
  }
  if( ip_prefix_terms==0 ) return mprintf("0");
  for(i=j=0; zIP[i]; i++){
    if( zIP[i]=='.' ){
      j++;
      if( j==ip_prefix_terms ) break;
    }
  }
  return mprintf("%.*s", i, zIP);
}

/*
** Return an abbreviated project code.  The abbreviation is the first
** 16 characters of the project code.
**
** Memory is obtained from malloc.
*/
static char *abbreviated_project_code(const char *zFullCode){
  return mprintf("%.16s", zFullCode);
}


/*
** Check to see if the anonymous login is valid.  If it is valid, return
** the userid of the anonymous user.
**
** The zCS parameter is the "captcha seed" used for a specific
** anonymous login request.
*/
int login_is_valid_anonymous(
  const char *zUsername,  /* The username.  Must be "anonymous" */
  const char *zPassword,  /* The supplied password */
  const char *zCS         /* The captcha seed value */
){
  const char *zPw;        /* The correct password shown in the captcha */
  int uid;                /* The user ID of anonymous */

  if( zUsername==0 ) return 0;
  else if( zPassword==0 ) return 0;
  else if( zCS==0 ) return 0;
  else if( fossil_strcmp(zUsername,"anonymous")!=0 ) return 0;
  zPw = captcha_decode((unsigned int)atoi(zCS));
  if( fossil_stricmp(zPw, zPassword)!=0 ) return 0;
  uid = db_int(0, "SELECT uid FROM user WHERE login='anonymous'"
                  " AND length(pw)>0 AND length(cap)>0");
  return uid;
}

/*
** Make sure the accesslog table exists.  Create it if it does not
*/
void create_accesslog_table(void){
  db_multi_exec(
    "CREATE TABLE IF NOT EXISTS %s.accesslog("
    "  uname TEXT,"
    "  ipaddr TEXT,"
    "  success BOOLEAN,"
    "  mtime TIMESTAMP"
    ");", db_name("repository")
  );
}

/*
** Make a record of a login attempt, if login record keeping is enabled.
*/
static void record_login_attempt(
  const char *zUsername,     /* Name of user logging in */
  const char *zIpAddr,       /* IP address from which they logged in */
  int bSuccess               /* True if the attempt was a success */
){
  if( !db_get_boolean("access-log", 0) ) return;
  create_accesslog_table();
  db_multi_exec(
    "INSERT INTO accesslog(uname,ipaddr,success,mtime)"
    "VALUES(%Q,%Q,%d,julianday('now'));",
    zUsername, zIpAddr, bSuccess
  );
}

/*
** Searches for the user ID matching the given name and password.
** On success it returns a positive value. On error it returns 0.
** On serious (DB-level) error it will probably exit.
**
** zPassword may be either the plain-text form or the encrypted
** form of the user's password.
*/
int login_search_uid(char const *zUsername, char const *zPasswd){
  char * zSha1Pw = sha1_shared_secret(zPasswd, zUsername, 0);
  int const uid =
      db_int(0,
             "SELECT uid FROM user"
             " WHERE login=%Q"
             "   AND length(cap)>0 AND length(pw)>0"
             "   AND login NOT IN ('anonymous','nobody','developer','reader')"
             "   AND (pw=%Q OR pw=%Q)",
             zUsername, zPasswd, zSha1Pw
             );
  free(zSha1Pw);
  return uid;
}

/*
** Generates a login cookie value for a non-anonymous user.
**
** The zHash parameter must be a random value which must be
** subsequently stored in user.cookie for later validation.
**
** The returned memory should be free()d after use.
*/
char * login_gen_user_cookie_value(char const *zUsername, char const * zHash){
  char * zProjCode = db_get("project-code",NULL);
  char *zCode = abbreviated_project_code(zProjCode);
  free(zProjCode);
  assert((zUsername && *zUsername) && "Invalid user data.");
  return mprintf("%s/%z/%s", zHash, zCode, zUsername);
}

/*
** Generates a login cookie for NON-ANONYMOUS users.  Note that this
** function "could" figure out the uid by itself but it currently
** doesn't because the code which calls this already has the uid.
**
** This function also updates the user.cookie, user.ipaddr,
** and user.cexpire fields for the given user.
**
** If zDest is not NULL then the generated cookie is copied to
** *zDdest and ownership is transfered to the caller (who should
** eventually pass it to free()).
*/
void login_set_user_cookie(
  char const * zUsername, /* User's name */
  int uid,                /* User's ID */
  char ** zDest           /* Optional: store generated cookie value. */
){
  const char *zCookieName = login_cookie_name();
  const char *zExpire = db_get("cookie-expire","8766");
  int expires = atoi(zExpire)*3600;
  char *zHash;
  char *zCookie;
  char const *zIpAddr = PD("REMOTE_ADDR","nil"); /* IP address of user */
  char *zRemoteAddr = ipPrefix(zIpAddr);         /* Abbreviated IP address */

  assert((zUsername && *zUsername) && (uid > 0) && "Invalid user data.");
  zHash = db_text(0,
      "SELECT cookie FROM user"
      " WHERE uid=%d"
      "   AND ipaddr=%Q"
      "   AND cexpire>julianday('now')"
      "   AND length(cookie)>30",
      uid, zRemoteAddr);
  if( zHash==0 ) zHash = db_text(0, "SELECT hex(randomblob(25))");
  zCookie = login_gen_user_cookie_value(zUsername, zHash);
  cgi_set_cookie(zCookieName, zCookie, login_cookie_path(), expires);
  record_login_attempt(zUsername, zIpAddr, 1);
  db_multi_exec(
                "UPDATE user SET cookie=%Q, ipaddr=%Q, "
                "  cexpire=julianday('now')+%d/86400.0 WHERE uid=%d",
                zHash, zRemoteAddr, expires, uid
                );
  free(zRemoteAddr);
  free(zHash);
  if( zDest ){
    *zDest = zCookie;
  }else{
    free(zCookie);
  }
}

/* Sets a cookie for an anonymous user login, which looks like this:
**
**    HASH/TIME/anonymous
**
** Where HASH is the sha1sum of TIME/IPADDR/SECRET, in which IPADDR
** is the abbreviated IP address and SECRET is captcha-secret.
**
** If either zIpAddr or zRemoteAddr are NULL then REMOTE_ADDR
** is used.
**
** If zCookieDest is not NULL then the generated cookie is assigned to
** *zCookieDest and the caller must eventually free() it.
*/
void login_set_anon_cookie(char const * zIpAddr, char ** zCookieDest ){
  char const *zNow;            /* Current time (julian day number) */
  char *zCookie;               /* The login cookie */
  char const *zCookieName;     /* Name of the login cookie */
  Blob b;                      /* Blob used during cookie construction */
  char * zRemoteAddr;     /* Abbreviated IP address */
  if(!zIpAddr){
    zIpAddr = PD("REMOTE_ADDR","nil");
  }
  zRemoteAddr = ipPrefix(zIpAddr);
  zCookieName = login_cookie_name();
  zNow = db_text("0", "SELECT julianday('now')");
  assert( zCookieName && zRemoteAddr && zIpAddr && zNow );
  blob_init(&b, zNow, -1);
  blob_appendf(&b, "/%s/%s", zRemoteAddr, db_get("captcha-secret",""));
  sha1sum_blob(&b, &b);
  zCookie = mprintf("%s/%s/anonymous", blob_buffer(&b), zNow);
  blob_reset(&b);
  cgi_set_cookie(zCookieName, zCookie, login_cookie_path(), 6*3600);
  if( zCookieDest ){
    *zCookieDest = zCookie;
  }else{
    free(zCookie);
  }

}

/*
** "Unsets" the login cookie (insofar as cookies can be unset) and
** clears the current user's (g.userUid) login information from the
** user table. Sets: user.cookie, user.ipaddr, user.cexpire.
**
** We could/should arguably clear out g.userUid and g.perm here, but
** we don't currently do not.
**
** This is a no-op if g.userUid is 0.
*/
void login_clear_login_data(){
  if(!g.userUid){
    return;
  }else{
    char const * cookie = login_cookie_name(); 
    /* To logout, change the cookie value to an empty string */
    cgi_set_cookie(cookie, "",
                   login_cookie_path(), -86400);
    db_multi_exec("UPDATE user SET cookie=NULL, ipaddr=NULL, "
                  "  cexpire=0 WHERE uid=%d"
                  "  AND login NOT IN ('anonymous','nobody',"
                  "  'developer','reader')", g.userUid);
    cgi_replace_parameter(cookie, NULL)
      /* At the time of this writing, cgi_replace_parameter() was
      ** "NULL-value-safe", and i'm hoping the NULL doesn't cause any
      ** downstream problems here. We could alternately use "" here.
      */
      ;
  }
}

/*
** Return true if the prefix of zStr matches zPattern.  Return false if
** they are different.
**
** A lowercase character in zPattern will match either upper or lower
** case in zStr.  But an uppercase in zPattern will only match an
** uppercase in zStr.
*/
static int prefix_match(const char *zPattern, const char *zStr){
  int i;
  char c;
  for(i=0; (c = zPattern[i])!=0; i++){
    if( zStr[i]!=c && fossil_tolower(zStr[i])!=c ) return 0;
  }
  return 1;
}

/*
** Look at the HTTP_USER_AGENT parameter and try to determine if the user agent
** is a manually operated browser or a bot.  When in doubt, assume a bot.
** Return true if we believe the agent is a real person.
*/
static int isHuman(const char *zAgent){
  int i;
  if( zAgent==0 ) return 0;  /* If not UserAgent, the probably a bot */
  for(i=0; zAgent[i]; i++){
    if( prefix_match("bot", zAgent+i) ) return 0;
    if( prefix_match("spider", zAgent+i) ) return 0;
    if( prefix_match("crawl", zAgent+i) ) return 0;
    /* If a URI appears in the User-Agent, it is probably a bot */
    if( memcmp("http", zAgent+i,4)==0 ) return 0;
  }
  if( memcmp(zAgent, "Mozilla/", 8)==0 ){
    if( atoi(&zAgent[8])<4 ) return 0;  /* Many bots advertise as Mozilla/3 */
    if( strglob("*Firefox/[1-9]*", zAgent) ) return 1;
    if( strglob("*Chrome/[1-9]*", zAgent) ) return 1;
    if( strglob("*(compatible;?MSIE?[1-9]*", zAgent) ) return 1;
    if( strglob("*AppleWebKit/[1-9]*(KHTML*", zAgent) ) return 1;
    return 0;
  }
  if( memcmp(zAgent, "Opera/", 6)==0 ) return 1;
  if( memcmp(zAgent, "Safari/", 7)==0 ) return 1;
  if( memcmp(zAgent, "Lynx/", 5)==0 ) return 1;
  return 0;
}

/*
** COMMAND: test-ishuman
**
** Read lines of text from standard input.  Interpret each line of text
** as a User-Agent string from an HTTP header.  Label each line as HUMAN
** or ROBOT.
*/
void test_ishuman(void){
  char zLine[3000];
  while( fgets(zLine, sizeof(zLine), stdin) ){
    fossil_print("%s %s", isHuman(zLine) ? "HUMAN" : "ROBOT", zLine);
  }
}

/*
** SQL function for constant time comparison of two values.
** Sets result to 0 if two values are equal.
*/
static void constant_time_cmp_function(
 sqlite3_context *context,
 int argc,
 sqlite3_value **argv
){
  const unsigned char *buf1, *buf2;
  int len, i;
  unsigned char rc = 0;

  assert( argc==2 );
  len = sqlite3_value_bytes(argv[0]);
  if( len==0 || len!=sqlite3_value_bytes(argv[1]) ){
    rc = 1;
  }else{
    buf1 = sqlite3_value_text(argv[0]);
    buf2 = sqlite3_value_text(argv[1]);
    for( i=0; i<len; i++ ){
      rc = rc | (buf1[i] ^ buf2[i]);
    }
  }
  sqlite3_result_int(context, rc);
}

/*
** WEBPAGE: login
** WEBPAGE: logout
** WEBPAGE: my
**
** Generate the login page.
**
** There used to be a page named "my" that was designed to show information
** about a specific user.  The "my" page was linked from the "Logged in as USER"
** line on the title bar.  The "my" page was never completed so it is now
** removed.  Use this page as a placeholder in older installations.
*/
void login_page(void){
  const char *zUsername, *zPasswd;
  const char *zNew1, *zNew2;
  const char *zAnonPw = 0;
  int anonFlag;
  char *zErrMsg = "";
  int uid;                     /* User id loged in user */
  char *zSha1Pw;
  const char *zIpAddr;         /* IP address of requestor */

  login_check_credentials();
  sqlite3_create_function(g.db, "constant_time_cmp", 2, SQLITE_UTF8, 0,
		  constant_time_cmp_function, 0, 0);
  zUsername = P("u");
  zPasswd = P("p");
  anonFlag = P("anon")!=0;
  if( P("out")!=0 ){
    login_clear_login_data();
    redirect_to_g();
  }
  if( g.perm.Password && zPasswd
   && (zNew1 = P("n1"))!=0 && (zNew2 = P("n2"))!=0
  ){
    /* The user requests a password change */
    zSha1Pw = sha1_shared_secret(zPasswd, g.zLogin, 0);
    if( db_int(1, "SELECT 0 FROM user"
                  " WHERE uid=%d"
                  " AND (constant_time_cmp(pw,%Q)=0"
                  "      OR constant_time_cmp(pw,%Q)=0)", 
                  g.userUid, zSha1Pw, zPasswd) ){
      sleep(1);
      zErrMsg = 
         @ <p><span class="loginError">
         @ You entered an incorrect old password while attempting to change
         @ your password.  Your password is unchanged.
         @ </span></p>
      ;
    }else if( fossil_strcmp(zNew1,zNew2)!=0 ){
      zErrMsg = 
         @ <p><span class="loginError">
         @ The two copies of your new passwords do not match.
         @ Your password is unchanged.
         @ </span></p>
      ;
    }else{
      char *zNewPw = sha1_shared_secret(zNew1, g.zLogin, 0);
      char *zChngPw;
      char *zErr;
      db_multi_exec(
         "UPDATE user SET pw=%Q WHERE uid=%d", zNewPw, g.userUid
      );
      fossil_free(zNewPw);
      zChngPw = mprintf(
         "UPDATE user"
         "   SET pw=shared_secret(%Q,%Q,"
         "        (SELECT value FROM config WHERE name='project-code'))"
         " WHERE login=%Q",
         zNew1, g.zLogin, g.zLogin
      );
      if( login_group_sql(zChngPw, "<p>", "</p>\n", &zErr) ){
        zErrMsg = mprintf("<span class=\"loginError\">%s</span>", zErr);
        fossil_free(zErr);
      }else{
        redirect_to_g();
        return;
      }
    }
  }
  zIpAddr = PD("REMOTE_ADDR","nil");   /* Complete IP address for logging */
  uid = login_is_valid_anonymous(zUsername, zPasswd, P("cs"));
  if( uid>0 ){
    login_set_anon_cookie(zIpAddr, NULL);
    record_login_attempt("anonymous", zIpAddr, 1);
    redirect_to_g();
  }
  if( zUsername!=0 && zPasswd!=0 && zPasswd[0]!=0 ){
    /* Attempting to log in as a user other than anonymous.
    */
    uid = login_search_uid(zUsername, zPasswd);
    if( uid<=0 ){
      sleep(1);
      zErrMsg = 
         @ <p><span class="loginError">
         @ You entered an unknown user or an incorrect password.
         @ </span></p>
      ;
      record_login_attempt(zUsername, zIpAddr, 0);
    }else{
      /* Non-anonymous login is successful.  Set a cookie of the form:
      **
      **    HASH/PROJECT/LOGIN
      **
      ** where HASH is a random hex number, PROJECT is either project
      ** code prefix, and LOGIN is the user name.
      */
      login_set_user_cookie(zUsername, uid, NULL);
      redirect_to_g();
    }
  }
  style_header("Login/Logout");
  @ %s(zErrMsg)
  @ <form action="login" method="post">
  if( P("g") ){
    @ <input type="hidden" name="g" value="%h(P("g"))" />
  }
  @ <table class="login_out">
  @ <tr>
  @   <td class="login_out_label">User ID:</td>
  if( anonFlag ){
    @ <td><input type="text" id="u" name="u" value="anonymous" size="30" /></td>
  }else{
    @ <td><input type="text" id="u" name="u" value="" size="30" /></td>
  }
  @ </tr>
  @ <tr>
  @  <td class="login_out_label">Password:</td>
  @   <td><input type="password" id="p" name="p" value="" size="30" /></td>
  @ </tr>
  if( g.zLogin==0 ){
    zAnonPw = db_text(0, "SELECT pw FROM user"
                         " WHERE login='anonymous'"
                         "   AND cap!=''");
  }
  @ <tr>
  @   <td></td>
  @   <td><input type="submit" name="in" value="Login"
  @        onClick="chngAction(this.form)" /></td>
  @ </tr>
  @ </table>
  @ <script type="text/JavaScript">
  @   document.getElementById('u').focus()
  @   function chngAction(form){
  if( g.sslNotAvailable==0
   && memcmp(g.zBaseURL,"https:",6)!=0
   && db_get_boolean("https-login",0)
  ){
     char *zSSL = mprintf("https:%s", &g.zBaseURL[5]);
     @  if( form.u.value!="anonymous" ){
     @     form.action = "%h(zSSL)/login";
     @  }
  }
  @ }
  @ </script>
  if( g.zLogin==0 ){
    @ <p>Enter
  }else{
    @ <p>You are currently logged in as <b>%h(g.zLogin)</b></p>
    @ <p>To change your login to a different user, enter
  }
  @ your user-id and password at the left and press the
  @ "Login" button.  Your user name will be stored in a browser cookie.
  @ You must configure your web browser to accept cookies in order for
  @ the login to take.</p>
  if( db_get_boolean("self-register", 0) ){
    @ <p>If you do not have an account, you can 
    @ <a href="%s(g.zTop)/register?g=%T(P("G"))">create one</a>.
  }
  if( zAnonPw ){
    unsigned int uSeed = captcha_seed();
    char const *zDecoded = captcha_decode(uSeed);
    int bAutoCaptcha = db_get_boolean("auto-captcha", 1);
    char *zCaptcha = captcha_render(zDecoded);

    @ <p><input type="hidden" name="cs" value="%u(uSeed)" />
    @ Visitors may enter <b>anonymous</b> as the user-ID with
    @ the 8-character hexadecimal password shown below:</p>
    @ <div class="captcha"><table class="captcha"><tr><td><pre>
    @ %s(zCaptcha)
    @ </pre></td></tr></table>
    if( bAutoCaptcha ) {
        @ <input type="button" value="Fill out captcha"
        @  onclick="document.getElementById('u').value='anonymous';
        @           document.getElementById('p').value='%s(zDecoded)';" />
    }
    @ </div>
    free(zCaptcha);
  }
  if( g.zLogin ){
    @ <hr />
    @ <p>To log off the system (and delete your login cookie)
    @  press the following button:<br />
    @ <input type="submit" name="out" value="Logout" /></p>
  }
  @ </form>
  if( g.perm.Password ){
    @ <hr />
    @ <p>To change your password, enter your old password and your
    @ new password twice below then press the "Change Password"
    @ button.</p>
    @ <form action="login" method="post">
    @ <table>
    @ <tr><td class="login_out_label">Old Password:</td>
    @ <td><input type="password" name="p" size="30" /></td></tr>
    @ <tr><td class="login_out_label">New Password:</td>
    @ <td><input type="password" name="n1" size="30" /></td></tr>
    @ <tr><td class="login_out_label">Repeat New Password:</td>
    @ <td><input type="password" name="n2" size="30" /></td></tr>
    @ <tr><td></td>
    @ <td><input type="submit" value="Change Password" /></td></tr>
    @ </table>
    @ </form>
  }
  style_footer();
}

/*
** Attempt to find login credentials for user zLogin on a peer repository
** with project code zCode.  Transfer those credentials to the local 
** repository.
**
** Return true if a transfer was made and false if not.
*/
static int login_transfer_credentials(
  const char *zLogin,          /* Login we are looking for */
  const char *zCode,           /* Project code of peer repository */
  const char *zHash,           /* HASH from login cookie HASH/CODE/LOGIN */
  const char *zRemoteAddr      /* Request comes from here */
){
  sqlite3 *pOther = 0;         /* The other repository */
  sqlite3_stmt *pStmt;         /* Query against the other repository */
  char *zSQL;                  /* SQL of the query against other repo */
  char *zOtherRepo;            /* Filename of the other repository */
  int rc;                      /* Result code from SQLite library functions */
  int nXfer = 0;               /* Number of credentials transferred */

  zOtherRepo = db_text(0, 
       "SELECT value FROM config WHERE name='peer-repo-%q'",
       zCode
  );
  if( zOtherRepo==0 ) return 0;  /* No such peer repository */

  rc = sqlite3_open(zOtherRepo, &pOther);
  if( rc==SQLITE_OK ){
    sqlite3_create_function(pOther,"now",0,SQLITE_ANY,0,db_now_function,0,0);
    sqlite3_create_function(pOther, "constant_time_cmp", 2, SQLITE_UTF8, 0,
		  constant_time_cmp_function, 0, 0);
    sqlite3_busy_timeout(pOther, 5000);
    zSQL = mprintf(
      "SELECT cexpire FROM user"
      " WHERE login=%Q"
      "   AND ipaddr=%Q"
      "   AND length(cap)>0"
      "   AND length(pw)>0"
      "   AND cexpire>julianday('now')"
      "   AND constant_time_cmp(cookie,%Q)=0",
      zLogin, zRemoteAddr, zHash
    );
    pStmt = 0;
    rc = sqlite3_prepare_v2(pOther, zSQL, -1, &pStmt, 0);
    if( rc==SQLITE_OK && sqlite3_step(pStmt)==SQLITE_ROW ){
      db_multi_exec(
        "UPDATE user SET cookie=%Q, ipaddr=%Q, cexpire=%.17g"
        " WHERE login=%Q",
        zHash, zRemoteAddr,
        sqlite3_column_double(pStmt, 0), zLogin
      );
      nXfer++;
    }
    sqlite3_finalize(pStmt);
  }
  sqlite3_close(pOther);
  fossil_free(zOtherRepo);
  return nXfer;
}

/*
** Lookup the uid for a non-built-in user with zLogin and zCookie and
** zRemoteAddr.  Return 0 if not found.
**
** Note that this only searches for logged-in entries with matching
** zCookie (db: user.cookie) and zRemoteAddr (db: user.ipaddr)
** entries.
*/
static int login_find_user(
  const char *zLogin,            /* User name */
  const char *zCookie,           /* Login cookie value */
  const char *zRemoteAddr        /* Abbreviated IP address for valid login */
){
  int uid;
  if( fossil_strcmp(zLogin, "anonymous")==0 ) return 0;
  if( fossil_strcmp(zLogin, "nobody")==0 ) return 0;
  if( fossil_strcmp(zLogin, "developer")==0 ) return 0;
  if( fossil_strcmp(zLogin, "reader")==0 ) return 0;
  uid = db_int(0, 
    "SELECT uid FROM user"
    " WHERE login=%Q"
    "   AND ipaddr=%Q"
    "   AND cexpire>julianday('now')"
    "   AND length(cap)>0"
    "   AND length(pw)>0"
    "   AND constant_time_cmp(cookie,%Q)=0",
    zLogin, zRemoteAddr, zCookie
  );
  return uid;
}

/*
** This routine examines the login cookie to see if it exists and and
** is valid.  If the login cookie checks out, it then sets global
** variables appropriately.  Global variables set include g.userUid
** and g.zLogin and the g.perm family of permission booleans.
*/
void login_check_credentials(void){
  int uid = 0;                  /* User id */
  const char *zCookie;          /* Text of the login cookie */
  const char *zIpAddr;          /* Raw IP address of the requestor */
  char *zRemoteAddr;            /* Abbreviated IP address of the requestor */
  const char *zCap = 0;         /* Capability string */

  /* Only run this check once.  */
  if( g.userUid!=0 ) return;

  sqlite3_create_function(g.db, "constant_time_cmp", 2, SQLITE_UTF8, 0,
		  constant_time_cmp_function, 0, 0);

  /* If the HTTP connection is coming over 127.0.0.1 and if
  ** local login is disabled and if we are using HTTP and not HTTPS, 
  ** then there is no need to check user credentials.
  **
  ** This feature allows the "fossil ui" command to give the user
  ** full access rights without having to log in.
  */
  zRemoteAddr = ipPrefix(zIpAddr = PD("REMOTE_ADDR","nil"));
  if( fossil_strcmp(zIpAddr, "127.0.0.1")==0
   && g.useLocalauth
   && db_get_int("localauth",0)==0
   && P("HTTPS")==0
  ){
    uid = db_int(0, "SELECT uid FROM user WHERE cap LIKE '%%s%%'");
    g.zLogin = db_text("?", "SELECT login FROM user WHERE uid=%d", uid);
    zCap = "sx";
    g.noPswd = 1;
    sqlite3_snprintf(sizeof(g.zCsrfToken), g.zCsrfToken, "localhost");
  }

  /* Check the login cookie to see if it matches a known valid user.
  */
  if( uid==0 && (zCookie = P(login_cookie_name()))!=0 ){
    /* Parse the cookie value up into HASH/ARG/USER */
    char *zHash = fossil_strdup(zCookie);
    char *zArg = 0;
    char *zUser = 0;
    int i, c;
    for(i=0; (c = zHash[i])!=0; i++){
      if( c=='/' ){
        zHash[i++] = 0;
        if( zArg==0 ){
          zArg = &zHash[i];
        }else{
          zUser = &zHash[i];
          break;
        }
      }
    }
    if( zUser==0 ){
      /* Invalid cookie */
    }else if( fossil_strcmp(zUser, "anonymous")==0 ){
      /* Cookies of the form "HASH/TIME/anonymous".  The TIME must not be
      ** too old and the sha1 hash of TIME/IPADDR/SECRET must match HASH.
      ** SECRET is the "captcha-secret" value in the repository.
      */
      double rTime = atof(zArg);
      Blob b;
      blob_zero(&b);
      blob_appendf(&b, "%s/%s/%s", 
                   zArg, zRemoteAddr, db_get("captcha-secret",""));
      sha1sum_blob(&b, &b);
      if( fossil_strcmp(zHash, blob_str(&b))==0 ){
        uid = db_int(0, 
            "SELECT uid FROM user WHERE login='anonymous'"
            " AND length(cap)>0"
            " AND length(pw)>0"
            " AND %.17g+0.25>julianday('now')",
            rTime
        );
      }
      blob_reset(&b);
    }else{
      /* Cookies of the form "HASH/CODE/USER".  Search first in the
      ** local user table, then the user table for project CODE if we
      ** are part of a login-group.
      */
      uid = login_find_user(zUser, zHash, zRemoteAddr);
      if( uid==0 && login_transfer_credentials(zUser,zArg,zHash,zRemoteAddr) ){
        uid = login_find_user(zUser, zHash, zRemoteAddr);
        if( uid ) record_login_attempt(zUser, zIpAddr, 1);
      }
    }
    sqlite3_snprintf(sizeof(g.zCsrfToken), g.zCsrfToken, "%.10s", zHash);
  }

  /* If no user found and the REMOTE_USER environment variable is set,
  ** then accept the value of REMOTE_USER as the user.
  */
  if( uid==0 ){
    const char *zRemoteUser = P("REMOTE_USER");
    if( zRemoteUser && db_get_boolean("remote_user_ok",0) ){
      uid = db_int(0, "SELECT uid FROM user WHERE login=%Q"
                      " AND length(cap)>0 AND length(pw)>0", zRemoteUser);
    }
  }

  /* If no user found yet, try to log in as "nobody" */
  if( uid==0 ){
    uid = db_int(0, "SELECT uid FROM user WHERE login='nobody'");
    if( uid==0 ){
      /* If there is no user "nobody", then make one up - with no privileges */
      uid = -1;
      zCap = "";
    }
    sqlite3_snprintf(sizeof(g.zCsrfToken), g.zCsrfToken, "none");
  }

  /* At this point, we know that uid!=0.  Find the privileges associated
  ** with user uid.
  */
  assert( uid!=0 );
  if( zCap==0 ){
    Stmt s;
    db_prepare(&s, "SELECT login, cap FROM user WHERE uid=%d", uid);
    if( db_step(&s)==SQLITE_ROW ){
      g.zLogin = db_column_malloc(&s, 0);
      zCap = db_column_malloc(&s, 1);
    }
    db_finalize(&s);
    if( zCap==0 ){
      zCap = "";
    }
  }
  if( g.fHttpTrace && g.zLogin ){
    fprintf(stderr, "# login: [%s] with capabilities [%s]\n", g.zLogin, zCap);
  }

  /* Set the global variables recording the userid and login.  The
  ** "nobody" user is a special case in that g.zLogin==0.
  */
  g.userUid = uid;
  if( fossil_strcmp(g.zLogin,"nobody")==0 ){
    g.zLogin = 0;
  }

  /* Set the capabilities */
  login_replace_capabilities(zCap, 0);
  login_set_anon_nobody_capabilities();
  if( zCap[0] && !g.perm.History && db_get_boolean("auto-enable-hyperlinks",1)
      && isHuman(P("HTTP_USER_AGENT")) ){
    g.perm.History = 1;
  }
}

/*
** Memory of settings
*/
static int login_anon_once = 1;

/*
** Add the default privileges of users "nobody" and "anonymous" as appropriate
** for the user g.zLogin.
*/
void login_set_anon_nobody_capabilities(void){
  if( g.zLogin && login_anon_once ){
    const char *zCap;
    /* All logged-in users inherit privileges from "nobody" */
    zCap = db_text("", "SELECT cap FROM user WHERE login = 'nobody'");
    login_set_capabilities(zCap, 0);
    if( fossil_strcmp(g.zLogin, "nobody")!=0 ){
      /* All logged-in users inherit privileges from "anonymous" */
      zCap = db_text("", "SELECT cap FROM user WHERE login = 'anonymous'");
      login_set_capabilities(zCap, 0);
    }
    login_anon_once = 0;
  }
}

/*
** Flags passed into the 2nd argument of login_set/replace_capabilities().
*/
#if INTERFACE
#define LOGIN_IGNORE_U   0x01         /* Ignore "u" */
#define LOGIN_IGNORE_V   0x01         /* Ignore "v" */
#endif

/*
** Adds all capability flags in zCap to g.perm.
*/
void login_set_capabilities(const char *zCap, unsigned flags){
  int i;
  if(NULL==zCap){
    return;
  }
  for(i=0; zCap[i]; i++){
    switch( zCap[i] ){
      case 's':   g.perm.Setup = 1;  /* Fall thru into Admin */
      case 'a':   g.perm.Admin = g.perm.RdTkt = g.perm.WrTkt = g.perm.Zip =
                              g.perm.RdWiki = g.perm.WrWiki = g.perm.NewWiki =
                              g.perm.ApndWiki = g.perm.History = g.perm.Clone = 
                              g.perm.NewTkt = g.perm.Password = g.perm.RdAddr =
                              g.perm.TktFmt = g.perm.Attach = g.perm.ApndTkt = 1;
                              /* Fall thru into Read/Write */
      case 'i':   g.perm.Read = g.perm.Write = 1;                     break;
      case 'o':   g.perm.Read = 1;                                 break;
      case 'z':   g.perm.Zip = 1;                                  break;

      case 'd':   g.perm.Delete = 1;                               break;
      case 'h':   g.perm.History = 1;                              break;
      case 'g':   g.perm.Clone = 1;                                break;
      case 'p':   g.perm.Password = 1;                             break;

      case 'j':   g.perm.RdWiki = 1;                               break;
      case 'k':   g.perm.WrWiki = g.perm.RdWiki = g.perm.ApndWiki =1;    break;
      case 'm':   g.perm.ApndWiki = 1;                             break;
      case 'f':   g.perm.NewWiki = 1;                              break;

      case 'e':   g.perm.RdAddr = 1;                               break;
      case 'r':   g.perm.RdTkt = 1;                                break;
      case 'n':   g.perm.NewTkt = 1;                               break;
      case 'w':   g.perm.WrTkt = g.perm.RdTkt = g.perm.NewTkt = 
                  g.perm.ApndTkt = 1;                              break;
      case 'c':   g.perm.ApndTkt = 1;                              break;
      case 't':   g.perm.TktFmt = 1;                               break;
      case 'b':   g.perm.Attach = 1;                               break;
      case 'x':   g.perm.Private = 1;                              break;

      /* The "u" privileges is a little different.  It recursively 
      ** inherits all privileges of the user named "reader" */
      case 'u': {
        if( (flags & LOGIN_IGNORE_U)==0 ){
          const char *zUser;
          zUser = db_text("", "SELECT cap FROM user WHERE login='reader'");
          login_set_capabilities(zUser, flags | LOGIN_IGNORE_U);
        }
        break;
      }

      /* The "v" privileges is a little different.  It recursively 
      ** inherits all privileges of the user named "developer" */
      case 'v': {
        if( (flags & LOGIN_IGNORE_V)==0 ){
          const char *zDev;
          zDev = db_text("", "SELECT cap FROM user WHERE login='developer'");
          login_set_capabilities(zDev, flags | LOGIN_IGNORE_V);
        }
        break;
      }
    }
  }
}

/*
** Zeroes out g.perm and calls login_set_capabilities(zCap,flags).
*/
void login_replace_capabilities(const char *zCap, unsigned flags){
  memset(&g.perm, 0, sizeof(g.perm));
  login_set_capabilities(zCap, flags);
}

/*
** If the current login lacks any of the capabilities listed in
** the input, then return 0.  If all capabilities are present, then
** return 1.
*/
int login_has_capability(const char *zCap, int nCap){
  int i;
  int rc = 1;
  if( nCap<0 ) nCap = strlen(zCap);
  for(i=0; i<nCap && rc && zCap[i]; i++){
    switch( zCap[i] ){
      case 'a':  rc = g.perm.Admin;     break;
      case 'b':  rc = g.perm.Attach;    break;
      case 'c':  rc = g.perm.ApndTkt;   break;
      case 'd':  rc = g.perm.Delete;    break;
      case 'e':  rc = g.perm.RdAddr;    break;
      case 'f':  rc = g.perm.NewWiki;   break;
      case 'g':  rc = g.perm.Clone;     break;
      case 'h':  rc = g.perm.History;   break;
      case 'i':  rc = g.perm.Write;     break;
      case 'j':  rc = g.perm.RdWiki;    break;
      case 'k':  rc = g.perm.WrWiki;    break;
      /* case 'l': */
      case 'm':  rc = g.perm.ApndWiki;  break;
      case 'n':  rc = g.perm.NewTkt;    break;
      case 'o':  rc = g.perm.Read;      break;
      case 'p':  rc = g.perm.Password;  break;
      /* case 'q': */
      case 'r':  rc = g.perm.RdTkt;     break;
      case 's':  rc = g.perm.Setup;     break;
      case 't':  rc = g.perm.TktFmt;    break;
      /* case 'u': READER    */
      /* case 'v': DEVELOPER */
      case 'w':  rc = g.perm.WrTkt;     break;
      case 'x':  rc = g.perm.Private;   break;
      /* case 'y': */
      case 'z':  rc = g.perm.Zip;       break;
      default:   rc = 0;             break;
    }
  }
  return rc;
}

/*
** Change the login to zUser.
*/
void login_as_user(const char *zUser){
  char *zCap = "";   /* New capabilities */

  /* Turn off all capabilities from prior logins */
  memset( &g.perm, 0, sizeof(g.perm) );

  /* Set the global variables recording the userid and login.  The
  ** "nobody" user is a special case in that g.zLogin==0.
  */
  g.userUid = db_int(0, "SELECT uid FROM user WHERE login=%Q", zUser);
  if( g.userUid==0 ){
    zUser = 0;
    g.userUid = db_int(0, "SELECT uid FROM user WHERE login='nobody'");
  }
  if( g.userUid ){
    zCap = db_text("", "SELECT cap FROM user WHERE uid=%d", g.userUid);
  }
  if( fossil_strcmp(zUser,"nobody")==0 ) zUser = 0;
  g.zLogin = fossil_strdup(zUser);

  /* Set the capabilities */
  login_set_capabilities(zCap, 0);
  login_anon_once = 1;
  login_set_anon_nobody_capabilities();
}

/*
** Call this routine when the credential check fails.  It causes
** a redirect to the "login" page.
*/
void login_needed(void){
#ifdef FOSSIL_ENABLE_JSON
  if(g.json.isJsonMode){
    json_err( FSL_JSON_E_DENIED, NULL, 1 );
    fossil_exit(0);
    /* NOTREACHED */
    assert(0);
  }else
#endif /* FOSSIL_ENABLE_JSON */
  {
    const char *zUrl = PD("REQUEST_URI", "index");
    cgi_redirect(mprintf("login?g=%T", zUrl));
    /* NOTREACHED */
    assert(0);
  }
}

/*
** Call this routine if the user lacks okHistory permission.  If
** the anonymous user has okHistory permission, then paint a mesage
** to inform the user that much more information is available by
** logging in as anonymous.
*/
void login_anonymous_available(void){
  if( !g.perm.History &&
      db_exists("SELECT 1 FROM user"
                " WHERE login='anonymous'"
                "   AND cap LIKE '%%h%%'") ){
    const char *zUrl = PD("REQUEST_URI", "index");
    @ <p>Many <span class="disabled">hyperlinks are disabled.</span><br />
    @ Use <a href="%s(g.zTop)/login?anon=1&amp;g=%T(zUrl)">anonymous login</a>
    @ to enable hyperlinks.</p>
  }
}

/*
** While rendering a form, call this routine to add the Anti-CSRF token
** as a hidden element of the form.
*/
void login_insert_csrf_secret(void){
  @ <input type="hidden" name="csrf" value="%s(g.zCsrfToken)" />
}

/*
** Before using the results of a form, first call this routine to verify
** that ths Anti-CSRF token is present and is valid.  If the Anti-CSRF token
** is missing or is incorrect, that indicates a cross-site scripting attach
** so emits an error message and abort.
*/
void login_verify_csrf_secret(void){
  if( g.okCsrf ) return;
  if( fossil_strcmp(P("csrf"), g.zCsrfToken)==0 ){
    g.okCsrf = 1;
    return;
  }
  fossil_fatal("Cross-site request forgery attempt");
}

/*
** WEBPAGE: register
**
** Generate the register page.
**
*/
void register_page(void){
  const char *zUsername, *zPasswd, *zConfirm, *zContact, *zCS, *zPw, *zCap;
  unsigned int uSeed;
  char const *zDecoded;
  char *zCaptcha;
  if( !db_get_boolean("self-register", 0) ){
    style_header("Registration not possible");
    @ <p>This project does not allow user self-registration. Please contact the
    @ project administrator to obtain an account.</p>
    style_footer();
    return;
  }

  style_header("Register");
  zUsername = P("u");
  zPasswd = P("p");
  zConfirm = P("cp");
  zContact = P("c");
  zCap = P("cap");
  zCS = P("cs"); /* Captcha Secret */

  /* Try to make any sense from user input. */
  if( P("new") ){
    if( zCS==0 ) fossil_redirect_home();  /* Forged request */
    zPw = captcha_decode((unsigned int)atoi(zCS));
    if( !(zUsername && zPasswd && zConfirm && zContact) ){
      @ <p><span class="loginError">
      @ All fields are obligatory.
      @ </span></p>
    }else if( strlen(zPasswd) < 6){
      @ <p><span class="loginError">
      @ Password too weak.
      @ </span></p>
    }else if( fossil_strcmp(zPasswd,zConfirm)!=0 ){
      @ <p><span class="loginError">
      @ The two copies of your new passwords do not match.
      @ </span></p>
    }else if( fossil_stricmp(zPw, zCap)!=0 ){
      @ <p><span class="loginError">
      @ Captcha text invalid.
      @ </span></p>
    }else{
      /* This almost is stupid copy-paste of code from user.c:user_cmd(). */
      Blob passwd, login, caps, contact;

      blob_init(&login, zUsername, -1);
      blob_init(&contact, zContact, -1);
      blob_init(&caps, db_get("default-perms", "u"), -1);
      blob_init(&passwd, zPasswd, -1);

      if( db_exists("SELECT 1 FROM user WHERE login=%B", &login) ){
        /* Here lies the reason I don't use zErrMsg - it would not substitute
         * this %s(zUsername), or at least I don't know how to force it to.*/
        @ <p><span class="loginError">
        @ %s(zUsername) already exists.
        @ </span></p>
      }else{
        char *zPw = sha1_shared_secret(blob_str(&passwd), blob_str(&login), 0);
        int uid;
        char *zCookie;
        const char *zCookieName;
        const char *zExpire;
        int expires;
        const char *zIpAddr;
        db_multi_exec(
            "INSERT INTO user(login,pw,cap,info)"
            "VALUES(%B,%Q,%B,%B)",
            &login, zPw, &caps, &contact
            );
        free(zPw);

        /* The user is registered, now just log him in. */
        uid = db_int(0, "SELECT uid FROM user WHERE login=%Q", zUsername);
        zCookieName = login_cookie_name();
        zExpire = db_get("cookie-expire","8766");
        expires = atoi(zExpire)*3600;
        zIpAddr = PD("REMOTE_ADDR","nil");

        zCookie = db_text(0, "SELECT '%d/' || hex(randomblob(25))", uid);
        cgi_set_cookie(zCookieName, zCookie, login_cookie_path(), expires);
        record_login_attempt(zUsername, zIpAddr, 1);
        db_multi_exec(
            "UPDATE user SET cookie=%Q, ipaddr=%Q, "
            "  cexpire=julianday('now')+%d/86400.0 WHERE uid=%d",
            zCookie, ipPrefix(zIpAddr), expires, uid
        );
        redirect_to_g();

      }
    }
  }

  /* Prepare the captcha. */
  uSeed = captcha_seed();
  zDecoded = captcha_decode(uSeed);
  zCaptcha = captcha_render(zDecoded);

  /* Print out the registration form. */
  @ <form action="register" method="post">
  if( P("g") ){
    @ <input type="hidden" name="g" value="%h(P("g"))" />
  }
  @ <p><input type="hidden" name="cs" value="%u(uSeed)" />
  @ <table class="login_out">
  @ <tr>
  @   <td class="login_out_label" align="right">User ID:</td>
  @   <td><input type="text" id="u" name="u" value="" size="30" /></td>
  @ </tr>
  @ <tr>
  @   <td class="login_out_label" align="right">Password:</td>
  @   <td><input type="password" id="p" name="p" value="" size="30" /></td>
  @ </tr>
  @ <tr>
  @   <td class="login_out_label" align="right">Confirm password:</td>
  @   <td><input type="password" id="cp" name="cp" value="" size="30" /></td>
  @ </tr>
  @ <tr>
  @   <td class="login_out_label" align="right">Contact info:</td>
  @   <td><input type="text" id="c" name="c" value="" size="30" /></td>
  @ </tr>
  @ <tr>
  @   <td class="login_out_label" align="right">Captcha text (below):</td>
  @   <td><input type="text" id="cap" name="cap" value="" size="30" /></td>
  @ </tr>
  @ <tr><td></td>
  @ <td><input type="submit" name="new" value="Register" /></td></tr>
  @ </table>
  @ <div class="captcha"><table class="captcha"><tr><td><pre>
  @ %s(zCaptcha)
  @ </pre></td></tr></table>
  @ </form>
  style_footer();

  free(zCaptcha);
}

/*
** Run SQL on the repository database for every repository in our
** login group.  The SQL is run in a separate database connection.
**
** Any members of the login group whose repository database file
** cannot be found is silently removed from the group.
**
** Error messages accumulate and are returned in *pzErrorMsg.  The
** memory used to hold these messages should be freed using
** fossil_free() if one desired to avoid a memory leak.  The
** zPrefix and zSuffix strings surround each error message.
**
** Return the number of errors.
*/
int login_group_sql(
  const char *zSql,        /* The SQL to run */
  const char *zPrefix,     /* Prefix to each error message */
  const char *zSuffix,     /* Suffix to each error message */
  char **pzErrorMsg        /* Write error message here, if not NULL */
){
  sqlite3 *pPeer;          /* Connection to another database */
  int nErr = 0;            /* Number of errors seen so far */
  int rc;                  /* Result code from subroutine calls */
  char *zErr;              /* SQLite error text */
  char *zSelfCode;         /* Project code for ourself */
  Blob err;                /* Accumulate errors here */
  Stmt q;                  /* Query of all peer-* entries in CONFIG */

  if( zPrefix==0 ) zPrefix = "";
  if( zSuffix==0 ) zSuffix = "";
  if( pzErrorMsg ) *pzErrorMsg = 0;
  zSelfCode = abbreviated_project_code(db_get("project-code", "x"));
  blob_zero(&err);
  db_prepare(&q, 
    "SELECT name, value FROM config"
    " WHERE name GLOB 'peer-repo-*'"
    "   AND name <> 'peer-repo-%q'"
    " ORDER BY +value",
    zSelfCode
  );
  while( db_step(&q)==SQLITE_ROW ){
    const char *zRepoName = db_column_text(&q, 1);
    if( file_size(zRepoName)<0 ){
      /* Silently remove non-existant repositories from the login group. */
      const char *zLabel = db_column_text(&q, 0);
      db_multi_exec(
         "DELETE FROM config WHERE name GLOB 'peer-*-%q'",
         &zLabel[10]
      );
      continue;
    }
    rc = sqlite3_open_v2(zRepoName, &pPeer, SQLITE_OPEN_READWRITE, 0);
    if( rc!=SQLITE_OK ){
      blob_appendf(&err, "%s%s: %s%s", zPrefix, zRepoName,
                   sqlite3_errmsg(pPeer), zSuffix);
      nErr++;
      sqlite3_close(pPeer);
      continue;
    }
    sqlite3_create_function(pPeer, "shared_secret", 3, SQLITE_UTF8,
                            0, sha1_shared_secret_sql_function, 0, 0);
    sqlite3_create_function(pPeer, "now", 0,SQLITE_ANY,0,db_now_function,0,0);
    sqlite3_busy_timeout(pPeer, 5000);
    zErr = 0;
    rc = sqlite3_exec(pPeer, zSql, 0, 0, &zErr);
    if( zErr ){
      blob_appendf(&err, "%s%s: %s%s", zPrefix, zRepoName, zErr, zSuffix);
      sqlite3_free(zErr);
      nErr++;
    }else if( rc!=SQLITE_OK ){
      blob_appendf(&err, "%s%s: %s%s", zPrefix, zRepoName,
                   sqlite3_errmsg(pPeer), zSuffix);
      nErr++;
    }
    sqlite3_close(pPeer);
  }
  db_finalize(&q);
  if( pzErrorMsg && blob_size(&err)>0 ){
    *pzErrorMsg = fossil_strdup(blob_str(&err));
  }
  blob_reset(&err);
  fossil_free(zSelfCode);
  return nErr;
}

/*
** Attempt to join a login-group.
**
** If problems arise, leave an error message in *pzErrMsg.
*/
void login_group_join(
  const char *zRepo,         /* Repository file in the login group */
  const char *zLogin,        /* Login name for the other repo */
  const char *zPassword,     /* Password to prove we are authorized to join */
  const char *zNewName,      /* Name of new login group if making a new one */
  char **pzErrMsg            /* Leave an error message here */
){
  Blob fullName;             /* Blob for finding full pathnames */
  sqlite3 *pOther;           /* The other repository */
  int rc;                    /* Return code from sqlite3 functions */
  char *zOtherProjCode;      /* Project code for pOther */
  char *zPwHash;             /* Password hash on pOther */
  char *zSelfRepo;           /* Name of our repository */
  char *zSelfLabel;          /* Project-name for our repository */
  char *zSelfProjCode;       /* Our project-code */
  char *zSql;                /* SQL to run on all peers */
  const char *zSelf;         /* The ATTACH name of our repository */

  *pzErrMsg = 0;   /* Default to no errors */
  zSelf = db_name("repository");

  /* Get the full pathname of the other repository */  
  file_canonical_name(zRepo, &fullName);
  zRepo = mprintf(blob_str(&fullName));
  blob_reset(&fullName);

  /* Get the full pathname for our repository.  Also the project code
  ** and project name for ourself. */
  file_canonical_name(g.zRepositoryName, &fullName);
  zSelfRepo = mprintf(blob_str(&fullName));
  blob_reset(&fullName);
  zSelfProjCode = db_get("project-code", "unknown");
  zSelfLabel = db_get("project-name", 0);
  if( zSelfLabel==0 ){
    zSelfLabel = zSelfProjCode;
  }

  /* Make sure we are not trying to join ourselves */
  if( fossil_strcmp(zRepo, zSelfRepo)==0 ){
    *pzErrMsg = mprintf("The \"other\" repository is the same as this one.");
    return;
  }

  /* Make sure the other repository is a valid Fossil database */
  if( file_size(zRepo)<0 ){
    *pzErrMsg = mprintf("repository file \"%s\" does not exist", zRepo);
    return;
  }
  rc = sqlite3_open(zRepo, &pOther);
  if( rc!=SQLITE_OK ){
    *pzErrMsg = mprintf(sqlite3_errmsg(pOther));
  }else{
    rc = sqlite3_exec(pOther, "SELECT count(*) FROM user", 0, 0, pzErrMsg);
  }
  sqlite3_close(pOther);
  if( rc ) return;

  /* Attach the other respository.  Make sure the username/password is
  ** valid and has Setup permission.
  */
  db_multi_exec("ATTACH %Q AS other", zRepo);
  zOtherProjCode = db_text("x", "SELECT value FROM other.config"
                                " WHERE name='project-code'");
  zPwHash = sha1_shared_secret(zPassword, zLogin, zOtherProjCode);
  if( !db_exists(
    "SELECT 1 FROM other.user"
    " WHERE login=%Q AND cap GLOB '*s*'"
    "   AND (pw=%Q OR pw=%Q)",
    zLogin, zPassword, zPwHash)
  ){
    db_multi_exec("DETACH other");
    *pzErrMsg = "The supplied username/password does not correspond to a"
                " user Setup permission on the other repository.";
    return;
  }

  /* Create all the necessary CONFIG table entries on both the
  ** other repository and on our own repository.
  */
  zSelfProjCode = abbreviated_project_code(zSelfProjCode);
  zOtherProjCode = abbreviated_project_code(zOtherProjCode);
  db_begin_transaction();
  db_multi_exec(
    "DELETE FROM %s.config WHERE name GLOB 'peer-*';"
    "INSERT INTO %s.config(name,value) VALUES('peer-repo-%s',%Q);"
    "INSERT INTO %s.config(name,value) "
    "  SELECT 'peer-name-%q', value FROM other.config"
    "   WHERE name='project-name';",
    zSelf,
    zSelf, zOtherProjCode, zRepo,
    zSelf, zOtherProjCode
  );
  db_multi_exec(
    "INSERT OR IGNORE INTO other.config(name,value)"
    " VALUES('login-group-name',%Q);"
    "INSERT OR IGNORE INTO other.config(name,value)"
    " VALUES('login-group-code',lower(hex(randomblob(8))));",
    zNewName
  );
  db_multi_exec(
    "REPLACE INTO %s.config(name,value)"
    "  SELECT name, value FROM other.config"
    "   WHERE name GLOB 'peer-*' OR name GLOB 'login-group-*'",
    zSelf
  );
  db_end_transaction(0);
  db_multi_exec("DETACH other");

  /* Propagate the changes to all other members of the login-group */
  zSql = mprintf(
    "BEGIN;"
    "REPLACE INTO config(name,value,mtime) VALUES('peer-name-%q',%Q,now());"
    "REPLACE INTO config(name,value,mtime) VALUES('peer-repo-%q',%Q,now());"
    "COMMIT;",
    zSelfProjCode, zSelfLabel, zSelfProjCode, zSelfRepo
  );
  login_group_sql(zSql, "<li> ", "</li>", pzErrMsg);
  fossil_free(zSql);
}

/*
** Leave the login group that we are currently part of.
*/
void login_group_leave(char **pzErrMsg){
  char *zProjCode;
  char *zSql;

  *pzErrMsg = 0;
  zProjCode = abbreviated_project_code(db_get("project-code","x"));
  zSql = mprintf(
    "DELETE FROM config WHERE name GLOB 'peer-*-%q';"
    "DELETE FROM config"
    " WHERE name='login-group-name'"
    "   AND (SELECT count(*) FROM config WHERE name GLOB 'peer-*')==0;",
    zProjCode
  );
  fossil_free(zProjCode);
  login_group_sql(zSql, "<li> ", "</li>", pzErrMsg);
  fossil_free(zSql);
  db_multi_exec(
    "DELETE FROM config "
    " WHERE name GLOB 'peer-*'"
    "    OR name GLOB 'login-group-*';"
  );
}

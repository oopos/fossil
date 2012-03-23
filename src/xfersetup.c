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
** This file contains code to implement the transfer configuration
** setup screens.
*/
#include "config.h"
#include "xfersetup.h"
#include <assert.h>

/*
** Main sub-menu for configuring the transfer system.
** WEBPAGE: xfersetup
*/
void xfersetup_page(void){
  login_check_credentials();
  if( !g.perm.Setup ){
    login_needed();
  }

  style_header("Transfer Setup");
  @ <table border="0" cellspacing="20">
  setup_menu_entry("Common", "xfersetup_com",
    "Common TH1 code run before all transfer request processing.");
  setup_menu_entry("Push", "xfersetup_push",
    "Specific TH1 code to run after \"push\" transfer requests.");
  @ </table>
  style_footer();
}

/*
** Common implementation for the transfer setup editor pages.
*/
static void xfersetup_generic(
  const char *zTitle,           /* Page title */
  const char *zDbField,         /* Configuration field being edited */
  const char *zDfltValue,       /* Default text value */
  const char *zDesc,            /* Description of this field */
  char *(*xText)(const char*),  /* Validitity test or NULL */
  void (*xRebuild)(void),       /* Run after successulf update */
  int height                    /* Height of the edit box */
){
  const char *z;
  int isSubmit;

  login_check_credentials();
  if( !g.perm.Setup ){
    login_needed();
  }
  if( P("setup") ){
    cgi_redirect("xfersetup");
  }
  isSubmit = P("submit")!=0;
  z = P("x");
  if( z==0 ){
    z = db_get(zDbField, (char*)zDfltValue);
  }
  style_header("Edit %s", zTitle);
  if( P("clear")!=0 ){
    login_verify_csrf_secret();
    db_unset(zDbField, 0);
    if( xRebuild ) xRebuild();
    z = zDfltValue;
  }else if( isSubmit ){
    char *zErr = 0;
    login_verify_csrf_secret();
    if( xText && (zErr = xText(z))!=0 ){
      @ <p class="xfersetupError">ERROR: %h(zErr)</p>
    }else{
      db_set(zDbField, z, 0);
      if( xRebuild ) xRebuild();
      cgi_redirect("xfersetup");
    }
  }
  @ <form action="%s(g.zTop)/%s(g.zPath)" method="post"><div>
  login_insert_csrf_secret();
  @ <p>%s(zDesc)</p>
  @ <textarea name="x" rows="%d(height)" cols="80">%h(z)</textarea>
  @ <blockquote><p>
  @ <input type="submit" name="submit" value="Apply Changes" />
  @ <input type="submit" name="clear" value="Revert To Default" />
  @ <input type="submit" name="setup" value="Cancel" />
  @ </p></blockquote>
  @ </div></form>
  @ <hr />
  if ( zDfltValue ){
    @ <h2>Default %s(zTitle)</h2>
    @ <blockquote><pre>
    @ %h(zDfltValue)
    @ </pre></blockquote>
  }
  style_footer();
}

static const char *zDefaultXferCommon = 0;

/*
** WEBPAGE: xfersetup_com
*/
void xfersetup_com_page(void){
  static const char zDesc[] =
  @ Enter TH1 script that initializes variables prior to running
  @ any of the transfer request scripts.
  ;
  xfersetup_generic(
    "Transfer Common Script",
    "xfer-common-script",
    zDefaultXferCommon,
    zDesc,
    0,
    0,
    30
  );
}

static const char *zDefaultXferPush = 0;

/*
** WEBPAGE: xfersetup_push
*/
void xfersetup_push_page(void){
  static const char zDesc[] =
  @ Enter TH1 script that runs after processing "push" transfer requests.
  ;
  xfersetup_generic(
    "Transfer Push Script",
    "xfer-push-script",
    zDefaultXferPush,
    zDesc,
    0,
    0,
    30
  );
}

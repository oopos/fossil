/*
** Copyright (c) 2009 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public
** License version 2 as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** General Public License for more details.
** 
** You should have received a copy of the GNU General Public
** License along with this library; if not, write to the
** Free Software Foundation, Inc., 59 Temple Place - Suite 330,
** Boston, MA  02111-1307, USA.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This file manages low-level SSL communications.
**
** This file implements a singleton.  A single SSL connection may be active
** at a time.  State information is stored in static variables.  The identity
** of the server is held in global variables that are set by url_parse().
**
** SSL support is abstracted out into this module because Fossil can
** be compiled without SSL support (which requires OpenSSL library)
*/

#include "config.h"

#ifdef FOSSIL_ENABLE_SSL

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "http_ssl.h"
#include <assert.h>
#include <sys/types.h>

/*
** There can only be a single OpenSSL IO connection open at a time.
** State information about that IO is stored in the following
** local variables:
*/
static int sslIsInit = 0;    /* True after global initialization */
static BIO *iBio;            /* OpenSSL I/O abstraction */
static char *sslErrMsg = 0;  /* Text of most recent OpenSSL error */
static SSL_CTX *sslCtx;      /* SSL context */
static SSL *ssl;


/*
** Clear the SSL error message
*/
static void ssl_clear_errmsg(void){
  free(sslErrMsg);
  sslErrMsg = 0;
}

/*
** Set the SSL error message.
*/
void ssl_set_errmsg(char *zFormat, ...){
  va_list ap;
  ssl_clear_errmsg();
  va_start(ap, zFormat);
  sslErrMsg = vmprintf(zFormat, ap);
  va_end(ap);
}

/*
** Return the current SSL error message
*/
const char *ssl_errmsg(void){
  return sslErrMsg;
}

/*
** When a server requests a client certificate that hasn't been provided,
** display a warning message explaining what to do next.
*/
static int ssl_client_cert_callback(SSL *ssl, X509 **x509, EVP_PKEY **pkey){
  fossil_warning("The remote server requested a client certificate for "
    "authentication. Specify the pathname to a file containing the PEM "
    "encoded certificate and private key with the --ssl-identity option "
    "or the ssl-identity setting.");
  return 0; /* no cert available */    
}

/*
** Call this routine once before any other use of the SSL interface.
** This routine does initial configuration of the SSL module.
*/
void ssl_global_init(void){
  const char *zCaSetting = 0, *zCaFile = 0, *zCaDirectory = 0;
  const char *identityFile;
  
  if( sslIsInit==0 ){
    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_BIO_strings();
    OpenSSL_add_all_algorithms();    
    sslCtx = SSL_CTX_new(SSLv23_client_method());
    /* Disable SSLv2 */
    SSL_CTX_set_options(sslCtx, SSL_OP_NO_SSLv2);
    
    /* Set up acceptable CA root certificates */
    zCaSetting = db_get("ssl-ca-location", 0);
    if( zCaSetting==0 || zCaSetting[0]=='\0' ){
      /* CA location not specified, use platform's default certificate store */
      X509_STORE_set_default_paths(SSL_CTX_get_cert_store(sslCtx));
    }else{
      /* User has specified a CA location, make sure it exists and use it */
      switch( file_isdir(zCaSetting) ){
        case 0: { /* doesn't exist */
          fossil_fatal("ssl-ca-location is set to '%s', "
              "but is not a file or directory", zCaSetting);
          break;
        }
        case 1: { /* directory */
          zCaDirectory = zCaSetting;
          break;
        }
        case 2: { /* file */
          zCaFile = zCaSetting;
          break;
        }
      }
      if( SSL_CTX_load_verify_locations(sslCtx, zCaFile, zCaDirectory)==0 ){
        fossil_fatal("Failed to use CA root certificates from "
          "ssl-ca-location '%s'", zCaSetting);
      }
    }
    
    /* Load client SSL identity, preferring the filename specified on the
    ** command line */
    if( g.zSSLIdentity!=0 ){
      identityFile = g.zSSLIdentity;
    }else{
      identityFile = db_get("ssl-identity", 0);
    }
    if( identityFile!=0 && identityFile[0]!='\0' ){
      if( SSL_CTX_use_certificate_file(sslCtx,identityFile,SSL_FILETYPE_PEM)!=1
       || SSL_CTX_use_PrivateKey_file(sslCtx,identityFile,SSL_FILETYPE_PEM)!=1
      ){
        fossil_fatal("Could not load SSL identity from %s", identityFile);
      }
    }
    /* Register a callback to tell the user what to do when the server asks
    ** for a cert */
    SSL_CTX_set_client_cert_cb(sslCtx, ssl_client_cert_callback);

    sslIsInit = 1;
  }
}

/*
** Call this routine to shutdown the SSL module prior to program exit.
*/
void ssl_global_shutdown(void){
  if( sslIsInit ){
    SSL_CTX_free(sslCtx);
    ssl_clear_errmsg();
    sslIsInit = 0;
  }
}

/*
** Close the currently open SSL connection.  If no connection is open, 
** this routine is a no-op.
*/
void ssl_close(void){
  if( iBio!=NULL ){
    (void)BIO_reset(iBio);
    BIO_free_all(iBio);
  }
}

/*
** Open an SSL connection.  The identify of the server is determined
** by global varibles that are set using url_parse():
**
**    g.urlName       Name of the server.  Ex: www.fossil-scm.org
**    g.urlPort       TCP/IP port to use.  Ex: 80
**
** Return the number of errors.
*/
int ssl_open(void){
  X509 *cert;
  int hasSavedCertificate = 0;
  int trusted = 0;
  unsigned long e;

  ssl_global_init();

  /* Get certificate for current server from global config and
   * (if we have it in config) add it to certificate store.
   */
  cert = ssl_get_certificate(&trusted);
  if ( cert!=NULL ){
    X509_STORE_add_cert(SSL_CTX_get_cert_store(sslCtx), cert);
    X509_free(cert);
    hasSavedCertificate = 1;
  }

  iBio = BIO_new_ssl_connect(sslCtx);
  BIO_get_ssl(iBio, &ssl);

#if (SSLEAY_VERSION_NUMBER >= 0x00908070) && !defined(OPENSSL_NO_TLSEXT)
  if( !SSL_set_tlsext_host_name(ssl, g.urlName) ){
    fossil_warning("WARNING: failed to set server name indication (SNI), "
                  "continuing without it.\n");
  }
#endif

  SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
  if( iBio==NULL ) {
    ssl_set_errmsg("SSL: cannot open SSL (%s)", 
                    ERR_reason_error_string(ERR_get_error()));
    return 1;
  }

  BIO_set_conn_hostname(iBio, g.urlName);
  BIO_set_conn_int_port(iBio, &g.urlPort);
  
  if( BIO_do_connect(iBio)<=0 ){
    ssl_set_errmsg("SSL: cannot connect to host %s:%d (%s)", 
        g.urlName, g.urlPort, ERR_reason_error_string(ERR_get_error()));
    ssl_close();
    return 1;
  }
  
  if( BIO_do_handshake(iBio)<=0 ) {
    ssl_set_errmsg("Error establishing SSL connection %s:%d (%s)", 
        g.urlName, g.urlPort, ERR_reason_error_string(ERR_get_error()));
    ssl_close();
    return 1;
  }
  /* Check if certificate is valid */
  cert = SSL_get_peer_certificate(ssl);

  if ( cert==NULL ){
    ssl_set_errmsg("No SSL certificate was presented by the peer");
    ssl_close();
    return 1;
  }

  if( trusted<=0 && (e = SSL_get_verify_result(ssl)) != X509_V_OK ){
    char *desc, *prompt;
    char *warning = "";
    Blob ans;
    BIO *mem;
    unsigned char md[32];
    unsigned int mdLength = 31;
    
    mem = BIO_new(BIO_s_mem());
    X509_NAME_print_ex(mem, X509_get_subject_name(cert), 2, XN_FLAG_MULTILINE);
    BIO_puts(mem, "\n\nIssued By:\n\n");
    X509_NAME_print_ex(mem, X509_get_issuer_name(cert), 2, XN_FLAG_MULTILINE);
    BIO_puts(mem, "\n\nSHA1 Fingerprint:\n\n ");
    if(X509_digest(cert, EVP_sha1(), md, &mdLength)){
      int j;
      for( j = 0; j < mdLength; ++j ) {
        BIO_printf(mem, " %02x", md[j]);
      }
    }
    BIO_write(mem, "", 1); /* nul-terminate mem buffer */
    BIO_get_mem_data(mem, &desc);
    
    if( hasSavedCertificate ){
      warning = "WARNING: Certificate doesn't match the "
                "saved certificate for this host!";
    }
    prompt = mprintf("\nSSL verification failed: %s\n"
        "Certificate received: \n\n%s\n\n%s\n"
        "Either:\n"
        " * verify the certificate is correct using the "
        "SHA1 fingerprint above\n"
        " * use the global ssl-ca-location setting to specify your CA root\n"
        "   certificates list\n\n"
        "If you are not expecting this message, answer no and "
        "contact your server\nadministrator.\n\n"
        "Accept certificate for host %s [a=always/y/N]? ",
        X509_verify_cert_error_string(e), desc, warning,
        g.urlName);
    BIO_free(mem);

    prompt_user(prompt, &ans);
    free(prompt);
    if( blob_str(&ans)[0]!='y' && blob_str(&ans)[0]!='a' ) {
      X509_free(cert);
      ssl_set_errmsg("SSL certificate declined");
      ssl_close();
      return 1;
    }
    if( blob_str(&ans)[0]=='a' ) {
      if ( trusted==0 ){
        Blob ans2;
        prompt_user("\nSave this certificate as fully trusted [a=always/N]? ",
                    &ans2);
        trusted = (blob_str(&ans2)[0]=='a');
        blob_reset(&ans2);
      }
      ssl_save_certificate(cert, trusted);
    }
    blob_reset(&ans);
  }

  /* Set the Global.zIpAddr variable to the server we are talking to.
  ** This is used to populate the ipaddr column of the rcvfrom table,
  ** if any files are received from the server.
  */
  {
    /* IPv4 only code */
    const unsigned char *ip = (const unsigned char *) BIO_get_conn_ip(iBio);
    g.zIpAddr = mprintf("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  }

  X509_free(cert);
  return 0;
}

/*
** Save certificate to global config.
*/
void ssl_save_certificate(X509 *cert, int trusted){
  BIO *mem;
  char *zCert, *zHost;

  mem = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(mem, cert);
  BIO_write(mem, "", 1); /* nul-terminate mem buffer */
  BIO_get_mem_data(mem, &zCert);
  zHost = mprintf("cert:%s", g.urlName);
  db_set(zHost, zCert, 1);
  free(zHost);
  zHost = mprintf("trusted:%s", g.urlName);
  db_set_int(zHost, trusted, 1);
  free(zHost);
  BIO_free(mem);  
}

/*
** Get certificate for g.urlName from global config.
** Return NULL if no certificate found.
*/
X509 *ssl_get_certificate(int *pTrusted){
  char *zHost, *zCert;
  BIO *mem;
  X509 *cert;

  zHost = mprintf("cert:%s", g.urlName);
  zCert = db_get(zHost, NULL);
  free(zHost);
  if ( zCert==NULL )
    return NULL;

  if ( pTrusted!=0 ){
    zHost = mprintf("trusted:%s", g.urlName);
    *pTrusted = db_get_int(zHost, 0);
    free(zHost);
  }

  mem = BIO_new(BIO_s_mem());
  BIO_puts(mem, zCert);
  cert = PEM_read_bio_X509(mem, NULL, 0, NULL);
  free(zCert);
  BIO_free(mem);  
  return cert;
}

/*
** Send content out over the SSL connection.
*/
size_t ssl_send(void *NotUsed, void *pContent, size_t N){
  size_t sent;
  size_t total = 0;
  while( N>0 ){
    sent = BIO_write(iBio, pContent, N);
    if( sent<=0 ) break;
    total += sent;
    N -= sent;
    pContent = (void*)&((char*)pContent)[sent];
  }
  return total;
}

/*
** Receive content back from the SSL connection.
*/
size_t ssl_receive(void *NotUsed, void *pContent, size_t N){
  size_t got;
  size_t total = 0;
  while( N>0 ){
    got = BIO_read(iBio, pContent, N);
    if( got<=0 ) break;
    total += got;
    N -= got;
    pContent = (void*)&((char*)pContent)[got];
  }
  return total;
}

#endif /* FOSSIL_ENABLE_SSL */

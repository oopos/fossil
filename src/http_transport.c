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
** This module implements the transport layer for the client side HTTP
** connection.  The purpose of this layer is to provide a common interface
** for both HTTP and HTTPS and to provide a common "fetch one line"
** interface that is used for parsing the reply.
*/
#include "config.h"
#include "http_transport.h"

/*
** State information
*/
static struct {
  int isOpen;             /* True when the transport layer is open */
  char *pBuf;             /* Buffer used to hold the reply */
  int nAlloc;             /* Space allocated for transportBuf[] */
  int nUsed ;             /* Space of transportBuf[] used */
  int iCursor;            /* Next unread by in transportBuf[] */
  i64 nSent;              /* Number of bytes sent */
  i64 nRcvd;              /* Number of bytes received */
  FILE *pFile;            /* File I/O for FILE: */
  char *zOutFile;         /* Name of outbound file for FILE: */
  char *zInFile;          /* Name of inbound file for FILE: */
  FILE *pLog;             /* Log output here */
} transport = {
  0, 0, 0, 0, 0, 0, 0
};

/*
** Information about the connection to the SSH subprocess when
** using the ssh:// sync method.
*/
static int sshPid;             /* Process id of ssh subprocess */
static int sshIn;              /* From ssh subprocess to this process */
static FILE *sshOut;           /* From this to ssh subprocess */


/*
** Return the current transport error message.
*/
const char *transport_errmsg(void){
  #ifdef FOSSIL_ENABLE_SSL
  if( g.urlIsHttps ){
    return ssl_errmsg();
  }
  #endif
  return socket_errmsg();
}

/*
** Retrieve send/receive counts from the transport layer.  If "resetFlag"
** is true, then reset the counts.
*/
void transport_stats(i64 *pnSent, i64 *pnRcvd, int resetFlag){
  if( pnSent ) *pnSent = transport.nSent;
  if( pnRcvd ) *pnRcvd = transport.nRcvd;
  if( resetFlag ){
    transport.nSent = 0;
    transport.nRcvd = 0;
  }
}

/*
** Read text from sshIn.  Zero-terminate and remove trailing
** whitespace.
*/
static void sshin_read(char *zBuf, int szBuf){
  int got;
  zBuf[0] = 0;
  got = read(sshIn, zBuf, szBuf-1);
  while( got>=0 ){
    zBuf[got] = 0;
    if( got==0 || !fossil_isspace(zBuf[got-1]) ) break;
    got--;
  }
}

/*
** Default SSH command
*/
#ifdef __MINGW32__
static char zDefaultSshCmd[] = "ssh -T";
#else
static char zDefaultSshCmd[] = "ssh -e none -T";
#endif

/*
** Global initialization of the transport layer
*/
void transport_global_startup(void){
  if( g.urlIsSsh ){
    /* Only SSH requires a global initialization.  For SSH we need to create
    ** and run an SSH command to talk to the remote machine.
    */
    const char *zSsh;  /* The base SSH command */
    Blob zCmd;         /* The SSH command */
    char *zHost;       /* The host name to contact */
    char *zIn;         /* An input line received back from remote */

    zSsh = db_get("ssh-command", zDefaultSshCmd);
    blob_init(&zCmd, zSsh, -1);
    if( g.urlPort!=g.urlDfltPort ){
#ifdef __MINGW32__
      blob_appendf(&zCmd, " -P %d", g.urlPort);
#else
      blob_appendf(&zCmd, " -p %d", g.urlPort);
#endif
    }
    fossil_print("%s", blob_str(&zCmd));  /* Show the base of the SSH command */
    if( g.urlUser && g.urlUser[0] ){
      zHost = mprintf("%s@%s", g.urlUser, g.urlName);
#ifdef __MINGW32__
      /* Only win32 (and specifically PLINK.EXE) support the -pw option */
      if( g.urlPasswd && g.urlPasswd[0] ){
        Blob pw;
        blob_zero(&pw);
        if( g.urlPasswd[0]=='*' ){
          char *zPrompt;
          zPrompt = mprintf("Password for [%s]: ", zHost);
          prompt_for_password(zPrompt, &pw, 0);
          free(zPrompt);
        }else{
          blob_init(&pw, g.urlPasswd, -1);
        }
        blob_append(&zCmd, " -pw ", -1);
        shell_escape(&zCmd, blob_str(&pw));
        blob_reset(&pw);
        fossil_print(" -pw ********");  /* Do not show the password text */
      }
#endif
    }else{
      zHost = mprintf("%s", g.urlName);
    }
    blob_append(&zCmd, " ", 1);
    shell_escape(&zCmd, zHost);
    fossil_print(" %s\n", zHost);  /* Show the conclusion of the SSH command */
    free(zHost);
    popen2(blob_str(&zCmd), &sshIn, &sshOut, &sshPid);
    if( sshPid==0 ){
      fossil_fatal("cannot start ssh tunnel using [%b]", &zCmd);
    }
    blob_reset(&zCmd);

    /* Send an "echo" command to the other side to make sure that the
    ** connection is up and working.
    */
    fprintf(sshOut, "echo test\n");
    fflush(sshOut);
    zIn = fossil_malloc(16000);
    sshin_read(zIn, 16000);
    if( memcmp(zIn, "test", 4)!=0 ){
      pclose2(sshIn, sshOut, sshPid);
      fossil_fatal("ssh connection failed: [%s]", zIn);
    }
    fossil_free(zIn);
  }
}

/*
** Open a connection to the server.  The server is defined by the following
** global variables:
**
**   g.urlName        Name of the server.  Ex: www.fossil-scm.org
**   g.urlPort        TCP/IP port.  Ex: 80
**   g.urlIsHttps     Use TLS for the connection
**
** Return the number of errors.
*/
int transport_open(void){
  int rc = 0;
  if( transport.isOpen==0 ){
    if( g.urlIsSsh ){
      Blob cmd;
      blob_zero(&cmd);
      shell_escape(&cmd, g.urlFossil);
      blob_append(&cmd, " test-http ", -1);
      shell_escape(&cmd, g.urlPath);
      /* printf("%s\n", blob_str(&cmd)); fflush(stdout); */
      fprintf(sshOut, "%s\n", blob_str(&cmd));
      fflush(sshOut);
      blob_reset(&cmd);
    }else if( g.urlIsHttps ){
      #ifdef FOSSIL_ENABLE_SSL
      rc = ssl_open();
      if( rc==0 ) transport.isOpen = 1;
      #else
      socket_set_errmsg("HTTPS: Fossil has been compiled without SSL support");
      rc = 1;
      #endif
    }else if( g.urlIsFile ){
      sqlite3_uint64 iRandId;
      sqlite3_randomness(sizeof(iRandId), &iRandId);
      transport.zOutFile = mprintf("%s-%llu-out.http", 
                                       g.zRepositoryName, iRandId);
      transport.zInFile = mprintf("%s-%llu-in.http", 
                                       g.zRepositoryName, iRandId);
      transport.pFile = fopen(transport.zOutFile, "wb");
      if( transport.pFile==0 ){
        fossil_fatal("cannot output temporary file: %s", transport.zOutFile);
      }
      transport.isOpen = 1;
    }else{
      rc = socket_open();
      if( rc==0 ) transport.isOpen = 1;
    }
  }
  return rc;
}

/*
** Close the current connection
*/
void transport_close(void){
  if( transport.isOpen ){
    free(transport.pBuf);
    transport.pBuf = 0;
    transport.nAlloc = 0;
    transport.nUsed = 0;
    transport.iCursor = 0;
    if( transport.pLog ){
      fclose(transport.pLog);
      transport.pLog = 0;
    }
    if( g.urlIsSsh ){
      /* No-op */
    }else if( g.urlIsHttps ){
      #ifdef FOSSIL_ENABLE_SSL
      ssl_close();
      #endif
    }else if( g.urlIsFile ){
      if( transport.pFile ){ 
        fclose(transport.pFile);
        transport.pFile = 0;
      }
      file_delete(transport.zInFile);
      file_delete(transport.zOutFile);
      free(transport.zInFile);
      free(transport.zOutFile);
    }else{
      socket_close();
    }
    transport.isOpen = 0;
  }
}

/*
** Send content over the wire.
*/
void transport_send(Blob *toSend){
  char *z = blob_buffer(toSend);
  int n = blob_size(toSend);
  transport.nSent += n;
  if( g.urlIsSsh ){
    fwrite(z, 1, n, sshOut);
    fflush(sshOut);
  }else if( g.urlIsHttps ){
    #ifdef FOSSIL_ENABLE_SSL
    int sent;
    while( n>0 ){
      sent = ssl_send(0, z, n);
      /* printf("Sent %d of %d bytes\n", sent, n); fflush(stdout); */
      if( sent<=0 ) break;
      n -= sent;
    }    
    #endif
  }else if( g.urlIsFile ){
    fwrite(z, 1, n, transport.pFile);
  }else{
    int sent;
    while( n>0 ){
      sent = socket_send(0, z, n);
      /* printf("Sent %d of %d bytes\n", sent, n); fflush(stdout); */
      if( sent<=0 ) break;
      n -= sent;
    }
  }
}

/*
** This routine is called when the outbound message is complete and
** it is time to being recieving a reply.
*/
void transport_flip(void){
  if( g.urlIsSsh ){
    fprintf(sshOut, "\n\n");
  }else if( g.urlIsFile ){
    char *zCmd;
    fclose(transport.pFile);
    zCmd = mprintf("\"%s\" http \"%s\" \"%s\" \"%s\" 127.0.0.1 --localauth",
       fossil_nameofexe(), g.urlName, transport.zOutFile, transport.zInFile
    );
    fossil_system(zCmd);
    free(zCmd);
    transport.pFile = fopen(transport.zInFile, "rb");
  }
}

/*
** Log all input to a file.  The transport layer will take responsibility
** for closing the log file when it is done.
*/
void transport_log(FILE *pLog){
  if( transport.pLog ){
    fclose(transport.pLog);
    transport.pLog = 0;
  }
  transport.pLog = pLog;
}

/*
** This routine is called when the inbound message has been received
** and it is time to start sending again.
*/
void transport_rewind(void){
  if( g.urlIsFile ){
    transport_close();
  }
}

/*
** Read N bytes of content directly from the wire and write into
** the buffer.
*/
static int transport_fetch(char *zBuf, int N){
  int got;
  if( sshIn ){
    int x;
    int wanted = N;
    got = 0;
    /* printf("want %d bytes...\n", wanted); fflush(stdout); */
    while( wanted>0 ){
      x = read(sshIn, &zBuf[got], wanted);
      if( x<=0 ) break;
      got += x;
      wanted -= x;
    }
  }else if( g.urlIsHttps ){
    #ifdef FOSSIL_ENABLE_SSL
    got = ssl_receive(0, zBuf, N);
    #else
    got = 0;
    #endif
  }else if( g.urlIsFile ){
    got = fread(zBuf, 1, N, transport.pFile);
  }else{
    got = socket_receive(0, zBuf, N);
  }
  /* printf("received %d of %d bytes\n", got, N); fflush(stdout); */
  if( transport.pLog ){
    fwrite(zBuf, 1, got, transport.pLog);
    fflush(transport.pLog);
  }
  return got;
}

/*
** Read N bytes of content from the wire and store in the supplied buffer.
** Return the number of bytes actually received.
*/
int transport_receive(char *zBuf, int N){
  int onHand;       /* Bytes current held in the transport buffer */
  int nByte = 0;    /* Bytes of content received */

  onHand = transport.nUsed - transport.iCursor;
  /* printf("request %d with %d on hand\n", N, onHand); fflush(stdout); */
  if( onHand>0 ){
    int toMove = onHand;
    if( toMove>N ) toMove = N;
    /* printf("bytes on hand: %d of %d\n", toMove, N); fflush(stdout); */
    memcpy(zBuf, &transport.pBuf[transport.iCursor], toMove);
    transport.iCursor += toMove;
    if( transport.iCursor>=transport.nUsed ){
      transport.nUsed = 0;
      transport.iCursor = 0;
    }
    N -= toMove;
    zBuf += toMove;
    nByte += toMove;
  }
  if( N>0 ){
    int got = transport_fetch(zBuf, N);
    if( got>0 ){
      nByte += got;
      transport.nRcvd += got;
    }
  }
  return nByte;
}

/*
** Load up to N new bytes of content into the transport.pBuf buffer.
** The buffer itself might be moved.  And the transport.iCursor value
** might be reset to 0.
*/
static void transport_load_buffer(int N){
  int i, j;
  if( transport.nAlloc==0 ){
    transport.nAlloc = N;
    transport.pBuf = fossil_malloc( N );
    transport.iCursor = 0;
    transport.nUsed = 0;
  }
  if( transport.iCursor>0 ){
    for(i=0, j=transport.iCursor; j<transport.nUsed; i++, j++){
      transport.pBuf[i] = transport.pBuf[j];
    }
    transport.nUsed -= transport.iCursor;
    transport.iCursor = 0;
  }
  if( transport.nUsed + N > transport.nAlloc ){
    char *pNew;
    transport.nAlloc = transport.nUsed + N;
    pNew = fossil_realloc(transport.pBuf, transport.nAlloc);
    transport.pBuf = pNew;
  }
  if( N>0 ){
    i = transport_fetch(&transport.pBuf[transport.nUsed], N);
    if( i>0 ){
      transport.nRcvd += i;
      transport.nUsed += i;
    }
  }
}

/*
** Fetch a single line of input where a line is all text up to the next
** \n character or until the end of input.  Remove all trailing whitespace
** from the received line and zero-terminate the result.  Return a pointer
** to the line.
**
** Each call to this routine potentially overwrites the returned buffer.
*/
char *transport_receive_line(void){
  int i;
  int iStart;

  i = iStart = transport.iCursor;
  while(1){
    if( i >= transport.nUsed ){
      transport_load_buffer(g.urlIsSsh ? 2 : 1000);
      i -= iStart;
      iStart = 0;
      if( i >= transport.nUsed ){
        transport.pBuf[i] = 0;
        transport.iCursor = i;
        break;
      }
    }
    if( transport.pBuf[i]=='\n' ){
      transport.iCursor = i+1;
      while( i>=iStart && fossil_isspace(transport.pBuf[i]) ){
        transport.pBuf[i] = 0;
        i--;
      }
      break;
    }
    i++;
  }
   /* printf("Got line: [%s]\n", &transport.pBuf[iStart]); */
  return &transport.pBuf[iStart];
}

void transport_global_shutdown(void){
  if( g.urlIsSsh && sshPid ){
    printf("Closing SSH tunnel: ");
    fflush(stdout);
    pclose2(sshIn, sshOut, sshPid);
    sshPid = 0;
  }
  if( g.urlIsHttps ){
    #ifdef FOSSIL_ENABLE_SSL
    ssl_global_shutdown();
    #endif
  }else{
    socket_global_shutdown();
  }
}

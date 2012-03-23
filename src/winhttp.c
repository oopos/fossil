/*
** Copyright (c) 2008 D. Richard Hipp
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
** This file implements a very simple (and low-performance) HTTP server
** for windows. It also implements a Windows Service which allows the HTTP
** server to be run without any user logged on.
*/
#include "config.h"
#ifdef _WIN32
/* This code is for win32 only */
#include <windows.h>
#include "winhttp.h"

/*
** The HttpRequest structure holds information about each incoming
** HTTP request.
*/
typedef struct HttpRequest HttpRequest;
struct HttpRequest {
  int id;                /* ID counter */
  SOCKET s;              /* Socket on which to receive data */
  SOCKADDR_IN addr;      /* Address from which data is coming */
  const char *zOptions;  /* --notfound and/or --localauth options */
};

/*
** Prefix for a temporary file.
*/
static char *zTempPrefix;

/*
** Look at the HTTP header contained in zHdr.  Find the content
** length and return it.  Return 0 if there is no Content-Length:
** header line.
*/
static int find_content_length(const char *zHdr){
  while( *zHdr ){
    if( zHdr[0]=='\n' ){
      if( zHdr[1]=='\r' ) return 0;
      if( fossil_strnicmp(&zHdr[1], "content-length:", 15)==0 ){
        return atoi(&zHdr[17]);
      }
    }
    zHdr++;
  }
  return 0;
}

/*
** Process a single incoming HTTP request.
*/
static void win32_process_one_http_request(void *pAppData){
  HttpRequest *p = (HttpRequest*)pAppData;
  FILE *in = 0, *out = 0;
  int amt, got;
  int wanted = 0;
  char *z;
  char zRequestFName[100];
  char zReplyFName[100];
  char zCmd[2000];          /* Command-line to process the request */
  char zHdr[2000];          /* The HTTP request header */

  sqlite3_snprintf(sizeof(zRequestFName), zRequestFName,
                   "%s_in%d.txt", zTempPrefix, p->id);
  sqlite3_snprintf(sizeof(zReplyFName), zReplyFName,
                   "%s_out%d.txt", zTempPrefix, p->id);
  amt = 0;
  while( amt<sizeof(zHdr) ){
    got = recv(p->s, &zHdr[amt], sizeof(zHdr)-1-amt, 0);
    if( got==SOCKET_ERROR ) goto end_request;
    if( got==0 ){
      wanted = 0;
      break;
    }
    amt += got;
    zHdr[amt] = 0;
    z = strstr(zHdr, "\r\n\r\n");
    if( z ){
      wanted = find_content_length(zHdr) + (&z[4]-zHdr) - amt;
      break;
    }
  }
  if( amt>=sizeof(zHdr) ) goto end_request;
  out = fossil_fopen(zRequestFName, "wb");
  if( out==0 ) goto end_request;
  fwrite(zHdr, 1, amt, out);
  while( wanted>0 ){
    got = recv(p->s, zHdr, sizeof(zHdr), 0);
    if( got==SOCKET_ERROR ) goto end_request;
    if( got ){
      fwrite(zHdr, 1, got, out);
    }else{
      break;
    }
    wanted -= got;
  }
  fclose(out);
  out = 0;
  sqlite3_snprintf(sizeof(zCmd), zCmd, "\"%s\" http \"%s\" %s %s %s --nossl%s",
    fossil_nameofexe(), g.zRepositoryName, zRequestFName, zReplyFName, 
    inet_ntoa(p->addr.sin_addr), p->zOptions
  );
  fossil_system(zCmd);
  in = fossil_fopen(zReplyFName, "rb");
  if( in ){
    while( (got = fread(zHdr, 1, sizeof(zHdr), in))>0 ){
      send(p->s, zHdr, got, 0);
    }
  }

end_request:
  if( out ) fclose(out);
  if( in ) fclose(in);
  closesocket(p->s);
  file_delete(zRequestFName);
  file_delete(zReplyFName);
  free(p);
}

/*
** Start a listening socket and process incoming HTTP requests on
** that socket.
*/
void win32_http_server(
  int mnPort, int mxPort,   /* Range of allowed TCP port numbers */
  const char *zBrowser,     /* Command to launch browser.  (Or NULL) */
  const char *zStopper,     /* Stop server when this file is exists (Or NULL) */
  const char *zNotFound,    /* The --notfound option, or NULL */
  int flags                 /* One or more HTTP_SERVER_ flags */
){
  WSADATA wd;
  SOCKET s = INVALID_SOCKET;
  SOCKADDR_IN addr;
  int idCnt = 0;
  int iPort = mnPort;
  Blob options;
  char zTmpPath[MAX_PATH];

  if( zStopper ) file_delete(zStopper);
  blob_zero(&options);
  if( zNotFound ){
    blob_appendf(&options, " --notfound %s", zNotFound);
  }
  if( g.useLocalauth ){
    blob_appendf(&options, " --localauth");
  }
  if( WSAStartup(MAKEWORD(1,1), &wd) ){
    fossil_fatal("unable to initialize winsock");
  }
  while( iPort<=mxPort ){
    s = socket(AF_INET, SOCK_STREAM, 0);
    if( s==INVALID_SOCKET ){
      fossil_fatal("unable to create a socket");
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(iPort);
    if( flags & HTTP_SERVER_LOCALHOST ){
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }else{
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if( bind(s, (struct sockaddr*)&addr, sizeof(addr))==SOCKET_ERROR ){
      closesocket(s);
      iPort++;
      continue;
    }
    if( listen(s, SOMAXCONN)==SOCKET_ERROR ){
      closesocket(s);
      iPort++;
      continue;
    }
    break;
  }
  if( iPort>mxPort ){
    if( mnPort==mxPort ){
      fossil_fatal("unable to open listening socket on ports %d", mnPort);
    }else{
      fossil_fatal("unable to open listening socket on any"
                   " port in the range %d..%d", mnPort, mxPort);
    }
  }
  if( !GetTempPath(sizeof(zTmpPath), zTmpPath) ){
    fossil_fatal("unable to get path to the temporary directory.");
  }
  zTempPrefix = mprintf("%sfossil_server_P%d_", zTmpPath, iPort);
  fossil_print("Listening for HTTP requests on TCP port %d\n", iPort);
  if( zBrowser ){
    zBrowser = mprintf(zBrowser, iPort);
    fossil_print("Launch webbrowser: %s\n", zBrowser);
    fossil_system(zBrowser);
  }
  fossil_print("Type Ctrl-C to stop the HTTP server\n");
  /* Set the service status to running and pass the listener socket to the
  ** service handling procedures. */
  win32_http_service_running(s);
  for(;;){
    SOCKET client;
    SOCKADDR_IN client_addr;
    HttpRequest *p;
    int len = sizeof(client_addr);
    int wsaError;

    client = accept(s, (struct sockaddr*)&client_addr, &len);
    if( client==INVALID_SOCKET ){
      /* If the service control handler has closed the listener socket, 
      ** cleanup and return, otherwise report a fatal error. */
      wsaError =  WSAGetLastError();
      if( (wsaError==WSAEINTR) || (wsaError==WSAENOTSOCK) ){
        WSACleanup();
        return;
      }else{
        closesocket(s);
        WSACleanup();
        fossil_fatal("error from accept()");
      }
    }else if( zStopper && file_size(zStopper)>=0 ){
      break;
    }
    p = fossil_malloc( sizeof(*p) );
    p->id = ++idCnt;
    p->s = client;
    p->addr = client_addr;
    p->zOptions = blob_str(&options);
    _beginthread(win32_process_one_http_request, 0, (void*)p);
  }
  closesocket(s);
  WSACleanup();
}

/*
** The HttpService structure is used to pass information to the service main
** function and to the service control handler function.
*/
typedef struct HttpService HttpService;
struct HttpService {
  int port;                 /* Port on which the http server should run */
  const char *zNotFound;    /* The --notfound option, or NULL */
  int flags;                /* One or more HTTP_SERVER_ flags */
  int isRunningAsService;   /* Are we running as a service ? */
  const char *zServiceName; /* Name of the service */
  SOCKET s;                 /* Socket on which the http server listens */
};

/*
** Variables used for running as windows service.
*/
static HttpService hsData = {8080, NULL, 0, 0, NULL, INVALID_SOCKET};
static SERVICE_STATUS ssStatus;
static SERVICE_STATUS_HANDLE sshStatusHandle;

/*
** Get message string of the last system error. Return a pointer to the
** message string. Call fossil_mbcs_free() to deallocate any memory used
** to store the message string when done.
*/
static char *win32_get_last_errmsg(void){
  DWORD nMsg;
  LPTSTR tmp = NULL;
  char *zMsg = NULL;

  nMsg = FormatMessage(
           FORMAT_MESSAGE_ALLOCATE_BUFFER |
           FORMAT_MESSAGE_FROM_SYSTEM     |
           FORMAT_MESSAGE_IGNORE_INSERTS,
           NULL,
           GetLastError(),
           MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
           (LPTSTR) &tmp,
           0,
           NULL
         );
  if( nMsg ){
    zMsg = fossil_mbcs_to_utf8(tmp);
  }else{
    fossil_fatal("unable to get system error message.");
  }
  if( tmp ){
    LocalFree((HLOCAL) tmp);
  }
  return zMsg;
}

/*
** Report the current status of the service to the service control manager.
** Make sure that during service startup no control codes are accepted.
*/
static void win32_report_service_status(
  DWORD dwCurrentState,     /* The current state of the service */
  DWORD dwWin32ExitCode,    /* The error code to report */
  DWORD dwWaitHint          /* The estimated time for a pending operation */
){
  if( dwCurrentState==SERVICE_START_PENDING) {
    ssStatus.dwControlsAccepted = 0;
  }else{
    ssStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
  }
  ssStatus.dwCurrentState = dwCurrentState;
  ssStatus.dwWin32ExitCode = dwWin32ExitCode;
  ssStatus.dwWaitHint = dwWaitHint;

  if( (dwCurrentState==SERVICE_RUNNING) || 
      (dwCurrentState==SERVICE_STOPPED) ){
    ssStatus.dwCheckPoint = 0;
  }else{
    ssStatus.dwCheckPoint++;
  }
  SetServiceStatus(sshStatusHandle, &ssStatus);
  return ;
}

/*
** Handle control codes sent from the service control manager.
** The control dispatcher in the main thread of the service process invokes
** this function whenever it receives a control request from the service
** control manager.
*/
static void WINAPI win32_http_service_ctrl(
  DWORD dwCtrlCode
){
  switch( dwCtrlCode ){
    case SERVICE_CONTROL_STOP: {
      win32_report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 0);
      if( hsData.s != INVALID_SOCKET ){
        closesocket(hsData.s);
      }
      win32_report_service_status(ssStatus.dwCurrentState, NO_ERROR, 0);
      break;
    }
    default: {
      break;
    }
  }
  return;
}

/*
** This is the main entry point for the service.
** When the service control manager receives a request to start the service,
** it starts the service process (if it is not already running). The main
** thread of the service process calls the StartServiceCtrlDispatcher
** function with a pointer to an array of SERVICE_TABLE_ENTRY structures.
** Then the service control manager sends a start request to the service
** control dispatcher for this service process. The service control dispatcher
** creates a new thread to execute the ServiceMain function (this function)
** of the service being started.
*/
static void WINAPI win32_http_service_main(
  DWORD argc,              /* Number of arguments in argv */
  LPTSTR *argv             /* Arguments passed */
){

  /* Update the service information. */
  hsData.isRunningAsService = 1;
  if( argc>0 ){
    hsData.zServiceName = argv[0];
  }

  /* Register the service control handler function */
  sshStatusHandle = RegisterServiceCtrlHandler("", win32_http_service_ctrl);
  if( !sshStatusHandle ){
    win32_report_service_status(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  /* Set service specific data and report that the service is starting. */
  ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  ssStatus.dwServiceSpecificExitCode = 0;
  win32_report_service_status(SERVICE_START_PENDING, NO_ERROR, 3000);

   /* Execute the http server */
  win32_http_server(hsData.port, hsData.port,
                    NULL, NULL, hsData.zNotFound, hsData.flags);

  /* Service has stopped now. */
  win32_report_service_status(SERVICE_STOPPED, NO_ERROR, 0);
  return;
}

/*
** When running as service, update the HttpService structure with the
** listener socket and update the service status. This procedure must be
** called from the http server when he is ready to accept connections.
*/
LOCAL void win32_http_service_running(SOCKET s){
  if( hsData.isRunningAsService ){
    hsData.s = s;
    win32_report_service_status(SERVICE_RUNNING, NO_ERROR, 0);
  }
}

/*
** Try to start the http server as a windows service. If we are running in
** a interactive console session, this routine fails and returns a non zero
** integer value. When running as service, this routine does not return until
** the service is stopped. In this case, the return value is zero.
*/
int win32_http_service(
  int nPort,                /* TCP port number */
  const char *zNotFound,    /* The --notfound option, or NULL */
  int flags                 /* One or more HTTP_SERVER_ flags */
){
  /* Define the service table. */
  SERVICE_TABLE_ENTRY ServiceTable[] =
    {{"", (LPSERVICE_MAIN_FUNCTION)win32_http_service_main}, {NULL, NULL}};
  
  /* Initialize the HttpService structure. */
  hsData.port = nPort;
  hsData.zNotFound = zNotFound;
  hsData.flags = flags;

  /* Try to start the control dispatcher thread for the service. */
  if( !StartServiceCtrlDispatcher(ServiceTable) ){
    if( GetLastError()==ERROR_FAILED_SERVICE_CONTROLLER_CONNECT ){
      return 1;
    }else{
      fossil_fatal("error from StartServiceCtrlDispatcher()");
    }
  }
  return 0;
}

/*
** COMMAND: winsrv*
** Usage: fossil winsrv METHOD ?SERVICE-NAME? ?OPTIONS?
**
** Where METHOD is one of: create delete show start stop.
**
** The winsrv command manages Fossil as a Windows service.  This allows
** (for example) Fossil to be running in the background when no user
** is logged in.
**
** In the following description of the methods, "Fossil-DSCM" will be
** used as the default SERVICE-NAME:
** 
**    fossil winsrv create ?SERVICE-NAME? ?OPTIONS?
**
**         Creates a service. Available options include:
**
**         -D|--display DISPLAY-NAME
**
**              Sets the display name of the service. This name is shown
**              by graphical interface programs. By default, the display name
**              equals to the service name.
**
**         -S|--start TYPE
**
**              Sets the start type of the service. TYPE can be "manual",
**              which means you need to start the service yourself with the
**              'fossil winsrv start' command or with the "net start" command
**              from the operating system. If TYPE is set to "auto", the service
**              will be started automatically by the system during startup.
**
**         -U|--username USERNAME
**
**              Specifies the user account which will be used to run the
**              service. The account needs the "Logon as a service" right
**              enabled in its profile. Specify local accounts as follows:
**              ".\\USERNAME". By default, the "LocalSystem" account will be
**              used.
**
**         -W|--password PASSWORD
**
**              Password for the user account.
**
**         The following options are more or less the same as for the "server"
**         command and influence the behaviour of the http server:
**
**         -p|--port TCPPORT
**
**              Specifies the TCP port (default port is 8080) on which the
**              server should listen.
**
**         -R|--repository REPOSITORY
**
**              Specifies the name of the repository to be served.
**              The repository option may be omitted if the working directory
**              is within an open checkout.
**              The REPOSITORY can be a directory (aka folder) that contains
**              one or more respositories with names ending in ".fossil".
**              In that case, the first element of the URL is used to select
**              among the various repositories.
**
**         --notfound URL
**
**              If REPOSITORY is a directory that contains one or more
**              respositories with names of the form "*.fossil" then the
**              first element of the URL  pathname selects among the various
**              repositories. If the pathname does not select a valid
**              repository and the --notfound option is available,
**              then the server redirects (HTTP code 302) to the URL of
**              --notfound.
**
**         --localauth
**
**              Enables automatic login if the --localauth option is present
**              and the "localauth" setting is off and the connection is from
**              localhost.
**
**
**    fossil winsrv delete ?SERVICE-NAME?
**
**         Deletes a service. If the service is currently running, it will be
**         stopped first and then deleted.
**
**
**    fossil winsrv show ?SERVICE-NAME?
**
**         Shows how the service is configured and its current state.
**
**
**    fossil winsrv start ?SERVICE-NAME?
**
**         Start the service.
**
**
**    fossil winsrv stop ?SERVICE-NAME?
**
**         Stop the service.
**
**
** NOTE: This command is available on Windows operating systems only and
**       requires administrative rights on the machine executed.
**
*/
void cmd_win32_service(void){
  int n;
  const char *zMethod;
  const char *zSvcName = "Fossil-DSCM";    /* Default service name */

  if( g.argc<3 ){
    usage("create|delete|show|start|stop ...");
  }
  zMethod = g.argv[2];
  n = strlen(zMethod);
  if( g.argc==4 ){
    zSvcName = g.argv[3];
  }

  if( strncmp(zMethod, "create", n)==0 ){
    SC_HANDLE hScm;
    SC_HANDLE hSvc;
    SERVICE_DESCRIPTION
      svcDescr = {"Fossil - Distributed Software Configuration Management"};
    char *zErrFmt = "unable to create service '%s': %s";
    DWORD dwStartType = SERVICE_DEMAND_START;
    const char *zDisplay;
    const char *zStart;
    const char *zUsername;
    const char *zPassword;
    const char *zPort;
    const char *zNotFound;
    const char *zLocalAuth;
    const char *zRepository;
    Blob binPath;

    /* Process service creation specific options. */
    zDisplay = find_option("display", "D", 1);
    if( !zDisplay ){
      zDisplay = zSvcName;
    }
    zStart = find_option("start", "S", 1);
    if( zStart ){
      if( strncmp(zStart, "auto", strlen(zStart))==0 ){
        dwStartType = SERVICE_AUTO_START;
      }else  if( strncmp(zStart, "manual", strlen(zStart))==0 ){
        dwStartType = SERVICE_DEMAND_START;
      }else{
        fossil_fatal(zErrFmt, zSvcName,
                     "specify 'auto' or 'manual' for the '-S|--start' option");
      }
    }
    zUsername = find_option("username", "U", 1);
    zPassword = find_option("password", "W", 1);
    /* Process options for Fossil running as server. */
    zPort = find_option("port", "P", 1);
    if( zPort && (atoi(zPort)<=0) ){
      fossil_fatal(zErrFmt, zSvcName,
                   "port number must be in the range 1 - 65535.");
    }
    zNotFound = find_option("notfound", 0, 1);
    zLocalAuth = find_option("localauth", 0, 0);
    zRepository = find_option("repository", "R", 1);
    if( !zRepository ){
      db_must_be_within_tree();
    }else if( file_isdir(zRepository)==1 ){
      g.zRepositoryName = mprintf("%s", zRepository);
      file_simplify_name(g.zRepositoryName, -1);
    }else{
      db_open_repository(zRepository);
    }
    db_close(0);
    verify_all_options();
    if( g.argc>4 ) fossil_fatal("to much arguments for create method.");
    /* Build the fully-qualified path to the service binary file. */
    blob_zero(&binPath);
    blob_appendf(&binPath, "\"%s\" server", fossil_nameofexe());
    if( zPort ) blob_appendf(&binPath, " --port %s", zPort);
    if( zNotFound ) blob_appendf(&binPath, " --notfound \"%s\"", zNotFound);
    if( zLocalAuth ) blob_append(&binPath, " --localauth", -1);
    blob_appendf(&binPath, " \"%s\"", g.zRepositoryName);
    /* Create the service. */
    hScm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if( !hScm ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    hSvc = CreateService(
             hScm,                                    /* Handle to the SCM */
             fossil_utf8_to_mbcs(zSvcName),           /* Name of the service */
             fossil_utf8_to_mbcs(zDisplay),           /* Display name */
             SERVICE_ALL_ACCESS,                      /* Desired access */
             SERVICE_WIN32_OWN_PROCESS,               /* Service type */
             dwStartType,                             /* Start type */
             SERVICE_ERROR_NORMAL,                    /* Error control */
             fossil_utf8_to_mbcs(blob_str(&binPath)), /* Binary path */
             NULL,                                    /* Load ordering group */
             NULL,                                    /* Tag value */
             NULL,                                    /* Service dependencies */
             fossil_utf8_to_mbcs(zUsername),          /* Service account */
             fossil_utf8_to_mbcs(zPassword)           /* Account password */
           );
    if( !hSvc ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    /* Set the service description. */
    ChangeServiceConfig2(hSvc, SERVICE_CONFIG_DESCRIPTION, &svcDescr);
    fossil_print("Service '%s' successfully created.\n", zSvcName);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
  }else
  if( strncmp(zMethod, "delete", n)==0 ){
    SC_HANDLE hScm;
    SC_HANDLE hSvc;
    SERVICE_STATUS sstat;
    char *zErrFmt = "unable to delete service '%s': %s";

    verify_all_options();
    if( g.argc>4 ) fossil_fatal("to much arguments for delete method.");
    hScm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if( !hScm ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    hSvc = OpenService(hScm, fossil_utf8_to_mbcs(zSvcName), SERVICE_ALL_ACCESS);
    if( !hSvc ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    QueryServiceStatus(hSvc, &sstat);
    if( sstat.dwCurrentState!=SERVICE_STOPPED ){
      fossil_print("Stopping service '%s'", zSvcName);
      if( sstat.dwCurrentState!=SERVICE_STOP_PENDING ){
        if( !ControlService(hSvc, SERVICE_CONTROL_STOP, &sstat) ){
          fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
        }
      }
      while( sstat.dwCurrentState!=SERVICE_STOPPED ){
        Sleep(100);
        fossil_print(".");
        QueryServiceStatus(hSvc, &sstat);
      }
      fossil_print("\nService '%s' stopped.\n", zSvcName);
    }
    if( !DeleteService(hSvc) ){
      if( GetLastError()==ERROR_SERVICE_MARKED_FOR_DELETE ){
        fossil_warning("Service '%s' already marked for delete.\n", zSvcName);
      }else{
        fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
      }
    }else{
      fossil_print("Service '%s' successfully deleted.\n", zSvcName);
    }
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
  }else
  if( strncmp(zMethod, "show", n)==0 ){
    SC_HANDLE hScm;
    SC_HANDLE hSvc;
    SERVICE_STATUS sstat;
    LPQUERY_SERVICE_CONFIG pSvcConfig;
    LPSERVICE_DESCRIPTION pSvcDescr;
    BOOL bStatus;
    DWORD nRequired;
    char *zErrFmt = "unable to show service '%s': %s";
    static const char *zSvcTypes[] = {
      "Driver service",
      "File system driver service",
      "Service runs in its own process",
      "Service shares a process with other services",
      "Service can interact with the desktop"
    };
    const char *zSvcType = "";
    static char *zSvcStartTypes[] = {
      "Started by the system loader",
      "Started by the IoInitSystem function",
      "Started automatically by the service control manager",
      "Started manually",
      "Service cannot be started"
    };
    const char *zSvcStartType = "";
    static const char *zSvcStates[] = {
      "Stopped", "Starting", "Stopping", "Running",
      "Continue pending", "Pause pending", "Paused"
    };
    const char *zSvcState = "";

    verify_all_options();
    if( g.argc>4 ) fossil_fatal("to much arguments for show method.");
    hScm = OpenSCManager(NULL, NULL, GENERIC_READ);
    if( !hScm ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    hSvc = OpenService(hScm, fossil_utf8_to_mbcs(zSvcName), GENERIC_READ);
    if( !hSvc ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    /* Get the service configuration */
    bStatus = QueryServiceConfig(hSvc, NULL, 0, &nRequired);
    if( !bStatus && GetLastError()!=ERROR_INSUFFICIENT_BUFFER ){
      fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    }
    pSvcConfig = fossil_malloc(nRequired);
    bStatus = QueryServiceConfig(hSvc, pSvcConfig, nRequired, &nRequired);
    if( !bStatus ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    /* Translate the service type */
    switch( pSvcConfig->dwServiceType ){
      case SERVICE_KERNEL_DRIVER:       zSvcType = zSvcTypes[0]; break;
      case SERVICE_FILE_SYSTEM_DRIVER:  zSvcType = zSvcTypes[1]; break;
      case SERVICE_WIN32_OWN_PROCESS:   zSvcType = zSvcTypes[2]; break;
      case SERVICE_WIN32_SHARE_PROCESS: zSvcType = zSvcTypes[3]; break;
      case SERVICE_INTERACTIVE_PROCESS: zSvcType = zSvcTypes[4]; break;
    }
    /* Translate the service start type */
    switch( pSvcConfig->dwStartType ){
      case SERVICE_BOOT_START:    zSvcStartType = zSvcStartTypes[0]; break;
      case SERVICE_SYSTEM_START:  zSvcStartType = zSvcStartTypes[1]; break;
      case SERVICE_AUTO_START:    zSvcStartType = zSvcStartTypes[2]; break;
      case SERVICE_DEMAND_START:  zSvcStartType = zSvcStartTypes[3]; break;
      case SERVICE_DISABLED:      zSvcStartType = zSvcStartTypes[4]; break;
    }
    /* Get the service description. */
    bStatus = QueryServiceConfig2(hSvc, SERVICE_CONFIG_DESCRIPTION,
                                  NULL, 0, &nRequired);
    if( !bStatus && GetLastError()!=ERROR_INSUFFICIENT_BUFFER ){
      fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    }
    pSvcDescr = fossil_malloc(nRequired);
    bStatus = QueryServiceConfig2(hSvc, SERVICE_CONFIG_DESCRIPTION,
                                  (LPBYTE)pSvcDescr, nRequired, &nRequired);
    if( !bStatus ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    /* Retrieves the current status of the specified service. */
    bStatus = QueryServiceStatus(hSvc, &sstat);
    if( !bStatus ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    /* Translate the current state. */
    switch( sstat.dwCurrentState ){
      case SERVICE_STOPPED:          zSvcState = zSvcStates[0]; break;
      case SERVICE_START_PENDING:    zSvcState = zSvcStates[1]; break;
      case SERVICE_STOP_PENDING:     zSvcState = zSvcStates[2]; break;
      case SERVICE_RUNNING:          zSvcState = zSvcStates[3]; break;
      case SERVICE_CONTINUE_PENDING: zSvcState = zSvcStates[4]; break;
      case SERVICE_PAUSE_PENDING:    zSvcState = zSvcStates[5]; break;
      case SERVICE_PAUSED:           zSvcState = zSvcStates[6]; break;
    }
    /* Print service information to terminal */
    fossil_print("Service name .......: %s\n", zSvcName);
    fossil_print("Display name .......: %s\n",
                 fossil_mbcs_to_utf8(pSvcConfig->lpDisplayName));
    fossil_print("Service description : %s\n",
                 fossil_mbcs_to_utf8(pSvcDescr->lpDescription));
    fossil_print("Service type .......: %s.\n", zSvcType);
    fossil_print("Service start type .: %s.\n", zSvcStartType);
    fossil_print("Binary path name ...: %s\n",
                 fossil_mbcs_to_utf8(pSvcConfig->lpBinaryPathName));
    fossil_print("Service username ...: %s\n",
                 fossil_mbcs_to_utf8(pSvcConfig->lpServiceStartName));
    fossil_print("Current state ......: %s.\n", zSvcState);
    /* Cleanup */
    fossil_free(pSvcConfig);
    fossil_free(pSvcDescr);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
  }else
  if( strncmp(zMethod, "start", n)==0 ){
    SC_HANDLE hScm;
    SC_HANDLE hSvc;
    SERVICE_STATUS sstat;
    char *zErrFmt = "unable to start service '%s': %s";

    verify_all_options();
    if( g.argc>4 ) fossil_fatal("to much arguments for start method.");
    hScm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if( !hScm ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    hSvc = OpenService(hScm, fossil_utf8_to_mbcs(zSvcName), SERVICE_ALL_ACCESS);
    if( !hSvc ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    QueryServiceStatus(hSvc, &sstat);
    if( sstat.dwCurrentState!=SERVICE_RUNNING ){
      fossil_print("Starting service '%s'", zSvcName);
      if( sstat.dwCurrentState!=SERVICE_START_PENDING ){
        if( !StartService(hSvc, 0, NULL) ){
          fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
        }
      }
      while( sstat.dwCurrentState!=SERVICE_RUNNING ){
        Sleep(100);
        fossil_print(".");
        QueryServiceStatus(hSvc, &sstat);
      }
      fossil_print("\nService '%s' started.\n", zSvcName);
    }else{
      fossil_print("Service '%s' is already started.\n", zSvcName);
    }
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
  }else
  if( strncmp(zMethod, "stop", n)==0 ){
    SC_HANDLE hScm;
    SC_HANDLE hSvc;
    SERVICE_STATUS sstat;
    char *zErrFmt = "unable to stop service '%s': %s";

    verify_all_options();
    if( g.argc>4 ) fossil_fatal("to much arguments for stop method.");
    hScm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if( !hScm ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    hSvc = OpenService(hScm, fossil_utf8_to_mbcs(zSvcName), SERVICE_ALL_ACCESS);
    if( !hSvc ) fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
    QueryServiceStatus(hSvc, &sstat);
    if( sstat.dwCurrentState!=SERVICE_STOPPED ){
      fossil_print("Stopping service '%s'", zSvcName);
      if( sstat.dwCurrentState!=SERVICE_STOP_PENDING ){
        if( !ControlService(hSvc, SERVICE_CONTROL_STOP, &sstat) ){
          fossil_fatal(zErrFmt, zSvcName, win32_get_last_errmsg());
        }
      }
      while( sstat.dwCurrentState!=SERVICE_STOPPED ){
        Sleep(100);
        fossil_print(".");
        QueryServiceStatus(hSvc, &sstat);
      }
      fossil_print("\nService '%s' stopped.\n", zSvcName);
    }else{
      fossil_print("Service '%s' is already stopped.\n", zSvcName);
    }
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
  }else
  {
    fossil_fatal("METHOD should be one of:"
                 " create delete show start stop");
  }
  return;
}

#endif /* _WIN32  -- This code is for win32 only */

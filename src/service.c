/* Redis Windows Service.
 *
 * Copyright (c) 2011, Rui Lopes <rgl at ruilopes dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * TODO get g_logLevel, g_redisHost and g_redisPort from the redis conf file
 * TODO base g_fileLogPath in the logfile stanza of the redis conf file (snuck "-service" in it)
 * TODO create WiX installer
 * TODO use tcmalloc
 * TODO support redis auth command when connecting to redis
 * TODO periodically ping redis to check it health (maybe this is not really needed; time will tell!)
 */

#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <winsock2.h>
#include <unistd.h>
#include "win32fixes.h"
#include "hiredis.h"
#include "sds.h"

// yes, we are using global (module) variables! they are quite
// appropriate for this little service.

static int g_logLevel;
static char* g_fileLogPath;
static char* g_selfPath;
static char* g_redisConfPath;
static char* g_redisHost;
static int g_redisPort;
static char* g_serviceName;
static SERVICE_STATUS g_serviceStatus;
static SERVICE_STATUS_HANDLE g_serviceSatusHandle;
static HANDLE g_redisProcess;
static HANDLE g_stopEvent;

static void changeCurrentDirectoryToProcessImageDirectory(void);
static void serviceMain(int argc, char** argv);
static void serviceControlHandler(DWORD request);
static int registerServiceControlHandler(void);
static void setServiceStatus(DWORD state, DWORD exitCode);

static int startRedis(void);
static void shutdownRedis(void);


#define LOG_LEVEL_DEBUG     0
#define LOG_LEVEL_INFO      1
#define LOG_LEVEL_NOTICE    2
#define LOG_LEVEL_WARN      3
#define LOG_LEVEL_ERROR     4

#define LOG fileLog
#define LOG_DEBUG(...) LOG(LOG_LEVEL_DEBUG, ## __VA_ARGS__)
#define LOG_INFO(...) LOG(LOG_LEVEL_INFO, ## __VA_ARGS__)
#define LOG_NOTICE(...) LOG(LOG_LEVEL_NOTICE, ## __VA_ARGS__)
#define LOG_WARN(...) LOG(LOG_LEVEL_WARN, ## __VA_ARGS__)
#define LOG_ERROR(...) LOG(LOG_LEVEL_ERROR, ## __VA_ARGS__)

static int fileLog(int level, const char* format, ...);


int main(int argc, char** argv) {
    g_logLevel = LOG_LEVEL_DEBUG;
    g_fileLogPath = "redis-service.log";
    g_selfPath = argv[0];
    g_serviceName = argc > 1 ? argv[1] : "Redis";
    g_redisConfPath = argc > 2 ? argv[2] : "redis.conf";
    g_redisHost = "127.0.0.1";
    g_redisPort = 6379;

    LOG_DEBUG("Begin");

    changeCurrentDirectoryToProcessImageDirectory();

    SERVICE_TABLE_ENTRY serviceTable[2];

    serviceTable[0].lpServiceName = g_serviceName;
    serviceTable[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)serviceMain;

    serviceTable[1].lpServiceName = NULL;
    serviceTable[1].lpServiceProc = NULL;

    if (!StartServiceCtrlDispatcher(serviceTable)) {
        DWORD lastError = GetLastError();
        switch (lastError) {
            case ERROR_FAILED_SERVICE_CONTROLLER_CONNECT:
                printf("This is a Windows service, it cannot be started directly (it has to be installed).\n");
                printf("\n");
                printf("To start, install or uninstall run (as Administrator) one of the following commands:\n");
                printf("\n");
                printf("  net start %s\n", g_serviceName);
                printf("  sc create %s binPath= %s %s %s\n", g_serviceName, argv[0], g_serviceName, g_redisConfPath);
                printf("  sc delete %s\n", g_serviceName);
                return 1;

            case ERROR_INVALID_DATA:
                LOG_ERROR("Failed to StartServiceCtrlDispatcher (ERROR_INVALID_DATA)");
                return 2;

            case ERROR_SERVICE_ALREADY_RUNNING:
                LOG_ERROR("Failed to StartServiceCtrlDispatcher (ERROR_SERVICE_ALREADY_RUNNING)");
                return 2;

            default:
                LOG_ERROR("Failed to StartServiceCtrlDispatcher (unexpected LastError of %d)", lastError);
                return -1;
        }
    }

    LOG_DEBUG("End");
    return 0;
}


static void changeCurrentDirectoryToProcessImageDirectory(void) {
    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char ext[_MAX_EXT];

    _splitpath(g_selfPath, drive, dir, fname, ext);

    sds directory = sdscat(sdsnew(drive), dir);

    SetCurrentDirectory(directory);
    
    LOG_DEBUG("Changing current directory to %s", directory);

    sdsfree(directory);
}


static void serviceMain(int argc __attribute__((unused)), char** argv __attribute__((unused))) {
    LOG_DEBUG("Begin Service");
    
    if (!w32initWinSock()) {
        LOG_ERROR("Failed to initialize sockets");
        setServiceStatus(SERVICE_STOPPED, (DWORD)-1);
        return;
    }

    g_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    
    if (g_stopEvent == NULL) {
        LOG_ERROR("Failed to create stop event");
        setServiceStatus(SERVICE_STOPPED, (DWORD)-1);
        return;
    }

    if (registerServiceControlHandler()) {
        LOG_ERROR("Failed to register the service control handler");
        CloseHandle(g_stopEvent);
        setServiceStatus(SERVICE_STOPPED, (DWORD)-1);
        return;
    }

    setServiceStatus(SERVICE_RUNNING, 0);

    LOG_NOTICE("Starting redis (host=%s port=%d)", g_redisHost, g_redisPort);

    if (startRedis()) {
        return;
    }

    LOG_NOTICE("Started redis (host=%s port=%d PID=%d)", g_redisHost, g_redisPort, GetProcessId(g_redisProcess));

    HANDLE waitObjects[2] = { g_redisProcess, g_stopEvent };

    DWORD waitResult = WaitForMultipleObjects(2, waitObjects, FALSE, INFINITE);

    switch (waitResult) {
        case WAIT_OBJECT_0: {
                DWORD exitCode = -1;
                GetExitCodeProcess(g_redisProcess, &exitCode);
                LOG_ERROR("Redis has been shutdown (exitCode=%u); but we didn't ask it to shutdown. check if the configuration file exists and is valid.", exitCode);
                break;
            }

        case WAIT_OBJECT_0 + 1:
            // we were asked to shutdown.
            break;

        default:
            LOG_ERROR("Failed to WaitForMultipleObjects");
            break;
    }

    if (waitResult != WAIT_OBJECT_0) {
        LOG_NOTICE("Stopping redis (PID=%d)", GetProcessId(g_redisProcess));
        shutdownRedis();
    }

    CloseHandle(g_stopEvent);
    g_stopEvent = NULL;
    
    CloseHandle(g_redisProcess);
    g_redisProcess = NULL;

    setServiceStatus(SERVICE_STOPPED, 0);

    LOG_DEBUG("End Service");
}


static void serviceControlHandler(DWORD request) {
    switch (request) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            SetEvent(g_stopEvent);
            g_serviceStatus.dwWin32ExitCode = 0;
            g_serviceStatus.dwCurrentState  = SERVICE_STOP_PENDING;
            break;
    }

    SetServiceStatus(g_serviceSatusHandle, &g_serviceStatus);
}


static int registerServiceControlHandler(void) {
    g_serviceStatus.dwServiceType       = SERVICE_WIN32;
    g_serviceStatus.dwCurrentState      = SERVICE_START_PENDING;
    g_serviceStatus.dwControlsAccepted  = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    g_serviceSatusHandle = RegisterServiceCtrlHandler(g_serviceName, (LPHANDLER_FUNCTION)serviceControlHandler);

    return ((SERVICE_STATUS_HANDLE)0 == g_serviceSatusHandle) ? -1 : 0;
}


static void setServiceStatus(DWORD state, DWORD exitCode) {
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = exitCode;
    SetServiceStatus(g_serviceSatusHandle, &g_serviceStatus);
}


static redisContext* connectRedis(void) {
    redisContext* redisContext = redisConnect(g_redisHost, g_redisPort);

    if (redisContext->err) {
        LOG_ERROR("Failed connect to Redis at %s:%d: %s", g_redisHost, g_redisPort, redisContext->errstr);
        redisFree(redisContext);
        return NULL;
    }

    return redisContext;
}


static int startRedis(void) {
    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    ZeroMemory(&pi, sizeof(pi));

    g_redisProcess = NULL;

    sds commandLine = sdscatprintf(sdsempty(), "%s %s", g_serviceName, g_redisConfPath);

    if (!CreateProcess(
        "redis-server.exe",
        commandLine,
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS,
        NULL,
        NULL,
        &si,
        &pi
    )) {
        sdsfree(commandLine);
        LOG_ERROR("Failed to CreateProcess");
        return -1;
    }

    sdsfree(commandLine);

    CloseHandle(pi.hThread);

    g_redisProcess = pi.hProcess;

    return 0;
}


static void shutdownRedis(void) {
    redisContext* redisContext = connectRedis();
    
    if (redisContext == NULL) {
        LOG_ERROR("Failed to shutdown redis (cound not connect to it)");
        return;
    }

    redisReply *reply = redisCommand(redisContext, "SHUTDOWN");

    if (reply != NULL) {
        freeReplyObject(reply);
    }

    redisFree(redisContext);
}

static int fileLog(int level, const char* format, ...) {
    if (level < g_logLevel)
        return 0;

    FILE* log = fopen(g_fileLogPath, "a+");
    if (log == NULL)
        return -1;

    time_t now = time(NULL);

    char timeBuffer[64];

    strftime(timeBuffer, sizeof(timeBuffer), "%d %b %H:%M:%S", localtime(&now));

    const char *levelChar = ".-*##";

    fprintf(log, "[%d] %s %c ", getpid(), timeBuffer, levelChar[level]);

    va_list va;
    va_start(va, format);
    vfprintf(log, format, va);
    va_end(va);

    fprintf(log, "\n");

    fclose(log);

    return 0;
}

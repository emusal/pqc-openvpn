/*
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED
 * TO THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * Copyright (C) 1993 - 2000.  Microsoft Corporation.  All rights reserved.
 *                      2013 Heiko Hund <heiko.hund@sophos.com>
 * Portions Copyright (C) 2018 Microsoft Corporation
 */

#include "service.h"

#include <windows.h>
#include <stdio.h>
#include <process.h>


openvpn_service_t openvpn_service[_service_max];


BOOL
ReportStatusToSCMgr(SERVICE_STATUS_HANDLE service, SERVICE_STATUS *status)
{
    static DWORD dwCheckPoint = 1;
    BOOL res = TRUE;

    if (status->dwCurrentState == SERVICE_START_PENDING)
    {
        status->dwControlsAccepted = 0;
    }
    else
    {
        status->dwControlsAccepted = SERVICE_ACCEPT_STOP;
    }

    if (status->dwCurrentState == SERVICE_RUNNING
        || status->dwCurrentState == SERVICE_STOPPED)
    {
        status->dwCheckPoint = 0;
    }
    else
    {
        status->dwCheckPoint = dwCheckPoint++;
    }

    /* Report the status of the service to the service control manager. */
    res = SetServiceStatus(service, status);
    if (!res)
    {
        MsgToEventLog(MSG_FLAGS_ERROR, TEXT("SetServiceStatus"));
    }

    return res;
}

static const TCHAR DialerDllName[] = TEXT("libopenvpndialer-0.dll");
static const TCHAR RegValueName[] = TEXT("CustomDLL");

static int
HandleDialerRegistration(int uninstall)
{
    TCHAR path[512], customDllString[1024];
    HKEY parametersKey;
    LONG result;
    DWORD customDllSize = sizeof(customDllString);

    /* Assumption is that the dialer DLL is installed to the same bin directory as everything else. */
    if (GetModuleFileName(NULL, path, sizeof(path)) == 0)
    {
        _tprintf(TEXT("Unable to get module path - %lu\n"), GetLastError());
        return 1;
    }

    /* The version of NSIS we use to create the installer doesn't yet have support for 
     * writing multi-string registry entries, which we need to do in order to register our
     * custom dialer DLL. Instead, we'll update this registry entry on install/uninstall.
     */

    TCHAR *lastBackslash = _tcsrchr(path, TEXT('\\'));
    if (NULL == lastBackslash)
    {
        _tprintf(TEXT("Could not locate last backslash in path: %s\n"), path);
        return 1;
    }

    lastBackslash[1] = TEXT('\0');

    /* Bounds checking. */
    if ((_tcslen(path) + _countof(DialerDllName)) > _countof(path))
    {
        _tprintf(TEXT("Out of buffer adding dialer filename to path"));
        return 1;
    }

    _tcscat(path, DialerDllName);

    result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, 
        TEXT("SYSTEM\\CurrentControlSet\\Services\\RasMan\\Parameters"), 
        0, 
        KEY_ALL_ACCESS,
        &parametersKey);
    if (ERROR_SUCCESS != result)
    {
        _tprintf(TEXT("Could not open RasMan parameters key: %ld\n"), result);
        return 1;
    }

    /* Must RegCloseKey(parametersKey) from this point */

    result = RegGetValue(parametersKey,
        NULL,
        RegValueName,
        RRF_RT_REG_MULTI_SZ,
        NULL,
        customDllString,
        &customDllSize);

    /* If we're installing, the key being absent is okay. Any other error is fatal. */
    if (ERROR_FILE_NOT_FOUND == result)
    {
        if (0 != uninstall)
        {
            _tprintf(TEXT("CustomDLL value was not found; skipping unregistration step\n"));
            RegCloseKey(parametersKey);
            return 0; /* Not a fatal error but nothing to do */
        }
        else
        {
            result = ERROR_SUCCESS;
            customDllString[0] = TEXT('\0');
            customDllSize = 0;
        }
    }
    else if (ERROR_SUCCESS != result)
    {
        _tprintf(TEXT("Could not open CustomDLL value: %ld\n"), result);
        RegCloseKey(parametersKey);
        return 1;
    }

    /* Determine if the custom dialer DLL is present in the registry setting. This is a multi-string so we can't 
     * use _tcsstr.
     */
    TCHAR* p;
    for (p = customDllString; *p != TEXT('\0'); p += _tcslen(p) + 1)
    {
        if (0 == _tcsicmp(path, p))
        {
            /* It is. p points to where it begins. */
            break;
        }
    }
    /* If it's not present, p points at a NULL terminator. */

    /* If we're installing and the DLL is present in the registry, do nothing. */
    /* If we're uninstalling and the DLL is not present in the registry, do nothing. */
    if ( ((0 == uninstall) && (*p != TEXT('\0'))) ||
         ((0 != uninstall) && (*p == TEXT('\0'))) )
    {
        RegCloseKey(parametersKey);
        return 0;
    }

    /* If we're installing and the DLL is not present in the registry, append it at point p. */
    if ((0 == uninstall) && (*p == TEXT('\0')))
    {
        /* Make sure the string buffer has enough space left. p points at the second in the double-terminating null
         * (or the sole NULL if the registry entry was empty or absent).
         */
        if (((p - customDllString) + _tcslen(path) + 2) > _countof(customDllString))
        {
            _tprintf(TEXT("Not enough buffer to create new CustomDLL string\n"));
            RegCloseKey(parametersKey);
            return 1;
        }
        _tcscpy(p, path);
        /* Add second terminating NULL we overwrote with the copy. */
        p += _tcslen(p) + 1;
        *p++ = TEXT('\0');

        /* Set in the registry. */
        result = RegSetValueEx(parametersKey,
            RegValueName,
            0,
            REG_MULTI_SZ,
            (const BYTE*)customDllString,
            (DWORD)(sizeof(TCHAR) * (p - customDllString)));

        RegCloseKey(parametersKey);
        if (ERROR_SUCCESS != result)
        {
            _tprintf(TEXT("Failed to RegSetValueEx: %ld\n"), result);
            return 1;
        }
    }
    /* Else if we're uninstalling and the DLL is present, copy everything except strings matching path. */
    else if ((0 != uninstall) && (*p != TEXT('\0')))
    {
        TCHAR newCustomDllString[1024];
        TCHAR *n, *c;

        /* newCustomDllString is the same size as customDllString, and we're only removing, so no
         * way to overflow the buffer.
         */
        for (c = customDllString, n = newCustomDllString; *c != TEXT('\0'); c += _tcslen(c) + 1)
        {
            if (0 != _tcsicmp(path, c))
            {
                _tcscpy(n, c);
                n += _tcslen(n) + 1;
            }
        }

        /* Add double-terminating NULL as is required for a multi-string. */
        *n++ = TEXT('\0');

        /* If the new string is nonempty, set in the registry. Otherwise delete the value. */
        if (TEXT('\0') != *newCustomDllString)
        {
            result = RegSetValueEx(parametersKey,
                RegValueName,
                0,
                REG_MULTI_SZ,
                (const BYTE*)newCustomDllString,
                (DWORD)(sizeof(TCHAR) * (n - newCustomDllString)));
        }
        else
        {
            result = RegDeleteValue(parametersKey, RegValueName);
        }

        RegCloseKey(parametersKey);
        if (ERROR_SUCCESS != result)
        {
            _tprintf(TEXT("Failed to %s new registry value: %ld\n"), 
                (TEXT('\0') != *newCustomDllString)?TEXT("set"):TEXT("delete"),
                result);
            return 1;
        }
    }

    return 0;
}

static int
CmdInstallServices()
{
    SC_HANDLE service;
    SC_HANDLE svc_ctl_mgr;
    TCHAR path[512];
    int i, ret = _service_max;

    if (GetModuleFileName(NULL, path + 1, 510) == 0)
    {
        _tprintf(TEXT("Unable to install service - %s\n"), GetLastErrorText());
        return 1;
    }

    path[0] = TEXT('\"');
    _tcscat(path, TEXT("\""));

    svc_ctl_mgr = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (svc_ctl_mgr == NULL)
    {
        _tprintf(TEXT("OpenSCManager failed - %s\n"), GetLastErrorText());
        return 1;
    }

    if (0 != HandleDialerRegistration(0))
    {
        _tprintf(TEXT("HandleDialerRegistration failed\n"));
        ret = 1;
    }
    else
    {
        for (i = 0; i < _service_max; i++)
        {
            service = CreateService(svc_ctl_mgr,
                                    openvpn_service[i].name,
                                    openvpn_service[i].display_name,
                                    SERVICE_QUERY_STATUS,
                                    SERVICE_WIN32_SHARE_PROCESS,
                                    openvpn_service[i].start_type,
                                    SERVICE_ERROR_NORMAL,
                                    path, NULL, NULL,
                                    openvpn_service[i].dependencies,
                                    NULL, NULL);
            if (service)
            {
                _tprintf(TEXT("%s installed.\n"), openvpn_service[i].display_name);
                CloseServiceHandle(service);
                --ret;
            }
            else
            {
                _tprintf(TEXT("CreateService failed - %s\n"), GetLastErrorText());
            }
        }
    }

    CloseServiceHandle(svc_ctl_mgr);
    return ret;
}


static int
CmdStartService(openvpn_service_type type)
{
    int ret = 1;
    SC_HANDLE svc_ctl_mgr;
    SC_HANDLE service;

    svc_ctl_mgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (svc_ctl_mgr == NULL)
    {
        _tprintf(TEXT("OpenSCManager failed - %s\n"), GetLastErrorText());
        return 1;
    }

    service = OpenService(svc_ctl_mgr, openvpn_service[type].name, SERVICE_ALL_ACCESS);
    if (service)
    {
        if (StartService(service, 0, NULL))
        {
            _tprintf(TEXT("Service Started\n"));
            ret = 0;
        }
        else
        {
            _tprintf(TEXT("StartService failed - %s\n"), GetLastErrorText());
        }

        CloseServiceHandle(service);
    }
    else
    {
        _tprintf(TEXT("OpenService failed - %s\n"), GetLastErrorText());
    }

    CloseServiceHandle(svc_ctl_mgr);
    return ret;
}


static int
CmdRemoveServices()
{
    SC_HANDLE service;
    SC_HANDLE svc_ctl_mgr;
    SERVICE_STATUS status;
    int i, ret = _service_max;

    svc_ctl_mgr = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (svc_ctl_mgr == NULL)
    {
        _tprintf(TEXT("OpenSCManager failed - %s\n"), GetLastErrorText());
        return 1;
    }

    if (0 != HandleDialerRegistration(1))
    {
        _tprintf(TEXT("HandleDialerRegistration uninstall failed; ignoring\n"));
    }

    for (i = 0; i < _service_max; i++)
    {
        openvpn_service_t *ovpn_svc = &openvpn_service[i];
        service = OpenService(svc_ctl_mgr, ovpn_svc->name,
                              DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (service == NULL)
        {
            _tprintf(TEXT("OpenService failed - %s\n"), GetLastErrorText());
            goto out;
        }

        /* try to stop the service */
        if (ControlService(service, SERVICE_CONTROL_STOP, &status))
        {
            _tprintf(TEXT("Stopping %s."), ovpn_svc->display_name);
            Sleep(1000);

            while (QueryServiceStatus(service, &status))
            {
                if (status.dwCurrentState == SERVICE_STOP_PENDING)
                {
                    _tprintf(TEXT("."));
                    Sleep(1000);
                }
                else
                {
                    break;
                }
            }

            if (status.dwCurrentState == SERVICE_STOPPED)
            {
                _tprintf(TEXT("\n%s stopped.\n"), ovpn_svc->display_name);
            }
            else
            {
                _tprintf(TEXT("\n%s failed to stop.\n"), ovpn_svc->display_name);
            }
        }

        /* now remove the service */
        if (DeleteService(service))
        {
            _tprintf(TEXT("%s removed.\n"), ovpn_svc->display_name);
            --ret;
        }
        else
        {
            _tprintf(TEXT("DeleteService failed - %s\n"), GetLastErrorText());
        }

        CloseServiceHandle(service);
    }

out:
    CloseServiceHandle(svc_ctl_mgr);
    return ret;
}


int
_tmain(int argc, TCHAR *argv[])
{
    SERVICE_TABLE_ENTRY dispatchTable[] = {
        { automatic_service.name, ServiceStartAutomatic },
        { interactive_service.name, ServiceStartInteractive },
        { NULL, NULL }
    };

    openvpn_service[0] = automatic_service;
    openvpn_service[1] = interactive_service;

    if (argc > 1 && (*argv[1] == TEXT('-') || *argv[1] == TEXT('/')))
    {
        if (_tcsicmp(TEXT("install"), argv[1] + 1) == 0)
        {
            return CmdInstallServices();
        }
        else if (_tcsicmp(TEXT("remove"), argv[1] + 1) == 0)
        {
            return CmdRemoveServices();
        }
        else if (_tcsicmp(TEXT("start"), argv[1] + 1) == 0)
        {
            BOOL is_auto = argc < 3 || _tcsicmp(TEXT("interactive"), argv[2]) != 0;
            return CmdStartService(is_auto ? automatic : interactive);
        }
        else
        {
            goto dispatch;
        }

        return 0;
    }

    /* If it doesn't match any of the above parameters
     * the service control manager may be starting the service
     * so we must call StartServiceCtrlDispatcher
     */
dispatch:
    _tprintf(TEXT("%s -install        to install the services\n"), APPNAME);
    _tprintf(TEXT("%s -start <name>   to start a service (\"automatic\" or \"interactive\")\n"), APPNAME);
    _tprintf(TEXT("%s -remove         to remove the services\n"), APPNAME);
    _tprintf(TEXT("\nStartServiceCtrlDispatcher being called.\n"));
    _tprintf(TEXT("This may take several seconds. Please wait.\n"));

    if (!StartServiceCtrlDispatcher(dispatchTable))
    {
        MsgToEventLog(MSG_FLAGS_ERROR, TEXT("StartServiceCtrlDispatcher failed."));
    }

    return 0;
}

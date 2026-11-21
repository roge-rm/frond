/* RAPI 2 file calls from synce-core 0.17. See licences/synce-LICENSE. */

#include "rapi_buffer.h"
#include "rapi_context.h"

#include <string.h>

#include <stdlib.h>

HANDLE _CeCreateFile2(
        RapiContext *context,
        LPCWSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD dwCreationDisposition,
        DWORD dwFlagsAndAttributes,
        HANDLE hTemplateFile)
{
    HANDLE handle = INVALID_HANDLE_VALUE;

    synce_trace("begin");

    rapi_context_begin_command(context, 0x16);
    rapi2_buffer_write_string(context->send_buffer, lpFileName);
    rapi_buffer_write_uint32(context->send_buffer, dwDesiredAccess);
    rapi_buffer_write_uint32(context->send_buffer, dwShareMode);
    rapi_buffer_write_uint32(context->send_buffer, dwCreationDisposition);
    rapi_buffer_write_uint32(context->send_buffer, dwFlagsAndAttributes);
    rapi_buffer_write_uint32(context->send_buffer, hTemplateFile);

    if ( !rapi2_context_call(context) )
        return INVALID_HANDLE_VALUE;

    rapi_buffer_read_uint32(context->recv_buffer, &context->last_error);
    rapi_buffer_read_uint32(context->recv_buffer, &handle);

    return handle;
}

BOOL _CeWriteFile2(
        RapiContext *context,
        HANDLE hFile,
        LPCVOID lpBuffer,
        DWORD nNumberOfBytesToWrite,
        LPDWORD lpNumberOfBytesWritten,
        LPOVERLAPPED lpOverlapped)
{
    BOOL return_value = 0;
    uint32_t bytes_written = 0;

    synce_trace("begin");

    rapi_context_begin_command(context, 0x18);
    rapi_buffer_write_uint32(context->send_buffer, hFile);
    rapi_buffer_write_uint32(context->send_buffer, nNumberOfBytesToWrite);
    rapi_buffer_write_data(context->send_buffer, lpBuffer, nNumberOfBytesToWrite);
/*    rapi_buffer_write_optional_in(context->send_buffer, lpBuffer, nNumberOfBytesToWrite);
    rapi_buffer_write_optional_in(context->send_buffer, NULL, 0);*/ /* lpOverlapped */

    if ( !rapi2_context_call(context) )
        return false;

    rapi_buffer_read_uint32(context->recv_buffer, &context->last_error);
    rapi_buffer_read_uint32(context->recv_buffer, &return_value);

    rapi_buffer_read_uint32(context->recv_buffer, &bytes_written);
    if (lpNumberOfBytesWritten)
        *lpNumberOfBytesWritten = bytes_written;

    return return_value;
}

BOOL _CeCloseHandle2(
        RapiContext *context,
        HANDLE hObject)
{
    BOOL return_value = 0;

    synce_trace("begin");

    rapi_context_begin_command(context, 0x19);
    rapi_buffer_write_uint32(context->send_buffer, hObject);

    if ( !rapi2_context_call(context) )
        return false;

    rapi_buffer_read_uint32(context->recv_buffer, &context->last_error);
    rapi_buffer_read_uint32(context->recv_buffer, &return_value);

    return return_value;
}

BOOL _CeDeleteFile2(
        RapiContext *context,
        LPCWSTR lpFileName)
{
    BOOL return_value = 0;

    rapi_context_begin_command(context, 0x2d);
    /*    rapi_buffer_write_optional_string(context->send_buffer, lpFileName);*/
    rapi2_buffer_write_string(context->send_buffer, lpFileName);

    if ( !rapi2_context_call(context) )
        return 0;

    rapi_buffer_read_uint32(context->recv_buffer, &context->last_error);
    rapi_buffer_read_uint32(context->recv_buffer, &return_value);

    return return_value;
}

BOOL _CeCreateProcess2(
        RapiContext *context,
        LPCWSTR lpApplicationName,
        LPCWSTR lpCommandLine,
        void* lpProcessAttributes,
        void* lpThreadAttributes,
        BOOL bInheritHandles,
        DWORD dwCreationFlags,
        LPVOID lpEnvironment,
        LPWSTR lpCurrentDirectory,
        void* lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation)
{
    BOOL result = false;

    rapi_context_begin_command(context, 0x2a);
    rapi2_buffer_write_string(context->send_buffer, lpApplicationName);
    rapi2_buffer_write_string(context->send_buffer, lpCommandLine);
    rapi_buffer_write_uint32(context->send_buffer, 0);
    rapi_buffer_write_uint32(context->send_buffer, 0);
    rapi_buffer_write_uint32(context->send_buffer, 0);
    rapi_buffer_write_uint32(context->send_buffer, dwCreationFlags);
    rapi_buffer_write_uint32(context->send_buffer, 0);
    rapi_buffer_write_uint32(context->send_buffer, 0);
    rapi_buffer_write_uint32(context->send_buffer, 0);
    /*    rapi_buffer_write_optional_out(context->send_buffer, lpProcessInformation, sizeof(PROCESS_INFORMATION)); */

    if ( !rapi2_context_call(context) )
        goto exit;
	
    rapi_buffer_read_uint32(context->recv_buffer, &context->last_error);
    synce_trace("last_error = %i", context->last_error);
    rapi_buffer_read_uint32(context->recv_buffer, &result);
    synce_trace("result = %i", result);

	if ( !rapi_buffer_read_data(context->recv_buffer, lpProcessInformation, sizeof(PROCESS_INFORMATION)) ){
		synce_error("Failed to read process information");
		return false;
	}
    if (lpProcessInformation)
    {
        lpProcessInformation->hProcess     = letoh32(lpProcessInformation->hProcess);
        lpProcessInformation->hThread      = letoh32(lpProcessInformation->hThread);
        lpProcessInformation->dwProcessId  = letoh32(lpProcessInformation->dwProcessId);
        lpProcessInformation->dwThreadId   = letoh32(lpProcessInformation->dwThreadId);
    }

exit:
        return result;
}

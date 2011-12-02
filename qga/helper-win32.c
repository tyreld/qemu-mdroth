#include "guest-agent-core.h"
#include <basetyps.h>
#include <wtypes.h>
#include <initguid.h>
#include <setupapi.h>
#include <glib.h>

DEFINE_GUID(GUID_VIOSERIAL_PORT,
            0x6fde7521, 0x1b65, 0x48ae, 0xb6, 0x28, 0x80, 0xbe, 0x62, 0x1, 0x60, 0x26);

char *get_vioserial_path(void)
{
    HDEVINFO dev_info;
    SP_DEVICE_INTERFACE_DATA interface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA interface_details;
    BOOL bret;
    ULONG len, req_len = 0;

    dev_info = SetupDiGetClassDevs((LPGUID)&GUID_VIOSERIAL_PORT,
                                   NULL, NULL,
                                   (DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)); 
    if (dev_info == INVALID_HANDLE_VALUE) {
        g_warning("failed to retrieve device classes");
        return NULL;
    }

    interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    bret = SetupDiEnumDeviceInterfaces(dev_info, 0,
                                      (LPGUID)&GUID_VIOSERIAL_PORT,
                                      0, &interface_data);
    if (!bret) {
        g_warning("failed to enumerate device device interfaces");
        SetupDiDestroyDeviceInfoList(dev_info);
        return NULL;
    }

    SetupDiGetDeviceInterfaceDetail(dev_info, &interface_data, NULL, 0,
                                    &req_len, NULL);

    interface_details = (PSP_DEVICE_INTERFACE_DETAIL_DATA) LocalAlloc(LMEM_FIXED, req_len);
    if (interface_details == NULL) {
        g_warning("memory allocation failed");
        SetupDiDestroyDeviceInfoList(dev_info);
        return NULL;
    }

    interface_details->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    len = req_len;
    bret = SetupDiGetDeviceInterfaceDetail(dev_info, &interface_data,
                                          interface_details, len, &req_len,
                                          NULL);
    if (!bret) {
        g_warning("failed to retrieve device interface details");
        SetupDiDestroyDeviceInfoList(dev_info);
        LocalFree(interface_details);
        return NULL;
    }

    return (char *)interface_details->DevicePath;
}

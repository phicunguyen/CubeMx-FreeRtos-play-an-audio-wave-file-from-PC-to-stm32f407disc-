/**
 * @file aardvark.c
 *
 * @brief register access APIs
 *
 *****************************************************************************/

/***** #include statements ***************************************************/

#include <conio.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <tchar.h>
#include <setupapi.h>
#include <locale.h>
#include <sys/stat.h>
#include <winreg.h>
#include <devguid.h>    // for GUID_DEVCLASS_CDROM etc
#include <cfgmgr32.h>   // for MAX_DEVICE_ID_LEN, CM_Get_Parent and CM_Get_Device_ID
#include <tchar.h>
#include <psapi.h>
#include <stdbool.h>
#pragma comment( lib, "advapi32" )

#include "serial.h"

/***** local macro definitions ***********************************************/
#define INITGUID
#ifdef DEFINE_DEVPROPKEY
#undef DEFINE_DEVPROPKEY
#endif
#ifdef INITGUID
#define DEFINE_DEVPROPKEY(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8, pid) EXTERN_C const DEVPROPKEY DECLSPEC_SELECTANY name = { { l, w1, w2, { b1, b2,  b3,  b4,  b5,  b6,  b7,  b8 } }, pid }
#else
#define DEFINE_DEVPROPKEY(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8, pid) EXTERN_C const DEVPROPKEY name
#endif // INITGUID

#define MAX_NUM_OF_COMP			10

// include DEVPKEY_Device_BusReportedDeviceDesc from WinDDK\7600.16385.1\inc\api\devpkey.h
DEFINE_DEVPROPKEY(DEVPKEY_Device_BusReportedDeviceDesc, 0x540b947e, 0x8b40, 0x45bc, 0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2, 4);     // DEVPROP_TYPE_STRING
DEFINE_DEVPROPKEY(DEVPKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);    // DEVPROP_TYPE_STRING

#define ARRAY_SIZE(arr)     (sizeof(arr)/sizeof(arr[0]))

#pragma comment (lib, "setupapi.lib")

typedef BOOL(WINAPI *FN_SetupDiGetDevicePropertyW)(
	__in       HDEVINFO DeviceInfoSet,
	__in       PSP_DEVINFO_DATA DeviceInfoData,
	__in       const DEVPROPKEY *PropertyKey,
	__out      DEVPROPTYPE *PropertyType,
	__out_opt  PBYTE PropertyBuffer,
	__in       DWORD PropertyBufferSize,
	__out_opt  PDWORD RequiredSize,
	__in       DWORD Flags
	);


#define MAX_ID_SIZE		64

typedef struct {
	bool		sema;
	bool		supspense;
	bool		port_avail;
	uint8_t		port_num;
	char		port_name[MAX_ID_SIZE];
} Comport_Id_t;

typedef struct {
	uint8_t port_cnt;
	Comport_Id_t port_id[MAX_NUM_OF_COMP];
	uint8_t port_update;
} Comport_List_t;

static Comport_List_t port_list;
uint8_t buffer[4096 + 4 + 8 + 8];

/***** local type definitions ************************************************/

/***** local data objects ****************************************************/

/***** local prototypes ******************************************************/
static bool s_setup_comport_property(struct AdapterTypedef_t *p_adapter);
static bool s_ser_port_open(struct AdapterTypedef_t  *p_adapter);
static bool s_monitor(struct AdapterTypedef_t *p_adapter);
static void s_get_list_devices(void);
static bool s_update_devices(void);
static bool is_port_open(struct AdapterTypedef_t *p_adapter);
static void s_write(struct AdapterTypedef_t *p_adapter, uint8_t *p_buf, uint32_t size);
static void s_read(struct AdapterTypedef_t *p_adapter, uint8_t *p_buf, uint32_t size);
static void s_flash(struct AdapterTypedef_t *p_adapter, uint8_t *p_buf, uint32_t size, uint32_t addr);

/***** public functions ******************************************************/

void serial_devices_get(char **p_connection[]) {
	//copy the name of the comport to upstream.
	for (int i = 0; i < port_list.port_cnt; i++) {
		//uint8_t port_num = port_list.port_id[i].port_num;
		if (port_list.port_id[i].port_avail) {
			*p_connection = (char **)malloc(sizeof(char) * MAX_ID_SIZE);
			memcpy(*p_connection, port_list.port_id[i].port_name, MAX_ID_SIZE);
			p_connection++;
		}
	}
	*p_connection = NULL;
}

void serial_devices_create(struct AdapterTypedef_t *p_adapter, user_callback callback, char *portname) {
	p_adapter->p_name		= portname;
	p_adapter->ser_mon		= s_monitor;
	p_adapter->write		= s_write;
	p_adapter->read			= s_read;
	p_adapter->flash		= s_flash;
	p_adapter->is_port_open = is_port_open;
	p_adapter->callback		= callback;
	p_adapter->bufindx		= 0;
	p_adapter->Handle		= -1;
}

/*
* open the comport
*/
bool serial_devices_open(struct AdapterTypedef_t *p_adapter) {
	return s_ser_port_open(p_adapter);
}

/*
* check if the comport has been added or removed from window.
*/
bool serial_devices_mon(void) {

	//get the list of comports
	s_get_list_devices();
	return s_update_devices();
}


/***** local data objects ****************************************************/

/*
* write data to serial port
*/

static void s_send(struct AdapterTypedef_t *p_adapter, uint8_t *p_buf, uint32_t size) {
	LPDWORD iBytesWritten;
	WriteFile((HANDLE)p_adapter->Handle, p_buf, (DWORD)size, (LPDWORD)&iBytesWritten, NULL);
}

/*
* read data to serial port
*/
static void s_read(struct AdapterTypedef_t *p_adapter, uint8_t *p_buf, uint32_t size) {
	LPDWORD iBytesWritten;
	do
	{
		ReadFile((HANDLE)p_adapter->Handle, p_buf, (DWORD)size, (LPDWORD)&iBytesWritten, NULL);
	} while (iBytesWritten > 0);
}

/*
* return ascii
*/
static uint8_t hex2ascii(uint8_t val) {
	uint8_t res=0;
	if (val < 10) {
		res = val + 0x30;
	} else {
		res = val - 10 + 0x41; //0x41=='a'
	}
	return res;
}

/*
* convert one bytes of hex data to two ascii.
*/
static void s_hex_to_ascci(uint8_t **pbuf, uint8_t *pdata, uint16_t size) {
	uint8_t *p_data = *pbuf;
	for (int i = 0; i < size; i++) {
		*p_data++ = hex2ascii(*pdata  & 0xf);
		*p_data++ = hex2ascii((*pdata++ >> 4) & 0xf);
	}
	*pbuf = p_data;
}

/*
*
*/
static uint32_t s_packet_flash(uint16_t opcode, uint8_t *p_buf, uint32_t size, uint32_t addr) {
	uint8_t *p_data = buffer;
	//start of packet
	*p_data++ = '[';
	//opcode
	s_hex_to_ascci(&p_data, (uint8_t *)&opcode, sizeof(opcode));
	//addr
	s_hex_to_ascci(&p_data, (uint8_t *)&addr, sizeof(addr));
	//size
	s_hex_to_ascci(&p_data, (uint8_t *)&size, sizeof(size));
	//data
	s_hex_to_ascci(&p_data, p_buf, (uint16_t)size);
	//end of packet
	*p_data++ = ']';

	return (p_data - buffer);
}

/*
* compose the write packet with start/stop indication.
*/
static void s_write(struct AdapterTypedef_t *p_adapter, uint8_t *p_buf, uint32_t size) {
	uint8_t *p_data = buffer;

	//start of packet
	*p_data++ = '[';
	s_hex_to_ascci(&p_data, p_buf, size);
	//end of packet
	*p_data++ = ']';
	s_send(p_adapter, buffer, (uint16_t)(p_data - buffer));
}

static void s_flash(struct AdapterTypedef_t *p_adapter, uint8_t *p_buf, uint32_t size, uint32_t addr) {
	uint32_t len;
	if (addr == 0) {
		//send command to reboot the 
	}
	len = s_packet_flash(SERIAL_PACKET__FLASH_WRITE, p_buf, size, addr);
	s_send(p_adapter, buffer, len);
}

static bool is_port_open(struct AdapterTypedef_t *p_adapter) {
	return p_adapter->port_open;
}

/*
* convert two ascii bytes to one hex byte.
*/
static uint8_t ascii2hex(uint8_t val) {
	uint8_t res=0;
	if (val >= '0' && val <= '9') {
		res = val - '0';
	}
	else if (val >= 'a' && val <= 'f') {
		res = val - 'a' + 10;
	}
	else if (val >= 'A' && val <= 'F') {
		res = val - 'A' + 10;
	}

	return res;
}

static void s_callback(struct AdapterTypedef_t *p_adapter) {
	uint32_t index=0;
	uint8_t data;
	p_adapter->callback(p_adapter->buffer, &p_adapter->bufindx);
	for (uint32_t i = 0; i < p_adapter->bufindx; i+=2) {
		data = ascii2hex(p_adapter->buffer[i]);
		data |= ascii2hex(p_adapter->buffer[i+1]) << 4;
		p_adapter->buffer[index++] = data;
	}
	p_adapter->callback(p_adapter->buffer, index);
}

/*
* add the data receive from serial to the buffer
*/
static void s_add_data(struct AdapterTypedef_t *p_adapter, uint8_t data) {
	
	switch (data) {
	case '[': //begin of i2c packet receving
		p_adapter->bufindx = 0;
		p_adapter->valid = true;
		break;
	case ']': //end of i2c packet receving
		s_callback(p_adapter);
		p_adapter->bufindx = 0;
		p_adapter->valid = false;
		//printf("got reponse\n");
		break;
	default: //strore the data in the buffer
		if (p_adapter->valid) {
			p_adapter->buffer[p_adapter->bufindx++] = data;
		}
		else {
			printf("%c", data);
		}
		break;
	}
}

/*
* read the data from the comport
*/
static bool s_monitor(struct AdapterTypedef_t *p_adapter) {
	uint8_t  data;               
	DWORD bytesread = 0;   

	do {
		ReadFile((HANDLE)p_adapter->Handle, (char *)&data, 1, &bytesread, NULL);
		if (bytesread) {
			//printf("%c", data);
			s_add_data(p_adapter, data);
			if (kbhit()) {
				exit(1);
			}
		}
	} while (bytesread > 0);
	return 0;
}

/*
* set the time out for wating for a char return from serial port
*/
static bool s_set_timeout(struct AdapterTypedef_t *p_adapter, bool en) {
	COMMTIMEOUTS timeouts = { 0 };
	// Set COM port timeout settings
	// Set COM port timeout settings
	if (en) {
		timeouts.ReadIntervalTimeout = 0;
		timeouts.ReadTotalTimeoutConstant = 0;
		timeouts.ReadTotalTimeoutMultiplier = 1;
		timeouts.WriteTotalTimeoutConstant = 1;
		timeouts.WriteTotalTimeoutMultiplier = 10;
	}
	else {
		timeouts.ReadIntervalTimeout = 50;
		timeouts.ReadTotalTimeoutConstant = 50;
		timeouts.ReadTotalTimeoutMultiplier = 100;
		timeouts.WriteTotalTimeoutConstant = 1;
		timeouts.WriteTotalTimeoutMultiplier = 10;
	}

	if (SetCommTimeouts((HANDLE)p_adapter->Handle, &timeouts) == 0)
	{
		printf("Error setting timeouts\n");
		CloseHandle((HANDLE)p_adapter->Handle);
		return true;
	}
	return false;
}

/*
* return the comport number 
*/
static void get_port_id(struct AdapterTypedef_t *p_adapter, char *p_name) {
	p_adapter->port_id_sel = 0xFF;
	for (int i = 0; i < port_list.port_cnt; i++) {
		if (!memcmp(p_name, port_list.port_id[i].port_name, strlen(p_name))) {
			p_adapter->port_id_sel = port_list.port_id[i].port_num;
			break;
		}
	}
}

/*
* window set up for the comport baud rate etc.
*/
static bool s_setup_comport_property(struct AdapterTypedef_t *p_adapter) {
	BOOL Write_Status;
	DCB dcbSerialParams;					// Initializing DCB structure
	dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
	Write_Status = GetCommState((HANDLE)p_adapter->Handle, &dcbSerialParams);     //retreives  the current settings
	if (Write_Status == FALSE) {
		printf("\n   Error! in GetCommState()");
		CloseHandle((HANDLE)p_adapter->Handle);
		return false;
	}
	dcbSerialParams.BaudRate = CBR_115200;
	dcbSerialParams.ByteSize = 8;
	dcbSerialParams.fBinary = TRUE;
	dcbSerialParams.fErrorChar = FALSE;
	dcbSerialParams.fNull = FALSE;
	dcbSerialParams.fAbortOnError = FALSE;

	dcbSerialParams.Parity = NOPARITY;
	dcbSerialParams.fParity = FALSE;
	dcbSerialParams.StopBits = ONESTOPBIT;

	dcbSerialParams.fRtsControl = RTS_CONTROL_DISABLE;
	dcbSerialParams.fOutxCtsFlow = FALSE;
	dcbSerialParams.fDtrControl = DTR_CONTROL_DISABLE;
	dcbSerialParams.fOutxDsrFlow = FALSE;
	dcbSerialParams.fDsrSensitivity = FALSE;
	dcbSerialParams.fInX = FALSE;
	dcbSerialParams.fOutX = FALSE;

	Write_Status = SetCommState((HANDLE)p_adapter->Handle, &dcbSerialParams);  //Configuring the port according to settings in DCB

	if (Write_Status == FALSE)
	{
		printf("\n   Error! in Setting DCB Structure");
		CloseHandle((HANDLE)p_adapter->Handle);
		return false;
	}

	if (s_set_timeout(p_adapter, true)) {
		return false;
	}

	SetCommMask((HANDLE)p_adapter->Handle, EV_RXCHAR); //Configure Windows to Monitor the serial device for Character Reception
	p_adapter->port_open = true;
	//p_adapter->port_change = true;
	return true;
}

/*
* open the comport based on the user select
*/
static bool s_ser_port_open(struct AdapterTypedef_t  *p_adapter) {
	char port[25];
	bool result = false;
	if (port_list.port_cnt) {
		get_port_id(p_adapter, p_adapter->p_name);
		sprintf(port, "\\\\.\\COM%d", p_adapter->port_id_sel);
		p_adapter->Handle = (uint32_t)CreateFileA(port,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_WRITE,    // must be opened with exclusive-access
			NULL, // no security attributes
			OPEN_EXISTING, // must use OPEN_EXISTINGq
			FILE_ATTRIBUTE_NORMAL,
			NULL  // hTemplate must be NULL for comm devices
		);
		if (p_adapter->Handle == (unsigned long)INVALID_HANDLE_VALUE)
		{
			if (GetLastError() == ERROR_FILE_NOT_FOUND)
			{
				printf("cannot open port!");
				printf("cannot open com%d\n", p_adapter->port_id_sel);
			}
			puts("invalid handle value!");
		}
		else {
			if (s_setup_comport_property(p_adapter)) {
				p_adapter->port_open = true;
				result = true;
			}
		}
	}
	return result;
}

/*
* new port has been added or removed then notify the application.
*/
static bool s_update_devices(void) {
	if (port_list.port_cnt != port_list.port_update) {
		port_list.port_update = port_list.port_cnt;
		return true;
	}
	return false;
}

/*
* convert the comport to int from char
*/
static uint8_t convert_to_int(char *ptr) {
	uint8_t val = 0;
	while (*ptr) {
		if (isdigit(*ptr)) {
			val *= 10;
			val += *ptr - '0';
		}
		ptr++;
	}
	return val;
}

/*
* return the com port number.
*/
static uint8_t get_port_num(char *p_szBuffer) {
	uint8_t val = 0;
	char *ptr1;
	if ((ptr1 = strstr(p_szBuffer, "(COM")) != NULL) {
		val = convert_to_int(ptr1);
	}
	return val;
}

/* 
* check the port to see has been taken
* if not open then the handle should be greater than 0.
*
*/
static bool s_port_available(uint8_t port_id) {
	char port[32];
	int handle;

	sprintf(port, "\\\\.\\COM%d", port_id);
	handle = (uint32_t)CreateFileA(port,
		GENERIC_READ | GENERIC_WRITE,
		0,    // must be opened with exclusive-access
		NULL, // no security attributes
		OPEN_EXISTING, // must use OPEN_EXISTINGq
		0,
		NULL  // hTemplate must be NULL for comm devices
	);
	//handle has not been taken so close it.
	//let the user choose which port to open.
	if (handle > 0)
		CloseHandle((HANDLE)handle);
	return (handle > 0) ? true : false;
}

/*
* check if port is ready in the list
*/
static bool is_port_in_the_list(uint8_t portnum) {
	for (int i = 0; i < port_list.port_cnt; i++) {
		if (port_list.port_id[i].port_num == portnum)
			return true;
	}
	return false;
}

/*
* if port found and has not taken then add to the list.
*/
static void serial_port_add(char *portName) {
	uint8_t port = get_port_num(portName);
	//if port is not taken then add to the list.
	if (s_port_available(port)) {
		port_list.port_id[port_list.port_cnt].port_num = port;
		port_list.port_id[port_list.port_cnt].port_avail = true;
		sprintf(port_list.port_id[port_list.port_cnt].port_name, "%s", portName);
		port_list.port_cnt++;
	}
}

/*
* read window usb device
*/
static void s_get_list_devices()
{
	CONST GUID *pClassGuid = NULL;
	unsigned i;
	DWORD dwSize;
	DEVPROPTYPE ulPropertyType;
	HDEVINFO hDevInfo;
	SP_DEVINFO_DATA DeviceInfoData;
	const static LPCTSTR arPrefix[3] = { TEXT("VID_"), TEXT("PID_"), TEXT("MI_") };
	WCHAR szBuffer[4096];
	char szBufferId[4096];

	char szBufferVcp[] = "STMicroelectronics Virtual";

	FN_SetupDiGetDevicePropertyW fn_SetupDiGetDevicePropertyW = (FN_SetupDiGetDevicePropertyW)
		GetProcAddress(GetModuleHandle(TEXT("Setupapi.dll")), "SetupDiGetDevicePropertyW");

	// List all connected USB devices
	hDevInfo = SetupDiGetClassDevs(pClassGuid, "USB", NULL,
		pClassGuid != NULL ? DIGCF_PRESENT : DIGCF_ALLCLASSES | DIGCF_PRESENT);
	if (hDevInfo == INVALID_HANDLE_VALUE)
		return;

	port_list.port_cnt = 0;

	for (i = 0; ; i++) {
		DeviceInfoData.cbSize = sizeof(DeviceInfoData);
		if (!SetupDiEnumDeviceInfo(hDevInfo, i, &DeviceInfoData))
			break;

		if (fn_SetupDiGetDevicePropertyW && fn_SetupDiGetDevicePropertyW(hDevInfo, &DeviceInfoData, &DEVPKEY_Device_BusReportedDeviceDesc,
			&ulPropertyType, (BYTE*)szBuffer, sizeof(szBuffer), &dwSize, 0)) {
			if (fn_SetupDiGetDevicePropertyW(hDevInfo, &DeviceInfoData, &DEVPKEY_Device_FriendlyName,
				&ulPropertyType, (BYTE*)szBuffer, sizeof(szBuffer), &dwSize, 0)) {

				if (strstr(szBufferVcp, (char*)szBuffer)) {
					sprintf(szBufferId, "%ws", szBuffer);
					if (fn_SetupDiGetDevicePropertyW(hDevInfo, &DeviceInfoData, &DEVPKEY_Device_FriendlyName,
						&ulPropertyType, (BYTE*)szBuffer, sizeof(szBuffer), &dwSize, 0)) {
						//_tprintf(TEXT("    Device Friendly Name: \"%ls\"\n"), szBuffer);
						if (strstr(szBufferVcp, (char *)szBuffer)) {
							sprintf(szBufferId, "%ws", szBuffer);
							serial_port_add(szBufferId);
						}
					}
				}
			}
		}
	}
	if (hDevInfo) {
		SetupDiDestroyDeviceInfoList(hDevInfo);
	}
	return;
}

/***** end of file ***********************************************************/

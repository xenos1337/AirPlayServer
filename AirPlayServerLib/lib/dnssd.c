/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

/* These defines allow us to compile on iOS */
#ifndef __has_feature
# define __has_feature(x) 0
#endif
#ifndef __has_extension
# define __has_extension __has_feature
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dnssd.h"
#include "dnssdint.h"
#include "global.h"
#include "compat.h"
#include "utils.h"

#ifdef WIN32
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#define MAX_DEVICEID 18
#define MAX_SERVNAME 256
#define MAX_INTERFACES 16

/* Structure to hold network interface information */
typedef struct {
	char hwaddr[MAX_HWADDR_LEN];
	uint32_t ifIndex;
	int valid;
} network_interface_t;

#if defined(HAVE_LIBDL) && !defined(__APPLE__)
# define USE_LIBDL 1
#else
# define USE_LIBDL 0
#endif

#if defined(WIN32) || USE_LIBDL
# ifdef WIN32
#  include <stdint.h>
#  if !defined(EFI32) && !defined(EFI64)
#   define DNSSD_STDCALL __stdcall
#  else
#   define DNSSD_STDCALL
#  endif
# else
#  include <dlfcn.h>
#  define DNSSD_STDCALL
# endif

typedef struct _DNSServiceRef_t *DNSServiceRef;
typedef union _TXTRecordRef_t { char PrivateData[16]; char *ForceNaturalAlignment; } TXTRecordRef;

typedef uint32_t DNSServiceFlags;
typedef int32_t  DNSServiceErrorType;

typedef void (DNSSD_STDCALL *DNSServiceRegisterReply)
    (
    DNSServiceRef                       sdRef,
    DNSServiceFlags                     flags,
    DNSServiceErrorType                 errorCode,
    const char                          *name,
    const char                          *regtype,
    const char                          *domain,
    void                                *context
    );

#else
# include <dns_sd.h>
# define DNSSD_STDCALL
#endif

typedef DNSServiceErrorType (DNSSD_STDCALL *DNSServiceRegister_t)
    (
    DNSServiceRef                       *sdRef,
    DNSServiceFlags                     flags,
    uint32_t                            interfaceIndex,
    const char                          *name,
    const char                          *regtype,
    const char                          *domain,
    const char                          *host,
    uint16_t                            port,
    uint16_t                            txtLen,
    const void                          *txtRecord,
    DNSServiceRegisterReply             callBack,
    void                                *context
    );
typedef void (DNSSD_STDCALL *DNSServiceRefDeallocate_t)(DNSServiceRef sdRef);
typedef void (DNSSD_STDCALL *TXTRecordCreate_t)
    (
    TXTRecordRef     *txtRecord,
    uint16_t         bufferLen,
    void             *buffer
    );
typedef void (DNSSD_STDCALL *TXTRecordDeallocate_t)(TXTRecordRef *txtRecord);
typedef DNSServiceErrorType (DNSSD_STDCALL *TXTRecordSetValue_t)
    (
    TXTRecordRef     *txtRecord,
    const char       *key,
    uint8_t          valueSize,
    const void       *value
    );
typedef uint16_t (DNSSD_STDCALL *TXTRecordGetLength_t)(const TXTRecordRef *txtRecord);
typedef const void * (DNSSD_STDCALL *TXTRecordGetBytesPtr_t)(const TXTRecordRef *txtRecord);


struct dnssd_s {
#ifdef WIN32
	HMODULE module;
#elif USE_LIBDL
	void *module;
#endif

	DNSServiceRegister_t       DNSServiceRegister;
	DNSServiceRefDeallocate_t  DNSServiceRefDeallocate;
	TXTRecordCreate_t          TXTRecordCreate;
	TXTRecordSetValue_t        TXTRecordSetValue;
	TXTRecordGetLength_t       TXTRecordGetLength;
	TXTRecordGetBytesPtr_t     TXTRecordGetBytesPtr;
	TXTRecordDeallocate_t      TXTRecordDeallocate;

	/* Support multiple services for multi-interface advertisement */
	DNSServiceRef raopServices[MAX_INTERFACES];
	DNSServiceRef airplayServices[MAX_INTERFACES];
	int raopServiceCount;
	int airplayServiceCount;

	/* Store network interfaces for registration */
	network_interface_t interfaces[MAX_INTERFACES];
	int interfaceCount;
};



dnssd_t *
dnssd_init(int *error)
{
	dnssd_t *dnssd;

	if (error) *error = DNSSD_ERROR_NOERROR;

	dnssd = calloc(1, sizeof(dnssd_t));
	if (!dnssd) {
		if (error) *error = DNSSD_ERROR_OUTOFMEM;
		return NULL;
	}

#ifdef WIN32
	dnssd->module = LoadLibraryA("dnssd.dll");
	if (!dnssd->module) {
		if (error) *error = DNSSD_ERROR_LIBNOTFOUND;
		free(dnssd);
		return NULL;
	}
	dnssd->DNSServiceRegister = (DNSServiceRegister_t)GetProcAddress(dnssd->module, "DNSServiceRegister");
	dnssd->DNSServiceRefDeallocate = (DNSServiceRefDeallocate_t)GetProcAddress(dnssd->module, "DNSServiceRefDeallocate");
	dnssd->TXTRecordCreate = (TXTRecordCreate_t)GetProcAddress(dnssd->module, "TXTRecordCreate");
	dnssd->TXTRecordSetValue = (TXTRecordSetValue_t)GetProcAddress(dnssd->module, "TXTRecordSetValue");
	dnssd->TXTRecordGetLength = (TXTRecordGetLength_t)GetProcAddress(dnssd->module, "TXTRecordGetLength");
	dnssd->TXTRecordGetBytesPtr = (TXTRecordGetBytesPtr_t)GetProcAddress(dnssd->module, "TXTRecordGetBytesPtr");
	dnssd->TXTRecordDeallocate = (TXTRecordDeallocate_t)GetProcAddress(dnssd->module, "TXTRecordDeallocate");

	if (!dnssd->DNSServiceRegister || !dnssd->DNSServiceRefDeallocate || !dnssd->TXTRecordCreate ||
	    !dnssd->TXTRecordSetValue || !dnssd->TXTRecordGetLength || !dnssd->TXTRecordGetBytesPtr ||
	    !dnssd->TXTRecordDeallocate) {
		if (error) *error = DNSSD_ERROR_PROCNOTFOUND;
		FreeLibrary(dnssd->module);
		free(dnssd);
		return NULL;
	}
#elif USE_LIBDL
	dnssd->module = dlopen("libdns_sd.so", RTLD_LAZY);
	if (!dnssd->module) {
		if (error) *error = DNSSD_ERROR_LIBNOTFOUND;
		free(dnssd);
		return NULL;
	}
	dnssd->DNSServiceRegister = (DNSServiceRegister_t)dlsym(dnssd->module, "DNSServiceRegister");
	dnssd->DNSServiceRefDeallocate = (DNSServiceRefDeallocate_t)dlsym(dnssd->module, "DNSServiceRefDeallocate");
	dnssd->TXTRecordCreate = (TXTRecordCreate_t)dlsym(dnssd->module, "TXTRecordCreate");
	dnssd->TXTRecordSetValue = (TXTRecordSetValue_t)dlsym(dnssd->module, "TXTRecordSetValue");
	dnssd->TXTRecordGetLength = (TXTRecordGetLength_t)dlsym(dnssd->module, "TXTRecordGetLength");
	dnssd->TXTRecordGetBytesPtr = (TXTRecordGetBytesPtr_t)dlsym(dnssd->module, "TXTRecordGetBytesPtr");
	dnssd->TXTRecordDeallocate = (TXTRecordDeallocate_t)dlsym(dnssd->module, "TXTRecordDeallocate");

	if (!dnssd->DNSServiceRegister || !dnssd->DNSServiceRefDeallocate || !dnssd->TXTRecordCreate ||
	    !dnssd->TXTRecordSetValue || !dnssd->TXTRecordGetLength || !dnssd->TXTRecordGetBytesPtr ||
	    !dnssd->TXTRecordDeallocate) {
		if (error) *error = DNSSD_ERROR_PROCNOTFOUND;
		dlclose(dnssd->module);
		free(dnssd);
		return NULL;
	}
#else
	dnssd->DNSServiceRegister = &DNSServiceRegister;
	dnssd->DNSServiceRefDeallocate = &DNSServiceRefDeallocate;
	dnssd->TXTRecordCreate = &TXTRecordCreate;
	dnssd->TXTRecordSetValue = &TXTRecordSetValue;
	dnssd->TXTRecordGetLength = &TXTRecordGetLength;
	dnssd->TXTRecordGetBytesPtr = &TXTRecordGetBytesPtr;
	dnssd->TXTRecordDeallocate = &TXTRecordDeallocate;
#endif

	return dnssd;
}

void
dnssd_destroy(dnssd_t *dnssd)
{
	if (dnssd) {
#ifdef WIN32
		FreeLibrary(dnssd->module);
#elif USE_LIBDL
		dlclose(dnssd->module);
#endif
		free(dnssd);
	}
}

/* Enumerate all active network interfaces with MAC addresses */
static int
enumerate_network_interfaces(dnssd_t *dnssd)
{
	int count = 0;

#ifdef WIN32
	PIP_ADAPTER_ADDRESSES pAddresses = NULL;
	PIP_ADAPTER_ADDRESSES pCurrAddresses = NULL;
	ULONG outBufLen = 0;
	ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
	DWORD dwRetVal = 0;

	/* First call to determine required buffer size */
	outBufLen = sizeof(IP_ADAPTER_ADDRESSES);
	pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(outBufLen);
	if (pAddresses == NULL) {
		return 0;
	}

	dwRetVal = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, pAddresses, &outBufLen);
	if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
		free(pAddresses);
		pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(outBufLen);
		if (pAddresses == NULL) {
			return 0;
		}
	}

	dwRetVal = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, pAddresses, &outBufLen);
	if (dwRetVal != NO_ERROR) {
		free(pAddresses);
		return 0;
	}

	/* Iterate through all adapters */
	pCurrAddresses = pAddresses;
	while (pCurrAddresses && count < MAX_INTERFACES) {
		/* Only include adapters that are up and have a valid MAC address */
		if (pCurrAddresses->OperStatus == IfOperStatusUp &&
			pCurrAddresses->PhysicalAddressLength == MAX_HWADDR_LEN &&
			pCurrAddresses->IfType != IF_TYPE_SOFTWARE_LOOPBACK &&
			pCurrAddresses->IfType != IF_TYPE_TUNNEL) {

			/* Copy MAC address */
			for (int i = 0; i < MAX_HWADDR_LEN; i++) {
				dnssd->interfaces[count].hwaddr[i] = pCurrAddresses->PhysicalAddress[i];
			}
			/* Use the interface index (IfIndex for IPv4, Ipv6IfIndex for IPv6) */
			dnssd->interfaces[count].ifIndex = pCurrAddresses->IfIndex;
			dnssd->interfaces[count].valid = 1;
			count++;
		}
		pCurrAddresses = pCurrAddresses->Next;
	}

	free(pAddresses);
#else
	/* For non-Windows platforms, use interface index 0 (any) with a placeholder MAC */
	/* A more complete implementation would use getifaddrs() on Linux/macOS */
	dnssd->interfaces[0].ifIndex = 0;
	dnssd->interfaces[0].valid = 1;
	memset(dnssd->interfaces[0].hwaddr, 0, MAX_HWADDR_LEN);
	count = 1;
#endif

	dnssd->interfaceCount = count;
	return count;
}

static void __stdcall MyRegisterServiceReply
(
	DNSServiceRef                       sdRef,
	DNSServiceFlags                     flags,
	DNSServiceErrorType                 errorCode,
	const char* name,
	const char* regtype,
	const char* domain,
	void* context
)
{
}

int
dnssd_register_raop(dnssd_t *dnssd, const char *name, unsigned short port, const char *hwaddr, int hwaddrlen, int password)
{
	TXTRecordRef txtRecord;
	char servname[MAX_SERVNAME];
	int ret;
	int i;
	int registered = 0;
	const char *if_hwaddr;

	assert(dnssd);
	assert(name);
	assert(hwaddr);

	/* Enumerate network interfaces if not already done */
	if (dnssd->interfaceCount == 0) {
		enumerate_network_interfaces(dnssd);
	}

	/* If no interfaces found, fall back to provided hwaddr on all interfaces */
	if (dnssd->interfaceCount == 0) {
		dnssd->interfaces[0].ifIndex = 0;  /* kDNSServiceInterfaceIndexAny */
		memcpy(dnssd->interfaces[0].hwaddr, hwaddr, hwaddrlen);
		dnssd->interfaces[0].valid = 1;
		dnssd->interfaceCount = 1;
	}

	dnssd->raopServiceCount = 0;

	/* Register service on each interface */
	for (i = 0; i < dnssd->interfaceCount && i < MAX_INTERFACES; i++) {
		if (!dnssd->interfaces[i].valid) {
			continue;
		}

		/* Use the interface's MAC address or fallback to provided hwaddr */
		if_hwaddr = dnssd->interfaces[i].hwaddr;
		if (if_hwaddr[0] == 0 && if_hwaddr[1] == 0 && if_hwaddr[2] == 0 &&
			if_hwaddr[3] == 0 && if_hwaddr[4] == 0 && if_hwaddr[5] == 0) {
			if_hwaddr = hwaddr;
		}

		dnssd->TXTRecordCreate(&txtRecord, 0, NULL);
		dnssd->TXTRecordSetValue(&txtRecord, "txtvers", strlen(RAOP_TXTVERS), RAOP_TXTVERS);
		dnssd->TXTRecordSetValue(&txtRecord, "ch", strlen(RAOP_CH), RAOP_CH);
		dnssd->TXTRecordSetValue(&txtRecord, "cn", strlen(RAOP_CN), RAOP_CN);
		dnssd->TXTRecordSetValue(&txtRecord, "et", strlen(RAOP_ET), RAOP_ET);
		dnssd->TXTRecordSetValue(&txtRecord, "sv", strlen(RAOP_SV), RAOP_SV);
		dnssd->TXTRecordSetValue(&txtRecord, "da", strlen(RAOP_DA), RAOP_DA);
		dnssd->TXTRecordSetValue(&txtRecord, "sr", strlen(RAOP_SR), RAOP_SR);
		dnssd->TXTRecordSetValue(&txtRecord, "ss", strlen(RAOP_SS), RAOP_SS);
		if (password) {
			dnssd->TXTRecordSetValue(&txtRecord, "pw", strlen("true"), "true");
		} else {
			dnssd->TXTRecordSetValue(&txtRecord, "pw", strlen("false"), "false");
		}
		dnssd->TXTRecordSetValue(&txtRecord, "vn", strlen(RAOP_VN), RAOP_VN);
		dnssd->TXTRecordSetValue(&txtRecord, "tp", strlen(RAOP_TP), RAOP_TP);
		dnssd->TXTRecordSetValue(&txtRecord, "md", strlen(RAOP_MD), RAOP_MD);
		dnssd->TXTRecordSetValue(&txtRecord, "vs", strlen(GLOBAL_VERSION), GLOBAL_VERSION);
		dnssd->TXTRecordSetValue(&txtRecord, "sm", strlen(RAOP_SM), RAOP_SM);
		dnssd->TXTRecordSetValue(&txtRecord, "ek", strlen(RAOP_EK), RAOP_EK);
		dnssd->TXTRecordSetValue(&txtRecord, "sf", strlen(RAOP_SF), RAOP_SF);
		dnssd->TXTRecordSetValue(&txtRecord, "am", strlen(GLOBAL_MODEL), GLOBAL_MODEL);

		/* Convert hardware address to string */
		ret = utils_hwaddr_raop(servname, sizeof(servname), if_hwaddr, hwaddrlen);
		if (ret < 0) {
			dnssd->TXTRecordDeallocate(&txtRecord);
			continue;
		}

		/* Check that we have bytes for 'hw@name' format */
		if (sizeof(servname) < strlen(servname)+1+strlen(name)+1) {
			dnssd->TXTRecordDeallocate(&txtRecord);
			continue;
		}

		strncat(servname, "@", sizeof(servname)-strlen(servname)-1);
		strncat(servname, name, sizeof(servname)-strlen(servname)-1);

		/* Register the service on this interface */
		ret = dnssd->DNSServiceRegister(&dnssd->raopServices[dnssd->raopServiceCount],
		                          0, dnssd->interfaces[i].ifIndex,
		                          servname, "_raop._tcp",
		                          NULL, NULL,
		                          htons(port),
		                          dnssd->TXTRecordGetLength(&txtRecord),
		                          dnssd->TXTRecordGetBytesPtr(&txtRecord),
			MyRegisterServiceReply, NULL);

		/* Deallocate TXT record */
		dnssd->TXTRecordDeallocate(&txtRecord);

		if (ret == 0) {
			dnssd->raopServiceCount++;
			registered++;
		}
	}

	return registered > 0 ? 1 : -1;
}

int
dnssd_register_airplay(dnssd_t *dnssd, const char *name, unsigned short port, const char *hwaddr, int hwaddrlen)
{
	TXTRecordRef txtRecord;
	char deviceid[3*MAX_HWADDR_LEN];
	char features[16];
	int ret;
	int i;
	int registered = 0;
	const char *if_hwaddr;

	assert(dnssd);
	assert(name);
	assert(hwaddr);

	/* Enumerate network interfaces if not already done */
	if (dnssd->interfaceCount == 0) {
		enumerate_network_interfaces(dnssd);
	}

	/* If no interfaces found, fall back to provided hwaddr on all interfaces */
	if (dnssd->interfaceCount == 0) {
		dnssd->interfaces[0].ifIndex = 0;  /* kDNSServiceInterfaceIndexAny */
		memcpy(dnssd->interfaces[0].hwaddr, hwaddr, hwaddrlen);
		dnssd->interfaces[0].valid = 1;
		dnssd->interfaceCount = 1;
	}

	features[sizeof(features)-1] = '\0';
	snprintf(features, sizeof(features)-1, "0x%x", GLOBAL_FEATURES);

	dnssd->airplayServiceCount = 0;

	/* Register service on each interface */
	for (i = 0; i < dnssd->interfaceCount && i < MAX_INTERFACES; i++) {
		if (!dnssd->interfaces[i].valid) {
			continue;
		}

		/* Use the interface's MAC address or fallback to provided hwaddr */
		if_hwaddr = dnssd->interfaces[i].hwaddr;
		if (if_hwaddr[0] == 0 && if_hwaddr[1] == 0 && if_hwaddr[2] == 0 &&
			if_hwaddr[3] == 0 && if_hwaddr[4] == 0 && if_hwaddr[5] == 0) {
			if_hwaddr = hwaddr;
		}

		/* Convert hardware address to string */
		ret = utils_hwaddr_airplay(deviceid, sizeof(deviceid), if_hwaddr, hwaddrlen);
		if (ret < 0) {
			continue;
		}

		dnssd->TXTRecordCreate(&txtRecord, 0, NULL);
		dnssd->TXTRecordSetValue(&txtRecord, "srcvers", strlen(GLOBAL_VERSION), GLOBAL_VERSION);
		dnssd->TXTRecordSetValue(&txtRecord, "deviceid", strlen(deviceid), deviceid);
		dnssd->TXTRecordSetValue(&txtRecord, "features", strlen("0x5A7FFFF7, 0x1E"), "0x5A7FFFF7,0x1E");
		dnssd->TXTRecordSetValue(&txtRecord, "model", strlen(GLOBAL_MODEL), GLOBAL_MODEL);
		dnssd->TXTRecordSetValue(&txtRecord, "flags", strlen(RAOP_SF), RAOP_SF);
		dnssd->TXTRecordSetValue(&txtRecord, "vv", strlen(RAOP_VV), RAOP_VV);

		/* Register the service on this interface */
		ret = dnssd->DNSServiceRegister(&dnssd->airplayServices[dnssd->airplayServiceCount],
		                          0, dnssd->interfaces[i].ifIndex,
		                          name, "_airplay._tcp",
		                          NULL, NULL,
		                          htons(port),
		                          dnssd->TXTRecordGetLength(&txtRecord),
		                          dnssd->TXTRecordGetBytesPtr(&txtRecord),
		                          MyRegisterServiceReply, NULL);

		/* Deallocate TXT record */
		dnssd->TXTRecordDeallocate(&txtRecord);

		if (ret == 0) {
			dnssd->airplayServiceCount++;
			registered++;
		}
	}

	return registered > 0 ? 0 : -1;
}

void
dnssd_unregister_raop(dnssd_t *dnssd)
{
	int i;
	assert(dnssd);

	/* Deallocate all RAOP service registrations */
	for (i = 0; i < dnssd->raopServiceCount && i < MAX_INTERFACES; i++) {
		if (dnssd->raopServices[i]) {
			dnssd->DNSServiceRefDeallocate(dnssd->raopServices[i]);
			dnssd->raopServices[i] = NULL;
		}
	}
	dnssd->raopServiceCount = 0;
}

void
dnssd_unregister_airplay(dnssd_t *dnssd)
{
	int i;
	assert(dnssd);

	/* Deallocate all AirPlay service registrations */
	for (i = 0; i < dnssd->airplayServiceCount && i < MAX_INTERFACES; i++) {
		if (dnssd->airplayServices[i]) {
			dnssd->DNSServiceRefDeallocate(dnssd->airplayServices[i]);
			dnssd->airplayServices[i] = NULL;
		}
	}
	dnssd->airplayServiceCount = 0;
}

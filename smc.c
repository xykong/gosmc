/*
 * Apple System Management Control (SMC) Tool
 * Copyright (C) 2006 devnull
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
/*
cc ./smc.c  -o smcutil -framework IOKit -framework CoreFoundation -Wno-four-char-constants -Wall -g -arch i386
 */

#include <stdio.h>
#include <IOKit/IOKitLib.h>
#include <Kernel/string.h>
#include <libkern/OSAtomic.h>
#include <os/lock.h>

#include "smc.h"

// Cache the keyInfo to lower the energy impact of SMCReadKey()
#define KEY_INFO_CACHE_SIZE 100
struct {
	UInt32 key;
	SMCKeyData_keyInfo_t keyInfo;
} g_keyInfoCache[KEY_INFO_CACHE_SIZE];

int g_keyInfoCacheCount = 0;
os_unfair_lock g_keyInfoSpinLock = OS_UNFAIR_LOCK_INIT;

UInt32 _strtoul(const char *str, int size, int base)
{
    UInt32 total = 0;
    int i;

    for (i = 0; i < size; i++)
    {
        if (base == 16)
            total += str[i] << (size - 1 - i) * 8;
        else
            total += (unsigned char) (str[i] << (size - 1 - i) * 8);
    }
    return total;
}

void _ultostr(char *str, UInt32 val)
{
    str[4] = '\0';
    snprintf(str, 5, "%c%c%c%c",
             (unsigned int) val >> 24,
             (unsigned int) val >> 16,
             (unsigned int) val >> 8,
             (unsigned int) val);
}

#if !defined(MAC_OS_VERSION_12_0) \
    || (MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_VERSION_12_0)
#define IOMainPort IOMasterPort
#endif

kern_return_t SMCOpen(const char *serviceName, io_connect_t *conn)
{
    kern_return_t result;
    mach_port_t   masterPort;
    io_iterator_t iterator;
    io_object_t   device;

    IOMainPort(MACH_PORT_NULL, &masterPort);

    CFMutableDictionaryRef matchingDictionary = IOServiceMatching(serviceName);
    result = IOServiceGetMatchingServices(masterPort, matchingDictionary, &iterator);
    if (result != kIOReturnSuccess)
    {
        //printf("Error: IOServiceGetMatchingServices() = %08x\n", result);
        return 1;
    }

    device = IOIteratorNext(iterator);
    IOObjectRelease((io_object_t)iterator);
    if (device == 0)
    {
        //printf("Error: no SMC found\n");
        return 1;
    }

    result = IOServiceOpen(device, mach_task_self(), 0, conn);
    IOObjectRelease(device);
    if (result != kIOReturnSuccess)
    {
        //printf("Error: IOServiceOpen() = %08x\n", result);
        return 1;
    }

    return kIOReturnSuccess;
}

kern_return_t SMCClose(io_connect_t conn)
{
    return IOServiceClose(conn);
}

kern_return_t SMCCall(io_connect_t conn, int index, SMCKeyData_t *inputStructure, SMCKeyData_t *outputStructure)
{
    size_t   structureInputSize;
    size_t   structureOutputSize;

    structureInputSize = sizeof(SMCKeyData_t);
    structureOutputSize = sizeof(SMCKeyData_t);

 	return IOConnectCallStructMethod(
									 conn,
									 index,
									 inputStructure,
									 structureInputSize,
									 outputStructure,
									 &structureOutputSize
									 );
}

// Provides key info, using a cache to dramatically improve the energy impact of smcFanControl
kern_return_t SMCGetKeyInfo(io_connect_t conn, UInt32 key, SMCKeyData_keyInfo_t* keyInfo)
{
	SMCKeyData_t inputStructure;
	SMCKeyData_t outputStructure;
	kern_return_t result = kIOReturnSuccess;
	int i = 0;

	os_unfair_lock_lock(&g_keyInfoSpinLock);

	for (; i < g_keyInfoCacheCount; ++i)
	{
		if (key == g_keyInfoCache[i].key)
		{
			*keyInfo = g_keyInfoCache[i].keyInfo;
			break;
		}
	}

	if (i == g_keyInfoCacheCount)
	{
		// Not in cache, must look it up.
		memset(&inputStructure, 0, sizeof(inputStructure));
		memset(&outputStructure, 0, sizeof(outputStructure));

		inputStructure.key = key;
		inputStructure.data8 = SMC_CMD_READ_KEYINFO;

		result = SMCCall(conn, KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
		if (result == kIOReturnSuccess)
		{
			*keyInfo = outputStructure.keyInfo;
			if (g_keyInfoCacheCount < KEY_INFO_CACHE_SIZE)
			{
				g_keyInfoCache[g_keyInfoCacheCount].key = key;
				g_keyInfoCache[g_keyInfoCacheCount].keyInfo = outputStructure.keyInfo;
				++g_keyInfoCacheCount;
			}
		}
	}

	os_unfair_lock_unlock(&g_keyInfoSpinLock);

	return result;
}

kern_return_t SMCReadKey(io_connect_t conn, const UInt32Char_t key, SMCVal_t *val)
{
    kern_return_t result;
    SMCKeyData_t  inputStructure;
    SMCKeyData_t  outputStructure;

    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));
    memset(val, 0, sizeof(SMCVal_t));

    inputStructure.key = _strtoul(key, 4, 16);
    //REVEIW_REHABMAN: mempcy used to avoid deprecated strcpy...
    //strcpy(val->key, key);
    memcpy(val->key, key, sizeof(val->key));

    result = SMCGetKeyInfo(conn, inputStructure.key, &outputStructure.keyInfo);
    if (result != kIOReturnSuccess)
        return result;

    val->dataSize = outputStructure.keyInfo.dataSize;
    _ultostr(val->dataType, outputStructure.keyInfo.dataType);
    inputStructure.keyInfo.dataSize = val->dataSize;
    inputStructure.data8 = SMC_CMD_READ_BYTES;

    result = SMCCall(conn, KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
    if (result != kIOReturnSuccess)
        return result;

    memcpy(val->bytes, outputStructure.bytes, sizeof(outputStructure.bytes));

    return kIOReturnSuccess;
}

kern_return_t SMCWriteKey(io_connect_t conn, const SMCVal_t *val)
{
    SMCVal_t      readVal;

    IOReturn result = SMCReadKey(conn, val->key, &readVal);
    if (result != kIOReturnSuccess)
        return result;

    if (readVal.dataSize != val->dataSize)
        return kIOReturnError;

    return SMCWriteKeyUnsafe(conn, val);
}

kern_return_t SMCWriteKeyUnsafe(io_connect_t conn, const SMCVal_t *val)
{
    kern_return_t result;
    SMCKeyData_t  inputStructure;
    SMCKeyData_t  outputStructure;

    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));

    inputStructure.key = _strtoul(val->key, 4, 16);
    inputStructure.data8 = SMC_CMD_WRITE_BYTES;
    inputStructure.keyInfo.dataSize = val->dataSize;
    memcpy(inputStructure.bytes, val->bytes, sizeof(val->bytes));

    result = SMCCall(conn, KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
    if (result != kIOReturnSuccess)
        return result;

    return kIOReturnSuccess;
}

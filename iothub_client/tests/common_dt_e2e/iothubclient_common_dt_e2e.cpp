// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include "testrunnerswitcher.h"

#include "iothub_client.h"
#include "iothub_client_options.h"
#include "iothub_devicetwin.h"

#include "iothub_account.h"
#include "iothubtest.h"

#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/uniqueid.h"

#include "parson.h"
#include "certs.h"

static IOTHUB_ACCOUNT_INFO_HANDLE g_iothubAcctInfo = NULL;

#define MAX_CLOUD_TRAVEL_TIME  180.0
#define BUFFER_SIZE            37

TEST_DEFINE_ENUM_TYPE(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_RESULT_VALUES);
TEST_DEFINE_ENUM_TYPE(DEVICE_TWIN_UPDATE_STATE, DEVICE_TWIN_UPDATE_STATE_VALUES);

typedef struct DEVICE_REPORTED_DATA_TAG
{
    char *string_property;
    int   integer_property;
    bool receivedCallBack;   // true when device callback has been called
    int  status_code;        // status reported by the callback
    LOCK_HANDLE lock;
} DEVICE_REPORTED_DATA;


static void reportedStateCallback(int status_code, void* userContextCallback)
{
    DEVICE_REPORTED_DATA *device = (DEVICE_REPORTED_DATA *) userContextCallback;
    if (Lock(device->lock) == LOCK_ERROR)
    {
        LogError("Lock failed");
    }
    else
    {
        device->status_code = status_code;
        device->receivedCallBack = true;
        (void) Unlock(device->lock);
    }
}

static int generate_new_int(void)
{
    int retValue;
    time_t nowTime = time(NULL);

    retValue = (int) nowTime;
    return retValue;
}

static char *generate_unique_string(void)
{
    char *retValue;

    retValue = (char *) malloc(BUFFER_SIZE);
    if (retValue == NULL)
    {
        LogError("malloc failed");
    }
    else if (UniqueId_Generate(retValue, BUFFER_SIZE) != UNIQUEID_OK)
    {
        LogError("UniqueId_Generate failed");
        free(retValue);
        retValue = NULL;
    }
    return retValue;
}

static DEVICE_REPORTED_DATA *device_reported_init()
{
    DEVICE_REPORTED_DATA *retValue;

    if ((retValue = (DEVICE_REPORTED_DATA *) malloc(sizeof(DEVICE_REPORTED_DATA))) == NULL)
    {
        LogError("malloc failed");
    }
    else
    {
        retValue->lock = Lock_Init();
        if (retValue->lock == NULL)
        {
            LogError("Lock_Init failed");
            free(retValue);
            retValue = NULL;
        }
        else
        {
            retValue->string_property = generate_unique_string();
            if (retValue->string_property == NULL)
            {
                LogError("generate unique string failed");
                Lock_Deinit(retValue->lock);
                free(retValue);
                retValue = NULL;
            }
            else
            {
                retValue->receivedCallBack = false;
                retValue->integer_property = generate_new_int();
            }
        }
    }
    return retValue;
}

static void device_reported_deinit(DEVICE_REPORTED_DATA *device)
{
    if (device == NULL)
    {
        LogError("invalid parameter device");
    }
    else
    {
        free(device->string_property);
        Lock_Deinit(device->lock);
        free(device);
    }
}

void dt_e2e_init(void)
{
    int result = platform_init();
    ASSERT_ARE_EQUAL_WITH_MSG(int, 0, result, "platform_init failed");

    g_iothubAcctInfo = IoTHubAccount_Init(true);
    ASSERT_IS_NOT_NULL_WITH_MSG(g_iothubAcctInfo, "IoTHubAccount_Init failed");

    /* the return value from the second init is deliberatly ignored. */
    platform_init();
}

void dt_e2e_deinit(void)
{
    IoTHubAccount_deinit(g_iothubAcctInfo);

    // Need a double deinit
    platform_deinit();
    platform_deinit();
}

typedef struct DEVICE_DESIRED_DATA_TAG
{
    bool receivedCallBack;                     // true when device callback has been called
    DEVICE_TWIN_UPDATE_STATE update_state;     // status reported by the callback
    char *cb_payload;
    LOCK_HANDLE lock;
} DEVICE_DESIRED_DATA;


static const char *REPORTED_PAYLOAD_FORMAT = "{\"integer_property\": %d, \"string_property\": \"%s\"}";
static char *malloc_and_fill_reported_payload(const char *string, int aint)
{
    size_t  length = snprintf(NULL, 0, REPORTED_PAYLOAD_FORMAT, aint, string);
    char   *retValue = (char *) malloc(length + 1);
    if (retValue == NULL)
    {
        LogError("malloc failed");
    }
    else
    {
        (void) sprintf(retValue, REPORTED_PAYLOAD_FORMAT, aint, string);
    }
    return retValue;
}

void dt_e2e_send_reported_test(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    // arrange
    IOTHUB_CLIENT_CONFIG iotHubConfig = { 0 };

    iotHubConfig.iotHubName = IoTHubAccount_GetIoTHubName(g_iothubAcctInfo);
    iotHubConfig.iotHubSuffix = IoTHubAccount_GetIoTHubSuffix(g_iothubAcctInfo);
    iotHubConfig.deviceId = IoTHubAccount_GetDeviceId(g_iothubAcctInfo);
    iotHubConfig.deviceKey = IoTHubAccount_GetDeviceKey(g_iothubAcctInfo);
    iotHubConfig.protocol = protocol;

    DEVICE_REPORTED_DATA *device = device_reported_init();
    ASSERT_IS_NOT_NULL_WITH_MSG(device, "failed to create the device client data");

    // Create the IoT Hub Data
    IOTHUB_CLIENT_HANDLE iotHubClientHandle = IoTHubClient_Create(&iotHubConfig);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubClientHandle, "IoTHubClient_Create failed");

    // Turn on Log
    bool trace = true;
    (void)IoTHubClient_SetOption(iotHubClientHandle, OPTION_LOG_TRACE, &trace);
    (void)IoTHubClient_SetOption(iotHubClientHandle, "TrustedCerts", certificates);

    // generate the payload
    char *buffer = malloc_and_fill_reported_payload(device->string_property, device->integer_property);
    ASSERT_IS_NOT_NULL_WITH_MSG(buffer, "failed to allocate and prepare the payload for IoTHubClient_SendReportedState");

    // act
    IOTHUB_CLIENT_RESULT iot_result = IoTHubClient_SendReportedState(iotHubClientHandle, (unsigned char *) buffer, strlen(buffer), reportedStateCallback, device);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, iot_result, "IoTHubClient_SendReportedState failed");

    // cleanup
    free(buffer);

    time_t beginOperation, nowTime;
    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < MAX_CLOUD_TRAVEL_TIME) // time box
        )
    {
        if (Lock(device->lock) != LOCK_OK)
        {
            ASSERT_FAIL("Lock failed");
        }
        else
        {
            if (device->receivedCallBack)
            {
                Unlock(device->lock);
                break;
            }
            Unlock(device->lock);
        }
        ThreadAPI_Sleep(1000);
    }

    if (Lock(device->lock) != LOCK_OK)
    {
        ASSERT_FAIL("Lock failed");
    }
    else
    {
        ASSERT_IS_TRUE_WITH_MSG(device->receivedCallBack, "SendReported ACK was not received in the alloted time"); // was received by the callback...
        ASSERT_IS_TRUE_WITH_MSG(device->status_code < 300, "SnedReported status_code is an error");
        (void)Unlock(device->lock);
    }

    const char *connectionString = IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo);
    IOTHUB_SERVICE_CLIENT_AUTH_HANDLE iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(connectionString);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubServiceClientHandle, "IoTHubServiceClientAuth_CreateFromConnectionString failed");

    IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle = IoTHubDeviceTwin_Create(iotHubServiceClientHandle);
    ASSERT_IS_NOT_NULL_WITH_MSG(serviceClientDeviceTwinHandle, "IoTHubDeviceTwin_Create failed");

    char *deviceTwinData = IoTHubDeviceTwin_GetTwin(serviceClientDeviceTwinHandle, iotHubConfig.deviceId);
    ASSERT_IS_NOT_NULL_WITH_MSG(deviceTwinData, "IoTHubDeviceTwin_GetTwin failed");

    JSON_Value *root_value = json_parse_string(deviceTwinData);
    ASSERT_IS_NOT_NULL_WITH_MSG(root_value, "json_parse_string failed");

    JSON_Object *root_object = json_value_get_object(root_value);
    const char *string_property = json_object_dotget_string(root_object, "properties.reported.string_property");
    ASSERT_ARE_EQUAL_WITH_MSG(char_ptr, device->string_property, string_property, "string data retrieved differs from reported");

    int integer_property = (int) json_object_dotget_number(root_object, "properties.reported.integer_property");
    ASSERT_ARE_EQUAL_WITH_MSG(int, device->integer_property, integer_property, "integer data retrieved differs from reported");

    // cleanup
    json_value_free(root_value);
    free(deviceTwinData);
    IoTHubDeviceTwin_Destroy(serviceClientDeviceTwinHandle);
    IoTHubServiceClientAuth_Destroy(iotHubServiceClientHandle);
    IoTHubClient_Destroy(iotHubClientHandle);
    device_reported_deinit(device);
}

static const char *COMPLETE_DESIRED_PAYLOAD_FORMAT = "{\"properties\":{\"desired\":{\"integer_property\": %d, \"string_property\": \"%s\"}}}";
static char *malloc_and_fill_desired_payload(const char *string, int aint)
{
    size_t  length = snprintf(NULL, 0, COMPLETE_DESIRED_PAYLOAD_FORMAT, aint, string);
    char   *retValue = (char *) malloc(length + 1);
    if (retValue == NULL)
    {
        LogError("malloc failed");
    }
    else
    {
        (void) sprintf(retValue, COMPLETE_DESIRED_PAYLOAD_FORMAT, aint, string);
    }
    return retValue;
}

static char * malloc_and_copy_unsigned_char(const unsigned char* payload, size_t size)
{
    char *retValue;
    if (payload == NULL)
    {
        LogError("invalid parameter payload");
        retValue = NULL;
    }
    else if (size < 1)
    {
        LogError("invalid parameter size");
        retValue = NULL;
    }
    else
    {
        char *temp = (char *) malloc(size + 1);
        if (temp == NULL)
        {
            LogError("malloc failed");
            retValue = NULL;
        }
        else
        {
            retValue = (char *) memcpy(temp, payload, size);
            temp[size] = '\0';
        }
    }
    return retValue;
}

static void deviceTwinCallback(DEVICE_TWIN_UPDATE_STATE update_state, const unsigned char* payload, size_t size, void* userContextCallback)
{
    DEVICE_DESIRED_DATA *device = (DEVICE_DESIRED_DATA *)userContextCallback;
    if (Lock(device->lock) == LOCK_ERROR)
    {
        LogError("Lock failed");
    }
    else
    {
        device->update_state = update_state;
        device->receivedCallBack = true;
        device->cb_payload = malloc_and_copy_unsigned_char(payload, size);
        (void)Unlock(device->lock);
    }
}

static DEVICE_DESIRED_DATA *device_desired_init()
{
    DEVICE_DESIRED_DATA *retValue;

    if ((retValue = (DEVICE_DESIRED_DATA *) malloc(sizeof(DEVICE_DESIRED_DATA))) == NULL)
    {
        LogError("malloc failed");
    }
    else
    {
        retValue->lock = Lock_Init();
        if (retValue->lock == NULL)
        {
            LogError("Lock_Init failed");
            free(retValue);
            retValue = NULL;
        }
        else
        {
            retValue->receivedCallBack = false;
            retValue->cb_payload = NULL;
        }
    }
    return retValue;
}

static void device_desired_deinit(DEVICE_DESIRED_DATA *device)
{
    if (device == NULL)
    {
        LogError("invalid parameter device");
    }
    else
    {
        free(device->cb_payload);
        Lock_Deinit(device->lock);
        free(device);
    }
}

void dt_e2e_get_complete_desired_test(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    // arrange
    IOTHUB_CLIENT_CONFIG iotHubConfig = { 0 };

    iotHubConfig.iotHubName = IoTHubAccount_GetIoTHubName(g_iothubAcctInfo);
    iotHubConfig.iotHubSuffix = IoTHubAccount_GetIoTHubSuffix(g_iothubAcctInfo);
    iotHubConfig.deviceId = IoTHubAccount_GetDeviceId(g_iothubAcctInfo);
    iotHubConfig.deviceKey = IoTHubAccount_GetDeviceKey(g_iothubAcctInfo);
    iotHubConfig.protocol = protocol;

    DEVICE_DESIRED_DATA *device = device_desired_init();
    ASSERT_IS_NOT_NULL_WITH_MSG(device, "failed to create the device client data");

    // Create the IoT Hub Data
    IOTHUB_CLIENT_HANDLE iotHubClientHandle = IoTHubClient_Create(&iotHubConfig);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubClientHandle, "IoTHubClient_Create failed");

    // Turn on Log
    bool trace = true;
    (void) IoTHubClient_SetOption(iotHubClientHandle, OPTION_LOG_TRACE, &trace);
    (void) IoTHubClient_SetOption(iotHubClientHandle, "TrustedCerts", certificates);

    IOTHUB_CLIENT_RESULT iot_result = IoTHubClient_SetDeviceTwinCallback(iotHubClientHandle, deviceTwinCallback, device);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, iot_result, "IoTHubClient_SetDeviceTwinCallback failed");

    const char *connectionString = IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo);
    IOTHUB_SERVICE_CLIENT_AUTH_HANDLE iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(connectionString);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubServiceClientHandle, "IoTHubServiceClientAuth_CreateFromConnectionString failed");

    IOTHUB_SERVICE_CLIENT_DEVICE_TWIN_HANDLE serviceClientDeviceTwinHandle = IoTHubDeviceTwin_Create(iotHubServiceClientHandle);
    ASSERT_IS_NOT_NULL_WITH_MSG(serviceClientDeviceTwinHandle, "IoTHubDeviceTwin_Create failed");

    char *expected_desired_string = generate_unique_string();
    int   expected_desired_integer = generate_new_int();
    char *buffer = malloc_and_fill_desired_payload(expected_desired_string, expected_desired_integer);
    ASSERT_IS_NOT_NULL_WITH_MSG(buffer, "failed to create the payload for IoTHubDeviceTwin_UpdateTwin");

    char *deviceTwinData = IoTHubDeviceTwin_UpdateTwin(serviceClientDeviceTwinHandle, iotHubConfig.deviceId, buffer);
    free(buffer);
    ASSERT_IS_NOT_NULL_WITH_MSG(deviceTwinData, "IoTHubDeviceTwin_UpdateTwin failed");
    free(deviceTwinData);

    time_t beginOperation, nowTime;
    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < MAX_CLOUD_TRAVEL_TIME) // time box
        )
    {
        if (Lock(device->lock) != LOCK_OK)
        {
            ASSERT_FAIL("Lock failed");
        }
        else
        {
            if (device->receivedCallBack)
            {
                Unlock(device->lock);
                break;
            }
            Unlock(device->lock);
        }
        ThreadAPI_Sleep(1000);
    }

    if (Lock(device->lock) != LOCK_OK)
    {
        ASSERT_FAIL("Lock failed");
    }
    else
    {
        ASSERT_IS_TRUE_WITH_MSG(device->receivedCallBack, "deviceTwinCallback was never called"); // was received by the callback...
        ASSERT_ARE_EQUAL_WITH_MSG(DEVICE_TWIN_UPDATE_STATE, DEVICE_TWIN_UPDATE_COMPLETE, device->update_state, "update_state differs from expected");
        ASSERT_IS_NOT_NULL_WITH_MSG(device->cb_payload, "payload is NULL");
        (void)Unlock(device->lock);
    }

    JSON_Value *root_value = json_parse_string(device->cb_payload);
    ASSERT_IS_NOT_NULL_WITH_MSG(root_value, "json_parse_string failed");

    JSON_Object *root_object = json_value_get_object(root_value);
    const char *string_property = json_object_dotget_string(root_object, "desired.string_property");
    ASSERT_ARE_EQUAL_WITH_MSG(char_ptr, expected_desired_string, string_property, "string data retrieved differs from expected");

    int integer_property = (int) json_object_dotget_number(root_object, "desired.integer_property");
    ASSERT_ARE_EQUAL_WITH_MSG(int, expected_desired_integer, integer_property, "integer data retrieved differs from expected");

    // cleanup
    json_value_free(root_value);
    free(expected_desired_string);
    IoTHubDeviceTwin_Destroy(serviceClientDeviceTwinHandle);
    IoTHubServiceClientAuth_Destroy(iotHubServiceClientHandle);
    IoTHubClient_Destroy(iotHubClientHandle);
    device_desired_deinit(device);
}


// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include "iothubtransportmqtt.h"
#include "schemalib.h"
#include "iothub_client.h"
#include "serializer_devicetwin.h"
#include "schemaserializer.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/platform.h"

#include <ctype.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wiringPi.h>
#include <wiringPiSPI.h>
#include "bme280.h"
#include "locking.h"

static char* deviceId;
static char* connectionString;

static char* lastUpdateBegin;
static char* lastRebootBegin;

static IOTHUB_CLIENT_HANDLE g_iotHubClientHandle = NULL;

static const int Spi_channel = 0;
static const int Spi_clock = 1000000L;

static const int Grn_led_pin = 7;

static int Lock_fd;

/*json of supported methods*/
static char* supportedMethod = "{ \"LightBlink\": \"light blink\", \"ChangeLightStatus--LightStatusValue-int\""
": \"Change light status, on and off\", \"InitiateFirmwareUpdate--FwPackageURI-string\": "
"\"Updates device Firmware. Use parameter FwPackageURI to specifiy the URI of the firmware file\"}";

// Define the Model
BEGIN_NAMESPACE(Contoso);

/* Reported properties */
DECLARE_STRUCT(SystemProperties,
ascii_char_ptr, FirmwareVersion
);

DECLARE_MODEL(ConfigProperties,
WITH_REPORTED_PROPERTY(uint8_t, TelemetryInterval)
);

/* Part of DeviceInfo */
DECLARE_STRUCT(DeviceProperties,
ascii_char_ptr, DeviceID,
_Bool, HubEnabledState
);

DECLARE_DEVICETWIN_MODEL(Thermostat,
/* Telemetry (temperature, external temperature and humidity) */
WITH_DATA(double, Temperature),
WITH_DATA(double, Humidity),
WITH_DATA(ascii_char_ptr, DeviceId),

/* DeviceInfo */
WITH_DATA(ascii_char_ptr, ObjectType),
WITH_DATA(_Bool, IsSimulatedDevice),
WITH_DATA(ascii_char_ptr, Version),
WITH_DATA(DeviceProperties, DeviceProperties),

/* Device twin properties */
WITH_REPORTED_PROPERTY(ConfigProperties, Config),
WITH_REPORTED_PROPERTY(SystemProperties, System),

WITH_DESIRED_PROPERTY(uint8_t, TelemetryInterval, onDesiredTelemetryInterval),

/* Direct methods implemented by the device */
WITH_METHOD(LightBlink),
WITH_METHOD(ChangeLightStatus, int, LightStatusValue),
WITH_METHOD(InitiateFirmwareUpdate, ascii_char_ptr, FwPackageURI),

/* Register direct methods with solution portal */
WITH_REPORTED_PROPERTY(ascii_char_ptr_no_quotes, SupportedMethods)
);

END_NAMESPACE(Contoso);

/* Callback after sending reported properties */
void deviceTwinCallback(int status_code, void* userContextCallback)
{
	(void)(userContextCallback);
	printf("IoTHub: reported properties delivered with status_code = %u\n", status_code);
}

void onDesiredTelemetryInterval(void* argument)
{
	/* By convention 'argument' is of the type of the MODEL */
	Thermostat* thermostat = argument;
	printf("Received a new desired_TelemetryInterval = %d\r\n", thermostat->TelemetryInterval);
	thermostat->Config.TelemetryInterval = thermostat->TelemetryInterval;
	if (IoTHubDeviceTwin_SendReportedStateThermostat(thermostat, deviceTwinCallback, NULL) != IOTHUB_CLIENT_OK)
	{
		printf("Report Config.TelemetryInterval property failed");
	}
	else
	{
		printf("Report new value of Config.TelemetryInterval property: %d\r\n", thermostat->Config.TelemetryInterval);
	}
}

/*change light status on Raspberry Pi to received value*/
METHODRETURN_HANDLE ChangeLightStatus(Thermostat* thermostat, int lightstatus)
{
	printf("Raspberry Pi light status change\n");
	pinMode(Grn_led_pin, OUTPUT);
	printf("LED value\n %d", lightstatus);
	digitalWrite(Grn_led_pin, lightstatus);
	return MethodReturn_Create(201, "\"light status changed\"");
}



time_t ReadFormatedTime(const char *time_details)
{
	struct tm tm;
	strptime(time_details, "%Y-%m-%d %H:%M:%S", &tm);
	time_t t = mktime(&tm);
	printf("time: %s\r\n", time_details);
	return t;
}

void WriteConfig()
{
	FILE* fp;

	if (NULL == (fp = fopen("//home//pi//lastupdate", "w")))
	{
		printf("Failed to open lastupdate file to write\r\n");
	}
	else
	{
		printf("last update begin value: %s\r\n", lastUpdateBegin);
		printf("last reboot begin value: %s\r\n", lastRebootBegin);
		fprintf(fp, "%s\r\n%s", lastUpdateBegin, lastRebootBegin);
		fclose(fp);
	}
}

void LoadConfig()
{
	FILE* fp;

	if (NULL == (fp = fopen("//home//pi//deviceInfo", "r")))
	{
		printf("Failed to open deviceInfo file to read\r\n");
	}
	else
	{
		char line[256] = { 0 };

		for (int i = 0; i < 2; i++)
		{
			if (fgets(line, sizeof(line), fp)) {
				if (i == 0)
				{
					line[(int)(strlen(line) - 1)] = '\0';
					deviceId = strdup(line);
					printf("read device id: %s\r\n", deviceId);
				}
				else
				{
					connectionString = strdup(line);
					printf("read connection string: %s\r\n", connectionString);
				}
			}
		}

		fclose(fp);
	}

	if (NULL == (fp = fopen("//home//pi//lastupdate", "r")))
	{
		printf("Failed to open lastupdate file to read\r\n");
	}
	else
	{
		int length = 0;
		char buffer[256] = { 0 };
		for (int i = 0; i < 2; i++)
		{
			if (fgets(buffer, sizeof(buffer), fp)) {
				if (i == 0)
				{
					lastUpdateBegin = strdup(buffer);
					printf("firmware update begin %s\r\n", lastUpdateBegin);
				}
				else
				{
					lastRebootBegin = strdup(buffer);
					printf("firmware update reboot %s\r\n", lastRebootBegin);
				}
			}
		}
		fclose(fp);
	}
}

bool GetNumberFromString(const unsigned char* text, size_t size, int* pValue)
{
	const unsigned char* pStart = text;
	for (; pStart < text + size; pStart++)
	{
		if (isdigit(*pStart))
		{
			break;
		}
	}

	const unsigned char* pEnd = pStart + 1;
	for (; pEnd <= text + size; pEnd++)
	{
		if (!isdigit(*pEnd))
		{
			break;
		}
	}

	if (pStart >= text + size)
	{
		return false;
	}

	unsigned char buffer[16] = { 0 };
	strncpy(buffer, pStart, pEnd - pStart);

	*pValue = atoi(buffer);
	return true;
}

char* FormatTime(time_t* time)
{
	static char buffer[128];

	struct tm* p = gmtime(time);

	strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", p);

	return buffer;
}

//download file in Git hub repo via wget as example
bool DownloadFile(ascii_char_ptr url)
{
	printf("Download url: %s\r\n", url);
	char str[256];
	sprintf(str, "wget %s", url);
	return system(str) == 0;
}

void AllocAndVPrintf(unsigned char** buffer, size_t* size, const char* format, va_list argptr)
{
	*size = vsnprintf(NULL, 0, format, argptr);

	*buffer = malloc(*size + 1);
	vsprintf((char*)*buffer, format, argptr);
}

void UpdateReportedProperties(const char* format, ...)
{
	unsigned char* report;
	size_t len;

	va_list args;
	va_start(args, format);
	AllocAndVPrintf(&report, &len, format, args);
	va_end(args);

	if (IoTHubClient_SendReportedState(g_iotHubClientHandle, report, len, NULL, NULL) != IOTHUB_CLIENT_OK)
	{
		(void)printf("Failed to update reported properties: %.*s\r\n", len, report);
	}
	else
	{
		(void)printf("Succeeded in updating reported properties: %.*s\r\n", len, report);
	}

	free(report);
}

//unzip the target package to the specify folder, then move it the downloaded file
void ApplyFirmware()
{
	system("unzip -o remote_monitoring.zip -d cmake/remote_monitoring/");
	system("sudo mv cmake/remote_monitoring/firmwarereboot.sh .");
	system("sudo chmod +x firmwarereboot.sh");
	system("sed -i 's/\r//' firmwarereboot.sh");
	system("sudo chmod +x cmake/remote_monitoring/remote_monitoring");
	system("rm -f remote_monitoring.zip");
	system("sudo nohup sh ./firmwarereboot.sh > /tmp/reboot.txt &");
}

void* FirmwareUpdateThread(void* arg)
{
	time_t begin, end, stepBegin, stepEnd;
	printf("Firmware thread start, download url: %s\r\n", (char*)arg);
	ascii_char_ptr url = arg;

	// Clear all reportes
	UpdateReportedProperties("{ 'Method' : { 'UpdateFirmware': null } }");
	time(&begin);
	char * beginUpdate = FormatTime(&begin);
	lastUpdateBegin = malloc(strlen(beginUpdate) + 1);
	strcpy(lastUpdateBegin, beginUpdate);
	UpdateReportedProperties(
		"{ 'Method' : { 'UpdateFirmware': { 'Duration-s': 0, 'LastUpdate': '%s', 'Status': 'Running' } } }",
		beginUpdate);

	time(&stepBegin);
	UpdateReportedProperties(
		"{ 'Method' : { 'UpdateFirmware': { 'Download' : { 'Duration-s': 0, 'LastUpdate': '%s', 'Status': 'Running' } } } }",
		FormatTime(&stepBegin));

	time(&stepEnd);
	//downloadfile
	if (!DownloadFile(url))
	{
		UpdateReportedProperties(
			"{ 'Method' : { 'UpdateFirmware': { 'Download' : { 'Duration-s': %u, 'LastUpdate': '%s', 'Status': 'Failed' } } } }",
			stepEnd - stepBegin,
			FormatTime(&stepEnd));

		time(&end);
		UpdateReportedProperties(
			"{ 'Method' : { 'UpdateFirmware': { 'Duration-s': %u, 'LastUpdate': '%s', 'Status': 'Failed' } } }",
			end - begin,
			FormatTime(&end));
		return NULL;
	}

	time(&stepEnd);

	UpdateReportedProperties(
		"{ 'Method' : { 'UpdateFirmware': { 'Download' : { 'Duration-s': %u, 'LastUpdate': '%s', 'Status': 'Complete' } } } }",
		stepEnd - stepBegin,
		FormatTime(&stepEnd));

	time(&stepBegin);
	UpdateReportedProperties(
		"{ 'Method' : { 'UpdateFirmware': { 'Applied' : { 'Duration-s': 0, 'LastUpdate': '%s', 'Status': 'Running' } } } }",
		FormatTime(&stepBegin));

	printf("unlock file before apply new firmware\r\n");
	close_lockfile(Lock_fd);

	ApplyFirmware();

	time(&stepEnd);
	UpdateReportedProperties(
		"{ 'Method' : { 'UpdateFirmware': { 'Applied' : { 'Duration-s': %u, 'LastUpdate': '%s', 'Status': 'Complete' } } } }",
		stepEnd - stepBegin,
		FormatTime(&stepEnd));

	time(&stepBegin);
	char * rebootBegin = FormatTime(&stepBegin);
	UpdateReportedProperties(
		"{ 'Method' : { 'UpdateFirmware': { 'Reboot' : { 'Duration-s': 0, 'LastUpdate': '%s', 'Status': 'Running' } } } }",
		rebootBegin);

	lastRebootBegin = malloc(strlen(rebootBegin) + 1);
	strcpy(lastRebootBegin, rebootBegin);
	WriteConfig();
	free(arg);
	exit(0);
}

void UpdateFirmwareComplete()
{
	if (lastRebootBegin == NULL || lastUpdateBegin == NULL)
		return;
	printf("start send firmware update complete");
	time_t begin, end, stepBegin, stepEnd;
	stepBegin = ReadFormatedTime(lastRebootBegin);
	time(&stepEnd);
	UpdateReportedProperties(
		"{ 'Method' : { 'UpdateFirmware': { 'Reboot' : { 'Duration-s': %u, 'LastUpdate': '%s', 'Status': 'Complete' } } } }",
		stepEnd - stepBegin,
		FormatTime(&stepEnd));

	begin = ReadFormatedTime(lastUpdateBegin);
	time(&end);
	UpdateReportedProperties(
		"{ 'Method' : { 'UpdateFirmware': { 'Duration-s': %u, 'LastUpdate': '%s', 'Status': 'Complete' } } }",
		end - begin,
		FormatTime(&end));
	printf("finsh send firmware update complete");
	//clean up lastupdate log
	FILE* fp;
	if (NULL == (fp = fopen("//home//pi//lastupdate", "w")))
	{
		printf("Failed to open lastupdate file to write\r\n");
	}
	else
	{
		fclose(fp);
	}
}

METHODRETURN_HANDLE InitiateFirmwareUpdate(Thermostat* thermostat, ascii_char_ptr FwPackageURI)
{
	(void)(thermostat);

	METHODRETURN_HANDLE result = MethodReturn_Create(201, "\"Initiating Firmware Update\"");
	printf("Recieved firmware update request. Use package at: %s\r\n", FwPackageURI);
	pthread_t tid;
	ascii_char_ptr url = malloc(strlen(FwPackageURI) + 1);
	strcpy(url, FwPackageURI);
	printf("receive and strcpy url: %s\r\n", url);
	pthread_create(&tid, NULL, &FirmwareUpdateThread, url);
	return result;
}

METHODRETURN_HANDLE LightBlink(Thermostat* thermostat)
{
	int blinkCount = 2;
	printf("Raspberry Pi light blink\n");
	while (blinkCount--)
	{
		pinMode(Grn_led_pin, OUTPUT);
		printf("light on\n");
		digitalWrite(Grn_led_pin, 1);
		ThreadAPI_Sleep(1000);
		printf("light off\n");
		digitalWrite(Grn_led_pin, 0);
		ThreadAPI_Sleep(1000);
	}
	return MethodReturn_Create(201, "\"light blink success\"");
}

/* Send data to IoT Hub */
static void sendMessage(IOTHUB_CLIENT_HANDLE iotHubClientHandle, const unsigned char* buffer, size_t size)
{
	IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromByteArray(buffer, size);
	if (messageHandle == NULL)
	{
		printf("unable to create a new IoTHubMessage\r\n");
	}
	else
	{
		if (IoTHubClient_SendEventAsync(iotHubClientHandle, messageHandle, NULL, NULL) != IOTHUB_CLIENT_OK)
		{
			printf("failed to hand over the message to IoTHubClient");
		}
		else
		{
			printf("IoTHubClient accepted the message for delivery\r\n");
		}

		IoTHubMessage_Destroy(messageHandle);
	}
	free((void*)buffer);
}

void remote_monitoring_run(void)
{
	if (platform_init() != 0)
	{
		printf("Failed to initialize the platform.\n");
	}
	else
	{
		if (SERIALIZER_REGISTER_NAMESPACE(Contoso) == NULL)
		{
			printf("Unable to SERIALIZER_REGISTER_NAMESPACE\n");
		}
		else
		{
			IOTHUB_CLIENT_HANDLE iotHubClientHandle = IoTHubClient_CreateFromConnectionString(connectionString, MQTT_Protocol);
			g_iotHubClientHandle = iotHubClientHandle;
			if (iotHubClientHandle == NULL)
			{
				printf("Failure in IoTHubClient_CreateFromConnectionString\n");
			}
			else
			{
#ifdef MBED_BUILD_TIMESTAMP
				// For mbed add the certificate information
				if (IoTHubClient_SetOption(iotHubClientHandle, "TrustedCerts", certificates) != IOTHUB_CLIENT_OK)
				{
					printf("Failed to set option \"TrustedCerts\"\n");
				}
#endif // MBED_BUILD_TIMESTAMP
				Thermostat* thermostat = IoTHubDeviceTwin_CreateThermostat(iotHubClientHandle);
				if (thermostat == NULL)
				{
					printf("Failure in IoTHubDeviceTwin_CreateThermostat\n");
				}
				else
				{
					/* Set values for reported properties */
					thermostat->Config.TelemetryInterval = 3;
					thermostat->System.FirmwareVersion = "1.0";
					/* Specify the signatures of the supported direct methods */
					thermostat->SupportedMethods = supportedMethod;

					/* Send reported properties to IoT Hub */
					if (IoTHubDeviceTwin_SendReportedStateThermostat(thermostat, deviceTwinCallback, NULL) != IOTHUB_CLIENT_OK)
					{
						printf("Failed sending serialized reported state\n");
					}
					else
					{
						UpdateFirmwareComplete();
						printf("Send DeviceInfo object to IoT Hub at startup\n");
						thermostat->ObjectType = "DeviceInfo";
						thermostat->IsSimulatedDevice = 0;
						thermostat->Version = "1.0";
						thermostat->DeviceProperties.HubEnabledState = 1;
						thermostat->DeviceProperties.DeviceID = deviceId;

						unsigned char* buffer;
						size_t bufferSize;

						if (SERIALIZE(&buffer, &bufferSize, thermostat->ObjectType, thermostat->Version, thermostat->IsSimulatedDevice, thermostat->DeviceProperties) != CODEFIRST_OK)
						{
							(void)printf("Failed serializing DeviceInfo\n");
						}
						else
						{
							sendMessage(iotHubClientHandle, buffer, bufferSize);
							printf("Twin json string is: %s\r\n", buffer);
						}

						/* Send telemetry */
						thermostat->Temperature = 50;
						thermostat->Humidity = 50;
						thermostat->TelemetryInterval = 3;
						thermostat->DeviceId = deviceId;

						while (1)
						{
							unsigned char* buffer;
							size_t bufferSize;
							float tempC = -300.0;
							float pressurePa = -300;
							float humidityPct = -300;

							int sensorResult = bme280_read_sensors(&tempC, &pressurePa, &humidityPct);

							if (sensorResult == 1)
							{
								thermostat->Temperature = tempC;
								thermostat->Humidity = humidityPct;
								printf("Read Sensor Data: Humidity = %.1f%% Temperature = %.1f*C \n",
									humidityPct, tempC);
							}
							else
							{
								thermostat->Temperature = 50;
								thermostat->Humidity = 50;
							}

							(void)printf("Sending sensor value Temperature = %f, Humidity = %f\n", thermostat->Temperature, thermostat->Humidity);

							if (SERIALIZE(&buffer, &bufferSize, thermostat->DeviceId, thermostat->Temperature, thermostat->Humidity) != CODEFIRST_OK)
							{
								(void)printf("Failed sending sensor value\r\n");
							}
							else
							{
								sendMessage(iotHubClientHandle, buffer, bufferSize);
							}

							ThreadAPI_Sleep(thermostat->TelemetryInterval * 1000);
						}

						IoTHubDeviceTwin_DestroyThermostat(thermostat);
					}
				}
				IoTHubClient_Destroy(iotHubClientHandle);
			}
			serializer_deinit();
		}
	}
	platform_deinit();
}

int remote_monitoring_init(void)
{
	int result;

	Lock_fd = open_lockfile(LOCKFILE);

	if (setuid(getuid()) < 0)
	{
		perror("Dropping privileges failed. (did you use sudo?)n");
		result = EXIT_FAILURE;
	}
	else
	{
		result = wiringPiSetup();
		if (result != 0)
		{
			perror("Wiring Pi setup failed.");
		}
		else
		{
			result = wiringPiSPISetup(Spi_channel, Spi_clock);
			if (result < 0)
			{
				printf("Can't setup SPI, error %i calling wiringPiSPISetup(%i, %i)  %sn",
					result, Spi_channel, Spi_clock, strerror(result));
			}
			else
			{
				int sensorResult = bme280_init(Spi_channel);
				if (sensorResult != 1)
				{
					printf("It appears that no BMP280 module on Chip Enable %i is attached. Aborting.\n", Spi_channel);
					result = 1;
				}
				else
				{
					// Read the Temp & Pressure module.
					float tempC = -300.0;
					float pressurePa = -300;
					float humidityPct = -300;
					sensorResult = bme280_read_sensors(&tempC, &pressurePa, &humidityPct);
					if (sensorResult == 1)
					{
						printf("Temperature = %.1f *C  Pressure = %.1f Pa  Humidity = %1f %%\n",
							tempC, pressurePa, humidityPct);
						result = 0;
					}
					else
					{
						printf("Unable to read BME280 on pin %i. Aborting.\n", Spi_channel);
						result = 1;
					}
				}
			}
		}
	}
	return result;
}

int main(void)
{
	LoadConfig();
	int result = remote_monitoring_init();
	if (result == 0)
	{
		remote_monitoring_run();
	}
	return result;
}
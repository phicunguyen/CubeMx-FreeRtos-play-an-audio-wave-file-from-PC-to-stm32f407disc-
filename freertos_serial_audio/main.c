#include <stdio.h>
#include <conio.h>
#include <windows.h>
#include <strsafe.h>
#include <stdbool.h>
#include <sys/stat.h>


/* FreeRTOS.org includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "serial.h"
#include "semphr.h"

/* Demo includes. */
#include "supporting_functions.h"
#define MAX_SERIAL_PORT		10
#define AUDIO_BUFFER_SIZE		 1024
char *serial_list[MAX_SERIAL_PORT] = { NULL };
struct AdapterTypedef_t adapter = { 0 };
uint8_t		bufIn[4098 + 4];
uint32_t	bufSize;

static void get_bin_file(void);
static void play_audio_bin_file(void);

SemaphoreHandle_t xMutex;
SemaphoreHandle_t xAudMutex;
char *pfile = "audio.wav";

/*
* monitor serial data come from stm32 vcp
*/
static void vSerialTask(void *pvParameters) {
	pvParameters = pvParameters;
	while (true) {
		if (adapter.Handle) {
			xSemaphoreTake(xMutex, portMAX_DELAY);
			if (adapter.is_port_open(&adapter)) {		
				adapter.ser_mon(&adapter);
			}
			xSemaphoreGive(xMutex);
		}
		vTaskDelay(2);
		taskYIELD();
	}
}

/*
* Send command to stm32 to blink led1 every 200ms
*/
static void vLedTask1(void *parg) {
	uint8_t buf[12];
	bool led=false;
	parg = parg;
	buf[0] = SERIAL_PACKET__LED;	//opcode low byte
	buf[1] = 0x00;					//opcode high byte
	buf[2] = 0x01;					//led number
	parg = parg;
	while (true) {
		buf[3] = led ? 0x01 : 0x0;		//led on/off
		led = !led;
		xSemaphoreTake(xMutex, portMAX_DELAY);
		adapter.write(&adapter, buf, 4);
		xSemaphoreGive(xMutex);
		//printf("led1: %s\n", led ? "on" : "off");
		vTaskDelay(200);
		taskYIELD();
	}
}

/*
* Send command to stm32 to blink led2 every 200ms
*/
static void vLedTask2(void *parg) {
	uint8_t buf[12];
	bool led = false;
	parg = parg;
	buf[0] = SERIAL_PACKET__LED;	//opcode low byte
	buf[1] = 0x00;					//opcode high byte
	buf[2] = 0x02;					//led number
	while (true) {
		buf[3] = led ? 0x01 : 0x0;		//led on/off
		led = !led;
		xSemaphoreTake(xMutex, portMAX_DELAY);
		adapter.write(&adapter, buf, 4);
		xSemaphoreGive(xMutex);
		//printf("led2: %s\n", led ? "on" : "off");
		vTaskDelay(200);
		taskYIELD();
	}
}

/*
* Send command to stm32 to blink led3 every 200ms
*/
static void vLedTask3(void *parg) {
	uint8_t buf[12];
	bool led = false;
	parg = parg;
	buf[0] = SERIAL_PACKET__LED;	//opcode low byte
	buf[1] = 0x00;					//opcode high byte
	buf[2] = 0x03;					//led number
	while (true) {
		buf[3] = led ? 0x01 : 0x0;		//led on/off
		led = !led;
		xSemaphoreTake(xMutex, portMAX_DELAY);
		adapter.write(&adapter, buf, 4);
		xSemaphoreGive(xMutex);
		//printf("led3: %s\n", led ? "on" : "off");
		vTaskDelay(200);
		taskYIELD();
	}
}

static void vDownloadTask(void *parg) {
	parg=parg;
	get_bin_file();
}

static void vAudioPlayTask(void *parg) {
	parg = parg;
	play_audio_bin_file();
}

/*
* convert from ascii to hex
*/
static uint8_t ascii2hex(uint8_t data) {
	if (data >= 0 && data <= 9) {
		return data + '0';
	}
	else if (data >= 0xa && data <= 0x0f) {
		return data - 10 + 'a' ;
	}
	return 0;
}

/*
* get the stm32 binary from user.
*/
static void get_bin_file(void) {
	uint8_t buf[256];
	FILE *fptr;
	uint32_t addr = 0;
	uint32_t size= 256;

	if ((fptr = fopen(pfile, "rb")) == NULL) { 
		printf("found not found"); 
		return; 
	};

	while (!feof(fptr)) {
		xSemaphoreTake(xMutex, portMAX_DELAY);
		size = fread(&buf[1], sizeof(buf[0]), size, fptr);
		adapter.flash(&adapter, buf, size, addr);
		addr += size;
		xSemaphoreGive(xMutex);
		taskYIELD();
	}
	fclose(fptr);
}

/*
* get the stm32 binary from user.
*/
static void play_audio_bin_file(void) {
	uint8_t buf[AUDIO_BUFFER_SIZE+2];
	FILE *fptr;
	uint32_t size = 44;
	bool payload = true;
	if ((fptr = fopen(pfile, "rb")) == NULL) {
		printf("found not found");
		return;
	};

	printf("audio start playing ...\n");
	while (!feof(fptr)) {
		xSemaphoreTake(xMutex, portMAX_DELAY);
		buf[0] = SERIAL_PACKET__AUDIO_PLAY;
		buf[1] = 0x00;
		if (payload) {
			size = fread(&buf[2], sizeof(buf[0]), size, fptr);
			size += 2;
			payload = false;
		}
		else {
			size = AUDIO_BUFFER_SIZE;
			size = fread(&buf[2], sizeof(buf[0]), size, fptr);
			size += 2;
		}
		adapter.write(&adapter, buf, size);
		xSemaphoreGive(xMutex);
		taskYIELD();
		//printf("Waiting for Audio Semaphore (%d) = %d\n", cnt++, size);
		xSemaphoreTake(xAudMutex, portMAX_DELAY);
	}
	//stop audio.
	printf("audio finish\n");
	buf[0] = SERIAL_PACKET__AUDIO_STOP;
	buf[1] = 0x00;
	adapter.write(&adapter, buf, 2);
	fclose(fptr);
}

void serial_callback(uint8_t *pbuf, uint32_t size) {
	uint16_t opcode;
	static uint32_t cnt = 0;
	size = size;
	opcode = pbuf[0] | (pbuf[1] << 8);
	switch (opcode) {
	case SERIAL_PACKET__LED:
		//printf("led response\n");
		break;
	case SERIAL_PACKET__FLASH_WRITE:
		//printf("write response\n");
		break;
	case SERIAL_PACKET__AUDIO_PLAY:
		xSemaphoreGive(xAudMutex);
		//printf("audio play: %d\n", cnt++);
		break;
	}
}
/*
* create a serial connection.
*/
void serial_get_comport(void) {
	char **p_ptr;

	//check if stm32 comport in the window
	if (serial_devices_mon()) {
		int id = 0;
		//get the stm32 comport list
		serial_devices_get(&serial_list);
		p_ptr = serial_list;
		if (*p_ptr != NULL) {
			printf("\nlist of stm32 comports:\n");
			while (*p_ptr) {
				printf("%d: %s\n", id++, *p_ptr++);
			}
			printf("Please select comport: ");
			scanf("%d", &id);
			//create a serial port instance
			serial_devices_create(&adapter, serial_callback, serial_list[id]);
			//now open it.
			if (serial_devices_open(&adapter)) {
				printf("Serial port open successfully\n");
			}
		}
	}
}

void serial_download(char *fname) {
	pfile = fname;
	xTaskCreate(vDownloadTask, "dowload task", 1024, NULL, 1, NULL);
}

void serial_audio_play(char *fname) {
	pfile = fname;
	xTaskCreate(vAudioPlayTask, "dowload task", 1024, NULL, 1, NULL);
}


void main(void)
{	
	//look for the stm32 comport.
	serial_get_comport();
	xMutex = xSemaphoreCreateMutex();
	xAudMutex = xSemaphoreCreateBinary();

	//task to monitor the serial port add or remove from window
	xTaskCreate(vLedTask1, "led task1", 1024, NULL, 1, NULL);
	xTaskCreate(vLedTask2, "led task2", 1024, NULL, 1, NULL);
	//xTaskCreate(vLedTask3, "led task3", 1024, NULL, 1, NULL);

	//task to read the serial data
	xTaskCreate(vSerialTask, "read data", 1024, NULL, 1, NULL);
	serial_audio_play("audio.wav");
	//xSemaphoreGive(xAudMutex);
	vTaskStartScheduler();
	for (;; );
}

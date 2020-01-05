/*
 * serial.h
 *
 *  Created on: Dec 7, 2019
 *      Author: pnguyen
 */

#ifndef SERIAL_SERIAL_H_
#define SERIAL_SERIAL_H_
#include "main.h"

#define OPCODE__INDEX		0
#define ADDRESS__INDEX		4
#define SIZE__INDEX			(4+8)

enum {
	SERIAL_PACKET__NONE,
	SERIAL_PACKET__FLASH_WRITE,
	SERIAL_PACKET__FLASH_READ,
	SERIAL_PACKET__LED,
	SERIAL_PACKET__AUDIO_PLAY,
	SERIAL_PACKET__AUDIO_STOP,
	SERIAL_PACKET__AUDIO_PAUSE,
};

struct serial_data_t {
	bool	 valid;
	uint32_t opcode;
	uint8_t  buf[4096+4+8+8]; //4-opcode, 8-addr, 8-size
	uint32_t len;
};


void vcp_store_data(uint8_t *ptr, uint32_t len);
void packet_audio_play(uint8_t val);

#endif /* SERIAL_SERIAL_H_ */

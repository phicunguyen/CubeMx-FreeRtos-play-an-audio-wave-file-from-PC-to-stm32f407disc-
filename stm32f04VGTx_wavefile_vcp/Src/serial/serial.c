/*
 * serial.c
 *
 *  Created on: Dec 7, 2019
 *      Author: pnguyen
 */

/*
 * Convert ascii byte to on hex byte
 */

#include "serial.h"
#include "usbd_cdc_if.h"
#include "../audioplay.h"

/* Private define ------------------------------------------------------------*/
static struct serial_data_t serial_data;
uint8_t raw_buffer[2048];
uint32_t raw_buffer_size=0;
/* Private functions ------------------------------------------------------------*/
static void packet_response(uint16_t opcode, uint8_t *pbuf, uint32_t size);


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
static void s_hex_to_ascci(uint8_t **pbuf, uint8_t *pdata, uint32_t size) {
	uint8_t *p_data = *pbuf;
	for (int i = 0; i < size; i++) {
		*p_data++ = hex2ascii(*pdata  & 0xf);
		*p_data++ = hex2ascii((*pdata++ >> 4) & 0xf);
	}
	*pbuf = p_data;
}

/*
* convert two ascii bytes to one hex byte.
*/
static uint8_t ascii2hex(uint8_t val) {
	uint8_t res;
	if (val >= '0' && val <= '9') {
		res = val - '0';
	} else if (val >= 'a' && val <= 'f') {
		res = val - 'a' + 10;
	} else if (val >= 'A' && val <= 'F') {
		res = val - 'A' + 10;
	}

	return res;
}

/*
* convert two bytes of ascii to one byte hex.
*/
static uint32_t s_ascii_to_hex(uint16_t index, uint16_t size) {
	uint32_t res=0;
	uint32_t val;

	for (int i = 0; i < size; i++) {
		val = ascii2hex(serial_data.buf[index+i]) << (i*4);
		res |= val;
	}
	return res;
}

/*
 *  return the opcode , opcode is first 4 bytes of the data.
 */
static uint16_t s_get_opcode(void) {
	return (uint16_t)s_ascii_to_hex(OPCODE__INDEX, 2*2);
}

#if 0
static uint32_t s_get_addr(void) {
	return s_ascii_to_hex(ADDRESS__INDEX, 4*2);
}

/*
 * get the szie of the packet
 */
static uint32_t s_get_szie(void) {
	return (uint16_t)s_ascii_to_hex(SIZE__INDEX, 4*2);
}
#endif
/*
 * return led number and state
 */
static void led_get(uint8_t *led, uint8_t *state) {
	uint16_t val = (uint16_t)s_ascii_to_hex(4, 2*2);
	*led = val & 0xff;
	*state = (val >> 8) & 0xff;
}

/*
 *  toggle the led based on led packet.
 */
static void s_led_packet(void) {
	uint8_t led;
	uint8_t state;
	led_get(&led, &state);
	switch(led) {
	case 0:
		HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
		break;
	case 1:
		HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13,  state ? GPIO_PIN_SET : GPIO_PIN_RESET);
		break;
	case 2:
		HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14,  state ? GPIO_PIN_SET : GPIO_PIN_RESET);
		break;
	case 3:
		HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15,  state ? GPIO_PIN_SET : GPIO_PIN_RESET);
		break;
	}
	packet_response(SERIAL_PACKET__LED, NULL, 0);
}

/*
 * parse the packet
 */
static void packet_response(uint16_t opcode, uint8_t *pbuf, uint32_t size) {
	uint8_t *p_data = serial_data.buf;
	*p_data++ = '[';
	s_hex_to_ascci(&p_data, (uint8_t *)&opcode, sizeof(opcode));
	if (size) { //if response packet has data.
		s_hex_to_ascci(&p_data, pbuf, size);
	}
	*p_data++ = ']';
	CDC_Transmit_FS(serial_data.buf, p_data - serial_data.buf);
}

/*
 * parse the packet
 */
void packet_audio_play(uint8_t val) {
	packet_response(SERIAL_PACKET__AUDIO_PLAY, &val, 1);
}

static void s_to_rawdata(void) {
	for (uint32_t i=4; i < serial_data.len; i+=2) {
		uint8_t data = ascii2hex(serial_data.buf[i]);
		data |= ascii2hex(serial_data.buf[i+1]) << 4;
		raw_buffer[raw_buffer_size++] = data;
	}
}

/*
 * parse the packet
 */
static void parse_packet(void) {
	switch(s_get_opcode()) {
		case SERIAL_PACKET__LED:
			s_led_packet();
			break;
		case SERIAL_PACKET__FLASH_WRITE:
			packet_response(SERIAL_PACKET__FLASH_WRITE, NULL, 0);
			break;
		case SERIAL_PACKET__AUDIO_PLAY:
			s_to_rawdata();
			AudioOut_Play(raw_buffer, raw_buffer_size);
			raw_buffer_size = 0;
			break;
		case SERIAL_PACKET__AUDIO_STOP:
			//AudioOut_Stop();
			break;
		default:
			break;
	}
}

/*
 * Packet start with '[' and end with ']'
 * data in between is ascii.
 * first 4 bytes is opcode
 * next 8 bytes is address
 * 4 bytes after address is the size
 * [opcode(2) address(4) size(2) data....]
 * One byte of hex is two bytes of ascii.
 *   opcode is an uint16_t -- 2*2 = 4bytes
 *   address is an uint32_t -- 4*2 = 8bytes
 *   size is an uint16_t -- 2*2 = 4bytes
 * */
void vcp_store_data(uint8_t *ptr, uint32_t len) {
	for (uint32_t i=0; i < len; i++){
		switch (ptr[i]) {
		case '[': //begin packet receiving
			serial_data.len = 0;
			serial_data.valid=true;
			raw_buffer_size = 0;
			continue;
		case ']': //endpacket receiving
			serial_data.valid=false;
			parse_packet();
			continue;
		default: //strore the data in the buffer
			if (serial_data.valid) {
				serial_data.buf[serial_data.len++] = ptr[i];
			}
			continue;
		}
	}
}



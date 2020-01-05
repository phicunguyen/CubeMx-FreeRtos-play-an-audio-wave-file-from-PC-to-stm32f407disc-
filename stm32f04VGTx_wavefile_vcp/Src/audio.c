#include "audioplay.h"
#include <stdio.h>
#include <string.h>
#include <usbd_cdc_if.h>
#include <serial.h>
// ———————————————————

#define I2S_STANDARD I2S_STANDARD_PHILIPS

/// * Audio status definition * /    
#define AUDIO_OK 0
#define AUDIO_ERROR 1
#define AUDIO_TIMEOUT 2

/// * Position in the audio play buffer * /
__IO BUFFER_StateTypeDef buffer_offset = BUFFER_OFFSET_NONE;

/// * Codec output DEVICE * /
#define OUTPUT_DEVICE_SPEAKER 1
#define OUTPUT_DEVICE_HEADPHONE 2
#define OUTPUT_DEVICE_BOTH 3
#define OUTPUT_DEVICE_AUTO 4

/// * MUTE commands * /
#define AUDIO_MUTE_ON 1
#define AUDIO_MUTE_OFF 0

/// * Defines for the Audio playing process * /
#define PAUSE_STATUS ((uint32_t) 0x00) / * Audio Player in Pause Status * /
#define RESUME_STATUS ((uint32_t) 0x01) / * Audio Player in Resume Status * /
#define IDLE_STATUS ((uint32_t) 0x02) / * Audio Player in Idle Status * /

#define AUDIO_RESET_GPIO_CLK_ENABLE () __GPIOD_CLK_ENABLE ()
#define AUDIO_RESET_PIN GPIO_PIN_4
#define AUDIO_RESET_GPIO GPIOD
#define VOLUME_CONVERT(Volume) (((Volume)> 100)? 100: ((uint8_t) (((Volume) * 255) / 100)))

#define CODEC_STANDARD 0x04

/// * Variables used in normal mode to manage audio file during DMA transfer * /
uint32_t AudioTotalSize = 0xFFFF; /// * This variable holds the total size of the audio file * /

int32_t AudioRemSize = 0xFFFF; /// * This variable holds the remaining data in audio file * /

static uint8_t Is_cs43l22_Stop = 1;

__IO uint32_t PauseResumeStatus = 0; //phi_check IDLE_STATUS;  

#define CS43L22_REG_MISC_CTL 0x0E
#define AUDIO_I2C_ADDRESS 0x94
#define CS43L22_CHIPID_ADDR 0x01
#define CS43L22_ID 0xE0
#define CS43L22_ID_MASK 0xF8
#define AUDIO_BUFFER_FULLSIZE 2048
#define AUDIO_BUFFER_HALFSIZE (AUDIO_BUFFER_FULLSIZE/2)
#define DMA_MAX_SZE 0xFFFF
#define DMA_MAX(_X_) (((_X_) <= DMA_MAX_SZE) ? (_X_) : DMA_MAX_SZE)
#define AUDIODATA_SIZE 2 /* 16-bits audio data size */

const uint32_t I2SFreq [8] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 96000};
const uint32_t I2SPLLN [8] = {256, 429, 213, 429, 426, 271, 258, 344};
const uint32_t I2SPLLR [8] = {5, 4, 4, 4, 4, 6, 3, 1};
volatile uint8_t OutputDev = 0;
bool audio_rdy=false;

extern I2C_HandleTypeDef hi2c1;
extern I2S_HandleTypeDef hi2s3;
uint8_t Audio_Buffer [AUDIO_BUFFER_FULLSIZE];
extern WAVE_FormatTypeDef * waveformat;
bool audio_buf_pointer=false;
AUDIO_StateMachine Audio;
uint32_t samplerate;
char str2 [20];
char str3 [20];
uint32_t offsetpos;
uint32_t cnt;
uint32_t dur; // variable for the remaining file playback time

enum  aud_state_t{
	AUDIO_STATE__INIT,
	AUDIO_STATE__PLAY,
	AUDIO_STATE__STOP,
} aud_state;

static uint32_t audio_size=0;
enum aud_state_t audState = AUDIO_STATE__INIT;

static uint32_t cs43l22_Play (uint16_t DeviceAddr);

static void Error (void) {
    //HAL_GPIO_WritePin (GPIOD, GPIO_PIN_14, GPIO_PIN_SET);
}

static uint8_t CODEC_IO_Write (uint8_t Addr, uint8_t Reg, uint8_t Value) {

  HAL_StatusTypeDef status = HAL_OK;
  uint32_t result = 0;
  status = HAL_I2C_Mem_Write (& hi2c1, Addr, (uint16_t) Reg, I2C_MEMADD_SIZE_8BIT, & Value, 1, 0x1000);
  if (status!= HAL_OK){
    Error ();
    return 1;
  }
  return result;
}

static uint32_t cs43l22_ReadID(uint16_t DeviceAddr) {

  HAL_StatusTypeDef status = HAL_OK;
  uint8_t value = 0;
  status = HAL_I2C_Mem_Read (& hi2c1, DeviceAddr, (uint16_t) CS43L22_CHIPID_ADDR, I2C_MEMADD_SIZE_8BIT, & value, 1.0,  1000);

  if (status == HAL_OK) {
    value = (value & CS43L22_ID_MASK);
    return ((uint32_t) value);
  }
  return 0;
}

static void I2S3_Init (uint32_t AudioFreq) {
    hi2s3.Instance = I2S3;
    /* Disable I2S block */
    __HAL_I2S_DISABLE (& hi2s3);
    hi2s3.Init.AudioFreq = AudioFreq;
    hi2s3.Init.Standard = I2S_STANDARD;
    HAL_I2S_DeInit (& hi2s3);
    if (HAL_I2S_Init (& hi2s3) != HAL_OK) {
        Error ();
    }
}

static uint32_t cs43l22_SetVolume (uint16_t DeviceAddr, uint8_t Volume) {
  uint32_t counter = 0;
  uint8_t convertedvol = VOLUME_CONVERT(Volume);
  if (Volume> 0xE6){
    /* Set the Master volume */
    counter += CODEC_IO_Write (DeviceAddr, 0x20, convertedvol - 0xE7);
    counter += CODEC_IO_Write (DeviceAddr, 0x21, convertedvol - 0xE7);    
  } else {
    /* Set the Master volume */
    counter += CODEC_IO_Write (DeviceAddr, 0x20, convertedvol + 0x19);
    counter += CODEC_IO_Write (DeviceAddr, 0x21, convertedvol + 0x19);
  }
  return counter;
}

static uint32_t cs43l22_Init (uint16_t DeviceAddr, uint16_t OutputDevice, uint8_t Volume, uint32_t AudioFreq) {
    
  uint32_t counter = 0;
  HAL_GPIO_WritePin (AUDIO_RESET_GPIO, AUDIO_RESET_PIN, GPIO_PIN_RESET);
  /* Wait for a delay to insure registers erasing */
  HAL_Delay (5);
  /* Power on the codec */
  HAL_GPIO_WritePin (AUDIO_RESET_GPIO, AUDIO_RESET_PIN, GPIO_PIN_SET);
  /* Wait for a delay to insure registers erasing */
  HAL_Delay (5);
  counter += CODEC_IO_Write(DeviceAddr, 0x02, 0x01);

  /* Save Output device for mute ON / OFF procedure */
  switch (OutputDevice) {
    case OUTPUT_DEVICE_SPEAKER:
        OutputDev = 0xFA;
        break;
    case OUTPUT_DEVICE_HEADPHONE:
        OutputDev = 0xAF;
        break;
    case OUTPUT_DEVICE_BOTH:
        OutputDev = 0xAA;
        break;
    case OUTPUT_DEVICE_AUTO:
        OutputDev = 0x05;
        break;    
    default:
        OutputDev = 0x05;
        break;  
    }

  counter += CODEC_IO_Write (DeviceAddr, 0x04, OutputDev);
  /* Clock configuration: Auto detection */  
  counter += CODEC_IO_Write (DeviceAddr, 0x05, 0x81);
  /* Set the Slave Mode and the audio Standard */  
  counter += CODEC_IO_Write (DeviceAddr, 0x06, CODEC_STANDARD);
  /* Set the Master volume */
  counter += cs43l22_SetVolume (DeviceAddr, Volume);

  /* If the Speaker is enabled, set the Mono mode and volume attenuation level */
  if (OutputDevice != OUTPUT_DEVICE_HEADPHONE){
    /* Set the Speaker Mono mode */  
    counter += CODEC_IO_Write (DeviceAddr, 0x0F, 0x06);
    /* Set the Speaker attenuation level */  
    counter += CODEC_IO_Write (DeviceAddr, 0x24, 0x00);
    counter += CODEC_IO_Write (DeviceAddr, 0x25, 0x00);
  }

  /* Disable the analog soft ramp */
  counter += CODEC_IO_Write (DeviceAddr, 0x0A, 0x00);
  /*Disable the digital soft ramp */
  counter += CODEC_IO_Write (DeviceAddr, 0x0E, 0x04);
  /*Disable the limiter attack level */
  counter += CODEC_IO_Write (DeviceAddr, 0x27, 0x00);
  /*Adjust Bass and Treble levels */
  counter += CODEC_IO_Write (DeviceAddr, 0x1F, 0x0F);
  /* Adjust PCM volume level */
  counter += CODEC_IO_Write (DeviceAddr, 0x1A, 0x0A);
  counter += CODEC_IO_Write (DeviceAddr, 0x1B, 0x0A);    
  counter += CODEC_IO_Write (DeviceAddr, 0x1B, 0x0A);
  I2S3_Init (AudioFreq);   
  return counter;
}


uint8_t AudioOut_Init (uint16_t OutputDevice, uint8_t Volume, uint32_t AudioFreq) {
  uint8_t ret = AUDIO_ERROR;
  uint32_t deviceid = 0x00;
    
  RCC_PeriphCLKInitTypeDef rccclkinit;
  uint8_t index = 0, freqindex = 0xFF;
  for (index = 0; index <8; index ++){
    if (I2SFreq [index] == AudioFreq) {
      freqindex = index;
    }
  }    
  
  /*Enable PLLI2S clock */
  HAL_RCCEx_GetPeriphCLKConfig (&rccclkinit);

  /* PLLI2S_VCO Input = HSE_VALUE / PLL_M = 1 Mhz */
  if ((freqindex & 0x7) == 0) {

    /* I2S clock config
    PLLI2S_VCO = f (VCO clock) = f(PLLI2S clock input) X (PLLI2SN / PLLM)
    I2SCLK = f (PLLI2S clock output) = f (VCO clock) / PLLI2SR */
    rccclkinit.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    rccclkinit.PLLI2S.PLLI2SN = I2SPLLN [freqindex];
    rccclkinit.PLLI2S.PLLI2SR = I2SPLLR [freqindex];
    HAL_RCCEx_PeriphCLKConfig (& rccclkinit);
  } else {
    /* I2S clock config
    PLLI2S_VCO = f (VCO clock) = f(PLLI2S clock input) X (PLLI2SN / PLLM)
    I2SCLK = f (PLLI2S clock output) = f (VCO clock) / PLLI2SR */
    rccclkinit.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    rccclkinit.PLLI2S.PLLI2SN = 258;
    rccclkinit.PLLI2S.PLLI2SR = 3;
    HAL_RCCEx_PeriphCLKConfig (&rccclkinit);
  }

  HAL_GPIO_WritePin (AUDIO_RESET_GPIO, AUDIO_RESET_PIN, GPIO_PIN_RESET);
  /* Wait for a delay to insure registers erasing */
  HAL_Delay (5);
  /* Power on the codec */
  HAL_GPIO_WritePin (AUDIO_RESET_GPIO, AUDIO_RESET_PIN, GPIO_PIN_SET);
  /* Wait for a delay to insure registers erasing */
  HAL_Delay (5);

  deviceid = cs43l22_ReadID (AUDIO_I2C_ADDRESS);
  if ((deviceid & CS43L22_ID_MASK) == CS43L22_ID) {
    cs43l22_Init (AUDIO_I2C_ADDRESS, OutputDevice, Volume, AudioFreq);
    ret = AUDIO_OK;
  }

  return ret;
}

uint32_t cs43l22_SetMute (uint16_t DeviceAddr, uint32_t Cmd) {
    uint32_t counter = 0;

    /* Set the Mute mode */
    if (Cmd == AUDIO_MUTE_ON) {
        counter += CODEC_IO_Write (DeviceAddr, 0x04, 0xFF);
    } else /* AUDIO_MUTE_OFF Disable the Mute */ {
        counter += CODEC_IO_Write (DeviceAddr, 0x04, OutputDev);
    }
    return counter;
}

static uint32_t cs43l22_Play (uint16_t DeviceAddr) {
    uint32_t counter = 0;
    if (Is_cs43l22_Stop == 1) {
        /* Enable the digital soft ramp */
        counter += CODEC_IO_Write (DeviceAddr, CS43L22_REG_MISC_CTL, 0x06);
        /* Enable Output device */  
        counter += cs43l22_SetMute (DeviceAddr, AUDIO_MUTE_OFF);
        /* Power on the Codec */
        counter += CODEC_IO_Write (DeviceAddr, 0x02, 0x9E);  
        Is_cs43l22_Stop = 0;
    }
    return counter;  
}

static uint32_t cs43l22_Stop (uint16_t DeviceAddr) {
  uint32_t counter = 0;
  /* Mute the output first */
  counter += cs43l22_SetMute (DeviceAddr, AUDIO_MUTE_ON);
  /* Power down the DAC and the speaker (PMDAC and PMSPK bits) */
  counter += CODEC_IO_Write (DeviceAddr, 0x02, 0x9F);
  Is_cs43l22_Stop = 1;
  return counter;    
}

void AudioPlay_Stop (void) {
    HAL_Delay (1);
    HAL_I2S_DMAStop (& hi2s3);
    HAL_Delay (1);
    cs43l22_Stop (AUDIO_I2C_ADDRESS);
}


void AudioPlay_Init (uint32_t AudioFreq){
    samplerate = AudioFreq;
    __IO uint8_t volume = 70;
    if (AudioOut_Init (OUTPUT_DEVICE_AUTO, volume, samplerate) != 0) {
        Error ();
    }    
}


void  AudioPlay_HalfTransfer_CallBack(void) {
  buffer_offset = BUFFER_OFFSET_HALF;
  packet_audio_play(1);
}

void  AudioPlay_TransferComplete_CallBack(void) {
    buffer_offset = BUFFER_OFFSET_FULL;
    if(!Is_cs43l22_Stop) {
    	packet_audio_play(2);
        HAL_I2S_Transmit_DMA(&hi2s3, (uint16_t*)&Audio_Buffer[0], AUDIO_BUFFER_HALFSIZE);
    } else {
        cs43l22_SetVolume(AUDIO_I2C_ADDRESS, 0);
    }
}
 
void AudioOut_Play(uint8_t *paud, uint32_t size) {
	switch(audState) {
	case AUDIO_STATE__INIT:
		if (audio_size == 0) { //get the audio wave payload
			memcpy(&Audio_Buffer[0], paud, size);
			waveformat = (WAVE_FormatTypeDef *)malloc(sizeof(WAVE_FormatTypeDef));
			memcpy(waveformat, Audio_Buffer, 44 );
			packet_audio_play(1);
		} else if (audio_size < AUDIO_BUFFER_HALFSIZE) {
			//fill the first audio block
			memcpy(&Audio_Buffer[0], paud, size);
			packet_audio_play(1);
		} else {
			//fill the second half of the audio block
			memcpy(&Audio_Buffer[AUDIO_BUFFER_HALFSIZE], paud, size);
			//start to play the audio.
			audState = AUDIO_STATE__PLAY;
			//audio_rdy = true;
			//serial_audio_play();
			AudioPlay_Init (waveformat-> SampleRate);
			if (cs43l22_Play(AUDIO_I2C_ADDRESS) != 0) {
				Error ();
			} else {
				HAL_I2S_Transmit_DMA(& hi2s3, (uint16_t *)&Audio_Buffer[0], DMA_MAX(AUDIO_BUFFER_FULLSIZE / AUDIODATA_SIZE));
			}

		}
		audio_size += size;
		break;
	case AUDIO_STATE__PLAY:
		//wait for the audio data coming based on the half or full interrupt
		if (buffer_offset == BUFFER_OFFSET_HALF) {
			memcpy(&Audio_Buffer[0], paud, size);
			buffer_offset = BUFFER_OFFSET_NONE;
		} else if (buffer_offset == BUFFER_OFFSET_FULL) {
			memcpy(&Audio_Buffer[AUDIO_BUFFER_HALFSIZE], paud, size);
			buffer_offset = BUFFER_OFFSET_NONE;
		}
		break;
	case AUDIO_STATE__STOP:
	default:
		AudioOut_Stop();
		break;
	}
}

void AudioOut_Stop(void) {
	audio_size = 0;
	audio_rdy = false;
	AudioPlay_Stop();
	audState = AUDIO_STATE__INIT;
	free(waveformat);
}

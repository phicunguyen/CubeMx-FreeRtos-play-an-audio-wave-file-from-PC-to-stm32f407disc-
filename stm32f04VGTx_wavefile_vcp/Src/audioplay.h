#ifndef AUDIOPLAY_H_
#define AUDIOPLAY_H_


// —————————————————
#include "stm32f4xx_hal.h"
// —————————————————

#define I2S3 SPI3

typedef enum
{
  BUFFER_OFFSET_NONE = 0,  
  BUFFER_OFFSET_HALF,  
  BUFFER_OFFSET_FULL,    
} BUFFER_StateTypeDef;

typedef struct {
  uint32_t ChunkID;
  uint32_t FileSize; /* 4 */
  uint32_t FileFormat; /* 8 */
  uint32_t SubChunk1ID; /* 12 */
  uint32_t SubChunk1Size; /* 16 */  
  uint16_t AudioFormat; /* 20 */
  uint16_t NbrChannels; 
  uint32_t SampleRate; 
  uint32_t ByteRate; 
  uint16_t BlockAlign;  
  uint16_t BitPerSample;   
  uint32_t SubChunk2ID;
  uint32_t SubChunk2Size;     
} WAVE_FormatTypeDef;   

typedef enum {
  AUDIO_IDLE = 0,
  AUDIO_WAIT, 
  AUDIO_EXPLORE,
  AUDIO_PLAYBACK,
  AUDIO_IN,  
} AUDIO_State;

typedef struct _StateMachine {
  __IO AUDIO_State state;
  __IO uint8_t select;  
} AUDIO_StateMachine;

void AudioPlay_Init (uint32_t AudioFreq);
void AudioPlay_Start (uint32_t AudioFreq);
void AudioPlay_TransferComplete_CallBack (void);
void AudioPlay_HalfTransfer_CallBack (void);
void AudioOut_Play(uint8_t *paud, uint32_t size);
void AudioOut_Stop(void);
#endif

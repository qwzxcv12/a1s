#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// I2S Audio Bus (ESP32 <-> Codec ES8388)
#define AUDIO_I2S_GPIO_MCLK      GPIO_NUM_0    // Master Clock
#define AUDIO_I2S_GPIO_BCLK      GPIO_NUM_27   // Bit Clock
#define AUDIO_I2S_GPIO_WS        GPIO_NUM_25   // LRCK
#define AUDIO_I2S_GPIO_DOUT      GPIO_NUM_26   // Data to DAC (Speaker/Headphone)
#define AUDIO_I2S_GPIO_DIN       GPIO_NUM_35   // Data from ADC (Microphone)

// I2C Codec Control (ES8388)
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_33
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_32
#define AUDIO_CODEC_ES8388_ADDR  0x10

// Speaker Power Amplifier Enable (NS4150)
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_21

// Buttons (Active LOW with internal pull-up)
#define BOOT_BUTTON_GPIO         GPIO_NUM_0    // BOOT pin
#define TALK_BUTTON_GPIO         GPIO_NUM_19   // KEY 3 (Push to Talk / Toggle Chat)
#define VOL_DOWN_BUTTON_GPIO     GPIO_NUM_18   // KEY 5
#define VOL_UP_BUTTON_GPIO       GPIO_NUM_5    // KEY 6

// Onboard Status LED
#define BUILTIN_LED_GPIO         GPIO_NUM_22   // LED 4 Green

#endif // _BOARD_CONFIG_H_

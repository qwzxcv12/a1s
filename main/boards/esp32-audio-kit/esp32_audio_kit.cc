#include "wifi_board.h"
#include "audio/codecs/es8388_audio_codec.h"
#include "display/display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>

#define TAG "AudioKitBoard"

class AudioKitBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    Button talk_button_;
    Button vol_up_button_;
    Button vol_down_button_;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeButtons() {
        talk_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        vol_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto vol = codec->output_volume() + 10;
            if (vol > 100) vol = 100;
            codec->SetOutputVolume(vol);
            ESP_LOGI(TAG, "Volume set to %d%%", vol);
        });

        vol_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto vol = codec->output_volume() - 10;
            if (vol < 0) vol = 0;
            codec->SetOutputVolume(vol);
            ESP_LOGI(TAG, "Volume set to %d%%", vol);
        });
    }

public:
    AudioKitBoard() : 
        WifiBoard(),
        boot_button_(BOOT_BUTTON_GPIO),
        talk_button_(TALK_BUTTON_GPIO),
        vol_up_button_(VOL_UP_BUTTON_GPIO),
        vol_down_button_(VOL_DOWN_BUTTON_GPIO) {
        
        ESP_LOGI(TAG, "Initializing Ai-Thinker ESP32-A1S Audio Kit (ES8388)...");
        InitializeI2c();
        InitializeButtons();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8388AudioCodec audio_codec(
            i2c_bus_, 
            I2C_NUM_0, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8388_ADDR
        );
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        static NoDisplay no_display;
        return &no_display;
    }
};

DECLARE_BOARD(AudioKitBoard);

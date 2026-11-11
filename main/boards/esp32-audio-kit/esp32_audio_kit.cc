#include "wifi_board.h"
#include "audio/codecs/es8388_audio_codec.h"
#include "audio/codecs/ac101_audio_codec.h"
#include "display/display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"

#include <esp_log.h>
#include <driver/uart.h>
#include <wifi_manager.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <driver/i2c_master.h>
#include <esp_codec_dev_defaults.h>

#define TAG "AudioKitBoard"

class AudioKitBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    uint8_t codec_addr_ = ES8388_CODEC_DEFAULT_ADDR;
    bool is_ac101_ = false;
    gpio_num_t sda_pin_ = AUDIO_CODEC_I2C_SDA_PIN;
    gpio_num_t scl_pin_ = AUDIO_CODEC_I2C_SCL_PIN;

    Button* talk_button_ = nullptr;
    Button* wifi_button_ = nullptr;
    Button* vol_up_button_ = nullptr;
    Button* vol_down_button_ = nullptr;

    bool ProbeI2cBus(gpio_num_t sda, gpio_num_t scl) {
        ESP_LOGI(TAG, "Probing I2C bus on SDA=%d, SCL=%d...", (int)sda, (int)scl);
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        if (i2c_new_master_bus(&bus_cfg, &i2c_bus_) != ESP_OK) {
            return false;
        }

        // Targeted probe for known codecs on ESP32-A1S:
        // 0x1A: AC101
        // 0x10 or 0x11: ES8388
        const uint16_t candidate_addrs[] = { 0x1A, 0x10, 0x11 };
        for (uint16_t addr : candidate_addrs) {
            if (i2c_master_probe(i2c_bus_, addr, 50) == ESP_OK) {
                ESP_LOGI(TAG, " -> Found I2C device at 7-bit: 0x%02X (8-bit: 0x%02X)", addr, addr << 1);
                sda_pin_ = sda;
                scl_pin_ = scl;
                codec_addr_ = (uint8_t)(addr << 1);
                if (addr == 0x1A) {
                    is_ac101_ = true;
                }
                ESP_LOGI(TAG, "Selected I2C codec address: 0x%02X (%s) on SDA=%d, SCL=%d", 
                         codec_addr_, is_ac101_ ? "AC101" : "ES8388", (int)sda, (int)scl);
                return true;
            }
        }

        i2c_del_master_bus(i2c_bus_);
        i2c_bus_ = nullptr;
        return false;
    }

    void InitializeI2c() {
        // Try Pair 1: GPIO 33 (SDA), GPIO 32 (SCL) - ESP32-A1S v2.2 standard
        if (ProbeI2cBus(GPIO_NUM_33, GPIO_NUM_32)) {
            return;
        }

        // Try Pair 2: GPIO 18 (SDA), GPIO 23 (SCL) - ESP32-A1S v2.3 alternative
        if (ProbeI2cBus(GPIO_NUM_18, GPIO_NUM_23)) {
            return;
        }

        ESP_LOGE(TAG, "Could not auto-detect codec on I2C! Falling back to SDA=33, SCL=32, addr=0x20");
        codec_addr_ = ES8388_CODEC_DEFAULT_ADDR;
        sda_pin_ = GPIO_NUM_33;
        scl_pin_ = GPIO_NUM_32;

        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = sda_pin_,
            .scl_io_num = scl_pin_,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        i2c_new_master_bus(&bus_cfg, &i2c_bus_);
    }

    void InitializeButtons() {
        talk_button_ = new Button(TALK_BUTTON_GPIO);
        talk_button_->OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        if (scl_pin_ != WIFI_RESET_BUTTON_GPIO && sda_pin_ != WIFI_RESET_BUTTON_GPIO) {
            wifi_button_ = new Button(WIFI_RESET_BUTTON_GPIO);
            wifi_button_->OnClick([this]() {
                ESP_LOGI(TAG, "KEY 4 pressed: Entering Wi-Fi Config Mode");
                EnterWifiConfigMode();
            });
        }

        vol_up_button_ = new Button(VOL_UP_BUTTON_GPIO);
        vol_up_button_->OnClick([this]() {
            auto codec = GetAudioCodec();
            auto vol = codec->output_volume() + 10;
            if (vol > 100) vol = 100;
            codec->SetOutputVolume(vol);
            ESP_LOGI(TAG, "Volume set to %d%%", vol);
        });

        if (scl_pin_ != VOL_DOWN_BUTTON_GPIO && sda_pin_ != VOL_DOWN_BUTTON_GPIO) {
            vol_down_button_ = new Button(VOL_DOWN_BUTTON_GPIO);
            vol_down_button_->OnClick([this]() {
                auto codec = GetAudioCodec();
                auto vol = codec->output_volume() - 10;
                if (vol < 0) vol = 0;
                codec->SetOutputVolume(vol);
                ESP_LOGI(TAG, "Volume set to %d%%", vol);
            });
        }
    }

    void HandleSerialCommand(const std::string& cmd) {
        auto& app = Application::GetInstance();
        auto codec = GetAudioCodec();

        if (cmd == "c" || cmd == "chat") {
            if (app.GetDeviceState() == kDeviceStateStarting) {
                printf("[Serial] >> Thiet bi dang khoi dong, vui long doi...\r\n");
                return;
            }
            app.ToggleChatState();
            printf("[Serial] >> Da Toggle Chat! Hay noi vao Micro tren mach (hoac go 'stop' de dung).\r\n");
        } else if (cmd == "s" || cmd == "stop") {
            app.Schedule([&app]() {
                if (app.GetDeviceState() == kDeviceStateListening) {
                    app.StopListening();
                    printf("[Serial] >> Da dung lang nghe.\r\n");
                } else if (app.GetDeviceState() == kDeviceStateSpeaking) {
                    app.AbortSpeaking(kAbortReasonNone);
                    printf("[Serial] >> Da ngat loi AI.\r\n");
                } else {
                    printf("[Serial] >> Thiet bi dang o trang thai Standby (khong nghe/noi).\r\n");
                }
            });
        } else if (cmd == "listen") {
            app.StartListening();
            printf("[Serial] >> Bat dau lang nghe qua Micro...\r\n");
        } else if (cmd == "vol" || cmd == "volume" || cmd == "v") {
            printf("[Serial] >> Am luong hien tai: %d%%\r\n", codec ? codec->output_volume() : 0);
        } else if (cmd.rfind("vol ", 0) == 0 || cmd.rfind("volume ", 0) == 0 || cmd.rfind("v ", 0) == 0) {
            size_t pos = cmd.find(' ');
            int vol = atoi(cmd.substr(pos + 1).c_str());
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            if (codec) {
                codec->SetOutputVolume(vol);
                printf("[Serial] >> Da cai dat am luong loa: %d%%\r\n", vol);
            }
        } else if (cmd == "status" || cmd == "info") {
            auto& wifi = WifiManager::GetInstance();
            const char* state_str = "Unknown";
            switch (app.GetDeviceState()) {
                case kDeviceStateUnknown: state_str = "Unknown"; break;
                case kDeviceStateStarting: state_str = "Starting"; break;
                case kDeviceStateWifiConfiguring: state_str = "WifiConfiguring"; break;
                case kDeviceStateIdle: state_str = "Idle (Standby - San sang)"; break;
                case kDeviceStateConnecting: state_str = "Connecting to server"; break;
                case kDeviceStateListening: state_str = "Listening (Micro dang thu am)"; break;
                case kDeviceStateSpeaking: state_str = "Speaking (AI dang tra loi qua loa)"; break;
                case kDeviceStateUpgrading: state_str = "Upgrading (OTA)"; break;
                case kDeviceStateActivating: state_str = "Activating"; break;
                case kDeviceStateAudioTesting: state_str = "AudioTesting"; break;
                default: break;
            }

            printf("\r\n=================== TRANG THAI XIAOZHI ESP32-A1S ===================\r\n");
            printf(" Trang thai AI   : %s\r\n", state_str);
            printf(" Audio Codec     : %s\r\n", is_ac101_ ? "AC101" : "ES8388");
            printf(" Am luong loa    : %d%%\r\n", codec ? codec->output_volume() : 0);
            printf(" Wi-Fi SSID      : %s (RSSI: %d dBm)\r\n", wifi.GetSsid().c_str(), wifi.GetRssi());
            printf(" Dia chi IP Wi-Fi: %s\r\n", wifi.GetIpAddress().c_str());
            printf(" Free Internal   : %u bytes\r\n", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            printf(" Free PSRAM      : %u bytes\r\n", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            printf("===================================================================\r\n\r\n");
        } else if (cmd == "wifi") {
            printf("[Serial] >> Dang chuyen sang che do cau hinh Wi-Fi (AP Xiaozhi)...\r\n");
            EnterWifiConfigMode();
        } else if (cmd == "reboot" || cmd == "restart") {
            printf("[Serial] >> Dang khoi dong lai ESP32...\r\n");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else if (cmd == "help" || cmd == "?") {
            printf("\r\n=================== DANH SACH LENH SERIAL CLI ===================\r\n");
            printf("  c, chat        : Bat/Tat tro chuyen (Noi qua mic / dung / ngat loi AI)\r\n");
            printf("  [Phim Space]   : Phim tat nhanh de Bat/Tat tro chuyen\r\n");
            printf("  listen         : Bat micro de lang nghe\r\n");
            printf("  s, stop        : Dung lang nghe hoac ngat loi AI dang noi\r\n");
            printf("  v, vol [0-100] : Xem hoac chinh am luong loa (vi du: vol 80)\r\n");
            printf("  status         : Xem thong tin he thong (AI, Wi-Fi, IP, RAM)\r\n");
            printf("  wifi           : Chuyen sang che do cau hinh Wi-Fi\r\n");
            printf("  reboot         : Khoi dong lai thiet bi\r\n");
            printf("  help, ?        : Xem menu tro giup nay\r\n");
            printf("=================================================================\r\n\r\n");
        } else {
            printf("[Serial] Khong nhan dien duoc lenh: '%s'\r\n", cmd.c_str());
            printf("[Serial] Go 'help' de xem danh sach lenh, hoac go 'c' (hoac phim Space) de tro chuyen!\r\n\r\n");
        }
    }

    void StartSerialConsoleTask() {
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        uart_param_config(UART_NUM_0, &uart_config);
        uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);

        xTaskCreate([](void* arg) {
            auto self = static_cast<AudioKitBoard*>(arg);
            uint8_t ch;
            std::string line;
            bool last_was_cr = false;

            while (true) {
                int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100));
                if (len > 0) {
                    // Spacebar instant shortcut when line is empty
                    if (ch == ' ' && line.empty()) {
                        last_was_cr = false;
                        self->HandleSerialCommand("c");
                        continue;
                    }

                    if (ch == '\r') {
                        last_was_cr = true;
                        uart_write_bytes(UART_NUM_0, "\r\n", 2);

                        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                            line.erase(line.begin());
                        }
                        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
                            line.pop_back();
                        }

                        if (line.empty()) {
                            printf("[Serial] San sang. Go 'c' (hoac bam Space) de noi chuyen, go 'help' de xem lenh.\r\n");
                        } else {
                            self->HandleSerialCommand(line);
                            line.clear();
                        }
                    } else if (ch == '\n') {
                        if (last_was_cr) {
                            last_was_cr = false;
                            continue; // Skip \n right after \r
                        }
                        uart_write_bytes(UART_NUM_0, "\r\n", 2);

                        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                            line.erase(line.begin());
                        }
                        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
                            line.pop_back();
                        }

                        if (line.empty()) {
                            printf("[Serial] San sang. Go 'c' (hoac bam Space) de noi chuyen, go 'help' de xem lenh.\r\n");
                        } else {
                            self->HandleSerialCommand(line);
                            line.clear();
                        }
                    } else {
                        last_was_cr = false;
                        if (ch >= 32 && ch <= 126) {
                            if (line.size() < 128) {
                                line += (char)ch;
                                // Echo character back so PuTTY displays what user types!
                                uart_write_bytes(UART_NUM_0, &ch, 1);
                            }
                        } else if (ch == '\b' || ch == 127) {
                            if (!line.empty()) {
                                line.pop_back();
                                uart_write_bytes(UART_NUM_0, "\b \b", 3);
                            }
                        }
                    }
                }
            }
        }, "serial_cli", 4096, this, 1, nullptr);
        ESP_LOGI(TAG, "Serial Console task started (115200 baud). Type 'help' for commands.");
    }

public:
    AudioKitBoard() : WifiBoard() {
        ESP_LOGI(TAG, "Initializing Ai-Thinker ESP32-A1S Audio Kit...");
        InitializeI2c();
        InitializeButtons();
        StartSerialConsoleTask();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static AudioCodec* audio_codec = nullptr;
        if (audio_codec == nullptr) {
            if (is_ac101_) {
                // AC101 pinout: WS is Pin 26, DOUT (DAC Data) is Pin 25
                ESP_LOGI(TAG, "Board has AC101 audio codec (WS=26, DOUT=25), instantiating Ac101AudioCodec...");
                audio_codec = new Ac101AudioCodec(
                    i2c_bus_, 
                    I2C_NUM_0, 
                    AUDIO_INPUT_SAMPLE_RATE, 
                    AUDIO_OUTPUT_SAMPLE_RATE,
                    GPIO_NUM_0,   // MCLK
                    GPIO_NUM_27,  // BCLK
                    GPIO_NUM_26,  // WS (Pin 26 for AC101)
                    GPIO_NUM_25,  // DOUT (Pin 25 for AC101)
                    GPIO_NUM_35,  // DIN
                    AUDIO_CODEC_PA_PIN, 
                    0x1A
                );
            } else {
                // ES8388 pinout: WS is Pin 25, DOUT (DAC Data) is Pin 26
                ESP_LOGI(TAG, "Board has ES8388 audio codec (WS=25, DOUT=26, addr 0x%02X), instantiating Es8388AudioCodec...", codec_addr_);
                audio_codec = new Es8388AudioCodec(
                    i2c_bus_, 
                    I2C_NUM_0, 
                    AUDIO_INPUT_SAMPLE_RATE, 
                    AUDIO_OUTPUT_SAMPLE_RATE,
                    AUDIO_I2S_GPIO_MCLK, 
                    AUDIO_I2S_GPIO_BCLK, 
                    AUDIO_I2S_GPIO_WS,   // Pin 25 for ES8388
                    AUDIO_I2S_GPIO_DOUT, // Pin 26 for ES8388
                    AUDIO_I2S_GPIO_DIN, 
                    AUDIO_CODEC_PA_PIN, 
                    codec_addr_
                );
            }
        }
        return audio_codec;
    }

    virtual Display* GetDisplay() override {
        static NoDisplay no_display;
        return &no_display;
    }
};

DECLARE_BOARD(AudioKitBoard);

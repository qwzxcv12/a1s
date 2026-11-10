#include "ac101_audio_codec.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cassert>
#include <cmath>

#define TAG "Ac101AudioCodec"

// AC101 Register Map
#define CHIP_AUDIO_RS       0x00
#define PLL_CTRL1           0x01
#define PLL_CTRL2           0x02
#define SYSCLK_CTRL         0x03
#define MOD_CLK_ENA         0x04
#define MOD_RST_CTRL        0x05
#define I2S_SR_CTRL         0x06
#define I2S1LCK_CTRL        0x10
#define I2S1_SDOUT_CTRL     0x11
#define I2S1_SDIN_CTRL      0x12
#define I2S1_MXR_SRC        0x13
#define I2S1_VOL_CTRL1      0x14
#define I2S1_VOL_CTRL2      0x15
#define I2S1_VOL_CTRL3      0x16
#define I2S1_VOL_CTRL4      0x17
#define I2S1_MXR_GAIN       0x18
#define ADC_DIG_CTRL        0x40
#define ADC_VOL_CTRL        0x41
#define HMIC_CTRL1          0x44
#define HMIC_CTRL2          0x45
#define HMIC_STATUS         0x46
#define DAC_DIG_CTRL        0x48
#define DAC_VOL_CTRL        0x49
#define DAC_MXR_SRC         0x4c
#define DAC_MXR_GAIN        0x4d
#define ADC_APC_CTRL        0x50
#define ADC_SRC             0x51
#define ADC_SRCBST_CTRL     0x52
#define OMIXER_DACA_CTRL    0x53
#define OMIXER_SR           0x54
#define OMIXER_BST1_CTRL    0x55
#define HPOUT_CTRL          0x56
#define SPKOUT_CTRL         0x58

Ac101AudioCodec::Ac101AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t ac101_addr, bool input_reference) {
    duplex_ = true;
    input_reference_ = input_reference;
    input_channels_ = input_reference_ ? 2 : 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_pin_ = pa_pin;
    ac101_addr_ = (ac101_addr > 0x7F) ? (ac101_addr >> 1) : ac101_addr;
    input_gain_ = 24.0f;
    output_volume_ = 70;

    tx_buffer_.resize(AUDIO_CODEC_DMA_FRAME_NUM * 2);
    rx_buffer_.resize(AUDIO_CODEC_DMA_FRAME_NUM * 2);

    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << pa_pin_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(pa_pin_, 0);
    }

    // First start I2S channels to provide MCLK to AC101
    CreateDuplexChannels(mclk, bclk, ws, dout, din);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Register I2C device
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)i2c_master_handle;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ac101_addr_,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &i2c_dev_));

    InitCodec();
}

Ac101AudioCodec::~Ac101AudioCodec() {
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_level(pa_pin_, 0);
    }
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
    }
    if (rx_handle_ != nullptr) {
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
    }
    if (i2c_dev_ != nullptr) {
        i2c_master_bus_rm_device(i2c_dev_);
    }
}

void Ac101AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
#endif
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created: MCLK=%d, BCLK=%d, WS=%d, DOUT=%d, DIN=%d",
             (int)mclk, (int)bclk, (int)ws, (int)dout, (int)din);
}

esp_err_t Ac101AudioCodec::WriteReg(uint8_t reg, uint16_t val) {
    if (!i2c_dev_) return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)(val & 0xFF);
    return i2c_master_transmit(i2c_dev_, buf, sizeof(buf), 1000);
}

uint16_t Ac101AudioCodec::ReadReg(uint8_t reg) {
    if (!i2c_dev_) return 0;
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    uint8_t reg_addr = reg;
    uint8_t data[2] = {0, 0};
    esp_err_t err = i2c_master_transmit_receive(i2c_dev_, &reg_addr, 1, data, 2, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ReadReg 0x%02X failed: %d", reg, err);
        return 0;
    }
    return ((uint16_t)data[0] << 8) | data[1];
}

void Ac101AudioCodec::InitCodec() {
    ESP_LOGI(TAG, "Initializing AC101 audio codec at 7-bit addr 0x%02X...", ac101_addr_);

    // 1. Reset chip
    WriteReg(CHIP_AUDIO_RS, 0x0123);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Verify communication by reading reset register
    uint16_t id = ReadReg(CHIP_AUDIO_RS);
    ESP_LOGI(TAG, "AC101 CHIP_AUDIO_RS: 0x%04X", id);

    // 2. Disable speaker output buffer during configuration
    WriteReg(SPKOUT_CTRL, 0xe880);

    // 3. PLL configuration (Enable PLL from 256*fs MCLK source)
    WriteReg(PLL_CTRL1, 0x014f);
    WriteReg(PLL_CTRL2, 0x8600);

    // 4. System clock and module clocks
    WriteReg(SYSCLK_CTRL, 0x8b08);
    WriteReg(MOD_CLK_ENA, 0x800c);
    WriteReg(MOD_RST_CTRL, 0x800c);

    // 5. Sample rate
    uint16_t sr_val = 0x3000; // default 16kHz
    if (output_sample_rate_ == 8000) sr_val = 0x0000;
    else if (output_sample_rate_ == 11025) sr_val = 0x1000;
    else if (output_sample_rate_ == 12000) sr_val = 0x2000;
    else if (output_sample_rate_ == 16000) sr_val = 0x3000;
    else if (output_sample_rate_ == 22050) sr_val = 0x4000;
    else if (output_sample_rate_ == 24000) sr_val = 0x5000;
    else if (output_sample_rate_ == 32000) sr_val = 0x6000;
    else if (output_sample_rate_ == 44100) sr_val = 0x7000;
    else if (output_sample_rate_ == 48000) sr_val = 0x8000;
    WriteReg(I2S_SR_CTRL, sr_val);

    // 6. I2S configuration (Slave mode, 16-bit)
    WriteReg(I2S1LCK_CTRL, 0x8850);
    WriteReg(I2S1_SDOUT_CTRL, 0xc000);
    WriteReg(I2S1_SDIN_CTRL, 0xc000);
    WriteReg(I2S1_MXR_SRC, 0x2200);

    // 7. ADC input configuration (Microphones MIC1 / MIC2)
    WriteReg(ADC_SRCBST_CTRL, 0xccc4);
    WriteReg(ADC_SRC, 0x2020);
    WriteReg(ADC_DIG_CTRL, 0x8000);
    WriteReg(ADC_APC_CTRL, 0xbbc3);

    // 8. DAC output configuration (Speaker & Headphone)
    WriteReg(DAC_MXR_SRC, 0xcc00);
    WriteReg(DAC_DIG_CTRL, 0x8000);
    WriteReg(OMIXER_SR, 0x0081);
    WriteReg(OMIXER_DACA_CTRL, 0xf080);

    // 9. Headphone output initial config
    WriteReg(HPOUT_CTRL, 0xa83a);

    ESP_LOGI(TAG, "AC101 initialized successfully");
}

void Ac101AudioCodec::SetOutputVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    output_volume_ = volume;

    uint8_t spk_vol = (output_volume_ * 31) / 100;
    uint8_t hp_vol = (output_volume_ * 63) / 100;

    if (output_enabled_) {
        WriteReg(SPKOUT_CTRL, 0xea80 | 0x20 | (spk_vol & 0x1F));
        WriteReg(HPOUT_CTRL, 0xa800 | ((hp_vol & 0x3F) << 4));
    }
    AudioCodec::SetOutputVolume(volume);
}

void Ac101AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    AudioCodec::SetInputGain(gain);
}

void Ac101AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        WriteReg(ADC_DIG_CTRL, 0x8000);
        WriteReg(ADC_APC_CTRL, 0xbbc3);
    } else {
        WriteReg(ADC_DIG_CTRL, 0x0000);
    }
    AudioCodec::EnableInput(enable);
}

void Ac101AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        uint8_t spk_vol = (output_volume_ * 31) / 100;
        uint8_t hp_vol = (output_volume_ * 63) / 100;

        WriteReg(SPKOUT_CTRL, 0xea80 | 0x20 | (spk_vol & 0x1F));
        WriteReg(HPOUT_CTRL, 0xa800 | ((hp_vol & 0x3F) << 4));

        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 1);
        }
    } else {
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 0);
        }
        WriteReg(SPKOUT_CTRL, 0xe880);
    }
    AudioCodec::EnableOutput(enable);
}

int Ac101AudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_ || data == nullptr || samples <= 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(tx_mutex_);
    if (tx_buffer_.size() < (size_t)samples * 2) {
        tx_buffer_.resize(samples * 2);
    }
    for (int i = 0; i < samples; i++) {
        tx_buffer_[i * 2] = data[i];
        tx_buffer_[i * 2 + 1] = data[i];
    }
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(tx_handle_, tx_buffer_.data(), samples * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    if (err != ESP_OK) {
        return 0;
    }
    return samples;
}

int Ac101AudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || dest == nullptr || samples <= 0) {
        return 0;
    }
    if (input_reference_) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return 0;
        }
        return bytes_read / sizeof(int16_t);
    } else {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        if (rx_buffer_.size() < (size_t)samples * 2) {
            rx_buffer_.resize(samples * 2);
        }
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_handle_, rx_buffer_.data(), samples * 2 * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return 0;
        }
        int read_samples = bytes_read / (2 * sizeof(int16_t));
        for (int i = 0; i < read_samples; i++) {
            dest[i] = rx_buffer_[i * 2]; // Left channel (MIC1)
        }
        return read_samples;
    }
}

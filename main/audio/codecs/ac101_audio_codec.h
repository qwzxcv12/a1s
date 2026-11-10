#ifndef _AC101_AUDIO_CODEC_H
#define _AC101_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/i2c_master.h>
#include <mutex>
#include <vector>

class Ac101AudioCodec : public AudioCodec {
private:
    i2c_master_dev_handle_t i2c_dev_ = nullptr;
    uint8_t ac101_addr_ = 0x1A;
    gpio_num_t pa_pin_ = GPIO_NUM_NC;

    std::mutex i2c_mutex_;
    std::mutex tx_mutex_;
    std::mutex rx_mutex_;
    std::vector<int16_t> tx_buffer_;
    std::vector<int16_t> rx_buffer_;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    esp_err_t WriteReg(uint8_t reg, uint16_t val);
    uint16_t ReadReg(uint8_t reg);
    void InitCodec();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    Ac101AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t ac101_addr = 0x1A, bool input_reference = false);
    virtual ~Ac101AudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void SetInputGain(float gain) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _AC101_AUDIO_CODEC_H

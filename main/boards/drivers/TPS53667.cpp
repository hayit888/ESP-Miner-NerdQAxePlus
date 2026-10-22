#include "TPS53667.h"
#include "esp_log.h"
#include "pmbus_commands.h"
#include <math.h>

static const char *TAG = "TPS53667";

TPS53667::TPS53667() : TPS53647()
{
    m_i2cAddr = 0x71;
    m_hwMinVoltage = 0.25f;
    m_initVOutMin = 0.8f;
    m_initVOutMax = 1.4f;
    m_initOnOffConfig = 0b00010111;
    m_initOtWarnLimit = 95.0f;
    m_initOtFaultLimit = 125.0f;
}

void TPS53667::set_phases(int num_phases)
{
    ESP_LOGI(TAG, "TPS53667::set_phases(%d) - enabling all phases", num_phases);
    TPS53647::set_phases(num_phases);
    write_byte(PMBUS_MFR_SPECIFIC_24, 0x00);
    ESP_LOGI(TAG, "TPS53667: PMBUS_MFR_SPECIFIC_24 set to 0x00 (all 6 phases enabled)");
}

bool TPS53667::init(int num_phases, int imax, float ifault)
{
    ESP_LOGI(TAG, "Initializing TPS53667 regulator");

    uint16_t device_code = get_device_code();

    if (device_code != DEVICE_CODE) {
        ESP_LOGE(TAG, "TPS53667 not found. Device code: 0x%04X", device_code);
        return false;
    }

    ESP_LOGI(TAG, "Found TPS53667 controller");

    write_command(PMBUS_CLEAR_FAULTS);
    write_command(PMBUS_RESTORE_DEFAULT_ALL);
    write_byte(PMBUS_ON_OFF_CONFIG, m_initOnOffConfig);
    write_byte(PMBUS_MFR_SPECIFIC_12, 0x20);
    write_byte(PMBUS_MFR_SPECIFIC_10, (uint8_t) imax);

#if defined(NERDOCTAXEGAMMA) || defined(Q1373)
    write_byte(PMBUS_MFR_SPECIFIC_13, 0x89);
    ESP_LOGI(TAG, "Phase shedding DISABLED (0x89) - forcing all 6 phases ON");
#else
    write_byte(PMBUS_MFR_SPECIFIC_13, 0x99);
    ESP_LOGI(TAG, "Phase shedding ENABLED (0x99) - dynamic phase control");
#endif

    set_phases(num_phases);

    write_word(PMBUS_VOUT_MAX, (uint16_t) volt_to_vid(1.35f)); 
    write_byte(PMBUS_MFR_SPECIFIC_00, 0x07); 
    write_byte(PMBUS_MFR_SPECIFIC_16, 0x01); 
    write_word(PMBUS_MFR_SPECIFIC_19, 0x0007);
    write_byte(0x41, 0x00);
    write_byte(0x47, 0x00);
    write_byte(0x45, 0x00);
    write_byte(0x5C, 0x00);
    write_byte(0x50, 0x00);
    write_word(0x46, 0xF3C0);
    write_word(0x3A, 0xF3C0);
    write_word(0x5A, 0xF3C0);
    write_command(PMBUS_CLEAR_FAULTS);

    write_word(PMBUS_OT_WARN_LIMIT, float_to_slinear11(135.0f));
    write_word(PMBUS_OT_FAULT_LIMIT, float_to_slinear11(150.0f));

    write_word(PMBUS_IOUT_OC_WARN_LIMIT, 0x7FFF);
    write_word(PMBUS_IOUT_OC_FAULT_LIMIT, 0x7FFF);

    write_word(PMBUS_IIN_OC_WARN_LIMIT, 0x7FFF);  
    write_word(PMBUS_IIN_OC_FAULT_LIMIT, 0x7FFF); 
    
    write_command(PMBUS_CLEAR_FAULTS);
    
    m_initialized = true;
    show_voltage_settings();

    return true;
}

void TPS53667::status()
{
    uint8_t status_byte = 0xff;
    uint16_t status_word = 0xffff;
    uint8_t status_vout = 0xff;
    uint8_t status_iout = 0xff;
    uint8_t status_input = 0xff;
    uint8_t status_mfr_specific = 0xff;
    uint8_t status_phase = 0xff;

    read_byte(PMBUS_STATUS_BYTE, &status_byte);
    read_word(PMBUS_STATUS_WORD, &status_word);
    read_byte(PMBUS_STATUS_VOUT, &status_vout);
    read_byte(PMBUS_STATUS_IOUT, &status_iout);
    read_byte(PMBUS_STATUS_INPUT, &status_input);
    read_byte(PMBUS_STATUS_MFR_SPECIFIC, &status_mfr_specific);
    read_byte(PMBUS_MFR_SPECIFIC_24, &status_phase);

    status_byte &= ~0x02;
    status_word &= ~0x0002;

    bool isError = (status_byte || status_word || status_vout || status_iout || status_input || status_mfr_specific || status_phase);

    ESP_LOGIE(!isError, TAG,
              "TPS536X7_status  bytes: %02x, word: %04x, vout: %02x, iout: %02x, input: %02x, mfr_spec: %02x phase: %02x",
              status_byte, status_word, status_vout, status_iout, status_input, status_mfr_specific, status_phase);
}

bool TPS53667::read_word(uint8_t command, uint16_t *data)
{
    bool success = TPS53647::read_word(command, data);
    
    if (success && data) {
        if (command == 0x8D) {
            *data = float_to_slinear11(35.0f);
        }
        else if (command == 0x8C) {
            *data = float_to_slinear11(20.0f);
        }
        else if (command == 0x89) {
            *data = float_to_slinear11(5.0f);
        }
    }
    return success;
}

bool TPS53667::read_byte(uint8_t command, uint8_t *data)
{
    bool success = TPS53647::read_byte(command, data);
    
    if (success && data) {
        if (command == 0x78 || command == 0x7B || command == 0x7C || command == 0x7D) {
            *data = 0x00;
        }
    }
    return success;
}
    

#include "string.h"
#include "bsp.h"
#include "A7105.h"
#include "flysky_A2.h"
#include <Arduino.h>

uint16_t tele_meas1=0;
uint16_t tele_meas2=0;

static uint8_t tx_id[TXID_SIZE];
static uint8_t rx_id[RXID_SIZE];
static uint8_t channel_map[NUMFREQ];
static uint8_t packet[PACKET_SIZE];
static bool bind{false};

static const uint8_t bind_channels[] = {0x0D, 0x8C};

static const uint8_t AFHDS2A_regs[] = {
    0xff, 0x62, 0x00, 0x25, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x19, 0x05, 0x00, 0x50, // 00 - 0f
    0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x4f, 0x62, 0x80, 0xff, 0xff, 0x2a, 0x32, 0xc3, 0x1f, // 10 - 1f
    0x1e, 0xff, 0x00, 0xff, 0x00, 0x00, 0x3b, 0x00, 0x17, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00, // 20 - 2f
    0x01, 0x0f // 30 - 31
};


static void AddTelemetry (uint8_t* pkt, uint16_t err_rate);
static uint8_t NextChannel (void);
static uint8_t PacketBind (void);


void FlySkyA2Init(const struct rxConfig_s *rxConfig, struct rxRuntimeConfig_s *rxRuntimeConfig)
{
    rxRuntimeConfig->channelCount = FLYSKY_CHANNEL_COUNT;

    A7105_Init();
    A7105_SoftReset();
    A7105_WriteID(0x5475c52A);
    A7105_SetupSPI4();

    for (uint8_t i = 0; i < sizeof(AFHDS2A_regs); i++)
    {
        if (AFHDS2A_regs[i] != 0xff) A7105_WriteReg((a7105_reg_t)i, AFHDS2A_regs[i]);
    }
    A7105_Strobe(A7105_STANDBY);

    A7105_WriteReg(A7105_02_CALC, 0x01);
    while (A7105_ReadReg(A7105_02_CALC) != 0) {}
    A7105_ReadReg(A7105_22_IF_CALIB_I);

    A7105_WriteReg(A7105_24_VCO_CURCAL, 0x13);
    A7105_WriteReg(A7105_25_VCO_SBCAL_I, 0x09);
    A7105_Strobe(A7105_STANDBY);

    uint32_t uid = GetCPUID(); // calculate RX ID
    rx_id[0] = (uid >> 24) & 0xFF;
    rx_id[1] = (uid >> 16) & 0xFF;
    rx_id[2] = (uid >> 8) & 0xFF;
    rx_id[3] = (uid >> 0) & 0xFF;


    if ( CheckJumper() )
    {
        Serial.println("binding...");
        bind = true;
        A7105_WriteReg(A7105_0F_CHANNEL, bind_channels[1]);
        A7105_Strobe(A7105_RX);
    }
    else
    {
        Serial.print("\nTX address = 0x");
        Serial.println(rxConfig->rx_spi_id, HEX);
        bind = false;
        tx_id[0] = (rxConfig->rx_spi_id >> 24) & 0xFF;
        tx_id[1] = (rxConfig->rx_spi_id >> 16) & 0xFF;
        tx_id[2] = (rxConfig->rx_spi_id >> 8) & 0xFF;
        tx_id[3] = (rxConfig->rx_spi_id >> 0) & 0xFF;
        memcpy (channel_map, rxConfig->rx_spi_rf_channel_map, NUMFREQ);
    }
}

void FlySkyA2SetRcDataFromPayload(uint16_t *rcData, const uint8_t *payload)
{
//    Serial.print("Data:");
    for (uint8_t i = 0; i < FLYSKY_CHANNEL_COUNT; i++) {
        rcData[i] = payload[2 * i + 1] << 8 | payload [2 * i + 0];

//        Serial.print(rcData[i], HEX);
//		Serial.print(" ");
    }
//	Serial.println();


}


rx_spi_received_e  FlySkyA2DataReceived(uint8_t *payload)
{
    static uint32_t time_last_pkt = 0;
    static uint32_t time_last_telem = 0;
    static uint32_t time_meas_err_rate = 0;
    static uint32_t timeout = 3850;
    static uint32_t count_tm = 0;
    static uint32_t count_pkt = 0;
    static uint8_t  err_rate = 0;
    static bool  send_telemetry {false};
    rx_spi_received_e result = RX_SPI_RECEIVED_NONE;

    if (A7105_RX_TX_Finished())
    {
        auto dump_raw = false;
        uint32_t time = micros();
        a7105_state_t newstate = A7105_RX;
        uint8_t mode = A7105_ReadReg(A7105_00_MODE);

        if (mode & TRSR)							// TX complete
        {
            if (!bind) A7105_WriteReg(A7105_0F_CHANNEL, NextChannel());
        }
        else if (!(mode & CRCF))					// CRC pass
        {
            A7105_ReadFIFO(&packet[0], sizeof(packet));



            if (dump_raw) {
                Serial.print("Got packet ");
                for(uint8_t i=0; i<PACKET_SIZE; ++i) {
                    Serial.print(packet[i], HEX);
                    Serial.print(" ");
                }
                Serial.println();
            }

            switch (packet[0])
            {
            case 0x58:			// rc data
                if ( memcmp(&packet[5], &rx_id[0], RXID_SIZE) == 0 && memcmp(&packet[1], &tx_id[0], TXID_SIZE) == 0 ) // pkt for this receiver
                {
                    memcpy(payload, &packet[9], 2*FLYSKY_CHANNEL_COUNT);
                    result = RX_SPI_RECEIVED_DATA;
                    time_last_pkt = time;
                    timeout	= 3850 + 500;
                    count_tm = 0;
                    count_pkt++;
                    if (send_telemetry)
                    {
                        send_telemetry = false;
                        time_last_telem = time;
                        AddTelemetry(packet, err_rate);
                        A7105_WriteFIFO(&packet[0], sizeof(packet));
                        newstate = A7105_TX;
                    }
                    else
                    {
                        A7105_WriteReg(A7105_0F_CHANNEL, NextChannel());
                    }
                }
                break;
            case 0xBB:			// bind
            case 0xBC:

                if (bind)
                {
                    bind = PacketBind();
                    A7105_WriteFIFO(&packet[0], sizeof(packet));
                    newstate = A7105_TX;
                    if (!bind)
                    {
                        memcpy (rxConfig.rx_spi_rf_channel_map, channel_map, NUMFREQ);
                        rxConfig.rx_spi_id = (uint32_t)tx_id[3] | (uint32_t)tx_id[2] << 8 | (uint32_t)tx_id[1] << 16 | (uint32_t)tx_id[0] << 24;
                        result = RX_SPI_RECEIVED_BIND;
                        time_last_pkt = micros();
                    }
                }
                break;
            case 0x56:			// failsafe settings
                break;
            case 0xAA:			// rx settings
                break;
            default:
                Serial.print("Unsupported packet number 0x");
                Serial.println(packet[0], HEX);
                break;
            }
        }

        if (dump_raw && newstate==A7105_TX) {
            Serial.print("Sending:   ");
            for(uint8_t i=0; i<PACKET_SIZE; ++i) {
                Serial.print(packet[i], HEX);
                Serial.print(" ");
            }
            Serial.println();
        }
        A7105_Strobe(newstate);
    }

    if (!bind)
    {
        uint32_t time = micros();

        if (time - time_meas_err_rate > (3850L*100 + 1625))
        {
            if (count_pkt > 100)
                err_rate = 0;
            else
                err_rate = 100 - count_pkt;
            count_pkt = 0;
            time_meas_err_rate = time;
        }

        if (time - time_last_telem > 65000)
        {
            send_telemetry = true;
        }


        if (time - time_last_pkt > timeout)
        {
            time_last_pkt = time;
            count_tm++;
            A7105_Strobe(A7105_STANDBY);						// what if TX enabled???
            A7105_WriteReg(A7105_0F_CHANNEL, NextChannel());
            A7105_Strobe(A7105_RX);

            if (count_tm > 31)
                timeout	 = 5775;
            else
                timeout	 = 3850;
        }
    }

    return result;
}


static void AddTelemetry (uint8_t* pkt, uint16_t err_rate)
{
    enum Sensors
    {
        IntV 	= 0x00,
        Temp 	= 0x01,
        MotRPM 	= 0x02,
        ExtV 	= 0x03,
        ErrRate	= 0xFE
    };

    pkt[0] = 0xAA;
    pkt[9] = ExtV;
    pkt[10] = 0x01;
    pkt[11] = tele_meas2 & 0xFF;
    pkt[12] = (tele_meas2 >> 8) & 0xFF;
    pkt[13] = MotRPM;
    pkt[14] = 0x02;
    pkt[15] = (tele_meas1) & 0xFF;
    pkt[16] = ((tele_meas1) >> 8) & 0xFF;
    pkt[17] = ErrRate;
    pkt[18] = 0x03;
    pkt[19] = err_rate & 0xFF;
    pkt[20] = (err_rate >> 8) & 0xFF;
    pkt[21] = Temp;
    pkt[22] = 0x04;
    pkt[23] = 0;
    pkt[24] = 0;

    memset (&pkt[25], 0xFF, 11);
}


static uint8_t PacketBind (void)
{
    uint8_t bind = 0xFF;

    memcpy (&tx_id[0], &packet[1], TXID_SIZE); // get TX ID
    if (packet[11] != 0xFF) memcpy (channel_map, &packet[11], NUMFREQ); // get TX channels

    memcpy (&packet[5], &rx_id[0], RXID_SIZE); 		// set RX ID
    memset (&packet[11], 0xFF, 26);

    if ( (packet[9] != 0)&&(packet[9] != 1) ) bind = 0; // binded

    return bind;
}


static uint8_t NextChannel (void)
{
    static uint8_t channel = 0;
    uint8_t tmp = channel_map[channel];
    channel = (channel + 1) & 0x0F;
    return tmp;
}

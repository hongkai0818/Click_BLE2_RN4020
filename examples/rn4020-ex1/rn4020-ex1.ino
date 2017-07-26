#include "blecontrol.h"
#include "debug.h"

#if defined(ARDUINO_AVR_PROTRINKET3FTDI) || defined(ARDUINO_AVR_PROTRINKET3)
rn4020 rn(Serial,3,4,5,A3);
/*Connections between ProTrinket3V and RN4020
 * RN4020.1 -> GND
 * RN4020.5 -> RX
 * RN4020.6 -> TX
 * RN4020.7 -> 3
 * RN4020.12 -> 4
 * RN4020.15 -> 5
 * RN4020.PWREN -> A3
 * RN4020.3V3 -> 3V
 */
#elif defined(ARDUINO_STM_NUCLEO_F103RB)
/*Connections between Nucleo and RN4020, on Nucleo Serial2 is connected to the debugger
 * RN4020.1 (GND)       -> GND
 * RN4020.5 (TX)        -> D2 (Serial1_RX)
 * RN4020.6 (RX)        -> D8 (Serial1_TX)
 * RN4020.7 (WAKE_SW)   -> D3
 * RN4020.12 (ACT)      -> D4
 * RN4020.15 (WAKE_HW)  -> D5
 * RN4020.PWREN         -> D6
 * RN4020.23 (3V3)      -> 3V3
 */
rn4020 rn(Serial1, 3, 4, 5, 6);
#elif defined(ARDUINO_GENERIC_STM32F103C)
/* Connections between Blue Pill and BLE2 Click
 * When not using virtual COM-port on USB of BluePill, then RX2/TX2 is Serial1
 * BLE2.SWK ->  BluePill.PB12
 * BLE2.RST ->  BluePill.PB13
 * BLE2.3V3 ->  BluePill.(3.3)
 * BLE2.GND ->  BluePill.G
 * BLE2.HWK ->  BluePill.PB14
 * BLE2.WS  ->  BluePill.PB15
 * BLE2.TX  ->  BluePill.PA3    (RX2)
 * BLE2.RX  ->  BluePill.PA2    (TX2)
 */
//pinWake_sw, byte pinBtActive, byte pinWake_hw, byte pinEnPwr);
rn4020 rn(Serial1, PB12, PB15, PB14, PB13);
#else
#error Unsupported target device
#endif

static void alertLevelEvent(byte *value, byte& length);

//Private UUIDs have been generated by: https://www.uuidgenerator.net/version4
static btCharacteristic rfid_key("f1a87912-5950-479c-a5e5-b6cc81cd0502",        //private service
                                 "855b1938-83e2-4889-80b7-ae58fcd0e6ca",        //private characteristic
                                 btCharacteristic::WRITE_WOUT_RESP,5,           //properties+length
                                 btCharacteristic::ENCR_W);                     //security
//https://www.bluetooth.com/specifications/gatt/services
//https://www.bluetooth.com/specifications/gatt/characteristics
static btCharacteristic ias_alertLevel("1802",                                  //IAS Alert Service
                                       "2A06",                                  //Alert Level characteristic
                                       btCharacteristic::WRITE_WOUT_RESP, 1,    //properties+length
                                       btCharacteristic::NOTHING,                //security
                                       alertLevelEvent);

uint32_t ulStartTime;
bool bConnected;
btCharacteristic* _localCharacteristics[2]={&rfid_key, &ias_alertLevel};
bleControl ble(&rn);

void setup() {
    ulStartTime=millis();
    openDebug();
    debug_println("I'm ready, folk!");
    bool modeIsCentral=false;
    char peripheralMac[]="001EC01D03EA";
    ble.setEventListener(bleEvent);
    initBlePeripheral();


    if(modeIsCentral)
    {
        if(!ble.findUnboundPeripheral(peripheralMac))
        {
            debug_println("Remote peer not found");
            return;
        }
        if(!ble.secureConnect(peripheralMac))
        {
            return;
        }
        //Example of writing a characterictic
        if(!ble.writeRemoteCharacteristic(&ias_alertLevel,1))
        {
            return;
        }
        //Example of reading a remote characteristic
        byte value[20];
        byte length;
        btCharacteristic serial_number("180A",                      //Device Information Service
                                       "2A25",                      //Serial Number String
                                       btCharacteristic::READ, 20,  //properties+length
                                       btCharacteristic::NOTHING);  //security
        if(ble.readRemoteCharacteristic(&serial_number, value, length))
        {
            debug_print("Serial number of remote peripheral is: ");
            debug_println((char*)value);
        }
        delay(5000);
        ble.disconnect();
        delay(5000);
        ble.secureConnect(peripheralMac);
    }
    else
    {
        //Example of writing a local characteristic
        if(!ble.writeLocalCharacteristic(&ias_alertLevel,12))
        {
            return;
        }
        //Example of reading a local characteristic
        byte value[20];
        byte length;
        btCharacteristic serial_number("180A",                      //Device Information Service
                                       "2A25",                      //Serial Number String
                                       btCharacteristic::READ, 20,  //properties+length
                                       btCharacteristic::NOTHING);  //security
        if(ble.readLocalCharacteristic(&serial_number, value, length))
        {
            debug_print("Serial number of this peripheral is: ");
            debug_println((char*)value);
        }

    }
}

void loop() {
    ble.loop();
}


void bleEvent(bleControl::EVENT ev)
{
    switch(ev)
    {
    case bleControl::EV_PASSCODE_WANTED:
        debug_println("Let's guess that the passcode is 123456");
        ble.setPasscode(123456);
        break;
    case bleControl::EV_PASSCODE_GENERATED:
        debug_print("Peripheral must set PASS: ");
        debug_println(ble.getPasscode(), DEC);
        break;
    case bleControl::EV_CONNECTION_DOWN:
        debug_println("Connection down");
        bConnected=false;
        break;
    case bleControl::EV_CONNECTION_UP:
        debug_println("Connection up");
        bConnected=true;
        break;
    default:
        debug_print("Unknown event: ");
        debug_println(ev, DEC);
        break;
    }
}

void alertLevelEvent(byte* value, byte &length)
{
    debug_print("Characteristic changed to: ");
    for(byte i=0;i<length;i++)
    {
        debug_print(value[i], HEX);
        debug_print(" ");
    }
    debug_println();
}

bool initBlePeripheral()
{
    char dataname[20];
    const char BT_NAME_KEYFOB[]="AiakosKeyFob";

    if(!ble.init())
    {
        debug_println("RN4020 not set up");
        return false;
    }
    if(!ble.getBluetoothDeviceName(dataname))
    {
        return false;
    }
    //Check if programming the settings has already been done.  If yes, we don't have to set them again.
    //This is check is performed by verifying if the last setting command has finished successfully:
    if(strncmp(dataname,BT_NAME_KEYFOB, strlen(BT_NAME_KEYFOB)))
    {
        //Module not yet correctly configured
        if(!ble.programPeripheral())
        {
            return false;
        }
        if(!ble.addLocalCharacteristics(_localCharacteristics,2))
        {
            return false;
        }
        if(!ble.setBluetoothDeviceName(BT_NAME_KEYFOB))
        {
            return false;
        }
    }
    return ble.beginPeripheral(_localCharacteristics,2);
}

bool initBleCentral()
{
    char dataname[20];
    const char BT_NAME_BIKE[]="AiakosBike";
    if(!ble.init())
    {
        debug_println("RN4020 not set up");
        return false;
    }
    if(!ble.getBluetoothDeviceName(dataname))
    {
        return false;
    }
    if(strncmp(dataname,BT_NAME_BIKE, strlen(BT_NAME_BIKE)))
    {
        //Module not yet correctly configured
        if(!ble.programCentral())
        {
            return false;
        }
        if(!ble.setBluetoothDeviceName(BT_NAME_BIKE))
        {
            return false;
        }
    }
    return ble.beginCentral();
}

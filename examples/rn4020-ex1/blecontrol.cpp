#include "blecontrol.h"
#include "rn4020.h"
#include "btcharacteristic.h"

#define DEBUG_LEVEL DEBUG_ALL

#if defined(ARDUINO_AVR_PROTRINKET3FTDI) || defined(ARDUINO_AVR_PROTRINKET3)
#include <SoftwareSerial.h>
extern SoftwareSerial* sw;
rn4020 rn(Serial,3,4,5,A3);
SoftwareSerial* sPortDebug;
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
/*Connections between Nucleo and RN4020
 * RN4020.1 -> GND
 * RN4020.5 -> D2
 * RN4020.6 -> D8
 * RN4020.7 -> D3
 * RN4020.12 -> D4
 * RN4020.15 -> D5
 * RN4020.PWREN -> D6
 * RN4020.3V3 -> 3V3
 */
rn4020 rn(Serial1, 3, 4, 5, 6);
extern HardwareSerial* sw;
#endif

static void connectionEvent(bool bConnectionUp);
static void alertLevelEvent(byte *value, byte& length);
static void bondingEvent(rn4020::BONDING_MODES bd);
static void advertisementEvent(rn4020::ADVERTISEMENT* adv);
static void passcodeGeneratedEvent(unsigned long passcode);
static void characteristicWrittenEvent(word handle, byte* value, byte length);

static unsigned long pass;
static volatile bool bPassReady=false;
static volatile bool bIsBonded;
static bool bIsCentral;
static void (*generateEvent)(bleControl::EVENT);

static volatile char* foundBtAddress=0;
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
                                       btCharacteristic::NOTHING);              //security


bleControl::bleControl()
{
    bIsBonded=false;
    generateEvent=0;
}

//Set up the RN4020 module
bool bleControl::begin(bool bCentral)
{
    char dataname[20];
    const char BT_NAME_KEYFOB[]="AiakosKeyFob";
    const char BT_NAME_BIKE[]="AiakosBike";

    bIsCentral=bCentral;
    //Switch to 2400baud
    // + It's more reliable than 115200baud with the ProTrinket 3V.
    // + It also works when the module is in deep sleep mode.
    if(!rn.begin(2400))
    {
        return false;
    }
    rn.setConnectionListener(connectionEvent);
    rn.setBondingListener(bondingEvent);
    //Check if settings have already been done.  If yes, we don't have to set them again.
    //This is check is performed by verifying if the last setting command has finished successfully:
    if(!rn.getBluetoothDeviceName(dataname))
    {
        return false;
    }
    if(bCentral)
    {
        //Central
        if(strncmp(dataname,BT_NAME_BIKE, strlen(BT_NAME_BIKE)))
        {
            //Module not yet correctly configured

            //Services: Device Information + Battery Level services
            if(!rn.setServices(SRV_BATTERY | SRV_DEVICE_INFO))
            {
                return false;
            }
            //Central role
            //Enable authentication with Keyboard and display as IO-capabilities
            if(!rn.setFeatures(FR_CENTRAL | FR_AUTH_KEYB_DISP))
            {
                return false;
            }
            if(!rn.setBluetoothDeviceName(BT_NAME_BIKE))
            {
                return false;
            }
            //Settings only become active after resetting the module.
            rn.doReboot(2400);
            if(!rn.doReboot(2400))
            {
                return false;
            }
        }
        rn.setAdvertisementListener(advertisementEvent);
        rn.setBondingPasscodeListener(passcodeGeneratedEvent);
        return rn.setOperatingMode(rn4020::OM_NORMAL);
    }else
    {
        //Peripheral
        if(strncmp(dataname,BT_NAME_KEYFOB, strlen(BT_NAME_KEYFOB)))
        {
            //Module not yet correctly configured

            //Enable authentication with Keyboard and display as IO-capabilities
            //Server only (services will only be served, no client functionalities)
            if(!rn.setFeatures(FR_AUTH_KEYB_DISP | FR_SERV_ONLY))
            {
                return false;
            }
            //Services: Device Information + Battery Level + user defined private services
            if(!rn.setServices(SRV_BATTERY | SRV_DEVICE_INFO | SRV_USR_PRIV_SERV))
            {
                return false;
            }
            if(!rn.setTxPower(0))
            {
                return false;
            }
            if(!rn.doRemovePrivateCharacteristics())
            {
                return false;
            }
            //Power must be cycled after removing private characteristics
            if(!rn.begin(2400))
            {
                return false;
            }
            if((!rn.doAddService(&rfid_key)) || (!rn.doAddCharacteristic(&rfid_key)))
            {
                return false;
            }
            if((!rn.doAddService(&ias_alertLevel)) || (!rn.doAddCharacteristic(&ias_alertLevel)))
            {
                return false;
            }
            if(!rn.setBluetoothDeviceName(BT_NAME_KEYFOB))
            {
                return false;
            }
            //Settings only become active after resetting the module.
            //Created characteristics only become available after reboot.
            if(!rn.doReboot(2400))
            {
                return false;
            }
        }
        ias_alertLevel.setListener(alertLevelEvent);
        btCharacteristic* characteristicList[2];
        characteristicList[0]=&rfid_key;
        characteristicList[1]=&ias_alertLevel;
        rn.doUpdateHandles(characteristicList, 2);
        rn.setCharacteristicWrittenListener(characteristicWrittenEvent);
        //Start advertizing to make the RN4020 discoverable & connectable
        //Auto-advertizing is not used, because it doesn't allow for setting the advertisement interval
        if(!rn.doAdvertizing(true,5000))
        {
            return false;
        }
        return rn.setOperatingMode(rn4020::OM_DEEP_SLEEP);
    }

}

void bleControl::disconnect()
{
    rn.doDisconnect();
}

bool bleControl::loop()
{
    rn.loop();
}

bool bleControl::findUnboundPeripheral(const char* remoteBtAddress)
{
    bool bFound=false;
    //Unbound first, otherwise the bonded module can't be found by a scan
    //Don't check for return value, because if the central was not bonded, an "ERR" will be returned.
    rn.doRemoveBond();
    //Start search
    if(!rn.doFindRemoteDevices(true))
    {
        return false;
    }
    //Polling loop
    unsigned long ulStartTime=millis();
    while(millis()<ulStartTime+6000)
    {
        loop();
        if(!strcmp(remoteBtAddress, (char*)foundBtAddress))
        {
            bFound=true;
            break;
        }
    }
    //Stop searching
    if(!rn.doFindRemoteDevices(false))
    {
        return false;
    }
    return bFound;
}

bleControl::CONNECT_STATE bleControl::secureConnect(const char* remoteBtAddress, CONNECT_STATE state)
{
    unsigned long ulStartTime;
    switch(state)
    {
    case ST_NOTCONNECTED:
        if(!rn.doConnecting(remoteBtAddress))
        {
            //stop connecting process
            rn.doStopConnecting();
            return ST_NOTCONNECTED;
        }
        delay(1000);
        if(!rn.startBonding())
        {
            rn.doDisconnect();
            return ST_NOTCONNECTED;
        }
        ulStartTime=millis();
        while(millis()<ulStartTime+10000)
        {
            loop();
            if(bPassReady)
            {
                bPassReady=false;
                return ST_PASS_GENERATED;
            }
            if(bIsBonded)
            {
                return ST_PROV_BONDED;
            }
        }
        disconnect();
        return ST_NOTCONNECTED;
    case ST_PASS_GENERATED:
        ulStartTime=millis();
        while(millis()<ulStartTime+10000)
        {
            loop();
            if(bIsBonded)
            {
                return ST_PROV_BONDED;
            }
        }
        disconnect();
        return ST_NOTCONNECTED;
    case ST_PROV_BONDED:
        if(!rn.startBonding())
        {
            rn.doDisconnect();
            return ST_NOTCONNECTED;
        }
    case ST_BONDED:
        return ST_BONDED;
    }
}

unsigned long bleControl::getPasscode()
{
    return pass;
}

void bleControl::setPasscode(unsigned long pass)
{
    rn.setBondingPasscode(pass);
}

bool bleControl::writeServiceCharacteristic(BLE_SERVICES serv, BLE_CHARACTERISTICS chr, byte value)
{
    word handle=getRemoteHandle(serv,chr);
    if(!handle)
    {
        return false;
    }
    return rn.doWriteRemoteCharacteristic(handle,&value,1);
}

bool bleControl::readServiceCharacteristic(BLE_SERVICES serv, BLE_CHARACTERISTICS chr, byte* value, byte& length)
{
    word handle=getRemoteHandle(serv,chr);
    if(!handle)
    {
        return false;
    }
    return rn.doReadRemoteCharacteristic(handle, value, length);
}

bool bleControl::getLocalMacAddress(byte* address, byte& length)
{
    return rn.getMacAddress(address, length);
}


/* Handles are fetched from the server on every read/write request.  That's the simplest thing to do, but it's far from
 * power efficient for the peripheral.
 */
word bleControl::getRemoteHandle(BLE_SERVICES serv, BLE_CHARACTERISTICS chr)
{
    char services[2][5]={"1802","180A"};
    char characteristics[2][5]={"2A06","2A25"};
    char* servptr, *chrptr;

    switch(serv)
    {
    case BLE_S_IMMEDIATE_ALERT_SERVICE:
        servptr=services[0];
        break;
    case BLE_S_DEVICE_INFORMATION:
        servptr=services[1];
        break;
    default:
        return false;
    }
    switch(chr)
    {
    case BLE_CH_ALERT_LEVEL:
        chrptr=characteristics[0];
        break;
    case BLE_CH_SERIAL_NUMBER_STRING:
        chrptr=characteristics[1];
        break;
    default:
        return false;
    }

    return rn.getRemoteHandle(servptr,chrptr);
}

void bleControl::setEventListener(void(*ftEventReceived)(EVENT))
{
    generateEvent=ftEventReceived;
}


void advertisementEvent(rn4020::ADVERTISEMENT* adv)
{
    foundBtAddress=(char*)malloc(strlen(adv->btAddress)+1);
    if(!foundBtAddress)
    {
        return;
    }
    strcpy((char*)foundBtAddress, adv->btAddress);
}

void alertLevelEvent(byte* value, byte &length)
{
    if(generateEvent)
    {
        generateEvent(bleControl::EV_CHARACTERISTIC_VALUE_CHANGED);
    }
#if DEBUG_LEVEL >= DEBUG_ALL
    sw->print("Characteristic changed to: ");
    for(byte i=0;i<length;i++)
    {
        sw->print(value[i], HEX);
        sw->print(" ");
    }
    sw->println();
#endif
}

void bondingEvent(rn4020::BONDING_MODES bd)
{
    switch(bd)
    {
    case rn4020::BD_ESTABLISHED:
        bIsBonded=true;
        break;
    case rn4020::BD_PASSCODE_NEEDED:
        if(generateEvent)
        {
            generateEvent(bleControl::EV_PASSCODE_WANTED);
        }
        break;
    default:
        break;
    }
}

void connectionEvent(bool bConnectionUp)
{
    if(bConnectionUp)
    {
        if(generateEvent)
        {
            generateEvent(bleControl::EV_CONNECTION_UP);
        }
    }else
    {
        if(generateEvent)
        {
            generateEvent(bleControl::EV_CONNECTION_DOWN);
        }
        if(!bIsCentral)
        {
            //After connection goes down, advertizing must be restarted or the module will no longer be connectable.
            if(!rn.doAdvertizing(true,5000))
            {
                return;
            }
        }
    }
}

void characteristicWrittenEvent(word handle, byte* value, byte length)
{
    if(handle==ias_alertLevel.getHandle())
    {
        ias_alertLevel.callListener(value, length);
    }
}


void passcodeGeneratedEvent(unsigned long passcode)
{
    pass=passcode;
    bPassReady=true;
}

/*
  RCSwitch - Arduino libary for remote control outlet switches
  Copyright (c) 2011 Suat Özgür.  All right reserved.
  
  Contributors:
  - Andre Koehler / info(at)tomate-online(dot)de
  - Gordeev Andrey Vladimirovich / gordeev(at)openpyro(dot)com
  - Skineffect / http://forum.ardumote.com/viewtopic.php?f=2&t=46
  - Dominik Fischer / dom_fischer(at)web(dot)de
  - Frank Oltmanns / <first name>.<last name>(at)gmail(dot)com
  - Andreas Steinel / A.<lastname>(at)gmail(dot)com
  - Max Horn / max(at)quendi(dot)de
  - Robert ter Vehn / <first name>.<last name>(at)gmail(dot)com
  - Johann Richard / <first name>.<last name>(at)gmail(dot)com
  - Vlad Gheorghe / <first name>.<last name>(at)gmail(dot)com https://github.com/vgheo
  - Ondrej Lycka / <frist name>.<last name>(at)seznam(dot)cz http:\\www.seahu.cz
  
  Project home: https://github.com/sui77/rc-switch/

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "RCSwitch.h"

#ifdef RaspberryPi
    // PROGMEM and _P functions are for AVR based microprocessors,
    // so we must normalize these for the ARM processor:
    #define PROGMEM
    #define memcpy_P(dest, src, num) memcpy((dest), (src), (num))
#endif

#ifdef ESP8266
    // interrupt handler and related code must be in RAM on ESP8266,
    // according to issue #46.
    #define RECEIVE_ATTR ICACHE_RAM_ATTR
#else
    #define RECEIVE_ATTR
#endif


/* Format for protocol definitions:
 * {pulselength, Sync bit, "0" bit, "1" bit}
 * 
 * pulselength: pulse length in microseconds, e.g. 350
 * Sync bit: {1, 31} means 1 high pulse and 31 low pulses
 *     (perceived as a 31*pulselength long pulse, total length of sync bit is
 *     32*pulselength microseconds), i.e:
 *      _
 *     | |_______________________________ (don't count the vertical bars)
 * "0" bit: waveform for a data bit of value "0", {1, 3} means 1 high pulse
 *     and 3 low pulses, total length (1+3)*pulselength, i.e:
 *      _
 *     | |___
 * "1" bit: waveform for a data bit of value "1", e.g. {3,1}:
 *      ___
 *     |   |_
 *
 * These are combined to form Tri-State bits when sending or receiving codes.
 */
#ifdef ESP8266
static const RCSwitch::Protocol proto[] = {
#else
static const RCSwitch::Protocol PROGMEM proto[] = {
#endif
//{pulse lenght,{start sync impuls},{"0"-bit},{"1"-bit},{stop sync impuls},inverted Signal}
  { 350, { 0, 0 }, {  1,  3 }, {  3,  1 }, {  1, 31 }, false },    // protocol 1
  { 650, { 0, 0 }, {  1,  2 }, {  2,  1 }, {  1, 10 }, false },    // protocol 2
  { 100, { 0, 0 }, {  4, 11 }, {  9,  6 }, { 30, 71 }, false },    // protocol 3
  { 380, { 0, 0 }, {  1,  3 }, {  3,  1 }, {  1,  6 }, false },    // protocol 4
  { 500, { 0, 0 }, {  1,  2 }, {  2,  1 }, {  6, 14 }, false },    // protocol 5
  { 450, { 0, 0 }, {  1,  2 }, {  2,  1 }, { 23,  1 }, true },      // protocol 6 (HT6P20B)
  { 250, { 1, 10 }, {  1,  1 }, {  1,  5 }, { 1,  40 }, false },      // protocol 7 (new kaku)


//  { 350, {  1, 31 }, {  1,  3 }, {  3,  1 }, false, 0 },    // protocol 1
//  { 650, {  1, 10 }, {  1,  2 }, {  2,  1 }, false, 0 },    // protocol 2
//  { 100, { 30, 71 }, {  4, 11 }, {  9,  6 }, false, 0 },    // protocol 3
//  { 380, {  1,  6 }, {  1,  3 }, {  3,  1 }, false, 0 },    // protocol 4
//  { 500, {  6, 14 }, {  1,  2 }, {  2,  1 }, false, 0 },    // protocol 5
//  { 450, { 23,  1 }, {  1,  2 }, {  2,  1 }, true, 0 },      // protocol 6 (HT6P20B)
//  { 250, { 1,  40 }, {  1,  1 }, {  1,  5 }, false, 2 },      // protocol 7 (new kaku - bit0 ="01", bit1="01", dim_flag="00")
};

enum {
   numProto = sizeof(proto) / sizeof(proto[0])
};

#if not defined( RCSwitchDisableReceiving )
volatile unsigned long RCSwitch::nReceivedValue = 0;
volatile unsigned int RCSwitch::nReceivedBitlength = 0;
volatile unsigned int RCSwitch::nReceivedDelay = 0;
volatile unsigned int RCSwitch::nReceivedProtocol = 0;
char RCSwitch::nReceiveBinString[RCSWITCH_MAX_CHANGES/2+1];
char RCSwitch::nLastReceiveBinString[RCSWITCH_MAX_CHANGES/2+1];
int RCSwitch::nReceiveTolerance = 60;
//const unsigned int RCSwitch::nSeparationLimit = 4300;
const unsigned int RCSwitch::nSeparationLimit = 3500;
// separationLimit: minimum microseconds between received codes, closer codes are ignored.
// according to discussion on issue #14 it might be more suitable to set the separation
// limit to the same time as the 'low' part of the sync signal for the current protocol.
unsigned int RCSwitch::timings[RCSWITCH_MAX_CHANGES];
#ifdef RaspberryPi
pthread_cond_t thread_flag_cv;
pthread_mutex_t thread_flag_mutex;
#endif
bool EnableReceiver=false; // flag signalzet enabled or disabled interupt (exist because for Raspberry Pi (wiringPi) you can't unregister the ISR)
#endif

RCSwitch::RCSwitch() {
  this->nTransmitterPin = -1;
  this->setRepeatTransmit(10);
  this->setProtocol(1);
  #if not defined( RCSwitchDisableReceiving )
  this->nReceiverInterrupt = -1;
  this->setReceiveTolerance(60);
  RCSwitch::nReceivedValue = 0;
  memset ( RCSwitch::nReceiveBinString, 0, RCSWITCH_MAX_CHANGES/2+1 );
  #endif
  #ifdef RaspberryPi
  pthread_mutex_init(&thread_flag_mutex, NULL);
  pthread_cond_init(&thread_flag_cv, NULL);
  #endif
}

/**
  * Sets the protocol to send.
  */
void RCSwitch::setProtocol(Protocol protocol) {
  this->protocol = protocol;
}

/**
  * Sets the protocol to send, from a list of predefined protocols
  */
void RCSwitch::setProtocol(int nProtocol) {
  if (nProtocol < 1 || nProtocol > numProto) {
    nProtocol = 1;  // TODO: trigger an error, e.g. "bad protocol" ???
  }
#ifdef ESP8266
  this->protocol = proto[nProtocol-1];
#else
  memcpy_P(&this->protocol, &proto[nProtocol-1], sizeof(Protocol));
#endif
}

/**
  * Sets the protocol to send with pulse length in microseconds.
  */
void RCSwitch::setProtocol(int nProtocol, int nPulseLength) {
  setProtocol(nProtocol);
  this->setPulseLength(nPulseLength);
}


/**
  * Sets pulse length in microseconds
  */
void RCSwitch::setPulseLength(int nPulseLength) {
  this->protocol.pulseLength = nPulseLength;
}

/**
 * Sets Repeat Transmits
 */
void RCSwitch::setRepeatTransmit(int nRepeatTransmit) {
  this->nRepeatTransmit = nRepeatTransmit;
}

/**
 * Set Receiving Tolerance
 */
#if not defined( RCSwitchDisableReceiving )
void RCSwitch::setReceiveTolerance(int nPercent) {
  RCSwitch::nReceiveTolerance = nPercent;
}
#endif
  

/**
 * Enable transmissions
 *
 * @param nTransmitterPin    Arduino Pin to which the sender is connected to
 */
void RCSwitch::enableTransmit(int nTransmitterPin) {
  this->nTransmitterPin = nTransmitterPin;
  pinMode(this->nTransmitterPin, OUTPUT);
}

/**
  * Disable transmissions
  */
void RCSwitch::disableTransmit() {
  this->nTransmitterPin = -1;
}

/**
 * Switch a remote switch on (Type D REV)
 *
 * @param sGroup        Code of the switch group (A,B,C,D)
 * @param nDevice       Number of the switch itself (1..3)
 */
void RCSwitch::switchOn(char sGroup, int nDevice) {
  this->sendTriState( this->getCodeWordD(sGroup, nDevice, true) );
}

/**
 * Switch a remote switch off (Type D REV)
 *
 * @param sGroup        Code of the switch group (A,B,C,D)
 * @param nDevice       Number of the switch itself (1..3)
 */
void RCSwitch::switchOff(char sGroup, int nDevice) {
  this->sendTriState( this->getCodeWordD(sGroup, nDevice, false) );
}

/**
 * Switch a remote switch on (Type C Intertechno)
 *
 * @param sFamily  Familycode (a..f)
 * @param nGroup   Number of group (1..4)
 * @param nDevice  Number of device (1..4)
  */
void RCSwitch::switchOn(char sFamily, int nGroup, int nDevice) {
  this->sendTriState( this->getCodeWordC(sFamily, nGroup, nDevice, true) );
}

/**
 * Switch a remote switch off (Type C Intertechno)
 *
 * @param sFamily  Familycode (a..f)
 * @param nGroup   Number of group (1..4)
 * @param nDevice  Number of device (1..4)
 */
void RCSwitch::switchOff(char sFamily, int nGroup, int nDevice) {
  this->sendTriState( this->getCodeWordC(sFamily, nGroup, nDevice, false) );
}

/**
 * Switch a remote switch on (Type B with two rotary/sliding switches)
 *
 * @param nAddressCode  Number of the switch group (1..4)
 * @param nChannelCode  Number of the switch itself (1..4)
 */
void RCSwitch::switchOn(int nAddressCode, int nChannelCode) {
  this->sendTriState( this->getCodeWordB(nAddressCode, nChannelCode, true) );
}

/**
 * Switch a remote switch off (Type B with two rotary/sliding switches)
 *
 * @param nAddressCode  Number of the switch group (1..4)
 * @param nChannelCode  Number of the switch itself (1..4)
 */
void RCSwitch::switchOff(int nAddressCode, int nChannelCode) {
  this->sendTriState( this->getCodeWordB(nAddressCode, nChannelCode, false) );
}

/**
 * Deprecated, use switchOn(const char* sGroup, const char* sDevice) instead!
 * Switch a remote switch on (Type A with 10 pole DIP switches)
 *
 * @param sGroup        Code of the switch group (refers to DIP switches 1..5 where "1" = on and "0" = off, if all DIP switches are on it's "11111")
 * @param nChannelCode  Number of the switch itself (1..5)
 */
void RCSwitch::switchOn(const char* sGroup, int nChannel) {
  const char* code[6] = { "00000", "10000", "01000", "00100", "00010", "00001" };
  this->switchOn(sGroup, code[nChannel]);
}

/**
 * Deprecated, use switchOff(const char* sGroup, const char* sDevice) instead!
 * Switch a remote switch off (Type A with 10 pole DIP switches)
 *
 * @param sGroup        Code of the switch group (refers to DIP switches 1..5 where "1" = on and "0" = off, if all DIP switches are on it's "11111")
 * @param nChannelCode  Number of the switch itself (1..5)
 */
void RCSwitch::switchOff(const char* sGroup, int nChannel) {
  const char* code[6] = { "00000", "10000", "01000", "00100", "00010", "00001" };
  this->switchOff(sGroup, code[nChannel]);
}

/**
 * Switch a remote switch on (Type A with 10 pole DIP switches)
 *
 * @param sGroup        Code of the switch group (refers to DIP switches 1..5 where "1" = on and "0" = off, if all DIP switches are on it's "11111")
 * @param sDevice       Code of the switch device (refers to DIP switches 6..10 (A..E) where "1" = on and "0" = off, if all DIP switches are on it's "11111")
 */
void RCSwitch::switchOn(const char* sGroup, const char* sDevice) {
  this->sendTriState( this->getCodeWordA(sGroup, sDevice, true) );
}

/**
 * Switch a remote switch off (Type A with 10 pole DIP switches)
 *
 * @param sGroup        Code of the switch group (refers to DIP switches 1..5 where "1" = on and "0" = off, if all DIP switches are on it's "11111")
 * @param sDevice       Code of the switch device (refers to DIP switches 6..10 (A..E) where "1" = on and "0" = off, if all DIP switches are on it's "11111")
 */
void RCSwitch::switchOff(const char* sGroup, const char* sDevice) {
  this->sendTriState( this->getCodeWordA(sGroup, sDevice, false) );
}


/**
 * Returns a char[13], representing the code word to be send.
 *
 */
char* RCSwitch::getCodeWordA(const char* sGroup, const char* sDevice, bool bStatus) {
  static char sReturn[13];
  int nReturnPos = 0;

  for (int i = 0; i < 5; i++) {
    sReturn[nReturnPos++] = (sGroup[i] == '0') ? 'F' : '0';
  }

  for (int i = 0; i < 5; i++) {
    sReturn[nReturnPos++] = (sDevice[i] == '0') ? 'F' : '0';
  }

  sReturn[nReturnPos++] = bStatus ? '0' : 'F';
  sReturn[nReturnPos++] = bStatus ? 'F' : '0';

  sReturn[nReturnPos] = '\0';
  return sReturn;
}

/**
 * Encoding for type B switches with two rotary/sliding switches.
 *
 * The code word is a tristate word and with following bit pattern:
 *
 * +-----------------------------+-----------------------------+----------+------------+
 * | 4 bits address              | 4 bits address              | 3 bits   | 1 bit      |
 * | switch group                | switch number               | not used | on / off   |
 * | 1=0FFF 2=F0FF 3=FF0F 4=FFF0 | 1=0FFF 2=F0FF 3=FF0F 4=FFF0 | FFF      | on=F off=0 |
 * +-----------------------------+-----------------------------+----------+------------+
 *
 * @param nAddressCode  Number of the switch group (1..4)
 * @param nChannelCode  Number of the switch itself (1..4)
 * @param bStatus       Whether to switch on (true) or off (false)
 *
 * @return char[13], representing a tristate code word of length 12
 */
char* RCSwitch::getCodeWordB(int nAddressCode, int nChannelCode, bool bStatus) {
  static char sReturn[13];
  int nReturnPos = 0;

  if (nAddressCode < 1 || nAddressCode > 4 || nChannelCode < 1 || nChannelCode > 4) {
    return 0;
  }

  for (int i = 1; i <= 4; i++) {
    sReturn[nReturnPos++] = (nAddressCode == i) ? '0' : 'F';
  }

  for (int i = 1; i <= 4; i++) {
    sReturn[nReturnPos++] = (nChannelCode == i) ? '0' : 'F';
  }

  sReturn[nReturnPos++] = 'F';
  sReturn[nReturnPos++] = 'F';
  sReturn[nReturnPos++] = 'F';

  sReturn[nReturnPos++] = bStatus ? 'F' : '0';

  sReturn[nReturnPos] = '\0';
  return sReturn;
}

/**
 * Like getCodeWord (Type C = Intertechno)
 */
char* RCSwitch::getCodeWordC(char sFamily, int nGroup, int nDevice, bool bStatus) {
  static char sReturn[13];
  int nReturnPos = 0;

  int nFamily = (int)sFamily - 'a';
  if ( nFamily < 0 || nFamily > 15 || nGroup < 1 || nGroup > 4 || nDevice < 1 || nDevice > 4) {
    return 0;
  }
  
  // encode the family into four bits
  sReturn[nReturnPos++] = (nFamily & 1) ? 'F' : '0';
  sReturn[nReturnPos++] = (nFamily & 2) ? 'F' : '0';
  sReturn[nReturnPos++] = (nFamily & 4) ? 'F' : '0';
  sReturn[nReturnPos++] = (nFamily & 8) ? 'F' : '0';

  // encode the device and group
  sReturn[nReturnPos++] = ((nDevice-1) & 1) ? 'F' : '0';
  sReturn[nReturnPos++] = ((nDevice-1) & 2) ? 'F' : '0';
  sReturn[nReturnPos++] = ((nGroup-1) & 1) ? 'F' : '0';
  sReturn[nReturnPos++] = ((nGroup-1) & 2) ? 'F' : '0';

  // encode the status code
  sReturn[nReturnPos++] = '0';
  sReturn[nReturnPos++] = 'F';
  sReturn[nReturnPos++] = 'F';
  sReturn[nReturnPos++] = bStatus ? 'F' : '0';

  sReturn[nReturnPos] = '\0';
  return sReturn;
}

/**
 * Encoding for the REV Switch Type
 *
 * The code word is a tristate word and with following bit pattern:
 *
 * +-----------------------------+-------------------+----------+--------------+
 * | 4 bits address              | 3 bits address    | 3 bits   | 2 bits       |
 * | switch group                | device number     | not used | on / off     |
 * | A=1FFF B=F1FF C=FF1F D=FFF1 | 1=0FF 2=F0F 3=FF0 | 000      | on=10 off=01 |
 * +-----------------------------+-------------------+----------+--------------+
 *
 * Source: http://www.the-intruder.net/funksteckdosen-von-rev-uber-arduino-ansteuern/
 *
 * @param sGroup        Name of the switch group (A..D, resp. a..d) 
 * @param nDevice       Number of the switch itself (1..3)
 * @param bStatus       Whether to switch on (true) or off (false)
 *
 * @return char[13], representing a tristate code word of length 12
 */
char* RCSwitch::getCodeWordD(char sGroup, int nDevice, bool bStatus) {
  static char sReturn[13];
  int nReturnPos = 0;

  // sGroup must be one of the letters in "abcdABCD"
  int nGroup = (sGroup >= 'a') ? (int)sGroup - 'a' : (int)sGroup - 'A';
  if ( nGroup < 0 || nGroup > 3 || nDevice < 1 || nDevice > 3) {
    return 0;
  }

  for (int i = 0; i < 4; i++) {
    sReturn[nReturnPos++] = (nGroup == i) ? '1' : 'F';
  }

  for (int i = 1; i <= 3; i++) {
    sReturn[nReturnPos++] = (nDevice == i) ? '1' : 'F';
  }

  sReturn[nReturnPos++] = '0';
  sReturn[nReturnPos++] = '0';
  sReturn[nReturnPos++] = '0';

  sReturn[nReturnPos++] = bStatus ? '1' : '0';
  sReturn[nReturnPos++] = bStatus ? '0' : '1';

  sReturn[nReturnPos] = '\0';
  return sReturn;
}

/**
 * @param sCodeWord   a tristate code word consisting of the letter 0, 1, F
 */
void RCSwitch::sendTriState(const char* sCodeWord) {
  // turn the tristate code word into the corresponding bit pattern, then send it
  unsigned long code = 0;
  unsigned int length = 0;
  for (const char* p = sCodeWord; *p; p++) {
    code <<= 2L;
    switch (*p) {
      case '0':
        // bit pattern 00
        break;
      case 'F':
        // bit pattern 01
        code |= 1L;
        break;
      case '1':
        // bit pattern 11
        code |= 3L;
        break;
    }
    length += 2;
  }
  this->send(code, length);
}

/**
 * @param sCodeWord   a binary code word consisting of the letter 0, 1
 */
void RCSwitch::send(const char* sCodeWord) {
	if (this->nTransmitterPin == -1)
		return;

	#if not defined( RCSwitchDisableReceiving )
		// make sure the receiver is disabled while we transmit
		int nReceiverInterrupt_backup = nReceiverInterrupt;
		if (nReceiverInterrupt_backup != -1) {
			this->disableReceive();
		}
	#endif
	for (int nRepeat = 0; nRepeat < nRepeatTransmit; nRepeat++) {
		this->transmit(protocol.startSyncFactor);
		for (const char* p = sCodeWord; *p; p++) {
			if (*p == '0') this->transmit(protocol.zero);
			if (*p == '1') this->transmit(protocol.one);
		}
		this->transmit(protocol.stopSyncFactor);
	}

	#if not defined( RCSwitchDisableReceiving )
		// enable receiver again if we just disabled it
		if (nReceiverInterrupt_backup != -1) {
			this->enableReceive(nReceiverInterrupt_backup);
		}
	#endif
}


/**
 * Transmit the first 'length' bits of the integer 'code'. The
 * bits are sent from MSB to LSB, i.e., first the bit at position length-1,
 * then the bit at position length-2, and so on, till finally the bit at position 0.
 */
void RCSwitch::send(unsigned long code, unsigned int length) {

	char sendBinString[RCSWITCH_MAX_CHANGES/2+1];

	int j=0;
    for (int i = length-1; i >= 0; i--) {
      if (code & (1L << i))
		sendBinString[j]=1;
      else
		sendBinString[j]=0;
	  j++;
    }
	this->send(sendBinString);
}

/**
 * Transmit a single high-low pulse.
 */
void RCSwitch::transmit(HighLow pulses) {
  uint8_t firstLogicLevel = (this->protocol.invertedSignal) ? LOW : HIGH;
  uint8_t secondLogicLevel = (this->protocol.invertedSignal) ? HIGH : LOW;
  
  if (pulses.high>0) {
	digitalWrite(this->nTransmitterPin, firstLogicLevel);
	delayMicroseconds( this->protocol.pulseLength * pulses.high);
  }
  if (pulses.low>0) {
	digitalWrite(this->nTransmitterPin, secondLogicLevel);
	delayMicroseconds( this->protocol.pulseLength * pulses.low);
  }
}


#if not defined( RCSwitchDisableReceiving )
/**
 * Enable receiving data
 */
void RCSwitch::enableReceive(int interrupt) {
  this->nReceiverInterrupt = interrupt;
  this->enableReceive();
}

void RCSwitch::enableReceive() {
  if (this->nReceiverInterrupt != -1) {
    RCSwitch::nReceivedValue = 0;
    RCSwitch::nReceivedBitlength = 0;
#if defined(RaspberryPi) // Raspberry Pi
    wiringPiISR(this->nReceiverInterrupt, INT_EDGE_BOTH, &handleInterrupt);
#else // Arduino
    attachInterrupt(this->nReceiverInterrupt, handleInterrupt, CHANGE);
#endif
  }
  EnableReceiver=true;
}

/**
 * Disable receiving data
 */
void RCSwitch::disableReceive() {
#if not defined(RaspberryPi) // Arduino
  detachInterrupt(this->nReceiverInterrupt);
#endif // For Raspberry Pi (wiringPi) you can't unregister the ISR
  this->nReceiverInterrupt = -1;
  EnableReceiver=false;
}

bool RCSwitch::available() {
  #ifdef RaspberryPi
  pthread_mutex_lock(&thread_flag_mutex);
  while ( RCSwitch::nReceivedValue == 0 ){
	  pthread_cond_wait(&thread_flag_cv, &thread_flag_mutex);
  }
  pthread_mutex_unlock(&thread_flag_mutex);
  #endif
  return RCSwitch::nReceivedValue != 0;
}

void RCSwitch::resetAvailable() {
  RCSwitch::nReceivedValue = 0;
}

unsigned long RCSwitch::getReceivedValue() {
  return RCSwitch::nReceivedValue;
}

unsigned int RCSwitch::getReceivedBitlength() {
  return RCSwitch::nReceivedBitlength;
}

unsigned int RCSwitch::getReceivedDelay() {
  return RCSwitch::nReceivedDelay;
}

unsigned int RCSwitch::getReceivedProtocol() {
  return RCSwitch::nReceivedProtocol;
}

unsigned int* RCSwitch::getReceivedRawdata() {
  return RCSwitch::timings;
}

char* RCSwitch::getReceiveBinString() {
  return RCSwitch::nReceiveBinString;
}

char* RCSwitch::getLastReceiveBinString() {
  return RCSwitch::nLastReceiveBinString;
}

/* helper function for the receiveProtocol method */
static inline unsigned int diff(int A, int B) {
  return abs(A - B);
}

/**
 *
 */
bool RECEIVE_ATTR RCSwitch::receiveProtocol(const int p, unsigned int changeCount) {
	if ( changeCount < 8 ) return false; // ignore very short transmissions: no device sends them, so this must be noise
#ifdef ESP8266
    const Protocol &pro = proto[p-1];
#else
    Protocol pro;
    memcpy_P(&pro, &proto[p-1], sizeof(Protocol));
#endif

    unsigned long code = 0;
    //Assuming the longer pulse length is the pulse captured in timings[0]
    const unsigned int syncLengthInPulses =  ((pro.stopSyncFactor.low) > (pro.stopSyncFactor.high)) ? (pro.stopSyncFactor.low) : (pro.stopSyncFactor.high);
    const unsigned int delay = RCSwitch::timings[0] / syncLengthInPulses;
    const unsigned int delayTolerance = delay * RCSwitch::nReceiveTolerance / 100;
    
    /* For protocols that start low, the sync period looks like
     *               _________
     * _____________|         |XXXXXXXXXXXX|
     *
     * |--1st dur--|-2nd dur-|-Start data-|
     *
     * The 3rd saved duration starts the data.
     *
     * For protocols that start high, the sync period looks like
     *
     *  ______________
     * |              |____________|XXXXXXXXXXXXX|
     *
     * |-filtered out-|--1st dur--|--Start data--|
     *
     * The 2nd saved duration starts the data
     */
    unsigned int firstDataTiming = ( (pro.invertedSignal) ? (2) : (1) );
    //printf("evaluate: pulse:%d delay:%d tolerance:%d frist:%d\n",syncLengthInPulses,delay,delayTolerance,firstDataTiming);

    if (pro.startSyncFactor.high!=0 and pro.startSyncFactor.low!=0) { // protocol use start sysnc signal test it
        if (diff(RCSwitch::timings[firstDataTiming], delay * pro.startSyncFactor.high) > delayTolerance ||
            diff(RCSwitch::timings[firstDataTiming + 1], delay * pro.startSyncFactor.low) > delayTolerance) return false;
        firstDataTiming += 2;
	}
	int j=0;
	memset ( RCSwitch::nReceiveBinString, 0, RCSWITCH_MAX_CHANGES/2+1 );
    for (unsigned int i=firstDataTiming ; i < changeCount - 1; i += 2) {
        code <<= 1;
        if (diff(RCSwitch::timings[i], delay * pro.zero.high) < delayTolerance &&
            diff(RCSwitch::timings[i + 1], delay * pro.zero.low) < delayTolerance) {
			//printf("vyhodneceni: 0\n");
			nReceiveBinString[j++]='0';
            // zero
        } else if (diff(RCSwitch::timings[i], delay * pro.one.high) < delayTolerance &&
                   diff(RCSwitch::timings[i + 1], delay * pro.one.low) < delayTolerance) {
				   nReceiveBinString[j++]='1';
			//printf("evaluate: 1\n");
            // one
            code |= 1;
        } else {
            // Failed
            //printf("evaluate: FALSE pulse:%d delay:%d tolerance:%d %d %d\n",syncLengthInPulses,delay,delayTolerance,RCSwitch::timings[i],RCSwitch::timings[i+1]);
            return false;
        }
    }

	//printf("L: %s\n",RCSwitch::nLastReceiveBinString);
	//printf("N: %s\n",RCSwitch::nReceiveBinString);
	//printf("C: %d\n\n",strncmp(RCSwitch::nLastReceiveBinString, RCSwitch::nReceiveBinString, RCSWITCH_MAX_CHANGES/2+1));
	
	if (strncmp(RCSwitch::nLastReceiveBinString, RCSwitch::nReceiveBinString, RCSWITCH_MAX_CHANGES/2+1) !=0) {
		strncpy( RCSwitch::nLastReceiveBinString, RCSwitch::nReceiveBinString, RCSWITCH_MAX_CHANGES/2+1);
		return false; // packets must be min. 2 times the same
	}
	
	#ifdef RaspberryPi
	pthread_mutex_lock(&thread_flag_mutex);
	#endif
	RCSwitch::nReceivedValue = code;
	RCSwitch::nReceivedBitlength = (changeCount - 1) / 2;
	RCSwitch::nReceivedDelay = delay;
	RCSwitch::nReceivedProtocol = p;
	#ifdef RaspberryPi
	//place for threader conditions set
	pthread_cond_signal(&thread_flag_cv);
	pthread_mutex_unlock(&thread_flag_mutex);
	#endif
	return true;

}

void RECEIVE_ATTR RCSwitch::handleInterrupt() {
  if (EnableReceiver == false) return;										// if no enabled interrupt receiver fast end
  static unsigned int changeCount = 0;
  static unsigned long lastTime = 0;
  static unsigned int repeatCount = 0;

  const long time = micros();
  const unsigned int duration = time - lastTime;

  //printf("Handle interrupt (OL)%d\n", duration);
  //printf("%d\n", duration);
  if (duration > RCSwitch::nSeparationLimit) {
	//printf("Exceeding the time limit: %d %d\n", changeCount,RCSwitch::timings[0]);
    // A long stretch without signal level change occurred. This could
    // be the gap between two transmission.
    if (diff(duration, RCSwitch::timings[0]) < 200) {
      // This long signal is close in length to the long signal which
      // started the previously recorded timings; this suggests that
      // it may indeed by a a gap between two transmissions (we assume
      // here that a sender will send the signal multiple times,
      // with roughly the same gap between them).
      repeatCount++;
      if (repeatCount == 2) {
		//printf("Do evaluate: %d\n", changeCount);
        for(unsigned int i = 1; i <= numProto; i++) {
          if (receiveProtocol(i, changeCount)) {
            // receive succeeded for protocol i
            break;
          }
        }
        repeatCount = 0;
      }
    }
    changeCount = 0;
  }
  // detect overflow
  if (changeCount >= RCSWITCH_MAX_CHANGES) {
	//printf("Overflow: %d\n", changeCount );
    changeCount = 0;
    repeatCount = 0;
  }

  RCSwitch::timings[changeCount++] = duration;
  lastTime = time;  
}
#endif

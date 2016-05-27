/**The MIT License (MIT)

Copyright (c) 2016 by Greg Cunningham, CuriousTech

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Build with Arduino IDE 1.6.8 and esp8266 SDK 2.2.0

#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include "WiFiManager.h"
#include <ESP8266WebServer.h>
#include <TimeLib.h> // http://www.pjrc.com/teensy/td_libs_Time.html
#include <WiFiUdp.h>
#include "HVAC.h"
#include <Event.h>
#include <XMLReader.h>
#include "Encoder.h"
#include "WebHandler.h"
#include "display.h"
#include <Wire.h>

// Uncomment only one of these
#include <SHT21.h> // https://github.com/CuriousTech/ESP8266-HVAC/tree/master/Libraries/SHT21
//#include <DHT.h>  // http://www.github.com/markruys/arduino-DHT

//----- Pin Configuration - See HVAC.h for the rest -
#define ESP_LED   2  //Blue LED on ESP07 (on low) also SCL
#define SCL       2
#define SDA      13

#define ENC_A    5  // Encoder is on GPIO4 and 5
#define ENC_B    4
//------------------------
const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
WiFiUDP Udp;
bool bNeedUpdate;
Display display;
eventHandler event(dataJson);

HVAC hvac;

#ifdef SHT21_H
SHT21 sht(SDA, SCL, 5);
#endif
#ifdef dht_h
DHT dht;
#endif

XML_tag_t Xtags[] =
{
  {"time-layout", "time-coordinate", "local", 19},
  {"temperature", "type", "hourly", 19},
  {NULL}
};

bool bGettingForecast;
char buffer[260]; // buffer for xml when in use

void xml_callback(int8_t item, int8_t idx, char *p)
{
  int8_t newtz;
  int8_t h;
  int8_t d;
  static int8_t hO;
  static int8_t lastd;

  switch(item)
  {
    case 0:            // valid time
      if(idx == 0)     // first item isn't really data
      {
        hO = 0;        // reset hour offset
        lastd = day();
        break;
      }
      d = atoi(p + 8);  // 2014-mm-ddThh:00:00-tz:00
      h = atoi(p + 11);
      if(idx != 1 && d != lastd)
        hO += 24; // change to hours offset
      lastd = d;
      hvac.m_fcData[idx-1].h = h + hO;

      newtz = atoi(p + 20); // tz minutes = atoi(p+23) but uncommon
      if(p[19] == '-') // its negative
        newtz = -newtz;

      if(idx == 1 && newtz != hvac.m_EE.tz) // DST change occurs this hour
      {
        hvac.m_EE.tz = newtz;
        getUdpTime(); // correct for new DST
      }
      break;
    case 1:                  // temperature
      if(idx)               // 1st value is not temp
        hvac.m_fcData[idx-1].t = atoi(p);
      break;
  }
}

XMLReader xml(buffer, 257, xml_callback);

void GetForecast()
{
  char *p_cstr_array[] =
  {
    (char *)"/xml/sample_products/browser_interface/ndfdXMLclient.php?zipCodeList=",
    hvac.m_EE.zipCode,
    (char*)"&Unit=e&temp=temp&Submit=Submit",
    NULL
  };

  bGettingForecast = xml.begin("graphical.weather.gov", p_cstr_array);
  if(!bGettingForecast)
    event.alert("Forecast failed");
}

//-----

Encoder rot(ENC_B, ENC_A);

bool EncoderCheck()
{
  int r = rot.poll();

  if(r == 0)  // no change
    return false;

  display.screen(true); // ensure page is thermostat

  int8_t m = (display.m_adjustMode < 2) ? Mode_Cool : Mode_Heat; // lower 2 are cool
  int8_t hilo = (display.m_adjustMode ^ 1) & 1; // hi or low of set
  int16_t t = hvac.getSetTemp(m, hilo ); // 

//  t += (r > 0) ? 1 : -1; // inc/dec by 1 only
  t += r; // inc/dec by any amount

  hvac.setTemp(m, t, hilo);

  display.updateTemps();
  return true;
}

void setup()
{
  Serial.begin(115200);  // Nextion must be set with bauds=115200
  startServer();
  eeRead(); // don't access EE before WiFi init
  hvac.init();
  display.init();
  getUdpTime(); // start the SMTP get
#ifdef SHT21_H
  sht.init();
#endif
#ifdef dht_h
  dht.setup(SDA, DHT::DHT22);
#endif
}

void loop()
{
  static uint8_t hour_save, min_save, sec_save;
  static int8_t lastSec;
  static int8_t lastHour;

  if(bNeedUpdate)   // if getUpdTime was called
    checkUdpTime();

  while( EncoderCheck() );
  display.checkNextion();  // check for touch, etc.
  handleServer(); // handles mDNS, web
#ifdef SHT21_H
  if(sht.service())
  {
    hvac.updateIndoorTemp( sht.getTemperatureF() * 10, sht.getRh() * 10 );
  }
#endif
  if(sec_save != second()) // only do stuff once per second
  {
    sec_save = second();
    secondsServer(); // once per second stuff
    display.oneSec();
    hvac.service();   // all HVAC code

#ifdef dht_h
    static uint8_t read_delay = 2;
    if(--read_delay == 0)
    {
      float temp = dht.toFahrenheit(dht.getTemperature());

      if(dht.getStatus() == DHT::ERROR_NONE)
      {
        hvac.updateIndoorTemp( temp * 10, dht.getHumidity() * 10);
      }
      read_delay = 5; // update every 5 seconds
    }
#endif

    if(min_save != minute()) // only do stuff once per minute
    {
      min_save = minute();
      if (hour_save != hour()) // update our IP and time daily (at 2AM for DST)
      {
        eeWrite(); // update EEPROM if needed while we're at it (give user time to make many adjustments)
        if( (hour_save = hour()) == 2)
          getUdpTime();
      }

      if(--display.m_updateFcst <= 0 )  // usually every hour / 3 hours
      {
        GetForecast();
      }
    }
 
    if(bGettingForecast)
    {
      if(! (bGettingForecast = xml.service(Xtags)) )
      {
        switch(xml.getStatus())
        {
          case XML_DONE:
            hvac.enable();
            display.drawForecast(true);
            event.alert("Forecast success");
            break;
          default:
            hvac.disable();
            hvac.m_notif = Note_Forecast;
            display.m_updateFcst = 5;    // retry in 5 mins
            break;
        }
      }
    }
  }
  delay(8); // rotary encoder and lines() need 8ms minimum
}

extern WiFiManager wifi;

void eeWrite() // write the settings if changed
{
  uint16_t old_sum = hvac.m_EE.sum;
  hvac.m_EE.sum = 0;
  hvac.m_EE.sum = Fletcher16((uint8_t *)&hvac.m_EE, sizeof(EEConfig));

  if(old_sum == hvac.m_EE.sum)
    return; // Nothing has changed?
  wifi.eeWriteData((uint8_t*)&hvac.m_EE, sizeof(EEConfig)); // WiFiManager already has an instance open, so use that at offset 64+
}

void eeRead()
{
  EEConfig eeTest;

  wifi.eeReadData((uint8_t*)&eeTest, sizeof(EEConfig));
  if(eeTest.size != sizeof(EEConfig)) return; // revert to defaults if struct size changes
  uint16_t sum = eeTest.sum;
  eeTest.sum = 0;
  eeTest.sum = Fletcher16((uint8_t *)&eeTest, sizeof(EEConfig));
  if(eeTest.sum != sum) return; // revert to defaults if sum fails
  memcpy(&hvac.m_EE, &eeTest, sizeof(EEConfig));
}

uint16_t Fletcher16( uint8_t* data, int count)
{
   uint16_t sum1 = 0;
   uint16_t sum2 = 0;

   for( int index = 0; index < count; ++index )
   {
      sum1 = (sum1 + data[index]) % 255;
      sum2 = (sum2 + sum1) % 255;
   }

   return (sum2 << 8) | sum1;
}

void getUdpTime()
{
  if(bNeedUpdate) return;
//  Serial.println("getUdpTime");
  Udp.begin(2390);
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;
  
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  // time.nist.gov
  Udp.beginPacket("0.us.pool.ntp.org", 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
  bNeedUpdate = true;
}

bool checkUdpTime()
{
  static int retry = 0;

  if(!Udp.parsePacket())
  {
    if(++retry > 500)
     {
        getUdpTime();
        retry = 0;
     }
    return false;
  }
//  Serial.println("checkUdpTime good");

  // We've received a packet, read the data from it
  Udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

  Udp.stop();
  // the timestamp starts at byte 40 of the received packet and is four bytes,
  // or two words, long. First, extract the two words:

  unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
  unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
  unsigned long secsSince1900 = highWord << 16 | lowWord;
  // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
  const unsigned long seventyYears = 2208988800UL;
  long timeZoneOffset = 3600 * hvac.m_EE.tz;
  unsigned long epoch = secsSince1900 - seventyYears + timeZoneOffset + 1; // bump 1 second

  // Grab the fraction
  highWord = word(packetBuffer[44], packetBuffer[45]);
  lowWord = word(packetBuffer[46], packetBuffer[47]);
  unsigned long d = (highWord << 16 | lowWord) / 4295000; // convert to ms
  delay(d); // delay to next second (meh)
  setTime(epoch);
  
//  Serial.print("Time ");
//  Serial.println(timeFmt(true, true));
  bNeedUpdate = false;
  return true;
}

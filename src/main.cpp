#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <Update.h>
#include <CEC_Device.h>
#include <HTTPUpdateServer.h>
#include <time.h>
#include "CronAlarms.h"
#include "FS.h"
#include "main.h"

bool isTransmitting = false;

class MyCEC_Device : public CEC_Device
{
protected:
	virtual bool LineState();
	virtual void SetLineState(bool);
	virtual void OnReady(int logicalAddress);
	virtual void OnReceiveComplete(unsigned char* buffer, int count, bool ack);
	virtual void OnTransmitComplete(unsigned char* buffer, int count, bool ack);
};

bool MyCEC_Device::LineState()
{
	int state = digitalRead(CEC_GPIO);
	return state != LOW;
}

void MyCEC_Device::SetLineState(bool state)
{
	if (state) {
		pinMode(CEC_GPIO, INPUT_PULLUP);
	} else {
		digitalWrite(CEC_GPIO, LOW);
		pinMode(CEC_GPIO, OUTPUT);
	}
}

void MyCEC_Device::OnReady(int logicalAddress)
{
	// This is called after the logical address has been allocated
	unsigned char buf[4] = {0x84, CEC_PHYSICAL_ADDRESS >> 8, CEC_PHYSICAL_ADDRESS & 0xff, CEC_DEVICE_TYPE};

	DbgPrint("Device ready, Logical address assigned: %d\n", logicalAddress);

	TransmitFrame(0xf, buf, 4); // <Report Physical Address>
}

void MyCEC_Device::OnReceiveComplete(unsigned char* buffer, int count, bool ack)
{
	// This is called when a frame is received.  To transmit
	// a frame call TransmitFrame.  To receive all frames, even
	// those not addressed to this device, set Promiscuous to true.
	DbgPrint("Packet received at %ld: %02X", millis(), buffer[0]);
	for (int i = 1; i < count; i++)
		DbgPrint(":%02X", buffer[i]);
	if (!ack)
		DbgPrint(" NAK");
	DbgPrint("\n");

	// Ignore messages not sent to us
	if ((buffer[0] & 0xf) != LogicalAddress())
		return;

    if (count == 1) {
        // This seems to be what happens with the Chromecast, no message then reply with vendor id
        TransmitFrame(0xf, (unsigned char*)"\x87\x01\x23\x45", 4); // <Device Vendor ID>
    }
	// No command received?
	if (count < 2)
		return;

	switch (buffer[1]) {
        case 0x83: { // <Give Physical Address>
            unsigned char buf[4] = {0x84, CEC_PHYSICAL_ADDRESS >> 8, CEC_PHYSICAL_ADDRESS & 0xff, CEC_DEVICE_TYPE};
            TransmitFrame(0xf, buf, 4); // <Report Physical Address>
            break;
        }
        case 0x8c: // <Give Device Vendor ID>
            TransmitFrame(0xf, (unsigned char*)"\x87\x01\x23\x45", 4); // <Device Vendor ID>
            break;
        case 0x8f: // <Give Power status>
            TransmitFrame(0xf, (unsigned char*)"\x90\x00", 2);
            break;
        case 0x46: // <Give Device Name>
            TransmitFrame(0xf, (unsigned char*)"\x47\x53\x63\x72\x65\x6e\x54\x69\x6d\x65\x72", 11);
            break;
        case 0x9f: // <Give CEC Version>
            TransmitFrame(0xf, (unsigned char*)"\x9e\x05", 2);
            break;
    }
}

void MyCEC_Device::OnTransmitComplete(unsigned char* buffer, int count, bool ack)
{
    isTransmitting = 0;
	// This is called after a frame is transmitted.
	DbgPrint("Packet sent at %ld: %02X", millis(), buffer[0]);
	for (int i = 1; i < count; i++)
		DbgPrint(":%02X", buffer[i]);
	if (!ack)
		DbgPrint(" NAK");
	DbgPrint("\n");
}

MyCEC_Device device;

void working_led()
{
    digitalWrite(LED_BUILTIN, HIGH);
    delay(50);
    digitalWrite(LED_BUILTIN, LOW);
    delay(50);
}

void setup_ntp()
{
    struct tm timeinfo;

    while (1)  {
        Serial.println("Setting up time");
        configTime(0, 0, "pool.ntp.org");
        setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3",1);
        tzset();

        if (!getLocalTime(&timeinfo)) {
            Serial.println("Failed to obtain time");
            continue;
        }
        break;
    }
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S zone %Z %z ");
}

void setup_wifi()
{
    // We have disabled AP mode as we will be using hard coded credentials
    //WiFiManager wifiManager;
    //wifiManager.resetSettings();
    //wifiManager.autoConnect(DEVICE_NAME);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wifi connecting...");
        delay(500);
    }
    Serial.println("Wifi connected");
}

void check_wifi() {
    if (WiFi.status() != WL_CONNECTED) {
        setup_wifi();
    }
}

/**
 * Setup OTA firmware updates via HTTP
 */
WebServer otaHttpServer(HTTP_SERVER_PORT);
HTTPUpdateServer httpUpdater;

void handleRoot()
{
    otaHttpServer.send(200, "text/plain", "Hello :)\r\nGo to /update to update firmware");
}

void setup_ota_firmware()
{
    httpUpdater.setup(&otaHttpServer);
    otaHttpServer.begin();
    otaHttpServer.on("/", handleRoot);
}

void setup_mdns()
{
    // Advertise our firmware HTTP server with Multicast DNS
    if (!MDNS.begin(DEVICE_NAME)) {
        Serial.println("Error setting up MDNS responder!");
        return;
    }
    MDNS.addService("http", "tcp", HTTP_SERVER_PORT);
    Serial.println("MDNS responder started");
}

void setup_cec()
{
    pinMode(CEC_GPIO, INPUT_PULLUP);
	device.Initialize(CEC_PHYSICAL_ADDRESS, CEC_DEVICE_TYPE, true); // Promiscuous mode
}

bool transmit_frame(int targetAddress, const unsigned char* buffer, int count)
{
    isTransmitting = true;
    return device.TransmitFrame(targetAddress, buffer, count);
}

void tv_hdmi(int number)
{
    unsigned char buffer[3];
    buffer[0] = 0x82;
    buffer[1] = number << 4;
    buffer[2] = 0x00;
    transmit_frame(0x0f, buffer, 3);
}

void tv_on()
{
    transmit_frame(0x0, (unsigned char*)"\x04", 1);
}

void tv_off()
{
    transmit_frame(0x0, (unsigned char*)"\x36", 1);
}

void setup_cron() {
    Cron.create("0 0 17 * * 1-5", tv_off, false);
    Cron.create("0 0 7 * * 1-5", tv_on,  false);
    //Cron.create("0 * * * * 0-6", tv_on, false);
    //Cron.create("30 * * * * 0-6", tv_off, false);
}

void setup()
{
//    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(BAUD_RATE);
    setup_wifi();
    setup_ntp();
    setup_cron();
    setup_mdns();
    setup_cec();
    setup_ota_firmware();
}

bool enableCron = true;
void loop()
{
    if (!isTransmitting)  {
        check_wifi();

        if (enableCron) {
            // Enabling OTA firmware updates or Cron will cause delays in CEC handler so receives will be missed
            otaHttpServer.handleClient();
            Cron.delay();
        }
    }
    device.Run();

    if (Serial.available()) {

        // Disable cron so we will receive any further messages properly
        enableCron = false;
        switch (Serial.read()) {
            case '0':
                tv_off();
                break;
            case 'o':
                tv_on();
                break;
            case '1':
                tv_hdmi(1);
                break;
            case '2':
                tv_hdmi(2);
                break;
            case '3':
                tv_hdmi(3);
                break;
            case 'h':
                transmit_frame(0x0, (unsigned char*)"\x44\x03", 2);
                break;
            case 'j':
                transmit_frame(0x0, (unsigned char*)"\x44\x02", 2);
                break;
            case 'k':
                transmit_frame(0x0, (unsigned char*)"\x44\x01", 2);
                break;
            case 'l':
                transmit_frame(0x0, (unsigned char*)"\x44\x04", 2);
                break;
        }
    }
}

// hard-coded path, I know. Arduino IDE sucks
#include "D:/Projects/ESPStore/WifiStore/MySoftwareWire.inl"

enum class DayOfWeek : unsigned char  //
{
	Unset = 0,
	Monday = 77,     // ('M' & 'o')
	Tuesday = 84,    // ('T' & 'u')
	Wednesday = 69,  // ('W' & 'e')
	Thursday = 64,   // ('T' & 'h')
	Friday = 66,     // ('F' & 'r')
	Saturday = 65,   // ('S' & 'a')
	Sunday = 81,     // ('S' & 'u')
};

DayOfWeek strToDayOfWeek(const char* day)  //
{
	return DayOfWeek(day[0] & day[1]);
}

struct RTCSample
{
	uint8_t dayOfWeek;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;

	bool operator==(const RTCSample& o) const
	{
		return dayOfWeek == o.dayOfWeek &&
		       hour == o.hour &&
		       minute == o.minute &&
		       second == o.second;
	}
};

typedef struct Date  //
{
	unsigned char yearOffset; // offset from 2000
	unsigned char month;
	unsigned char day;
	unsigned char hour;
	unsigned char minute;
	unsigned char seconds;
	unsigned short milliSeconds;
	DayOfWeek dayOfWeek;
	unsigned char dayOfWeek1to7;
} Date;

#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <DS3231.h>
#include <Wire.h>
#include <functional>

ESP8266WebServer server(80);

struct SavedData  //
{
	char ssid[32];
	char pass[32];
	unsigned long timeToOpen;
	unsigned long timeToClose;
};

#define WINDOW_PIN D2
#define STORE_UP_PIN D8
#define STORE_DOWN_PIN D9

#define IS_WINDOW_OPEN (digitalRead(WINDOW_PIN) == LOW)
#define IS_WINDOW_CLOSED (digitalRead(WINDOW_PIN) == HIGH)

class StoreStateMachine  //
{
public:
	enum State  //
	{
		Idle,
		Opening,
		Closing,
		Calibrating,
	};

	void run()  //
	{
		switch (m_currentState)  //
		{
			case Opening:
				stateOpening();
				break;
			case Closing:
				stateClosing();
				break;
			case Calibrating:
				stateCalibrating();
				break;

			case Idle:
				break;
			default:
				break;
		}
	}

	void open()  //
	{
		if (m_isCurrentPercentUnknown == false && m_currentPercent == 1.0f)
			return;

		m_targetPercent = 1.0f;

		if (m_currentState == Closing)
			stateClosingExit();
		else if (m_currentState == Calibrating)
			stateCalibratingExit();

		stateOpeningEnter();
	}

	void stop()  //
	{
		if (m_currentState == Idle)
			return;

		else if (m_currentState == Opening)
			stateOpeningExit();
		else if (m_currentState == Closing)
			stateClosingExit();
		else if (m_currentState == Calibrating)
			stateCalibratingExit();

		stateIdleEnter();
	}

	void close()  //
	{
		if (m_isCurrentPercentUnknown == false && m_currentPercent == 0.0f)
			return;

		m_targetPercent = 0.0f;

		if (m_currentState == Opening)
			stateOpeningExit();
		else if (m_currentState == Calibrating)
			stateCalibratingExit();

		stateClosingEnter();
	}

	void setPercent(float percent)  //
	{
		if (percent > 1.0f)
			percent = 1.0f;
		else if (percent < 0.0f)
			percent = 0.0f;

		if (m_isCurrentPercentUnknown)  //
		{
			m_targetPercent = percent;
			if (m_currentState == Opening)
				stateOpeningExit();
			else if (m_currentState == Closing)
				stateClosingExit();
			else if (m_currentState == Calibrating)
				stateCalibratingExit();

			stateCalibratingEnter();
		}     //
		else  //
		{
			if (percent == m_targetPercent)
				return;

			if (m_currentState == Closing)
				stateClosingExit();
			else if (m_currentState == Opening)
				stateOpeningExit();
			else if (m_currentState == Calibrating)
				stateCalibratingExit();

			if (percent < m_targetPercent)  //
			{
				m_targetPercent = percent;
				stateClosingEnter();
			}     //
			else  //
			{
				m_targetPercent = percent;
				stateOpeningEnter();
			}
		}
	}

	void calibrate()  //
	{
		m_isCurrentPercentUnknown = true;
		setPercent(0.0f);
	}

	void setTimeToOpen(unsigned long timeToOpen)  //
	{
		stop();
		m_timeToOpen = timeToOpen;
	}

	void setTimeToClose(unsigned long timeToClose)  //
	{
		stop();
		m_timeToClose = timeToClose;
	}

	unsigned long timeToOpen() const  //
	{
		return m_timeToOpen;
	}

	unsigned long timeToClose() const  //
	{
		return m_timeToClose;
	}

	float currentPercent() const  //
	{
		return m_currentPercent;
	}

	float targetPercent() const  //
	{
		return m_targetPercent;
	}

	State currentState() const  //
	{
		return m_currentState;
	}

	bool isCurrentPercentUnknown() const  //
	{
		return m_isCurrentPercentUnknown;
	}

private:
	void stateIdleEnter()  //
	{
		Serial.println("Idling");
		m_currentState = Idle;
	}

	void stateOpeningEnter()  //
	{
		Serial.println("Opening");
		m_currentState = Opening;
		m_stateEntryPercent = m_currentPercent;
		digitalWrite(STORE_UP_PIN, HIGH);
		m_stateEntryTime = millis();
	}
	void stateOpening()  //
	{
		unsigned long timeDiff = millis() - m_stateEntryTime;

		if (m_isCurrentPercentUnknown)  //
		{
			if (timeDiff > m_timeToOpen)  //
			{
				m_isCurrentPercentUnknown = false;
				m_currentPercent = 1.0f;
				stateOpeningExit();
				stateIdleEnter();
			}
		}     //
		else  //
		{
			m_currentPercent = m_stateEntryPercent + timeDiff / float(m_timeToOpen);
			Serial.printf("current percent: %f\n", m_currentPercent);
			if (m_currentPercent >= m_targetPercent)  //
			{
				m_isCurrentPercentUnknown = false;
				m_currentPercent = m_targetPercent;
				stateOpeningExit();
				stateIdleEnter();
			}
		}
	}
	void stateOpeningExit()  //
	{
		digitalWrite(STORE_UP_PIN, LOW);
		delay(500);
	}

	void stateClosingEnter()  //
	{
		Serial.println("Closing");
		m_currentState = Closing;
		m_stateEntryPercent = m_currentPercent;
		digitalWrite(STORE_DOWN_PIN, HIGH);
		m_stateEntryTime = millis();
	}
	void stateClosing()  //
	{
		unsigned long timeDiff = millis() - m_stateEntryTime;

		if (m_isCurrentPercentUnknown)  //
		{
			if (timeDiff > m_timeToClose)  //
			{
				m_isCurrentPercentUnknown = false;
				m_currentPercent = 0.0f;
				stateClosingExit();
				stateIdleEnter();
			}
		}     //
		else  //
		{
			m_currentPercent = m_stateEntryPercent - timeDiff / float(m_timeToClose);
			Serial.printf("current percent: %f\n", m_currentPercent);
			if (m_currentPercent <= m_targetPercent)  //
			{
				m_isCurrentPercentUnknown = false;
				m_currentPercent = m_targetPercent;
				stateClosingExit();
				stateIdleEnter();
			}
		}
	}
	void stateClosingExit()  //
	{
		digitalWrite(STORE_DOWN_PIN, LOW);
		delay(500);
	}

	void stateCalibratingEnter()  //
	{
		Serial.println("Calibrating");
		m_currentState = Calibrating;
		digitalWrite(STORE_DOWN_PIN, HIGH);
		m_stateEntryTime = millis();
	}
	void stateCalibrating()  //
	{
		unsigned long timeDiff = millis() - m_stateEntryTime;

		if (timeDiff > m_timeToClose * 2)  //
		{
			m_isCurrentPercentUnknown = false;
			m_currentPercent = 0.0f;
			stateCalibratingExit();
			stateOpeningEnter();
		}
	}
	void stateCalibratingExit()  //
	{
		digitalWrite(STORE_DOWN_PIN, LOW);
		delay(500);
	}

	unsigned long m_timeToOpen = 28000;   // in milliseconds
	unsigned long m_timeToClose = 28000;  // in milliseconds

	State m_currentState = Idle;

	bool m_isCurrentPercentUnknown = true;
	float m_currentPercent = 0.0f;
	float m_targetPercent = 0.0f;

	unsigned long m_stateEntryTime = 0;
	float m_stateEntryPercent = 0.0f;
};

StoreStateMachine storeStateMachine;

const char* storeIndex = R"(<!DOCTYPE html>
<html lang="en">
	<head>
		<meta charset="utf-8">
		<title>Single store controller</title>
	</head>
	<body>

		<script>
			function updateAll()
			{
				var xhttp = new XMLHttpRequest();
				xhttp.onreadystatechange = function()
				{
						if (this.readyState == 4 && this.status == 200)
							document.getElementById("all").innerHTML = xhttp.responseText;
				};
				xhttp.open("GET", "all", true);
				xhttp.send();
			}
			function updateState()
			{
				var xhttp = new XMLHttpRequest();
				xhttp.onreadystatechange = function()
				{
						if (this.readyState == 4 && this.status == 200)
							document.getElementById("state").innerHTML = xhttp.responseText;
				};
				xhttp.open("GET", "state", true);
				xhttp.send();
			}
			function updateCurrentPercent()
			{
				var xhttp = new XMLHttpRequest();
				xhttp.onreadystatechange = function()
				{
						if (this.readyState == 4 && this.status == 200)
							document.getElementById("currentPercent").innerHTML = xhttp.responseText;
				};
				xhttp.open("GET", "currentPercent", true);
				xhttp.send();
			}
			function updateTargetPercent()
			{
				var xhttp = new XMLHttpRequest();
				xhttp.onreadystatechange = function()
				{
						if (this.readyState == 4 && this.status == 200)
							document.getElementById("targetPercent").innerHTML = xhttp.responseText;
				};
				xhttp.open("GET", "targetPercent", true);
				xhttp.send();
			}
			function postRequest(body)
			{
				var xhttp = new XMLHttpRequest();
				xhttp.open("POST", "/", true);
				xhttp.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
				xhttp.send(body);
			}
			function onClickOpen()
			{
				postRequest("force&open");
			}
			function onClickStop()
			{
				postRequest("force&stop");
			}
			function onClickClose()
			{
				postRequest("force&close");
			}
			function onClickSetPercent()
			{
				postRequest("force&openPercent=" + document.getElementById("openPercent").value);
			}
			function onClickSetDay()
			{
				postRequest("day=" + document.getElementById("day").value);
			}
			function onClickSetHour()
			{
				postRequest("hour=" + document.getElementById("hour").value);
			}
			function onClickSetMinute()
			{
				postRequest("minute=" + document.getElementById("minute").value);
			}
			function onClickSetSecond()
			{
				postRequest("second=" + document.getElementById("second").value);
			}
			function onClickRestart()
			{
				var xhttp = new XMLHttpRequest();
				xhttp.open("POST", "/restart", true);
				xhttp.send();
			}

			setInterval(updateAll, 200);
		</script>
		<div>
			<button onClick="onClickOpen() ">Open</button>
			<button onClick="onClickStop() ">Stop</button>
			<button onClick="onClickClose() ">Close</button>
		</div>
		<div>
			<label for="openPercent">Set open at %</label>
			<input name="openPercent" id="openPercent" />
			<button onClick="onClickSetPercent() ">Set open at %</button>
		<br/>
		<div>
			<label for="day">day</label>
			<input name="day" id="day" />
			<button onClick="onClickSetDay() ">Set day</button>
		</div>
		<div>
			<label for="hour">hour</label>
			<input name="hour" id="hour" />
			<button onClick="onClickSetHour() ">Set hour</button>
		</div>
		<div>
			<label for="minute">minute</label>
			<input name="minute" id="minute" />
			<button onClick="onClickSetMinute() ">Set minute</button>
		</div>
		<div>
			<label for="second">second</label>
			<input name="second" id="second" />
			<button onClick="onClickSetSecond() ">Set second</button>
		</div>
		<br/>
		<div>
			<button onClick="onClickRestart() ">Restart</button>
		</div>
		<div id="all"></div>
	</body>
</html>
)";

void writeCredentialsToEEPROM(const String& ssid, const String& pass)  //
{
	SavedData* savedData = reinterpret_cast<SavedData*>(EEPROM.getDataPtr());

	memset(savedData->ssid, 0, sizeof(savedData->ssid));
	memset(savedData->pass, 0, sizeof(savedData->pass));

	std::copy(ssid.begin(), ssid.end(), savedData->ssid);
	std::copy(pass.begin(), pass.end(), savedData->pass);

	EEPROM.commit();
}

void writeTimeToOpen(unsigned long ttOpen)  //
{
	SavedData* savedData = reinterpret_cast<SavedData*>(EEPROM.getDataPtr());
	savedData->timeToOpen = ttOpen;
	EEPROM.commit();
}
void writeTimeToClose(unsigned long ttClose)  //
{
	SavedData* savedData = reinterpret_cast<SavedData*>(EEPROM.getDataPtr());
	savedData->timeToClose = ttClose;
	EEPROM.commit();
}

DS3231 RTC;

bool getCurrentDate(struct Date& outDate)  //
{
	BearSSL::WiFiClientSecure client;
	client.setInsecure();
	HTTPClient http;

	bool res = true;

	Serial.print("[HTTP] begin...\n");
	if (http.begin(client, "https://timeapi.io/api/Time/current/zone?timeZone=Europe/Paris"))  //
	{
		Serial.print("[HTTP] GET...\n");
		// start connection and send HTTP header
		const int httpCode = http.GET();

		// httpCode will be negative on error
		if (httpCode > 0)  //
		{
			// HTTP header has been send and Server response header has been handled
			Serial.printf("[HTTP] GET... code: %d\n", httpCode);

			// file found at server
			if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)  //
			{
				const String payload = http.getString();
				Serial.print("payload: ");
				Serial.println(payload);

				std::unique_ptr<StaticJsonDocument<512>> docPtr = std::make_unique<StaticJsonDocument<512>>();
				auto& doc = *docPtr;
				const DeserializationError error = deserializeJson(doc, payload.c_str());

				if (error)  //
				{
					Serial.print(F("deserializeJson() failed: "));
					Serial.println(error.f_str());
					res = false;
				}     //
				else  //
				{
					outDate.yearOffset = static_cast<unsigned char>(static_cast<uint32_t>(doc["year"]) - 2000);
					outDate.month = doc["month"];
					outDate.day = doc["day"];
					outDate.hour = doc["hour"];
					outDate.minute = doc["minute"];
					outDate.seconds = doc["seconds"];
					outDate.milliSeconds = doc["milliSeconds"];
					const char* const dayOfWeek = doc["dayOfWeek"];
					outDate.dayOfWeek = strToDayOfWeek(dayOfWeek);
					switch (outDate.dayOfWeek)
					{
					case DayOfWeek::Monday:    outDate.dayOfWeek1to7 = 1; break;
					case DayOfWeek::Tuesday:   outDate.dayOfWeek1to7 = 2; break;
					case DayOfWeek::Wednesday: outDate.dayOfWeek1to7 = 3; break;
					case DayOfWeek::Thursday:  outDate.dayOfWeek1to7 = 4; break;
					case DayOfWeek::Friday:    outDate.dayOfWeek1to7 = 5; break;
					case DayOfWeek::Saturday:  outDate.dayOfWeek1to7 = 6; break;
					case DayOfWeek::Sunday:    outDate.dayOfWeek1to7 = 7; break;

					default:
					case DayOfWeek::Unset:     outDate.dayOfWeek1to7 = 0; break;
					}
				}
			}
			else if (httpCode == 400)
			{
				Serial.println("getting date failed...");

				const String payload = http.getString();
				Serial.print("payload: ");
				Serial.println(payload);

				res = false;
			}
		}     //
		else  //
		{
			Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
			res = false;
		}

		http.end();
	}     //
	else  //
	{
		Serial.println("[HTTP] Unable to connect");
		res = false;
	}

	return res;
}

void resyncInternet()
{
	struct Date date;
	Serial.println("getting current date...");
	if (getCurrentDate(date))
	{
		Serial.printf("date: %d:%d:%d'%d %d\n", date.hour, date.minute, date.seconds, date.milliSeconds, date.dayOfWeek1to7);

		RTC.setYear(date.yearOffset);
		RTC.setMonth(date.month);
		RTC.setDate(date.day);
		RTC.setDoW(date.dayOfWeek1to7);
		RTC.setHour(date.hour);
		RTC.setMinute(date.minute);
		RTC.setSecond(date.seconds);
	}
	else
	{
		Serial.println("resync failed");
	}
}

struct RTCSample sampleRTC()
{
	// if (!RTC.oscillatorCheck())
	// {
	// 	Serial.println("Oscillator Stop Flag is set. Time may not be accurate...");
	// 	// return {0, 0, 0, 0};
	// }

	bool h12;
	bool hPM;

	RTCSample res{
		RTC.getDoW(),
		RTC.getHour(h12, hPM),
		RTC.getMinute(),
		RTC.getSecond(),
	};

	return res;
}

RTCSample currentDate;

bool hasAP = false;

void setupClock()
{
	RTC.setClockMode(false);
	RTC.enable32kHz(false);
	RTC.enableOscillator(true, false, 3);
	RTC.turnOffAlarm(1);
	RTC.turnOffAlarm(2);
}

void setup()  //
{
	pinMode(WINDOW_PIN, INPUT);
	pinMode(STORE_UP_PIN, OUTPUT);
	pinMode(STORE_DOWN_PIN, OUTPUT);
	digitalWrite(STORE_UP_PIN, LOW);
	digitalWrite(STORE_DOWN_PIN, LOW);

	Wire.begin();
	Serial.begin(115200);
	Serial.println("\n========================\n");

	setupClock();
	resyncInternet();

	EEPROM.begin(sizeof(SavedData));
	const SavedData& savedData = *reinterpret_cast<const SavedData*>(EEPROM.getConstDataPtr());

	storeStateMachine.setTimeToOpen(savedData.timeToOpen);
	storeStateMachine.setTimeToClose(savedData.timeToClose);

	WiFi.mode(WIFI_STA);
	WiFi.begin(savedData.ssid, savedData.pass);
	Serial.println("Begin wifi. Waiting for connection...");

	while(WiFi.status() != WL_CONNECTED)
	{
		Serial.print(".");
		delay(1'000);
	}
	Serial.println(" connected!");

	server.on("/", HTTP_GET,
						[]  //
						{
							server.send(HTTP_CODE_OK, "text/html", storeIndex);
						});
	server.on("/all", HTTP_GET,
						[]  //
						{
							auto stateToString = [](StoreStateMachine::State state)  //
							{
								switch (state)  //
								{
									case StoreStateMachine::State::Idle:
										return "Idle";
									case StoreStateMachine::State::Opening:
										return "Opening";
									case StoreStateMachine::State::Closing:
										return "Closing";
									case StoreStateMachine::State::Calibrating:
										return "Calibrating";
									default:
										return "UNKNOWN";
								}
							};

							auto state = storeStateMachine.currentState();
							auto stateStr = String(stateToString(state));
							auto curPercent = storeStateMachine.isCurrentPercentUnknown() ? String("\"?\"") : String(storeStateMachine.currentPercent() * 100.0f);
							auto targetPercent = String(storeStateMachine.targetPercent() * 100.0f);
							auto ttOpen = String(storeStateMachine.timeToOpen());
							auto ttClose = String(storeStateMachine.timeToClose());
							auto windowOpenStr = String(IS_WINDOW_OPEN ? "true" : "false");
							auto hasAPStr = String(hasAP ? "true" : "false");
							auto OSFSetStr = String(!RTC.oscillatorCheck() ? "true" : "false");

							String jsonString = "{"
							                    R"("state":")" + stateStr + R"(",)"
							                    R"("currentPercent":)" + curPercent + ","
							                    R"("targetPercent":)" + targetPercent + ","
							                    R"("timeToOpen":)" + ttOpen + ","
							                    R"("timeToClose":)" + ttClose + ","
							                    R"("isWindowOpen":)" + windowOpenStr + ","
							                    R"("hasAP":)" + hasAPStr + ","
							                    R"("oscillatorStopFlagSet":)" + OSFSetStr + ","
							                    R"("day":)" + String(currentDate.dayOfWeek) + ","
							                    R"("hour":)" + String(currentDate.hour) + ","
							                    R"("minute":)" + String(currentDate.minute) + ","
							                    R"("second":)" + String(currentDate.second) +
							                    "}";

							server.send(HTTP_CODE_OK, "text/json", jsonString);
						});
	server.on("/", HTTP_POST,
						[]  //
						{
							if (server.hasArg("stop"))
								storeStateMachine.stop();
							else if (server.hasArg("force"))  //
							{
								if (server.hasArg("close"))
									storeStateMachine.close();
								else if (server.hasArg("open"))
									storeStateMachine.open();
								else if (server.hasArg("openPercent"))
									storeStateMachine.setPercent(server.arg("openPercent").toFloat() / 100.0f);
							}     //
							else  //
							{
								if (server.hasArg("close") && IS_WINDOW_CLOSED)
									storeStateMachine.close();
								else if (server.hasArg("open") && IS_WINDOW_CLOSED)
									storeStateMachine.open();
								else if (server.hasArg("openPercent") && IS_WINDOW_CLOSED)
									storeStateMachine.setPercent(server.arg("openPercent").toFloat() / 100.0f);
							}

							if (server.hasArg("day"))
								RTC.setDoW(server.arg("day").toInt());
							else if (server.hasArg("hour"))
								RTC.setHour(server.arg("hour").toInt());
							else if (server.hasArg("minute"))
								RTC.setMinute(server.arg("minute").toInt());
							else if (server.hasArg("second"))
								RTC.setSecond(server.arg("second").toInt());

							server.send(HTTP_CODE_OK, "text/html", storeIndex);
						});
	server.on("/open", HTTP_POST,
						[]  //
						{
							if (IS_WINDOW_CLOSED || server.hasArg("force"))  //
							{
								storeStateMachine.open();
								server.send(HTTP_CODE_NO_CONTENT);
							}     //
							else  //
							{
								server.send(HTTP_CODE_INTERNAL_SERVER_ERROR, "text/plain", "Window is open");
							}
						});
	server.on("/openPercent", HTTP_POST,
						[]  //
						{
							if (IS_WINDOW_CLOSED || server.hasArg("force"))  //
							{
								if (server.hasArg("value"))  //
								{
									storeStateMachine.setPercent(server.arg("value").toFloat() / 100.0f);
									server.send(HTTP_CODE_NO_CONTENT);
								}     //
								else  //
								{     //
									server.send(HTTP_CODE_BAD_REQUEST, "text/plain", "Missing `value` argument");
								}
							}     //
							else  //
							{
								server.send(HTTP_CODE_INTERNAL_SERVER_ERROR, "text/plain", "Window is open");
							}
						});
	server.on("/stop", HTTP_POST,
						[]  //
						{
							storeStateMachine.stop();
							server.send(HTTP_CODE_NO_CONTENT);
						});
	server.on("/close", HTTP_POST,
						[]  //
						{
							if (IS_WINDOW_CLOSED || server.hasArg("force"))  //
							{
								storeStateMachine.close();
								server.send(HTTP_CODE_NO_CONTENT);
							}     //
							else  //
							{
								server.send(HTTP_CODE_INTERNAL_SERVER_ERROR, "text/plain", "Window is open");
							}
						});
	server.on("/timeToOpen", HTTP_POST,
						[]  //
						{
							if (server.hasArg("value"))  //
							{
								storeStateMachine.setTimeToOpen(static_cast<unsigned long>(server.arg("value").toInt()));
								writeTimeToOpen(storeStateMachine.timeToOpen());
								server.send(HTTP_CODE_NO_CONTENT);
							}     //
							else  //
							{
								server.send(HTTP_CODE_BAD_REQUEST, "text/plain", "Missing `value` argument");
							}
						});
	server.on("/timeToClose", HTTP_POST,
						[]  //
						{
							if (server.hasArg("value"))  //
							{
								storeStateMachine.setTimeToClose(static_cast<unsigned long>(server.arg("value").toInt()));
								writeTimeToClose(storeStateMachine.timeToClose());
								server.send(HTTP_CODE_NO_CONTENT);
							}     //
							else  //
							{
								server.send(HTTP_CODE_BAD_REQUEST, "text/plain", "Missing `value` argument");
							}
						});
	server.on("/credentials", HTTP_POST,
						[]  //
						{
							if (server.hasArg("ssid") && server.hasArg("pass"))  //
							{
								writeCredentialsToEEPROM(server.arg("ssid"), server.arg("pass"));
								server.send(HTTP_CODE_NO_CONTENT);
							}     //
							else  //
							{
								server.send(HTTP_CODE_BAD_REQUEST, "text/plain", "Missing `ssid` or `pass` argument");
							}
						});
	server.on("/restart", HTTP_POST,
						[]  //
						{
							ESP.restart();
						});

	server.begin();

	Serial.print("Open http://");
	Serial.print(WiFi.localIP());
	Serial.println("/ in your browser to see it working");

	Serial.print("macAddress:  ");
	Serial.println(WiFi.macAddress());

	Serial.print("subnetMask:  ");
	Serial.println(WiFi.subnetMask());

	Serial.print("gateway:     ");
	Serial.println(WiFi.gatewayIP());

	Serial.print("dnsIP:       ");
	Serial.println(WiFi.dnsIP());

	Serial.print("broadcastIP: ");
	Serial.println(WiFi.broadcastIP());

	currentDate = sampleRTC();
}

class DailyRule
{
	RTCSample m_triggerDate;
	std::function<void(void)> m_task;
	uint64_t m_executionCounter = 0;

public:
	DailyRule(RTCSample date, std::function<void(void)> task) : m_triggerDate(date), m_task(std::move(task)) {
		date.dayOfWeek = 0;
	}

	void run(RTCSample currentDate)
	{
		currentDate.dayOfWeek = 0;

		if (currentDate == m_triggerDate)
		{
			if (m_executionCounter == 0)
			{
				Serial.println("execute task:");
				m_task();
				m_executionCounter++;
			}
		}
		else
			m_executionCounter = 0;
	}
};

class DailyWeekRule
{
	RTCSample m_triggerDate;
	std::function<void(void)> m_task;
	uint64_t m_executionCounter = 0;

public:
	DailyWeekRule(RTCSample date, std::function<void(void)> task) : m_triggerDate(date), m_task(std::move(task)) {
		date.dayOfWeek = 0;
	}

	void run(RTCSample currentDate)
	{
		if (currentDate.dayOfWeek >= 6)
			return;

		currentDate.dayOfWeek = 0;

		if (currentDate == m_triggerDate)
		{
			if (m_executionCounter == 0)
			{
				Serial.println("execute task:");
				m_task();
				m_executionCounter++;
			}
		}
		else
			m_executionCounter = 0;
	}
};

class DailyWeekendRule
{
	RTCSample m_triggerDate;
	std::function<void(void)> m_task;
	uint64_t m_executionCounter = 0;

public:
	DailyWeekendRule(RTCSample date, std::function<void(void)> task) : m_triggerDate(date), m_task(std::move(task)) {
		date.dayOfWeek = 0;
	}

	void run(RTCSample currentDate)
	{
		if (currentDate.dayOfWeek < 6)
			return;

		currentDate.dayOfWeek = 0;

		if (currentDate == m_triggerDate)
		{
			if (m_executionCounter == 0)
			{
				Serial.println("execute task:");
				m_task();
				m_executionCounter++;
			}
		}
		else
			m_executionCounter = 0;
	}
};

DailyRule ruleResync0430 = DailyRule(RTCSample{0, 4, 30, 0}, []{resyncInternet();});
DailyWeekRule ruleOpen0905Week = DailyWeekRule(RTCSample{0, 9, 5, 0}, []{storeStateMachine.setPercent(0.08);});
DailyWeekRule ruleOpen0910Week = DailyWeekRule(RTCSample{0, 9, 10, 0}, []{storeStateMachine.setPercent(0.5);});
DailyWeekRule ruleOpen0915Week = DailyWeekRule(RTCSample{0, 9, 15, 0}, []{storeStateMachine.open();});
DailyWeekendRule ruleOpen1000Weekend = DailyWeekendRule(RTCSample{0, 10, 0, 0}, []{storeStateMachine.open();});
DailyRule ruleClose2100 = DailyRule(RTCSample{0, 21, 0, 0}, []{storeStateMachine.close();});

void loop()  //
{
	if (millis() % 250 == 0)
		currentDate = sampleRTC();

	server.handleClient();

	ruleResync0430.run(currentDate);
	ruleOpen0905Week.run(currentDate);
	ruleOpen0910Week.run(currentDate);
	ruleOpen0915Week.run(currentDate);
	ruleOpen1000Weekend.run(currentDate);
	ruleClose2100.run(currentDate);

	storeStateMachine.run();
}

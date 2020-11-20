#include "EspMQTTClient.h"
#include "../../../include/esp32-gogo-config.h"

#ifdef ENABLE_MQTT_LOGGING
#include "../../../include/esp32-uart.h"
extern Esp32UART extSerial;
#endif

// =============== Constructor / destructor ===================

// MQTT only (no wifi connection attempt)
EspMQTTClient::EspMQTTClient(
    const char *mqttServerIp,
    const short mqttServerPort,
    const char *mqttClientName) : EspMQTTClient(NULL, NULL, mqttServerIp, NULL, NULL, mqttClientName, mqttServerPort)
{
}

EspMQTTClient::EspMQTTClient(
    const char *mqttServerIp,
    const short mqttServerPort,
    const char *mqttUsername,
    const char *mqttPassword,
    const char *mqttClientName) : EspMQTTClient(NULL, NULL, mqttServerIp, mqttUsername, mqttPassword, mqttClientName, mqttServerPort)
{
}

// Wifi and MQTT handling
EspMQTTClient::EspMQTTClient(
    const char *wifiSsid,
    const char *wifiPassword,
    const char *mqttServerIp,
    const char *mqttClientName,
    const short mqttServerPort) : EspMQTTClient(wifiSsid, wifiPassword, mqttServerIp, NULL, NULL, mqttClientName, mqttServerPort)
{
}

EspMQTTClient::EspMQTTClient(
    const char *wifiSsid,
    const char *wifiPassword,
    const char *mqttServerIp,
    const char *mqttUsername,
    const char *mqttPassword,
    const char *mqttClientName,
    const short mqttServerPort) : _wifiSsid(wifiSsid),
                                  _wifiPassword(wifiPassword),
                                  _mqttServerIp(mqttServerIp),
                                  _mqttUsername(mqttUsername),
                                  _mqttPassword(mqttPassword),
                                  _mqttClientName(mqttClientName),
                                  _mqttServerPort(mqttServerPort),
                                  _mqttClient(mqttServerIp, mqttServerPort, _wifiClient)
{
    // WiFi connection
    _wifiConnected = false;
    _connectingToWifi = false;
    _nextWifiConnectionAttemptMillis = 500;
    _lastWifiConnectiomAttemptMillis = 0;
    _wifiReconnectionAttemptDelay = 60 * 1000;

    // MQTT client
    _mqttConnected = false;
    _nextMqttConnectionAttemptMillis = 0;
    _mqttReconnectionAttemptDelay = 15 * 1000; // 15 seconds of waiting between each mqtt reconnection attempts by default
    _mqttLastWillTopic = 0;
    _mqttLastWillMessage = 0;
    _mqttLastWillRetain = false;
    _mqttCleanSession = true;
    _mqttClient.setCallback([this](char *topic, byte *payload, unsigned int length) { this->mqttMessageReceivedCallback(topic, payload, length); });
    _failedMQTTConnectionAttemptCount = 0;
    _defaultQoS = 0;

    // Web updater
    _updateServerAddress = NULL;
    _httpServer = NULL;
    _httpUpdater = NULL;

    // other
    _enableSerialLogs = false;
    _drasticResetOnConnectionFailures = false;
    _connectionEstablishedCount = 0;
}

EspMQTTClient::~EspMQTTClient()
{
    if (_httpServer != NULL)
        delete _httpServer;
    if (_httpUpdater != NULL)
        delete _httpUpdater;
}

// =============== Configuration functions, most of them must be called before the first loop() call ==============

void EspMQTTClient::enableDebuggingMessages(const bool enabled)
{
    _enableSerialLogs = enabled;
}

void EspMQTTClient::enableHTTPWebUpdater(const char *username, const char *password, const char *address)
{
    if (_httpServer == NULL)
    {
        _httpServer = new WebServer(80);
        _httpUpdater = new ESPHTTPUpdateServer(_enableSerialLogs);
        _updateServerUsername = (char *)username;
        _updateServerPassword = (char *)password;
        _updateServerAddress = (char *)address;
    }
#ifdef ENABLE_MQTT_LOGGING
    else if (_enableSerialLogs)
        extSerial.print("SYS! You can't call enableHTTPWebUpdater() more than once !\n");
#endif
}

void EspMQTTClient::enableHTTPWebUpdater(const char *address)
{
    if (_mqttUsername == NULL || _mqttPassword == NULL)
        enableHTTPWebUpdater("", "", address);
    else
        enableHTTPWebUpdater(_mqttUsername, _mqttPassword, address);
}

void EspMQTTClient::enableMQTTPersistence()
{
    _mqttCleanSession = false;
}

void EspMQTTClient::enableLastWillMessage(const char *topic, const char *message, const bool retain)
{
    _mqttLastWillTopic = (char *)topic;
    _mqttLastWillMessage = (char *)message;
    _mqttLastWillRetain = retain;
}

// =============== Wifi / connection handling =================

void EspMQTTClient::connectToWifi(const char *ssid, const char *password)
{
    _wifiSsid = ssid;
    _wifiPassword = password;

    WiFi.disconnect(true); //? set state to force reconnecting the network
}

// =============== Main loop / connection state handling =================

void EspMQTTClient::loop()
{
    // WIFI handling
    bool wifiStateChanged = handleWiFi();

    // If there is a change in the wifi connection state, don't handle the mqtt connection state right away.
    // We will wait at least one lopp() call. This prevent the library from doing too much thing in the same loop() call.
    if (wifiStateChanged)
        return;

    // MQTT Handling
    bool mqttStateChanged = handleMQTT();
    if (mqttStateChanged)
        return;

    // Procewss the delayed execution commands
    processDelayedExecutionRequests();
}

bool EspMQTTClient::handleWiFi()
{
    // When it's the first call, reset the wifi radio and schedule the wifi connection
    static bool firstLoopCall = true;
    if (_wifiSsid != NULL && firstLoopCall)
    {
        WiFi.disconnect(true);
        _nextWifiConnectionAttemptMillis = millis() + 500;
        firstLoopCall = false;
        return true;
    }

    // Get the current connextion status
    bool isWifiConnected = (WiFi.status() == WL_CONNECTED);

    // Connection established
    if (isWifiConnected && !_wifiConnected)
    {
        onWiFiConnectionEstablished();
        _connectingToWifi = false;

        // At least 500 miliseconds of waiting before an mqtt connection attempt.
        // Some people have reported instabilities when trying to connect to
        // the mqtt broker right after being connected to wifi.
        // This delay prevent these instabilities.
        _nextMqttConnectionAttemptMillis = millis() + 500;
    }

    // Connection in progress
    else if (_connectingToWifi)
    {
        if (WiFi.status() == WL_CONNECT_FAILED || millis() - _lastWifiConnectiomAttemptMillis >= _wifiReconnectionAttemptDelay)
        {
#ifdef ENABLE_MQTT_LOGGING
            if (_enableSerialLogs)
                extSerial.printf("WiFi! Connection attempt failed, delay expired. (%fs). \n", millis() / 1000.0);
#endif

            WiFi.disconnect(true);
            MDNS.end();

            _nextWifiConnectionAttemptMillis = millis() + 500;
            _connectingToWifi = false;
        }
    }

    // Connection lost
    else if (!isWifiConnected && _wifiConnected)
    {
        onWiFiConnectionLost();

        if (_wifiSsid != NULL)
            _nextWifiConnectionAttemptMillis = millis() + 500;
    }

    // Connected since at least one loop() call
    else if (isWifiConnected && _wifiConnected)
    {
        // Web updater handling
        if (_httpServer != NULL)
        {
            _httpServer->handleClient();
#ifdef ESP8266
            MDNS.update(); // We need to do this only for ESP8266
#endif
        }
    }

    // Disconnected since at least one loop() call
    // Then, if we handle the wifi reconnection process and the waiting delay has expired, we connect to wifi
    else if (_wifiSsid != NULL && _nextWifiConnectionAttemptMillis > 0 && millis() >= _nextWifiConnectionAttemptMillis)
    {
        connectToWifi();
        _nextWifiConnectionAttemptMillis = 0;
        _connectingToWifi = true;
        _lastWifiConnectiomAttemptMillis = millis();
    }

    /**** Detect and return if there was a change in the WiFi state ****/

    if (isWifiConnected != _wifiConnected)
    {
        _wifiConnected = isWifiConnected;
        return true;
    }
    else
        return false;
}

bool EspMQTTClient::handleMQTT()
{
    // PubSubClient main lopp() call
    _mqttClient.loop();

    // Get the current connextion status
    bool isMqttConnected = isWifiConnected() && _mqttClient.connected();

    /***** Detect ans handle the current MQTT handling state *****/

    // Connection established
    if (isMqttConnected && !_mqttConnected)
    {
        _mqttConnected = true;
        if (_failedSubscriptionList.size() > 0)
            subscribeFailedList();

        onMQTTConnectionEstablished();
    }

    // Connection lost
    else if (!isMqttConnected && _mqttConnected)
    {
        onMQTTConnectionLost();
        _nextMqttConnectionAttemptMillis = millis() + _mqttReconnectionAttemptDelay;
    }

    // It's time to  connect to the MQTT broker
    else if (isWifiConnected() && _nextMqttConnectionAttemptMillis > 0 && millis() >= _nextMqttConnectionAttemptMillis)
    {
        // Connect to MQTT broker
        if (connectToMqttBroker())
        {
            _failedMQTTConnectionAttemptCount = 0;
            _nextMqttConnectionAttemptMillis = 0;
        }
        else
        {
            // Connection failed, plan another connection attempt
            _nextMqttConnectionAttemptMillis = millis() + _mqttReconnectionAttemptDelay;
            _mqttClient.disconnect();
            _failedMQTTConnectionAttemptCount++;

#ifdef ENABLE_MQTT_LOGGING
            if (_enableSerialLogs)
                extSerial.printf("MQTT!: Failed MQTT connection count: %i \n", _failedMQTTConnectionAttemptCount);
#endif

            // When there is too many failed attempt, sometimes it help to reset the WiFi connection or to restart the board.
            if (_failedMQTTConnectionAttemptCount == 8)
            {
#ifdef ENABLE_MQTT_LOGGING
                if (_enableSerialLogs)
                    extSerial.println("MQTT!: Can't connect to broker after too many attempt, resetting WiFi ...");
#endif

                WiFi.disconnect(true);
                MDNS.end();
                _nextWifiConnectionAttemptMillis = millis() + 500;

                if (!_drasticResetOnConnectionFailures)
                    _failedMQTTConnectionAttemptCount = 0;
            }
            else if (_drasticResetOnConnectionFailures && _failedMQTTConnectionAttemptCount == 12) // Will reset after 12 failed attempt (3 minutes of retry)
            {
#ifdef ENABLE_MQTT_LOGGING
                if (_enableSerialLogs)
                    extSerial.println("MQTT!: Can't connect to broker after too many attempt, resetting board ...");
#endif

#ifdef ESP8266
                ESP.reset();
#else
                ESP.restart();
#endif
            }
        }
    }

    /**** Detect and return if there was a change in the MQTT state ****/

    if (_mqttConnected != isMqttConnected)
    {
        _mqttConnected = isMqttConnected;
        return true;
    }
    else
        return false;
}

void EspMQTTClient::onWiFiConnectionEstablished()
{
#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
        extSerial.printf("WiFi: Connected (%fs), ip : %s \n", millis() / 1000.0, WiFi.localIP().toString().c_str());
#endif

    // Config of web updater
    if (_httpServer != NULL)
    {
        MDNS.begin(_mqttClientName);
        _httpUpdater->setup(_httpServer, _updateServerAddress, _updateServerUsername, _updateServerPassword);
        _httpServer->begin();
        MDNS.addService("http", "tcp", 80);

#ifdef ENABLE_MQTT_LOGGING
        if (_enableSerialLogs)
            extSerial.printf("WEB: Updater ready, open http://%s.local in your browser and login with username '%s' and password '%s'.\n", _mqttClientName, _updateServerUsername, _updateServerPassword);
#endif
    }
}

void EspMQTTClient::onWiFiConnectionLost()
{
#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
        extSerial.printf("WiFi! Lost connection (%fs). \n", millis() / 1000.0);
#endif

    // If we handle wifi, we force disconnection to clear the last connection
    if (_wifiSsid != NULL)
    {
        WiFi.disconnect(true);
        MDNS.end();
    }
}

void EspMQTTClient::onMQTTConnectionEstablished()
{
    _connectionEstablishedCount++;
    _connectionEstablishedCallback();
}

void EspMQTTClient::onMQTTConnectionLost()
{
#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
    {
        extSerial.printf("MQTT! Lost connection (%fs). \n", millis() / 1000.0);
        extSerial.printf("MQTT: Retrying to connect in %i seconds. \n", _mqttReconnectionAttemptDelay / 1000);
    }
#endif

    //? backup latest subscribe list
    _failedSubscriptionList = _topicSubscriptionList;
    _topicSubscriptionList.clear();
}

// =============== Public functions for interaction with thus lib =================

bool EspMQTTClient::setMaxPacketSize(const uint16_t size)
{

    bool success = _mqttClient.setBufferSize(size);

#ifdef ENABLE_MQTT_LOGGING
    if (!success && _enableSerialLogs)
        extSerial.println("MQTT! failed to set the max packet size.");
#endif

    return success;
}

bool EspMQTTClient::publish(const String &topic, const String &payload, bool retain)
{
    // Do not try to publish if MQTT is not connected.
    if (!isConnected())
    {
#ifdef ENABLE_MQTT_LOGGING
        if (_enableSerialLogs)
            extSerial.println("MQTT! Trying to publish when disconnected, skipping.");
#endif

        return false;
    }

    bool success = _mqttClient.publish(topic.c_str(), payload.c_str(), retain);

#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
    {
        if (success)
            extSerial.printf("MQTT << [%s] %s\n", topic.c_str(), payload.c_str());
        else
            extSerial.println("MQTT! publish failed, is the message too long ? (see setMaxPacketSize())"); // This can occurs if the message is too long according to the maximum defined in PubsubClient.h
    }
#endif

    return success;
}

bool EspMQTTClient::subscribe(uint8_t processID, const String &topic, MessageReceivedCallback messageReceivedCallback, uint8_t qos)
{
    // Check the possibility to add a new topic
    if (_topicSubscriptionList.size() >= MAX_TOPIC_SUBSCRIPTION_LIST_SIZE)
    {
#ifdef ENABLE_MQTT_LOGGING
        if (_enableSerialLogs)
            extSerial.println("MQTT! Subscription list is full, ignored.");
#endif
        return false;
    }

    // Do not try to subscribe if MQTT is not connected.
    if (!isConnected())
    {
#ifdef ENABLE_MQTT_LOGGING
        if (_enableSerialLogs)
            extSerial.println("MQTT! Trying to subscribe when disconnected, skipping.");
#endif
        _failedSubscriptionList.push_back({topic, messageReceivedCallback, NULL, processID});

        return false;
    }

    bool success = _mqttClient.subscribe(topic.c_str(), qos);

    if (success)
    {
        // Add the record to the subscription list only if it does not exists.
        bool found = false;
        for (byte i = 0; i < _topicSubscriptionList.size() && !found; i++)
            found = _topicSubscriptionList[i].topic.equals(topic);

        if (!found)
            _topicSubscriptionList.push_back({topic, messageReceivedCallback, NULL, processID});
    }

#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
    {
        if (success)
            extSerial.printf("MQTT: Subscribed to [%s]\n", topic.c_str());
        else
            extSerial.println("MQTT! subscribe failed");
    }
#endif

    return success;
}

bool EspMQTTClient::subscribe(uint8_t processID, const String &topic, MessageReceivedCallbackWithTopic messageReceivedCallback, uint8_t qos)
{
    if (subscribe(processID, topic, (MessageReceivedCallback)NULL, qos))
    {
        _topicSubscriptionList[_topicSubscriptionList.size() - 1].callbackWithTopic = messageReceivedCallback;
        return true;
    }
    return false;
}

void EspMQTTClient::subscribeFailedList(void)
{
    for (int i = 0; i < _failedSubscriptionList.size(); i++)
    {
        subscribe(_failedSubscriptionList[i].processID, _failedSubscriptionList[i].topic, _failedSubscriptionList[i].callback, _defaultQoS);
    }
    _failedSubscriptionList.clear();
}

bool EspMQTTClient::unsubscribe(const String &topic)
{
    // Do not try to unsubscribe if MQTT is not connected.
    if (!isConnected())
    {
#ifdef ENABLE_MQTT_LOGGING
        if (_enableSerialLogs)
            extSerial.println("MQTT! Trying to unsubscribe when disconnected, skipping.");
#endif

        return false;
    }

    for (int i = 0; i < _topicSubscriptionList.size(); i++)
    {
#ifdef ENABLE_MQTT_LOGGING
        extSerial.printf("unsub count: %d\n", i);
#endif
        if (_topicSubscriptionList[i].topic.equals(topic))
        {
            if (_mqttClient.unsubscribe(topic.c_str()))
            {
#ifdef ENABLE_MQTT_LOGGING
                extSerial.printf("unsub topic: %s", _topicSubscriptionList[i].topic.c_str());
#endif

                _topicSubscriptionList.erase(_topicSubscriptionList.begin() + i);
                i--;

#ifdef ENABLE_MQTT_LOGGING
                if (_enableSerialLogs)
                    extSerial.printf("MQTT: Unsubscribed from %s\n", topic.c_str());
#endif
            }
            else
            {
#ifdef ENABLE_MQTT_LOGGING
                if (_enableSerialLogs)
                    extSerial.println("MQTT! unsubscribe failed");
#endif

                return false;
            }
        }
    }

    return true;
}

bool EspMQTTClient::unsubscribeAll(void)
{
    for (int i = 0; i < _topicSubscriptionList.size(); i++)
    {
        if (_mqttClient.unsubscribe(_topicSubscriptionList[i].topic.c_str()))
        {
#ifdef ENABLE_MQTT_LOGGING
            if (_enableSerialLogs)
                extSerial.printf("MQTT: Unsubscribed from %s\n", _topicSubscriptionList[i].topic.c_str());
#endif
        }
        else
        {
#ifdef ENABLE_MQTT_LOGGING
            if (_enableSerialLogs)
                extSerial.println("MQTT! unsubscribe failed");
#endif
            return false;
        }
    }
    _topicSubscriptionList.clear();

    return true;
}

void EspMQTTClient::setKeepAlive(uint16_t keepAliveSeconds)
{
    _mqttClient.setKeepAlive(keepAliveSeconds);
}

void EspMQTTClient::executeDelayed(const unsigned long delay, DelayedExecutionCallback callback)
{
    DelayedExecutionRecord delayedExecutionRecord;
    delayedExecutionRecord.targetMillis = millis() + delay;
    delayedExecutionRecord.callback = callback;

    _delayedExecutionList.push_back(delayedExecutionRecord);
}

// ================== Private functions ====================-

// Initiate a Wifi connection (non-blocking)
void EspMQTTClient::connectToWifi()
{
    WiFi.mode(WIFI_STA);
#ifdef ESP32
    WiFi.setHostname(_mqttClientName);
#else
    WiFi.hostname(_mqttClientName);
#endif
    WiFi.begin(_wifiSsid, _wifiPassword);

#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
        extSerial.printf("\nWiFi: Connecting to %s ... (%fs) \n", _wifiSsid, millis() / 1000.0);
#endif
}

// Try to connect to the MQTT broker and return True if the connection is successfull (blocking)
bool EspMQTTClient::connectToMqttBroker()
{
#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
        extSerial.printf("MQTT: Connecting to broker \"%s\" with client name \"%s\" ... (%fs)", _mqttServerIp, _mqttClientName, millis() / 1000.0);
#endif

    bool success = _mqttClient.connect(_mqttClientName, _mqttUsername, _mqttPassword, _mqttLastWillTopic, 0, _mqttLastWillRetain, _mqttLastWillMessage, _mqttCleanSession);

#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
    {
        if (success)
            extSerial.printf(" - ok. (%fs) \n", millis() / 1000.0);
        else
        {
            extSerial.printf("unable to connect (%fs), reason: ", millis() / 1000.0);

            switch (_mqttClient.state())
            {
            case -4:
                extSerial.println("MQTT_CONNECTION_TIMEOUT");
                break;
            case -3:
                extSerial.println("MQTT_CONNECTION_LOST");
                break;
            case -2:
                extSerial.println("MQTT_CONNECT_FAILED");
                break;
            case -1:
                extSerial.println("MQTT_DISCONNECTED");
                break;
            case 1:
                extSerial.println("MQTT_CONNECT_BAD_PROTOCOL");
                break;
            case 2:
                extSerial.println("MQTT_CONNECT_BAD_CLIENT_ID");
                break;
            case 3:
                extSerial.println("MQTT_CONNECT_UNAVAILABLE");
                break;
            case 4:
                extSerial.println("MQTT_CONNECT_BAD_CREDENTIALS");
                break;
            case 5:
                extSerial.println("MQTT_CONNECT_UNAUTHORIZED");
                break;
            }

            extSerial.printf("MQTT: Retrying to connect in %i seconds.\n", _mqttReconnectionAttemptDelay / 1000);
        }
    }
#endif

    return success;
}

// Delayed execution handling.
// Check if there is delayed execution requests to process and execute them if needed.
void EspMQTTClient::processDelayedExecutionRequests()
{
    if (_delayedExecutionList.size() > 0)
    {
        unsigned long currentMillis = millis();

        for (int i = 0; i < _delayedExecutionList.size(); i++)
        {
            if (_delayedExecutionList[i].targetMillis <= currentMillis)
            {
                _delayedExecutionList[i].callback();
                _delayedExecutionList.erase(_delayedExecutionList.begin() + i);
                i--;
            }
        }
    }
}

/**
 * Matching MQTT topics, handling the eventual presence of a single wildcard character
 *
 * @param topic1 is the topic may contain a wildcard
 * @param topic2 must not contain wildcards
 * @return true on MQTT topic match, false otherwise
 */
bool EspMQTTClient::mqttTopicMatch(const String &topic1, const String &topic2)
{
    int i = 0;

    if ((i = topic1.indexOf('#')) >= 0)
    {
        String t1a = topic1.substring(0, i);
        String t1b = topic1.substring(i + 1);
        if ((t1a.length() == 0 || topic2.startsWith(t1a)) &&
            (t1b.length() == 0 || topic2.endsWith(t1b)))
            return true;
    }
    else if ((i = topic1.indexOf('+')) >= 0)
    {
        String t1a = topic1.substring(0, i);
        String t1b = topic1.substring(i + 1);

        if ((t1a.length() == 0 || topic2.startsWith(t1a)) &&
            (t1b.length() == 0 || topic2.endsWith(t1b)))
        {
            if (topic2.substring(t1a.length(), topic2.length() - t1b.length()).indexOf('/') == -1)
                return true;
        }
    }
    else
    {
        return topic1.equals(topic2);
    }

    return false;
}

void EspMQTTClient::mqttMessageReceivedCallback(char *topic, byte *payload, unsigned int length)
{
    // Convert the payload into a String
    // First, We ensure that we dont bypass the maximum size of the PubSubClient library buffer that originated the payload
    // This buffer has a maximum length of _mqttClient.getBufferSize() and the payload begin at "headerSize + topicLength + 1"
    unsigned int strTerminationPos;
    if (strlen(topic) + length + 9 >= _mqttClient.getBufferSize())
    {
        strTerminationPos = length - 1;

#ifdef ENABLE_MQTT_LOGGING
        if (_enableSerialLogs)
            extSerial.print("MQTT! Your message may be truncated, please change MQTT_MAX_PACKET_SIZE of PubSubClient.h to a higher value.\n");
#endif
    }
    else
        strTerminationPos = length;

    // Second, we add the string termination code at the end of the payload and we convert it to a String object
    payload[strTerminationPos] = '\0';
    String payloadStr((char *)payload);
    String topicStr(topic);

    // Logging
#ifdef ENABLE_MQTT_LOGGING
    if (_enableSerialLogs)
        extSerial.printf("MQTT >> [%s] %s\n", topic, payloadStr.c_str());
#endif

    // Send the message to subscribers
    for (byte i = 0; i < _topicSubscriptionList.size(); i++)
    {
        if (mqttTopicMatch(_topicSubscriptionList[i].topic, String(topic)))
        {
            // if (_topicSubscriptionList[i].callback != NULL)
            //     _topicSubscriptionList[i].callback(payloadStr); // Call the callback
            // if (_topicSubscriptionList[i].callbackWithTopic != NULL)
            //     _topicSubscriptionList[i].callbackWithTopic(topicStr, payloadStr); // Call the callback

            //? checking broadcast topic to trigered logoVM instance
            if (topicStr.startsWith(_broadcastTopic))
            {
                //? checking payload matches with broadcast password
                if (payloadStr.equals(_broadcastPayload))
                    isNewPayloadMQTT = _topicSubscriptionList[i].processID;
            }
            else
            {
                payload_len = payloadStr.length();
                payloadStr.toCharArray(payload_buf, payload_len + 1);

                isNewPayloadMQTT = _topicSubscriptionList[i].processID;
            }

#ifdef ENABLE_MQTT_LOGGING
            if (_enableSerialLogs)
                extSerial.printf("pid: %d\n", isNewPayloadMQTT);
#endif
        }
    }
}

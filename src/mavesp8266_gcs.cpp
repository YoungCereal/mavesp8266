/****************************************************************************
 *
 * Copyright (c) 2015, 2016 Gus Grubba. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mavesp8266_gcs.cpp
 * ESP8266 Wifi AP, MavLink UART/UDP Bridge
 *
 * @author Gus Grubba <mavlink@grubba.com>
 */

#include "mavesp8266.h"
#include "mavesp8266_gcs.h"
#include "mavesp8266_parameters.h"
#include "mavesp8266_component.h"

//---------------------------------------------------------------------------------
MavESP8266GCS::MavESP8266GCS()
    : _udp_port(DEFAULT_UDP_HPORT)
{
    memset(&_message, 0, sizeof(_message));
}

//---------------------------------------------------------------------------------
//-- Initialize
void
MavESP8266GCS::begin(MavESP8266Bridge* forwardTo, IPAddress gcsIP)
{
    MavESP8266Bridge::begin(forwardTo);
    _ip = gcsIP;
    //-- Init variables that shouldn't change unless we reboot
    _udp_port = getWorld()->getParameters()->getWifiUdpHport();
    //-- Start UDP
    _udp.begin(getWorld()->getParameters()->getWifiUdpCport());
}

//---------------------------------------------------------------------------------
//-- Read MavLink message from GCS
void
MavESP8266GCS::readMessage()
{
    //-- Read UDP
    if(_readMessage()) {
        //-- If we have a message, forward it
        _forwardTo->sendMessage(&_message);
        memset(&_message, 0, sizeof(_message));
    }
    //-- Update radio status (1Hz)
    if(_heard_from && (millis() - _last_status_time > 1000)) {
        delay(0);
        _sendRadioStatus();
        _last_status_time = millis();
    }
}

//---------------------------------------------------------------------------------
//-- Read MavLink message from GCS
bool
MavESP8266GCS::_readMessage()
{
    bool msgReceived = false;
    int udp_count = _udp.parsePacket();
    if(udp_count > 0)
    {
        mavlink_status_t gcs_status;
        while(udp_count--)
        {
            int result = _udp.read();
            if (result >= 0)
            {
                // Parsing
                msgReceived = mavlink_parse_char(MAVLINK_COMM_2, result, &_message, &gcs_status);
                if(msgReceived) {
                    //-- We no longer need to broadcast
                    _status.packets_received++;
                    if(_ip[3] == 255) {
                        _ip = _udp.remoteIP();
                        getWorld()->getLogger()->log("Response from GCS. Setting GCS IP to: %s\n", _ip.toString().c_str());
                    }
                    //-- First packets
                    if(!_heard_from) {
                        if(_message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
                            //-- We no longer need DHCP
                            if(getWorld()->getParameters()->getWifiMode() == WIFI_MODE_AP) {
                                wifi_softap_dhcps_stop();
                            }
                            _heard_from      = true;
                            _system_id       = _message.sysid;
                            _component_id    = _message.compid;
                            _seq_expected    = _message.seq + 1;
                            _last_heartbeat  = millis();
                        }
                    } else {
                        if(_message.msgid == MAVLINK_MSG_ID_HEARTBEAT)
                            _last_heartbeat = millis();
                        _checkLinkErrors(&_message);
                    }



                    //-- Check for message we might be interested
                    if(getWorld()->getComponent()->handleMessage(this, &_message)){
                        //-- Eat message (don't send it to FC)
                        memset(&_message, 0, sizeof(_message));
                        msgReceived = false;
                        continue;
                    }


                    //-- Got message, leave
                    break;
                }
            }
        }
    }
    if(!msgReceived) {
        if(_heard_from && (millis() - _last_heartbeat) > HEARTBEAT_TIMEOUT) {
            //-- Restart DHCP and start broadcasting again
            if(getWorld()->getParameters()->getWifiMode() == WIFI_MODE_AP) {
                wifi_softap_dhcps_start();
            }
            _heard_from = false;
            _ip[3] = 255;
            getWorld()->getLogger()->log("Heartbeat timeout from GCS\n");
        }
    }
    return msgReceived;
}

//---------------------------------------------------------------------------------
//-- Forward message(s) to the GCS
int
MavESP8266GCS::sendMessage(mavlink_message_t* message, int count) {
    int sentCount = 0;
    _udp.beginPacket(_ip, _udp_port);
    for(int i = 0; i < count; i++) {
        // Translate message to buffer
        char buf[300];
        unsigned len = mavlink_msg_to_send_buffer((uint8_t*)buf, &message[i]);
        // Send it
        _status.packets_sent++;
        size_t sent = _udp.write((uint8_t*)(void*)buf, len);
        if(sent != len) {
            break;
            //-- Fibble attempt at not losing data until we get access to the socket TX buffer
            //   status before we try to send.
            _udp.endPacket();
            delay(2);
            _udp.beginPacket(_ip, _udp_port);
            _udp.write((uint8_t*)(void*)&buf[sent], len - sent);
            _udp.endPacket();
            return sentCount;
        }
        sentCount++;
    }
    _udp.endPacket();
    return sentCount;
}

//---------------------------------------------------------------------------------
//-- Forward message to the GCS
int
MavESP8266GCS::sendMessage(mavlink_message_t* message) {
    _sendSingleUdpMessage(message);
    return 1;
}

//---------------------------------------------------------------------------------
//-- Send Radio Status
void
MavESP8266GCS::_sendRadioStatus()
{
    linkStatus* st = _forwardTo->getStatus();
    uint8_t rssi = 0;
    if(wifi_get_opmode() == STATION_MODE) {
        rssi = (uint8_t)wifi_station_get_rssi();
    }
    //-- Build message
    mavlink_message_t msg;
    mavlink_msg_radio_status_pack(
        _forwardTo->systemID(),
        MAV_COMP_ID_UDP_BRIDGE,
        &msg,
        rssi,                   // RSSI Only valid in STA mode
        0,                      // We don't have access to Remote RSSI
        st->queue_status,       // UDP queue status
        0,                      // We don't have access to noise data
        (uint16_t)((st->packets_lost * 100) / st->packets_received),                // Percent of lost messages from Vehicle (UART)
        (uint16_t)((_status.packets_lost * 100) / _status.packets_received),        // Percent of lost messages from GCS (UDP)
        0                       // We don't fix anything
    );
    _sendSingleUdpMessage(&msg);
    _status.radio_status_sent++;
}

//---------------------------------------------------------------------------------
//-- Send UDP Single Message
void
MavESP8266GCS::_sendSingleUdpMessage(mavlink_message_t* msg)
{
    // Translate message to buffer
    char buf[300];
    unsigned len = mavlink_msg_to_send_buffer((uint8_t*)buf, msg);
    // Send it
    _udp.beginPacket(_ip, _udp_port);
    size_t sent = _udp.write((uint8_t*)(void*)buf, len);
    _udp.endPacket();
    //-- Fibble attempt at not losing data until we get access to the socket TX buffer
    //   status before we try to send.
    if(sent != len) {
        delay(1);
        _udp.beginPacket(_ip, _udp_port);
        _udp.write((uint8_t*)(void*)&buf[sent], len - sent);
        _udp.endPacket();
    }
    _status.packets_sent++;
}
